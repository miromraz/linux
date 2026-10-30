// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the published-device/zero-kref window without timing assumptions. */
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>

#include "internals.h"

struct nvmem_test_context {
	struct kunit *test;
	struct device *parent;
	struct nvmem_device *provider;
	struct notifier_block nb;
	unsigned int removals;
};

static int nvmem_test_read(void *priv, unsigned int offset, void *val,
			   size_t bytes)
{
	memset(val, 0x5a, bytes);
	return 0;
}

static int nvmem_test_notify(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	struct nvmem_test_context *ctx = container_of(nb, typeof(*ctx), nb);
	struct nvmem_device *found, *nvmem = data;
	struct kunit *test = ctx->test;
	struct nvmem_cell *cell;
	unsigned int refs;

	if (event != NVMEM_REMOVE || nvmem != ctx->provider)
		return NOTIFY_DONE;

	ctx->removals++;
	/* Removal cannot unpublish the device until this callback returns. */
	KUNIT_EXPECT_EQ(test, kref_read(&nvmem->refcnt), 0);
	refs = kref_read(&nvmem->dev.kobj.kref);
	found = nvmem_device_get(ctx->parent, "nvmem-lifetime-test");
	KUNIT_EXPECT_PTR_EQ(test, found, ERR_PTR(-EPROBE_DEFER));
	/* On an unfixed kernel, do not put the resurrected reference. */
	if (!IS_ERR(found))
		return NOTIFY_DONE;
	found = nvmem_device_find("nvmem-lifetime-test", device_match_name);
	KUNIT_EXPECT_PTR_EQ(test, found, ERR_PTR(-EPROBE_DEFER));
	cell = nvmem_cell_get(ctx->parent, "byte");
	KUNIT_EXPECT_PTR_EQ(test, cell, ERR_PTR(-EPROBE_DEFER));
	KUNIT_EXPECT_EQ(test, kref_read(&nvmem->refcnt), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&nvmem->dev.kobj.kref), refs);
	return NOTIFY_OK;
}

static void nvmem_removal_lookup_test(struct kunit *test)
{
	struct nvmem_test_context ctx = { .test = test };
	struct nvmem_cell_info info = { .name = "byte", .bytes = 1 };
	struct nvmem_cell_lookup lookup = {
		.nvmem_name = "nvmem-lifetime-test",
		.cell_name = "byte",
		.dev_id = "nvmem-lifetime-consumer",
		.con_id = "byte",
	};
	struct nvmem_config config = {
		.name = "nvmem-lifetime-test",
		.id = NVMEM_DEVID_NONE,
		.size = 1,
		.reg_read = nvmem_test_read,
		.owner = THIS_MODULE,
		.cells = &info,
		.ncells = 1,
	};
	struct nvmem_device *held;
	struct nvmem_cell *cell;
	unsigned int parent_refs;
	int ret, cycle;
	u8 value;

	ctx.parent = root_device_register("nvmem-lifetime-consumer");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx.parent);
	config.dev = ctx.parent;
	ctx.nb.notifier_call = nvmem_test_notify;
	ret = nvmem_register_notifier(&ctx.nb);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_parent;
	nvmem_add_cell_lookups(&lookup, 1);
	parent_refs = kref_read(&ctx.parent->kobj.kref);

	/* Repeat with the same name, proving removal permits registration again. */
	for (cycle = 0; cycle < 2; cycle++) {
		ctx.provider = nvmem_register(&config);
		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, ctx.provider);
		if (IS_ERR(ctx.provider))
			break;
		get_device(&ctx.provider->dev);
		held = nvmem_device_get(ctx.parent, config.name);
		cell = nvmem_cell_get(ctx.parent, "byte");
		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, held);
		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, cell);
		KUNIT_EXPECT_EQ(test, kref_read(&ctx.provider->refcnt), 3);
		if (!cycle) {
			/* Final provider put initiates removal directly. */
			if (!IS_ERR(held))
				nvmem_device_put(held);
			if (!IS_ERR(cell))
				nvmem_cell_put(cell);
			nvmem_unregister(ctx.provider);
		} else {
			/* Existing consumers delay removal until their last put. */
			nvmem_unregister(ctx.provider);
			KUNIT_EXPECT_EQ(test, ctx.removals, (unsigned int)cycle);
			if (!IS_ERR(held)) {
				ret = nvmem_device_read(held, 0, 1, &value);
				KUNIT_EXPECT_EQ(test, ret, 1);
				KUNIT_EXPECT_EQ(test, value, (u8)0x5a);
				nvmem_device_put(held);
			}
			KUNIT_EXPECT_EQ(test, ctx.removals, (unsigned int)cycle);
			if (!IS_ERR(cell))
				nvmem_cell_put(cell);
		}
		KUNIT_EXPECT_EQ(test, ctx.removals, (unsigned int)cycle + 1);
		KUNIT_EXPECT_EQ(test, kref_read(&ctx.provider->refcnt), 0);
		/* Only our sentinel device reference should remain after removal. */
		KUNIT_EXPECT_EQ(test, kref_read(&ctx.provider->dev.kobj.kref), 1);
		put_device(&ctx.provider->dev);
		KUNIT_EXPECT_EQ(test, kref_read(&ctx.parent->kobj.kref), parent_refs);
		held = nvmem_device_get(ctx.parent, config.name);
		KUNIT_EXPECT_PTR_EQ(test, held, ERR_PTR(-EPROBE_DEFER));
	}
	nvmem_del_cell_lookups(&lookup, 1);
	nvmem_unregister_notifier(&ctx.nb);
out_parent:
	root_device_unregister(ctx.parent);
}

static struct kunit_case nvmem_core_test_cases[] = {
	KUNIT_CASE(nvmem_removal_lookup_test),
	{}
};

static struct kunit_suite nvmem_core_test_suite = {
	.name = "nvmem-lifetime",
	.test_cases = nvmem_core_test_cases,
};
kunit_test_suite(nvmem_core_test_suite);

MODULE_LICENSE("GPL");
