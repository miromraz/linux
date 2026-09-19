// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments DRV2624 haptic driver
 *
 * Copyright (c) 2026 miromraz <mraz.miro@seznam.cz>
 *
 * Based on drv260x.c by Dan Murphy <dmurphy@ti.com>.
 *
 * DRV2624 is the successor of the DRV260x family. Compared to DRV260x
 * it adds an internal RAM with a per-effect pointer table fed through a
 * Waveform Sequencer, a separate GO register, and a different mode
 * encoding.
 *
 * The input force-feedback layer (ff-memless) drives FF_RUMBLE with a
 * magnitude only: play(magnitude>0) starts an effect and play(0) is
 * called when its duration elapses. It never tells the driver the
 * duration at play() time (replay.length in the combined effect is
 * always 0), so the driver cannot decide up front whether a request is
 * a short tap or a long rumble.
 *
 * This driver plays a hybrid that gets both right without knowing the
 * duration in advance:
 *
 *   - Every start fires a synthesised one-period (~6 ms) waveform from
 *     the chip's RAM library through the Waveform Sequencer, with
 *     open-loop auto-brake. That single braked impulse reads as a crisp
 *     tap rather than a buzz, and is what stock Android does. A 15 ms
 *     keyboard/theme tap, which is only ~2.5 motor cycles of RTP drive
 *     and feels like a buzz, thus stays a pure click.
 *
 *   - If the effect is still running a short time after the click
 *     (DRV2624_CLICK_TO_RTP_MS), the driver hands over to real-time
 *     playback (RTP) at the requested magnitude and drives until
 *     play(0) arrives, giving sustained rumbles their full length.
 *
 * The driver synthesises its waveform library at probe from the
 * actuator's resonant frequency (ti,lra-frequency-hz sets the click
 * length) and uploads it to chip RAM, so there is no firmware blob to
 * ship or load.
 * A `ti,autocal-comp` byte-array DT property can carry the device's
 * factory autocal compensation; if absent the chip's internal
 * defaults are used.
 *
 * The register tuning mirrors what stock Android runs on the Pixel 4a
 * (sunfish), read from the stock dtbo and the Pixel haptics HAL: always
 * open loop; the click is a sine wave at the ~155 Hz open-loop period
 * played from RAM at 100 % gain; the long RTP buzz is a square wave at
 * the ~145 Hz open-loop period; RATED_VOLTAGE 109 / OD_CLAMP 161; brake
 * factor 1x and BEMF gain 0. The per-board numbers come from DT.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#define DRV2624_REG_CHIP_ID		0x00
#define DRV2624_REG_STATUS		0x01
#define DRV2624_REG_LRA_PERIOD_H	0x05	/* measured closed-loop period */
#define DRV2624_REG_LRA_PERIOD_L	0x06
#define DRV2624_REG_MODE		0x07
#define   DRV2624_MODE_MASK		GENMASK(1, 0)
#define   DRV2624_TRIG_PIN_FUNC_MASK	GENMASK(3, 2)	/* Table 8-10 */
/* MODE[1:0], datasheet Table 8-10: 0 RTP, 1 waveform sequencer, 2 diag, 3 autocal */
#define   DRV2624_MODE_RTP		0x00
#define   DRV2624_MODE_RAM_WAVE_SEQ	0x01
#define DRV2624_REG_CONTROL1		0x08
#define   DRV2624_CTRL1_LRA		BIT(7)	/* LRA_ERM, Table 8-11 */
#define   DRV2624_CTRL1_OPEN_LOOP	BIT(6)	/* CONTROL_LOOP: 1 = open loop */
#define   DRV2624_CTRL1_AUTO_BRK_OL	BIT(4)
#define   DRV2624_CTRL1_AUTO_BRK_INTO_STBY  BIT(3)
#define DRV2624_REG_GO			0x0C
#define   DRV2624_GO_BIT		BIT(0)
#define DRV2624_REG_CONTROL2		0x0D
#define   DRV2624_CTRL2_INTERVAL_1MS	BIT(5)		/* PLAYBACK_INTERVAL, Table 8-17 */
#define   DRV2624_CTRL2_DIG_MEM_GAIN_MASK  GENMASK(1, 0)	/* Table 8-17 */
#define DRV2624_REG_RTP_INPUT		0x0E
#define DRV2624_REG_WAV_FRM_SEQ1	0x0F
#define DRV2624_REG_WAV_FRM_SEQ2	0x10
#define DRV2624_REG_WAV_SEQ_LOOP1	0x17
#define DRV2624_REG_RATED_VOLT		0x1F
#define DRV2624_REG_OD_CLAMP		0x20
#define DRV2624_REG_AUTOCAL_COMP	0x21	/* A_CAL_COMP + A_CAL_BEMF, 0x21/0x22 */
#define DRV2624_REG_LOOP_CONTROL	0x23	/* NG/FB_BRAKE/LOOP_GAIN/BEMF_GAIN */
#define   DRV2624_FB_BRAKE_FACTOR_MASK	GENMASK(6, 4)
#define   DRV2624_BEMF_GAIN_MASK	GENMASK(1, 0)
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

#define DRV2624_CHIP_ID_MASK		GENMASK(7, 4)	/* CHIPID[3:0], Table 8-2 */
#define DRV2624_CHIP_ID_VAL		0x00		/* DRV2624 */

/* Default LRA resonant frequency, Hz */
#define DRV2624_DEF_LRA_HZ		205

/*
 * Waveform library layout, per the DRV2624 datasheet (SLOS893D section
 * 7.6.9.2 "Loading Data to RAM"):
 *
 *   byte 0		revision, must be 0
 *   bytes 1..3N	N header entries of 3 bytes each: the effect's start
 *			address (upper byte, lower byte) followed by a
 *			configuration byte holding WAVEFORM_REPEATS[2:0] in
 *			bits [7:5] and the effect size[4:0] in bits [4:0]
 *			(size in bytes, even, 2..30; Fig 7-16).
 *			An entry's position in the header is its effect ID,
 *			numbered from 1.
 *   then		the waveform data: interleaved voltage/time pairs.
 *			Voltage is 7-bit signed, full scale 63, and its MSB
 *			is the linear-ramp flag. Time is a tick count; a
 *			tick is 1 ms because drv2624_hw_init() sets
 *			PLAYBACK_INTERVAL.
 *
 * We build exactly one effect, ID 1, because that is the only one the
 * input FF_RUMBLE path can select — FF_RUMBLE carries a magnitude and a
 * duration, not an effect name, so there is no way for userspace to ask
 * for a second library entry.
 */
#define DRV2624_ROM_HEADER_ENTRIES	1
#define DRV2624_ROM_DATA_START		(1 + 3 * DRV2624_ROM_HEADER_ENTRIES)
#define DRV2624_ROM_LIB_SIZE		(DRV2624_ROM_DATA_START + 2)

/* Voltage field is 7-bit signed; bit 7 is the ramp flag, not magnitude. */
#define DRV2624_AMP_FULL_SCALE		63

/* Effect ID of the click we build in drv2624_upload_rom(). */
#define DRV2624_ROM_EFFECT_CLICK	1

/*
 * How long after the RAM click starts we hand a still-running effect
 * over to RTP. A ~6 ms click plus this margin means a 15 ms theme tap
 * has already ended (play(0) has cleared the magnitude) and stays a pure
 * braked click, while any effect lasting >= 40 ms gets the crisp click
 * as an attack and then RTP sustain until play(0).
 */
#define DRV2624_CLICK_TO_RTP_MS	40

enum drv2624_actuator {
	DRV2624_ACTUATOR_LRA,
	DRV2624_ACTUATOR_ERM,
};

struct drv2624_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regmap *regmap;
	struct work_struct work;
	struct delayed_work handover;

	struct gpio_desc *enable_gpio;
	struct regulator *vdd;

	enum drv2624_actuator actuator;
	u32 lra_freq_hz;
	u32 ol_lra_period;	/* DT-supplied per-unit factory cal; 0 = derive from freq */
	u32 rtp_ol_lra_period;	/* open-loop period used for RTP; 0 = use click period */
	u32 ol_period_click;	/* open-loop period programmed for the click */
	u8 rated_volt_raw;	/* raw register value; 0 = leave at chip default */
	u8 od_clamp_raw;	/* raw register value; 0 = leave at chip default */
	u8 fb_brake_factor;	/* CONTROL 0x23[6:4]; chip reset default 3 */
	u8 bemf_gain;		/* CONTROL 0x23[1:0]; chip reset default 2 */
	u8 autocal[2];
	bool autocal_present;

	u8 magnitude;
	u8 gain;		/* DIG_MEM_GAIN value currently programmed */
	bool parked;		/* chip is in Waveform Sequencer park state */
	bool rtp_wave;		/* chip has RTP square wave + RTP period loaded */
};

static const struct regmap_config drv2624_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	/*
	 * RAM access registers (0xFD/0xFE/0xFF) live above the normal
	 * control-register window, so the regmap window has to extend
	 * to 0xFF — otherwise the ROM upload regmap_writes get rejected
	 * as out-of-range and probe fails with -EIO.
	 */
	.max_register = DRV2624_REG_RAM_DATA,
};

/*
 * Park the chip in RAM Waveform Sequencer mode with effect 1 (CLICK)
 * pre-selected. With the chip parked this way, a GO write triggers the
 * ROM CLICK from the uploaded library — the path that feels like stock
 * Android, vs. raw RTP which sounds buzzy on a narrow-band LRA. MODE is
 * written with update_bits so TRIG_PIN_FUNC (bits 3:2) stays 0.
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
	return regmap_update_bits(h->regmap, DRV2624_REG_MODE,
				  DRV2624_MODE_MASK, DRV2624_MODE_RAM_WAVE_SEQ);
}

/* Write the 10-bit OL_LRA_PERIOD (bits [9:8] in PERIOD_H, [7:0] in PERIOD_L). */
static int drv2624_write_ol_period(struct drv2624_data *h, u32 period)
{
	int error;

	period = min_t(u32, period, 0x3FF);
	error = regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_H,
			     (period >> 8) & 0x03);
	if (error)
		return error;
	return regmap_write(h->regmap, DRV2624_REG_OL_LRA_PERIOD_L,
			    period & 0xFF);
}

/*
 * Map the FF magnitude to DIG_MEM_GAIN (Table 8-17): 0=100% .. 3=25%.
 *
 * Stock Android runs the click open loop with a 1x brake factor and plays
 * the RAM click at 100 % gain. An earlier "50 % is best" measurement was
 * made in CLOSED loop with a 4x brake factor (both since dropped to match
 * stock), so it no longer applies and is superseded. Normal taps therefore
 * play at 100 %; only clearly light requests drop to 50 %. ff-memless hands
 * the driver about 3/4 of the requested magnitude (a 0xffff request arrives
 * as ~96 after >> 9), so a 0.5 theme value arrives as ~48; the threshold is
 * on the value that actually arrives.
 */
static u8 drv2624_mag_to_gain(u8 mag)
{
	if (mag >= 48)
		return 0;	/* 100 %: stock full-scale click */
	return 2;		/* 50 % */
}

/*
 * Main work, scheduled from play() (which runs in atomic context and
 * cannot do I2C). On a non-zero magnitude it fires the braked RAM click
 * and arms the RTP handover; on magnitude 0 it stops and re-parks.
 */
static void drv2624_worker(struct work_struct *work)
{
	struct drv2624_data *h = container_of(work, struct drv2624_data, work);
	struct device *dev = &h->client->dev;
	u8 mag = h->magnitude;
	u8 gain;
	int error;

	if (!mag) {
		/*
		 * Stop. Wait out a handover that may already be running on
		 * another CPU (it never waits on this work, so this cannot
		 * deadlock) -- otherwise it could re-arm GO right after we
		 * clear it and leave the motor running. Then brake and re-park
		 * so the next tap fires a click.
		 */
		cancel_delayed_work_sync(&h->handover);
		error = regmap_write(h->regmap, DRV2624_REG_GO, 0);
		if (error)
			dev_err(dev, "GO clear failed: %d\n", error);
		/*
		 * If the RTP path swapped in the square wave + RTP period,
		 * restore the click's sine wave + open-loop period so the next
		 * tap fires the stock-matching click.
		 */
		if (h->rtp_wave) {
			regmap_update_bits(h->regmap, DRV2624_REG_LRA_WAVE_SHAPE,
					   DRV2624_LRA_WAVE_SINE,
					   DRV2624_LRA_WAVE_SINE);
			drv2624_write_ol_period(h, h->ol_period_click);
			h->rtp_wave = false;
		}
		error = drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
		if (error)
			dev_err(dev, "re-park failed: %d\n", error);
		else
			h->parked = true;
		return;
	}

	/*
	 * (a) DIG_MEM_GAIN scales library (click) playback only; it is
	 * ignored in RTP. Write it only when it changed.
	 */
	gain = drv2624_mag_to_gain(mag);
	if (gain != h->gain) {
		error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL2,
					   DRV2624_CTRL2_DIG_MEM_GAIN_MASK, gain);
		if (error) {
			dev_err(dev, "DIG_MEM_GAIN write failed: %d\n", error);
			return;
		}
		h->gain = gain;
	}

	/*
	 * (a2) if a prior RTP burst left the square wave + RTP period, put the
	 * click's sine wave + open-loop period back before firing (guarded by
	 * the flag so a plain click that follows another click writes nothing).
	 */
	if (h->rtp_wave) {
		error = regmap_update_bits(h->regmap, DRV2624_REG_LRA_WAVE_SHAPE,
					   DRV2624_LRA_WAVE_SINE,
					   DRV2624_LRA_WAVE_SINE);
		if (error) {
			dev_err(dev, "wave-shape restore failed: %d\n", error);
			return;
		}
		error = drv2624_write_ol_period(h, h->ol_period_click);
		if (error) {
			dev_err(dev, "OL period restore failed: %d\n", error);
			return;
		}
		h->rtp_wave = false;
	}

	/* (b) make sure the sequencer is parked so GO fires the RAM click. */
	if (!h->parked) {
		error = drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
		if (error) {
			dev_err(dev, "park failed: %d\n", error);
			return;
		}
		h->parked = true;
	}

	/* (c) fire the braked one-period click out of RAM. */
	error = regmap_write(h->regmap, DRV2624_REG_GO, DRV2624_GO_BIT);
	if (error) {
		dev_err(dev, "GO write failed: %d\n", error);
		return;
	}

	/* (d) if the effect outlives the click, hand over to RTP. */
	mod_delayed_work(system_dfl_wq, &h->handover,
			 msecs_to_jiffies(DRV2624_CLICK_TO_RTP_MS));
}

/*
 * Delayed handover: if the effect is still running after the click
 * window, switch to RTP so it sustains at the requested magnitude until
 * play(0). If play(0) already fired, magnitude is 0 and we do nothing.
 */
static void drv2624_handover(struct work_struct *work)
{
	struct drv2624_data *h = container_of(to_delayed_work(work),
					      struct drv2624_data, handover);
	struct device *dev = &h->client->dev;
	u8 mag = h->magnitude;
	int error;

	if (!mag)
		return;

	/*
	 * Stock drives the long RTP buzz with a square wave at the RTP
	 * open-loop period (145 Hz on sunfish), distinct from the sine-wave
	 * click. Swap those in before switching to RTP; the click path (or
	 * stop) restores the sine wave + click period afterwards.
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_LRA_WAVE_SHAPE,
				   DRV2624_LRA_WAVE_SINE, 0);
	if (error) {
		dev_err(dev, "RTP wave-shape write failed: %d\n", error);
		return;
	}
	error = drv2624_write_ol_period(h, h->rtp_ol_lra_period ?
					h->rtp_ol_lra_period : h->ol_period_click);
	if (error) {
		dev_err(dev, "RTP OL period write failed: %d\n", error);
		return;
	}
	h->rtp_wave = true;

	error = regmap_update_bits(h->regmap, DRV2624_REG_MODE,
				   DRV2624_MODE_MASK, DRV2624_MODE_RTP);
	if (error) {
		dev_err(dev, "RTP mode write failed: %d\n", error);
		return;
	}
	h->parked = false;
	error = regmap_write(h->regmap, DRV2624_REG_RTP_INPUT, mag);
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
	cancel_delayed_work_sync(&h->handover);
	/* Stop any active playback by clearing GO (datasheet Table 8-15). */
	regmap_write(h->regmap, DRV2624_REG_GO, 0);
	if (!drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK))
		h->parked = true;
}

/*
 * Build the single-click library and upload it synchronously to chip RAM
 * starting at address 0. The chip's playback engine walks the header
 * itself, so we only have to lay the bytes out in the documented order.
 *
 * Synchronous upload from probe (rather than request_firmware_nowait)
 * removes the chip-state race where a haptic event arrives before an
 * async callback finishes and leaves the chip stuck in RTP mode.
 */
static int drv2624_upload_rom(struct drv2624_data *h)
{
	u8 lib[DRV2624_ROM_LIB_SIZE];
	unsigned int click_ms, freq_hz;
	int error;
	size_t i;

	/*
	 * Drive for one resonant period, rounded to the 1 ms playback
	 * tick: long enough for the LRA to reach peak displacement, short
	 * enough that AUTO_BRK_OL starts braking before the next cycle
	 * accelerates it again. That single-period impulse is what makes a
	 * click read as a tap rather than a buzz. A 172 Hz actuator (Pixel
	 * sunfish) gives 6 ms; the 205 Hz default gives 5 ms.
	 *
	 * freq_hz is re-checked here because ti,lra-frequency-hz comes from
	 * DT and a 0 would divide by zero.
	 */
	freq_hz = h->lra_freq_hz ? h->lra_freq_hz : DRV2624_DEF_LRA_HZ;
	click_ms = clamp_t(unsigned int, DIV_ROUND_CLOSEST(1000, freq_hz), 1, 255);

	lib[0] = 0;				/* revision */
	lib[1] = DRV2624_ROM_DATA_START >> 8;	/* effect 1 start, upper */
	lib[2] = DRV2624_ROM_DATA_START & 0xFF;	/* effect 1 start, lower */
	lib[3] = 2;				/* no repeats, 2 data bytes */
	lib[4] = DRV2624_AMP_FULL_SCALE;	/* voltage */
	lib[5] = click_ms;			/* time, in 1 ms ticks */

	error = regmap_write(h->regmap, DRV2624_REG_RAM_ADDR_UPPER, 0);
	if (error)
		return error;
	error = regmap_write(h->regmap, DRV2624_REG_RAM_ADDR_LOWER, 0);
	if (error)
		return error;
	for (i = 0; i < ARRAY_SIZE(lib); i++) {
		error = regmap_write(h->regmap, DRV2624_REG_RAM_DATA, lib[i]);
		if (error)
			return error;
	}
	return 0;
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
	 * TRIG_PIN_FUNC resets to 1 = external level trigger, in which mode
	 * "the GO bit cannot be used" (SLOS893D Table 8-10). Nothing drives
	 * the TRIG/INTZ pin on this board, so select the pulse-trigger function
	 * (0), where GO starts and stops playback. Measured: with the reset
	 * value every RTP request left the actuator silent.
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_MODE,
				   DRV2624_TRIG_PIN_FUNC_MASK, 0);
	if (error)
		return error;

	/*
	 * CONTROL1: set actuator type (LRA), run open loop (stock sunfish
	 * always drives open loop, "ctrl_loop=1"), enable open-loop auto-brake
	 * (a brake waveform played at end of drive) and auto-brake into standby
	 * (decelerate the LRA when GO returns to 0 rather than cutting drive
	 * cold and leaving the actuator to ring down at its natural Q). Without
	 * AUTO_BRK_INTO_STBY a typical phone LRA rings for ~700 ms after the
	 * kernel ends an effect, which is felt as a long buzz instead of a
	 * crisp tap. Effective byte 0xD8 (bit3 AUTO_BRK_INTO_STBY resets to 1).
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL1,
				   DRV2624_CTRL1_LRA |
				   DRV2624_CTRL1_OPEN_LOOP |
				   DRV2624_CTRL1_AUTO_BRK_OL |
				   DRV2624_CTRL1_AUTO_BRK_INTO_STBY,
				   (h->actuator == DRV2624_ACTUATOR_LRA ?
					DRV2624_CTRL1_LRA : 0) |
				   DRV2624_CTRL1_OPEN_LOOP |
				   DRV2624_CTRL1_AUTO_BRK_OL |
				   DRV2624_CTRL1_AUTO_BRK_INTO_STBY);
	if (error)
		return error;

	/*
	 * CONTROL2: select the 1 ms playback interval. The default 5 ms tick
	 * stretches every ROM effect 5x too long. DIG_MEM_GAIN[1:0] is left
	 * at reset (0 = 100 %) here and driven per-effect from the magnitude
	 * in the worker; h->gain tracks it and is reset to match.
	 */
	error = regmap_update_bits(h->regmap, DRV2624_REG_CONTROL2,
				   DRV2624_CTRL2_INTERVAL_1MS,
				   DRV2624_CTRL2_INTERVAL_1MS);
	if (error)
		return error;
	h->gain = 0;

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

	/*
	 * LOOP_CONTROL (0x23): FB_BRAKE_FACTOR[6:4] and BEMF_GAIN[1:0] from DT
	 * (chip reset defaults 3 and 2). LOOP_GAIN[3:2] and NG_THRESH[7] are
	 * left untouched via the mask. Stock sunfish sets both to 0 (1x brake,
	 * no BEMF gain), giving 0x04 with LOOP_GAIN at its reset value 1.
	 */
	regmap_update_bits(h->regmap, DRV2624_REG_LOOP_CONTROL,
			   DRV2624_FB_BRAKE_FACTOR_MASK | DRV2624_BEMF_GAIN_MASK,
			   (h->fb_brake_factor << 4) | h->bemf_gain);

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
			drv2624_write_ol_period(h, period);
			h->ol_period_click = period;
		}
	}
	/* Init leaves the click's sine wave + period programmed. */
	h->rtp_wave = false;

	/* Build and upload the waveform library to chip RAM. */
	error = drv2624_upload_rom(h);
	if (error)
		return error;

	/*
	 * Park in WAV_SEQ with effect 1 (CLICK) selected. From here every
	 * play() fires the click; the worker switches RTP in for effects
	 * that outlive the click and re-parks on stop.
	 */
	error = drv2624_park_seq(h, DRV2624_ROM_EFFECT_CLICK);
	if (error)
		return error;
	h->parked = true;

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
	INIT_DELAYED_WORK(&h->handover, drv2624_handover);

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
	 * Open-loop period used for the long RTP buzz (stock sunfish drives it
	 * at a lower frequency than the click). Defaults to the click period.
	 */
	if (device_property_read_u32(dev, "ti,rtp-ol-lra-period",
				     &h->rtp_ol_lra_period))
		h->rtp_ol_lra_period = h->ol_lra_period;

	/*
	 * FB_BRAKE_FACTOR (0x23[6:4]) and BEMF_GAIN (0x23[1:0]); default to the
	 * chip reset values (3 and 2) when absent. Stock sunfish sets both 0.
	 */
	h->fb_brake_factor = 3;
	if (!device_property_read_u32(dev, "ti,fb-brake-factor", &val))
		h->fb_brake_factor = val;
	h->bemf_gain = 2;
	if (!device_property_read_u32(dev, "ti,bemf-gain", &val))
		h->bemf_gain = val;

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
	cancel_delayed_work_sync(&h->handover);

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

	cancel_work_sync(&h->work);
	cancel_delayed_work_sync(&h->handover);

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
MODULE_AUTHOR("miromraz <mraz.miro@seznam.cz>");
MODULE_LICENSE("GPL");
