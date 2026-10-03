// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5714 combo PMIC fuel gauge.
 *
 * Copyright (c) 2024, map220v <map220v300@gmail.com>
 *
 * The measured quantities live in an indirectly addressed SRAM: write the
 * word address to RADDR, then read the value from RDATA. Register layout and
 * the fixed-point scaling come from the Samsung downstream driver; no public
 * datasheet is available.
 */
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>

#define SM5714_FG_REG_SRAM_RADDR	0x8c
#define SM5714_FG_REG_SRAM_RDATA	0x8d

#define SM5714_FG_ADDR_SRAM_SOC		0x00
#define SM5714_FG_ADDR_SRAM_VBAT	0x03
#define SM5714_FG_ADDR_SRAM_CURRENT	0x05
#define SM5714_FG_ADDR_SRAM_TEMPERATURE	0x07

struct sm5714_fg {
	struct power_supply *psy;
	struct i2c_client *i2c;
	int charge_full_design_uah;
};

static enum power_supply_property sm5714_fg_props[] = {
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
};

static int sm5714_fg_read_sram(struct sm5714_fg *drv, u8 addr)
{
	int ret;

	ret = i2c_smbus_write_word_data(drv->i2c, SM5714_FG_REG_SRAM_RADDR, addr);
	if (ret < 0)
		return ret;

	return i2c_smbus_read_word_data(drv->i2c, SM5714_FG_REG_SRAM_RDATA);
}

/* SRAM words are sign-magnitude: bit 15 is the sign. */
static int sm5714_fg_sign(int raw, int mag)
{
	return (raw & 0x8000) ? -mag : mag;
}

static int sm5714_fg_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct sm5714_fg *drv = power_supply_get_drvdata(psy);
	int raw;

	switch (psp) {
	case POWER_SUPPLY_PROP_TEMP:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_TEMPERATURE);
		if (raw < 0)
			return raw;
		/* tenths of a degree Celsius */
		val->intval = sm5714_fg_sign(raw, ((raw & 0x7fff) * 10 * 2989) >> 19);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_SOC);
		if (raw < 0)
			return raw;
		/* 8.8 fixed-point percent, clamped to 100% */
		val->intval = min((raw * 10) >> 8, 1000) / 10;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_VBAT);
		if (raw < 0)
			return raw;
		/* microvolts, 2700 mV offset */
		val->intval = (2700 + sm5714_fg_sign(raw, ((raw & 0x7fff) * 10) / 109)) * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_CURRENT);
		if (raw < 0)
			return raw;
		/* microamps, negative while discharging */
		val->intval = sm5714_fg_sign(raw, ((raw & 0x7fff) * 1000) / 2044) * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	/* no capacity learning on this gauge: full is the design capacity */
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (!drv->charge_full_design_uah)
			return -ENODATA;
		val->intval = drv->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		/*
		 * The gauge exposes no coulomb counter, so scale the design
		 * charge by the reported state of charge.
		 */
		if (!drv->charge_full_design_uah)
			return -ENODATA;
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_SOC);
		if (raw < 0)
			return raw;
		val->intval = drv->charge_full_design_uah / 100 *
			      (min((raw * 10) >> 8, 1000) / 10);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct power_supply_desc sm5714_fg_desc = {
	.name		= "sm5714-fg",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= sm5714_fg_props,
	.num_properties	= ARRAY_SIZE(sm5714_fg_props),
	.get_property	= sm5714_fg_get_property,
};

static int sm5714_fg_probe(struct i2c_client *i2c)
{
	struct power_supply_config fg_cfg = { };
	struct power_supply_battery_info *info;
	struct sm5714_fg *drv;
	int ret;

	drv = devm_kzalloc(&i2c->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	drv->i2c = i2c;

	fg_cfg.drv_data = drv;
	fg_cfg.fwnode = dev_fwnode(&i2c->dev);

	drv->psy = devm_power_supply_register(&i2c->dev, &sm5714_fg_desc, &fg_cfg);
	if (IS_ERR(drv->psy))
		return dev_err_probe(&i2c->dev, PTR_ERR(drv->psy),
				     "failed to register power supply\n");

	/*
	 * An optional "monitored-battery" node supplies the design capacity;
	 * without it the charge_* properties report -ENODATA.
	 */
	ret = power_supply_get_battery_info(drv->psy, &info);
	if (ret == -ENODEV || ret == -ENOENT)
		return 0;
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "failed to get battery info\n");

	if (info->charge_full_design_uah > 0)
		drv->charge_full_design_uah = info->charge_full_design_uah;
	power_supply_put_battery_info(drv->psy, info);

	return 0;
}

static const struct i2c_device_id sm5714_fg_id[] = {
	{ "sm5714-fg" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5714_fg_id);

static const struct of_device_id sm5714_fg_of_match[] = {
	{ .compatible = "siliconmitus,sm5714-fg" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5714_fg_of_match);

static struct i2c_driver sm5714_fg_driver = {
	.driver = {
		.name = "sm5714-fg",
		.of_match_table = sm5714_fg_of_match,
	},
	.probe = sm5714_fg_probe,
	.id_table = sm5714_fg_id,
};
module_i2c_driver(sm5714_fg_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5714 fuel gauge driver");
MODULE_AUTHOR("map220v <map220v300@gmail.com>");
MODULE_LICENSE("GPL");
