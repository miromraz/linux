// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 CPU SRAM LDO regulator
 *
 * The little (LL) and big (L) Cortex-A53 clusters and the CCI are fed from a
 * bank of eight on-SoC SRAM LDOs in the always-on infracfg block (INFRACFG_AO
 * at 0x10001000). The vendor CPU DVFS driver programs all eight to one voltage
 * and uses it as the Vsram rail that must stay at or above Vproc (the DA9214
 * buck A rail) during a frequency change (drivers/misc/mediatek/base/power/
 * mt6797/mt_cpufreq.c, set_cur_volt_sram_l()/get_cur_volt_sram_l()). Exposing
 * it as a regulator lets mediatek-cpufreq and mtk-cci-devfreq use it as the
 * "sram"/"mem" supply and do the usual Vsram/Vproc tracking, which is what
 * allows CPU OPPs above the fixed-LDO 1.09 V cap.
 *
 * Register layout (offsets from INFRACFG_AO):
 *   CTRL_0 0xf98  [7:0]  per-LDO enable, 0xff = all eight on
 *   CTRL_1 0xf9c  four LDO voltage fields, one per byte, value in [3:0]
 *   CTRL_2 0xfa0  four more LDO voltage fields, same layout
 * All eight fields carry the same value. The voltage code maps linearly as
 * 25 mV per step from 0.90 V at code 3 (vendor VOLT_TO_PMIC_VAL), so the
 * 1.00-1.20 V operating range the vendor allows (MIN/MAX_VSRAM_VOLT) is
 * codes 7..15.
 */

#include <linux/bitops.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define MT6797_CPULDO_CTRL_0		0xf98
#define MT6797_CPULDO_CTRL_1		0xf9c
#define MT6797_CPULDO_CTRL_2		0xfa0

#define MT6797_CPULDO_EN_MASK		GENMASK(7, 0)
#define MT6797_CPULDO_VOLT_MASK		GENMASK(3, 0)

/* Voltage code 7 == 1.00 V, 25 mV per code, up to code 15 == 1.20 V. */
#define MT6797_CPULDO_MIN_SEL		7
#define MT6797_CPULDO_MIN_UV		1000000
#define MT6797_CPULDO_STEP_UV		25000
#define MT6797_CPULDO_N_VOLTAGES	9

static int mt6797_cpu_sram_set_voltage_sel(struct regulator_dev *rdev,
					   unsigned int sel)
{
	unsigned int code = sel + MT6797_CPULDO_MIN_SEL;
	unsigned int packed = code | (code << 8) | (code << 16) | (code << 24);
	int ret;

	ret = regmap_write(rdev->regmap, MT6797_CPULDO_CTRL_1, packed);
	if (ret)
		return ret;

	return regmap_write(rdev->regmap, MT6797_CPULDO_CTRL_2, packed);
}

static int mt6797_cpu_sram_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int val, code;
	int ret;

	ret = regmap_read(rdev->regmap, MT6797_CPULDO_CTRL_1, &val);
	if (ret)
		return ret;

	code = val & MT6797_CPULDO_VOLT_MASK;
	/* The vendor maps code 0 to 1.05 V (get_cur_volt_sram_l); report it. */
	if (code == 0)
		return (1050000 - MT6797_CPULDO_MIN_UV) / MT6797_CPULDO_STEP_UV;
	if (code < MT6797_CPULDO_MIN_SEL)
		return -EINVAL;

	return code - MT6797_CPULDO_MIN_SEL;
}

static const struct regulator_ops mt6797_cpu_sram_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.set_voltage_sel	= mt6797_cpu_sram_set_voltage_sel,
	.get_voltage_sel	= mt6797_cpu_sram_get_voltage_sel,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

static const struct regulator_desc mt6797_cpu_sram_desc = {
	.name		= "vsram-cpu",
	.ops		= &mt6797_cpu_sram_ops,
	.type		= REGULATOR_VOLTAGE,
	.owner		= THIS_MODULE,
	.n_voltages	= MT6797_CPULDO_N_VOLTAGES,
	.min_uV		= MT6797_CPULDO_MIN_UV,
	.uV_step	= MT6797_CPULDO_STEP_UV,
	/*
	 * Vendor up-slew: PMIC_VOLT_UP_SETTLE_TIME ~= 1 us per 12.5 mV; the
	 * regulator core then blocks for the settle after a raise, before the
	 * consumer (cpufreq/cci) raises Vproc.
	 */
	.ramp_delay	= 12500,
	.enable_reg	= MT6797_CPULDO_CTRL_0,
	.enable_mask	= MT6797_CPULDO_EN_MASK,
	.enable_val	= MT6797_CPULDO_EN_MASK,
};

static int mt6797_cpu_sram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct regmap *regmap;

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get parent infracfg regmap\n");

	config.dev = dev;
	config.of_node = np;
	config.regmap = regmap;
	config.init_data = of_get_regulator_init_data(dev, np,
						      &mt6797_cpu_sram_desc);

	rdev = devm_regulator_register(dev, &mt6797_cpu_sram_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register regulator\n");

	return 0;
}

static const struct of_device_id mt6797_cpu_sram_of_match[] = {
	{ .compatible = "mediatek,mt6797-cpu-sram" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6797_cpu_sram_of_match);

static struct platform_driver mt6797_cpu_sram_driver = {
	.probe = mt6797_cpu_sram_probe,
	.driver = {
		.name = "mt6797-cpu-sram-regulator",
		.of_match_table = mt6797_cpu_sram_of_match,
		/* This is a live CPU rail; do not allow unbind. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_cpu_sram_driver);

MODULE_DESCRIPTION("MediaTek MT6797 CPU SRAM LDO regulator");
MODULE_LICENSE("GPL");
