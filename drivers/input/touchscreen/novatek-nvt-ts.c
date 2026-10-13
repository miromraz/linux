// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Novatek NT11205 i2c touchscreen controller as found
 * on the Acer Iconia One 7 B1-750 tablet.
 *
 * Copyright (c) 2023 Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>

#include <linux/unaligned.h>

#include <drm/drm_panel.h>

#define NVT_TS_TOUCH_START		0x00
#define NVT_TS_TOUCH_SIZE		6

#define NVT_TS_PARAMETERS_START		0x78
/* These are offsets from NVT_TS_PARAMETERS_START */
#define NVT_TS_PARAMS_WIDTH		0x04
#define NVT_TS_PARAMS_HEIGHT		0x06
#define NVT_TS_PARAMS_MAX_TOUCH		0x09
#define NVT_TS_PARAMS_MAX_BUTTONS	0x0a
#define NVT_TS_PARAMS_IRQ_TYPE		0x0b
#define NVT_TS_PARAMS_CHIP_ID		0x0e
#define NVT_TS_PARAMS_SIZE		0x0f

#define NVT_TS_MAX_TOUCHES		10
#define NVT_TS_MAX_SIZE			4096

#define NVT_TS_TOUCH_INVALID		0xff
#define NVT_TS_TOUCH_SLOT_SHIFT		3
#define NVT_TS_TOUCH_TYPE_MASK		GENMASK(2, 0)
#define NVT_TS_TOUCH_NEW		1
#define NVT_TS_TOUCH_UPDATE		2
#define NVT_TS_TOUCH_RELEASE		3

static const int nvt_ts_irq_type[4] = {
	IRQF_TRIGGER_RISING,
	IRQF_TRIGGER_FALLING,
	IRQF_TRIGGER_LOW,
	IRQF_TRIGGER_HIGH
};

struct nvt_ts_i2c_chip_data {
	u8 chip_id;
};

struct nvt_ts_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data regulators[2];
	struct touchscreen_properties prop;
	int max_touches;
	u8 buf[NVT_TS_TOUCH_SIZE * NVT_TS_MAX_TOUCHES];

	/*
	 * For an in-cell TDDI module the touch controller shares the DSI
	 * panel's power and reset, so the DRM panel-follower framework must
	 * drive nvt_ts_start()/nvt_ts_stop() in lockstep with the panel's
	 * prepare/unprepare. @prepared tracks whether the panel is currently
	 * prepared (always true for non-follower boards); it gates the
	 * hardware access in start/stop so the two power sources never fight.
	 */
	struct drm_panel_follower panel_follower;
	bool is_panel_follower;
	bool prepared;
};

static int nvt_ts_read_data(struct i2c_client *client, u8 reg, u8 *data, int count)
{
	struct i2c_msg msg[2] = {
		{
			.addr = client->addr,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = count,
			.buf = data,
		}
	};
	int ret;

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret != ARRAY_SIZE(msg)) {
		dev_err(&client->dev, "Error reading from 0x%02x: %d\n", reg, ret);
		return (ret < 0) ? ret : -EIO;
	}

	return 0;
}

static irqreturn_t nvt_ts_irq(int irq, void *dev_id)
{
	struct nvt_ts_data *data = dev_id;
	struct device *dev = &data->client->dev;
	int i, error, slot, x, y;
	bool active;
	u8 *touch;

	error = nvt_ts_read_data(data->client, NVT_TS_TOUCH_START, data->buf,
				 data->max_touches * NVT_TS_TOUCH_SIZE);
	if (error)
		return IRQ_HANDLED;

	for (i = 0; i < data->max_touches; i++) {
		touch = &data->buf[i * NVT_TS_TOUCH_SIZE];

		if (touch[0] == NVT_TS_TOUCH_INVALID)
			continue;

		slot = touch[0] >> NVT_TS_TOUCH_SLOT_SHIFT;
		if (slot < 1 || slot > data->max_touches) {
			dev_warn(dev, "slot %d out of range, ignoring\n", slot);
			continue;
		}

		switch (touch[0] & NVT_TS_TOUCH_TYPE_MASK) {
		case NVT_TS_TOUCH_NEW:
		case NVT_TS_TOUCH_UPDATE:
			active = true;
			break;
		case NVT_TS_TOUCH_RELEASE:
			active = false;
			break;
		default:
			dev_warn(dev, "slot %d unknown state %d\n", slot, touch[0] & 7);
			continue;
		}

		slot--;
		x = (touch[1] << 4) | (touch[3] >> 4);
		y = (touch[2] << 4) | (touch[3] & 0x0f);

		input_mt_slot(data->input, slot);
		input_mt_report_slot_state(data->input, MT_TOOL_FINGER, active);
		touchscreen_report_pos(data->input, &data->prop, x, y, true);
	}

	input_mt_sync_frame(data->input);
	input_sync(data->input);

	return IRQ_HANDLED;
}

static int nvt_ts_start(struct input_dev *dev)
{
	struct nvt_ts_data *data = input_get_drvdata(dev);
	int error;

	/*
	 * In panel-follower mode the chip is powered by the DSI panel, so
	 * defer the hardware bring-up to nvt_ts_panel_prepared() when the
	 * panel is still unprepared (e.g. the input device is opened while
	 * the display is blanked).
	 */
	if (!data->prepared)
		return 0;

	error = regulator_bulk_enable(ARRAY_SIZE(data->regulators), data->regulators);
	if (error) {
		dev_err(&data->client->dev, "failed to enable regulators\n");
		return error;
	}

	enable_irq(data->client->irq);
	gpiod_set_value_cansleep(data->reset_gpio, 0);

	return 0;
}

static void nvt_ts_stop(struct input_dev *dev)
{
	struct nvt_ts_data *data = input_get_drvdata(dev);

	/* The panel already powered the chip down, nothing to undo. */
	if (!data->prepared)
		return;

	disable_irq(data->client->irq);
	gpiod_set_value_cansleep(data->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(data->regulators), data->regulators);
}

static int nvt_ts_suspend(struct device *dev)
{
	struct nvt_ts_data *data = i2c_get_clientdata(to_i2c_client(dev));

	/* The panel follower powers the chip down via panel_unpreparing(). */
	if (data->is_panel_follower)
		return 0;

	guard(mutex)(&data->input->mutex);

	if (input_device_enabled(data->input))
		nvt_ts_stop(data->input);

	return 0;
}

static int nvt_ts_resume(struct device *dev)
{
	struct nvt_ts_data *data = i2c_get_clientdata(to_i2c_client(dev));

	/* The panel follower powers the chip up via panel_prepared(). */
	if (data->is_panel_follower)
		return 0;

	guard(mutex)(&data->input->mutex);

	if (input_device_enabled(data->input))
		nvt_ts_start(data->input);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nvt_ts_pm_ops, nvt_ts_suspend, nvt_ts_resume);

static int nvt_ts_panel_prepared(struct drm_panel_follower *follower)
{
	struct nvt_ts_data *data =
		container_of(follower, struct nvt_ts_data, panel_follower);

	guard(mutex)(&data->input->mutex);

	data->prepared = true;

	/*
	 * The panel just restored power and reset to the in-cell controller,
	 * re-initialising it. Mirror resume: bring the touch side back up
	 * (reset release + IRQ enable) only if userspace has the device open.
	 */
	if (input_device_enabled(data->input))
		return nvt_ts_start(data->input);

	return 0;
}

static int nvt_ts_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct nvt_ts_data *data =
		container_of(follower, struct nvt_ts_data, panel_follower);

	guard(mutex)(&data->input->mutex);

	/*
	 * The panel is about to cut the shared rails and assert reset. Disable
	 * the IRQ and stop touching the bus before that happens; clear
	 * @prepared so a concurrent input open/close does not poke the chip
	 * while it is powered down.
	 */
	if (input_device_enabled(data->input))
		nvt_ts_stop(data->input);

	data->prepared = false;

	return 0;
}

static const struct drm_panel_follower_funcs nvt_ts_panel_follower_funcs = {
	.panel_prepared = nvt_ts_panel_prepared,
	.panel_unpreparing = nvt_ts_panel_unpreparing,
};

static int nvt_ts_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int error, width, height, irq_type;
	struct nvt_ts_data *data;
	const struct nvt_ts_i2c_chip_data *chip;
	struct input_dev *input;

	if (!client->irq) {
		dev_err(dev, "Error no irq specified\n");
		return -EINVAL;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	chip = device_get_match_data(&client->dev);
	if (!chip)
		return -EINVAL;

	data->client = client;
	i2c_set_clientdata(client, data);

	/*
	 * Non-follower boards power the chip from input open/close, so it is
	 * always considered prepared. Follower boards start unprepared until
	 * the panel reports it (see drm_is_panel_follower() handling below).
	 */
	data->prepared = !drm_is_panel_follower(dev);

	/*
	 * VCC is the analog voltage supply
	 * IOVCC is the digital voltage supply
	 */
	data->regulators[0].supply = "vcc";
	data->regulators[1].supply = "iovcc";
	error = devm_regulator_bulk_get(dev, ARRAY_SIZE(data->regulators), data->regulators);
	if (error) {
		dev_err(dev, "cannot get regulators: %d\n", error);
		return error;
	}

	error = regulator_bulk_enable(ARRAY_SIZE(data->regulators), data->regulators);
	if (error) {
		dev_err(dev, "failed to enable regulators: %d\n", error);
		return error;
	}

	data->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	error = PTR_ERR_OR_ZERO(data->reset_gpio);
	if (error) {
		regulator_bulk_disable(ARRAY_SIZE(data->regulators), data->regulators);
		dev_err(dev, "failed to request reset GPIO: %d\n", error);
		return error;
	}

	/* Wait for controller to come out of reset before params read */
	msleep(100);
	error = nvt_ts_read_data(data->client, NVT_TS_PARAMETERS_START,
				 data->buf, NVT_TS_PARAMS_SIZE);
	gpiod_set_value_cansleep(data->reset_gpio, 1); /* Put back in reset */
	regulator_bulk_disable(ARRAY_SIZE(data->regulators), data->regulators);
	if (error)
		return error;

	width  = get_unaligned_be16(&data->buf[NVT_TS_PARAMS_WIDTH]);
	height = get_unaligned_be16(&data->buf[NVT_TS_PARAMS_HEIGHT]);
	data->max_touches = data->buf[NVT_TS_PARAMS_MAX_TOUCH];
	irq_type = data->buf[NVT_TS_PARAMS_IRQ_TYPE];

	if (width > NVT_TS_MAX_SIZE || height >= NVT_TS_MAX_SIZE ||
	    data->max_touches > NVT_TS_MAX_TOUCHES ||
	    irq_type >= ARRAY_SIZE(nvt_ts_irq_type) ||
	    data->buf[NVT_TS_PARAMS_CHIP_ID] != chip->chip_id) {
		dev_err(dev, "Unsupported touchscreen parameters: %*ph\n",
			NVT_TS_PARAMS_SIZE, data->buf);
		return -EIO;
	}

	dev_dbg(dev, "Detected %dx%d touchscreen with %d max touches\n",
		width, height, data->max_touches);

	if (data->buf[NVT_TS_PARAMS_MAX_BUTTONS])
		dev_warn(dev, "Touchscreen buttons are not supported\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = client->name;
	input->id.bustype = BUS_I2C;
	input->open = nvt_ts_start;
	input->close = nvt_ts_stop;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, width - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, height - 1, 0, 0);
	touchscreen_parse_properties(input, true, &data->prop);

	error = input_mt_init_slots(input, data->max_touches,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	data->input = input;
	input_set_drvdata(input, data);

	error = devm_request_threaded_irq(dev, client->irq, NULL, nvt_ts_irq,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN |
						nvt_ts_irq_type[irq_type],
					  client->name, data);
	if (error) {
		dev_err(dev, "failed to request irq: %d\n", error);
		return error;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "failed to register input device: %d\n", error);
		return error;
	}

	/*
	 * An in-cell TDDI module (e.g. the NT36772 on the Planet Gemini PDA)
	 * is powered and reset by its DSI panel, so register as a panel
	 * follower to be told when the panel powers the chip down and back up.
	 * drm_panel_add_follower() calls panel_prepared() synchronously if the
	 * panel is already prepared, which sets @prepared. Boards without a
	 * "panel" phandle keep the input-open driven power sequencing.
	 */
	if (drm_is_panel_follower(dev)) {
		data->panel_follower.funcs = &nvt_ts_panel_follower_funcs;
		error = devm_drm_panel_add_follower(dev, &data->panel_follower);
		if (!error) {
			data->is_panel_follower = true;
		} else if (error == -ENODEV) {
			/* Not actually a follower, fall back to legacy power. */
			data->prepared = true;
		} else {
			return error;
		}
	}

	return 0;
}

static const struct nvt_ts_i2c_chip_data nvt_nt11205_ts_data = {
	.chip_id = 0x05,
};

static const struct nvt_ts_i2c_chip_data nvt_nt36672a_ts_data = {
	.chip_id = 0x08,
};

/*
 * NT36772 (Planet Gemini PDA). It uses the same 6-byte touch record and the
 * parameter block at 0x78 as the NT36672A, served at the FW I2C address 0x01.
 * Its parameter block reports chip id 0x08, as the NT36672A does.
 */
static const struct nvt_ts_i2c_chip_data nvt_nt36772_ts_data = {
	.chip_id = 0x08,
};

static const struct of_device_id nvt_ts_of_match[] = {
	{ .compatible = "novatek,nt11205-ts", .data = &nvt_nt11205_ts_data },
	{ .compatible = "novatek,nt36672a-ts", .data = &nvt_nt36672a_ts_data },
	{ .compatible = "novatek,nt36772-ts", .data = &nvt_nt36772_ts_data },
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_ts_of_match);

static const struct i2c_device_id nvt_ts_i2c_id[] = {
	{ "nt11205-ts", (unsigned long) &nvt_nt11205_ts_data },
	{ "nt36672a-ts", (unsigned long) &nvt_nt36672a_ts_data },
	{ "nt36772-ts", (unsigned long) &nvt_nt36772_ts_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, nvt_ts_i2c_id);

static struct i2c_driver nvt_ts_driver = {
	.driver = {
		.name	= "novatek-nvt-ts",
		.pm	= pm_sleep_ptr(&nvt_ts_pm_ops),
		.of_match_table = nvt_ts_of_match,
	},
	.probe = nvt_ts_probe,
	.id_table = nvt_ts_i2c_id,
};

module_i2c_driver(nvt_ts_driver);

MODULE_DESCRIPTION("Novatek NT11205 touchscreen driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
