// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2019 MediaTek Inc.

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/suspend.h>
#include <linux/mfd/mt6323/core.h>
#include <linux/mfd/mt6323/registers.h>
#include <linux/mfd/mt6328/core.h>
#include <linux/mfd/mt6328/registers.h>
#include <linux/mfd/mt6331/core.h>
#include <linux/mfd/mt6331/registers.h>
#include <linux/mfd/mt6351/core.h>
#include <linux/mfd/mt6351/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6397/registers.h>

/* Fast recovery, then one probe per five seconds while the parent is masked. */
#define MT6397_IRQ_RETRIES 5
#define MT6397_IRQ_RETRY_MS 100
#define MT6397_IRQ_SLOW_MS 5000

static int mt6397_irq_handle_reg(struct mt6397_chip *chip, int reg, int irqbase);

/* irqlock protects state, deadlines and desired/read-back mask ownership. */
static void mt6397_irq_schedule(struct mt6397_chip *chip)
{
	unsigned long now = jiffies;
	unsigned long delay = time_before(now, chip->irq_retry_at) ?
		chip->irq_retry_at - now : 0;

	if (!chip->irq_stopping && !chip->irq_suspended)
		queue_delayed_work(system_freezable_wq, &chip->irq_retry_work, delay);
}

static void mt6397_irq_failed(struct mt6397_chip *chip, int error)
{
	chip->irq_masks_valid = 0;
	if (chip->irq_stopping)
		return;
	if (!chip->irq_disabled) {
		chip->irq_disabled = true;
		disable_irq_nosync(chip->irq);
		chip->irq_retry_at = jiffies + msecs_to_jiffies(MT6397_IRQ_RETRY_MS);
		dev_err_ratelimited(chip->dev, "IRQ bus error %d; parent masked\n",
				    error);
	}
	if (chip->irq_retries >= MT6397_IRQ_RETRIES && !chip->irq_exhausted) {
		chip->irq_exhausted = true;
		dev_err(chip->dev, "PMIC IRQ delivery degraded; slow recovery probes\n");
	}
	mt6397_irq_schedule(chip);
}

static int mt6397_irq_sync_masks(struct mt6397_chip *chip)
{
	unsigned int i, actual;
	int ret;

	for (i = 0; i < chip->num_irq_regs; i++) {
		u16 mask = chip->irq_suspended ? chip->wake_mask[i] :
						chip->irq_masks_cur[i];

		/* Force writes: an error leaves the hardware value unknown. */
		ret = regmap_write(chip->regmap, chip->int_con[i], mask);
		if (!ret)
			ret = regmap_read_bypassed(chip->regmap, chip->int_con[i], &actual);
		if (!ret && actual != mask)
			ret = -EIO;
		if (ret) {
			chip->irq_masks_valid = 0;
			return ret;
		}
		chip->irq_masks_cache[i] = mask;
		chip->irq_masks_valid |= BIT(i);
	}
	return 0;
}

static void mt6397_irq_retry(struct work_struct *work)
{
	struct mt6397_chip *chip = container_of(to_delayed_work(work),
					struct mt6397_chip, irq_retry_work);
	unsigned int i, delay;
	int ret, scan_ret;

	/* Never wait for the parent while holding a nested chip's bus lock. */
	synchronize_irq(chip->irq);
	mutex_lock(&chip->irqlock);
	if (chip->irq_stopping || chip->irq_suspended || !chip->irq_disabled)
		goto out;
	/* A queued invocation from the previous phase cannot spend its budget. */
	if (time_before(jiffies, chip->irq_retry_at)) {
		mt6397_irq_schedule(chip);
		goto out;
	}
	if (chip->irq_retries < MT6397_IRQ_RETRIES)
		chip->irq_retries++;
	delay = chip->irq_retries >= MT6397_IRQ_RETRIES ?
		MT6397_IRQ_SLOW_MS : MT6397_IRQ_RETRY_MS;
	chip->irq_retry_at = jiffies + msecs_to_jiffies(delay);
	if (chip->irq_wake_enabled) {
		ret = disable_irq_wake(chip->irq);
		if (ret) {
			mt6397_irq_failed(chip, ret);
			goto out;
		}
		chip->irq_wake_enabled = false;
	}
	ret = mt6397_irq_sync_masks(chip);
	if (ret) {
		mt6397_irq_failed(chip, ret);
		goto out;
	}
	/*
	 * Parent remains disabled throughout status delivery and W1C completion.
	 * Nested children may change desired masks, so reconcile again afterward.
	 */
	mutex_unlock(&chip->irqlock);
	for (i = 0; i < chip->num_irq_regs; i++) {
		/* A bad bank must not starve accessible critical sources elsewhere. */
		scan_ret = mt6397_irq_handle_reg(chip, chip->int_status[i], i * 16);
		if (!ret)
			ret = scan_ret;
	}
	mutex_lock(&chip->irqlock);
	if (chip->irq_stopping)
		goto out;
	if (!ret)
		ret = mt6397_irq_sync_masks(chip);
	if (ret) {
		mt6397_irq_failed(chip, ret);
		goto out;
	}
	chip->irq_retries = 0;
	chip->irq_exhausted = false;
	/* Nonblocking cancellation is safe under irqlock. Stale work is harmless. */
	cancel_delayed_work(&chip->irq_retry_work);
	chip->irq_disabled = false;
	enable_irq(chip->irq);
out:
	mutex_unlock(&chip->irqlock);
}

static void mt6397_irq_lock(struct irq_data *data)
{
	struct mt6397_chip *mt6397 = irq_data_get_irq_chip_data(data);

	mutex_lock(&mt6397->irqlock);
}

static void mt6397_irq_sync_unlock(struct irq_data *data)
{
	struct mt6397_chip *mt6397 = irq_data_get_irq_chip_data(data);
	int ret;

	if (!mt6397->irq_stopping && !mt6397->irq_disabled) {
		ret = mt6397_irq_sync_masks(mt6397);
		if (ret)
			mt6397_irq_failed(mt6397, ret);
	}

	mutex_unlock(&mt6397->irqlock);
}

static void mt6397_irq_disable(struct irq_data *data)
{
	struct mt6397_chip *mt6397 = irq_data_get_irq_chip_data(data);
	int shift = data->hwirq & 0xf;
	int reg = data->hwirq >> 4;

	mt6397->irq_masks_cur[reg] &= ~BIT(shift);
}

static void mt6397_irq_enable(struct irq_data *data)
{
	struct mt6397_chip *mt6397 = irq_data_get_irq_chip_data(data);
	int shift = data->hwirq & 0xf;
	int reg = data->hwirq >> 4;

	mt6397->irq_masks_cur[reg] |= BIT(shift);
}

static int mt6397_irq_set_wake(struct irq_data *irq_data, unsigned int on)
{
	struct mt6397_chip *mt6397 = irq_data_get_irq_chip_data(irq_data);
	int shift = irq_data->hwirq & 0xf;
	int reg = irq_data->hwirq >> 4;

	if (on)
		mt6397->wake_mask[reg] |= BIT(shift);
	else
		mt6397->wake_mask[reg] &= ~BIT(shift);

	return 0;
}

static struct irq_chip mt6397_irq_chip = {
	.name = "mt6397-irq",
	.irq_bus_lock = mt6397_irq_lock,
	.irq_bus_sync_unlock = mt6397_irq_sync_unlock,
	.irq_enable = mt6397_irq_enable,
	.irq_disable = mt6397_irq_disable,
	.irq_set_wake = pm_sleep_ptr(mt6397_irq_set_wake),
};

static int mt6397_irq_handle_reg(struct mt6397_chip *mt6397, int reg,
				 int irqbase)
{
	unsigned int status = 0;
	int i, irq, ret;

	ret = regmap_read(mt6397->regmap, reg, &status);
	if (ret)
		return ret;

	for (i = 0; i < 16; i++) {
		if (status & BIT(i)) {
			irq = irq_find_mapping(mt6397->irq_domain, irqbase + i);
			if (irq)
				handle_nested_irq(irq);
		}
	}

	ret = regmap_write(mt6397->regmap, reg, status);
	if (ret)
		return ret;
	/*
	 * Drain the posted wrapper write. New status is not an ack failure:
	 * the same source may have asserted again. Never clear this new snapshot.
	 */
	return regmap_read_bypassed(mt6397->regmap, reg, &status);
}

static irqreturn_t mt6397_irq_thread(int irq, void *data)
{
	struct mt6397_chip *mt6397 = data;
	unsigned int i;
	int ret = 0;

	mutex_lock(&mt6397->irqlock);
	if (mt6397->irq_disabled || mt6397->irq_stopping) {
		mutex_unlock(&mt6397->irqlock);
		return IRQ_HANDLED;
	}
	mutex_unlock(&mt6397->irqlock);

	/* Nested actions may take irqlock via their irqchip callbacks. */
	for (i = 0; i < mt6397->num_irq_regs; i++) {
		ret = mt6397_irq_handle_reg(mt6397, mt6397->int_status[i], i * 16);
		if (ret)
			break;
	}
	mutex_lock(&mt6397->irqlock);
	if (ret) {
		mt6397_irq_failed(mt6397, ret);
	} else if (!mt6397->irq_disabled) {
		mt6397->irq_retries = 0;
		mt6397->irq_exhausted = false;
	}
	mutex_unlock(&mt6397->irqlock);

	return IRQ_HANDLED;
}

static int mt6397_irq_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hw)
{
	struct mt6397_chip *mt6397 = d->host_data;

	irq_set_chip_data(irq, mt6397);
	irq_set_chip_and_handler(irq, &mt6397_irq_chip, handle_level_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops mt6397_irq_domain_ops = {
	.map = mt6397_irq_domain_map,
};

static int mt6397_irq_pm_notifier(struct notifier_block *notifier,
				  unsigned long pm_event, void *unused)
{
	struct mt6397_chip *chip =
		container_of(notifier, struct mt6397_chip, pm_nb);
	int ret = 0;

	mutex_lock(&chip->irqlock);
	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		if (chip->irq_disabled || chip->irq_stopping) {
			ret = -EBUSY;
			break;
		}
		chip->irq_suspended = true;
		ret = mt6397_irq_sync_masks(chip);
		if (!ret)
			ret = enable_irq_wake(chip->irq);
		if (!ret) {
			chip->irq_wake_enabled = true;
			break;
		}
		chip->irq_suspended = false;
		mt6397_irq_failed(chip, ret);
		break;
	case PM_POST_SUSPEND:
		if (chip->irq_wake_enabled) {
			ret = disable_irq_wake(chip->irq);
			if (!ret)
				chip->irq_wake_enabled = false;
		}
		chip->irq_suspended = false;
		if (!ret && !chip->irq_disabled)
			ret = mt6397_irq_sync_masks(chip);
		if (ret || chip->irq_disabled)
			mt6397_irq_failed(chip, ret ?: -EIO);
		break;
	default:
		mutex_unlock(&chip->irqlock);
		return NOTIFY_DONE;
	}
	mutex_unlock(&chip->irqlock);
	return notifier_from_errno(ret);
}

static void mt6397_irq_cleanup(void *data)
{
	struct mt6397_chip *chip = data;

	unregister_pm_notifier(&chip->pm_nb);
	mutex_lock(&chip->irqlock);
	chip->irq_stopping = true;
	if (!chip->irq_disabled) {
		chip->irq_disabled = true;
		disable_irq_nosync(chip->irq);
	}
	mutex_unlock(&chip->irqlock);
	cancel_delayed_work_sync(&chip->irq_retry_work);
	free_irq(chip->irq, chip);
	if (chip->irq_wake_enabled && disable_irq_wake(chip->irq))
		dev_err(chip->dev, "Failed to disarm parent IRQ wake on removal\n");
}

static void mt6397_irq_remove_domain(void *data)
{
	struct mt6397_chip *chip = data;

	unsigned int i;

	/* MFD resource mappings outlive the children, but not this domain. */
	for (i = 0; i < chip->num_irq_regs * 16; i++)
		irq_dispose_mapping(irq_find_mapping(chip->irq_domain, i));
	irq_domain_remove(chip->irq_domain);
}

int mt6397_irq_init(struct mt6397_chip *chip)
{
	int ret;

	mutex_init(&chip->irqlock);
	INIT_DELAYED_WORK(&chip->irq_retry_work, mt6397_irq_retry);

	switch (chip->chip_id) {
	case MT6323_CHIP_ID:
		chip->num_irq_regs = 2;
		chip->int_con[0] = MT6323_INT_CON0;
		chip->int_con[1] = MT6323_INT_CON1;
		chip->int_status[0] = MT6323_INT_STATUS0;
		chip->int_status[1] = MT6323_INT_STATUS1;
		break;
	case MT6328_CHIP_ID:
		chip->num_irq_regs = 3;
		chip->int_con[0] = MT6328_INT_CON0;
		chip->int_con[1] = MT6328_INT_CON1;
		chip->int_con[2] = MT6328_INT_CON2;
		chip->int_status[0] = MT6328_INT_STATUS0;
		chip->int_status[1] = MT6328_INT_STATUS1;
		chip->int_status[2] = MT6328_INT_STATUS2;
		break;
	case MT6331_CHIP_ID:
		chip->num_irq_regs = 2;
		chip->int_con[0] = MT6331_INT_CON0;
		chip->int_con[1] = MT6331_INT_CON1;
		chip->int_status[0] = MT6331_INT_STATUS_CON0;
		chip->int_status[1] = MT6331_INT_STATUS_CON1;
		break;
	case MT6351_CHIP_ID:
		chip->num_irq_regs = 4;
		chip->int_con[0] = MT6351_INT_CON0;
		chip->int_con[1] = MT6351_INT_CON1;
		chip->int_con[2] = MT6351_INT_CON2;
		chip->int_con[3] = MT6351_INT_CON3;
		chip->int_status[0] = MT6351_INT_STATUS0;
		chip->int_status[1] = MT6351_INT_STATUS1;
		chip->int_status[2] = MT6351_INT_STATUS2;
		chip->int_status[3] = MT6351_INT_STATUS3;
		break;
	case MT6391_CHIP_ID:
	case MT6397_CHIP_ID:
		chip->num_irq_regs = 2;
		chip->int_con[0] = MT6397_INT_CON0;
		chip->int_con[1] = MT6397_INT_CON1;
		chip->int_status[0] = MT6397_INT_STATUS0;
		chip->int_status[1] = MT6397_INT_STATUS1;
		break;

	default:
		dev_err(chip->dev, "unsupported chip: 0x%x\n", chip->chip_id);
		return -ENODEV;
	}

	/* Verify masks before requesting the parent, including the final bank. */
	ret = mt6397_irq_sync_masks(chip);
	if (ret)
		return ret;

	chip->pm_nb.notifier_call = mt6397_irq_pm_notifier;
	chip->irq_domain = irq_domain_create_linear(dev_fwnode(chip->dev),
						    chip->num_irq_regs * 16,
						    &mt6397_irq_domain_ops, chip);
	if (!chip->irq_domain) {
		dev_err(chip->dev, "could not create irq domain\n");
		return -ENOMEM;
	}

	ret = devm_add_action_or_reset(chip->dev, mt6397_irq_remove_domain, chip);
	if (ret)
		return ret;

	ret = request_threaded_irq(chip->irq, NULL, mt6397_irq_thread,
				   IRQF_ONESHOT, "mt6397-pmic", chip);
	if (ret) {
		dev_err(chip->dev, "failed to register irq=%d; err: %d\n",
			chip->irq, ret);
		return ret;
	}

	ret = devm_add_action_or_reset(chip->dev, mt6397_irq_cleanup, chip);
	if (ret)
		return ret;

	return register_pm_notifier(&chip->pm_nb);
}

#if IS_ENABLED(CONFIG_MFD_MT6397_IRQ_KUNIT_TEST)
#include "mt6397-irq-test.c"
#endif
