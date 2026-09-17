// SPDX-License-Identifier: GPL-2.0
/*
 * Tell the remote processors when the application processor is about to
 * suspend, over the "sleepstate" SMP2P entry.
 *
 * The DSPs keep sending the AP work - sensor reverse-RPC in particular - for as
 * long as they believe it is awake. Nothing tells them otherwise on suspend, so
 * requests issued just before the freezer runs are never serviced and the DSP
 * takes a fatal error. Clearing the sleepstate bit before suspending, and
 * giving the remote a moment to quiesce, avoids that.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/suspend.h>

/*
 * The remote needs a little time to see the bit change and stop talking to us
 * before the freezer stops servicing it. The value matches what the vendor
 * kernel uses and is tuned for SMP2P interrupt latency.
 */
#define SLEEPSTATE_QUIESCE_US	200000

struct smp2p_sleepstate {
	struct qcom_smem_state *state;
	struct notifier_block nb;
	struct completion ack;
	u32 mask;
};

static void smp2p_sleepstate_set(struct smp2p_sleepstate *ss, bool awake)
{
	qcom_smem_state_update_bits(ss->state, ss->mask, awake ? ss->mask : 0);
}

static int smp2p_sleepstate_pm_notify(struct notifier_block *nb,
				      unsigned long event, void *unused)
{
	struct smp2p_sleepstate *ss = container_of(nb, struct smp2p_sleepstate, nb);

	switch (event) {
	case PM_SUSPEND_PREPARE:
		/*
		 * Arm the ack before clearing the bit, then wait for the remote
		 * to acknowledge on "sleepstate_see". A live ADSP acks in well
		 * under the ceiling and we return early; with no DSP loaded the
		 * ack never comes and the timeout bounds the delay.
		 */
		reinit_completion(&ss->ack);
		smp2p_sleepstate_set(ss, false);
		wait_for_completion_timeout(&ss->ack,
					    usecs_to_jiffies(SLEEPSTATE_QUIESCE_US));
		break;
	case PM_POST_SUSPEND:
		smp2p_sleepstate_set(ss, true);
		break;
	}

	return NOTIFY_DONE;
}

/*
 * The remote acknowledges us on the "sleepstate_see" entry. It is not a wakeup
 * request: with the sensor stack streaming, this fires at ~2.5 Hz, so treating
 * it as one would abort every suspend. Complete the ack the suspend path may be
 * waiting on (harmless when it is not) and leave the interrupt counter behind -
 * it is the only direct measure of how busy the sensor island is.
 */
static irqreturn_t smp2p_sleepstate_isr(int irq, void *data)
{
	struct smp2p_sleepstate *ss = data;

	complete(&ss->ack);
	return IRQ_HANDLED;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct smp2p_sleepstate *ss;
	unsigned int bit;
	int irq, ret;

	ss = devm_kzalloc(&pdev->dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	init_completion(&ss->ack);

	ss->state = devm_qcom_smem_state_get(&pdev->dev, NULL, &bit);
	if (IS_ERR(ss->state))
		return dev_err_probe(&pdev->dev, PTR_ERR(ss->state),
				     "failed to acquire sleepstate\n");
	ss->mask = BIT(bit);

	/* We are running, so say so before anyone can ask. */
	smp2p_sleepstate_set(ss, true);

	/* smp2p inbound interrupts are nested, so this has to be threaded. */
	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						smp2p_sleepstate_isr,
						IRQF_ONESHOT, "sleepstate", ss);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to request sleepstate irq\n");
	} else if (irq != -ENXIO) {
		/*
		 * -ENXIO means no ack interrupt is wired up, which is allowed
		 * (the suspend path then just waits out the timeout). Anything
		 * else, including a bogus 0, is a real failure - map 0 to
		 * -EINVAL so dev_err_probe() cannot return success.
		 */
		return dev_err_probe(&pdev->dev, irq ? irq : -EINVAL,
				     "bad sleepstate irq\n");
	}

	/*
	 * Every PM notifier already runs before userspace is frozen, so the
	 * priority does not order us against the freezer. INT_MAX only puts us
	 * ahead of the other PM notifiers, telling the remote as early as
	 * possible within PM_SUSPEND_PREPARE.
	 */
	ss->nb.notifier_call = smp2p_sleepstate_pm_notify;
	ss->nb.priority = INT_MAX;
	ret = register_pm_notifier(&ss->nb);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ss);

	return 0;
}

static void smp2p_sleepstate_remove(struct platform_device *pdev)
{
	struct smp2p_sleepstate *ss = platform_get_drvdata(pdev);

	unregister_pm_notifier(&ss->nb);
}

static const struct of_device_id smp2p_sleepstate_of_match[] = {
	{ .compatible = "qcom,smp2p-sleepstate" },
	{}
};
MODULE_DEVICE_TABLE(of, smp2p_sleepstate_of_match);

static struct platform_driver smp2p_sleepstate_driver = {
	.probe = smp2p_sleepstate_probe,
	.remove = smp2p_sleepstate_remove,
	.driver = {
		.name = "smp2p_sleepstate",
		.of_match_table = smp2p_sleepstate_of_match,
	},
};
module_platform_driver(smp2p_sleepstate_driver);

MODULE_DESCRIPTION("Qualcomm SMP2P sleepstate notifier");
MODULE_LICENSE("GPL");
