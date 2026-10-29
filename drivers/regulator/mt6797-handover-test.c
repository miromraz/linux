// SPDX-License-Identifier: GPL-2.0-only
/* Real regulator-core tests with a synthetic, failure-atomic provider.
 * This is not a model of MT6797 MMIO, clocks, or the boot handover driver.
 */
#include <kunit/test.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/platform_device.h>

struct handover_test {
	struct kunit *test;
	unsigned int sel;
	unsigned int writes;
	int enable_error;
	bool fail;
};

static int handover_get(struct regulator_dev *rdev)
{
	struct handover_test *ctx = rdev_get_drvdata(rdev);

	return ctx->sel;
}

static int handover_set(struct regulator_dev *rdev, unsigned int sel)
{
	struct handover_test *ctx = rdev_get_drvdata(rdev);
	int uv = regulator_list_voltage_linear(rdev, sel);

	if (ctx->fail)
		return -EIO;
	/* Fixed SRAM is 1.20 V in this fixture; check every physical write. */
	KUNIT_EXPECT_GE(ctx->test, uv, 1040000);
	KUNIT_EXPECT_LE(ctx->test, uv, 1200000);
	ctx->sel = sel;
	ctx->writes++;
	return 0;
}

static int handover_enable(struct regulator_dev *rdev)
{
	struct handover_test *ctx = rdev_get_drvdata(rdev);

	return ctx->enable_error;
}

static const struct regulator_ops handover_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = handover_get,
	.set_voltage_sel = handover_set,
	.enable = handover_enable,
};

static const struct regulator_desc handover_desc = {
	.name = "handover-test",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &handover_ops,
	.min_uV = 1000000,
	.uV_step = 10000,
	.n_voltages = 21,
};

static void handover_registration(struct kunit *test)
{
	struct handover_test ctx = { .test = test, .enable_error = -EIO };
	struct regulator_init_data init = {
		.constraints = { .always_on = true },
	};
	struct regulator_config config = { .init_data = &init, .driver_data = &ctx };
	struct platform_device *pdev;
	struct regulator_dev *rdev;

	pdev = platform_device_register_simple("handover-registration", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	config.dev = &pdev->dev;
	/* No voltage constraints: enable rejection must fail registration. */
	rdev = regulator_register(config.dev, &handover_desc, &config);
	KUNIT_EXPECT_TRUE(test, IS_ERR(rdev));
	if (IS_ERR(rdev))
		KUNIT_EXPECT_EQ(test, PTR_ERR(rdev), -EIO);
	else
		regulator_unregister(rdev);
	/* Document the core's intentional missing-enable-operation exception. */
	ctx.enable_error = -EINVAL;
	rdev = regulator_register(config.dev, &handover_desc, &config);
	KUNIT_EXPECT_FALSE(test, IS_ERR(rdev));
	if (!IS_ERR(rdev))
		regulator_unregister(rdev);
	platform_device_unregister(pdev);
}

static void handover_arbitration_grid(struct kunit *test, int step)
{
	struct regulator_desc desc = handover_desc;
	struct handover_test ctx = { .test = test, .sel = 20 };
	struct regulator_consumer_supply supplies[] = {
		REGULATOR_SUPPLY("cpu0", "handover-consumers"),
		REGULATOR_SUPPLY("cpu4", "handover-consumers"),
		REGULATOR_SUPPLY("cci", "handover-consumers"),
	};
	struct regulator_init_data init = {
		.constraints = {
			.min_uV = 1040000, .max_uV = 1200000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		},
		.num_consumer_supplies = ARRAY_SIZE(supplies),
		.consumer_supplies = supplies,
	};
	struct regulator_config config = { .init_data = &init, .driver_data = &ctx };
	struct platform_device *pdev;
	struct regulator_dev *rdev;
	struct regulator *users[3];
	unsigned int writes;
	int i;

	desc.uV_step = step;
	desc.n_voltages = 200000 / step + 1;
	ctx.sel = desc.n_voltages - 1;
	pdev = platform_device_register_simple("handover-consumers", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	config.dev = &pdev->dev;
	rdev = regulator_register(config.dev, &desc, &config);
	KUNIT_EXPECT_FALSE(test, IS_ERR(rdev));
	if (IS_ERR(rdev))
		goto device;
	for (i = 0; i < 3; i++) {
		users[i] = regulator_get(&pdev->dev, supplies[i].supply);
		KUNIT_EXPECT_FALSE(test, IS_ERR(users[i]));
		if (IS_ERR(users[i]))
			goto consumers;
	}
	/* Establish every user's own vote even when the aggregate is higher. */
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[0], 1200000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[1], 1101000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[2], 1141000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[0], 1040000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(users[0]), 1150000);
	ctx.fail = true;
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[2], 1180000, 1200000), -EIO);
	ctx.fail = false;
	/* Repeating the old request after failure is a core no-op. */
	writes = ctx.writes;
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[2], 1141000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, ctx.writes, writes);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[1], 1161000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(users[0]),
			1000000 + DIV_ROUND_UP(161000, step) * step);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[1], 1040000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(users[0]), 1150000);
	/* A failed vote did not replace CCI's 1.141 V floor. */
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[2], 1040000, 1200000), 0);
	KUNIT_EXPECT_EQ(test, regulator_get_voltage(users[0]),
			1000000 + DIV_ROUND_UP(40000, step) * step);
	KUNIT_EXPECT_EQ(test, regulator_set_voltage(users[0], 1200001, 1300000), -EINVAL);
consumers:
	while (i--)
		regulator_put(users[i]);
	regulator_unregister(rdev);
device:
	platform_device_unregister(pdev);
}

static void handover_arbitration(struct kunit *test)
{
	handover_arbitration_grid(test, 10000);
	handover_arbitration_grid(test, 25000);
}

static struct kunit_case handover_cases[] = {
	KUNIT_CASE(handover_registration),
	KUNIT_CASE(handover_arbitration),
	{}
};

static struct kunit_suite handover_suite = {
	.name = "mt6797-regulator-core",
	.test_cases = handover_cases,
};

kunit_test_suite(handover_suite);
MODULE_LICENSE("GPL");
