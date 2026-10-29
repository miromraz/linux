// SPDX-License-Identifier: GPL-2.0-only
/* Included to exercise the actual child probe and callbacks with regulator core. */
#include <kunit/test.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>

struct da9211_test_bus {
	struct kunit *test;
	unsigned int regs[512];
	unsigned int a_writes;
	int a_error;
	bool unguarded;
	int mask_error;
	int ack_error;
};

static int da9211_test_read(void *context, unsigned int reg, unsigned int *val)
{
	struct da9211_test_bus *bus = context;

	if (reg == DA9211_REG_BUCKA_CONT && bus->a_error)
		return bus->a_error;
	*val = bus->regs[reg];
	return 0;
}

static int da9211_test_write(void *context, unsigned int reg, unsigned int val)
{
	struct da9211_test_bus *bus = context;

	if (reg == DA9211_REG_MASK_B && bus->mask_error)
		return bus->mask_error;
	if (reg == DA9211_REG_EVENT_B && val == DA9211_E_OV_CURR_A && bus->ack_error)
		return bus->ack_error;
	if (reg == DA9211_REG_VBUCKA_A || reg == DA9211_REG_BUCKA_CONT)
		bus->a_writes++;
	/* No registration/peer operation may disturb the protected live A. */
	if (!bus->unguarded)
		KUNIT_EXPECT_EQ(bus->test, bus->a_writes, 0U);
	if (reg == DA9211_REG_EVENT_B)
		bus->regs[reg] &= ~val; /* W1C */
	else
		bus->regs[reg] = val;
	return 0;
}

static const struct regmap_config da9211_test_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 511,
	.reg_read = da9211_test_read,
	.reg_write = da9211_test_write,
};

struct da9211_test_notifier {
	struct notifier_block nb;
	int count;
};

static int da9211_test_notify(struct notifier_block *nb, unsigned long event,
			      void *data)
{
	struct da9211_test_notifier *n = container_of(nb,
					struct da9211_test_notifier, nb);

	if (event == REGULATOR_EVENT_OVER_CURRENT)
		n->count++;
	return NOTIFY_OK;
}

static void da9211_test_independent(struct kunit *test)
{
	struct da9211_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct regulator_consumer_supply supply =
		REGULATOR_SUPPLY("B", "da9211-test-parent");
	struct regulator_init_data init[2] = {
		{ .constraints = { .min_uV = 1200000, .max_uV = 1200000,
			.apply_uV = true } },
		{ .constraints = { .min_uV = 1000000, .max_uV = 1200000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE },
		  .num_consumer_supplies = 1, .consumer_supplies = &supply },
	};
	struct da9211_pdata pdata = { .num_buck = 2,
		.init_data = { &init[0], &init[1] } };
	struct da9211 chip = { .pdata = &pdata, .boot_barrier = true };
	struct platform_device *parent, *a, *b;
	struct regulator *consumer;
	struct da9211_test_notifier notifier = { .nb.notifier_call = da9211_test_notify };
	int id, ret;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->test = test;
	bus->regs[DA9211_REG_BUCKA_CONT] = DA9211_BUCKA_EN;
	bus->regs[DA9211_REG_BUCKB_CONT] = DA9211_BUCKA_EN;
	bus->regs[DA9211_REG_VBUCKA_A] = 85; /* 1.15 V */
	bus->regs[DA9211_REG_VBUCKB_A] = 85;
	parent = platform_device_register_simple("da9211-test-parent", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(parent));
	mutex_init(&chip.irq_lock);
	INIT_DELAYED_WORK(&chip.mask_work, da9211_mask_recover);
	mutex_init(&chip.gpio_lock);
	chip.dev = &parent->dev;
	dev_set_drvdata(chip.dev, &chip);
	chip.regmap = regmap_init(chip.dev, NULL, bus, &da9211_test_regmap);
	KUNIT_EXPECT_FALSE(test, IS_ERR(chip.regmap));
	if (IS_ERR(chip.regmap))
		goto parent_out;
	id = 0;
	a = platform_device_register_data(chip.dev, "da9211-buck",
					PLATFORM_DEVID_AUTO, &id, sizeof(id));
	KUNIT_EXPECT_FALSE(test, IS_ERR(a));
	if (IS_ERR(a))
		goto map_out;
	KUNIT_EXPECT_PTR_EQ(test, chip.rdev[0], NULL);
	id = 1;
	b = platform_device_register_data(chip.dev, "da9211-buck",
					PLATFORM_DEVID_AUTO, &id, sizeof(id));
	KUNIT_EXPECT_FALSE(test, IS_ERR(b));
	if (IS_ERR(b))
		goto a_out;
	KUNIT_EXPECT_NOT_NULL(test, chip.rdev[1]);
	consumer = regulator_get(chip.dev, "B");
	KUNIT_EXPECT_FALSE(test, IS_ERR(consumer));
	if (IS_ERR(consumer))
		goto b_out;
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(consumer, 1100000, 1100000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(consumer), 1100000);
	/* Downward normalization must also reject without touching A or B. */
	init[0].constraints.min_uV = 1100000;
	init[0].constraints.max_uV = 1100000;
	ret = device_reprobe(&a->dev);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, chip.rdev[0], NULL);
	/* Supplied GPIO must be rejected before core can dereference it. */
	pdata.gpiod_ren[0] = (struct gpio_desc *)&chip;
	KUNIT_EXPECT_EQ(test, da9211_buck_probe(a), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_PTR_EQ(test, a->dev.driver, NULL);
	pdata.gpiod_ren[0] = NULL;
#if IS_ENABLED(CONFIG_OF)
	{
		struct property property = { .name = "enable-gpio" };
		struct device_node node = { .properties = &property };

		pdata.reg_node[0] = &node;
		KUNIT_EXPECT_EQ(test, da9211_buck_probe(a), -EOPNOTSUPP);
		property.name = "enable-gpios";
		KUNIT_EXPECT_EQ(test, da9211_buck_probe(a), -EOPNOTSUPP);
		pdata.reg_node[0] = NULL;
	}
#endif
	bus->a_error = -EPROBE_DEFER;
	KUNIT_EXPECT_EQ(test, da9211_buck_probe(a), -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_PTR_EQ(test, chip.rdev[0], NULL);
	bus->a_error = -EIO;
	KUNIT_EXPECT_EQ(test, da9211_buck_probe(a), -EIO);
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_PTR_EQ(test, chip.rdev[0], NULL);
	bus->a_error = 0;
	init[0].constraints.min_uV = 1040000;
	init[0].constraints.max_uV = 1200000;
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_NOT_NULL(test, chip.rdev[0]);
	device_release_driver(&a->dev);
	KUNIT_EXPECT_EQ(test, regulator_register_notifier(consumer, &notifier.nb), 0);
	bus->ack_error = -EIO;
	bus->regs[DA9211_REG_EVENT_B] = DA9211_E_OV_CURR_A | DA9211_E_OV_CURR_B;
	KUNIT_EXPECT_EQ(test, da9211_irq_handler(0, &chip), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, notifier.count, 1);
	KUNIT_EXPECT_EQ(test, bus->regs[DA9211_REG_EVENT_B], DA9211_E_OV_CURR_A);
	KUNIT_EXPECT_EQ(test, regulator_unregister_notifier(consumer, &notifier.nb), 0);
	bus->ack_error = 0;
	bus->mask_error = -EIO;
	KUNIT_EXPECT_EQ(test, da9211_buck_mask(&chip, 0, true), -EIO);
	cancel_delayed_work_sync(&chip.mask_work);
	KUNIT_EXPECT_EQ(test, chip.pending_mask, (unsigned int)DA9211_M_OV_CURR_A);
	bus->mask_error = 0;
	da9211_mask_recover(&chip.mask_work.work);
	KUNIT_EXPECT_EQ(test, chip.pending_mask, 0U);
	KUNIT_EXPECT_TRUE(test, bus->regs[DA9211_REG_MASK_B] & DA9211_M_OV_CURR_A);
	KUNIT_EXPECT_PTR_EQ(test, chip.rdev[0], NULL);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(consumer, 1200000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(consumer), 1200000);
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_EQ(test, bus->a_writes, 0U);
	regulator_put(consumer);
b_out:
	platform_device_unregister(b);
a_out:
	platform_device_unregister(a);
map_out:
	regmap_exit(chip.regmap);
parent_out:
	platform_device_unregister(parent);
}

/* Exercise the parent registration route with sparse physical presence. */
static void da9211_test_sparse(struct kunit *test)
{
	struct da9211_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct regulator_init_data init = {
		.constraints = { .min_uV = 1000000, .max_uV = 1200000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE },
	};
	struct da9211_pdata pdata = { .num_buck = 1,
		.init_data = { &init, &init } };
	struct da9211 chip = { .pdata = &pdata };
	struct platform_device *parent;
	void *group;
	int id;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->unguarded = true;
	bus->regs[DA9211_REG_CONFIG_E] = DA9211_SLAVE_SEL;
	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->test = test;
	bus->regs[DA9211_REG_BUCKA_CONT] = DA9211_BUCKA_EN;
	bus->regs[DA9211_REG_BUCKB_CONT] = DA9211_BUCKA_EN;
	bus->regs[DA9211_REG_VBUCKA_A] = 85;
	bus->regs[DA9211_REG_VBUCKB_A] = 85;
	parent = platform_device_register_simple("da9211-test-sparse", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(parent));
	mutex_init(&chip.irq_lock);
	INIT_DELAYED_WORK(&chip.mask_work, da9211_mask_recover);
	mutex_init(&chip.gpio_lock);
	chip.dev = &parent->dev;
	dev_set_drvdata(chip.dev, &chip);
	chip.regmap = regmap_init(chip.dev, NULL, bus, &da9211_test_regmap);
	KUNIT_EXPECT_FALSE(test, IS_ERR(chip.regmap));
	if (IS_ERR(chip.regmap))
		goto out;
	for (id = 1; id >= 0; id--) {
		group = devres_open_group(chip.dev, NULL, GFP_KERNEL);
		KUNIT_EXPECT_NOT_NULL(test, group);
		if (!group)
			break;
		chip.present = BIT(id);
		KUNIT_EXPECT_EQ(test, da9211_regulator_init(&chip), 0);
		KUNIT_EXPECT_NOT_NULL(test, chip.rdev[id]);
		KUNIT_EXPECT_PTR_EQ(test, chip.rdev[id ^ 1], NULL);
		if (chip.rdev[id]) {
			KUNIT_EXPECT_EQ(test, rdev_get_id(chip.rdev[id]), id);
			/* Non-Gemini A and independent B retain ordinary callbacks. */
			KUNIT_EXPECT_EQ(test, da9211_set_voltage_sel(chip.rdev[id], 80), 0);
			KUNIT_EXPECT_EQ(test, da9211_disable(chip.rdev[id]), 0);
			KUNIT_EXPECT_EQ(test, da9211_enable(chip.rdev[id]), 0);
		}
		devres_release_group(chip.dev, group);
		KUNIT_EXPECT_PTR_EQ(test, chip.rdev[id], NULL);
	}
	chip.present = BIT(1);
	bus->regs[DA9211_REG_CONFIG_E] = 0;
	KUNIT_EXPECT_EQ(test, da9211_regulator_init(&chip), -EINVAL);
	chip.present = 0;
	KUNIT_EXPECT_EQ(test, da9211_regulator_init(&chip), -ENODEV);
	regmap_exit(chip.regmap);
out:
	platform_device_unregister(parent);
}

struct da9211_test_gpio {
	struct gpio_chip chip;
	int value;
};

static int da9211_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct da9211_test_gpio *gpio = gpiochip_get_data(gc);

	return gpio->value;
}

static int da9211_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct da9211_test_gpio *gpio = gpiochip_get_data(gc);

	gpio->value = value;
	return 0;
}

static void da9211_test_supplied_gpio(struct kunit *test)
{
	struct da9211_test_gpio gpio = { .chip = {
		.label = "da9211-test-gpio", .base = -1, .ngpio = 1,
		.get = da9211_gpio_get, .set = da9211_gpio_set,
		.direction_output = da9211_gpio_set,
	} };
	struct da9211_test_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct regulator_init_data init = { .constraints = {
		.min_uV = 1000000, .max_uV = 1200000,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	} };
	struct da9211_pdata pdata = { .init_data = { &init, &init } };
	struct da9211 chip = { .pdata = &pdata };
	struct platform_device *parent, *a;
	struct gpio_desc *desc;
	int id = 0;

	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_EQ(test, gpiochip_add_data(&gpio.chip, &gpio), 0);
	desc = gpiochip_request_own_desc(&gpio.chip, 0, "da9211-test",
				       GPIO_ACTIVE_HIGH, GPIOD_OUT_HIGH);
	KUNIT_EXPECT_FALSE(test, IS_ERR(desc));
	if (IS_ERR(desc))
		goto gpio_out;
	pdata.gpiod_ren[0] = desc;
	bus->test = test;
	bus->unguarded = true;
	bus->regs[DA9211_REG_VBUCKA_A] = 85;
	parent = platform_device_register_simple("da9211-test-gpio-parent", -1, NULL, 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR(parent));
	if (IS_ERR(parent))
		goto desc_out;
	chip.dev = &parent->dev;
	mutex_init(&chip.irq_lock);
	INIT_DELAYED_WORK(&chip.mask_work, da9211_mask_recover);
	mutex_init(&chip.gpio_lock);
	dev_set_drvdata(chip.dev, &chip);
	chip.regmap = regmap_init(chip.dev, NULL, bus, &da9211_test_regmap);
	KUNIT_EXPECT_FALSE(test, IS_ERR(chip.regmap));
	if (IS_ERR(chip.regmap))
		goto parent_out;
	a = platform_device_register_data(chip.dev, "da9211-buck",
					 PLATFORM_DEVID_AUTO, &id, sizeof(id));
	KUNIT_EXPECT_FALSE(test, IS_ERR(a));
	if (IS_ERR(a))
		goto map_out;
	KUNIT_EXPECT_NOT_NULL(test, chip.rdev[0]);
	if (chip.rdev[0]) {
		KUNIT_EXPECT_EQ(test, da9211_disable(chip.rdev[0]), 0);
		KUNIT_EXPECT_EQ(test, gpio.value, 0);
		KUNIT_EXPECT_EQ(test, da9211_enable(chip.rdev[0]), 0);
	}
	device_release_driver(&a->dev);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(desc), 1);
	KUNIT_EXPECT_EQ(test, device_reprobe(&a->dev), 0);
	KUNIT_EXPECT_NOT_NULL(test, chip.rdev[0]);
	platform_device_unregister(a);
	/* The last regulator disappearing must not consume the caller's GPIO. */
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(desc, 0), 0);
	KUNIT_EXPECT_EQ(test, gpio.value, 0);
map_out:
	regmap_exit(chip.regmap);
parent_out:
	platform_device_unregister(parent);
desc_out:
	gpiochip_free_own_desc(desc);
gpio_out:
	gpiochip_remove(&gpio.chip);
}

static struct kunit_case da9211_cases[] = {
	KUNIT_CASE(da9211_test_independent),
	KUNIT_CASE(da9211_test_sparse),
	KUNIT_CASE(da9211_test_supplied_gpio),
	{}
};

static struct kunit_suite da9211_suite = {
	.name = "da9211-registration",
	.test_cases = da9211_cases,
};
kunit_test_suite(da9211_suite);
