// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Awinic AW9120 20-channel I2C LED controller.
 *
 * Copyright (C) 2026 Miroslav Mraz <miroslav.mraz@techmania.cz>
 *
 * The chip exposes 20 constant-current LED outputs behind an 8-bit register
 * address / 16-bit big-endian data I2C interface. Each output is controlled
 * independently: the per-channel drive current lives in the IMAX registers and
 * the per-channel PWM duty is written through the command register (CMDR) with
 * a single "set PWM" instruction. This driver exposes one LED class device per
 * output and maps brightness directly onto that 8-bit PWM value.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define AW9120_MAX_LEDS		20

/* Reset / chip-id register; reads back the chip id. */
#define AW9120_REG_RSTR		0x00
#define AW9120_CHIP_ID		0xb223

/* Global control register. */
#define AW9120_REG_GCR		0x01
#define AW9120_GCR_ENABLE	BIT(0)

/* LED enable registers: LER1 outputs 0-11, LER2 outputs 12-19. */
#define AW9120_REG_LER1		0x50
#define AW9120_REG_LER2		0x51
#define AW9120_LER1_ALL		0x0fff
#define AW9120_LER2_ALL		0x00ff

/* Control-source select: 1 = I2C/command register, 0 = internal SRAM. */
#define AW9120_REG_CTRS1	0x55
#define AW9120_REG_CTRS2	0x56

/* Per-channel max-current registers, four 4-bit nibbles each. */
#define AW9120_REG_IMAX1	0x57
#define AW9120_IMAX_DEFAULT	0x1111

/* Command register: executes one instruction immediately. */
#define AW9120_REG_CMDR		0x61
#define AW9120_CMD_SETPWM	0xa000
#define AW9120_CMD_CHAN(x)	((x) << 8)

#define AW9120_REG_MAX		0x7f

struct aw9120_led {
	struct led_classdev cdev;
	struct regmap *regmap;
	u32 channel;
};

static int aw9120_brightness_set(struct led_classdev *cdev,
				 enum led_brightness brightness)
{
	struct aw9120_led *led = container_of(cdev, struct aw9120_led, cdev);

	return regmap_write(led->regmap, AW9120_REG_CMDR,
			    AW9120_CMD_SETPWM |
			    AW9120_CMD_CHAN(led->channel) | brightness);
}

static int aw9120_chip_init(struct regmap *regmap)
{
	static const struct reg_sequence init_seq[] = {
		{ AW9120_REG_GCR, 0 },
		{ AW9120_REG_IMAX1 + 0, AW9120_IMAX_DEFAULT },
		{ AW9120_REG_IMAX1 + 1, AW9120_IMAX_DEFAULT },
		{ AW9120_REG_IMAX1 + 2, AW9120_IMAX_DEFAULT },
		{ AW9120_REG_IMAX1 + 3, AW9120_IMAX_DEFAULT },
		{ AW9120_REG_IMAX1 + 4, AW9120_IMAX_DEFAULT },
		{ AW9120_REG_LER1, AW9120_LER1_ALL },
		{ AW9120_REG_LER2, AW9120_LER2_ALL },
		{ AW9120_REG_CTRS1, AW9120_LER1_ALL },
		{ AW9120_REG_CTRS2, AW9120_LER2_ALL },
		{ AW9120_REG_GCR, AW9120_GCR_ENABLE },
	};

	return regmap_multi_reg_write(regmap, init_seq, ARRAY_SIZE(init_seq));
}

static void aw9120_power_off(void *data)
{
	struct gpio_desc *enable_gpio = data;

	gpiod_set_value_cansleep(enable_gpio, 0);
}

static void aw9120_regulator_disable(void *data)
{
	regulator_disable(data);
}

static int aw9120_probe_dt(struct device *dev, struct regmap *regmap)
{
	int count, ret;

	count = device_get_child_node_count(dev);
	if (!count || count > AW9120_MAX_LEDS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid LED node count: %d\n", count);

	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init_data = { .fwnode = child };
		struct aw9120_led *led;

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		ret = fwnode_property_read_u32(child, "reg", &led->channel);
		if (ret || led->channel >= AW9120_MAX_LEDS)
			return dev_err_probe(dev, ret ? ret : -EINVAL,
					     "invalid LED channel\n");

		led->regmap = regmap;
		led->cdev.max_brightness = 0xff;
		led->cdev.brightness_set_blocking = aw9120_brightness_set;

		ret = devm_led_classdev_register_ext(dev, &led->cdev,
						     &init_data);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register LED %u\n",
					     led->channel);
	}

	return 0;
}

static const struct regmap_config aw9120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW9120_REG_MAX,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static int aw9120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *enable_gpio;
	struct regulator *vcc;
	struct regmap *regmap;
	unsigned int chip_id;
	int ret;

	regmap = devm_regmap_init_i2c(client, &aw9120_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to init regmap\n");

	vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(vcc))
		return dev_err_probe(dev, PTR_ERR(vcc), "failed to get vcc\n");

	ret = regulator_enable(vcc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vcc\n");

	ret = devm_add_action_or_reset(dev, aw9120_regulator_disable, vcc);
	if (ret)
		return ret;

	/* Optional PDN pin: pulse low then high to reset and power the chip. */
	enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(enable_gpio))
		return dev_err_probe(dev, PTR_ERR(enable_gpio),
				     "failed to get enable gpio\n");
	if (enable_gpio) {
		usleep_range(5000, 6000);
		gpiod_set_value_cansleep(enable_gpio, 1);
		usleep_range(5000, 6000);

		ret = devm_add_action_or_reset(dev, aw9120_power_off,
					       enable_gpio);
		if (ret)
			return ret;
	}

	ret = regmap_read(regmap, AW9120_REG_RSTR, &chip_id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id\n");
	if (chip_id != AW9120_CHIP_ID)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip id 0x%04x\n", chip_id);

	ret = aw9120_chip_init(regmap);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init chip\n");

	return aw9120_probe_dt(dev, regmap);
}

static const struct of_device_id aw9120_of_match[] = {
	{ .compatible = "awinic,aw9120" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9120_of_match);

static struct i2c_driver aw9120_driver = {
	.driver = {
		.name = "leds-aw9120",
		.of_match_table = aw9120_of_match,
	},
	.probe = aw9120_probe,
};
module_i2c_driver(aw9120_driver);

MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_DESCRIPTION("Awinic AW9120 20-channel LED controller driver");
MODULE_LICENSE("GPL");
