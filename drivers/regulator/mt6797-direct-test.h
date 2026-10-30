/* SPDX-License-Identifier: GPL-2.0-only */
/* Discovery injection is explicit: these cases do not validate OF/NVMEM. */
#include <linux/cpumask.h>

struct direct_cpu {
	struct device *cpu_dev;
	struct mutex callback_lock, reg_lock;
	struct clk *cpu_clk, *inter_clk;
	unsigned long current_freq;
	int intermediate_voltage, opp_cpu;
	bool dvfs_fault;
	unsigned int rail_user;
	u64 rail_cookie;
	cpumask_t cpus;
};

struct mtk_ccifreq_drv {
	struct device *dev;
	u64 rail_cookie;
};

static struct device *direct_devices[3];
static int direct_sharing_error;

static struct device *direct_get_cpu_device(unsigned int cpu)
{
	return direct_devices[cpu / 4];
}

static int direct_get_sharing(struct device *dev, struct cpumask *mask)
{
	/* UML is UP. CPU0's membership check still executes in the real body. */
	cpumask_clear(mask);
	cpumask_set_cpu(0, mask);
	return direct_sharing_error;
}

static struct clk *direct_clock;

static struct clk *direct_clk_get(struct device *dev, const char *id)
{
	return clk_hw_get_clk(__clk_get_hw(direct_clock), NULL);
}

#define clk_get direct_clk_get
#define get_cpu_device direct_get_cpu_device
#define dev_pm_opp_of_get_sharing_cpus direct_get_sharing
#include "mt6797-direct-callbacks.h"
#undef dev_pm_opp_of_get_sharing_cpus
#undef get_cpu_device
#undef clk_get

static void direct_callbacks(struct kunit *test, struct conversion_fixture *f,
			     struct device **dev, int mode)
{
	struct direct_cpu cpu = {}, sibling = {};
	struct mtk_ccifreq_drv *cci;
	struct mt6797_provider_status status;
	unsigned int refs[3];
	u64 old_cookie;
	int i, guard, vote;
	void *group;
	bool retained = mode == 6 || mode == 9;

	for (i = 0; i < 3; i++) {
		direct_devices[i] = dev[i];
		refs[i] = kref_read(&dev[i]->kobj.kref);
	}
	direct_clock = f->clk;
	direct_sharing_error = -EPROBE_DEFER;
	KUNIT_EXPECT_EQ(test, direct_cpu_init(&cpu, 0), -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, cpu.rail_cookie, 0ULL);
	direct_sharing_error = 0;
	if (mode == 7) {
		f->fail_write = f->writes + 1;
		KUNIT_EXPECT_EQ(test, direct_cpu_init(&cpu, 0), -EIO);
		KUNIT_EXPECT_EQ(test, cpu.rail_cookie, 0ULL);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 1);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[0]->kobj.kref), refs[0]);
		return;
	}
	if (mode == 8) {
		struct ownership_notifier n = {
			.nb.notifier_call = ownership_invalidate, .dev = dev[0],
		};
		int ret, writes = f->writes;

		KUNIT_ASSERT_EQ(test, direct_cpu_init(&cpu, 0), 0);
		direct_cpu_release(&cpu);
		writes = f->writes;
		KUNIT_ASSERT_EQ(test, mt6797_preflight_test_regulator_notifier(0, &n.nb, true), 0);
		ret = direct_cpu_init(&cpu, 0);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_regulator_notifier(0, &n.nb, false), 0);
		KUNIT_EXPECT_EQ(test, ret, -ESTALE);
		KUNIT_EXPECT_EQ(test, f->writes, writes);
		KUNIT_EXPECT_EQ(test, cpu.rail_cookie, 0ULL);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 1);
		return;
	}
	KUNIT_ASSERT_EQ(test, direct_cpu_init(&cpu, 0), 0);
	KUNIT_ASSERT_EQ(test, direct_cpu_init(&sibling, 0), 0);
	KUNIT_EXPECT_NE(test, cpu.rail_cookie, sibling.rail_cookie);
	old_cookie = cpu.rail_cookie;
	direct_cpu_release(&cpu);
	KUNIT_EXPECT_EQ(test, cpu.rail_cookie, 0ULL);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 2);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[0]->kobj.kref), refs[0] + 1);
	direct_cpu_release(&sibling);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 1);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[0]->kobj.kref), refs[0]);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, old_cookie), -ESTALE);
	/* CCI absent: its guard must already exist before any local allocation. */
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 1);
	group = devres_open_group(dev[2], NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, group);
	if (mode == 5)
		f->fail_write = f->writes + 1;
	KUNIT_EXPECT_EQ(test, direct_cci_probe(f->dev[2]), mode == 5 ? -EIO : -EAGAIN);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 1);
	KUNIT_EXPECT_GE(test, guard, f->live_floor[2]);
	devres_release_group(dev[2], group);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2]);
	if (mode == 5)
		return;

	/* Remove hook is deliberately invoked directly: probe remains gated. */
	group = devres_open_group(dev[2], NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, group);
	cci = devm_kzalloc(dev[2], sizeof(*cci), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cci);
	cci->dev = dev[2];
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[2], 2, &cci->rail_cookie), 0);
	old_cookie = cci->rail_cookie;
	platform_set_drvdata(f->dev[2], cci);
	if (mode == 6) {
		f->fail_write = f->writes + 1;
		f->commit_error = true;
	}
	if (mode == 9) {
		platform_device_unregister(f->rail[0]);
		f->rail[0] = NULL;
	}
	direct_cci_remove(f->dev[2]);
	KUNIT_EXPECT_EQ(test, cci->rail_cookie, 0ULL);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote),
			retained ? 2 : 1);
	KUNIT_EXPECT_GE(test, max(guard, vote), f->live_floor[2]);
	/* Assert registry ownership before AND after driver-local devres free. */
	KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2] + retained);
	platform_set_drvdata(f->dev[2], NULL);
	devres_release_group(dev[2], group);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2] + retained);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, old_cookie), -ESTALE);
	if (mode == 9) {
		struct conversion_endpoint *e = f->replacement;
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = e, .step_uv = 10000,
		};

		KUNIT_ASSERT_NOT_NULL(test, e);
		e->fixture = f;
		KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), -EPROBE_DEFER);
		f->rail[0] = platform_device_register_simple("conversion-rail", 0, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[0]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[0]->dev, 0, &io), 0);
		mt6797_preflight_test_flush();
	}
	if (retained) {
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.retained, BIT(2));
		KUNIT_ASSERT_EQ(test, mt6797_consumer_recover(), 0);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2]);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 1);
		KUNIT_EXPECT_EQ(test, direct_cci_probe(f->dev[2]), -EAGAIN);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, old_cookie), -ESTALE);
	}
}
