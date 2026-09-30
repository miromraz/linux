// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5714 Type-C CC detection driver.
 *
 * The SM5714 combo PMIC integrates a charger, a fuel gauge and a Type-C/USB
 * block. This driver handles the CC detection block: it registers a Type-C
 * port with the USB Type-C class, reports partner attach/detach, switches the
 * USB controller between device and host role through a USB role switch, drives
 * the mode mux (a companion MUIC) and powers VBUS while acting as a host.
 * USB Power Delivery is not handled.
 *
 * Register layout from the Samsung downstream sm5714_typec driver.
 */
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>

#define SM5714_REG_INT1			0x01
#define SM5714_REG_INT5			0x05
#define SM5714_REG_INT_MASK1		0x06
#define SM5714_REG_CC_STATUS		0x28
#define SM5714_REG_CC_CNTL1		0x29

#define SM5714_INT1_ATTACH		BIT(3)
#define SM5714_INT1_DETACH		BIT(4)

/*
 * Interrupt mask registers: a set bit masks its source. INT_MASK1..5 sit at
 * 0x06..0x0a. Enable only INT1 attach/detach; mask everything else, which is
 * unused here and otherwise storms the shared CC IRQ line.
 */
#define SM5714_INT_MASK1_DEFAULT	(0xff & ~(SM5714_INT1_ATTACH | SM5714_INT1_DETACH))
#define SM5714_INT_MASK_ALL		0xff

/* CC_STATUS: current attach state and cable orientation. */
#define SM5714_CC_ATTACH_TYPE		GENMASK(2, 0)
#define SM5714_CC_ATTACH_SOURCE		0x1	/* partner sources power: we are the sink/device */
#define SM5714_CC_ATTACH_SINK		0x2	/* partner sinks power: we are the host/source */
#define SM5714_CC_ADV_CURR		GENMASK(4, 3)	/* Rp advertised by partner, when we sink */
#define SM5714_CC_ADV_CURR_1_5A		BIT(3)
#define SM5714_CC_CABLE_FLIP		BIT(5)		/* set: CC2, cable reversed */

/*
 * CC_CNTL1 selects the port role. It is written as a whole byte (verified on
 * hardware: 0x41 attaches a DRP, 0x45 forces sink-only, 0x49 forces source).
 */
#define SM5714_CC_CNTL1_DRP		0x41
#define SM5714_CC_CNTL1_SNK		0x45
#define SM5714_CC_CNTL1_SRC		0x49

struct sm5714_cc {
	struct device *dev;
	struct regmap *regmap;
	struct usb_role_switch *role_sw;
	struct typec_port *port;
	struct typec_partner *partner;
	struct regulator *vbus;
	struct mutex lock;	/* serialises role, mux and vbus state */
	bool vbus_on;
	int irq;
};

static void sm5714_cc_set_vbus(struct sm5714_cc *cc, bool on)
{
	int ret;

	if (cc->vbus_on == on)
		return;

	ret = on ? regulator_enable(cc->vbus) : regulator_disable(cc->vbus);
	if (ret) {
		dev_err(cc->dev, "failed to switch VBUS %s: %d\n", on ? "on" : "off", ret);
		return;
	}
	cc->vbus_on = on;
}

/* Advertised Rp current, meaningful only while we are the sink. */
static enum typec_pwr_opmode sm5714_cc_pwr_opmode(unsigned int cc_status)
{
	switch (cc_status & SM5714_CC_ADV_CURR) {
	case 0:
		return TYPEC_PWR_MODE_USB;
	case SM5714_CC_ADV_CURR_1_5A:
		return TYPEC_PWR_MODE_1_5A;
	default:
		return TYPEC_PWR_MODE_3_0A;
	}
}

static void sm5714_cc_disconnect(struct sm5714_cc *cc)
{
	if (cc->partner) {
		typec_unregister_partner(cc->partner);
		cc->partner = NULL;
	}

	sm5714_cc_set_vbus(cc, false);
	typec_set_mode(cc->port, TYPEC_STATE_SAFE);
	typec_set_orientation(cc->port, TYPEC_ORIENTATION_NONE);
	typec_set_pwr_opmode(cc->port, TYPEC_PWR_MODE_USB);
	typec_set_pwr_role(cc->port, TYPEC_SINK);
	typec_set_data_role(cc->port, TYPEC_DEVICE);

	/*
	 * Fall back to device role rather than USB_ROLE_NONE: with nothing on
	 * CC the board is normally cabled to a host/charger, and keeping the
	 * gadget enabled preserves the USB-networking debug link.
	 */
	usb_role_switch_set_role(cc->role_sw, USB_ROLE_DEVICE);
}

static void sm5714_cc_attach(struct sm5714_cc *cc, unsigned int cc_status)
{
	struct typec_partner_desc desc = { .accessory = TYPEC_ACCESSORY_NONE };
	bool host = (cc_status & SM5714_CC_ATTACH_TYPE) == SM5714_CC_ATTACH_SINK;

	if (cc->partner) {
		typec_unregister_partner(cc->partner);
		cc->partner = NULL;
	}

	typec_set_orientation(cc->port, (cc_status & SM5714_CC_CABLE_FLIP) ?
			      TYPEC_ORIENTATION_REVERSE : TYPEC_ORIENTATION_NORMAL);

	cc->partner = typec_register_partner(cc->port, &desc);
	if (IS_ERR(cc->partner)) {
		dev_err(cc->dev, "failed to register partner: %pe\n", cc->partner);
		cc->partner = NULL;
	}

	if (host) {
		typec_set_pwr_role(cc->port, TYPEC_SOURCE);
		typec_set_data_role(cc->port, TYPEC_HOST);
		typec_set_pwr_opmode(cc->port, TYPEC_PWR_MODE_USB);
		/* Route D+/D- to the host controller; MUIC leaves BC1.2 mode. */
		typec_set_mode(cc->port, TYPEC_STATE_USB);
		usb_role_switch_set_role(cc->role_sw, USB_ROLE_HOST);
		sm5714_cc_set_vbus(cc, true);
	} else {
		typec_set_pwr_role(cc->port, TYPEC_SINK);
		typec_set_data_role(cc->port, TYPEC_DEVICE);
		typec_set_pwr_opmode(cc->port, sm5714_cc_pwr_opmode(cc_status));
		/*
		 * Leave the mux in its safe state so the MUIC keeps its
		 * automatic BC1.2 handling, which routes D+/D- for device mode.
		 */
		typec_set_mode(cc->port, TYPEC_STATE_SAFE);
		usb_role_switch_set_role(cc->role_sw, USB_ROLE_DEVICE);
	}
}

static void sm5714_cc_update(struct sm5714_cc *cc)
{
	unsigned int st, type;

	mutex_lock(&cc->lock);

	if (regmap_read(cc->regmap, SM5714_REG_CC_STATUS, &st)) {
		mutex_unlock(&cc->lock);
		return;
	}

	type = st & SM5714_CC_ATTACH_TYPE;
	dev_dbg(cc->dev, "CC_STATUS 0x%02x (attach type %u)\n", st, type);

	if (type == SM5714_CC_ATTACH_SOURCE || type == SM5714_CC_ATTACH_SINK)
		sm5714_cc_attach(cc, st);
	else
		sm5714_cc_disconnect(cc);

	mutex_unlock(&cc->lock);
}

static irqreturn_t sm5714_cc_irq(int irq, void *data)
{
	struct sm5714_cc *cc = data;
	unsigned int int1, dummy;
	int i;

	/* interrupt registers clear on read */
	if (regmap_read(cc->regmap, SM5714_REG_INT1, &int1))
		return IRQ_NONE;
	for (i = SM5714_REG_INT1 + 1; i <= SM5714_REG_INT5; i++)
		regmap_read(cc->regmap, i, &dummy);

	if (int1 & (SM5714_INT1_ATTACH | SM5714_INT1_DETACH))
		sm5714_cc_update(cc);

	return IRQ_HANDLED;
}

static int sm5714_cc_port_type_set(struct typec_port *port, enum typec_port_type type)
{
	struct sm5714_cc *cc = typec_get_drvdata(port);
	unsigned int val;

	switch (type) {
	case TYPEC_PORT_SRC:
		val = SM5714_CC_CNTL1_SRC;
		break;
	case TYPEC_PORT_SNK:
		val = SM5714_CC_CNTL1_SNK;
		break;
	case TYPEC_PORT_DRP:
		val = SM5714_CC_CNTL1_DRP;
		break;
	default:
		return -EINVAL;
	}

	return regmap_write(cc->regmap, SM5714_REG_CC_CNTL1, val);
}

static const struct typec_operations sm5714_cc_ops = {
	.port_type_set = sm5714_cc_port_type_set,
};

static const struct regmap_config sm5714_cc_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int sm5714_cc_hw_init(struct sm5714_cc *cc)
{
	unsigned int dummy;
	int ret, i;

	ret = regmap_write(cc->regmap, SM5714_REG_CC_CNTL1, SM5714_CC_CNTL1_DRP);
	if (ret)
		return ret;

	/* drop stale interrupts, then enable only attach/detach */
	for (i = SM5714_REG_INT1; i <= SM5714_REG_INT5; i++)
		regmap_read(cc->regmap, i, &dummy);

	ret = regmap_write(cc->regmap, SM5714_REG_INT_MASK1, SM5714_INT_MASK1_DEFAULT);
	if (ret)
		return ret;
	for (i = SM5714_REG_INT_MASK1 + 1; i <= SM5714_REG_INT_MASK1 + 4; i++) {
		ret = regmap_write(cc->regmap, i, SM5714_INT_MASK_ALL);
		if (ret)
			return ret;
	}

	return 0;
}

static int sm5714_cc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_capability cap = { };
	struct fwnode_handle *connector;
	struct sm5714_cc *cc;
	int ret;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;
	cc->dev = dev;
	cc->irq = client->irq;
	mutex_init(&cc->lock);
	i2c_set_clientdata(client, cc);

	cc->regmap = devm_regmap_init_i2c(client, &sm5714_cc_regmap);
	if (IS_ERR(cc->regmap))
		return PTR_ERR(cc->regmap);

	cc->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(cc->vbus))
		return dev_err_probe(dev, PTR_ERR(cc->vbus), "no VBUS regulator\n");

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return dev_err_probe(dev, -ENODEV, "no connector node\n");

	cc->role_sw = fwnode_usb_role_switch_get(connector);
	if (IS_ERR(cc->role_sw)) {
		ret = dev_err_probe(dev, PTR_ERR(cc->role_sw), "no USB role switch\n");
		goto err_put_fwnode;
	}

	ret = sm5714_cc_hw_init(cc);
	if (ret) {
		dev_err_probe(dev, ret, "failed to initialise CC block\n");
		goto err_put_role;
	}

	cap.type = TYPEC_PORT_DRP;
	cap.data = TYPEC_PORT_DRD;
	cap.prefer_role = TYPEC_SINK;
	cap.orientation_aware = true;
	cap.driver_data = cc;
	cap.ops = &sm5714_cc_ops;
	cap.fwnode = connector;

	/* Acquires the mode mux from the connector graph; may defer. */
	cc->port = typec_register_port(dev, &cap);
	if (IS_ERR(cc->port)) {
		ret = dev_err_probe(dev, PTR_ERR(cc->port), "failed to register port\n");
		goto err_put_role;
	}

	sm5714_cc_update(cc);

	ret = request_threaded_irq(cc->irq, NULL, sm5714_cc_irq, IRQF_ONESHOT,
				   "sm5714-cc", cc);
	if (ret) {
		dev_err_probe(dev, ret, "failed to request IRQ\n");
		goto err_unreg_port;
	}

	fwnode_handle_put(connector);
	return 0;

err_unreg_port:
	typec_unregister_port(cc->port);
err_put_role:
	usb_role_switch_put(cc->role_sw);
err_put_fwnode:
	fwnode_handle_put(connector);
	return ret;
}

static void sm5714_cc_remove(struct i2c_client *client)
{
	struct sm5714_cc *cc = i2c_get_clientdata(client);

	free_irq(cc->irq, cc);
	if (cc->partner)
		typec_unregister_partner(cc->partner);
	sm5714_cc_set_vbus(cc, false);
	typec_unregister_port(cc->port);
	usb_role_switch_put(cc->role_sw);
}

/*
 * The CC IRQ is a level line on the TLMM (not a wake source here). During
 * system resume it can fire before the geni I2C controller it hangs off is
 * runtime-resumed; the handler's I2C read then fails with -EACCES, the level
 * stays asserted and re-fires thousands of times ("error turning SE
 * resources:-13" storm). Mask it across sleep: the I2C adapter (our parent)
 * resumes before this client's resume callback, so re-enabling here is safe,
 * and any attach/detach that happened while masked is latched and serviced
 * on the first IRQ after enable.
 */
static int sm5714_cc_suspend(struct device *dev)
{
	struct sm5714_cc *cc = dev_get_drvdata(dev);

	disable_irq(cc->irq);
	return 0;
}

static int sm5714_cc_resume(struct device *dev)
{
	struct sm5714_cc *cc = dev_get_drvdata(dev);

	enable_irq(cc->irq);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(sm5714_cc_pm_ops, sm5714_cc_suspend, sm5714_cc_resume);

static const struct of_device_id sm5714_cc_of_match[] = {
	{ .compatible = "siliconmitus,sm5714-cc" },
	{}
};
MODULE_DEVICE_TABLE(of, sm5714_cc_of_match);

static struct i2c_driver sm5714_cc_driver = {
	.driver = {
		.name = "sm5714-cc",
		.of_match_table = sm5714_cc_of_match,
		.pm = pm_sleep_ptr(&sm5714_cc_pm_ops),
	},
	.probe = sm5714_cc_probe,
	.remove = sm5714_cc_remove,
};
module_i2c_driver(sm5714_cc_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5714 Type-C CC detection");
MODULE_LICENSE("GPL");
