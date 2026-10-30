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
 * All eight fields must decode to the same voltage. The code maps linearly as
 * 25 mV per step from 0.90 V at code 3 (vendor VOLT_TO_PMIC_VAL), so the
 * 1.00-1.20 V operating range the vendor allows (MIN/MAX_VSRAM_VOLT) is
 * codes 7..15.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#include "mt6797-boot-barrier.h"

#define MT6797_CPULDO_CTRL_0		0xf98
#define MT6797_CPULDO_CTRL_1		0xf9c
#define MT6797_CPULDO_CTRL_2		0xfa0

#define MT6797_CPULDO_EN_MASK		GENMASK(7, 0)
#define MT6797_CPULDO_VOLT_MASK		GENMASK(3, 0)
/*
 * One CTRL word packs four per-LDO VOLT fields, each in bits [3:0] of its byte.
 * Update only those fields; a full-word write would clobber the [7:4] nibbles.
 */
#define MT6797_CPULDO_VOLT_FIELDS_MASK	(MT6797_CPULDO_VOLT_MASK | \
					 (MT6797_CPULDO_VOLT_MASK << 8) | \
					 (MT6797_CPULDO_VOLT_MASK << 16) | \
					 (MT6797_CPULDO_VOLT_MASK << 24))

/* Voltage code 7 == 1.00 V, 25 mV per code, up to code 15 == 1.20 V. */
#define MT6797_CPULDO_MIN_SEL		7
#define MT6797_CPULDO_MIN_UV		1000000
#define MT6797_CPULDO_STEP_UV		25000
#define MT6797_CPULDO_N_VOLTAGES	9

/* Normal callbacks are serialized by the regulator core's rdev lock.
 * This does not exclude firmware writers; SPM ownership remains a platform
 * prerequisite. Never treat a saved selector as live hardware readback.
 */
struct mt6797_cpu_sram {
	/* Reboot/reprobe required; normal reads must not clear containment. */
	struct mutex rail_lock;
	bool unresolved;
	bool boot_barrier;
	struct regmap *regmap;
	struct device *dev;
};

static int mt6797_cpu_sram_read_bank(struct regmap *regmap, unsigned int words[2])
{
	unsigned int enable, code;
	int ret, sel = -1, decoded, i;

	ret = regmap_read(regmap, MT6797_CPULDO_CTRL_0, &enable);
	if (ret)
		return ret;
	/* All-off is legal before the core's voltage-before-enable sequence. */
	enable &= MT6797_CPULDO_EN_MASK;
	if (enable && enable != MT6797_CPULDO_EN_MASK)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		ret = regmap_read(regmap, MT6797_CPULDO_CTRL_1 + 4 * i,
				  &words[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < 8; i++) {
		code = (words[i / 4] >> (8 * (i % 4))) & MT6797_CPULDO_VOLT_MASK;
		/* Vendor code 0 and code 9 both mean 1.05 V. */
		if (!code)
			code = 9;
		/* Codes 1/2 are 0.60/0.70 V, 3..6 are 0.90..0.975 V:
		 * electrically defined, but outside our operating table.
		 */
		if (code < MT6797_CPULDO_MIN_SEL)
			return -EINVAL;
		decoded = code - MT6797_CPULDO_MIN_SEL;
		if (sel >= 0 && decoded != sel)
			return -EINVAL;
		sel = decoded;
	}
	return sel;
}

static int mt6797_cpu_sram_read(struct regulator_dev *rdev, unsigned int words[2])
{
	return mt6797_cpu_sram_read_bank(rdev->regmap, words);
}

static unsigned int mt6797_cpu_sram_delay(unsigned int old_sel,
					  unsigned int new_sel)
{
	unsigned int delta, delay;

	if (old_sel == new_sel)
		return 0;
	/* BSP mt_cpufreq.c PMIC_VOLT_{UP,DOWN}_SETTLE_TIME and
	 * _calc_pmic_settle_time(), converted from 10 uV units to uV.
	 */
	if (new_sel > old_sel) {
		delta = (new_sel - old_sel) * MT6797_CPULDO_STEP_UV;
		delay = DIV_ROUND_UP(delta, 12500) + 5;
	} else {
		delta = (old_sel - new_sel) * MT6797_CPULDO_STEP_UV;
		delay = delta * 2 / 6250 + 5;
	}
	return max(delay, 25U);
}

static int mt6797_cpu_sram_get_voltage_sel(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	unsigned int words[2];
	int ret;

	if (sram->unresolved)
		return -EIO;
	ret = mt6797_cpu_sram_read(rdev, words);
	if (ret < 0)
		dev_err(&rdev->dev, "invalid SRAM bank or read failure: %d\n", ret);
	return ret;
}

static int mt6797_cpu_sram_program(struct mt6797_cpu_sram *sram,
				   unsigned int sel)
{
	unsigned int saved[2], check[2], code, packed;
	int old_sel, ret, restore = 0, err, i;

	if (sel >= MT6797_CPULDO_N_VOLTAGES)
		return -EINVAL;
	if (sram->unresolved)
		return -EIO;
	old_sel = mt6797_cpu_sram_read_bank(sram->regmap, saved);
	if (old_sel < 0) {
		dev_err(sram->dev, "cannot snapshot SRAM bank: %d\n", old_sel);
		return old_sel;
	}
	code = sel + MT6797_CPULDO_MIN_SEL;
	packed = code | (code << 8) | (code << 16) | (code << 24);
	for (i = 0; i < 2; i++) {
		ret = regmap_update_bits(sram->regmap, MT6797_CPULDO_CTRL_1 + 4 * i,
					 MT6797_CPULDO_VOLT_FIELDS_MASK, packed);
		if (ret)
			goto recover;
	}
	/* Own the wait: the core does not delay after a failed setter. */
	udelay(mt6797_cpu_sram_delay(old_sel, sel));
	ret = mt6797_cpu_sram_read_bank(sram->regmap, check);
	if (ret < 0)
		goto recover;
	if (ret == sel &&
	    (check[0] & MT6797_CPULDO_VOLT_FIELDS_MASK) == packed &&
	    (check[1] & MT6797_CPULDO_VOLT_FIELDS_MASK) == packed)
		return 0;
	ret = -EIO;

recover:
	/* One bounded attempt, both words even if the first restore fails.
	 * Wait the worst supported downward transition before and after it:
	 * partial progress/read errors do not supply a known starting voltage.
	 * Synthetic regmap faults test this; ordinary MMIO writes return zero.
	 */
	udelay(mt6797_cpu_sram_delay(MT6797_CPULDO_N_VOLTAGES - 1, 0));
	for (i = 0; i < 2; i++) {
		err = regmap_update_bits(sram->regmap, MT6797_CPULDO_CTRL_1 + 4 * i,
					 MT6797_CPULDO_VOLT_FIELDS_MASK, saved[i]);
		if (err)
			restore = err;
	}
	udelay(mt6797_cpu_sram_delay(MT6797_CPULDO_N_VOLTAGES - 1, 0));
	err = mt6797_cpu_sram_read_bank(sram->regmap, check);
	if (err != old_sel ||
	    (check[0] & MT6797_CPULDO_VOLT_FIELDS_MASK) !=
	    (saved[0] & MT6797_CPULDO_VOLT_FIELDS_MASK) ||
	    (check[1] & MT6797_CPULDO_VOLT_FIELDS_MASK) !=
	    (saved[1] & MT6797_CPULDO_VOLT_FIELDS_MASK))
		restore = -EIO;
	if (restore) {
		sram->unresolved = true;
		dev_err(sram->dev, "SRAM recovery unresolved (%d), operations blocked\n",
			restore);
	} else {
		dev_err(sram->dev, "SRAM programming failed (%d), bank restored\n", ret);
	}
	return ret;
}

static int mt6797_cpu_sram_set_voltage_sel_unlocked(struct regulator_dev *rdev,
						 unsigned int sel)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);

	if (sram->boot_barrier)
		return mt6797_boot_barrier_check(regulator_is_enabled_regmap(rdev),
			mt6797_cpu_sram_get_voltage_sel(rdev), sel);
	return mt6797_cpu_sram_program(sram, sel);
}

static int mt6797_cpu_sram_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&sram->rail_lock);
	ret = mt6797_cpu_sram_set_voltage_sel_unlocked(rdev, sel);
	mutex_unlock(&sram->rail_lock);
	return ret;
}

static int mt6797_cpu_sram_enable_unlocked(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	int ret;

	ret = mt6797_cpu_sram_get_voltage_sel(rdev);
	/* Registration tolerates -EINVAL for a missing enable operation. */
	if (ret < 0)
		return ret == -EINVAL ? -EIO : ret;
	if (sram->boot_barrier)
		return mt6797_boot_barrier_check(
			regulator_is_enabled_regmap(rdev), ret, ret);
	ret = regulator_enable_regmap(rdev);
	if (ret)
		return ret;
	udelay(mt6797_cpu_sram_delay(MT6797_CPULDO_N_VOLTAGES - 1, 0));
	ret = regulator_is_enabled_regmap(rdev);
	return ret > 0 ? 0 : (ret < 0 ? ret : -EIO);
}

static int mt6797_cpu_sram_enable(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&sram->rail_lock);
	ret = mt6797_cpu_sram_enable_unlocked(rdev);
	mutex_unlock(&sram->rail_lock);
	return ret;
}

static int mt6797_cpu_sram_disable_unlocked(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);

	if (sram->boot_barrier)
		return -EBUSY;
	return regulator_disable_regmap(rdev);
}

static int mt6797_cpu_sram_disable(struct regulator_dev *rdev)
{
	struct mt6797_cpu_sram *sram = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&sram->rail_lock);
	ret = mt6797_cpu_sram_disable_unlocked(rdev);
	mutex_unlock(&sram->rail_lock);
	return ret;
}

static const struct regulator_ops mt6797_cpu_sram_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.set_voltage_sel	= mt6797_cpu_sram_set_voltage_sel,
	.get_voltage_sel	= mt6797_cpu_sram_get_voltage_sel,
	.enable			= mt6797_cpu_sram_enable,
	.disable		= mt6797_cpu_sram_disable,
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
	.enable_reg	= MT6797_CPULDO_CTRL_0,
	.enable_mask	= MT6797_CPULDO_EN_MASK,
	.enable_val	= MT6797_CPULDO_EN_MASK,
};

static int mt6797_cpu_sram_owned_read_unlocked(void *data)
{
	struct mt6797_cpu_sram *sram = data;
	unsigned int enable, words[2];
	int ret;

	if (sram->unresolved)
		return -EIO;
	ret = regmap_read(sram->regmap, MT6797_CPULDO_CTRL_0, &enable);
	if (ret)
		return ret;
	if ((enable & MT6797_CPULDO_EN_MASK) != MT6797_CPULDO_EN_MASK)
		return -EIO;
	ret = mt6797_cpu_sram_read_bank(sram->regmap, words);
	return ret < 0 ? ret : MT6797_CPULDO_MIN_UV + ret * MT6797_CPULDO_STEP_UV;
}

static int mt6797_cpu_sram_owned_read(void *data)
{
	struct mt6797_cpu_sram *sram = data;
	int ret;

	mutex_lock(&sram->rail_lock);
	ret = mt6797_cpu_sram_owned_read_unlocked(data);
	mutex_unlock(&sram->rail_lock);
	return ret;
}

static int mt6797_cpu_sram_owned_write(void *data, int uv)
{
	struct mt6797_cpu_sram *sram = data;
	int ret;

	if (uv < MT6797_CPULDO_MIN_UV || uv > 1200000 ||
	    uv % MT6797_CPULDO_STEP_UV)
		return -EINVAL;
	mutex_lock(&sram->rail_lock);
	ret = mt6797_cpu_sram_owned_read_unlocked(sram);
	if (ret >= 0)
		ret = mt6797_cpu_sram_program(sram,
			(uv - MT6797_CPULDO_MIN_UV) / MT6797_CPULDO_STEP_UV);
	mutex_unlock(&sram->rail_lock);
	return ret;
}

static int mt6797_cpu_sram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct mt6797_cpu_sram *sram;
	struct regmap *regmap;
	struct mt6797_rail_io io = {
		.read = mt6797_cpu_sram_owned_read,
		.write = mt6797_cpu_sram_owned_write,
		.step_uv = MT6797_CPULDO_STEP_UV,
	};
	int ret;

	regmap = syscon_node_to_regmap(dev->parent->of_node);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to get parent infracfg regmap\n");

	sram = devm_kzalloc(dev, sizeof(*sram), GFP_KERNEL);
	if (!sram)
		return -ENOMEM;
	mutex_init(&sram->rail_lock);
	sram->regmap = regmap;
	sram->dev = dev;
	ret = mt6797_boot_barrier_supply(np, "mem-supply");
	if (ret < 0)
		return ret;
	sram->boot_barrier = ret;
	if (sram->boot_barrier) {
		io.data = sram;
		/* Group4 will adopt this preregistered, enabled endpoint. */
		return mt6797_provider_attach(dev, MT6797_SRAM, &io);
	}
	config.driver_data = sram;
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

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
#include "mt6797-cpu-sram-test.c"
#endif
