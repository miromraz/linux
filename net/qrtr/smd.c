// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Sony Mobile Communications Inc.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>

#include "qrtr.h"

/* Upper bound on packets queued while probe is still running */
#define QRTR_SMD_EARLY_MAX	64

struct qrtr_smd_dev {
	struct qrtr_endpoint ep;
	struct rpmsg_endpoint *channel;
	struct device *dev;

	/* Protected by qrtr_smd_lock */
	struct sk_buff_head early;	/* packets received before ep registered */
	bool ready;			/* ep registered, post packets directly */
	bool dead;			/* probe failed or removed, drop packets */
};

/*
 * The rpmsg core creates the endpoint, and so lets the transport deliver
 * packets, before it calls probe(). The per-channel state therefore has to be
 * creatable from the receive callback as well as from probe(), which needs a
 * lock that exists before the state does.
 * ponytail: one lock for all channels (a handful of remotes, low packet rate);
 * move it into a per-channel structure if that ever contends.
 */
static DEFINE_SPINLOCK(qrtr_smd_lock);

/* Called with qrtr_smd_lock held; @new is consumed if used */
static struct qrtr_smd_dev *qrtr_smd_dev_get(struct rpmsg_device *rpdev,
					     struct qrtr_smd_dev *new)
{
	struct qrtr_smd_dev *qdev = dev_get_drvdata(&rpdev->dev);

	if (qdev || !new)
		return qdev;

	skb_queue_head_init(&new->early);
	new->dev = &rpdev->dev;
	dev_set_drvdata(&rpdev->dev, new);

	return new;
}

/* from smd to qrtr */
static int qcom_smd_qrtr_callback(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 addr)
{
	struct qrtr_smd_dev *qdev;
	struct sk_buff *skb;
	unsigned long flags;
	int rc = 0;

	/*
	 * May run in atomic context, possibly before probe(). Alloc from
	 * devres with GFP_ATOMIC so the state outlives remove() until the
	 * endpoint is gone.
	 */
	spin_lock_irqsave(&qrtr_smd_lock, flags);
	qdev = dev_get_drvdata(&rpdev->dev);
	if (!qdev) {
		qdev = devm_kzalloc(&rpdev->dev, sizeof(*qdev), GFP_ATOMIC);
		qdev = qrtr_smd_dev_get(rpdev, qdev);
	}

	if (!qdev) {
		rc = -ENOMEM;
	} else if (qdev->dead) {
		/* torn down, the endpoint is about to go away */
	} else if (qdev->ready) {
		rc = qrtr_endpoint_post(&qdev->ep, data, len);
		if (rc == -EINVAL) {
			dev_err(qdev->dev, "invalid ipcrouter packet\n");
			/* return 0 to let smd drop the packet */
			rc = 0;
		}
	} else if (skb_queue_len(&qdev->early) >= QRTR_SMD_EARLY_MAX) {
		dev_warn_ratelimited(&rpdev->dev,
				     "early packet queue full, dropping %d-byte packet\n",
				     len);
		rc = -EAGAIN;
	} else {
		skb = alloc_skb(len, GFP_ATOMIC | __GFP_NOWARN);
		if (skb) {
			skb_put_data(skb, data, len);
			__skb_queue_tail(&qdev->early, skb);
			dev_warn_ratelimited(&rpdev->dev,
					     "queued %d-byte packet received before probe completed\n",
					     len);
		} else {
			rc = -ENOMEM;
		}
	}
	spin_unlock_irqrestore(&qrtr_smd_lock, flags);

	return rc;
}

/* from qrtr to smd */
static int qcom_smd_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_smd_dev *qdev = container_of(ep, struct qrtr_smd_dev, ep);
	int rc;

	rc = skb_linearize(skb);
	if (rc)
		goto out;

	rc = rpmsg_send(qdev->channel, skb->data, skb->len);

out:
	if (rc)
		kfree_skb(skb);
	else
		consume_skb(skb);
	return rc;
}

static int qcom_smd_qrtr_probe(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qdev, *spare;
	struct sk_buff *skb;
	unsigned long flags;
	int rc;

	spare = devm_kzalloc(&rpdev->dev, sizeof(*spare), GFP_KERNEL);
	if (!spare)
		return -ENOMEM;

	/* The callback may already have created the state; use it if so */
	spin_lock_irqsave(&qrtr_smd_lock, flags);
	qdev = qrtr_smd_dev_get(rpdev, spare);
	spin_unlock_irqrestore(&qrtr_smd_lock, flags);
	if (qdev != spare)
		devm_kfree(&rpdev->dev, spare);

	qdev->channel = rpdev->ept;
	qdev->ep.xmit = qcom_smd_qrtr_send;

	rc = qrtr_endpoint_register(&qdev->ep, QRTR_EP_NID_AUTO);
	if (rc) {
		spin_lock_irqsave(&qrtr_smd_lock, flags);
		qdev->dead = true;
		__skb_queue_purge(&qdev->early);
		spin_unlock_irqrestore(&qrtr_smd_lock, flags);
		return rc;
	}

	/*
	 * Deliver what arrived while we were registering, in order, and only
	 * then let the callback post directly. Both happen under the lock the
	 * callback takes, so nothing can overtake the queue.
	 */
	spin_lock_irqsave(&qrtr_smd_lock, flags);
	while ((skb = __skb_dequeue(&qdev->early))) {
		if (qrtr_endpoint_post(&qdev->ep, skb->data, skb->len) == -EINVAL)
			dev_err(qdev->dev, "invalid ipcrouter packet\n");
		kfree_skb(skb);
	}
	qdev->ready = true;
	spin_unlock_irqrestore(&qrtr_smd_lock, flags);

	dev_dbg(&rpdev->dev, "Qualcomm SMD QRTR driver probed\n");

	return 0;
}

static void qcom_smd_qrtr_remove(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qdev = dev_get_drvdata(&rpdev->dev);
	unsigned long flags;

	/*
	 * The endpoint outlives remove(), so keep qdev in drvdata (devres
	 * frees it later) and mark it dead for any late callback.
	 */
	spin_lock_irqsave(&qrtr_smd_lock, flags);
	qdev->ready = false;
	qdev->dead = true;
	spin_unlock_irqrestore(&qrtr_smd_lock, flags);

	qrtr_endpoint_unregister(&qdev->ep);
}

static const struct rpmsg_device_id qcom_smd_qrtr_smd_match[] = {
	{ "IPCRTR" },
	{}
};

static struct rpmsg_driver qcom_smd_qrtr_driver = {
	.probe = qcom_smd_qrtr_probe,
	.remove = qcom_smd_qrtr_remove,
	.callback = qcom_smd_qrtr_callback,
	.id_table = qcom_smd_qrtr_smd_match,
	.drv = {
		.name = "qcom_smd_qrtr",
	},
};

module_rpmsg_driver(qcom_smd_qrtr_driver);

MODULE_ALIAS("rpmsg:IPCRTR");
MODULE_DESCRIPTION("Qualcomm IPC-Router SMD interface driver");
MODULE_LICENSE("GPL v2");
