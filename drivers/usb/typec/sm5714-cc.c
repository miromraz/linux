// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5714 Type-C CC detection: switch the USB controller between
 * device and host role, and power VBUS while a sink (OTG device) is attached.
 *
 * Only attach detection is handled; USB PD is not. Register layout from the
 * Samsung downstream sm5714_typec driver.
 */
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>

#define SM5714_REG_INT1			0x01
#define SM5714_REG_INT2			0x02
#define SM5714_REG_INT3			0x03
#define SM5714_REG_INT4			0x04
#define SM5714_REG_INT5			0x05
#define SM5714_REG_INT_MASK1		0x06
#define SM5714_REG_CC_STATUS		0x28

#define SM5714_INT1_ATTACH		BIT(3)
#define SM5714_INT1_DETACH		BIT(4)

#define SM5714_CC_ATTACH_TYPE		GENMASK(2, 0)
#define SM5714_CC_ATTACH_SOURCE		0x1	/* partner sources power: we are a device */
#define SM5714_CC_ATTACH_SINK		0x2	/* partner sinks power: we are the host */

struct sm5714_cc {
	struct device *dev;
	struct regmap *regmap;
	struct usb_role_switch *role_sw;
	struct regulator *vbus;
	struct mutex lock;	/* role and vbus state */
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

static void sm5714_cc_update(struct sm5714_cc *cc)
{
	enum usb_role role = USB_ROLE_DEVICE;
	unsigned int st = 0;

	mutex_lock(&cc->lock);

	/*
	 * Default to device mode: it is what a charger or a PC needs, and what
	 * keeps a debug gadget reachable. Become host only on an explicit sink.
	 */
	if (!regmap_read(cc->regmap, SM5714_REG_CC_STATUS, &st) &&
	    (st & SM5714_CC_ATTACH_TYPE) == SM5714_CC_ATTACH_SINK)
		role = USB_ROLE_HOST;

	dev_dbg(cc->dev, "CC_STATUS 0x%02x -> %s\n", st, usb_role_string(role));

	if (role != USB_ROLE_HOST)
		sm5714_cc_set_vbus(cc, false);
	usb_role_switch_set_role(cc->role_sw, role);
	if (role == USB_ROLE_HOST)
		sm5714_cc_set_vbus(cc, true);

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
	for (i = SM5714_REG_INT2; i <= SM5714_REG_INT5; i++)
		regmap_read(cc->regmap, i, &dummy);

	if (int1 & (SM5714_INT1_ATTACH | SM5714_INT1_DETACH))
		sm5714_cc_update(cc);

	return IRQ_HANDLED;
}

static const struct regmap_config sm5714_cc_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static void sm5714_cc_put_role_sw(void *data)
{
	usb_role_switch_put(data);
}

static int sm5714_cc_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sm5714_cc *cc;
	unsigned int dummy;
	int ret, i;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;
	cc->dev = dev;
	mutex_init(&cc->lock);

	cc->regmap = devm_regmap_init_i2c(client, &sm5714_cc_regmap);
	if (IS_ERR(cc->regmap))
		return PTR_ERR(cc->regmap);

	cc->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(cc->vbus))
		return dev_err_probe(dev, PTR_ERR(cc->vbus), "no VBUS regulator\n");

	cc->role_sw = usb_role_switch_get(dev);
	if (IS_ERR_OR_NULL(cc->role_sw))
		return dev_err_probe(dev, cc->role_sw ? PTR_ERR(cc->role_sw) : -EPROBE_DEFER,
				     "no USB role switch\n");
	ret = devm_add_action_or_reset(dev, sm5714_cc_put_role_sw, cc->role_sw);
	if (ret)
		return ret;

	/* drop stale interrupts, unmask attach/detach */
	for (i = SM5714_REG_INT1; i <= SM5714_REG_INT5; i++)
		regmap_read(cc->regmap, i, &dummy);
	regmap_update_bits(cc->regmap, SM5714_REG_INT_MASK1,
			   SM5714_INT1_ATTACH | SM5714_INT1_DETACH, 0);

	sm5714_cc_update(cc);

	cc->irq = client->irq;
	i2c_set_clientdata(client, cc);

	return devm_request_threaded_irq(dev, cc->irq, NULL, sm5714_cc_irq,
					 IRQF_ONESHOT, "sm5714-cc", cc);
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
};
module_i2c_driver(sm5714_cc_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5714 Type-C attach detection");
MODULE_LICENSE("GPL");
