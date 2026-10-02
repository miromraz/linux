// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5KGW1P CMOS image sensor driver (Galaxy A52 4G main camera).
 *
 * Register tables decoded from the vendor CamX sensor module
 * (com.samsung.sensormodule.0_1_lsi_s5kgw1p.bin), see s5kgw1p_modes.h. The init
 * list uses indirect page writes and is replayed exactly in order. Structure
 * modelled on the s5k3l6xx driver that works on this SoC (SM7125).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5KGW1P_REG_CHIP_ID	CCI_REG16(0x0000)
#define S5KGW1P_CHIP_ID		0xf971
#define S5KGW1P_ID_RETRIES	4
#define S5KGW1P_REG_MODE_SELECT	CCI_REG8(0x0100)
#define S5KGW1P_REG_ORIENTATION	CCI_REG8(0x0101)	/* bit0 hflip, bit1 vflip */
#define S5KGW1P_REG_EXPOSURE	CCI_REG16(0x0202)	/* coarse integration time */
#define S5KGW1P_REG_ANALOG_GAIN	CCI_REG16(0x0204)
#define S5KGW1P_REG_DIGITAL_GAIN CCI_REG16(0x020e)
#define S5KGW1P_REG_FLL		CCI_REG16(0x0340)	/* frame length lines */
#define S5KGW1P_REG_LLP		CCI_REG16(0x0342)	/* line length pck */
#define S5KGW1P_REG_TEST_PATTERN CCI_REG16(0x0600)

#define S5KGW1P_EXPOSURE_DEF	3000
/*
 * Limits from the vendor CamX sensor library (com.samsung.sensor.s5kgw1p.so,
 * CalculateExposure/FillExposureSettings) and sensor module (minLineCount 4):
 * analogue gain code = gain * 32, capped at 0x400 (32x) in the binned modes;
 * digital gain code = gain * 256, capped at 0xfff; frame length is kept at
 * least 4 lines above the coarse integration time.
 */
#define S5KGW1P_EXPOSURE_MIN	4
#define S5KGW1P_EXPOSURE_MARGIN	4
#define S5KGW1P_AGAIN_MIN	0x20
#define S5KGW1P_AGAIN_MAX	0x400
#define S5KGW1P_DGAIN_MIN	0x100
#define S5KGW1P_DGAIN_MAX	0xfff

#include "s5kgw1p_modes.h"

struct s5kgw1p_mode {
	u32 width;
	u32 height;
	u32 llp;		/* vendor line_length_pck */
	u32 fll;		/* vendor frame_length_lines = minimum */
	u32 fll_def;
	s64 pixel_rate;		/* VT pixel rate, checked by frame rate */
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
	bool fast_1g;		/* also write s5kgw1p_fast_1g */
	struct v4l2_rect crop;	/* analogue crop, 2x2-binned array units */
};

/*
 * C-PHY symbol rate: 19.2 MHz / 3 * op_pll_mult (0x0310) / 0x0312, 1001.6 MHz
 * in vendor mode[1]. The vendor 4x4 mode runs its link at 1708.8 MHz, which
 * camss rejects ("Pixel clock is too high for CSIPHY"), so it is moved onto
 * the mode[1] output PLL. Its VT pixel rate is then 864 MP/s, measured:
 * 4160 x 20000 -> 10.38 fps, 8324 x 6920 -> 15.01 fps, 8324 x 14968 -> 6.94 fps.
 */
static const s64 s5kgw1p_link_freq_menu[] = { 1000000000 };

static const struct cci_reg_sequence s5kgw1p_fast_1g[] = {
	{ CCI_REG16(0x030c), 0x0001 }, { CCI_REG16(0x0310), 0x0139 },
	{ CCI_REG16(0x0312), 0x0002 },
};

static const struct s5kgw1p_mode s5kgw1p_modes[] = {
	{	/* vendor mode[1]: 2x2 binned full FOV, 30.0 fps (measured) */
		.width = 4624, .height = 3468, .llp = 13672, .fll = 3512,
		.fll_def = 3512,
		.pixel_rate = 1440000000,
		.regs = s5kgw1p_mode_regs,
		.num_regs = ARRAY_SIZE(s5kgw1p_mode_regs),
		/* x 0x0010..0x242f, y 0x0000..0x1b1f: the whole array */
		.crop = { 0, 0, 4624, 3468 },
	},
	{	/*
		 * vendor mode[2]: 4x4 binned 16:9 crop (8000x4512), vendor
		 * 4160 x 1730 at 240 fps; 30 fps by default here, 120 at most.
		 */
		.width = 2000, .height = 1128, .llp = 4160, .fll = 1730,
		.fll_def = 6923, .pixel_rate = 864000000,
		.regs = s5kgw1p_mode_fast_regs,
		.num_regs = ARRAY_SIZE(s5kgw1p_mode_fast_regs),
		.fast_1g = true,
		/* x 0x0280..0x21bf, y 0x04c0..0x165f, relative to mode[1] */
		.crop = { 312, 608, 4000, 2256 },
	},
};

static const char * const s5kgw1p_supply_names[] = {
	"vio", "vana", "vdig", "vaf", "vcustom",
};
#define S5KGW1P_NUM_SUPPLIES ARRAY_SIZE(s5kgw1p_supply_names)

static const char * const s5kgw1p_test_pattern_menu[] = {
	"Disabled", "Solid Color", "100% Color Bars", "Fade to Grey", "PN9",
};

struct s5kgw1p {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *clk;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5KGW1P_NUM_SUPPLIES];
	u32 mclk_freq;

	const struct s5kgw1p_mode *mode;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	struct mutex lock;	/* serialise power/stream state */
	bool streaming;
};

static inline struct s5kgw1p *to_s5kgw1p(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5kgw1p, sd);
}

/* Readout is GRBG; each flip moves the pattern by one pixel on that axis. */
static u32 s5kgw1p_code(struct s5kgw1p *s)
{
	static const u32 codes[2][2] = {
		{ MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SRGGB10_1X10 },
		{ MEDIA_BUS_FMT_SBGGR10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10 },
	};

	return codes[s->vflip->val][s->hflip->val];
}

static int s5kgw1p_power_on(struct device *dev)
{
	struct s5kgw1p *s = to_s5kgw1p(dev_get_drvdata(dev));
	int ret;

	ret = regulator_bulk_enable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(s->clk);
	if (ret)
		goto err_reg;
	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(s->reset_gpio, 0);	/* release reset */
	usleep_range(10000, 11000);
	return 0;

err_reg:
	regulator_bulk_disable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	return ret;
}

static int s5kgw1p_power_off(struct device *dev)
{
	struct s5kgw1p *s = to_s5kgw1p(dev_get_drvdata(dev));

	gpiod_set_value_cansleep(s->reset_gpio, 1);
	clk_disable_unprepare(s->clk);
	regulator_bulk_disable(S5KGW1P_NUM_SUPPLIES, s->supplies);
	return 0;
}

/* vendor masterSettings: free-running, not waiting for an external sync */
static const struct cci_reg_sequence s5kgw1p_master[] = {
	{CCI_REG8(0x3f0b), 0x00}, {CCI_REG8(0x3041), 0x00},
	{CCI_REG8(0x3040), 0x00}, {CCI_REG8(0x4b81), 0x00},
};

static int s5kgw1p_start(struct s5kgw1p *s)
{
	int ret = 0;

	/* init[0..1] = page 0x4000 + software reset (0x6010); vendor waits 35 ms */
	cci_multi_reg_write(s->regmap, s5kgw1p_init, 2, &ret);
	if (ret)
		return ret;
	usleep_range(35000, 36000);
	cci_multi_reg_write(s->regmap, s5kgw1p_init + 2,
			    ARRAY_SIZE(s5kgw1p_init) - 2, &ret);
	cci_multi_reg_write(s->regmap, s5kgw1p_master,
			    ARRAY_SIZE(s5kgw1p_master), &ret);
	cci_multi_reg_write(s->regmap, s->mode->regs, s->mode->num_regs, &ret);
	if (s->mode->fast_1g)
		cci_multi_reg_write(s->regmap, s5kgw1p_fast_1g,
				    ARRAY_SIZE(s5kgw1p_fast_1g), &ret);
	if (ret)
		return ret;
	ret = __v4l2_ctrl_handler_setup(&s->ctrls);
	if (ret)
		return ret;
	/*
	 * Vendor stream-on is a 16-bit write to mode_select, which also sets
	 * image_orientation (0x0101): carry the flips in its low byte.
	 */
	return cci_write(s->regmap, CCI_REG16(0x0100),
			 0x0100 | s->hflip->val | s->vflip->val << 1, NULL);
}

static int s5kgw1p_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5kgw1p *s = to_s5kgw1p(sd);
	struct i2c_client *c = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&s->lock);
	if (s->streaming == !!enable)
		goto out;

	if (enable) {
		ret = pm_runtime_resume_and_get(&c->dev);
		if (ret < 0)
			goto out;
		ret = s5kgw1p_start(s);
		if (ret) {
			pm_runtime_put(&c->dev);
			goto out;
		}
	} else {
		cci_write(s->regmap, S5KGW1P_REG_MODE_SELECT, 0, NULL);
		pm_runtime_put(&c->dev);
	}
	s->streaming = enable;
	/* flips change the Bayer order, so they are fixed while streaming */
	__v4l2_ctrl_grab(s->hflip, enable);
	__v4l2_ctrl_grab(s->vflip, enable);
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int s5kgw1p_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5kgw1p *s = container_of(ctrl->handler, struct s5kgw1p, ctrls);
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(s->exposure, S5KGW1P_EXPOSURE_MIN,
					 s->mode->height + ctrl->val -
					 S5KGW1P_EXPOSURE_MARGIN, 1,
					 min(S5KGW1P_EXPOSURE_DEF, s->mode->height +
					     ctrl->val - S5KGW1P_EXPOSURE_MARGIN));

	if (!pm_runtime_get_if_in_use(&c->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s->regmap, S5KGW1P_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s->regmap, S5KGW1P_REG_ANALOG_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(s->regmap, S5KGW1P_REG_DIGITAL_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(s->regmap, S5KGW1P_REG_ORIENTATION,
				s->hflip->val | s->vflip->val << 1, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s->regmap, S5KGW1P_REG_FLL,
				s->mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_HBLANK:
		ret = cci_write(s->regmap, S5KGW1P_REG_LLP,
				s->mode->width + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s->regmap, S5KGW1P_REG_TEST_PATTERN, ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_put(&c->dev);
	return ret;
}

static const struct v4l2_ctrl_ops s5kgw1p_ctrl_ops = {
	.s_ctrl = s5kgw1p_set_ctrl,
};

static void s5kgw1p_fill_fmt(struct s5kgw1p *s, const struct s5kgw1p_mode *mode,
			     struct v4l2_mbus_framefmt *f)
{
	f->width = mode->width;
	f->height = mode->height;
	f->code = s5kgw1p_code(s);
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
}

static int s5kgw1p_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct s5kgw1p *s = to_s5kgw1p(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(state, 0);
	else
		s5kgw1p_fill_fmt(s, s->mode, &fmt->format);
	return 0;
}

/*
 * Selection rectangles in 2x2-binned units, the largest format (4624x3468)
 * being the whole active array, so that user space can tell which part of
 * the field of view a mode covers (lens shading, digital zoom).
 */
static int s5kgw1p_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	struct s5kgw1p *s = to_s5kgw1p(sd);
	const struct s5kgw1p_mode *mode = s->mode;
	const struct v4l2_mbus_framefmt *fmt;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY) {
			fmt = v4l2_subdev_state_get_format(state, 0);
			mode = v4l2_find_nearest_size(s5kgw1p_modes,
						      ARRAY_SIZE(s5kgw1p_modes),
						      width, height,
						      fmt->width, fmt->height);
		}
		sel->r = mode->crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = s5kgw1p_modes[0].crop;
		return 0;
	}
	return -EINVAL;
}

static int s5kgw1p_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
{
	struct s5kgw1p *s = to_s5kgw1p(sd);
	const struct s5kgw1p_mode *mode;
	int ret = 0;

	mode = v4l2_find_nearest_size(s5kgw1p_modes, ARRAY_SIZE(s5kgw1p_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);
	s5kgw1p_fill_fmt(s, mode, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(state, 0) = fmt->format;
		return 0;
	}

	mutex_lock(&s->lock);
	if (s->streaming) {
		ret = -EBUSY;
		goto out;
	}
	s->mode = mode;
	v4l2_ctrl_s_ctrl_int64(s->pixel_rate, mode->pixel_rate);
	v4l2_ctrl_modify_range(s->hblank, mode->llp - mode->width,
			       mode->llp - mode->width, 1,
			       mode->llp - mode->width);
	v4l2_ctrl_modify_range(s->vblank, mode->fll - mode->height,
			       0xffff - mode->height, 1,
			       mode->fll_def - mode->height);
	v4l2_ctrl_s_ctrl(s->vblank, mode->fll_def - mode->height);
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int s5kgw1p_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = s5kgw1p_code(to_s5kgw1p(sd));
	return 0;
}

static int s5kgw1p_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(s5kgw1p_modes) ||
	    fse->code != s5kgw1p_code(to_s5kgw1p(sd)))
		return -EINVAL;
	fse->min_width = fse->max_width = s5kgw1p_modes[fse->index].width;
	fse->min_height = fse->max_height = s5kgw1p_modes[fse->index].height;
	return 0;
}

static int s5kgw1p_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	s5kgw1p_fill_fmt(to_s5kgw1p(sd), &s5kgw1p_modes[0],
			 v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops s5kgw1p_video_ops = {
	.s_stream = s5kgw1p_s_stream,
};

static int s5kgw1p_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				   struct v4l2_mbus_config *config)
{
	/* 3 C-PHY data lanes, enforced at probe time. */
	config->type = V4L2_MBUS_CSI2_CPHY;
	config->bus.mipi_csi2.num_data_lanes = 3;

	return 0;
}

static const struct v4l2_subdev_pad_ops s5kgw1p_pad_ops = {
	.enum_mbus_code = s5kgw1p_enum_mbus_code,
	.enum_frame_size = s5kgw1p_enum_frame_size,
	.get_fmt = s5kgw1p_get_fmt,
	.set_fmt = s5kgw1p_set_fmt,
	.get_selection = s5kgw1p_get_selection,
	.get_mbus_config = s5kgw1p_get_mbus_config,
};

static const struct v4l2_subdev_ops s5kgw1p_subdev_ops = {
	.video = &s5kgw1p_video_ops,
	.pad = &s5kgw1p_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5kgw1p_internal_ops = {
	.init_state = s5kgw1p_init_state,
};

static int s5kgw1p_init_ctrls(struct s5kgw1p *s)
{
	struct v4l2_ctrl_handler *h = &s->ctrls;
	struct i2c_client *c = v4l2_get_subdevdata(&s->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(h, 13);

	ctrl = v4l2_ctrl_new_int_menu(h, NULL, V4L2_CID_LINK_FREQ, 0, 0,
				      s5kgw1p_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->pixel_rate = v4l2_ctrl_new_std(h, NULL, V4L2_CID_PIXEL_RATE, 1,
					  INT_MAX * 2LL, 1, s->mode->pixel_rate);
	if (s->pixel_rate)
		s->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s->exposure = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_EXPOSURE,
					S5KGW1P_EXPOSURE_MIN,
					s->mode->fll_def - S5KGW1P_EXPOSURE_MARGIN,
					1, S5KGW1P_EXPOSURE_DEF);
	v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5KGW1P_AGAIN_MIN, S5KGW1P_AGAIN_MAX, 1,
			  S5KGW1P_AGAIN_MIN);
	v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  S5KGW1P_DGAIN_MIN, S5KGW1P_DGAIN_MAX, 1,
			  S5KGW1P_DGAIN_MIN);
	s->hflip = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_HFLIP,
				     0, 1, 1, 0);
	if (s->hflip)
		s->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s->vflip = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_VFLIP,
				     0, 1, 1, 0);
	if (s->vflip)
		s->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s->vblank = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_VBLANK,
				      s->mode->fll - s->mode->height,
				      0xffff - s->mode->height, 1,
				      s->mode->fll_def - s->mode->height);
	s->hblank = v4l2_ctrl_new_std(h, &s5kgw1p_ctrl_ops, V4L2_CID_HBLANK,
				      s->mode->llp - s->mode->width,
				      s->mode->llp - s->mode->width, 1,
				      s->mode->llp - s->mode->width);
	if (s->hblank)
		s->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std_menu_items(h, &s5kgw1p_ctrl_ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5kgw1p_test_pattern_menu) - 1,
				     0, 0, s5kgw1p_test_pattern_menu);

	ret = v4l2_fwnode_device_parse(&c->dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(h, &s5kgw1p_ctrl_ops, &props);

	if (h->error) {
		ret = h->error;
		v4l2_ctrl_handler_free(h);
		return ret;
	}
	s->sd.ctrl_handler = h;
	return ret;
}

static int s5kgw1p_parse_dt(struct s5kgw1p *s, struct device *dev)
{
	struct v4l2_fwnode_endpoint ep = { .bus_type = V4L2_MBUS_CSI2_CPHY };
	struct fwnode_handle *fh;
	int ret;

	s->clk = devm_clk_get(dev, "mclk");
	if (IS_ERR(s->clk))
		return dev_err_probe(dev, PTR_ERR(s->clk), "no mclk\n");
	s->mclk_freq = clk_get_rate(s->clk);

	s->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s->reset_gpio), "no reset gpio\n");

	fh = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!fh)
		return dev_err_probe(dev, -EINVAL, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_parse(fh, &ep);
	fwnode_handle_put(fh);
	if (ret)
		return dev_err_probe(dev, ret, "endpoint parse failed\n");
	if (ep.bus.mipi_csi2.num_data_lanes != 3)
		return dev_err_probe(dev, -EINVAL, "3 data lanes required\n");
	return 0;
}

static int s5kgw1p_probe(struct i2c_client *c)
{
	struct device *dev = &c->dev;
	struct s5kgw1p *s;
	u64 id;
	unsigned int i;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	v4l2_i2c_subdev_init(&s->sd, c, &s5kgw1p_subdev_ops);
	s->sd.internal_ops = &s5kgw1p_internal_ops;

	s->regmap = devm_cci_regmap_init_i2c(c, 16);
	if (IS_ERR(s->regmap))
		return dev_err_probe(dev, PTR_ERR(s->regmap), "regmap init\n");

	ret = s5kgw1p_parse_dt(s, dev);
	if (ret)
		return ret;

	for (i = 0; i < S5KGW1P_NUM_SUPPLIES; i++)
		s->supplies[i].supply = s5kgw1p_supply_names[i];
	ret = devm_regulator_bulk_get(dev, S5KGW1P_NUM_SUPPLIES, s->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "regulators\n");

	ret = s5kgw1p_power_on(dev);
	if (ret)
		return ret;

	/*
	 * The sensor intermittently NAKs the very first I2C access after reset
	 * is released; CCI reports it as a queue timeout (-110) and the probe
	 * would otherwise fail for the whole boot. Retry the chip-id read,
	 * power-cycling the sensor between attempts, before giving up.
	 */
	for (i = 0; ; i++) {
		ret = 0;
		cci_read(s->regmap, S5KGW1P_REG_CHIP_ID, &id, &ret);
		if (!ret && id == S5KGW1P_CHIP_ID)
			break;
		if (i >= S5KGW1P_ID_RETRIES) {
			dev_err(dev, "chip id 0x%04llx (expected 0x%04x) ret %d\n",
				id, S5KGW1P_CHIP_ID, ret);
			ret = ret ? ret : -ENODEV;
			goto err_power;
		}
		dev_dbg(dev, "chip id read failed (0x%04llx ret %d), retrying\n",
			id, ret);
		s5kgw1p_power_off(dev);
		usleep_range(5000, 6000);
		ret = s5kgw1p_power_on(dev);
		if (ret)
			return ret;
	}
	dev_info(dev, "Samsung S5KGW1P detected, chip id 0x%04llx\n", id);

	s->mode = &s5kgw1p_modes[0];
	ret = s5kgw1p_init_ctrls(s);
	if (ret)
		goto err_power;

	s->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto err_ctrl;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev_sensor(&s->sd);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&s->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&s->ctrls);
err_power:
	s5kgw1p_power_off(dev);
	return ret;
}

static void s5kgw1p_remove(struct i2c_client *c)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(c);
	struct s5kgw1p *s = to_s5kgw1p(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&c->dev);
	if (!pm_runtime_status_suspended(&c->dev))
		s5kgw1p_power_off(&c->dev);
	pm_runtime_set_suspended(&c->dev);
}

static const struct dev_pm_ops s5kgw1p_pm_ops = {
	SET_RUNTIME_PM_OPS(s5kgw1p_power_off, s5kgw1p_power_on, NULL)
};

static const struct of_device_id s5kgw1p_of_match[] = {
	{ .compatible = "samsung,s5kgw1p" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5kgw1p_of_match);

static struct i2c_driver s5kgw1p_i2c_driver = {
	.driver = {
		.name = "s5kgw1p",
		.of_match_table = s5kgw1p_of_match,
		.pm = &s5kgw1p_pm_ops,
	},
	.probe = s5kgw1p_probe,
	.remove = s5kgw1p_remove,
};
module_i2c_driver(s5kgw1p_i2c_driver);

MODULE_DESCRIPTION("Samsung S5KGW1P sensor driver");
MODULE_AUTHOR("Miroslav Mráz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
