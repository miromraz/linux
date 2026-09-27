// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, map220v <map220v300@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/of_gpio.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define SM5714_FG_REG_DEVICE_ID           0x00
#define SM5714_FG_REG_CTRL				  0x01
#define SM5714_FG_REG_INTFG               0x02
#define SM5714_FG_REG_STATUS              0x03
#define SM5714_FG_REG_INTFG_MASK          0x04

#define SM5714_FG_REG_SRAM_PROT		      0x8B
#define SM5714_FG_REG_SRAM_RADDR		  0x8C
#define SM5714_FG_REG_SRAM_RDATA		  0x8D
#define SM5714_FG_REG_SRAM_WADDR		  0x8E
#define SM5714_FG_REG_SRAM_WDATA		  0x8F

#define SM5714_FG_ADDR_SRAM_SOC			  0x00
#define SM5714_FG_ADDR_SRAM_OCV			  0x01
#define SM5714_FG_ADDR_SRAM_VBAT		  0x03
#define SM5714_FG_ADDR_SRAM_VSYS		  0x04
#define SM5714_FG_ADDR_SRAM_CURRENT		  0x05
#define SM5714_FG_ADDR_SRAM_TEMPERATURE	  0x07
#define SM5714_FG_ADDR_SRAM_VBAT_AVG	  0x08
#define SM5714_FG_ADDR_SRAM_CURRENT_AVG	  0x09
#define SM5714_FG_ADDR_SRAM_STATE         0x15

struct sm5714_fg {
	struct power_supply *psy;
	struct i2c_client *i2c;
};

static enum power_supply_property sm5714_fg_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static int sm5714_fg_get_status(int *status)
{
	union power_supply_propval val;
	struct power_supply *psy;
	int ret = -EINVAL;

	psy = power_supply_get_by_name("sm5714_charger");
	if (!psy)
		return ret;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS,
					&val);
	power_supply_put(psy);
	if (ret)
		return ret;

	*status = val.intval;

	return ret;
}

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
	case POWER_SUPPLY_PROP_STATUS:
		return sm5714_fg_get_status(&val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_TEMPERATURE);
		if (raw < 0)
			return raw;
		/* decidegrees C, scale from the vendor driver */
		val->intval = sm5714_fg_sign(raw, ((raw & 0x7fff) * 10 * 2989) >> 19);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_SOC);
		if (raw < 0)
			return raw;
		/* 8.8 fixed point percent */
		val->intval = min((raw * 10) >> 8, 1000) / 10;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_VBAT);
		if (raw < 0)
			return raw;
		/* mV = 2700 +/- raw * 10 / 109 */
		val->intval = (2700 + sm5714_fg_sign(raw, ((raw & 0x7fff) * 10) / 109)) * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		raw = sm5714_fg_read_sram(drv, SM5714_FG_ADDR_SRAM_CURRENT);
		if (raw < 0)
			return raw;
		/* mA = raw * 1000 / 2044, negative while discharging */
		val->intval = sm5714_fg_sign(raw, ((raw & 0x7fff) * 1000) / 2044) * 1000;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc sm5714_fg_desc = {
	.name			= "sm5714_fg",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= sm5714_fg_props,
	.num_properties		= ARRAY_SIZE(sm5714_fg_props),
	.get_property		= sm5714_fg_get_property,
};

static int sm5714_fg_probe(struct i2c_client *i2c)
{
	struct sm5714_fg *drv;

	struct power_supply *psy;
	struct power_supply_config fg_cfg = { };

	drv = devm_kzalloc(&i2c->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	drv->i2c = i2c;

	fg_cfg.drv_data = drv;
	fg_cfg.of_node = i2c->dev.of_node;

	psy = devm_power_supply_register(&i2c->dev, &sm5714_fg_desc,
							&fg_cfg);

	if (IS_ERR(psy)) {
		dev_err(&i2c->dev, "failed to register power supply\n");
		return PTR_ERR(psy);
	}

	drv->psy = psy;
	return 0;
}

static const struct i2c_device_id sm5714_i2c_ids[] = {
	{ "sm5714-fg", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, sm5714_i2c_ids);

static const struct of_device_id sm5714_of_match_table[] = {
	{ .compatible = "siliconmitus,sm5714-fg", },
	{ },
};
MODULE_DEVICE_TABLE(of, sm5714_of_match_table);

static struct i2c_driver sm5714_fg_driver = {
	.driver = {
		.name = "sm5714-fg",
		.of_match_table = sm5714_of_match_table,
	},
	.probe	= sm5714_fg_probe,
	.id_table   = sm5714_i2c_ids,
};

module_i2c_driver(sm5714_fg_driver);

MODULE_DESCRIPTION("Samsung SM5714-FG");
MODULE_AUTHOR("map220v <map220v300@gmail.com>");
MODULE_LICENSE("GPL");
