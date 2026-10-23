// SPDX-License-Identifier: GPL-2.0-only
//
// onsemi FUSB301/FUSB301A autonomous USB Type-C controller

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>

#define FUSB301_REG_DEVICE_ID	0x01
#define FUSB301_REG_MODE	0x02
#define FUSB301_REG_CONTROL	0x03
#define FUSB301_REG_MASK	0x10
#define FUSB301_REG_STATUS	0x11
#define FUSB301_REG_TYPE	0x12
#define FUSB301_REG_INTERRUPT	0x13

#define FUSB301_DEVICE_ID	0x12

#define FUSB301_MODE_SOURCE	BIT(0)
#define FUSB301_MODE_SOURCE_ACC	BIT(1)
#define FUSB301_MODE_SINK	BIT(2)
#define FUSB301_MODE_SINK_ACC	BIT(3)
#define FUSB301_MODE_DRP	BIT(4)
#define FUSB301_MODE_DRP_ACC	BIT(5)
#define FUSB301_MODE_MASK	GENMASK(5, 0)

#define FUSB301_CONTROL_HOST_CUR	GENMASK(2, 1)
#define FUSB301_CONTROL_INT_MASK	BIT(0)
#define FUSB301_HOST_CUR_DEFAULT	BIT(1)
#define FUSB301_HOST_CUR_1_5A	BIT(2)
#define FUSB301_HOST_CUR_3A	GENMASK(2, 1)

#define FUSB301_STATUS_ORIENT	GENMASK(5, 4)
#define FUSB301_STATUS_VBUSOK	BIT(3)
#define FUSB301_STATUS_BC_LVL	GENMASK(2, 1)
#define FUSB301_STATUS_ATTACH	BIT(0)
#define FUSB301_ORIENT_CC1	BIT(4)
#define FUSB301_ORIENT_CC2	BIT(5)
#define FUSB301_BC_DEFAULT	BIT(1)
#define FUSB301_BC_1_5A	BIT(2)
#define FUSB301_BC_3A	GENMASK(2, 1)

#define FUSB301_TYPE_SINK	BIT(4)
#define FUSB301_TYPE_SOURCE	BIT(3)

/*
 * Poll period used when the controller's interrupt line is not routed. A
 * sink only needs to notice a charger within human reaction time, and VBUS
 * itself is handled by the charger independently of the Type-C class, so a
 * relaxed period keeps the wakeup rate low.
 */
#define FUSB301_POLL_INTERVAL_MS	500

struct fusb301 {
	struct device *dev;
	struct regmap *regmap;
	struct typec_capability cap;
	struct typec_port *port;
	struct typec_partner *partner;
	struct fwnode_handle *connector;
	struct usb_role_switch *role_sw;
	struct regulator *vbus;
	bool vbus_on;
	int irq;
	struct delayed_work poll_work;
	enum typec_pwr_opmode pwr_opmode;
	unsigned int partner_type;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	unsigned int sink_current_ua;
};

/*
 * As a Type-C source (DFP, Attached.SRC) the FUSB301 does not switch VBUS
 * itself; the board gates it with an external supply. Enable that supply only
 * while a sink partner is attached and disable it again otherwise.
 */
static void fusb301_vbus_set(struct fusb301 *fusb, bool on)
{
	int ret;

	if (!fusb->vbus || fusb->vbus_on == on)
		return;

	if (on)
		ret = regulator_enable(fusb->vbus);
	else
		ret = regulator_disable(fusb->vbus);
	if (ret) {
		dev_warn(fusb->dev, "failed to %s VBUS: %d\n",
			 on ? "enable" : "disable", ret);
		return;
	}

	fusb->vbus_on = on;
}

static unsigned int fusb301_mode_for_port_type(enum typec_port_type type)
{
	switch (type) {
	case TYPEC_PORT_SRC:
		return FUSB301_MODE_SOURCE;
	case TYPEC_PORT_SNK:
		return FUSB301_MODE_SINK;
	case TYPEC_PORT_DRP:
	default:
		return FUSB301_MODE_DRP;
	}
}

static enum typec_role fusb301_default_role(struct fusb301 *fusb)
{
	switch (fusb->cap.type) {
	case TYPEC_PORT_SRC:
		return TYPEC_SOURCE;
	case TYPEC_PORT_SNK:
		return TYPEC_SINK;
	case TYPEC_PORT_DRP:
	default:
		return fusb->cap.prefer_role == TYPEC_SOURCE ?
			TYPEC_SOURCE : TYPEC_SINK;
	}
}

static unsigned int fusb301_host_current(enum typec_pwr_opmode mode)
{
	switch (mode) {
	case TYPEC_PWR_MODE_1_5A:
		return FUSB301_HOST_CUR_1_5A;
	case TYPEC_PWR_MODE_3_0A:
		return FUSB301_HOST_CUR_3A;
	case TYPEC_PWR_MODE_USB:
	default:
		return FUSB301_HOST_CUR_DEFAULT;
	}
}

static enum typec_orientation fusb301_orientation(unsigned int status)
{
	switch (status & FUSB301_STATUS_ORIENT) {
	case FUSB301_ORIENT_CC1:
		return TYPEC_ORIENTATION_NORMAL;
	case FUSB301_ORIENT_CC2:
		return TYPEC_ORIENTATION_REVERSE;
	default:
		return TYPEC_ORIENTATION_NONE;
	}
}

static enum typec_pwr_opmode fusb301_sink_opmode(unsigned int status)
{
	switch (status & FUSB301_STATUS_BC_LVL) {
	case FUSB301_BC_1_5A:
		return TYPEC_PWR_MODE_1_5A;
	case FUSB301_BC_3A:
		return TYPEC_PWR_MODE_3_0A;
	case FUSB301_BC_DEFAULT:
	default:
		return TYPEC_PWR_MODE_USB;
	}
}

/* Current a source advertises through its Rp resistor, in microamperes. */
static unsigned int fusb301_sink_current_ua(enum typec_pwr_opmode mode)
{
	switch (mode) {
	case TYPEC_PWR_MODE_1_5A:
		return 1500000;
	case TYPEC_PWR_MODE_3_0A:
		return 3000000;
	case TYPEC_PWR_MODE_USB:
	default:
		return 500000;
	}
}

/*
 * Publish the Type-C current the attached source advertises so a charger
 * wired as a supplicant (power-supplies phandle) can raise its input current
 * limit. Zero means no source we draw from is attached.
 */
static void fusb301_set_sink_current(struct fusb301 *fusb, unsigned int ua)
{
	if (fusb->sink_current_ua == ua)
		return;

	fusb->sink_current_ua = ua;
	power_supply_changed(fusb->psy);
}

static int fusb301_psy_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct fusb301 *fusb = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = fusb->sink_current_ua > 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = fusb->sink_current_ua;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property fusb301_psy_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int fusb301_set_port_type(struct typec_port *port,
				  enum typec_port_type type)
{
	struct fusb301 *fusb = typec_get_drvdata(port);
	int ret;

	ret = regmap_update_bits(fusb->regmap, FUSB301_REG_MODE,
				 FUSB301_MODE_MASK,
				 fusb301_mode_for_port_type(type));
	if (ret)
		return ret;

	fusb->cap.type = type;
	return 0;
}

static const struct typec_operations fusb301_typec_ops = {
	.port_type_set = fusb301_set_port_type,
};

static void fusb301_unregister_partner(struct fusb301 *fusb)
{
	if (!fusb->partner)
		return;

	typec_unregister_partner(fusb->partner);
	fusb->partner = NULL;
}

static int fusb301_update_status(struct fusb301 *fusb)
{
	struct typec_partner_desc desc = { };
	unsigned int status, type, partner_type;
	enum typec_role pwr_role;
	enum typec_data_role data_role;
	int ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_STATUS, &status);
	if (ret)
		return ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_TYPE, &type);
	if (ret)
		return ret;

	if (!(status & FUSB301_STATUS_ATTACH)) {
		fusb301_vbus_set(fusb, false);
		fusb301_unregister_partner(fusb);
		fusb->partner_type = 0;
		fusb301_set_sink_current(fusb, 0);
		pwr_role = fusb301_default_role(fusb);
		data_role = pwr_role == TYPEC_SOURCE ? TYPEC_HOST : TYPEC_DEVICE;
		usb_role_switch_set_role(fusb->role_sw, USB_ROLE_NONE);
		typec_set_data_role(fusb->port, data_role);
		typec_set_pwr_role(fusb->port, pwr_role);
		typec_set_vconn_role(fusb->port, pwr_role);
		typec_set_pwr_opmode(fusb->port, fusb->pwr_opmode);
		typec_set_orientation(fusb->port, TYPEC_ORIENTATION_NONE);
		return 0;
	}

	partner_type = type & (FUSB301_TYPE_SINK | FUSB301_TYPE_SOURCE);
	if (!partner_type)
		return -EIO;

	if (partner_type & FUSB301_TYPE_SINK)
		pwr_role = TYPEC_SOURCE;
	else
		pwr_role = TYPEC_SINK;

	/* Source VBUS only for a sink partner, never for an accessory. */
	fusb301_vbus_set(fusb, partner_type & FUSB301_TYPE_SINK);

	if (partner_type != fusb->partner_type) {
		fusb301_unregister_partner(fusb);
		fusb->partner = typec_register_partner(fusb->port, &desc);
		if (IS_ERR(fusb->partner)) {
			ret = PTR_ERR(fusb->partner);
			fusb->partner = NULL;
			return ret;
		}

		data_role = pwr_role == TYPEC_SOURCE ? TYPEC_HOST : TYPEC_DEVICE;
		usb_role_switch_set_role(fusb->role_sw,
					 pwr_role == TYPEC_SOURCE ?
					 USB_ROLE_HOST : USB_ROLE_DEVICE);
		typec_set_pwr_role(fusb->port, pwr_role);
		typec_set_vconn_role(fusb->port, pwr_role);
		typec_set_data_role(fusb->port, data_role);
	}

	if (pwr_role == TYPEC_SINK) {
		enum typec_pwr_opmode opmode = fusb301_sink_opmode(status);

		typec_set_pwr_opmode(fusb->port, opmode);
		fusb301_set_sink_current(fusb, fusb301_sink_current_ua(opmode));
	} else {
		typec_set_pwr_opmode(fusb->port, fusb->pwr_opmode);
		fusb301_set_sink_current(fusb, 0);
	}

	typec_set_orientation(fusb->port, fusb301_orientation(status));
	fusb->partner_type = partner_type;

	return 0;
}

static irqreturn_t fusb301_irq(int irq, void *data)
{
	struct fusb301 *fusb = data;
	unsigned int interrupt;
	int ret;

	ret = regmap_read(fusb->regmap, FUSB301_REG_INTERRUPT, &interrupt);
	if (ret)
		return IRQ_NONE;

	ret = fusb301_update_status(fusb);
	if (ret)
		dev_warn(fusb->dev, "failed to update Type-C status: %d\n", ret);

	return IRQ_HANDLED;
}

/*
 * Boards that do not route the active-low interrupt poll the controller:
 * fusb301_update_status() only touches the port state on an actual change,
 * so it is safe to run it periodically.
 */
static void fusb301_poll_work(struct work_struct *work)
{
	struct fusb301 *fusb = container_of(work, struct fusb301, poll_work.work);
	int ret;

	ret = fusb301_update_status(fusb);
	if (ret)
		dev_warn_ratelimited(fusb->dev,
				     "failed to update Type-C status: %d\n", ret);

	queue_delayed_work(system_freezable_power_efficient_wq, &fusb->poll_work,
			   msecs_to_jiffies(FUSB301_POLL_INTERVAL_MS));
}

static const struct regmap_config fusb301_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FUSB301_REG_INTERRUPT,
};

static int fusb301_hw_init(struct fusb301 *fusb)
{
	unsigned int control;
	int ret;

	ret = regmap_write(fusb->regmap, FUSB301_REG_MODE,
			   fusb301_mode_for_port_type(fusb->cap.type));
	if (ret)
		return ret;

	/*
	 * In polling mode (no interrupt line) keep the global interrupt mask
	 * set so the INT pin is never asserted; otherwise an unrouted or
	 * shared INT line would be left stuck low with nothing to clear it.
	 */
	control = fusb301_host_current(fusb->pwr_opmode);
	if (fusb->irq <= 0)
		control |= FUSB301_CONTROL_INT_MASK;

	ret = regmap_update_bits(fusb->regmap, FUSB301_REG_CONTROL,
				 FUSB301_CONTROL_HOST_CUR |
				 FUSB301_CONTROL_INT_MASK, control);
	if (ret)
		return ret;

	return regmap_write(fusb->regmap, FUSB301_REG_MASK, 0);
}

static int fusb301_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = { };
	struct device *dev = &client->dev;
	struct fusb301 *fusb;
	const char *opmode;
	unsigned int id;
	int ret;

	fusb = devm_kzalloc(dev, sizeof(*fusb), GFP_KERNEL);
	if (!fusb)
		return -ENOMEM;

	fusb->dev = dev;
	fusb->regmap = devm_regmap_init_i2c(client, &fusb301_regmap_config);
	if (IS_ERR(fusb->regmap))
		return dev_err_probe(dev, PTR_ERR(fusb->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(fusb->regmap, FUSB301_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");
	if (id != FUSB301_DEVICE_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected device ID 0x%02x\n", id);

	fusb->connector = device_get_named_child_node(dev, "connector");
	if (!fusb->connector)
		return dev_err_probe(dev, -ENODEV, "missing connector node\n");

	ret = typec_get_fw_cap(&fusb->cap, fusb->connector);
	if (ret)
		goto err_put_connector;

	ret = fwnode_property_read_string(fusb->connector,
					  "typec-power-opmode", &opmode);
	if (ret)
		goto err_put_connector;

	ret = typec_find_pwr_opmode(opmode);
	if (ret < 0 || ret == TYPEC_PWR_MODE_PD) {
		ret = -EINVAL;
		goto err_put_connector;
	}
	fusb->pwr_opmode = ret;
	fusb->irq = client->irq;

	fusb->vbus = devm_of_regulator_get_optional(dev,
						    to_of_node(fusb->connector),
						    "vbus");
	if (IS_ERR(fusb->vbus)) {
		ret = PTR_ERR(fusb->vbus);
		if (ret != -ENODEV) {
			dev_err_probe(dev, ret, "failed to get VBUS supply\n");
			goto err_put_connector;
		}
		fusb->vbus = NULL;
	}

	/*
	 * The USB controller behind the connector learns its host/device role
	 * from this optional role switch. Absence is fine (e.g. a host-only
	 * port); -EPROBE_DEFER means the switch is not ready yet.
	 */
	fusb->role_sw = fwnode_usb_role_switch_get(fusb->connector);
	if (IS_ERR(fusb->role_sw)) {
		ret = dev_err_probe(dev, PTR_ERR(fusb->role_sw),
				    "failed to get USB role switch\n");
		goto err_put_connector;
	}

	fusb->cap.revision = USB_TYPEC_REV_1_1;
	fusb->cap.orientation_aware = true;
	fusb->cap.driver_data = fusb;
	fusb->cap.ops = &fusb301_typec_ops;

	ret = fusb301_hw_init(fusb);
	if (ret)
		goto err_put_role_sw;

	fusb->port = typec_register_port(dev, &fusb->cap);
	if (IS_ERR(fusb->port)) {
		ret = PTR_ERR(fusb->port);
		goto err_put_role_sw;
	}

	fusb->psy_desc.name = devm_kasprintf(dev, GFP_KERNEL, "fusb301-%s",
					     dev_name(dev));
	if (!fusb->psy_desc.name) {
		ret = -ENOMEM;
		goto err_unregister_port;
	}
	fusb->psy_desc.type = POWER_SUPPLY_TYPE_USB;
	fusb->psy_desc.properties = fusb301_psy_props;
	fusb->psy_desc.num_properties = ARRAY_SIZE(fusb301_psy_props);
	fusb->psy_desc.get_property = fusb301_psy_get_property;

	psy_cfg.fwnode = dev_fwnode(dev);
	psy_cfg.drv_data = fusb;
	fusb->psy = devm_power_supply_register(dev, &fusb->psy_desc, &psy_cfg);
	if (IS_ERR(fusb->psy)) {
		ret = dev_err_probe(dev, PTR_ERR(fusb->psy),
				    "failed to register power supply\n");
		goto err_unregister_port;
	}

	ret = fusb301_update_status(fusb);
	if (ret)
		goto err_unregister_port;

	i2c_set_clientdata(client, fusb);

	if (fusb->irq > 0) {
		ret = request_threaded_irq(fusb->irq, NULL, fusb301_irq,
					   IRQF_ONESHOT, dev_name(dev), fusb);
		if (ret)
			goto err_unregister_port;
	} else {
		dev_info(dev, "no interrupt, polling every %u ms\n",
			 FUSB301_POLL_INTERVAL_MS);
		INIT_DELAYED_WORK(&fusb->poll_work, fusb301_poll_work);
		queue_delayed_work(system_freezable_power_efficient_wq,
				   &fusb->poll_work,
				   msecs_to_jiffies(FUSB301_POLL_INTERVAL_MS));
	}

	fwnode_handle_put(fusb->connector);
	return 0;

err_unregister_port:
	fusb301_vbus_set(fusb, false);
	fusb301_unregister_partner(fusb);
	typec_unregister_port(fusb->port);
err_put_role_sw:
	usb_role_switch_set_role(fusb->role_sw, USB_ROLE_NONE);
	usb_role_switch_put(fusb->role_sw);
err_put_connector:
	fwnode_handle_put(fusb->connector);
	return ret;
}

static void fusb301_remove(struct i2c_client *client)
{
	struct fusb301 *fusb = i2c_get_clientdata(client);

	if (fusb->irq > 0)
		free_irq(fusb->irq, fusb);
	else
		cancel_delayed_work_sync(&fusb->poll_work);
	fusb301_vbus_set(fusb, false);
	fusb301_unregister_partner(fusb);
	typec_unregister_port(fusb->port);
	usb_role_switch_set_role(fusb->role_sw, USB_ROLE_NONE);
	usb_role_switch_put(fusb->role_sw);
}

static void fusb301_shutdown(struct i2c_client *client)
{
	struct fusb301 *fusb = i2c_get_clientdata(client);

	/*
	 * Stop event processing before turning VBUS off, otherwise the poll
	 * work could run once more, see a still-attached sink and re-enable
	 * the VBUS supply (and keep issuing I2C transfers) after shutdown.
	 */
	if (fusb->irq > 0)
		disable_irq(fusb->irq);
	else
		cancel_delayed_work_sync(&fusb->poll_work);
	fusb301_vbus_set(fusb, false);
}

static const struct of_device_id fusb301_of_match[] = {
	{ .compatible = "onnn,fusb301" },
	{ .compatible = "onnn,fusb301a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fusb301_of_match);

static const struct i2c_device_id fusb301_i2c_id[] = {
	{ "fusb301" },
	{ "fusb301a" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fusb301_i2c_id);

static struct i2c_driver fusb301_driver = {
	.probe = fusb301_probe,
	.remove = fusb301_remove,
	.shutdown = fusb301_shutdown,
	.driver = {
		.name = "fusb301",
		.of_match_table = fusb301_of_match,
	},
	.id_table = fusb301_i2c_id,
};
module_i2c_driver(fusb301_driver);

MODULE_DESCRIPTION("onsemi FUSB301 autonomous USB Type-C controller");
MODULE_LICENSE("GPL");
