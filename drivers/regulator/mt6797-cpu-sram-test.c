// SPDX-License-Identifier: GPL-2.0-only
/* Included by the driver: actual full-bank adapter over faulting transport. */
#include <kunit/test.h>

struct sram_test_bus {
	struct kunit *test;
	unsigned int words[3];
	int calls, fail;
	bool persistent;
};

static int sram_test_read(void *data, unsigned int reg, unsigned int *val)
{
	struct sram_test_bus *b = data;

	if (++b->calls == b->fail || (b->persistent && b->calls > b->fail))
		return -EIO;
	*val = b->words[(reg - MT6797_CPULDO_CTRL_0) / 4];
	return 0;
}

static int sram_test_write(void *data, unsigned int reg, unsigned int val)
{
	struct sram_test_bus *b = data;
	unsigned int i, code;

	if (++b->calls == b->fail || (b->persistent && b->calls > b->fail))
		return -EIO;
	KUNIT_EXPECT_NE(b->test, reg, MT6797_CPULDO_CTRL_0);
	KUNIT_EXPECT_EQ(b->test, val & 0xf0f0f0f0, 0xa0a0a0a0U);
	for (i = 0; i < 4; i++) {
		code = (val >> (i * 8)) & 15;
		/* Both old 1.10 V and new 1.125 V fit proc 1.04 V. */
		KUNIT_EXPECT_GE(b->test, code, 11U);
		KUNIT_EXPECT_LE(b->test, code, 12U);
	}
	b->words[(reg - MT6797_CPULDO_CTRL_0) / 4] = val;
	KUNIT_EXPECT_EQ(b->test, b->words[0], 255U);
	return 0;
}

static void sram_adapter_faults(struct kunit *test)
{
	const struct regmap_config config = {
		.reg_bits = 32, .val_bits = 32, .reg_stride = 4,
		.max_register = MT6797_CPULDO_CTRL_2,
		.reg_read = sram_test_read, .reg_write = sram_test_write,
	};
	struct sram_test_bus *b;
	struct mt6797_cpu_sram sram = {};
	struct platform_device *pdev;
	int ret, count, fail;

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	b->test = test;
	pdev = platform_device_register_simple("sram-adapter-test", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	sram.dev = &pdev->dev;
	sram.regmap = regmap_init(sram.dev, NULL, b, &config);
	if (IS_ERR(sram.regmap)) {
		KUNIT_FAIL(test, "regmap allocation failed");
		platform_device_unregister(pdev);
		return;
	}
	mutex_init(&sram.rail_lock);
	b->words[0] = 255;
	b->words[1] = b->words[2] = 0xabababab;
	KUNIT_EXPECT_EQ(test, mt6797_cpu_sram_owned_write(&sram, 1125000), 0);
	count = b->calls;
	KUNIT_EXPECT_EQ(test, mt6797_cpu_sram_owned_read(&sram), 1125000);
	/* Every transport boundary on the successful raw write, including its
	 * precondition reads, masked updates and checked readback. One-shot
	 * errors permit bounded restoration; repeated faults are containment.
	 */
	for (fail = 1; fail <= count; fail++) {
		b->words[1] = b->words[2] = 0xabababab;
		b->calls = 0;
		b->fail = fail;
		ret = mt6797_cpu_sram_owned_write(&sram, 1125000);
		KUNIT_EXPECT_LT(test, ret, 0);
		KUNIT_EXPECT_EQ(test, b->words[1], 0xababababU);
		KUNIT_EXPECT_EQ(test, b->words[2], 0xababababU);
		KUNIT_EXPECT_FALSE(test, sram.unresolved);
	}
	b->fail = 0;
	b->words[0] = 127;
	KUNIT_EXPECT_EQ(test, mt6797_cpu_sram_owned_write(&sram, 1125000), -EIO);
	b->words[0] = 255;
	b->words[1] = b->words[2] = 0xabababab;
	b->calls = 0;
	b->fail = 9;
	b->persistent = true;
	KUNIT_EXPECT_EQ(test, mt6797_cpu_sram_owned_write(&sram, 1125000), -EIO);
	KUNIT_EXPECT_TRUE(test, sram.unresolved);
	b->persistent = false;
	b->fail = 0;
	KUNIT_EXPECT_EQ(test, mt6797_cpu_sram_owned_read(&sram), -EIO);
	regmap_exit(sram.regmap);
	platform_device_unregister(pdev);
}

static struct kunit_case sram_adapter_cases[] = {
	KUNIT_CASE(sram_adapter_faults),
	{}
};

static struct kunit_suite sram_adapter_suite = {
	.name = "mt6797-sram-adapter",
	.test_cases = sram_adapter_cases,
};
kunit_test_suite(sram_adapter_suite);
