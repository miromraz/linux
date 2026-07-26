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

#include <linux/delay.h>
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
		smp2p_sleepstate_set(ss, false);
		usleep_range(SLEEPSTATE_QUIESCE_US, SLEEPSTATE_QUIESCE_US + 500);
		break;
	case PM_POST_SUSPEND:
		smp2p_sleepstate_set(ss, true);
		break;
	}

	return NOTIFY_DONE;
}

static int smp2p_sleepstate_probe(struct platform_device *pdev)
{
	struct smp2p_sleepstate *ss;
	unsigned int bit;
	int ret;

	ss = devm_kzalloc(&pdev->dev, sizeof(*ss), GFP_KERNEL);
	if (!ss)
		return -ENOMEM;

	ss->state = devm_qcom_smem_state_get(&pdev->dev, NULL, &bit);
	if (IS_ERR(ss->state))
		return dev_err_probe(&pdev->dev, PTR_ERR(ss->state),
				     "failed to acquire sleepstate\n");
	ss->mask = BIT(bit);

	/* We are running, so say so before anyone can ask. */
	smp2p_sleepstate_set(ss, true);

	/*
	 * Run before the notifiers that freeze userspace, so the remote has
	 * already been told by the time its RPC service stops responding.
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
