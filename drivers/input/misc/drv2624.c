// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments DRV2624 haptic driver
 *
 * Copyright (c) 2026 miromraz <mraz.miro@seznam.cz>
 *
 * Based on drv260x.c by Dan Murphy <dmurphy@ti.com>.
 *
 * DRV2624 is the successor of the DRV260x family. Compared to DRV260x
 * it uses a different mode encoding and a separate GO register, but the
 * FF_RUMBLE play path is the same real-time-playback (RTP) model that
 * drv260x.c uses: on a non-zero magnitude the driver selects RTP mode,
 * writes the amplitude to RTP_INPUT and sets GO; the chip then vibrates
 * until GO is cleared. The input force-feedback (ff-memless) layer
 * starts an effect with a magnitude and ends it with magnitude 0 when
 * the effect duration elapses, so the driver never has to time or guess
 * durations itself.
 *
 * A `ti,autocal-comp` byte-array DT property can carry the device's
 * factory autocal compensation; if absent the chip's internal
 * defaults are used.
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
#define DRV2624_REG_LRA_PERIOD_H	0x05	/* measured closed-loop period */
#define DRV2624_REG_LRA_PERIOD_L	0x06
#define DRV2624_REG_MODE		0x07
#define   DRV2624_MODE_MASK		GENMASK(1, 0)
/* MODE[1:0], datasheet Table 8-10: 0 RTP, 1 waveform sequencer, 2 diag, 3 autocal */
#define   DRV2624_MODE_RTP		0x00
#define DRV2624_REG_CONTROL1		0x08
#define   DRV2624_CTRL1_LRA		BIT(7)	/* LRA_ERM, Table 8-11 */
#define   DRV2624_CTRL1_AUTO_BRK_OL	BIT(4)
#define   DRV2624_CTRL1_AUTO_BRK_INTO_STBY  BIT(3)
#define DRV2624_REG_GO			0x0C
#define   DRV2624_GO_BIT		BIT(0)
#define DRV2624_REG_RTP_INPUT		0x0E
#define DRV2624_REG_RATED_VOLT		0x1F
#define DRV2624_REG_OD_CLAMP		0x20
#define DRV2624_REG_AUTOCAL_COMP	0x21	/* A_CAL_COMP + A_CAL_BEMF, 0x21/0x22 */
#define DRV2624_REG_DRIVE_TIME		0x27
#define DRV2624_REG_BLANKING_IDISS	0x28
#define DRV2624_REG_ZC_DET_TIME		0x29
#define DRV2624_REG_LRA_WAVE_SHAPE	0x2C
#define   DRV2624_LRA_WAVE_SINE		BIT(0)
#define DRV2624_REG_OL_LRA_PERIOD_H	0x2E
#define DRV2624_REG_OL_LRA_PERIOD_L	0x2F
#define DRV2624_REG_MAX			0x30

#define DRV2624_CHIP_ID_MASK		GENMASK(7, 4)	/* CHIPID[3:0], Table 8-2 */
#define DRV2624_CHIP_ID_VAL		0x00		/* DRV2624 */

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
	u32 lra_freq_hz;
	u32 ol_lra_period;	/* DT-supplied per-unit factory cal; 0 = derive from freq */
	u8 rated_volt_raw;	/* raw register value; 0 = leave at chip default */
	u8 od_clamp_raw;	/* raw register value; 0 = leave at chip default */
	u8 autocal[2];
	bool autocal_present;

	u8 magnitude;
};

static const struct regmap_config drv2624_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = DRV2624_REG_MAX,
};

/*
 * ff-memless drives this: a non-zero magnitude starts vibration, and a
 * magnitude of 0 (posted when the effect's duration elapses) stops it.
 * On start, select RTP mode and write the 7-bit amplitude to RTP_INPUT,
 * then set GO; the chip drives the LRA until GO is cleared. On stop,
 * clear GO — with AUTO_BRK_INTO_STBY set in CONTROL1 the chip brakes the
 * LRA and idles by itself (datasheet Table 8-15; there is no STOP bit).
 */
static void drv2624_worker(struct work_struct *work)
{
	struct drv2624_data *h = container_of(work, struct drv2624_data, work);
	struct device *dev = &h->client->dev;
	int error;

	if (!h->magnitude) {
		error = regmap_write(h->regmap, DRV2624_REG_GO, 0);
		if (error)
			dev_err(dev, "GO clear failed: %d\n", error);
		return;
	}

	error = regmap_update_bits(h->regmap, DRV2624_REG_MODE,
				   DRV2624_MODE_MASK, DRV2624_MODE_RTP);
	if (error) {
		dev_err(dev, "RTP mode write failed: %d\n", error);
		return;
	}
	error = regmap_write(h->regmap, DRV2624_REG_RTP_INPUT, h->magnitude);
	if (error) {
		dev_err(dev, "RTP_INPUT write failed: %d\n", error);
		return;
	}
	error = regmap_write(h->regmap, DRV2624_REG_GO, DRV2624_GO_BIT);
	if (error)
		dev_err(dev, "GO bit write failed: %d\n", error);
}

static int drv2624_play(struct input_dev *input, void *data,
			struct ff_effect *effect)
{
	struct drv2624_data *h = input_get_drvdata(input);
	u16 mag;

	mag = effect->u.rumble.strong_magnitude;
	if (!mag)
		mag = effect->u.rumble.weak_magnitude;

	h->magnitude = mag >> 9;	/* u16 → 0..0x7F RTP range */

	schedule_work(&h->work);
	return 0;
}

static void drv2624_close(struct input_dev *input)
{
	struct drv2624_data *h = input_get_drvdata(input);

	cancel_work_sync(&h->work);
	/* Stop any active playback by clearing GO (datasheet Table 8-15). */
	regmap_write(h->regmap, DRV2624_REG_GO, 0);
}

static int drv2624_hw_init(struct drv2624_data *h)
{
	struct device *dev = &h->client->dev;
	unsigned int chip_id, period, status;
	int error;

	error = regmap_read(h->regmap, DRV2624_REG_CHIP_ID, &chip_id);
	if (error) {
		dev_err(dev, "failed to read CHIP_ID: %d\n", error);
		return error;
	}
	if ((chip_id & DRV2624_CHIP_ID_MASK) != DRV2624_CHIP_ID_VAL) {
		dev_err(dev, "unexpected CHIPID nibble in 0x%02x\n", chip_id);
		return -ENODEV;
	}

	/*
	 * CONTROL1: set actuator type (LRA), enable open-loop auto-brake (a
	 * brake waveform played at end of drive in open loop) and auto-brake
	 * into standby (decelerate the LRA when GO returns to 0 rather than
	 * cutting drive cold and leaving the actuator to ring down at its
	 * natural Q). Without AUTO_BRK_INTO_STBY a typical phone LRA rings
	 * for ~700 ms after the kernel ends an effect, which is felt as a
	 * long buzz instead of a crisp tap.
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL1,
				   DRV2624_CTRL1_LRA |
				   DRV2624_CTRL1_AUTO_BRK_OL |
				   DRV2624_CTRL1_AUTO_BRK_INTO_STBY,
				   (h->actuator == DRV2624_ACTUATOR_LRA ?
					DRV2624_CTRL1_LRA : 0) |
				   DRV2624_CTRL1_AUTO_BRK_OL |
				   DRV2624_CTRL1_AUTO_BRK_INTO_STBY);
	if (error)
		return error;

	/*
	 * Program rated and overdrive voltages if explicit raw register
	 * values were supplied via DT. The chip reset defaults
	 * (RATED_VOLT=0x3F, OD_CLAMP=0x89) are safe
	 * for the LRAs we've seen and match what the downstream Pixel HAL
	 * ends up with after autocal. Writing computed values from a
	 * voltage-in-mV formula is dangerous: the closed-form encoding
	 * depends on f_LRA, playback interval, OD_CLAMP_LATCH, and the
	 * chip revision, and easy approximations clamp to 0xFF for
	 * normal LRAs — which immediately over-currents on the next play.
	 */
	if (h->rated_volt_raw) {
		error = regmap_write(h->regmap, DRV2624_REG_RATED_VOLT,
				     h->rated_volt_raw);
		if (error)
			return error;
	}
	if (h->od_clamp_raw) {
		error = regmap_write(h->regmap, DRV2624_REG_OD_CLAMP,
				     h->od_clamp_raw);
		if (error)
			return error;
	}

	/*
	 * Apply factory autocal compensation if present: A_CAL_COMP (0x21)
	 * and A_CAL_BEMF (0x22) only. Pixel devices ship these two bytes
	 * per-unit in /persist/haptics/drv2624.cal; userspace (or DT, via
	 * the optional ti,autocal-comp property) passes them to the driver.
	 * 0x23 holds NG_THRESH/FB_BRAKE_FACTOR/LOOP_GAIN/BEMF_GAIN and must
	 * be left at its reset default. Without the bytes the chip falls
	 * back to its internal calibration defaults — usable but less tuned.
	 */
	if (h->autocal_present) {
		regmap_write(h->regmap, DRV2624_REG_AUTOCAL_COMP + 0, h->autocal[0]);
		regmap_write(h->regmap, DRV2624_REG_AUTOCAL_COMP + 1, h->autocal[1]);
	}

	/* LRA timing: DRIVE_TIME is the half-cycle drive duration. */
	if (h->actuator == DRV2624_ACTUATOR_LRA && h->lra_freq_hz) {
		u32 drive_time = (5U * (1000U - h->lra_freq_hz)) / h->lra_freq_hz;

		drive_time = min_t(u32, drive_time, 0x1F);
		regmap_update_bits(h->regmap, DRV2624_REG_DRIVE_TIME,
				   0x1F, drive_time);
	}

	/* BEMF sample timing + zero-cross detect — Pixel-downstream defaults. */
	regmap_write(h->regmap, DRV2624_REG_BLANKING_IDISS, 0x22);
	regmap_write(h->regmap, DRV2624_REG_ZC_DET_TIME, 0x00);

	/* Sine wave shape (default square sounds buzzier). */
	regmap_update_bits(h->regmap, DRV2624_REG_LRA_WAVE_SHAPE,
			   DRV2624_LRA_WAVE_SINE, DRV2624_LRA_WAVE_SINE);

	/*
	 * Open-loop LRA period. The chip uses this until the closed-loop
	 * tracker locks on resonance. Register unit is 24.615 us (datasheet
	 * Table 8-47); period_ticks ~ 41000 / f_Hz, and OL_LRA_PERIOD is
	 * 10 bits wide (bits [9:8] in PERIOD_H, [7:0] in PERIOD_L). The DT prop
	 * ti,ol-lra-period overrides the computed value with the chip's
	 * per-unit factory-calibrated period (Pixel devices ship this in
	 * /persist/haptics/drv2624.cal as "lra_period: NNN").
	 */
	if (h->actuator == DRV2624_ACTUATOR_LRA) {
		if (h->ol_lra_period)
			period = h->ol_lra_period;
		else if (h->lra_freq_hz)
			period = 41000U / h->lra_freq_hz;
		else
			period = 0;
		if (period) {
			period = min_t(u32, period, 0x3FF);
			regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_H,
				     (period >> 8) & 0x03);
			regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_L,
				     period & 0xFF);
		}
	}

	/*
	 * Let the programming settle, then read STATUS to clear any latched
	 * event bits (DIAG_RESULT, PRG_ERROR, etc.; datasheet Table 8-4,
	 * sticky and clear-on-read) before the first play.
	 */
	usleep_range(5000, 8000);
	regmap_read(h->regmap, DRV2624_REG_STATUS, &status);

	return 0;
}

static int drv2624_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct drv2624_data *h;
	const char *actuator;
	int error;
	u32 val;

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

	if (device_property_read_u32(dev, "ti,lra-frequency-hz", &h->lra_freq_hz))
		h->lra_freq_hz = DRV2624_DEF_LRA_HZ;

	/*
	 * Optional raw RATED_VOLT / OD_CLAMP register values. The chip's
	 * reset defaults (0x3E / 0x89) are safe and usually correct, so
	 * these are only needed when a board's downstream HAL set
	 * different values (e.g. a stronger or weaker LRA). Skip if zero.
	 */
	if (!device_property_read_u32(dev, "ti,rated-voltage-reg", &val))
		h->rated_volt_raw = val;
	if (!device_property_read_u32(dev, "ti,od-clamp-reg", &val))
		h->od_clamp_raw = val;

	/*
	 * Optional per-unit factory-calibrated open-loop LRA period (raw
	 * 9-bit register value, ~24.39 us/tick). Overrides the formula
	 * derived from ti,lra-frequency-hz. On Pixel sunfish this comes
	 * from /persist/haptics/drv2624.cal "lra_period: 241".
	 */
	device_property_read_u32(dev, "ti,ol-lra-period", &h->ol_lra_period);

	/*
	 * Optional factory autocal compensation. The board's per-device
	 * calibration file (e.g. /persist/haptics/drv2624.cal on Pixel
	 * sunfish) carries an "autocal: X Y" line; pass those two bytes
	 * (A_CAL_COMP, A_CAL_BEMF) via DT as ti,autocal-comp.
	 */
	if (!device_property_read_u8_array(dev, "ti,autocal-comp",
					   h->autocal, sizeof(h->autocal)))
		h->autocal_present = true;

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

	/*
	 * The input device is registered non-devm and unregistered
	 * explicitly in remove() so that its close() callback (which talks
	 * I2C and re-parks the chip) runs while the chip is still powered,
	 * and so ff-memless cannot schedule work after teardown.
	 */
	h->input_dev = input_allocate_device();
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
		goto err_free_input;

	error = input_register_device(h->input_dev);
	if (error)
		goto err_free_input;

	return 0;

err_free_input:
	input_free_device(h->input_dev);
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

	/*
	 * Unregister first (runs close() while the chip is still powered and
	 * stops ff-memless from scheduling more work), then flush any pending
	 * work, then drop power.
	 */
	input_unregister_device(h->input_dev);
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

	/* No software standby bit exists; stop playback and power down. */
	regmap_write(h->regmap, DRV2624_REG_GO, 0);
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

	/*
	 * Dropping the enable line resets the chip's registers, so re-run
	 * the full init sequence on resume to restore the LRA configuration
	 * and calibration.
	 */
	return drv2624_hw_init(h);
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
MODULE_AUTHOR("miromraz <mraz.miro@seznam.cz>");
MODULE_LICENSE("GPL");
