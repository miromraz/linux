// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC fingerprint sensor behind a QSEE trustlet.
 *
 * Some Qualcomm phones wire an FPC1020-family match-on-chip sensor to an SPI
 * bus that TrustZone owns: a vendor trustlet drives the part, and enrolment
 * and matching happen inside it. What Linux owns is the sensor's IO supply,
 * its reset and interrupt lines, and the one path into the trustlet, a
 * kernel-only SCM call. This driver provides those, and /dev/fpc_tee to reach
 * them from userspace (see include/uapi/misc/fpc1020.h):
 *
 *  - FPC_TEE_IOC_XFER carries one command buffer to the trustlet and back.
 *    The command protocol belongs to userspace, as it does in the vendor HAL.
 *    The first one loads the trustlet and registers the listener services it
 *    stores its data through. QSEE refuses to start an app that is already
 *    running, so this happens once per boot.
 *  - poll() reports EPOLLIN while the interrupt line is high. The trustlet's
 *    capture sequence waits for that level, and its own commands pulse the
 *    line, so an edge does not mean a finger.
 *  - The trustlet keeps its template database in the normal world's
 *    filesystem, through QSEE's file service (0xa) and GP file service
 *    (0x7000). QSEE answers a command that needs one with a request in the
 *    listener's shared buffer, and the command cannot complete until the
 *    request is served. The request goes to whoever waits in
 *    FPC_TEE_IOC_LISTENER_RECV, and the answer comes back through
 *    FPC_TEE_IOC_LISTENER_SEND while the command blocks: which files to
 *    allow is userspace policy.
 *
 * Copyright (C) 2026 Giuseppe Maggio <jertlok@proton.me>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/pm_wakeup.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>

#include <uapi/misc/fpc1020.h>

/*
 * The trustlet's request is an envelope holding the command buffer's length
 * and physical address, sent as both request and response. The trustlet
 * reads that length as the buffer's size: a short one is accepted and does
 * nothing.
 */
#define FPC1020_ENV_SIZE		64
#define FPC1020_CMDBUF_SIZE		PAGE_SIZE

#define FPC1020_LISTENER_TIMEOUT_MS	30000

/* The listener services, with the buffer sizes qseecomd registers them with. */
static const struct {
	u32 id;
	size_t size;
} fpc1020_listeners[] = {
	{ .id = 0x000a, .size = 0x5000 },	/* file service */
	{ .id = 0x7000, .size = 0x7e000 },	/* GP file service */
};

#define FPC1020_NUM_LISTENERS		ARRAY_SIZE(fpc1020_listeners)

enum fpc1020_listener_state {
	FPC1020_LISTENER_IDLE,
	FPC1020_LISTENER_PENDING,	/* request in the buffer, not taken */
	FPC1020_LISTENER_SERVING,	/* taken, waiting for the answer */
	FPC1020_LISTENER_DONE,		/* answer in the buffer */
};

struct fpc1020 {
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	const char *firmware;
	wait_queue_head_t irq_wq;
	struct miscdevice miscdev;

	/* Serializes loading and every command to the trustlet. */
	struct mutex lock;
	struct qcom_tzmem_pool *tzpool;
	bool ready;
	u32 app_id;

	struct {
		void *buf;
		dma_addr_t phys;
		bool registered;
	} listener[FPC1020_NUM_LISTENERS];

	/* Guards the listener_* fields below, shared with the ioctl paths. */
	spinlock_t listener_lock;
	enum fpc1020_listener_state listener_state;
	unsigned int listener_index;
	int listener_status;
	wait_queue_head_t listener_req_wq;
	wait_queue_head_t listener_rsp_wq;
};

static irqreturn_t fpc1020_irq_handler(int irq, void *data)
{
	struct fpc1020 *fpc1020 = data;

	pm_wakeup_event(fpc1020->dev, 1000);
	wake_up_interruptible(&fpc1020->irq_wq);

	return IRQ_HANDLED;
}

/* The vendor driver's reset timings. */
static void fpc1020_hw_reset(struct fpc1020 *fpc1020)
{
	gpiod_set_value_cansleep(fpc1020->reset_gpio, 0);
	usleep_range(100, 200);
	gpiod_set_value_cansleep(fpc1020->reset_gpio, 1);
	usleep_range(5000, 5100);
	gpiod_set_value_cansleep(fpc1020->reset_gpio, 0);
	usleep_range(5000, 5100);
}

/*
 * Called by qcom_scm_qseecom_app_send_listener(), with the QSEECOM call lock
 * and fpc1020->lock held, when the trustlet asks for @listener_id: hand the
 * request to userspace and wait for its answer.
 */
static int fpc1020_listener_handle(void *priv, u32 listener_id)
{
	struct fpc1020 *fpc1020 = priv;
	unsigned int i;
	long left;
	int ret;

	for (i = 0; i < FPC1020_NUM_LISTENERS; i++)
		if (fpc1020_listeners[i].id == listener_id &&
		    fpc1020->listener[i].registered)
			break;

	if (i == FPC1020_NUM_LISTENERS) {
		dev_warn_ratelimited(fpc1020->dev,
				     "request for unknown listener 0x%x\n",
				     listener_id);
		return -ENOENT;
	}

	spin_lock(&fpc1020->listener_lock);
	fpc1020->listener_index = i;
	fpc1020->listener_state = FPC1020_LISTENER_PENDING;
	spin_unlock(&fpc1020->listener_lock);
	wake_up_interruptible(&fpc1020->listener_req_wq);

	left = wait_event_killable_timeout(fpc1020->listener_rsp_wq,
					   fpc1020->listener_state == FPC1020_LISTENER_DONE,
					   msecs_to_jiffies(FPC1020_LISTENER_TIMEOUT_MS));

	spin_lock(&fpc1020->listener_lock);
	if (fpc1020->listener_state == FPC1020_LISTENER_DONE)
		ret = fpc1020->listener_status;
	else
		ret = left < 0 ? left : -ETIMEDOUT;
	fpc1020->listener_state = FPC1020_LISTENER_IDLE;
	spin_unlock(&fpc1020->listener_lock);

	if (ret)
		dev_warn_ratelimited(fpc1020->dev,
				     "listener 0x%x request not served: %d\n",
				     listener_id, ret);
	return ret;
}

static int fpc1020_listener_recv(struct fpc1020 *fpc1020, unsigned long arg)
{
	struct fpc_tee_listener l;
	unsigned int i;
	long left;

	if (copy_from_user(&l, (void __user *)arg, sizeof(l)))
		return -EFAULT;

	left = wait_event_interruptible_timeout(fpc1020->listener_req_wq,
						fpc1020->listener_state == FPC1020_LISTENER_PENDING,
						msecs_to_jiffies(l.timeout_ms));
	if (left < 0)
		return left;
	if (left == 0)
		return -ETIMEDOUT;

	spin_lock(&fpc1020->listener_lock);
	if (fpc1020->listener_state != FPC1020_LISTENER_PENDING) {
		spin_unlock(&fpc1020->listener_lock);
		return -EAGAIN;
	}
	fpc1020->listener_state = FPC1020_LISTENER_SERVING;
	i = fpc1020->listener_index;
	spin_unlock(&fpc1020->listener_lock);

	l.id = fpc1020_listeners[i].id;
	l.len = min_t(size_t, l.len, fpc1020_listeners[i].size);

	if (copy_to_user(u64_to_user_ptr(l.buf), fpc1020->listener[i].buf, l.len) ||
	    copy_to_user((void __user *)arg, &l, sizeof(l))) {
		spin_lock(&fpc1020->listener_lock);
		fpc1020->listener_status = -EFAULT;
		fpc1020->listener_state = FPC1020_LISTENER_DONE;
		spin_unlock(&fpc1020->listener_lock);
		wake_up(&fpc1020->listener_rsp_wq);
		return -EFAULT;
	}

	return 0;
}

static int fpc1020_listener_send(struct fpc1020 *fpc1020, unsigned long arg)
{
	struct fpc_tee_listener l;
	unsigned int i;
	int status;

	if (copy_from_user(&l, (void __user *)arg, sizeof(l)))
		return -EFAULT;

	spin_lock(&fpc1020->listener_lock);
	i = fpc1020->listener_index;
	if (fpc1020->listener_state != FPC1020_LISTENER_SERVING ||
	    l.id != fpc1020_listeners[i].id ||
	    l.len > fpc1020_listeners[i].size) {
		spin_unlock(&fpc1020->listener_lock);
		return -EINVAL;
	}
	spin_unlock(&fpc1020->listener_lock);

	/* Only the task serving the request writes the buffer. */
	status = l.status ? -EIO : 0;
	if (copy_from_user(fpc1020->listener[i].buf, u64_to_user_ptr(l.buf), l.len))
		status = -EFAULT;

	spin_lock(&fpc1020->listener_lock);
	if (fpc1020->listener_state == FPC1020_LISTENER_SERVING) {
		fpc1020->listener_status = status;
		fpc1020->listener_state = FPC1020_LISTENER_DONE;
	}
	spin_unlock(&fpc1020->listener_lock);
	wake_up(&fpc1020->listener_rsp_wq);

	return status == -EFAULT ? -EFAULT : 0;
}

static int fpc1020_send_locked(struct fpc1020 *fpc1020, void *cmdbuf, size_t len)
{
	phys_addr_t cmdbuf_phys = qcom_tzmem_to_phys(cmdbuf);
	void *env;
	int ret;

	lockdep_assert_held(&fpc1020->lock);

	if (cmdbuf_phys + len > SZ_4G)
		return -EFBIG;

	env = qcom_tzmem_alloc(fpc1020->tzpool, FPC1020_ENV_SIZE,
			       GFP_KERNEL | GFP_DMA32);
	if (!env)
		return -ENOMEM;

	memset(env, 0, FPC1020_ENV_SIZE);
	put_unaligned_le32(len, env);
	put_unaligned_le32(cmdbuf_phys, env + 4);

	ret = qcom_scm_qseecom_app_send_listener(fpc1020->app_id,
						 env, FPC1020_ENV_SIZE,
						 env, FPC1020_ENV_SIZE,
						 fpc1020_listener_handle, fpc1020);

	qcom_tzmem_free(env);
	return ret;
}

/*
 * The trustlet ships split: an .mdt with its headers and a .bNN file per
 * program header. QSEE takes them concatenated, the .mdt first.
 */
static int fpc1020_load_trustlet_locked(struct fpc1020 *fpc1020)
{
	const struct firmware *mdt, **segs;
	struct device *dev = fpc1020->dev;
	unsigned int phnum, i, loaded = 0;
	int stem = strlen(fpc1020->firmware) - 4;
	const struct elf32_hdr *ehdr;
	size_t total, off;
	dma_addr_t phys;
	void *img;
	int ret;

	if (stem <= 0 || strcmp(fpc1020->firmware + stem, ".mdt"))
		return dev_err_probe(dev, -EINVAL, "firmware-name is not an .mdt\n");

	ret = request_firmware(&mdt, fpc1020->firmware, dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot load %s\n", fpc1020->firmware);

	/* The image's class is the trustlet's, not QSEE's: both run. */
	ehdr = (const struct elf32_hdr *)mdt->data;
	if (mdt->size >= sizeof(struct elf64_hdr) &&
	    !memcmp(ehdr->e_ident, ELFMAG, SELFMAG) &&
	    ehdr->e_ident[EI_CLASS] == ELFCLASS64) {
		phnum = ((const struct elf64_hdr *)mdt->data)->e_phnum;
	} else if (mdt->size >= sizeof(*ehdr) &&
		   !memcmp(ehdr->e_ident, ELFMAG, SELFMAG) &&
		   ehdr->e_ident[EI_CLASS] == ELFCLASS32) {
		phnum = ehdr->e_phnum;
	} else {
		ret = dev_err_probe(dev, -EINVAL, "%s is not an ELF\n",
				    fpc1020->firmware);
		goto release_mdt;
	}

	segs = kcalloc(phnum, sizeof(*segs), GFP_KERNEL);
	if (!segs) {
		ret = -ENOMEM;
		goto release_mdt;
	}

	total = mdt->size;
	for (; loaded < phnum; loaded++) {
		char *name = kasprintf(GFP_KERNEL, "%.*s.b%02u", stem,
				       fpc1020->firmware, loaded);

		if (!name) {
			ret = -ENOMEM;
			goto release_segs;
		}
		ret = request_firmware(&segs[loaded], name, dev);
		if (ret)
			dev_err_probe(dev, ret, "cannot load %s\n", name);
		kfree(name);
		if (ret)
			goto release_segs;
		total += segs[loaded]->size;
	}

	img = dma_alloc_coherent(dev, total, &phys, GFP_KERNEL);
	if (!img) {
		ret = -ENOMEM;
		goto release_segs;
	}

	memcpy(img, mdt->data, mdt->size);
	for (off = mdt->size, i = 0; i < phnum; off += segs[i]->size, i++)
		memcpy(img + off, segs[i]->data, segs[i]->size);

	ret = qcom_scm_qseecom_app_load(phys, mdt->size, total, &fpc1020->app_id);
	dma_free_coherent(dev, total, img, phys);
	if (ret)
		dev_err(dev, "cannot start the trustlet, already running?: %d\n", ret);

release_segs:
	for (i = 0; i < loaded; i++)
		release_firmware(segs[i]);
	kfree(segs);
release_mdt:
	release_firmware(mdt);
	return ret;
}

static int fpc1020_bring_up_locked(struct fpc1020 *fpc1020)
{
	unsigned int i;
	int ret;

	lockdep_assert_held(&fpc1020->lock);

	ret = fpc1020_load_trustlet_locked(fpc1020);
	if (ret)
		return ret;

	/* Without a listener only the template database commands fail. */
	for (i = 0; i < FPC1020_NUM_LISTENERS; i++) {
		ret = qcom_scm_qseecom_listener_register(fpc1020_listeners[i].id,
							 fpc1020->listener[i].phys,
							 fpc1020_listeners[i].size);
		if (ret)
			dev_err(fpc1020->dev, "cannot register listener 0x%x: %d\n",
				fpc1020_listeners[i].id, ret);
		else
			fpc1020->listener[i].registered = true;
	}

	fpc1020->ready = true;
	return 0;
}

static int fpc1020_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(file->private_data, struct fpc1020,
					  miscdev);
	return 0;
}

static int fpc1020_xfer(struct fpc1020 *fpc1020, unsigned long arg)
{
	struct fpc_tee_xfer x;
	void *cmdbuf;
	int ret;

	if (copy_from_user(&x, (void __user *)arg, sizeof(x)))
		return -EFAULT;
	if (x.pad)
		return -EINVAL;
	if (!x.len || x.len > FPC1020_CMDBUF_SIZE)
		return -EMSGSIZE;

	ret = mutex_lock_interruptible(&fpc1020->lock);
	if (ret)
		return ret;

	/*
	 * Not at open(): ->open() runs inside the path walk that opened the
	 * node, and the firmware loader's own walks count their symlinks
	 * against that walk's MAXSYMLINKS. Nine files looked up through a
	 * symlinked /lib exhaust it.
	 */
	if (!fpc1020->ready) {
		ret = fpc1020_bring_up_locked(fpc1020);
		if (ret)
			goto unlock;
	}

	cmdbuf = qcom_tzmem_alloc(fpc1020->tzpool, FPC1020_CMDBUF_SIZE,
				  GFP_KERNEL | GFP_DMA32);
	if (!cmdbuf) {
		ret = -ENOMEM;
		goto unlock;
	}

	memset(cmdbuf, 0, FPC1020_CMDBUF_SIZE);
	if (copy_from_user(cmdbuf, u64_to_user_ptr(x.buf), x.len)) {
		ret = -EFAULT;
		goto free;
	}

	ret = fpc1020_send_locked(fpc1020, cmdbuf, x.len);
	if (!ret && copy_to_user(u64_to_user_ptr(x.buf), cmdbuf, x.len))
		ret = -EFAULT;

free:
	qcom_tzmem_free(cmdbuf);
unlock:
	mutex_unlock(&fpc1020->lock);
	return ret;
}

static long fpc1020_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fpc1020 *fpc1020 = file->private_data;

	switch (cmd) {
	case FPC_TEE_IOC_XFER:
		return fpc1020_xfer(fpc1020, arg);
	case FPC_TEE_IOC_LISTENER_RECV:
		return fpc1020_listener_recv(fpc1020, arg);
	case FPC_TEE_IOC_LISTENER_SEND:
		return fpc1020_listener_send(fpc1020, arg);
	default:
		return -ENOTTY;
	}
}

static __poll_t fpc1020_poll(struct file *file, poll_table *wait)
{
	struct fpc1020 *fpc1020 = file->private_data;

	poll_wait(file, &fpc1020->irq_wq, wait);

	return gpiod_get_value_cansleep(fpc1020->irq_gpio) > 0 ?
	       EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations fpc1020_fops = {
	.owner = THIS_MODULE,
	.open = fpc1020_open,
	.unlocked_ioctl = fpc1020_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.poll = fpc1020_poll,
	.llseek = noop_llseek,
};

static int fpc1020_probe(struct platform_device *pdev)
{
	struct qcom_tzmem_pool_config pool_cfg = {
		/* A command buffer and its envelope, each page-aligned. */
		.initial_size = 2 * PAGE_SIZE,
		.policy = QCOM_TZMEM_POLICY_STATIC,
	};
	struct device *dev = &pdev->dev;
	struct fpc1020 *fpc1020;
	unsigned int i;
	int irq, ret;

	fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020), GFP_KERNEL);
	if (!fpc1020)
		return -ENOMEM;

	fpc1020->dev = dev;
	mutex_init(&fpc1020->lock);
	spin_lock_init(&fpc1020->listener_lock);
	init_waitqueue_head(&fpc1020->irq_wq);
	init_waitqueue_head(&fpc1020->listener_req_wq);
	init_waitqueue_head(&fpc1020->listener_rsp_wq);
	platform_set_drvdata(pdev, fpc1020);

	ret = device_property_read_string(dev, "firmware-name", &fpc1020->firmware);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read firmware-name\n");

	ret = devm_regulator_get_enable(dev, "vdd-io");
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable vdd-io\n");

	fpc1020->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(fpc1020->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc1020->reset_gpio),
				     "cannot get reset gpio\n");

	fpc1020->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(fpc1020->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc1020->irq_gpio),
				     "cannot get irq gpio\n");

	irq = gpiod_to_irq(fpc1020->irq_gpio);
	if (irq < 0)
		return dev_err_probe(dev, irq, "irq gpio has no interrupt\n");

	fpc1020_hw_reset(fpc1020);

	ret = devm_request_threaded_irq(dev, irq, NULL, fpc1020_irq_handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					dev_name(dev), fpc1020);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request irq\n");

	/* A finger wakes the system while the trustlet waits for one. */
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;
	ret = devm_pm_set_wake_irq(dev, irq);
	if (ret)
		return ret;

	/* QSEE apps are 32-bit: everything handed to one lies below 4 GB. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "cannot set DMA mask\n");

	for (i = 0; i < FPC1020_NUM_LISTENERS; i++) {
		fpc1020->listener[i].buf =
			dmam_alloc_coherent(dev, fpc1020_listeners[i].size,
					    &fpc1020->listener[i].phys, GFP_KERNEL);
		if (!fpc1020->listener[i].buf)
			return -ENOMEM;
	}

	fpc1020->tzpool = devm_qcom_tzmem_pool_new(dev, &pool_cfg);
	if (IS_ERR(fpc1020->tzpool))
		return dev_err_probe(dev, PTR_ERR(fpc1020->tzpool),
				     "cannot create TZ memory pool\n");

	fpc1020->miscdev.minor = MISC_DYNAMIC_MINOR;
	fpc1020->miscdev.name = "fpc_tee";
	fpc1020->miscdev.fops = &fpc1020_fops;
	fpc1020->miscdev.parent = dev;

	return misc_register(&fpc1020->miscdev);
}

static void fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020 *fpc1020 = platform_get_drvdata(pdev);
	unsigned int i;

	misc_deregister(&fpc1020->miscdev);

	/* QSEE would otherwise keep writing into buffers devres frees. */
	for (i = 0; i < FPC1020_NUM_LISTENERS; i++)
		if (fpc1020->listener[i].registered &&
		    qcom_scm_qseecom_listener_unregister(fpc1020_listeners[i].id))
			dev_err(&pdev->dev, "cannot unregister listener 0x%x\n",
				fpc1020_listeners[i].id);
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020" },
	{ }
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name = "fpc1020",
		.of_match_table = fpc1020_of_match,
	},
	.probe = fpc1020_probe,
	.remove = fpc1020_remove,
};
module_platform_driver(fpc1020_driver);

MODULE_AUTHOR("Giuseppe Maggio <jertlok@proton.me>");
MODULE_DESCRIPTION("FPC fingerprint sensor behind a QSEE trustlet");
MODULE_LICENSE("GPL");
