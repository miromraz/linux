// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Samsung S5K3L6XX 1/3" 13M CMOS Image Sensor.
 *
 * Copyright (C) 2020-2021 Purism SPC
 *
 * Based on S5K5BAF driver
 * Copyright (C) 2013, Samsung Electronics Co., Ltd.
 * Andrzej Hajda <a.hajda@samsung.com>
 *
 * Based on S5K6AA driver authored by Sylwester Nawrocki
 * Copyright (C) 2013, Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gcd.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/pm.h>
#include <linux/slab.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-fwnode.h>

static int debug;
module_param(debug, int, 0644);

#define S5K3L6XX_DRIVER_NAME		"s5k3l6xx"
#define S5K3L6XX_DEFAULT_MCLK_FREQ	24000000U
#define S5K3L6XX_CLK_NAME		"mclk"

#define S5K3L6XX_REG_MODEL_ID_L		0x0000
#define S5K3L6XX_REG_MODEL_ID_H		0x0001
#define S5K3L6XX_MODEL_ID_L		0x30
#define S5K3L6XX_MODEL_ID_H		0xc6

#define S5K3L6XX_REG_REVISION_NUMBER	0x0002
#define S5K3L6XX_REVISION_NUMBER	0xb0

#define S5K3L6XX_REG_LANE_MODE		0x0114
#define S5K3L6XX_REG_COARSE_INTEGRATION_TIME		0x0202 // 2 bytes
#define S5K3L6XX_REG_ANALOG_GAIN		0x0204 // 2 bytes
#define S5K3L6XX_REG_DIGITAL_GAIN		0x020e // 2 bytes

#define S5K3L6XX_REG_FRAME_LENGTH_LINES	0x0340 // 2 bytes
#define S5K3L6XX_REG_LINE_LENGTH_PCK	0x0342 // 2 bytes

#define S5K3L6XX_REG_REQUESTED_LINK_RATE	0x0820 // 4 bytes (fixed point)

#define S5K3L6XX_REG_TEST_PATTERN_MODE	0x0601
#define S5K3L6XX_TEST_PATTERN_SOLID_COLOR	0x01
#define S5K3L6XX_TEST_PATTERN_COLOR_BAR	0x02

#define S5K3L6XX_REG_TEST_DATA_RED	0x0602
#define S5K3L6XX_REG_TEST_DATA_GREENR	0x0604
#define S5K3L6XX_REG_TEST_DATA_BLUE	0x0606
#define S5K3L6XX_REG_TEST_DATA_GREENB	0x0608

#define S5K3L6XX_REG_PLL_PD		0x3c1e

#define S5K3L6XX_REG_MODE_SELECT	0x100
#define S5K3L6XX_MODE_STREAMING		0x1
#define S5K3L6XX_MODE_STANDBY		0x0

#define S5K3L6XX_REG_DATA_FORMAT	0x0112
#define S5K3L6XX_DATA_FORMAT_RAW10	0x0A0A

#define S5K3L6XX_CIS_WIDTH		4208
#define S5K3L6XX_CIS_HEIGHT		3120


static const char * const s5k3l6xx_supply_names[] = {
	"vddio", /* Digital I/O (1.8V or 2.8V) */
	"vdda", /* Analog (2.8V) */
	"vddd", /* Digital Core (1.05V expected) */
};

#define S5K3L6XX_NUM_SUPPLIES ARRAY_SIZE(s5k3l6xx_supply_names)


struct s5k3l6xx_reg {
	u16 address;
	u16 val;
	// Size of a single write.
	u8 size;
};

/*
 * a52q vendor tables, decoded from com.samsung.sensormodule.2_0_lsi_s5k3l6.bin
 * (QTI Chromatix). MCLK 19.2MHz, 4-lane RAW10, 4000x3000@30, requested link
 * rate 1190 Mbps/lane -> link_freq 595.2MHz, pixel_rate 476.16 MP/s.
 */
static const struct s5k3l6xx_reg s5k3l6xx_vendor_init[] = {
	{ 0x306a, 0x2f4c, 2 }, { 0x306c, 0xca01, 2 }, { 0x307a, 0x0d20, 2 },
	{ 0x309e, 0x002d, 2 }, { 0x3072, 0x0013, 2 }, { 0x3074, 0x0977, 2 },
	{ 0x3076, 0x9411, 2 }, { 0x3024, 0x0016, 2 }, { 0x3002, 0x0e00, 2 },
	{ 0x3006, 0x1000, 2 }, { 0x300a, 0x0c00, 2 }, { 0x3018, 0xc500, 2 },
	{ 0x303a, 0x0204, 2 }, { 0x3266, 0x0001, 2 }, { 0x38da, 0x000a, 2 },
	{ 0x38dc, 0x000b, 2 }, { 0x38d6, 0x000a, 2 }, { 0x3070, 0x3d00, 2 },
	{ 0x3084, 0x1314, 2 }, { 0x3086, 0x0ce7, 2 }, { 0x3004, 0x0800, 2 },
	{ 0x3c08, 0xffff, 2 },
};

static const struct s5k3l6xx_reg s5k3l6xx_vendor_4000x3000[] = {
	{ 0x314a, 0x5f00, 2 }, { 0x3064, 0xffcf, 2 }, { 0x3066, 0x7e00, 2 },
	{ 0x309c, 0x0640, 2 }, { 0x380c, 0x001a, 2 }, { 0x32b2, 0x0000, 2 },
	{ 0x32b4, 0x0000, 2 }, { 0x32b6, 0x0000, 2 }, { 0x32b8, 0x0000, 2 },
	{ 0x3090, 0x8800, 2 }, { 0x3238, 0x000c, 2 }, { 0x0100, 0x0000, 2 },
	{ 0x0344, 0x0070, 2 }, { 0x0348, 0x100f, 2 }, { 0x0346, 0x0044, 2 },
	{ 0x034a, 0x0bfb, 2 }, { 0x034c, 0x0fa0, 2 }, { 0x034e, 0x0bb8, 2 },
	{ 0x0202, 0x03de, 2 }, { 0x3400, 0x0000, 2 }, { 0x3402, 0x4e42, 2 },
	{ 0x0136, 0x1333, 2 }, { 0x0304, 0x0003, 2 }, { 0x0306, 0x0070, 2 },
	{ 0x030c, 0x0003, 2 }, { 0x030e, 0x005d, 2 }, { 0x3c16, 0x0000, 2 },
	{ 0x0342, 0x1320, 2 }, { 0x0340, 0x0cb5, 2 }, { 0x0900, 0x0000, 2 },
	{ 0x0386, 0x0001, 2 }, { 0x3452, 0x0000, 2 }, { 0x345a, 0x0000, 2 },
	{ 0x345c, 0x0000, 2 }, { 0x345e, 0x0000, 2 }, { 0x3460, 0x0000, 2 },
	{ 0x38c4, 0x0009, 2 }, { 0x38d8, 0x002a, 2 }, { 0x38da, 0x000a, 2 },
	{ 0x38dc, 0x000b, 2 }, { 0x38c2, 0x000a, 2 }, { 0x38c0, 0x000f, 2 },
	{ 0x38d6, 0x000a, 2 }, { 0x38d4, 0x0009, 2 }, { 0x38b0, 0x000f, 2 },
	{ 0x3932, 0x1000, 2 }, { 0x0820, 0x04a6, 2 }, { 0x3c34, 0x0008, 2 },
	{ 0x3c36, 0x3800, 2 }, { 0x3c38, 0x0020, 2 }, { 0x393e, 0x4000, 2 },
	{ 0x3892, 0x3600, 2 }, { 0x3c1e, 0x0100, 2 },
};

#define PAD_CIS 0
#define NUM_CIS_PADS 1
#define NUM_ISP_PADS 1


struct s5k3l6xx_frame {
	char *name;
	u32 width;
	u32 height;
	u32 code; // Media bus code
	const struct s5k3l6xx_reg  *streamregs;
	u16 streamregcount;
	u16 mipi_multiplier; // op_pll_multiplier
	u16 mipi_post_scaler;
	u16 min_frame_length_lines;
	u16 min_line_length_pck;
	u16 data_format;
};

struct s5k3l6xx_ctrls {
	struct v4l2_ctrl_handler handler;
	struct { /* Mirror cluster */
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};
	struct { /* Exposure and gain cluster */
		struct v4l2_ctrl *exposure;
		struct v4l2_ctrl *analog_gain;
		struct v4l2_ctrl *digital_gain;
	};
	struct { /* Link properties */
		struct v4l2_ctrl *link_freq;
		struct v4l2_ctrl *pixel_rate;
	};
	struct { /* Frame properties */
		struct v4l2_ctrl *hblank;
		struct v4l2_ctrl *vblank;
	};
};

struct s5k3l6xx {
	struct gpio_desc *rst_gpio;
	/* a52q: optional MIPI_SEL switch (gpio66) shared with the macro sensor;
	 * driven active while this sensor is powered to route it to CSIPHY3. */
	struct gpio_desc *mipi_sel_gpio;
	struct regulator_bulk_data supplies[S5K3L6XX_NUM_SUPPLIES];
	enum v4l2_mbus_type bus_type;
	u8 nlanes;

	struct clk *clock;
	u32 mclk_frequency;

	struct v4l2_subdev cis_sd;
	struct media_pad cis_pad;

	struct v4l2_subdev sd;

	/* protects the struct members below */
	struct mutex lock;

	int error;

	/* Currently selected frame format */
	const struct s5k3l6xx_frame *frame_fmt;

	struct s5k3l6xx_ctrls ctrls;

	/* Solid color test pattern is in effect,
	 * write needs to happen after color choice writes.
	 * It doesn't seem that controls guarantee any order of application. */
	unsigned int apply_test_solid:1;

	unsigned int streaming:1;
	unsigned int apply_cfg:1;
	unsigned int apply_crop:1;
	unsigned int valid_auto_alg:1;
	unsigned int power;

	u16 line_length;
	u16 frame_length;
	u32 pixel_clock;
};

// Frame sizes are only available in RAW, so this effectively replaces pixfmt.
// The a52q only uses the vendor 4000x3000 RAW10 mode.
static const struct s5k3l6xx_frame s5k3l6xx_frames[] = {
	{
		.name = "a52q vendor 4000x3000 10bpp 30fps",
		.width = 4000, .height = 3000,
		.streamregs = s5k3l6xx_vendor_4000x3000,
		.streamregcount = ARRAY_SIZE(s5k3l6xx_vendor_4000x3000),
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.mipi_multiplier = 124,
		.mipi_post_scaler = 0,
		.min_frame_length_lines = 3253,
		.min_line_length_pck = 4896,
		.data_format = S5K3L6XX_DATA_FORMAT_RAW10,
	},
};

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct s5k3l6xx, ctrls.handler)->sd;
}

static inline bool s5k5baf_is_cis_subdev(struct v4l2_subdev *sd)
{
	return sd->entity.function == MEDIA_ENT_F_CAM_SENSOR;
}

static inline struct s5k3l6xx *to_s5k3l6xx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k3l6xx, sd);
}

struct s5k3l6xx_read_result {
	u8 value;
	int retcode;
};

static struct s5k3l6xx_read_result __s5k3l6xx_i2c_read(struct s5k3l6xx *state, u16 addr)
{
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	__be16 w;
	struct s5k3l6xx_read_result ret;
	struct i2c_msg msg[] = {
		{ .addr = c->addr, .flags = 0,
		  .len = 2, .buf = (u8 *)&w },
		{ .addr = c->addr, .flags = I2C_M_RD,
		  .len = 1, .buf = (u8 *)&ret.value },
	};
	w = cpu_to_be16(addr);
	ret.retcode = i2c_transfer(c->adapter, msg, 2);

	if (ret.retcode != 2) {
		v4l2_err(c, "i2c_read: error during transfer (%d)\n", ret.retcode);
	}
	return ret;
}

static struct s5k3l6xx_read_result s5k3l6xx_i2c_read(struct s5k3l6xx *state, u16 addr)
{
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	struct s5k3l6xx_read_result ret;
	ret = __s5k3l6xx_i2c_read(state, addr);
	v4l2_dbg(3, debug, c, "i2c_read: 0x%04x : 0x%02x\n", addr, ret.value);
	return ret;
}

static int s5k3l6xx_i2c_write(struct s5k3l6xx *state, u16 addr, u8 val)
{
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	u8 buf[3] = { addr >> 8, addr & 0xFF, val };

	struct i2c_msg msg[] = {
		{ .addr = c->addr, .flags = 0,
		  .len = 3, .buf = buf },
	};
	int ret;
	int actual;

	v4l2_dbg(3, debug, c, "i2c_write to 0x%04x : 0x%02x\n", addr, val);

	ret = i2c_transfer(c->adapter, msg, 1);

	if (ret != 1) {
		v4l2_err(c, "i2c_write: error during transfer (%d)\n", ret);
		return ret;
	}
	// Not sure if actually needed. So really debugging code at the moment.
	actual = s5k3l6xx_i2c_read(state, addr).value;
	if (actual != val)
		v4l2_err(c, "i2c_write: value didn't stick. 0x%04x = 0x%02x != 0x%02x", addr, actual, val);
	return 0;
}

static int s5k3l6xx_i2c_write2(struct s5k3l6xx *state, u16 addr, u16 val)
{
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	u8 buf[4] = { addr >> 8, addr & 0xFF, (val >> 8) & 0xff, val & 0xff };

	struct i2c_msg msg[] = {
		{ .addr = c->addr, .flags = 0,
		  .len = 4, .buf = buf },
	};
	int ret;

	v4l2_dbg(3, debug, c, "i2c_write to 0x%04x : 0x%04x\n", addr, val);

	ret = i2c_transfer(c->adapter, msg, 1);

	// 1 message expected
	if (ret != 1) {
		v4l2_err(c, "i2c_write: error during transfer (%d)\n", ret);
		return ret;
	}
	return 0;
}

static int s5k3l6xx_submit_regs(struct s5k3l6xx *state, const struct s5k3l6xx_reg *regs, u16 regcount) {
       unsigned i;
       int ret = 0;
       for (i = 0; i < regcount; i++) {
	       if (regs[i].size == 2)
		       ret = s5k3l6xx_i2c_write2(state, regs[i].address, regs[i].val);
	       else
		       ret = s5k3l6xx_i2c_write(state, regs[i].address, (u8)regs[i].val);
	       if (ret < 0)
		       break;
       }
       return ret;
}

static struct s5k3l6xx_read_result s5k3l6xx_read(struct s5k3l6xx *state, u16 addr)
{
	return s5k3l6xx_i2c_read(state, addr);
}

static int s5k3l6xx_write(struct s5k3l6xx *state, u16 addr, u8 val)
{
	return s5k3l6xx_i2c_write(state, addr, val);
}

static int s5k3l6xx_find_pixfmt(const struct v4l2_mbus_framefmt *mf)
{
	int i, c = -1;

	for (i = 0; i < ARRAY_SIZE(s5k3l6xx_frames); i++) {
		if ((mf->colorspace != V4L2_COLORSPACE_DEFAULT)
				&& (mf->colorspace != V4L2_COLORSPACE_RAW))
			continue;
		if ((mf->width != s5k3l6xx_frames[i].width) || (mf->height != s5k3l6xx_frames[i].height))
			continue;
		if (mf->code == s5k3l6xx_frames[i].code)
			return i;
	}
	return c;
}

static int s5k3l6xx_hw_set_config(struct s5k3l6xx *state) {
	const struct s5k3l6xx_frame *frame_fmt = state->frame_fmt;
	/* The vendor init + mode tables carry the full PLL/MIPI setup for the
	 * a52q's 19.2MHz MCLK. */
	struct s5k3l6xx_reg fmt_regs[] = {
		{ S5K3L6XX_REG_DATA_FORMAT, frame_fmt->data_format, 2 },
		{ S5K3L6XX_REG_LANE_MODE,   state->nlanes - 1,      1 },
	};
	int ret;

	v4l2_dbg(3, debug, &state->sd, "Setting frame format %s", frame_fmt->name);

	ret = s5k3l6xx_submit_regs(state, s5k3l6xx_vendor_init,
				   ARRAY_SIZE(s5k3l6xx_vendor_init));
	if (ret < 0)
		return ret;
	ret = s5k3l6xx_submit_regs(state, frame_fmt->streamregs,
				   frame_fmt->streamregcount);
	if (ret < 0)
		return ret;
	return s5k3l6xx_submit_regs(state, fmt_regs, ARRAY_SIZE(fmt_regs));
}

static int s5k3l6xx_hw_set_test_pattern(struct s5k3l6xx *state, int id)
{
	return s5k3l6xx_write(state, S5K3L6XX_REG_TEST_PATTERN_MODE, (u8)id);
}

static int s5k3l6xx_power_on(struct s5k3l6xx *state)
{
	int ret;

	v4l2_dbg(1, debug, &state->sd, "power_ON\n");

	ret = regulator_bulk_enable(S5K3L6XX_NUM_SUPPLIES, state->supplies);
	if (ret < 0)
		goto err;

	/* route the shared MIPI switch to this sensor */
	gpiod_set_value_cansleep(state->mipi_sel_gpio, 1);

	usleep_range(10, 20);

	ret = clk_set_rate(state->clock, state->mclk_frequency);
	if (ret < 0)
		goto err_reg_dis;

	ret = clk_prepare_enable(state->clock);
	if (ret < 0)
		goto err_reg_dis;

	v4l2_dbg(1, debug, &state->sd, "ON. clock frequency: %ld\n",
		 clk_get_rate(state->clock));

	gpiod_set_value_cansleep(state->rst_gpio, 0);
	/* 1ms = 25000 cycles at 25MHz */
	usleep_range(1000, 1200);

	/*
	 * this additional reset-active is not documented but makes power-on
	 * more stable.
	 */
	usleep_range(400, 800);
	gpiod_set_value_cansleep(state->rst_gpio, 1);
	usleep_range(400, 800);
	gpiod_set_value_cansleep(state->rst_gpio, 0);

	msleep(10);

	return 0;

err_reg_dis:
	regulator_bulk_disable(S5K3L6XX_NUM_SUPPLIES, state->supplies);
err:
	v4l2_err(&state->sd, "%s() failed (%d)\n", __func__, ret);
	return ret;
}

static int s5k3l6xx_power_off(struct s5k3l6xx *state)
{
	int ret;

	v4l2_dbg(1, debug, &state->sd, "power_OFF\n");

	state->apply_cfg = 0;
	state->apply_crop = 0;

	usleep_range(20, 40);

	gpiod_set_value_cansleep(state->rst_gpio, 1);

	if (!IS_ERR(state->clock))
		clk_disable_unprepare(state->clock);

	ret = regulator_bulk_disable(S5K3L6XX_NUM_SUPPLIES, state->supplies);
	if (ret < 0)
		v4l2_err(&state->sd, "failed to disable regulators\n");
	else
		v4l2_dbg(1, debug, &state->sd, "OFF\n");
	return 0;
}

/*
 * V4L2 subdev core and video operations
 */

static int s5k3l6xx_set_power(struct v4l2_subdev *sd, int on)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	int ret = 0;

	mutex_lock(&state->lock);

	if (state->power != !on)
		goto out;

	if (on) {
		ret = s5k3l6xx_power_on(state);
		if (ret < 0)
			goto out;
		state->power++;
	} else {
		ret = s5k3l6xx_power_off(state);
		state->power--;
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int s5k3l6xx_hw_set_stream(struct s5k3l6xx *state, int enable)
{
	int ret;

	v4l2_dbg(3, debug, &state->sd, "set_stream %d", enable);

	if (!enable) {
		ret = s5k3l6xx_i2c_write(state, S5K3L6XX_REG_MODE_SELECT,
					 S5K3L6XX_MODE_STANDBY);
		if (ret)
			return ret;

		return 0;
	}

	ret = s5k3l6xx_i2c_write(state, S5K3L6XX_REG_PLL_PD, 0x01);
	if (ret)
		return ret;

	ret = s5k3l6xx_i2c_write(state, S5K3L6XX_REG_MODE_SELECT,
				 S5K3L6XX_MODE_STREAMING);
	if (ret)
		return ret;

	return s5k3l6xx_i2c_write(state, S5K3L6XX_REG_PLL_PD, 0x00);
}

static int s5k3l6xx_s_stream(struct v4l2_subdev *sd, int on)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	int ret = 0;

	if (state->streaming == !!on) {
		return 0;
	}

	if (on) {
		ret = pm_runtime_get_sync(&c->dev);
		if (ret < 0) {
			dev_err(&c->dev, "%s: pm_runtime_get failed: %d\n",
				__func__, ret);
			// Not actually sure why this is _noidle.
			// Because the device was not actually acquired?
			pm_runtime_put_noidle(&c->dev);
			return ret;
		}

		ret = v4l2_ctrl_handler_setup(&state->ctrls.handler);
		if (ret < 0)
			goto fail_ctrl;
		mutex_lock(&state->lock);
		ret = s5k3l6xx_hw_set_config(state);
		if (ret < 0)
			goto fail_to_start;
		ret = s5k3l6xx_hw_set_stream(state, 1);
		if (ret < 0)
			goto fail_to_start;
	} else {
		mutex_lock(&state->lock);
		ret = s5k3l6xx_hw_set_stream(state, 0);
		if (ret < 0) {
			mutex_unlock(&state->lock);
			return ret;
		}
		pm_runtime_put(&c->dev);
	}
	state->streaming = !state->streaming;
	mutex_unlock(&state->lock);
	return 0;
fail_to_start:
	mutex_unlock(&state->lock);
fail_ctrl:
	pm_runtime_put(&c->dev);
	dev_err(&c->dev, "failed to start stream: %d\n", ret);
	return ret;
}

/*
 * V4L2 subdev pad level and video operations
 */
static int s5k3l6xx_try_cis_format(struct v4l2_mbus_framefmt *mf)
{
	int pixfmt;
	const struct s5k3l6xx_frame *mode = v4l2_find_nearest_size(s5k3l6xx_frames,
				      ARRAY_SIZE(s5k3l6xx_frames),
				      width, height,
				      mf->width, mf->height);
	struct v4l2_mbus_framefmt candidate = *mf;
	candidate.width = mode->width;
	candidate.height = mode->height;

	pixfmt = s5k3l6xx_find_pixfmt(&candidate);
	if (pixfmt < 0)
		return pixfmt;

	mf->colorspace = V4L2_COLORSPACE_RAW;
	mf->code = s5k3l6xx_frames[pixfmt].code;
	mf->field = V4L2_FIELD_NONE;

	return pixfmt;
}

static int s5k3l6xx_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *mf;

	mf = v4l2_subdev_state_get_format(sd_state, PAD_CIS);
	s5k3l6xx_try_cis_format(mf);
	return 0;
}

static int s5k3l6xx_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	unsigned repeats[ARRAY_SIZE(s5k3l6xx_frames)] = {0};
	unsigned i, j;
	unsigned matching = 0;

	/* Find unique codes within the frame configs array.
	 * The algorithm is O(n^2), but there's only a handful of configs,
	 * meaning that it's unlikely to take a long time.
	 * The repeats array's size is determined at compile time.
	 */
	for (i = 0; i < ARRAY_SIZE(s5k3l6xx_frames); i++) {
		for (j = 0; j < i; j++) {
			if (s5k3l6xx_frames[j].code == s5k3l6xx_frames[i].code) {
				repeats[i]++;
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(s5k3l6xx_frames); i++) {
		if (repeats[i] != 0)
			continue;

		if (matching == code->index) {
			code->code = s5k3l6xx_frames[i].code;
			return 0;
		}
		matching++;
	}

	return -EINVAL;
}

static int s5k3l6xx_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned i;
	unsigned matching = 0;

	for (i = 0; i < ARRAY_SIZE(s5k3l6xx_frames); i++) {
		if (fse->code != s5k3l6xx_frames[i].code)
			continue;

		if (fse->index == matching) {
			fse->code = s5k3l6xx_frames[i].code;
			fse->min_width = s5k3l6xx_frames[i].width;
			fse->max_width = s5k3l6xx_frames[i].width;
			fse->max_height = s5k3l6xx_frames[i].height;
			fse->min_height = s5k3l6xx_frames[i].height;

			return 0;
		}
		matching++;
	}

	dev_dbg(sd->dev, "fsize i %d m %d", i, matching);

	return -EINVAL;
}

static void s5k3l6xx_get_current_cis_format(struct v4l2_subdev *sd, struct v4l2_mbus_framefmt *mf)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	// FIXME: This won't work for debug mode,
	// which is meant to adjust to whatever userspace wants.
	// Maybe save what userspace set last.
	mf->width = state->frame_fmt->width;
	mf->height = state->frame_fmt->height;
	mf->code = state->frame_fmt->code;
	mf->colorspace = V4L2_COLORSPACE_RAW;
}

static int s5k3l6xx_get_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		mf = v4l2_subdev_state_get_format(sd_state, fmt->pad);
		fmt->format = *mf;
		dev_dbg(sd->dev, "try mf %dx%d", mf->width, mf->height);
		return 0;
	}

	mf = &fmt->format;
	if (fmt->pad == PAD_CIS) {
		s5k3l6xx_get_current_cis_format(sd, mf);
		dev_dbg(sd->dev, "mf %dx%d", mf->width, mf->height);
		return 0;
	}
	dev_err(sd->dev, "Not a CIS pad! %d", fmt->pad);
	return 0;
}

/** Calculating the MIPI clock
 *
 * Cast:
 * - extclk (typically 24MHz)
 * - MIPI predivider (mipi_div)
 * - MIPI multiplier (mipi_multiplier)
 * - MIPI postscaler (mipi_scaler)
 *
 * Calculation:
 * extclk * mipi_multiplier * 2 ^ mipi_scaler / mipi_div
 *
 * With defaults:
 * 24 * 0x64 * 2 ^ 0 / 4
 * = 24 * 100 * 2 ^ 0 / 4
 * = 600 [MHz]
 */
#define MIPI_CLOCK(extclk, div, multiplier, scaler) ((extclk / div * multiplier) >> scaler)

// Lane rates for the OP PLL (op_pre_pll_div 4, op_pll_mult = frame->mipi_multiplier)
// at the a52q's 19.2MHz MCLK. set_fmt selects the entry matching the active mode;
// these must match the DT link-frequencies so the CSIPHY settle count is correct.
// menu[0] also seeds the pixel_clock in probe().
static const s64 s5k3l6xx_link_freqs_menu[] = {
	MIPI_CLOCK(19200000, 4, 108, 1),	/* 259.2 MHz */
	MIPI_CLOCK(19200000, 4, 100, 0),	/* 480 MHz */
	MIPI_CLOCK(19200000, 4, 124, 0),	/* 595.2 MHz (a52q vendor 4000x3000) */
};


static void s5k3l6xx_update_exposure(struct s5k3l6xx *state)
{
	u16 frame_length = max(state->frame_fmt->min_frame_length_lines, state->frame_length);

	__v4l2_ctrl_modify_range(state->ctrls.exposure, 2, frame_length - 2, 1, min(0x03de, frame_length - 2));
}

static void s5k3l6xx_update_blanking(struct s5k3l6xx *state) {
	u16 frame_length = max(state->frame_fmt->min_frame_length_lines, state->frame_length);
	u16 line_length = max(state->frame_fmt->min_line_length_pck, state->line_length);

	__v4l2_ctrl_s_ctrl(state->ctrls.vblank, frame_length - state->frame_fmt->height);
	__v4l2_ctrl_s_ctrl(state->ctrls.hblank, line_length - state->frame_fmt->width);

        __v4l2_ctrl_modify_range(state->ctrls.vblank, state->frame_fmt->min_frame_length_lines - state->frame_fmt->height, 0xFFFF - state->frame_fmt->height, 1, frame_length - state->frame_fmt->height);
        __v4l2_ctrl_modify_range(state->ctrls.hblank, state->frame_fmt->min_line_length_pck - state->frame_fmt->width, 0xFFF0 - state->frame_fmt->width, 16, line_length - state->frame_fmt->width);

	s5k3l6xx_update_exposure(state);
}

static int s5k3l6xx_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *mf = &fmt->format;
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	int pixfmt_idx = 0;
	unsigned mipi_clk;
	s32 i;

	mf->field = V4L2_FIELD_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, fmt->pad) = *mf;
		return 0;
	}

	mutex_lock(&state->lock);

	if (state->streaming) {
		mutex_unlock(&state->lock);
		return -EBUSY;
	}


	pixfmt_idx = s5k3l6xx_try_cis_format(mf);
	if (pixfmt_idx == -1) {
		v4l2_err(sd, "set_fmt choice unsupported");
		mutex_unlock(&state->lock);
		return -EINVAL; // could not find the format. Unsupported
	}
	state->frame_fmt = &s5k3l6xx_frames[pixfmt_idx];
	mf->width = state->frame_fmt->width;
	mf->height = state->frame_fmt->height;

	mipi_clk = MIPI_CLOCK(state->mclk_frequency, 4, state->frame_fmt->mipi_multiplier, state->frame_fmt->mipi_post_scaler);

	// Report the smallest freq larger than configured.
	// Freqs array must increase.
	for (i = ARRAY_SIZE(s5k3l6xx_link_freqs_menu) - 1; i > 0; i--)
		if (mipi_clk > s5k3l6xx_link_freqs_menu[i - 1])
			break;

	mf->code = state->frame_fmt->code;
	mf->colorspace = V4L2_COLORSPACE_RAW;
	mutex_unlock(&state->lock);

	/*
	Change of controls is conceptually atomic
	and should maybe be done within the same lock acquisition,
	but this ends up calling s_ctrl.
	s_ctrl can be called from an unguarded context,
	and it acquires the lock itself,
	so those two are placed outside of the lock to avoid deadlocking.
	*/
	__v4l2_ctrl_s_ctrl(state->ctrls.link_freq, i);
	s5k3l6xx_update_blanking(state);

	return 0;
}

static struct v4l2_rect get_crop(const struct s5k3l6xx_frame *fmt)
{
	struct v4l2_rect ret = {
		.top = 0,
		.left = 0,
		.width = fmt->width,
		.height = fmt->height,
	};
	return ret;
}

static int s5k3l6xx_get_selection(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_selection *sel)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);

	// FIXME: does crop rectangle affect vblank/hblank?
	// If no, then it can be independent of mode (frame format).
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		mutex_lock(&state->lock);
		switch (sel->which) {
		case V4L2_SUBDEV_FORMAT_TRY:
			v4l2_subdev_state_get_crop(sd_state, sel->pad);
			break;
		case V4L2_SUBDEV_FORMAT_ACTIVE:
			sel->r = get_crop(state->frame_fmt);
			break;
		}
		mutex_unlock(&state->lock);
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = S5K3L6XX_CIS_WIDTH;
		sel->r.height = S5K3L6XX_CIS_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}


static int s5k3l6xx_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);

	mutex_lock(&state->lock);
	u16 frame_length = max(state->frame_fmt->min_frame_length_lines, state->frame_length);
	u16 line_length = max(state->frame_fmt->min_line_length_pck, state->line_length);

	u32 divisor = gcd((u32)frame_length * line_length, state->pixel_clock);
	fi->interval.numerator   = (u32)frame_length * line_length / divisor;
	fi->interval.denominator = state->pixel_clock / divisor;
	mutex_unlock(&state->lock);

	return 0;
}


static void s5k3l6xx_set_frame_interval(struct s5k3l6xx *state,
				       struct v4l2_subdev_frame_interval *fi)
{
	const struct s5k3l6xx_frame *fmt = state->frame_fmt;

	if (fi->interval.numerator == 0 || fi->interval.denominator == 0) {
		fi->interval.numerator = (u32)fmt->min_line_length_pck * fmt->min_frame_length_lines;
		fi->interval.denominator = state->pixel_clock;
	}

	u64 frame_pix = div_u64((u64)state->pixel_clock * fi->interval.numerator, fi->interval.denominator);

	u32 target_line_length = clamp_val(ALIGN(DIV_ROUND_UP(frame_pix, fmt->min_frame_length_lines), 16), fmt->min_line_length_pck, 0xFFF0u);
	state->frame_length = clamp_val(DIV_ROUND_UP(frame_pix, target_line_length), fmt->min_frame_length_lines, 0xFFFFu);
	state->line_length  = clamp_val(ALIGN(DIV_ROUND_UP(frame_pix, state->frame_length), 16), fmt->min_line_length_pck, 0xFFF0u);

	u32 divisor = gcd((u32)state->frame_length * state->line_length, state->pixel_clock);
	fi->interval.numerator   = (u32)state->frame_length * state->line_length / divisor;
	fi->interval.denominator = state->pixel_clock / divisor;
}

static int s5k3l6xx_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct s5k3l6xx *state = to_s5k3l6xx(sd);

	mutex_lock(&state->lock);
	s5k3l6xx_set_frame_interval(state, fi);
	mutex_unlock(&state->lock);

	s5k3l6xx_update_blanking(state);
	return 0;
}

static const struct v4l2_subdev_pad_ops s5k3l6xx_pad_ops = {
	.enum_mbus_code		= s5k3l6xx_enum_mbus_code,
	.enum_frame_size	= s5k3l6xx_enum_frame_size,
	//.enum_frame_interval	= s5k3l6xx_enum_frame_interval,
	.get_frame_interval     = s5k3l6xx_g_frame_interval,
	.set_frame_interval     = s5k3l6xx_s_frame_interval,
	.get_fmt		= s5k3l6xx_get_fmt,
	.set_fmt		= s5k3l6xx_set_fmt,
	.get_selection		= s5k3l6xx_get_selection,
	// TODO: add set_selection
};

static const struct v4l2_subdev_video_ops s5k3l6xx_video_ops = {
	.s_stream		= s5k3l6xx_s_stream,
};

static const struct v4l2_subdev_internal_ops s5k3l6xx_internal_ops = {
	.init_state = s5k3l6xx_init_state,
};

/*
 * V4L2 subdev controls
 */

static int s5k3l6xx_set_test_color(struct s5k3l6xx *state, struct v4l2_ctrl *ctrl, u16 color_channel) {
	int ret = s5k3l6xx_i2c_write2(state, color_channel, (u16)ctrl->val & 0x3ff);
	if (ret < 0)
		return ret;
	if (state->apply_test_solid)
		ret = s5k3l6xx_hw_set_test_pattern(state, S5K3L6XX_TEST_PATTERN_SOLID_COLOR);
	return ret;
}

static int s5k3l6xx_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	int in_use;
	int ret = 0;

	v4l2_dbg(1, debug, sd, "ctrl: %s, value: %d\n", ctrl->name, ctrl->val);

	// Don't do anything when powered off.
	// It will get called again when powering up.
	if (state->power == 0)
		return 0;
	/* v4l2_ctrl_handler_setup() function may not be used in the device’s runtime PM
	 * runtime_resume callback, as it has no way to figure out the power state of the device.
	 * https://www.kernel.org/doc/html/latest/driver-api/media/camera-sensor.html#control-framework
	 * Okay, so what's the right way to do it? So far relying on state->power.
	 */

	in_use = pm_runtime_get_if_in_use(&c->dev);

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		// Analog gain supported up to 0x200 (16). Gain = register / 32, so 0x20 gives gain 1.
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_ANALOG_GAIN, (u16)ctrl->val & 0x3ff);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_DIGITAL_GAIN, (u16)ctrl->val & 0xfff);
		break;
	case V4L2_CID_EXPOSURE:
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_COARSE_INTEGRATION_TIME, (u16)ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		state->frame_length = ctrl->val + state->frame_fmt->height;
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_FRAME_LENGTH_LINES, state->frame_length);
		s5k3l6xx_update_exposure(state);
		break;
	case V4L2_CID_HBLANK:
		state->line_length = ctrl->val + state->frame_fmt->width;
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_LINE_LENGTH_PCK, state->line_length);
		break;
	case V4L2_CID_LINK_FREQ:
		ret = s5k3l6xx_i2c_write2(state, S5K3L6XX_REG_REQUESTED_LINK_RATE, ctrl->val / 1000000 * 2);
		break;
	case V4L2_CID_TEST_PATTERN:
		state->apply_test_solid = (ctrl->val == S5K3L6XX_TEST_PATTERN_SOLID_COLOR);
		v4l2_dbg(3, debug, sd, "Setting pattern %d", ctrl->val);
		ret = s5k3l6xx_hw_set_test_pattern(state, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		ret = s5k3l6xx_set_test_color(state, ctrl, S5K3L6XX_REG_TEST_DATA_RED);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		ret = s5k3l6xx_set_test_color(state, ctrl, S5K3L6XX_REG_TEST_DATA_GREENR);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		ret = s5k3l6xx_set_test_color(state, ctrl, S5K3L6XX_REG_TEST_DATA_BLUE);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		ret = s5k3l6xx_set_test_color(state, ctrl, S5K3L6XX_REG_TEST_DATA_GREENB);
		break;
	}

	if (in_use) { // came from other context than resume, need to manage PM
		pm_runtime_put(&c->dev);
	}

	return ret;
}

static const struct v4l2_ctrl_ops s5k3l6xx_ctrl_ops = {
	.s_ctrl	= s5k3l6xx_s_ctrl,
};

static const char * const s5k3l6xx_test_pattern_menu[] = {
	"Disabled",
	"Solid", // Color selectable
	"Bars", // 8 bars 100% saturation: black, blue, red, magents, green, cyan, yellow, white
	"Fade", // Bars fading towards 50% at the bottom. 512px high. Subdivided into left smooth and right quantized halves.
	"PN9", // pseudo-random noise
	"White",
	"LFSR32",
	"Address",
};


static int s5k3l6xx_initialize_ctrls(struct s5k3l6xx *state)
{
	const struct v4l2_ctrl_ops *ops = &s5k3l6xx_ctrl_ops;
	struct s5k3l6xx_ctrls *ctrls = &state->ctrls;
	struct v4l2_ctrl_handler *hdl = &ctrls->handler;
	struct i2c_client *c = v4l2_get_subdevdata(&state->sd);
	struct v4l2_fwnode_device_properties props;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, 16);
	if (ret < 0) {
		v4l2_err(&state->sd, "cannot init ctrl handler (%d)\n", ret);
		return ret;
	}

	// Values will be updated in s5k3l6xx_update_exposure

	// Exposure time (min: 2; max: frame length lines - 2; default: reset value)
	ctrls->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,

					    2, 3258, 1, 0x03de);
	ctrls->vblank = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK,
					  1, 1, 1, 1);
	ctrls->hblank = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HBLANK,
					  1, 1, 1, 1);

	ctrls->pixel_rate = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE,
					      state->pixel_clock, state->pixel_clock, 1, state->pixel_clock);
	ctrls->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	// Analog gain range: 1x - 16x
	ctrls->analog_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_ANALOGUE_GAIN,
					0x20, 0x200, 1, 0x20);

	// Digital gain range: 1x - 3x
	ctrls->digital_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_DIGITAL_GAIN,
					0x100, 0x300, 1, 0x100);

	v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k3l6xx_test_pattern_menu) - 1,
				     0, 0, s5k3l6xx_test_pattern_menu);

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_TEST_PATTERN_RED, 0, 1023, 1, 512);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_TEST_PATTERN_GREENR, 0, 1023, 1, 512);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_TEST_PATTERN_BLUE, 0, 1023, 1, 512);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_TEST_PATTERN_GREENB, 0, 1023, 1, 512);


	ctrls->link_freq = v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_LINK_FREQ,
						  ARRAY_SIZE(s5k3l6xx_link_freqs_menu) - 1,
						  0, s5k3l6xx_link_freqs_menu);

	ctrls->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (hdl->error) {
		v4l2_err(&state->sd, "error creating controls (%d)\n",
			 hdl->error);
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	ret = v4l2_fwnode_device_parse(&c->dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, ops, &props);
	if (ret)
		return ret;

	state->sd.ctrl_handler = hdl;
	return 0;
}

/*
 * V4L2 subdev internal operations
 */
static const struct v4l2_subdev_core_ops s5k3l6xx_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
};

static const struct v4l2_subdev_ops s5k3l6xx_subdev_ops = {
	.core = &s5k3l6xx_core_ops,
	.pad = &s5k3l6xx_pad_ops,
	.video = &s5k3l6xx_video_ops,
};

static int __maybe_unused s5k3l6xx_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3l6xx *state = to_s5k3l6xx(sd);

	dev_dbg(dev, "%s\n", __func__);

	if (state->streaming)
		s5k3l6xx_hw_set_stream(state, 0);

	return s5k3l6xx_set_power(sd, FALSE);
}

static int __maybe_unused s5k3l6xx_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3l6xx *state = to_s5k3l6xx(sd);
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = s5k3l6xx_set_power(sd, TRUE);

	if (ret == 0 && state->streaming) {
		ret = s5k3l6xx_hw_set_config(state);
		if (ret < 0) {
			state->streaming = 0;
			return ret;
		}

		ret = s5k3l6xx_hw_set_stream(state, 1);
		if (ret)
			state->streaming = 0;
	}

	return ret;
}

static int s5k3l6xx_parse_device_node(struct s5k3l6xx *state, struct device *dev)
{
	struct device_node *node = dev->of_node;
	struct device_node *node_ep;
	struct v4l2_fwnode_endpoint ep = { .bus_type = 0 };
	int ret;

	if (!node) {
		dev_err(dev, "no device-tree node provided\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "clock-frequency",
				   &state->mclk_frequency);
	if (ret < 0) {
		state->mclk_frequency = S5K3L6XX_DEFAULT_MCLK_FREQ;
		dev_warn(dev, "using default %u Hz clock frequency\n",
			 state->mclk_frequency);
	}

	state->rst_gpio = devm_gpiod_get(dev, "rstn", GPIOD_OUT_LOW);
	if (IS_ERR(state->rst_gpio)) {
		dev_err(dev, "failed to get rstn gpio: %pe\n", state->rst_gpio);
		return PTR_ERR(state->rst_gpio);
	}

	/* Shared with the macro sensor, so request non-exclusively. */
	state->mipi_sel_gpio = devm_gpiod_get_optional(dev, "mipi-sel",
					GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(state->mipi_sel_gpio)) {
		dev_err(dev, "failed to get mipi-sel gpio: %pe\n", state->mipi_sel_gpio);
		return PTR_ERR(state->mipi_sel_gpio);
	}

	node_ep = of_graph_get_next_endpoint(node, NULL);
	if (!node_ep) {
		dev_err(dev, "no endpoint defined at node %pOF\n", node);
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(node_ep), &ep);
	of_node_put(node_ep);
	if (ret) {
		dev_err(dev, "fwnode endpoint parse failed\n");
		return ret;
	}

	state->bus_type = ep.bus_type;

	switch (state->bus_type) {
	case V4L2_MBUS_CSI2_DPHY:
		state->nlanes = (unsigned char)ep.bus.mipi_csi2.num_data_lanes;
		break;
	default:
		dev_err(dev, "unsupported bus %d in endpoint defined at node %pOF\n",
			state->bus_type, (void*)node);
		return -EINVAL;
	}

	return 0;
}

static int s5k3l6xx_configure_subdevs(struct s5k3l6xx *state,
				     struct i2c_client *c)
{
	struct v4l2_subdev *sd;
	int ret;

	/*sd = &state->cis_sd;
	v4l2_subdev_init(sd, &s5k5baf_cis_subdev_ops);
	sd->owner = THIS_MODULE;
	v4l2_set_subdevdata(sd, state);
	snprintf(sd->name, sizeof(sd->name), "S5K5BAF-CIS %d-%04x",
		 i2c_adapter_id(c->adapter), c->addr);

	sd->internal_ops = &s5k5baf_cis_subdev_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	state->cis_pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, NUM_CIS_PADS, &state->cis_pad);
	if (ret < 0)
		goto err;*/
	sd = &state->sd;
	v4l2_i2c_subdev_init(sd, c, &s5k3l6xx_subdev_ops);
	sd->internal_ops = &s5k3l6xx_internal_ops;
	v4l2_info(sd, "probe i2c %px", (void*)c);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	state->cis_pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, NUM_CIS_PADS, &state->cis_pad);

	if (!ret)
		return 0;

	//media_entity_cleanup(&state->cis_sd.entity);
//err:
	dev_err(&c->dev, "cannot init media entity %s\n", sd->name);
	return ret;
}

static int s5k3l6xx_probe(struct i2c_client *c)
{
	struct s5k3l6xx *state;
	int ret;
	unsigned i;
	unsigned long mclk_freq;
	struct s5k3l6xx_read_result test;

	state = devm_kzalloc(&c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);

	ret = s5k3l6xx_parse_device_node(state, &c->dev);
	if (ret < 0) {
		pr_err("s5k3l6xx_parse_device_node: failed");
		return ret;
	}

	ret = s5k3l6xx_configure_subdevs(state, c);
	if (ret < 0) {
		pr_err("s5k3l6xx_configure_subdevs: failed");
		return ret;
	}

	for (i = 0; i < S5K3L6XX_NUM_SUPPLIES; i++)
		state->supplies[i].supply = s5k3l6xx_supply_names[i];

	ret = devm_regulator_bulk_get(&c->dev, S5K3L6XX_NUM_SUPPLIES,
				      state->supplies);
	if (ret < 0) {
		pr_err("s5k3l6xx configure regulators: failed");
		goto err_me;
	}

	state->clock = devm_clk_get(state->sd.dev, S5K3L6XX_CLK_NAME);
	if (IS_ERR(state->clock)) {
		pr_err("get clk failed: failed");
		ret = -EPROBE_DEFER;
		goto err_me;
	}

	mclk_freq = clk_get_rate(state->clock);
	/* The sensor supports an MCLK between 6MHz and 32MHz; the a52q feeds it
	 * from camcc (19.2MHz).
	 */
	if ((mclk_freq < 6000000) || (mclk_freq > 32000000)) {
		dev_err(&c->dev,
			"External clock frequency out of range: %lu.\n",
			mclk_freq);
		ret = -EINVAL;
		goto err_me;
	}
	/* pixel_rate = link_freq * 2 (DDR) * nlanes / bits_per_sample(10) */
	state->pixel_clock = s5k3l6xx_link_freqs_menu[0] * 2 * state->nlanes / 10;

	ret = s5k3l6xx_power_on(state);
	if (ret < 0) {
		pr_err("s5k3l6xx_power_on: failed");
		goto err_me;
	}
	state->power = 1;

	test = s5k3l6xx_read(state, S5K3L6XX_REG_MODEL_ID_L);
	if (test.retcode < 0) {
		ret = test.retcode;
		goto err_power;
	} else if (test.value != S5K3L6XX_MODEL_ID_L) {
		dev_err(&c->dev, "model mismatch: 0x%X != 0x30\n", test.value);
		ret = -EINVAL;
		goto err_power;
	} else
		dev_info(&c->dev, "model low: 0x%X\n", test.value);

	test = s5k3l6xx_read(state, S5K3L6XX_REG_MODEL_ID_H);
	if (test.retcode < 0) {
		ret = test.retcode;
		goto err_power;
	} else if (test.value != S5K3L6XX_MODEL_ID_H) {
		dev_err(&c->dev, "model mismatch: 0x%X != 0xC6\n", test.value);
		ret = -EINVAL;
		goto err_power;
	} else
		dev_info(&c->dev, "model high: 0x%X\n", test.value);

	test = s5k3l6xx_read(state, S5K3L6XX_REG_REVISION_NUMBER);
	if (test.retcode < 0) {
		ret = test.retcode;
		goto err_power;
	} else if (test.value != S5K3L6XX_REVISION_NUMBER) {
		dev_err(&c->dev, "revision mismatch: 0x%X != 0xB0\n", test.value);
		ret = -EINVAL;
		goto err_power;
	} else
		dev_info(&c->dev, "revision number: 0x%X\n", test.value);

	ret = s5k3l6xx_initialize_ctrls(state);
	if (ret < 0)
		goto err_power;

	ret = v4l2_async_register_subdev_sensor(&state->sd);
	if (ret < 0)
		goto err_ctrl;

	pm_runtime_set_active(&c->dev);
	pm_runtime_enable(&c->dev);
	// I don't really know why this idle is needed
	pm_runtime_idle(&c->dev);

	// Default frame.
	state->frame_fmt = &s5k3l6xx_frames[0];

	struct v4l2_subdev_frame_interval fi = {
		.interval = { .numerator = 1, .denominator = 30 },
	};
	s5k3l6xx_set_frame_interval(state, &fi);

	s5k3l6xx_update_blanking(state);

	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(state->sd.ctrl_handler);
err_power:
	s5k3l6xx_power_off(state);
err_me:
	media_entity_cleanup(&state->sd.entity);
	media_entity_cleanup(&state->cis_sd.entity);
	return ret;
}

static void s5k3l6xx_remove(struct i2c_client *c)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(c);
	struct s5k3l6xx *state = to_s5k3l6xx(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	media_entity_cleanup(&sd->entity);

	sd = &state->cis_sd;
	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	pm_runtime_disable(&c->dev);
	pm_runtime_set_suspended(&c->dev);
	pm_runtime_put_noidle(&c->dev);
}

static const struct dev_pm_ops s5k3l6xx_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(s5k3l6xx_suspend, s5k3l6xx_resume, NULL)
};

static const struct i2c_device_id s5k3l6xx_id[] = {
	{ S5K3L6XX_DRIVER_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, s5k3l6xx_id);

static const struct of_device_id s5k3l6xx_of_match[] = {
	{ .compatible = "samsung,s5k3l6xx" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k3l6xx_of_match);

static struct i2c_driver s5k3l6xx_i2c_driver = {
	.driver = {
		.pm = &s5k3l6xx_pm_ops,
		.of_match_table = s5k3l6xx_of_match,
		.name = S5K3L6XX_DRIVER_NAME
	},
	.probe		= s5k3l6xx_probe,
	.remove		= s5k3l6xx_remove,
	.id_table	= s5k3l6xx_id,
};

module_i2c_driver(s5k3l6xx_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K3L6XX 13M camera driver");
MODULE_AUTHOR("Martin Kepplinger <martin.kepplinger@puri.sm>");
MODULE_AUTHOR("Dorota Czaplejewicz <dorota.czaplejewicz@puri.sm>");
MODULE_LICENSE("GPL v2");
