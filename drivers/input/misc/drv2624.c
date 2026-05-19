// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments DRV2624 haptic driver
 *
 * Copyright (c) 2026
 *
 * Based on drv260x.c by Dan Murphy <dmurphy@ti.com>.
 *
 * DRV2624 is the successor of the DRV260x family. Compared to DRV260x,
 * it adds an internal 1 kB RAM with a per-effect pointer table fed
 * through a Waveform Sequencer; a separate START/STOP register; and a
 * different mode encoding.
 *
 * This driver implements two play paths driven from the input
 * force-feedback (FF_RUMBLE) framework:
 *
 *   - Short rumbles (<DRV2624_SHORT_PLAY_MS): the chip is parked in
 *     RAM Waveform Sequencer mode with effect 1 (CLICK) pre-selected.
 *     The play callback just toggles the GO bit, the chip plays the
 *     pre-tuned waveform from the loaded firmware blob, and the
 *     onboard auto-brake stops the LRA cleanly. This is what
 *     feedbackd uses for button-press / keyboard events and is the
 *     path that produces stock-Android-style crisp clicks.
 *
 *   - Long rumbles: the chip switches to RTP mode, drives at the
 *     requested amplitude for the requested duration, then switches
 *     back to the Waveform Sequencer park state for the next short
 *     event.
 *
 * The driver loads `drv2624.bin` (TI library format: 20-byte header
 * + RAM image) via request_firmware() at probe; without it short
 * effects degrade to RTP. A `ti,autocal-comp` byte array DT property
 * can carry the device's factory autocal compensation; if absent the
 * chip's internal defaults are used.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
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
#define   DRV2624_MODE_STANDBY		BIT(6)
#define   DRV2624_MODE_MASK		GENMASK(1, 0)
#define   DRV2624_MODE_RAM_PLAYBACK	0x00
#define   DRV2624_MODE_RAM_WAVE_SEQ	0x01
#define   DRV2624_MODE_RTP		0x02
#define   DRV2624_MODE_DIAGNOSTICS	0x03
#define DRV2624_REG_CONTROL1		0x08
#define   DRV2624_CTRL1_LRA		BIT(7)
#define   DRV2624_CTRL1_AUTO_BRK_OL	BIT(3)
#define   DRV2624_CTRL1_AUTO_BRK_INTO_STBY  BIT(2)
#define DRV2624_REG_GO			0x0C
#define   DRV2624_GO_BIT		BIT(0)
#define DRV2624_REG_CONTROL2		0x0D
#define   DRV2624_CTRL2_LIB_LRA		BIT(7)
#define   DRV2624_CTRL2_INTERVAL_1MS	BIT(5)
#define   DRV2624_CTRL2_STOP_BIT	BIT(0)
#define DRV2624_REG_RTP_INPUT		0x0E
#define DRV2624_REG_WAV_FRM_SEQ1	0x0F
#define DRV2624_REG_WAV_FRM_SEQ2	0x10
#define DRV2624_REG_WAV_SEQ_LOOP1	0x17
#define DRV2624_REG_RATED_VOLT		0x1F
#define DRV2624_REG_OD_CLAMP		0x20
#define DRV2624_REG_AUTOCAL_COMP	0x21	/* compensation result, 3 bytes 0x21..0x23 */
#define DRV2624_REG_DRIVE_TIME		0x27
#define DRV2624_REG_BLANKING_IDISS	0x28
#define DRV2624_REG_ZC_DET_TIME		0x29
#define DRV2624_REG_LRA_WAVE_SHAPE	0x2C
#define   DRV2624_LRA_WAVE_SINE		BIT(0)
#define DRV2624_REG_OL_LRA_PERIOD_H	0x2E
#define DRV2624_REG_OL_LRA_PERIOD_L	0x2F
#define DRV2624_REG_MAX			0x30

/* RAM access (auto-increment on RAM_DATA writes) */
#define DRV2624_REG_RAM_ADDR_UPPER	0xFD
#define DRV2624_REG_RAM_ADDR_LOWER	0xFE
#define DRV2624_REG_RAM_DATA		0xFF

#define DRV2624_CHIP_ID_VAL		0x03

/* Firmware (drv2624.bin) header layout, little-endian. */
#define DRV2624_FW_MAGIC		0x2624
#define DRV2624_FW_HEADER_SIZE		20

/* Default rated/overdrive voltages for a generic LRA (Vrms) */
#define DRV2624_DEF_RATED_MV		2100	/* ~2.1V rms */
#define DRV2624_DEF_OD_MV		3000	/* ~3.0V peak */

/* Default LRA resonant frequency, Hz */
#define DRV2624_DEF_LRA_HZ		205

/*
 * Below this requested rumble length we use ROM CLICK from the loaded
 * waveform library instead of RTP — gives crisp single-impulse taps.
 * Anything longer falls back to RTP for sustained vibration.
 */
#define DRV2624_SHORT_PLAY_MS		100

/* Pre-set ROM effect IDs in TI/Pixel's drv2624.bin library */
#define DRV2624_ROM_EFFECT_CLICK	1
#define DRV2624_ROM_EFFECT_TICK		2
#define DRV2624_ROM_EFFECT_DCLICK	3
#define DRV2624_ROM_EFFECT_HEAVY	4

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
	u32 ol_lra_period;	/* DT-supplied per-unit factory cal; 0 = derive from freq */
	u8 autocal[3];
	bool autocal_present;

	const u8 *fw_ram;
	size_t fw_ram_size;

	u8 magnitude;
	u32 replay_length;
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

/*
 * Park the chip in RAM Waveform Sequencer mode with effect 1 (CLICK)
 * pre-selected. With the chip parked this way, the worker's GO write
 * triggers the ROM CLICK from the loaded firmware library — that's
 * the path that feels like stock Android, vs. raw RTP which sounds
 * buzzy on a narrow-band LRA.
 */
static int drv2624_park_seq(struct drv2624_data *h, u8 effect_id)
{
	int error;

	error = regmap_write(h->regmap, DRV2624_REG_WAV_FRM_SEQ1, effect_id);
	if (error)
		return error;
	error = regmap_write(h->regmap, DRV2624_REG_WAV_FRM_SEQ2, 0);
	if (error)
		return error;
	error = regmap_write(h->regmap, DRV2624_REG_WAV_SEQ_LOOP1, 0);
	if (error)
		return error;
	return regmap_write(h->regmap, DRV2624_REG_MODE,
			    DRV2624_MODE_RAM_WAVE_SEQ);
}

static void drv2624_worker(struct work_struct *work)
{
	struct drv2624_data *h = container_of(work, struct drv2624_data, work);
	struct device *dev = &h->client->dev;
	int error;

	/* Magnitude 0 means stop; the FF memless layer calls this at end-of-effect. */
	if (!h->magnitude) {
		/*
		 * Pulse the STOP bit only — full writes here clear LIB_LRA and
		 * route subsequent ROM playback through the ERM library, which
		 * sounds buzzy on an LRA.
		 */
		regmap_update_bits(h->regmap, DRV2624_REG_CONTROL2,
				   DRV2624_CTRL2_STOP_BIT,
				   DRV2624_CTRL2_STOP_BIT);
		if (h->fw_ram_size)
			drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
		return;
	}

	/*
	 * Short effect → trigger whatever's already parked. The chip will
	 * be in WAV_SEQ + WAV_FRM_SEQ1=CLICK if either (a) the driver's
	 * own firmware load callback succeeded, or (b) userspace put it
	 * there out-of-band (e.g. /etc/local.d init script while the
	 * driver's request_firmware path is being debugged). Either way,
	 * GO triggers playback of whatever's currently selected.
	 */
	if (h->replay_length <= DRV2624_SHORT_PLAY_MS) {
		unsigned int mode = 0;

		regmap_read(h->regmap, DRV2624_REG_MODE, &mode);
		if ((mode & DRV2624_MODE_MASK) == DRV2624_MODE_RAM_WAVE_SEQ) {
			error = regmap_write(h->regmap, DRV2624_REG_GO,
					     DRV2624_GO_BIT);
			if (error)
				dev_err(dev, "GO write failed: %d\n", error);
			return;
		}
		/* Chip not parked in SEQ — fall through to RTP path. */
	}

	/*
	 * Long effect (or chip in RTP mode) → RTP path. End-of-effect
	 * (magnitude=0 above) re-parks in WAV_SEQ if firmware is loaded.
	 */
	error = regmap_write(h->regmap, DRV2624_REG_MODE, DRV2624_MODE_RTP);
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
	h->replay_length = effect->replay.length;

	schedule_work(&h->work);
	return 0;
}

static void drv2624_close(struct input_dev *input)
{
	struct drv2624_data *h = input_get_drvdata(input);

	cancel_work_sync(&h->work);
	regmap_write(h->regmap, DRV2624_REG_CONTROL2,
		     DRV2624_CTRL2_INTERVAL_1MS | DRV2624_CTRL2_STOP_BIT);
	if (h->fw_ram_size)
		drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
}

/*
 * Async firmware-loaded callback. The TI library blob (drv2624.bin)
 * is a 20-byte header (magic 0x2624, fw_size, build date, checksum,
 * effect count) followed by the RAM image — a per-effect pointer
 * table + voltage-time sample pairs. The chip's playback engine
 * resolves the layout itself; we just bulk-write the post-header
 * bytes into RAM starting at address 0, then park the chip in
 * WAV_SEQ mode with effect 1 (CLICK) selected so future GO triggers
 * play the ROM CLICK.
 */
static void drv2624_fw_loaded(const struct firmware *fw, void *context)
{
	struct drv2624_data *h = context;
	struct device *dev = &h->client->dev;
	int error, i;
	u32 magic;

	if (!fw) {
		dev_info(dev, "no drv2624.bin; ROM effects unavailable, RTP-only\n");
		return;
	}
	if (fw->size <= DRV2624_FW_HEADER_SIZE) {
		dev_warn(dev, "drv2624.bin truncated (%zu bytes)\n", fw->size);
		goto out_release;
	}
	magic = le32_to_cpu(*(const __le32 *)fw->data);
	if ((magic & 0xFFFF) != DRV2624_FW_MAGIC) {
		dev_warn(dev, "drv2624.bin bad magic 0x%08x\n", magic);
		goto out_release;
	}

	error = regmap_write(h->regmap, DRV2624_REG_RAM_ADDR_UPPER, 0);
	if (error)
		goto out_release;
	error = regmap_write(h->regmap, DRV2624_REG_RAM_ADDR_LOWER, 0);
	if (error)
		goto out_release;
	for (i = DRV2624_FW_HEADER_SIZE; i < fw->size; i++) {
		error = regmap_write(h->regmap, DRV2624_REG_RAM_DATA, fw->data[i]);
		if (error) {
			dev_warn(dev, "RAM upload failed at byte %d: %d\n", i, error);
			goto out_release;
		}
	}
	h->fw_ram_size = fw->size - DRV2624_FW_HEADER_SIZE;
	dev_info(dev, "drv2624.bin uploaded (%zu byte RAM image)\n", h->fw_ram_size);

	/*
	 * Park chip in WAV_SEQ + CLICK now that the library is loaded.
	 * From here, drv2624_worker just toggles GO for short rumbles
	 * and the chip plays ROM CLICK.
	 */
	if (drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK))
		dev_warn(dev, "failed to park chip in WAV_SEQ\n");

out_release:
	release_firmware(fw);
}

static int drv2624_upload_firmware(struct drv2624_data *h)
{
	struct device *dev = &h->client->dev;
	int error;

	/*
	 * Async load: probe doesn't block on the user-mode firmware helper.
	 * On success the callback uploads to chip RAM and parks in WAV_SEQ.
	 * On failure the kernel firmware loader prints "Direct firmware load
	 * for drv2624.bin failed with error -N" automatically and the chip
	 * stays in the RTP-only fallback from hw_init.
	 */
	error = request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
					"drv2624.bin", dev, GFP_KERNEL,
					h, drv2624_fw_loaded);
	if (error)
		dev_warn(dev, "request_firmware_nowait failed: %d\n", error);
	return 0;
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

	/*
	 * Take the chip into standby while we program it. The standby bit
	 * blocks playback; we exit it at the end after parking in WAV_SEQ.
	 */
	regmap_write(h->regmap, DRV2624_REG_MODE,
		     DRV2624_MODE_STANDBY | DRV2624_MODE_RAM_WAVE_SEQ);

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
	 * CONTROL2: select LRA library + 1 ms playback interval. The default
	 * 5 ms tick stretches every ROM effect 5× too long. (The LIB_LRA bit
	 * is chip-side mode-dependent and may read back 0 on some revisions —
	 * we set it anyway; the INTERVAL bit is what actually changes feel.)
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL2,
				   DRV2624_CTRL2_LIB_LRA | DRV2624_CTRL2_INTERVAL_1MS,
				   DRV2624_CTRL2_LIB_LRA | DRV2624_CTRL2_INTERVAL_1MS);
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

	/*
	 * Apply factory autocal compensation if present. Pixel devices
	 * ship these three bytes per-unit in /persist/haptics/drv2624.cal;
	 * userspace (or DT, via the optional ti,autocal-comp property)
	 * passes them to the driver. Without them the chip falls back to
	 * its internal calibration defaults — usable but less tuned.
	 */
	if (h->autocal_present) {
		regmap_write(h->regmap, DRV2624_REG_AUTOCAL_COMP + 0, h->autocal[0]);
		regmap_write(h->regmap, DRV2624_REG_AUTOCAL_COMP + 1, h->autocal[1]);
		regmap_write(h->regmap, DRV2624_REG_AUTOCAL_COMP + 2, h->autocal[2]);
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
	 * tracker locks on resonance. Register unit is 24.39 us (datasheet);
	 * period_ticks ≈ 41000 / f_Hz, and the field is 9 bits wide
	 * (bit 8 in PERIOD_H, bits 7:0 in PERIOD_L). The DT prop
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
			period = min_t(u32, period, 0x1FF);
			regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_H,
				     (period >> 8) & 0x01);
			regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_L,
				     period & 0xFF);
		}
	}

	/* Upload the RAM waveform library (drv2624.bin). */
	drv2624_upload_firmware(h);

	/*
	 * Park in WAV_SEQ with effect 1 (CLICK) selected. This is the
	 * persistent state — the play() callback just toggles GO for
	 * short effects, RTP gets switched in and back out for long ones.
	 */
	if (h->fw_ram_size) {
		error = drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
		if (error)
			return error;
	} else {
		/* No firmware → default to RTP mode out of standby. */
		error = regmap_write(h->regmap, DRV2624_REG_MODE, DRV2624_MODE_RTP);
		if (error)
			return error;
	}

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
	 * sunfish) carries an "autocal: X Y Z" line; pass those three
	 * bytes via DT as ti,autocal-comp.
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

	/*
	 * Dropping the enable line wipes the chip's RAM (per datasheet —
	 * RAM is volatile across the ULP/standby gate). Re-run the full
	 * init sequence on resume so the RAM library, calibration, and
	 * WAV_SEQ park state are all restored.
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
MODULE_LICENSE("GPL");
