// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2025 Vasiliy Doylov <nekocwd@mainlining.org>
// Copyright (c) 2025 Frieder Hannenheim <git@fhannenheim.net>

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/math.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-event.h>

#define LC898219XI_NAME "lc898219xi"
/* Actuator has 12 bit resolution */
#define LC898219XI_MAX_FOCUS_POS (4096 - 1)
#define LC898219XI_MIN_FOCUS_POS 0
#define LC898219XI_FOCUS_STEPS 1
#define LC898219XI_DAC_ADDR CCI_REG16(0x84)

static const char *const lc898219xi_supply_names[] = {
	"vdd",
	"vio",
	"vana",
};

struct lc898219xi {
	struct regulator_bulk_data supplies[ARRAY_SIZE(lc898219xi_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct v4l2_subdev sd;
	struct regmap *regmap;
	struct device *sensor;
	/* Protected by the control handler lock. */
	bool powered;
};

static inline struct lc898219xi *sd_to_lc898219xi(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct lc898219xi, sd);
}

static int lc898219xi_set_dac(struct lc898219xi *lc898219xi, u16 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&lc898219xi->sd);
	int ret;

	/*
	/*
	 * The control's 0..4095 maps one to one onto the 12-bit
	 * two's-complement target position, 2047 down to -2048.
	 */
	s16 actuator_val = 2047 - val;
	u16 actuator_val_s12 = actuator_val & LC898219XI_MAX_FOCUS_POS;

	ret = cci_write(lc898219xi->regmap, LC898219XI_DAC_ADDR,
			actuator_val_s12, NULL);
	if (ret)
		dev_err(&client->dev, "failed to set DAC: %d\n", ret);

	return ret;
}

/* One wake attempt. The rails are already up when this is called. */
static int lc898219xi_wake(struct i2c_client *client)
{
	int regdata, retry, ret;

	regdata = i2c_smbus_read_byte_data(client, 0xF0);
	if (regdata < 0) {
		/* A bus error is not a bad part; report it as such. */
		dev_dbg(&client->dev, "chip id read failed: %d\n", regdata);
		return regdata;
	}
	if (regdata != 0xA5) {
		dev_dbg(&client->dev, "bad chip id: %x\n", regdata);
		return -ENODEV;
	}

	usleep_range(1000, 1010);

	ret = i2c_smbus_write_byte_data(client, 0xE0, 0x01);
	if (ret < 0)
		return ret;
	msleep(8);

	for (retry = 0; retry < 10; retry++) {
		int check = i2c_smbus_read_byte_data(client, 0xB3);

		if (check < 0)
			return check;
		if ((check & 0xE0) == 0)
			return i2c_smbus_write_byte_data(client, 0x8C, 0xE9);
		usleep_range(1000, 1010);
	}

	dev_dbg(&client->dev, "LSI wake up check failed\n");
	return -ETIMEDOUT;
}

static int lc898219xi_power_on(struct lc898219xi *lc898219xi)
{
	struct i2c_client *client = v4l2_get_subdevdata(&lc898219xi->sd);
	int attempt, ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(lc898219xi_supply_names),
				    lc898219xi->supplies);

	if (ret < 0) {
		dev_err(&client->dev, "failed to enable regulators\n");
		return ret;
	}

	usleep_range(8000, 10000);

	/*
	 * Early in boot the camera rails are still ramping and the first wake
	 * attempt fails, which used to leave the whole camera dead: the sensor
	 * waits on this subdev through lens-focus, so it never registers either.
	 * Give the coil a few more tries before giving up.
	 */
	for (attempt = 0; attempt < 3; attempt++) {
		ret = lc898219xi_wake(client);
		if (!ret)
			return 0;
		usleep_range(10000, 12000);
	}

	dev_err(&client->dev, "wake up failed: %d\n", ret);
	regulator_bulk_disable(ARRAY_SIZE(lc898219xi_supply_names),
			       lc898219xi->supplies);
	return ret;
}

static int lc898219xi_power_off(struct lc898219xi *lc898219xi)
{
	regulator_bulk_disable(ARRAY_SIZE(lc898219xi_supply_names),
			       lc898219xi->supplies);
	return 0;
}

/*
 * Program the cached focus position into a coil that has just been powered and
 * woken, and let s_ctrl reach the chip from now on.
 */
static int lc898219xi_apply_focus(struct lc898219xi *lc898219xi)
{
	int ret;

	v4l2_ctrl_lock(lc898219xi->focus);
	ret = lc898219xi_set_dac(lc898219xi, lc898219xi->focus->cur.val);
	lc898219xi->powered = !ret;
	v4l2_ctrl_unlock(lc898219xi->focus);

	return ret;
}

static int __maybe_unused lc898219xi_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898219xi *lc898219xi = sd_to_lc898219xi(sd);

	/* Stop s_ctrl before the rails go away. */
	v4l2_ctrl_lock(lc898219xi->focus);
	lc898219xi->powered = false;
	v4l2_ctrl_unlock(lc898219xi->focus);

	lc898219xi_power_off(lc898219xi);
	return 0;
}

static int __maybe_unused lc898219xi_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lc898219xi *lc898219xi = sd_to_lc898219xi(sd);
	int ret;

	ret = lc898219xi_power_on(lc898219xi);
	if (ret < 0) {
		dev_err(dev, "power on failed\n");
		return ret;
	}

	/* s_ctrl only cached the focus position while the coil was off. */
	ret = lc898219xi_apply_focus(lc898219xi);
	if (ret)
		lc898219xi_power_off(lc898219xi);

	return ret;
}

static int lc898219xi_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lc898219xi *lc898219xi = container_of(ctrl->handler,
						     struct lc898219xi, ctrls);

	/*
	 * The coil is powered together with the sensor, not by this control.
	 * While it is off, let the control framework cache the value; it is
	 * applied at the next power up. The runtime PM state cannot tell: the
	 * coil is active with a zero usage count until the autosuspend fires,
	 * and not active yet while it resumes.
	 */
	if (!lc898219xi->powered)
		return 0;

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
		return lc898219xi_set_dac(lc898219xi, ctrl->val);

	return 0;
}

static const struct v4l2_ctrl_ops lc898219xi_ctrl_ops = {
	.s_ctrl = lc898219xi_set_ctrl,
};

static const struct v4l2_subdev_core_ops lc898219xi_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
};

static const struct v4l2_subdev_ops lc898219xi_ops = {
	.core = &lc898219xi_core_ops,
};

static int lc898219xi_init_controls(struct lc898219xi *lc898219xi)
{
	struct v4l2_ctrl_handler *hdl = &lc898219xi->ctrls;
	const struct v4l2_ctrl_ops *ops = &lc898219xi_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 1);

	lc898219xi->focus = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
					      LC898219XI_MIN_FOCUS_POS,
					      LC898219XI_MAX_FOCUS_POS,
					      LC898219XI_FOCUS_STEPS, 0);

	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	lc898219xi->sd.ctrl_handler = hdl;

	return 0;
}

/*
 * The coil has nothing to do while the sensor is off, and a coil left powered
 * at idle is audible. Make the sensor that names this lens in lens-focus a
 * runtime PM consumer of it, so that the PM core powers the coil up with the
 * sensor and lets it power down again when the sensor does.
 */
static int lc898219xi_link_sensor(struct lc898219xi *lc898219xi,
				  struct device *dev)
{
	struct device_node *np, *lens;
	struct device *sensor;

	for_each_node_with_property(np, "lens-focus") {
		lens = of_parse_phandle(np, "lens-focus", 0);
		of_node_put(lens);
		if (lens == dev->of_node)
			break;
	}
	if (!np)
		return dev_err_probe(dev, -ENODEV,
				     "no sensor refers to this lens\n");

	sensor = bus_find_device_by_of_node(&i2c_bus_type, np);
	of_node_put(np);
	if (!sensor)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "sensor is not registered yet\n");

	if (!device_link_add(sensor, dev,
			     DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME)) {
		put_device(sensor);
		return dev_err_probe(dev, -EINVAL,
				     "failed to link to the sensor\n");
	}

	/*
	 * Keep the sensor rather than the link: the driver core frees the link
	 * behind our back if the sensor is unregistered first, and
	 * device_link_remove() copes with that where device_link_del() on a
	 * stale link would not.
	 */
	lc898219xi->sensor = sensor;

	return 0;
}

static int lc898219xi_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lc898219xi *lc898219xi;
	unsigned int i;
	int ret;

	lc898219xi = devm_kzalloc(dev, sizeof(*lc898219xi), GFP_KERNEL);
	if (!lc898219xi)
		return -ENOMEM;

	lc898219xi->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(lc898219xi->regmap))
		return dev_err_probe(dev, PTR_ERR(lc898219xi->regmap),
				     "failed to initialize CCI\n");

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&lc898219xi->sd, client, &lc898219xi_ops);

	for (i = 0; i < ARRAY_SIZE(lc898219xi_supply_names); i++)
		lc898219xi->supplies[i].supply = lc898219xi_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(lc898219xi_supply_names),
				      lc898219xi->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* Before the power on, so that a deferral does not cycle the coil. */
	ret = lc898219xi_link_sensor(lc898219xi, dev);
	if (ret)
		return ret;

	ret = lc898219xi_power_on(lc898219xi);
	if (ret) {
		/*
		 * Retry rather than fail for good: a coil that is not awake yet
		 * would otherwise keep the sensor unregistered for the whole
		 * uptime, since lens-focus makes the sensor wait for us.
		 */
		ret = dev_err_probe(dev,
				    ret == -ETIMEDOUT ? -EPROBE_DEFER : ret,
				    "power on failed\n");
		goto err_unlink_sensor;
	}

	ret = lc898219xi_init_controls(lc898219xi);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init v4l2 controls\n");
		goto err_power_off;
	}

	/* The coil is powered: program the default focus position. */
	ret = lc898219xi_apply_focus(lc898219xi);
	if (ret)
		goto err_free_handler;

	/* Initialize subdev */
	lc898219xi->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	ret = media_entity_pads_init(&lc898219xi->sd.entity, 0, NULL);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to init media entity pads");
		goto err_free_handler;
	}

	lc898219xi->sd.entity.function = MEDIA_ENT_F_LENS;

	/*
	 * Enable runtime PM. As the device has been powered manually, mark it
	 * as active, and increase the usage count without resuming the device.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev(&lc898219xi->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to register V4L2 subdev\n");
		goto err_pm;
	}

	/*
	 * Finally, enable autosuspend and decrease the usage count. The device
	 * will get suspended after the autosuspend delay, turning the power
	 * off.
	 */
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	media_entity_cleanup(&lc898219xi->sd.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&lc898219xi->ctrls);
err_power_off:
	lc898219xi_power_off(lc898219xi);
err_unlink_sensor:
	device_link_remove(lc898219xi->sensor, dev);
	put_device(lc898219xi->sensor);
	return ret;
}

static void lc898219xi_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lc898219xi *lc898219xi = sd_to_lc898219xi(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(&lc898219xi->sd);
	media_entity_cleanup(&lc898219xi->sd.entity);

	/*
	 * Disable runtime PM. In case runtime PM is disabled in the kernel,
	 * make sure to turn power off manually.
	 */
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		lc898219xi_runtime_suspend(dev);
	pm_runtime_set_suspended(dev);

	/* The runtime PM callbacks use the focus control, so free it only now. */
	v4l2_ctrl_handler_free(&lc898219xi->ctrls);

	device_link_remove(lc898219xi->sensor, dev);
	put_device(lc898219xi->sensor);
}

static const struct of_device_id lc898219xi_of_table[] = {
	{ .compatible = "onnn,lc898219xi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, lc898219xi_of_table);

static const struct dev_pm_ops lc898219xi_pm_ops = {
	SET_RUNTIME_PM_OPS(lc898219xi_runtime_suspend,
			   lc898219xi_runtime_resume, NULL)
};

static struct i2c_driver lc898219xi_i2c_driver = {
	.driver = {
		.name = LC898219XI_NAME,
		.pm = &lc898219xi_pm_ops,
		.of_match_table = lc898219xi_of_table,
	},
	.probe = lc898219xi_probe,
	.remove = lc898219xi_remove,
};
module_i2c_driver(lc898219xi_i2c_driver);

MODULE_AUTHOR("Vasiliy Doylov <nekocwd@mainlining.org>");
MODULE_AUTHOR("Frieder Hannenheim <git@fhannenheim.net>");
MODULE_DESCRIPTION("Onsemi LC898219XI VCM driver");
MODULE_LICENSE("GPL");
