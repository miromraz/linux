// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tell the ADSP whether the application processor is suspended.
 *
 * The sensor framework (SEE) on the ADSP watches the AP's "sleepstate" SMP2P
 * entry and batches or wakes accordingly. Without it, SEE's remote processor
 * state thread faults when the AP enters system sleep and takes the whole
 * ADSP (audio included) down with it. The ADSP can in turn raise its
 * "sleepstate_see" entry to ask for the AP to stay up briefly.
 *
 * Based on the Qualcomm downstream smp2p_sleepstate driver.
 * Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 */
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

#define SLEEPSTATE_AP_AWAKE	BIT(12)

struct sleepstate {
	struct qcom_smem_state *state;
	struct wakeup_source *ws;
	struct notifier_block pm_nb;
};

static int sleepstate_pm_notifier(struct notifier_block *nb,
				  unsigned long event, void *unused)
{
	struct sleepstate *ss = container_of(nb, struct sleepstate, pm_nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		qcom_smem_state_update_bits(ss->state, SLEEPSTATE_AP_AWAKE, 0);
		break;
	case PM_POST_SUSPEND:
		qcom_smem_state_update_bits(ss->state, SLEEPSTATE_AP_AWAKE,
					    SLEEPSTATE_AP_AWAKE);
		break;
	}

	return NOTIFY_DONE;
}

static irqreturn_t sleepstate_see_irq(int irq, void *data)
{
	struct sleepstate *ss = data;

	__pm_wakeup_event(ss->ws, 200);

	return IRQ_HANDLED;
}

static void sleepstate_unregister(void *data)
{
	struct sleepstate *ss = data;

	unregister_pm_notifier(&ss->pm_nb);
	wakeup_source_unregister(ss->ws);
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sleepstate *ss;
	unsigned int bit;
	int irq, ret;

	ss = devm_kzalloc(dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->state = devm_qcom_smem_state_get(dev, NULL, &bit);
	if (IS_ERR(ss->state))
		return dev_err_probe(dev, PTR_ERR(ss->state),
				     "failed to get sleepstate smem state\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ss->ws = wakeup_source_register(dev, "smp2p-sleepstate");
	if (!ss->ws)
		return -ENOMEM;

	qcom_smem_state_update_bits(ss->state, SLEEPSTATE_AP_AWAKE,
				    SLEEPSTATE_AP_AWAKE);

	ss->pm_nb.notifier_call = sleepstate_pm_notifier;
	ss->pm_nb.priority = INT_MAX;
	ret = register_pm_notifier(&ss->pm_nb);
	if (ret) {
		wakeup_source_unregister(ss->ws);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, sleepstate_unregister, ss);
	if (ret)
		return ret;

	return devm_request_threaded_irq(dev, irq, NULL, sleepstate_see_irq,
					 IRQF_ONESHOT, "smp2p-sleepstate", ss);
}

static const struct of_device_id smp2p_sleepstate_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{}
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.driver = {
		.name = "smp2p-sleepstate",
		.of_match_table = smp2p_sleepstate_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P AP sleep state notifier");
MODULE_LICENSE("GPL");
