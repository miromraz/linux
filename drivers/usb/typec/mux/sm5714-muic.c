// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5714 MUIC USB data switch, used as a Type-C mode switch.
 *
 * The SM5714 MUIC contains the analog switch for the USB D+/D- data lines.
 * In automatic mode it runs BC1.2 charger detection and routes the data lines
 * itself, which works while the port acts as a USB device. When the port acts
 * as a host it leaves the data lines open, so the switch has to be forced onto
 * the USB path manually. The Type-C CC detection block drives this switch
 * through the connector graph: TYPEC_STATE_USB forces the USB path, any other
 * state returns to automatic so BC1.2 keeps working.
 *
 * Copyright (C) 2026 Miroslav Mráz <miroslav.mraz@techmania.cz>
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_mux.h>

#define SM5714_MUIC_REG_DEVICE_ID	0x00
#define SM5714_MUIC_DEVICE_ID_VENDOR	GENMASK(2, 0)
#define SM5714_MUIC_VENDOR_ID		0x01

#define SM5714_MUIC_REG_MANUAL_SW	0x06
#define SM5714_MUIC_MANUAL_SW_MANUAL	BIT(7)
#define SM5714_MUIC_MANUAL_SW_DM	GENMASK(5, 3)
#define SM5714_MUIC_MANUAL_SW_DP	GENMASK(2, 0)
/* D+/D- path selectors: 0 = open, 1 = USB */
#define SM5714_MUIC_PATH_OPEN		0x0
#define SM5714_MUIC_PATH_USB		0x1

/* Force both data lines onto the USB path (manual mode). */
#define SM5714_MUIC_MANUAL_SW_USB \
	(SM5714_MUIC_MANUAL_SW_MANUAL | \
	 FIELD_PREP(SM5714_MUIC_MANUAL_SW_DM, SM5714_MUIC_PATH_USB) | \
	 FIELD_PREP(SM5714_MUIC_MANUAL_SW_DP, SM5714_MUIC_PATH_USB))
/* Hand the data lines back to automatic BC1.2 routing. */
#define SM5714_MUIC_MANUAL_SW_AUTO	0x00

struct sm5714_muic {
	struct regmap *regmap;
	struct typec_mux_dev *mux;
};

static const struct regmap_config sm5714_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SM5714_MUIC_REG_MANUAL_SW,
};

static int sm5714_muic_mux_set(struct typec_mux_dev *mux,
			       struct typec_mux_state *state)
{
	struct sm5714_muic *muic = typec_mux_get_drvdata(mux);
	unsigned int manual_sw;

	switch (state->mode) {
	case TYPEC_STATE_USB:
		manual_sw = SM5714_MUIC_MANUAL_SW_USB;
		break;
	case TYPEC_STATE_SAFE:
		manual_sw = SM5714_MUIC_MANUAL_SW_AUTO;
		break;
	default:
		/* This switch only routes USB D+/D-; reject alternate modes. */
		if (state->alt)
			return -EOPNOTSUPP;
		manual_sw = SM5714_MUIC_MANUAL_SW_AUTO;
		break;
	}

	return regmap_write(muic->regmap, SM5714_MUIC_REG_MANUAL_SW, manual_sw);
}

static int sm5714_muic_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_mux_desc mux_desc = { };
	struct sm5714_muic *muic;
	unsigned int val;
	int ret;

	muic = devm_kzalloc(dev, sizeof(*muic), GFP_KERNEL);
	if (!muic)
		return -ENOMEM;

	muic->regmap = devm_regmap_init_i2c(client, &sm5714_muic_regmap_config);
	if (IS_ERR(muic->regmap))
		return dev_err_probe(dev, PTR_ERR(muic->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(muic->regmap, SM5714_MUIC_REG_DEVICE_ID, &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");

	if (FIELD_GET(SM5714_MUIC_DEVICE_ID_VENDOR, val) != SM5714_MUIC_VENDOR_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected device ID 0x%02x\n", val);

	/* Leave the switch in automatic mode so BC1.2 works until we go host. */
	ret = regmap_write(muic->regmap, SM5714_MUIC_REG_MANUAL_SW,
			   SM5714_MUIC_MANUAL_SW_AUTO);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reset data switch\n");

	mux_desc.drvdata = muic;
	mux_desc.fwnode = dev_fwnode(dev);
	mux_desc.set = sm5714_muic_mux_set;

	muic->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(muic->mux))
		return dev_err_probe(dev, PTR_ERR(muic->mux),
				     "failed to register typec mux\n");

	i2c_set_clientdata(client, muic);

	return 0;
}

static void sm5714_muic_remove(struct i2c_client *client)
{
	struct sm5714_muic *muic = i2c_get_clientdata(client);

	typec_mux_unregister(muic->mux);

	/* Restore automatic mode so BC1.2 charger detection keeps working. */
	regmap_write(muic->regmap, SM5714_MUIC_REG_MANUAL_SW,
		     SM5714_MUIC_MANUAL_SW_AUTO);
}

static const struct i2c_device_id sm5714_muic_table[] = {
	{ .name = "sm5714-muic" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5714_muic_table);

static const struct of_device_id sm5714_muic_of_table[] = {
	{ .compatible = "siliconmitus,sm5714-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5714_muic_of_table);

static struct i2c_driver sm5714_muic_driver = {
	.driver = {
		.name = "sm5714-muic",
		.of_match_table = sm5714_muic_of_table,
	},
	.probe		= sm5714_muic_probe,
	.remove		= sm5714_muic_remove,
	.id_table	= sm5714_muic_table,
};
module_i2c_driver(sm5714_muic_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5714 MUIC USB data switch driver");
MODULE_AUTHOR("Miroslav Mráz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
