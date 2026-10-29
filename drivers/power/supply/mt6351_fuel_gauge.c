// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fuel gauge for the MediaTek MT6351 PMIC.
 *
 * The MT6351 integrates a hardware fuel-gauge ADC (FGADC) that measures the
 * battery current through an external sense resistor. This driver reads the
 * instantaneous FGADC current together with the battery voltage from the PMIC
 * AUXADC and feeds both to the adc-battery-helper, which derives the
 * open-circuit voltage, capacity and charge status from the battery's
 * ocv-capacity-table and internal resistance.
 *
 * On power-path designs such as the Gemini PDA (BQ25896 between the battery
 * and the system) the battery voltage is the AUXADC ISENSE channel, which is
 * the channel the vendor fuel-gauge and OCV profile use
 * (battery_meter_hal.c:1114 read_adc_v_bat_sense() under SWCHR_POWER_PATH).
 *
 * The FGADC register map and current conversion are taken from the MT6797
 * vendor kernel drivers/misc/mediatek/power/mt6797/battery_meter_hal.c.
 *
 * Copyright (c) 2026 Miroslav Mraz
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "adc-battery-helper.h"

#define MT6351_FGADC_CON0		0x0ca4
#define   FG_ON				BIT(0)
#define   FG_CAL			GENMASK(3, 2)
#define   FG_AUTOCALRATE		GENMASK(6, 4)
#define   FG_SW_READ_PRE		BIT(9)
#define   FG_LATCHDATA_ST		BIT(10)
#define   FG_SW_CLEAR			BIT(11)
#define   FG_CON0_CTRL			GENMASK(15, 8)
#define MT6351_FG_CURRENT_OUT		0x0cba
#define MT6351_FGADC_CON15		0x0cc2
#define   FG_OSR			GENMASK(3, 0)
#define MT6351_TOP_CKPDN_CON2		0x0246
#define   FGADC_ANA_CK_PDN		BIT(2)
#define   FGADC_DIG_CK_PDN		BIT(3)

/* Current-mode, auto-calibration rate 2 (battery_meter_hal.c:336). */
#define FG_INIT_CAL	(FIELD_PREP(FG_CAL, 2) | FIELD_PREP(FG_AUTOCALRATE, 2))

/*
 * FG_CURRENT_OUT LSB is 158.122 uA referenced to a 20 mohm shunt
 * (battery_meter_hal.c:36,495). For a sense resistor of R micro-ohms the
 * current in micro-amps is therefore
 *   raw * 158.122uA * 20000uohm / R = raw * (158122 * 20) / R.
 * The board's effective shunt folds R_FG (10 mohm) and the vendor CAR_TUNE
 * gain (115/100) together: 10 mohm / 1.15 = 8696 uohm.
 */
#define MT6351_FG_CUR_NUM		(158122 * 20)

/* Latch handshake must complete well within this (battery_meter_hal.c:420). */
#define FG_LATCH_TIMEOUT_US		50000
#define FG_LATCH_POLL_US		1000

struct mt6351_fg {
	/* Must be the first member, see adc-battery-helper.h. */
	struct adc_battery_helper helper;
	struct regmap *regmap;
	struct iio_channel *vbat;
	struct power_supply *psy;
	u32 shunt_uohm;
};

static int mt6351_fg_read_current(struct mt6351_fg *fg, int *curr_ua)
{
	unsigned int con0, raw;
	int ret, clear_ret, idle_ret;
	int sample;

	/* Request a fresh latch of the current register. */
	ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0, FG_CON0_CTRL,
				 FG_SW_READ_PRE);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(fg->regmap, MT6351_FGADC_CON0, con0,
				       con0 & FG_LATCHDATA_ST,
				       FG_LATCH_POLL_US, FG_LATCH_TIMEOUT_US);
	if (!ret)
		ret = regmap_read(fg->regmap, MT6351_FG_CURRENT_OUT, &raw);

	/*
	 * Release the latch and return the control byte to idle regardless of
	 * the read result. If the clear does not complete the next read would
	 * return a stale value, so report that as an error too.
	 */
	clear_ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				       FG_CON0_CTRL, FG_SW_CLEAR);
	if (!clear_ret)
		clear_ret = regmap_read_poll_timeout(fg->regmap, MT6351_FGADC_CON0, con0,
						     !(con0 & FG_LATCHDATA_ST),
						     FG_LATCH_POLL_US, FG_LATCH_TIMEOUT_US);
	idle_ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
				      FG_CON0_CTRL, 0);

	if (ret)
		return ret;
	if (clear_ret)
		return clear_ret;

	if (idle_ret)
		return idle_ret;

	/*
	 * Vendor battery_meter_hal.c subtracts 65535 for negative samples:
	 * one's complement, including negative zero, not two's complement.
	 * Positive is charging. This changes encoding, not calibration.
	 */
	sample = raw & 0xffff;
	if (sample > 0x7fff)
		sample -= 0xffff;
	*curr_ua = div_s64((s64)sample * MT6351_FG_CUR_NUM,
			   fg->shunt_uohm);
	return 0;
}

static int mt6351_fg_get_voltage_and_current_now(struct power_supply *psy,
						 int *volt_uv, int *curr_ua)
{
	struct mt6351_fg *fg = power_supply_get_drvdata(psy);
	int ret, mv;

	/* ISENSE is a 3:1 divided voltage; the IIO processed value is in mV. */
	ret = iio_read_channel_processed(fg->vbat, &mv);
	if (ret < 0)
		return ret;
	*volt_uv = mv * 1000;

	return mt6351_fg_read_current(fg, curr_ua);
}

static int mt6351_fg_hw_init(struct mt6351_fg *fg)
{
	unsigned int con0;
	int ret;

	ret = regmap_read(fg->regmap, MT6351_FGADC_CON0, &con0);
	if (ret)
		return ret;

	/* The bootloader normally starts the gauge; only init if it did not. */
	if (!(con0 & FG_ON)) {
		/* Ungate the FGADC analog and digital clocks. */
		ret = regmap_update_bits(fg->regmap, MT6351_TOP_CKPDN_CON2,
					 FGADC_ANA_CK_PDN | FGADC_DIG_CK_PDN, 0);
		if (ret)
			return ret;

		/* Set current/auto-calibration mode, then enable the gauge. */
		ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0,
					 FG_CAL | FG_AUTOCALRATE | FG_ON, FG_INIT_CAL);
		if (ret)
			return ret;
		ret = regmap_update_bits(fg->regmap, MT6351_FGADC_CON0, FG_ON, FG_ON);
		if (ret)
			return ret;
	}

	/*
	 * The current LSB assumes an oversample rate of 8; enforce it whether
	 * or not the bootloader already started the gauge.
	 */
	return regmap_update_bits(fg->regmap, MT6351_FGADC_CON15, FG_OSR,
				  FIELD_PREP(FG_OSR, 8));
}

static const struct power_supply_desc mt6351_fg_psy_desc = {
	.name			= "mt6351-fuel-gauge",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.get_property		= adc_battery_helper_get_property,
	.external_power_changed	= adc_battery_helper_external_power_changed,
	.properties		= adc_battery_helper_properties,
	.num_properties		= ADC_HELPER_NUM_PROPERTIES,
};

static int mt6351_fg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *mt6397 = dev_get_drvdata(dev->parent);
	struct power_supply_config psy_cfg = {};
	struct mt6351_fg *fg;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->regmap = mt6397->regmap;

	ret = device_property_read_u32(dev, "shunt-resistor-micro-ohms",
				       &fg->shunt_uohm);
	if (ret || !fg->shunt_uohm)
		return dev_err_probe(dev, -EINVAL,
				     "missing/invalid shunt-resistor-micro-ohms\n");

	fg->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(fg->vbat))
		return dev_err_probe(dev, PTR_ERR(fg->vbat), "getting vbat channel\n");

	ret = mt6351_fg_hw_init(fg);
	if (ret)
		return dev_err_probe(dev, ret, "initialising FGADC\n");

	platform_set_drvdata(pdev, fg);

	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &mt6351_fg_psy_desc, &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy),
				     "registering power supply\n");

	return adc_battery_helper_init(&fg->helper, fg->psy,
				       mt6351_fg_get_voltage_and_current_now, NULL);
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6351_fg_pm_ops, adc_battery_helper_suspend,
				adc_battery_helper_resume);

static const struct of_device_id mt6351_fg_of_match[] = {
	{ .compatible = "mediatek,mt6351-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_fg_of_match);

static struct platform_driver mt6351_fg_driver = {
	.driver = {
		.name = "mt6351-fuel-gauge",
		.of_match_table = mt6351_fg_of_match,
		.pm = pm_sleep_ptr(&mt6351_fg_pm_ops),
	},
	.probe = mt6351_fg_probe,
};
module_platform_driver(mt6351_fg_driver);

MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_DESCRIPTION("MediaTek MT6351 PMIC fuel gauge");
MODULE_LICENSE("GPL");
