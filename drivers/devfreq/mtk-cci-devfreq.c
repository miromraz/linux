// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/clk/mediatek-dvfs.h>
#include <linux/devfreq.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/mt6797.h>

struct mtk_ccifreq_platform_data {
	int min_volt_shift;
	int max_volt_shift;
	int proc_max_volt;
	int sram_min_volt;
	int sram_max_volt;
};

struct mtk_ccifreq_drv {
	u64 rail_cookie;
	struct mutex callback_lock;
	struct mt6797_txn_outcome outcome;
	struct device *dev;
	struct devfreq *devfreq;
	struct regulator *proc_reg;
	struct regulator *sram_reg;
	struct clk *cci_clk;
	struct clk *inter_clk;
	int inter_voltage;
	unsigned long pre_freq;
	bool dvfs_fault;
	/* Avoid race condition for regulators between notify and policy */
	struct mutex reg_lock;
	struct notifier_block opp_nb;
	const struct mtk_ccifreq_platform_data *soc_data;
	int vtrack_max;
	int opp_token;
};

/*
 * Select the per-speed-bin OPP set, mirroring mediatek-cpufreq: the efuse
 * "speed_grade" cell gives the bin, and each OPP lists the bins it applies to
 * through opp-supported-hw. SoCs without the cell (or without CONFIG_NVMEM)
 * have no binning and use every OPP.
 */
static int mtk_ccifreq_opp_set_supported_hw(struct mtk_ccifreq_drv *drv)
{
	struct dev_pm_opp_config config = {};
	unsigned int supported_hw;
	u8 bin;
	int ret;

	ret = nvmem_cell_read_u8(drv->dev, "speed_grade", &bin);
	if (ret == -ENOENT || ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;

	supported_hw = BIT(bin);
	config.supported_hw = &supported_hw;
	config.supported_hw_count = 1;

	ret = dev_pm_opp_set_config(drv->dev, &config);
	if (ret < 0)
		return ret;

	drv->opp_token = ret;

	return 0;
}

static int mtk_ccifreq_set_voltage(struct mtk_ccifreq_drv *drv, int new_voltage)
{
	const struct mtk_ccifreq_platform_data *soc_data = drv->soc_data;
	struct device *dev = drv->dev;
	int pre_voltage, pre_vsram, new_vsram, vsram, voltage, ret;
	int retry_max = drv->vtrack_max;

	if (!drv->sram_reg) {
		ret = regulator_set_voltage(drv->proc_reg, new_voltage,
					    drv->soc_data->proc_max_volt);
		return ret;
	}

	pre_voltage = regulator_get_voltage(drv->proc_reg);
	if (pre_voltage < 0) {
		dev_err(dev, "invalid vproc value: %d\n", pre_voltage);
		return pre_voltage;
	}

	pre_vsram = regulator_get_voltage(drv->sram_reg);
	if (pre_vsram < 0) {
		dev_err(dev, "invalid vsram value: %d\n", pre_vsram);
		return pre_vsram;
	}

	new_vsram = clamp(new_voltage + soc_data->min_volt_shift,
			  soc_data->sram_min_volt, soc_data->sram_max_volt);

	do {
		if (pre_voltage <= new_voltage) {
			vsram = clamp(pre_voltage + soc_data->max_volt_shift,
				      soc_data->sram_min_volt, new_vsram);
			ret = regulator_set_voltage(drv->sram_reg, vsram,
						    soc_data->sram_max_volt);
			if (ret)
				return ret;

			if (vsram == soc_data->sram_max_volt ||
			    new_vsram == soc_data->sram_min_volt)
				voltage = new_voltage;
			else
				voltage = vsram - soc_data->min_volt_shift;

			ret = regulator_set_voltage(drv->proc_reg, voltage,
						    soc_data->proc_max_volt);
			if (ret)
				return ret;
		} else {
			voltage = max(new_voltage,
				      pre_vsram - soc_data->max_volt_shift);
			ret = regulator_set_voltage(drv->proc_reg, voltage,
						    soc_data->proc_max_volt);
			if (ret)
				return ret;

			if (voltage == new_voltage)
				vsram = new_vsram;
			else
				vsram = max(new_vsram,
					    voltage + soc_data->min_volt_shift);

			ret = regulator_set_voltage(drv->sram_reg, vsram,
						    soc_data->sram_max_volt);
			if (ret)
				return ret;
		}

		pre_voltage = voltage;
		pre_vsram = vsram;

		if (--retry_max < 0) {
			dev_err(dev,
				"over loop count, failed to set voltage\n");
			return -EINVAL;
		}
	} while (voltage != new_voltage || vsram != new_vsram);

	/*
	 * A Vproc floor on the proc regulator can hold the real Vproc above the
	 * requested value; the loop tracks the requested Vproc, so re-read both
	 * rails and raise Vsram if it ended up below the real Vproc plus the
	 * minimum shift. This only ever raises Vsram (a no-op where Vproc is not
	 * floored).
	 */
	pre_voltage = regulator_get_voltage(drv->proc_reg);
	pre_vsram = regulator_get_voltage(drv->sram_reg);
	if (pre_voltage > 0 && pre_vsram > 0) {
		vsram = clamp(pre_voltage + soc_data->min_volt_shift,
			      soc_data->sram_min_volt, soc_data->sram_max_volt);
		if (vsram > pre_vsram) {
			ret = regulator_set_voltage(drv->sram_reg, vsram,
						    soc_data->sram_max_volt);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/*
 * The registry owns all regulator requests and live OPP reservations. Never
 * hold reg_lock across begin/disposal: deferred OPP puts can notify here.
 * Admission remains closed until devfreq can consume partial/unknown outcomes.
 */
static int mtk_ccifreq_owned_target(struct mtk_ccifreq_drv *drv,
				  unsigned long *freq)
{
	struct mt6797_consumer_txn *txn;
	int ret;

	if (!mutex_trylock(&drv->callback_lock))
		return -EBUSY;
	if (drv->dvfs_fault) {
		ret = -EIO;
		goto unlock;
	}
	ret = mt6797_consumer_txn_begin(2, drv->rail_cookie, *freq, &txn);
	if (ret) {
		drv->outcome.path = MT6797_TXN_OLD;
		drv->outcome.rate = drv->pre_freq;
		drv->outcome.retry = true;
		goto unlock;
	}
	ret = mt6797_consumer_txn_prepare(txn);
	if (!ret)
		ret = mt6797_consumer_txn_switch(txn);
	if (ret)
		mt6797_consumer_txn_abort(txn, ret, &drv->outcome);
	else
		ret = mt6797_consumer_txn_finish(txn, &drv->outcome);
	drv->pre_freq = drv->outcome.rate;
	drv->dvfs_fault = ret && !drv->outcome.retry;
unlock:
	drv->outcome.error = ret;
	/* Zero denotes unknown; errors never claim the requested rate occurred. */
	*freq = drv->pre_freq;
	mutex_unlock(&drv->callback_lock);
	return ret;
}

static int mtk_ccifreq_target(struct device *dev, unsigned long *freq,
			      u32 flags)
{
	struct mtk_ccifreq_drv *drv = dev_get_drvdata(dev);
	struct clk *cci_pll;
	struct dev_pm_opp *opp;
	unsigned long opp_rate;
	bool unchanged;
	int voltage, pre_voltage, inter_voltage, target_voltage, ret;

	if (!drv)
		return -EINVAL;
	if (drv->rail_cookie)
		return mtk_ccifreq_owned_target(drv, freq);

	mutex_lock(&drv->reg_lock);

	if (drv->dvfs_fault) {
		ret = -EIO;
		goto out_unlock;
	}

	inter_voltage = drv->inter_voltage;
	cci_pll = clk_get_parent(drv->cci_clk);

	opp_rate = *freq;
	opp = devfreq_recommended_opp(dev, &opp_rate, 1);
	if (IS_ERR(opp)) {
		dev_err(dev, "failed to find opp for freq: %ld\n", opp_rate);
		ret = PTR_ERR(opp);
		goto out_unlock;
	}

	voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	/*
	 * devfreq_recommended_opp() rounded the requested frequency up to a
	 * real OPP in opp_rate. The incoming *freq may not be an OPP at all
	 * (the passive governor interpolates a frequency when no required-opps
	 * link the parent cpufreq OPPs to the CCI ones), so report the rounded
	 * rate back: the devfreq core records it as previous_freq and looks it
	 * up in the frequency table in devfreq_update_status(). Leaving *freq
	 * at a non-OPP value makes that lookup fail on every polling interval.
	 */
	*freq = opp_rate;

	if (drv->pre_freq == *freq && clk_get_rate(drv->cci_clk) == *freq) {
		ret = 0;
		goto out_unlock;
	}

	pre_voltage = regulator_get_voltage(drv->proc_reg);
	if (pre_voltage < 0) {
		dev_err(dev, "invalid vproc value: %d\n", pre_voltage);
		ret = pre_voltage;
		goto out_unlock;
	}

	/* Own a preparation floor; a peer's aggregate voltage is not a vote. */
	target_voltage = max(pre_voltage, max(inter_voltage, voltage));
	ret = mtk_ccifreq_set_voltage(drv, target_voltage);
	if (ret) {
		dev_err(dev, "failed to prepare voltage\n");
		goto out_fault;
	}

	ret = mtk_dvfs_switch_clock_tracked(drv->cci_clk, cci_pll, drv->inter_clk,
					   *freq, &unchanged);
	if (ret) {
		/* Retain our successful preparation request, and the old cache. */
		if (unchanged)
			goto out_unlock;
		goto out_fault;
	}

	/* Replace the preparation vote with our persistent target floor. */
	ret = mtk_ccifreq_set_voltage(drv, voltage);
	if (ret) {
		dev_err(dev, "failed to set target voltage\n");
		goto out_fault;
	}

	drv->pre_freq = *freq;
	mutex_unlock(&drv->reg_lock);

	return 0;

out_fault:
	/* Core request rollback does not imply physical voltage rollback. */
	drv->dvfs_fault = true;
	drv->pre_freq = 0;

out_unlock:
	mutex_unlock(&drv->reg_lock);
	return ret;
}

static int mtk_ccifreq_opp_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct dev_pm_opp *opp = data;
	struct mtk_ccifreq_drv *drv;
	unsigned long freq, volt;
	int ret = 0;

	drv = container_of(nb, struct mtk_ccifreq_drv, opp_nb);

	if (event == OPP_EVENT_ADJUST_VOLTAGE) {
		mutex_lock(&drv->reg_lock);
		freq = dev_pm_opp_get_freq(opp);

		/* current opp item is changed */
		if (drv->dvfs_fault) {
			ret = -EIO;
		} else if (freq == drv->pre_freq) {
			volt = dev_pm_opp_get_voltage(opp);
			ret = mtk_ccifreq_set_voltage(drv, volt);
			if (ret) {
				drv->dvfs_fault = true;
				drv->pre_freq = 0;
			}
		}
		mutex_unlock(&drv->reg_lock);
	}

	/* The OPP table is already committed and its caller ignores this result.
	 * Keep invalid state blocked locally, but let later listeners observe it.
	 */
	if (ret)
		dev_err(drv->dev, "OPP committed but voltage reconciliation failed: %d\n",
			ret);
	return NOTIFY_OK;
}

static struct devfreq_dev_profile mtk_ccifreq_profile = {
	.target = mtk_ccifreq_target,
};

static void mtk_ccifreq_release_protection(struct mtk_ccifreq_drv *drv)
{
	mutex_lock(&drv->callback_lock);
	mt6797_consumer_release(2, drv->rail_cookie);
	drv->rail_cookie = 0;
	mutex_unlock(&drv->callback_lock);
}

static int mtk_ccifreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_ccifreq_drv *drv;
	struct devfreq_passive_data *passive_data;
	struct dev_pm_opp *opp;
	unsigned long rate, opp_volt;
	int ret;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->dev = dev;
	mutex_init(&drv->callback_lock);
	if (of_machine_is_compatible("planet,gemini-pda")) {
		ret = mt6797_consumer_acquire(dev, 2, &drv->rail_cookie);
		if (ret)
			return ret;
		/* Phase3 remains closed. Use the normal protected release boundary. */
		mtk_ccifreq_release_protection(drv);
		return -EAGAIN;
	}
	drv->soc_data = (const struct mtk_ccifreq_platform_data *)
				of_device_get_match_data(&pdev->dev);
	mutex_init(&drv->reg_lock);
	platform_set_drvdata(pdev, drv);

	drv->cci_clk = devm_clk_get(dev, "cci");
	if (IS_ERR(drv->cci_clk)) {
		ret = PTR_ERR(drv->cci_clk);
		return dev_err_probe(dev, ret, "failed to get cci clk\n");
	}

	drv->inter_clk = devm_clk_get(dev, "intermediate");
	if (IS_ERR(drv->inter_clk)) {
		ret = PTR_ERR(drv->inter_clk);
		return dev_err_probe(dev, ret,
				     "failed to get intermediate clk\n");
	}

	drv->proc_reg = devm_regulator_get_optional(dev, "proc");
	if (IS_ERR(drv->proc_reg)) {
		ret = PTR_ERR(drv->proc_reg);
		return dev_err_probe(dev, ret,
				     "failed to get proc regulator\n");
	}

	ret = regulator_enable(drv->proc_reg);
	if (ret) {
		dev_err(dev, "failed to enable proc regulator\n");
		return ret;
	}

	drv->sram_reg = devm_regulator_get_optional(dev, "sram");
	if (IS_ERR(drv->sram_reg)) {
		ret = PTR_ERR(drv->sram_reg);
		if (ret == -EPROBE_DEFER)
			goto out_free_resources;

		drv->sram_reg = NULL;
	} else {
		ret = regulator_enable(drv->sram_reg);
		if (ret) {
			dev_err(dev, "failed to enable sram regulator\n");
			goto out_free_resources;
		}
	}

	/*
	 * We assume min voltage is 0 and tracking target voltage using
	 * min_volt_shift for each iteration.
	 * The retry_max is 3 times of expected iteration count.
	 */
	drv->vtrack_max = 3 * DIV_ROUND_UP(max(drv->soc_data->sram_max_volt,
					       drv->soc_data->proc_max_volt),
					   drv->soc_data->min_volt_shift);

	ret = clk_prepare_enable(drv->cci_clk);
	if (ret)
		goto out_free_resources;

	ret = mtk_ccifreq_opp_set_supported_hw(drv);
	if (ret) {
		dev_err(dev, "failed to set supported hw: %d\n", ret);
		goto out_disable_cci_clk;
	}

	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_err(dev, "failed to add opp table: %d\n", ret);
		goto out_clear_opp_config;
	}

	rate = clk_get_rate(drv->inter_clk);
	opp = dev_pm_opp_find_freq_ceil(dev, &rate);
	if (IS_ERR(opp)) {
		ret = PTR_ERR(opp);
		dev_err(dev, "failed to get intermediate opp: %d\n", ret);
		goto out_remove_opp_table;
	}
	drv->inter_voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	rate = U32_MAX;
	opp = dev_pm_opp_find_freq_floor(drv->dev, &rate);
	if (IS_ERR(opp)) {
		dev_err(dev, "failed to get opp\n");
		ret = PTR_ERR(opp);
		goto out_remove_opp_table;
	}

	opp_volt = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	ret = mtk_ccifreq_set_voltage(drv, opp_volt);
	if (ret) {
		dev_err(dev, "failed to scale to highest voltage %lu in proc_reg\n",
			opp_volt);
		goto out_remove_opp_table;
	}

	passive_data = devm_kzalloc(dev, sizeof(*passive_data), GFP_KERNEL);
	if (!passive_data) {
		ret = -ENOMEM;
		goto out_remove_opp_table;
	}

	passive_data->parent_type = CPUFREQ_PARENT_DEV;
	drv->devfreq = devm_devfreq_add_device(dev, &mtk_ccifreq_profile,
					       DEVFREQ_GOV_PASSIVE,
					       passive_data);
	if (IS_ERR(drv->devfreq)) {
		ret = -EPROBE_DEFER;
		dev_err(dev, "failed to add devfreq device: %ld\n",
			PTR_ERR(drv->devfreq));
		goto out_remove_opp_table;
	}

	drv->opp_nb.notifier_call = mtk_ccifreq_opp_notifier;
	ret = dev_pm_opp_register_notifier(dev, &drv->opp_nb);
	if (ret) {
		dev_err(dev, "failed to register opp notifier: %d\n", ret);
		goto out_remove_opp_table;
	}
	return 0;

out_remove_opp_table:
	dev_pm_opp_of_remove_table(dev);

out_clear_opp_config:
	dev_pm_opp_clear_config(drv->opp_token);

out_disable_cci_clk:
	clk_disable_unprepare(drv->cci_clk);

out_free_resources:
	if (regulator_is_enabled(drv->proc_reg))
		regulator_disable(drv->proc_reg);
	if (!IS_ERR_OR_NULL(drv->sram_reg) &&
	    regulator_is_enabled(drv->sram_reg))
		regulator_disable(drv->sram_reg);

	return ret;
}

static void mtk_ccifreq_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_ccifreq_drv *drv;

	drv = platform_get_drvdata(pdev);
	if (drv->rail_cookie) {
		mtk_ccifreq_release_protection(drv);
		return;
	}

	dev_pm_opp_unregister_notifier(dev, &drv->opp_nb);
	dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_clear_config(drv->opp_token);
	clk_disable_unprepare(drv->cci_clk);
	regulator_disable(drv->proc_reg);
	if (drv->sram_reg)
		regulator_disable(drv->sram_reg);
}

static const struct mtk_ccifreq_platform_data mt6797_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 300000,
	.proc_max_volt = 1200000,
	.sram_min_volt = 1000000,
	.sram_max_volt = 1200000,
};

static const struct mtk_ccifreq_platform_data mt8183_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1150000,
};

static const struct mtk_ccifreq_platform_data mt8186_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 250000,
	.proc_max_volt = 1118750,
	.sram_min_volt = 850000,
	.sram_max_volt = 1118750,
};

static const struct of_device_id mtk_ccifreq_machines[] = {
	{ .compatible = "mediatek,mt6797-cci", .data = &mt6797_platform_data },
	{ .compatible = "mediatek,mt8183-cci", .data = &mt8183_platform_data },
	{ .compatible = "mediatek,mt8186-cci", .data = &mt8186_platform_data },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_ccifreq_machines);

static struct platform_driver mtk_ccifreq_platdrv = {
	.probe	= mtk_ccifreq_probe,
	.remove = mtk_ccifreq_remove,
	.driver = {
		.name = "mtk-ccifreq",
		.of_match_table = mtk_ccifreq_machines,
	},
};
module_platform_driver(mtk_ccifreq_platdrv);

MODULE_DESCRIPTION("MediaTek CCI devfreq driver");
MODULE_AUTHOR("Jia-Wei Chang <jia-wei.chang@mediatek.com>");
MODULE_LICENSE("GPL v2");

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include "../regulator/mt6797-combined-test.h"
int mt6797_test_cci_probe(struct platform_device *pdev)
{
	bool destroyed = false;
	void *group;
	int ret;

	group = devres_open_group(&pdev->dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	ret = devm_add_action(&pdev->dev, mt6797_test_probe_destroyed, &destroyed);
	if (ret) {
		devres_release_group(&pdev->dev, group);
		return ret;
	}
	ret = mtk_ccifreq_probe(pdev);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		devres_release_group(&pdev->dev, group);
		if (!destroyed)
			return -EUCLEAN;
	} else {
		devm_remove_action(&pdev->dev, mt6797_test_probe_destroyed, &destroyed);
		devres_remove_group(&pdev->dev, group);
	}
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
/* Remove is unreachable after the gated probe: install only its owned lease. */
int mt6797_test_cci_remove(struct platform_device *pdev)
{
	struct mtk_ccifreq_drv *drv;
	void *group;
	int ret;

	group = devres_open_group(&pdev->dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		ret = -ENOMEM;
		goto out;
	}
	mutex_init(&drv->callback_lock);
	ret = mt6797_consumer_acquire(&pdev->dev, 2, &drv->rail_cookie);
	if (ret)
		goto out;
	platform_set_drvdata(pdev, drv);
	mtk_ccifreq_remove(pdev);
	platform_set_drvdata(pdev, NULL);
out:
	devres_release_group(&pdev->dev, group);
	return ret;
}
#endif

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include <kunit/test.h>
#include "../regulator/mt6797-provider.h"

static int cci_test_veto(struct notifier_block *nb, unsigned long event, void *data)
{
	return event == PRE_RATE_CHANGE ? NOTIFY_BAD : NOTIFY_OK;
}

/* Execute the production dispatch, retaining callback state across attempts. */
void mt6797_test_cci_targets(struct kunit *test, struct device *dev,
			   u64 cookie, struct clk *mux)
{
	struct mtk_ccifreq_drv drv = { .dev = dev, .rail_cookie = cookie,
		.pre_freq = 100 };
	void *saved = dev_get_drvdata(dev);
	struct notifier_block veto = { .notifier_call = cci_test_veto };
	unsigned long rate;
	int guard, vote;

	mutex_init(&drv.callback_lock);
	dev_set_drvdata(dev, &drv);
	KUNIT_EXPECT_EQ(test, clk_notifier_register(mux, &veto), 0);
	rate = 200;
	KUNIT_EXPECT_EQ(test, mtk_ccifreq_target(dev, &rate, 0), -EBUSY);
	KUNIT_EXPECT_EQ(test, rate, 100UL);
	KUNIT_EXPECT_TRUE(test, drv.outcome.retry);
	KUNIT_EXPECT_FALSE(test, drv.dvfs_fault);
	KUNIT_EXPECT_EQ(test, clk_notifier_unregister(mux, &veto), 0);
	mt6797_preflight_test_flush();
	rate = 200;
	KUNIT_EXPECT_EQ(test, mtk_ccifreq_target(dev, &rate, 0), 0);
	KUNIT_EXPECT_EQ(test, rate, 200UL);
	KUNIT_EXPECT_EQ(test, clk_get_rate(mux), rate);
	KUNIT_EXPECT_EQ(test, drv.outcome.retained_uv, 1100000);
	mt6797_preflight_test_flush();
	rate = 100;
	KUNIT_EXPECT_EQ(test, mtk_ccifreq_target(dev, &rate, 0), 0);
	KUNIT_EXPECT_EQ(test, rate, 100UL);
	KUNIT_EXPECT_EQ(test, clk_get_rate(mux), rate);
	KUNIT_EXPECT_EQ(test, drv.outcome.retained_uv, 1080000);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 2);
	KUNIT_EXPECT_EQ(test, vote, 1080000);
	KUNIT_EXPECT_FALSE(test, drv.dvfs_fault);
	mt6797_preflight_test_flush();
	dev_set_drvdata(dev, saved);
}
#endif
