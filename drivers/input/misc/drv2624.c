// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments DRV2624 haptic driver
 *
 * Copyright (c) 2026
 *
 * Based on drv260x.c by Dan Murphy <dmurphy@ti.com>.
 * DRV2624 is the successor of the DRV260x family. Compared to DRV260x,
 * DRV2624 adds an internal RAM playback waveform sequencer, separate
 * START/STOP register, and a different mode encoding. This driver
 * implements basic real-time playback (RTP) mode driven by the input
 * force-feedback (FF_RUMBLE) framework, suitable for use by feedbackd
 * and similar userspace haptic stacks.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define DRV2624_REG_CHIP_ID		0x00
#define DRV2624_REG_STATUS		0x01
#define DRV2624_REG_MODE		0x07
#define   DRV2624_MODE_STANDBY		BIT(6)
#define   DRV2624_MODE_MASK		GENMASK(2, 0)
#define   DRV2624_MODE_RAM_PLAYBACK	0x00
#define   DRV2624_MODE_RAM_WAVE_SEQ	0x01
#define   DRV2624_MODE_RTP		0x02
#define   DRV2624_MODE_DIAGNOSTICS	0x03
#define DRV2624_REG_RTP_INPUT		0x0E
#define DRV2624_REG_GO			0x0C
#define   DRV2624_GO_BIT		BIT(0)
#define DRV2624_REG_STOP		0x0D
#define   DRV2624_STOP_BIT		BIT(0)
#define DRV2624_REG_RATED_VOLT		0x1F
#define DRV2624_REG_OD_CLAMP		0x20
#define DRV2624_REG_LRA_PERIOD_H	0x2E
#define DRV2624_REG_LRA_PERIOD_L	0x2F
#define DRV2624_REG_CONTROL1		0x27
#define   DRV2624_CTRL1_LRA		BIT(7)
#define DRV2624_REG_MAX			0x30

#define DRV2624_CHIP_ID_VAL		0x03

/* Default rated/overdrive voltages for a generic LRA (Vrms) */
#define DRV2624_DEF_RATED_MV		2100	/* ~2.1V rms */
#define DRV2624_DEF_OD_MV		3000	/* ~3.0V peak */

/* Default LRA resonant frequency, Hz */
#define DRV2624_DEF_LRA_HZ		205

enum drv2624_actuator {
	DRV2624_ACTUATOR_LRA,
	DRV2624_ACTUATOR_ERM,
};

struct drv2624_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regmap *regmap;
	struct work_struct work;

	struct gpio_desc *enable_gpio;
	struct regulator *vdd;

	enum drv2624_actuator actuator;
	u32 rated_mv;
	u32 od_mv;
	u32 lra_freq_hz;

	u8 magnitude;
};

static const struct regmap_config drv2624_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = DRV2624_REG_MAX,
};

/*
 * RATED_VOLTAGE register encoding (from datasheet, LRA mode, OD_CLAMP_LATCH=0):
 *   reg_val = round( v_mv * sqrt(1 - 4*300us*lra_hz) / (5.3438 mV) )
 * For Pixel-class LRAs (~205 Hz) this collapses to roughly:
 *   reg_val ≈ v_mv * 100 / 575
 * Driver uses the simplified linear approximation since vendor parts vary
 * and the exact constant gets tuned via DT (ti,rated-voltage-mv).
 */
static u8 drv2624_voltage_to_reg(u32 mv)
{
	return min_t(u32, (mv * 100U) / 575U, 0xFF);
}

static void drv2624_worker(struct work_struct *work)
{
	struct drv2624_data *h = container_of(work, struct drv2624_data, work);
	int error;

	if (!h->magnitude) {
		error = regmap_write(h->regmap, DRV2624_REG_STOP, DRV2624_STOP_BIT);
		if (error)
			dev_err(&h->client->dev, "stop write failed: %d\n", error);
		return;
	}

	/*
	 * RTP_INPUT is signed 8-bit (-127..+127). The FF_RUMBLE strong/weak
	 * magnitude is u16 — scale to the positive RTP range only.
	 */
	error = regmap_write(h->regmap, DRV2624_REG_RTP_INPUT, h->magnitude);
	if (error) {
		dev_err(&h->client->dev, "rtp_input write failed: %d\n", error);
		return;
	}

	error = regmap_write(h->regmap, DRV2624_REG_GO, DRV2624_GO_BIT);
	if (error)
		dev_err(&h->client->dev, "go bit write failed: %d\n", error);
}

static int drv2624_play(struct input_dev *input, void *data,
			struct ff_effect *effect)
{
	struct drv2624_data *h = input_get_drvdata(input);
	u16 mag;

	mag = effect->u.rumble.strong_magnitude;
	if (!mag)
		mag = effect->u.rumble.weak_magnitude;

	/* Scale u16 magnitude into the 0..0x7F positive RTP range */
	h->magnitude = mag >> 9;

	schedule_work(&h->work);
	return 0;
}

static void drv2624_close(struct input_dev *input)
{
	struct drv2624_data *h = input_get_drvdata(input);

	cancel_work_sync(&h->work);
	regmap_write(h->regmap, DRV2624_REG_STOP, DRV2624_STOP_BIT);
}

static int drv2624_hw_init(struct drv2624_data *h)
{
	struct device *dev = &h->client->dev;
	unsigned int chip_id, period;
	int error;

	error = regmap_read(h->regmap, DRV2624_REG_CHIP_ID, &chip_id);
	if (error) {
		dev_err(dev, "failed to read CHIP_ID: %d\n", error);
		return error;
	}
	if (chip_id != DRV2624_CHIP_ID_VAL) {
		dev_err(dev, "unexpected CHIP_ID 0x%02x (want 0x%02x)\n",
			chip_id, DRV2624_CHIP_ID_VAL);
		return -ENODEV;
	}

	/* Set actuator type */
	error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL1,
				   DRV2624_CTRL1_LRA,
				   h->actuator == DRV2624_ACTUATOR_LRA ?
					DRV2624_CTRL1_LRA : 0);
	if (error)
		return error;

	/* Program rated and overdrive voltages */
	error = regmap_write(h->regmap, DRV2624_REG_RATED_VOLT,
			     drv2624_voltage_to_reg(h->rated_mv));
	if (error)
		return error;

	error = regmap_write(h->regmap, DRV2624_REG_OD_CLAMP,
			     drv2624_voltage_to_reg(h->od_mv));
	if (error)
		return error;

	/* Program LRA resonant period (for LRA actuators) */
	if (h->actuator == DRV2624_ACTUATOR_LRA && h->lra_freq_hz) {
		/* period = 1 / (LRA_PERIOD_LSB * f_lra), LSB = 24.39us */
		period = 1000000U / (h->lra_freq_hz * 24U + h->lra_freq_hz / 3U);
		regmap_write(h->regmap, DRV2624_REG_LRA_PERIOD_H,
			     (period >> 8) & 0xFF);
		regmap_write(h->regmap, DRV2624_REG_LRA_PERIOD_L,
			     period & 0xFF);
	}

	/* Take out of standby, set RTP mode */
	error = regmap_write(h->regmap, DRV2624_REG_MODE, DRV2624_MODE_RTP);
	if (error)
		return error;

	return 0;
}

static int drv2624_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct drv2624_data *h;
	const char *actuator;
	int error;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->client = client;
	i2c_set_clientdata(client, h);
	INIT_WORK(&h->work, drv2624_worker);

	/* Actuator type (lra/erm), defaults to LRA */
	h->actuator = DRV2624_ACTUATOR_LRA;
	if (!device_property_read_string(dev, "ti,actuator", &actuator)) {
		if (!strcmp(actuator, "erm"))
			h->actuator = DRV2624_ACTUATOR_ERM;
		else if (strcmp(actuator, "lra"))
			return dev_err_probe(dev, -EINVAL,
				"ti,actuator must be 'lra' or 'erm'\n");
	}

	if (device_property_read_u32(dev, "ti,rated-voltage-mv", &h->rated_mv))
		h->rated_mv = DRV2624_DEF_RATED_MV;
	if (device_property_read_u32(dev, "ti,overdrive-voltage-mv", &h->od_mv))
		h->od_mv = DRV2624_DEF_OD_MV;
	if (device_property_read_u32(dev, "ti,lra-frequency-hz", &h->lra_freq_hz))
		h->lra_freq_hz = DRV2624_DEF_LRA_HZ;

	h->vdd = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(h->vdd)) {
		if (PTR_ERR(h->vdd) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(h->vdd),
					     "failed to get vdd regulator\n");
		h->vdd = NULL;
	}
	if (h->vdd) {
		error = regulator_enable(h->vdd);
		if (error)
			return dev_err_probe(dev, error,
					     "failed to enable vdd\n");
	}

	h->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(h->enable_gpio)) {
		error = PTR_ERR(h->enable_gpio);
		goto err_disable_vdd;
	}
	if (h->enable_gpio)
		usleep_range(250, 500);	/* datasheet: wait 250us after EN high */

	h->regmap = devm_regmap_init_i2c(client, &drv2624_regmap_config);
	if (IS_ERR(h->regmap)) {
		error = PTR_ERR(h->regmap);
		goto err_gpio_low;
	}

	error = drv2624_hw_init(h);
	if (error)
		goto err_gpio_low;

	h->input_dev = devm_input_allocate_device(dev);
	if (!h->input_dev) {
		error = -ENOMEM;
		goto err_gpio_low;
	}

	h->input_dev->name = "drv2624:haptics";
	h->input_dev->close = drv2624_close;
	input_set_drvdata(h->input_dev, h);
	input_set_capability(h->input_dev, EV_FF, FF_RUMBLE);

	error = input_ff_create_memless(h->input_dev, NULL, drv2624_play);
	if (error)
		goto err_gpio_low;

	error = input_register_device(h->input_dev);
	if (error)
		goto err_gpio_low;

	return 0;

err_gpio_low:
	if (h->enable_gpio)
		gpiod_set_value_cansleep(h->enable_gpio, 0);
err_disable_vdd:
	if (h->vdd)
		regulator_disable(h->vdd);
	return error;
}

static void drv2624_remove(struct i2c_client *client)
{
	struct drv2624_data *h = i2c_get_clientdata(client);

	cancel_work_sync(&h->work);

	if (h->enable_gpio)
		gpiod_set_value_cansleep(h->enable_gpio, 0);
	if (h->vdd)
		regulator_disable(h->vdd);
}

static int drv2624_suspend(struct device *dev)
{
	struct drv2624_data *h = dev_get_drvdata(dev);

	guard(mutex)(&h->input_dev->mutex);

	if (!input_device_enabled(h->input_dev))
		return 0;

	regmap_update_bits(h->regmap, DRV2624_REG_MODE,
			   DRV2624_MODE_STANDBY, DRV2624_MODE_STANDBY);
	if (h->enable_gpio)
		gpiod_set_value_cansleep(h->enable_gpio, 0);
	if (h->vdd)
		regulator_disable(h->vdd);
	return 0;
}

static int drv2624_resume(struct device *dev)
{
	struct drv2624_data *h = dev_get_drvdata(dev);
	int error;

	guard(mutex)(&h->input_dev->mutex);

	if (!input_device_enabled(h->input_dev))
		return 0;

	if (h->vdd) {
		error = regulator_enable(h->vdd);
		if (error)
			return error;
	}
	if (h->enable_gpio) {
		gpiod_set_value_cansleep(h->enable_gpio, 1);
		usleep_range(250, 500);
	}
	regmap_update_bits(h->regmap, DRV2624_REG_MODE,
			   DRV2624_MODE_STANDBY, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(drv2624_pm_ops, drv2624_suspend, drv2624_resume);

static const struct i2c_device_id drv2624_i2c_id[] = {
	{ "drv2624" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, drv2624_i2c_id);

static const struct of_device_id drv2624_of_match[] = {
	{ .compatible = "ti,drv2624" },
	{ }
};
MODULE_DEVICE_TABLE(of, drv2624_of_match);

static struct i2c_driver drv2624_driver = {
	.probe = drv2624_probe,
	.remove = drv2624_remove,
	.id_table = drv2624_i2c_id,
	.driver = {
		.name = "drv2624-haptics",
		.of_match_table = drv2624_of_match,
		.pm = pm_sleep_ptr(&drv2624_pm_ops),
	},
};
module_i2c_driver(drv2624_driver);

MODULE_DESCRIPTION("Texas Instruments DRV2624 haptic driver");
MODULE_LICENSE("GPL");
