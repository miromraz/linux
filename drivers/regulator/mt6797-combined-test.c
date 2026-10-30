// SPDX-License-Identifier: GPL-2.0-only
/* Synthetic registers, real OF/CPU/NVMEM/CCF/OPP/regulator frameworks.
 * This boot fixture retains suppliers with the permanent production guards.
 * Run only in the dedicated QEMU image, which halts after this suite.
 */
#include <kunit/test.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/regulator/consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/mt6797.h>
#include "da9211-regulator.h"
#include "mt6797-provider.h"
#include "mt6797-combined-test.h"

struct combined_clock {
	struct clk_hw hw;
	struct notifier_block nb;
	unsigned long rate;
	unsigned int pre, post, abort;
	bool reject, pause;
	struct completion entered, resume;
};

static unsigned long combined_recalc(struct clk_hw *hw, unsigned long parent)
{
	return container_of(hw, struct combined_clock, hw)->rate;
}

static int combined_determine(struct clk_hw *hw, struct clk_rate_request *req)
{
	return 0;
}

static int combined_set(struct clk_hw *hw, unsigned long rate,
			unsigned long parent)
{
	container_of(hw, struct combined_clock, hw)->rate = rate;
	return 0;
}

static int combined_clock_event(struct notifier_block *nb, unsigned long event,
				void *data)
{
	struct combined_clock *c = container_of(nb, struct combined_clock, nb);

	if (event == PRE_RATE_CHANGE) {
		c->pre++;
		if (c->pause) {
			complete(&c->entered);
			wait_for_completion(&c->resume);
		}
		if (c->reject)
			return NOTIFY_BAD;
	} else if (event == POST_RATE_CHANGE) {
		c->post++;
	} else if (event == ABORT_RATE_CHANGE) {
		c->abort++;
	}
	return NOTIFY_OK;
}

static const struct clk_ops combined_clock_ops = {
	.recalc_rate = combined_recalc,
	.determine_rate = combined_determine,
	.set_rate = combined_set,
};

struct combined {
	struct completion observer_entered, observer_resume;
	bool observer_pause;
	struct notifier_block vote_nb;
	bool detach_at_transfer;
	struct combined_clock clocks[6];
	struct mutex observer_lock;
	struct kunit *test;
	unsigned int buck[512], sram[3], writes;
	u8 bin;
	struct platform_device *parent, *rail[2], *cci, *cpu;
	struct regmap *map[2];
	void *group[2];
};

static void check_rails(struct combined *f)
{
	int proc = 300000 + (f->buck[DA9211_REG_VBUCKA_A] & 127) * 10000;
	int i, uv;

	if (!f->test)
		return;
	KUNIT_EXPECT_EQ(f->test, f->buck[DA9211_REG_BUCKA_CONT],
			(unsigned int)DA9211_BUCKA_EN);
	KUNIT_EXPECT_EQ(f->test, f->sram[0], 255U);
	KUNIT_EXPECT_GE(f->test, proc, 1100000);
	for (i = 0; i < 8; i++) {
		uv = 825000 + ((f->sram[1 + i / 4] >> (8 * (i % 4))) & 15) * 25000;
		KUNIT_EXPECT_GE(f->test, uv, proc);
		KUNIT_EXPECT_LE(f->test, uv - proc, 200000);
	}
}

static int buck_read(void *data, unsigned int reg, unsigned int *val)
{
	struct combined *f = data;

	*val = f->buck[reg];
	return 0;
}

static int buck_write(void *data, unsigned int reg, unsigned int val)
{
	struct combined *f = data;

	mutex_lock(&f->observer_lock);
	if (f->observer_pause) {
		complete(&f->observer_entered);
		wait_for_completion(&f->observer_resume);
	}
	f->buck[reg] = val;
	f->writes++;
	check_rails(f);
	mutex_unlock(&f->observer_lock);
	return 0;
}

static int sram_read(void *data, unsigned int reg, unsigned int *val)
{
	struct combined *f = data;

	*val = f->sram[(reg - 0xf98) / 4];
	return 0;
}

static int sram_write(void *data, unsigned int reg, unsigned int val)
{
	struct combined *f = data;

	mutex_lock(&f->observer_lock);
	if (f->test) {
		KUNIT_EXPECT_NE(f->test, reg, 0xf98U);
		KUNIT_EXPECT_EQ(f->test, val & 0xf0f0f0f0, 0xa0a0a0a0U);
	}
	f->sram[(reg - 0xf98) / 4] = val;
	f->writes++;
	check_rails(f);
	mutex_unlock(&f->observer_lock);
	return 0;
}

static int bin_read(void *data, unsigned int off, void *val, size_t bytes)
{
	struct combined *f = data;

	memset(val, f->bin, bytes);
	return 0;
}

static struct platform_device *node_device(const char *path, const char *name)
{
	struct device_node *np = of_find_node_by_path(path);
	struct platform_device *pdev;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		pdev = of_platform_device_create(np, name, NULL);
	of_node_put(np);
	return pdev;
}

struct acquire_task {
	struct completion *start;
	struct completion done;
	u64 cookie;
	int ret;
};

static int acquire_thread(void *arg)
{
	struct acquire_task *t = arg;

	wait_for_completion(t->start);
	t->ret = mt6797_consumer_acquire(get_cpu_device(0), 0, &t->cookie);
	complete_all(&t->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

struct clock_race {
	struct combined_clock *clock;
	struct acquire_task acquire;
	struct completion start, done;
	struct task_struct *rate_thread, *acquire_thread;
	unsigned long rate;
	int ret;
};

static int combined_rate_thread(void *data)
{
	struct clock_race *r = data;

	r->ret = clk_set_rate(r->clock->hw.clk, r->rate);
	complete(&r->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static void clock_race_cleanup(void *data)
{
	struct clock_race *r = data;

	complete_all(&r->clock->resume);
	complete_all(&r->start);
	if (!IS_ERR_OR_NULL(r->rate_thread))
		kthread_stop(r->rate_thread);
	if (!IS_ERR_OR_NULL(r->acquire_thread))
		kthread_stop(r->acquire_thread);
	r->clock->pause = false;
	if (r->acquire.cookie)
		mt6797_consumer_release(0, r->acquire.cookie);
}

static void combined_clock_matrix(struct kunit *test, struct combined *f)
{
	int i, abort;

	for (i = 0; i < 6; i++) {
		for (abort = 0; abort < 2; abort++) {
			struct combined_clock *c = &f->clocks[i];
			struct clock_race *r;
			struct mt6797_provider_status *before, *after;
			unsigned int pre = c->pre, post = c->post, aborted = c->abort;

			before = kunit_kzalloc(test, sizeof(*before), GFP_KERNEL);
			after = kunit_kzalloc(test, sizeof(*after), GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, before);
			KUNIT_ASSERT_NOT_NULL(test, after);
			r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, r);
			r->clock = c;
			r->rate = 150 + i + abort;
			init_completion(&r->start);
			init_completion(&r->done);
			init_completion(&r->acquire.done);
			r->acquire.start = &r->start;
			reinit_completion(&c->entered);
			reinit_completion(&c->resume);
			KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							clock_race_cleanup, r), 0);
			mt6797_preflight_test_flush();
			mt6797_provider_status(before);
			c->reject = abort;
			c->pause = true;
			r->rate_thread = kthread_run(combined_rate_thread, r, "combined-rate");
			KUNIT_ASSERT_FALSE(test, IS_ERR(r->rate_thread));
			KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&c->entered,
									5 * HZ), 0UL);
			r->acquire_thread = kthread_run(acquire_thread, &r->acquire,
							"combined-clock-peer");
			KUNIT_ASSERT_FALSE(test, IS_ERR(r->acquire_thread));
			complete_all(&r->start);
			/* PRE invalidates the published proof. A peer either rejects
			 * it immediately or waits for CCF; it cannot acquire a lease.
			 */
			if (wait_for_completion_timeout(&r->acquire.done, HZ / 20)) {
				KUNIT_EXPECT_LT(test, r->acquire.ret, 0);
				KUNIT_EXPECT_EQ(test, r->acquire.cookie, 0ULL);
			}
			complete_all(&c->resume);
			KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->done, 5 * HZ), 0UL);
			KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->acquire.done,
									5 * HZ), 0UL);
			KUNIT_EXPECT_EQ(test, c->pre, pre + 1);
			KUNIT_EXPECT_EQ(test, c->post, post + !abort);
			KUNIT_EXPECT_EQ(test, c->abort, aborted + abort);
			if (abort)
				KUNIT_EXPECT_LT(test, r->ret, 0);
			else
				KUNIT_EXPECT_EQ(test, r->ret, 0);
			if (r->acquire.ret)
				KUNIT_EXPECT_EQ(test, r->acquire.cookie, 0ULL);
			else
				KUNIT_EXPECT_NE(test, r->acquire.cookie, 0ULL);
			kunit_release_action(test, clock_race_cleanup, r);
			c->reject = false;
			mt6797_preflight_test_flush();
			KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
			mt6797_provider_status(after);
			KUNIT_EXPECT_GT(test, after->preflight.generation,
					 before->preflight.generation);
			KUNIT_EXPECT_EQ(test, after->active, 0UL);
			KUNIT_EXPECT_EQ(test, after->adopted, 0UL);
			KUNIT_EXPECT_FALSE(test, after->engine.ready);
		}
	}
}

static int invalidate_vote(struct notifier_block *nb, unsigned long event, void *arg)
{
	if (event == REGULATOR_EVENT_PRE_VOLTAGE_CHANGE)
		dev_pm_opp_adjust_voltage(get_cpu_device(0), 200,
					 1070000, 1070000, 1070000);
	mt6797_preflight_test_event();
	return NOTIFY_OK;
}

struct transaction_race {
	struct completion entered, resume, start;
	struct acquire_task acquire;
	struct task_struct *thread;
	int phase;
};

static void transaction_pause(void *data, unsigned int user, bool returning,
			      int phase)
{
	struct transaction_race *r = data;

	if (user || returning || phase != r->phase)
		return;
	complete(&r->entered);
	wait_for_completion(&r->resume);
}

static void transaction_race_cleanup(void *data)
{
	struct transaction_race *r = data;

	complete_all(&r->resume);
	complete_all(&r->start);
	if (!IS_ERR_OR_NULL(r->thread))
		kthread_stop(r->thread);
	mt6797_test_transfer_hook(NULL, NULL);
	if (r->acquire.cookie)
		mt6797_consumer_release(0, r->acquire.cookie);
}

static void combined_transaction_matrix(struct kunit *test, struct combined *f)
{
	int i, phase;

	for (i = 0; i < 6; i++) {
		for (phase = 0; phase < 2; phase++) {
			struct transaction_race *r;
			unsigned int writes;

			r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, r);
			init_completion(&r->entered);
			init_completion(&r->resume);
			init_completion(&r->start);
			init_completion(&r->acquire.done);
			r->acquire.start = &r->start;
			r->phase = phase;
			KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
						transaction_race_cleanup, r), 0);
			mt6797_preflight_test_flush();
			mt6797_test_transfer_hook(transaction_pause, r);
			r->thread = kthread_run(acquire_thread, &r->acquire, "combined-proof");
			KUNIT_ASSERT_FALSE(test, IS_ERR(r->thread));
			complete_all(&r->start);
			KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->entered,
									5 * HZ), 0UL);
			writes = f->writes;
			/* Snapshot/reservation rejects rate mutation; the unlocked
			 * transfer boundary allows notification and invalidates proof.
			 */
			KUNIT_EXPECT_EQ(test, clk_set_rate(f->clocks[i].hw.clk,
							170 + i + phase), phase ? 0 : -EBUSY);
			complete_all(&r->resume);
			KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->acquire.done,
									5 * HZ), 0UL);
			KUNIT_EXPECT_EQ(test, r->acquire.ret, phase ? -ESTALE : 0);
			if (phase) {
				KUNIT_EXPECT_EQ(test, r->acquire.cookie, 0ULL);
				KUNIT_EXPECT_EQ(test, f->writes, writes);
			}
			kunit_release_action(test, transaction_race_cleanup, r);
			mt6797_preflight_test_flush();
			KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
		}
	}
}

static void transfer_fault(void *data, unsigned int user, bool returning, int phase)
{
	struct combined *f = data;

	if (!returning || !phase)
		return;
	if (f->detach_at_transfer && !user) {
		devres_release_group(&f->rail[0]->dev, f->group[0]);
		f->group[0] = NULL;
	} else if (!f->detach_at_transfer && user == 1) {
		/* Real adapter read rejects the invalid enable bank. */
		f->sram[0] = 0;
	}
}

static void transfer_fault_cleanup(void *data)
{
	struct combined *f = data;

	mt6797_test_transfer_hook(NULL, NULL);
	f->sram[0] = 255;
}

static void combined_recovery_matrix(struct kunit *test, struct combined *f,
				     struct device_node *buck)
{
	unsigned int refs[2];
	u64 cookie[2];
	int mode, i, guard, vote;

	for (mode = 0; mode < 2; mode++) {
		for (i = 0; i < 2; i++) {
			refs[i] = kref_read(&get_cpu_device(4 * i)->kobj.kref);
			KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(
				get_cpu_device(4 * i), i, &cookie[i]), 0);
		}
		f->sram[0] = 0;
		for (i = 0; i < 2; i++) {
			KUNIT_EXPECT_LT(test, mt6797_consumer_release(i, cookie[i]), 0);
			KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i,
								&guard, &vote), 2);
			KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(4 * i)->kobj.kref),
					refs[i] + 1);
		}
		f->sram[0] = 255;
		f->detach_at_transfer = mode;
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
						transfer_fault_cleanup, f), 0);
		mt6797_test_transfer_hook(transfer_fault, f);
		KUNIT_EXPECT_LT(test, mt6797_consumer_recover(), 0);
		kunit_release_action(test, transfer_fault_cleanup, f);
		/* Later-peer failure must preserve that peer after the first retires.
		 * Detach before first transfer must preserve both peers.
		 */
		for (i = 0; i < 2; i++) {
			bool retained = mode || i;

			KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(4 * i)->kobj.kref),
					refs[i] + retained);
			KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i,
								&guard, &vote), retained ? 2 : 1);
			KUNIT_EXPECT_GE(test, max(guard, vote), 1060000 + 20000 * i);
			KUNIT_EXPECT_EQ(test, mt6797_consumer_release(i, cookie[i]), -ESTALE);
		}
		if (mode) {
			KUNIT_EXPECT_PTR_EQ(test, dev_get_drvdata(&f->parent->dev), NULL);
			f->group[0] = devres_open_group(&f->rail[0]->dev, NULL, GFP_KERNEL);
			KUNIT_ASSERT_NOT_NULL(test, f->group[0]);
			KUNIT_ASSERT_EQ(test, mt6797_test_buck(f->rail[0], f->map[0], buck), 0);
		}
		mt6797_preflight_test_event();
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
		for (i = 0; i < 2; i++)
			KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(4 * i)->kobj.kref),
					refs[i]);
	}
}

/* Suppliers are deliberately retained by this isolated VM. Only the observer
 * belongs to the case: serialize disarm with every assertion-bearing callback.
 */
static void observer_shutdown(void *data)
{
	struct combined *f = data;

	mutex_lock(&f->observer_lock);
	f->test = NULL;
	mutex_unlock(&f->observer_lock);
}

struct observer_race {
	struct combined *f;
	struct completion started, done;
	struct task_struct *writer, *shutdown;
};

static int observer_writer(void *data)
{
	struct observer_race *r = data;
	struct combined *f = r->f;

	regmap_write(f->map[0], DA9211_REG_VBUCKA_A, f->buck[DA9211_REG_VBUCKA_A]);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static int observer_closer(void *data)
{
	struct observer_race *r = data;

	complete(&r->started);
	observer_shutdown(r->f);
	complete(&r->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static void observer_race_cleanup(void *data)
{
	struct observer_race *r = data;

	complete_all(&r->f->observer_resume);
	if (!IS_ERR_OR_NULL(r->writer))
		kthread_stop(r->writer);
	if (!IS_ERR_OR_NULL(r->shutdown))
		kthread_stop(r->shutdown);
	r->f->observer_pause = false;
	observer_shutdown(r->f);
}

static void combined_observer_boundary(struct kunit *test, struct combined *f)
{
	struct observer_race *r;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	r->f = f;
	init_completion(&r->started);
	init_completion(&r->done);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, observer_race_cleanup, r), 0);
	f->observer_pause = true;
	r->writer = kthread_run(observer_writer, r, "combined-write");
	KUNIT_ASSERT_FALSE(test, IS_ERR(r->writer));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&f->observer_entered, 5 * HZ), 0UL);
	r->shutdown = kthread_run(observer_closer, r, "combined-disarm");
	KUNIT_ASSERT_FALSE(test, IS_ERR(r->shutdown));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->started, 5 * HZ), 0UL);
	KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&r->done, HZ / 20), 0UL);
	complete_all(&f->observer_resume);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&r->done, 5 * HZ), 0UL);
	kunit_release_action(test, observer_race_cleanup, r);
	KUNIT_EXPECT_PTR_EQ(test, f->test, NULL);
}

static void combined_discovery(struct kunit *test)
{
	static const struct regmap_config maps[2] = {
		{ .reg_bits = 16, .val_bits = 8, .max_register = 511,
		  .reg_read = buck_read, .reg_write = buck_write },
		{ .reg_bits = 32, .val_bits = 32, .reg_stride = 4,
		  .max_register = 0xfa0,
		  .reg_read = sram_read, .reg_write = sram_write },
	};
	struct nvmem_config nc = { .name = "combined-speedbin", .size = 1,
		.word_size = 1, .stride = 1, .reg_read = bin_read,
		.add_legacy_fixed_of_cells = true };
	struct mt6797_provider_status *status;
	struct device_node *np, *rail_np[2];
	struct platform_device *efuse;
	struct nvmem_device *nvmem;
	struct clk_hw *hw, *clocks[6];
	struct combined *f;
	cpumask_t mask;
	u64 cookie[2], generation;
	u32 order = 0, scenario = 0;
	struct completion start;
	struct acquire_task tasks[2];
	struct task_struct *threads[2];
	int i, id, guard, vote, refs, probe_refs[3];

	KUNIT_ASSERT_TRUE(test, of_machine_is_compatible("test,mt6797-combined"));
	KUNIT_ASSERT_EQ(test, num_present_cpus(), 8U);
	status = kunit_kzalloc(test, sizeof(*status), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, status);
	f = kzalloc_obj(*f);
	KUNIT_ASSERT_NOT_NULL(test, f);
	init_completion(&f->observer_entered);
	init_completion(&f->observer_resume);
	mutex_init(&f->observer_lock);
	f->test = test;
	f->vote_nb.notifier_call = invalidate_vote;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, observer_shutdown, f), 0);
	of_property_read_u32(of_root, "test-scenario", &scenario);
	if (scenario == 2)
		f->bin = 32;
	if (scenario == 4)
		f->bin = 1;
	f->buck[DA9211_REG_CONFIG_E] = DA9211_SLAVE_SEL;
	f->buck[DA9211_REG_BUCKA_CONT] = DA9211_BUCKA_EN;
	f->buck[DA9211_REG_VBUCKA_A] = 80; /* 1.10 V */
	f->sram[0] = 255;
	f->sram[1] = f->sram[2] = 0xabababab; /* 1.10 V */
	f->parent = platform_device_register_simple("combined-parent", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->parent));
	f->cci = node_device("/test-cci", "combined-cci");
	KUNIT_ASSERT_NOT_NULL(test, f->cci);
	f->cpu = platform_device_register_simple("combined-cpu", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->cpu));
	for (i = 0; i < 2; i++) {
		cpumask_clear(&mask);
		KUNIT_ASSERT_EQ(test, dev_pm_opp_of_get_sharing_cpus(get_cpu_device(4 * i),
								&mask), 0);
		KUNIT_EXPECT_EQ(test, cpumask_weight(&mask), 4);
		KUNIT_EXPECT_EQ(test, cpumask_first(&mask), 4U * i);
	}
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_test_cpu_probe(f->cpu, -1), -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, mt6797_test_cci_probe(f->cci), -EPROBE_DEFER);

	/* Suppliers arrive after the real preflight worker has deferred. */
	efuse = node_device("/test-efuse", "combined-efuse");
	KUNIT_ASSERT_NOT_NULL(test, efuse);
	nc.dev = &efuse->dev;
	nc.priv = f;
	/* Scenario 5 isolates NVMEM absence with all other suppliers ready. */
	if (scenario != 5) {
		nvmem = nvmem_register(&nc);
		KUNIT_ASSERT_FALSE(test, IS_ERR(nvmem));
	}
	for (i = 0; i < 6; i++) {
		char path[40], name[40];

		snprintf(path, sizeof(path), "/test-clock-%d", i);
		snprintf(name, sizeof(name), "combined-clock-%d", i);
		np = of_find_node_by_path(path);
		KUNIT_ASSERT_NOT_NULL(test, np);
		if (scenario == 3 && i == 5) {
			of_node_put(np);
			continue;
		}
		{
			struct combined_clock *c = &f->clocks[i];
			struct clk_init_data init = {
				.name = name, .ops = &combined_clock_ops,
			};

			c->rate = scenario == 1 && i == 5 ? 0 : 100 + i;
			init_completion(&c->entered);
			init_completion(&c->resume);
			c->hw.init = &init;
			KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &c->hw), 0);
			c->nb.notifier_call = combined_clock_event;
			/* Production notifier runs first, then this barrier observes PRE. */
			c->nb.priority = -1;
			KUNIT_ASSERT_EQ(test, clk_notifier_register(c->hw.clk, &c->nb), 0);
			hw = &c->hw;
		}
		clocks[i] = hw;
		KUNIT_ASSERT_EQ(test, of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw), 0);
		of_node_put(np);
	}
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	rail_np[0] = of_find_node_by_path("/test-buck/regulators/BUCKA");
	rail_np[1] = of_find_node_by_path("/test-infra/sram");
	of_property_read_u32(of_root, "test-order", &order);
	for (i = 0; i < 2; i++) {
		id = i ^ order;
		f->rail[id] = platform_device_alloc("combined-rail", id);
		KUNIT_ASSERT_NOT_NULL(test, f->rail[id]);
		if (id) {
			struct platform_device *infra;

			infra = node_device("/test-infra", "combined-infra");
			KUNIT_ASSERT_NOT_NULL(test, infra);
			f->rail[id]->dev.parent = &infra->dev;
			f->rail[id]->dev.of_node = of_node_get(rail_np[id]);
		} else {
			f->rail[id]->dev.parent = &f->parent->dev;
		}
		KUNIT_ASSERT_EQ(test, platform_device_add(f->rail[id]), 0);
		f->map[id] = regmap_init(&f->rail[id]->dev, NULL, f, &maps[id]);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->map[id]));
		if (id)
			KUNIT_ASSERT_EQ(test, of_syscon_register_regmap(
				f->rail[id]->dev.parent->of_node, f->map[id]), 0);
		f->group[id] = devres_open_group(&f->rail[id]->dev, NULL, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, f->group[id]);
		KUNIT_ASSERT_EQ(test, id ? mt6797_test_sram(f->rail[id]) :
			mt6797_test_buck(f->rail[id], f->map[id], rail_np[id]), 0);
		mt6797_preflight_test_flush();
		if (!i) {
			KUNIT_EXPECT_LT(test, mt6797_test_cpu_probe(f->cpu, -1), 0);
			KUNIT_EXPECT_EQ(test, f->writes, 0U);
		}
	}
	mt6797_provider_status(status);
	if (scenario) {
		KUNIT_EXPECT_EQ(test, status->preflight.error,
				 scenario == 3 || scenario == 5 ? -EPROBE_DEFER :
				 scenario == 4 ? -ENOENT : -EINVAL);
		KUNIT_EXPECT_EQ(test, f->writes, 0U);
		KUNIT_EXPECT_FALSE(test, status->converted);
		KUNIT_EXPECT_FALSE(test, status->engine.ready);
		KUNIT_EXPECT_EQ(test, status->active, 0UL);
		KUNIT_EXPECT_EQ(test, status->adopted, 0UL);
		if (scenario == 5) {
			KUNIT_EXPECT_EQ(test, mt6797_test_cpu_probe(f->cpu, -1),
					-EPROBE_DEFER);
			KUNIT_EXPECT_EQ(test, mt6797_test_cci_probe(f->cci),
					-EPROBE_DEFER);
		}
		return;
	}
	KUNIT_ASSERT_EQ(test, status->preflight.error, 0);
	KUNIT_EXPECT_TRUE(test, status->converted);
	KUNIT_EXPECT_FALSE(test, status->engine.ready);
	KUNIT_EXPECT_GT(test, f->writes, 0U);
	KUNIT_EXPECT_EQ(test, status->preflight.rail_uv[1], 1200000);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ(test, status->preflight.speedbin[i], 0);
		KUNIT_EXPECT_EQ(test, status->preflight.uv[i][1], 1060000UL + 20000 * i);
		KUNIT_EXPECT_EQ(test, status->preflight.rate[i][0], 100UL + 2 * i);
		KUNIT_EXPECT_EQ(test, status->preflight.rate[i][1], 101UL + 2 * i);
	}
	/* Full production CPU probe enumerates/deduplicates both groups, then
	 * the closed admission gate unwinds the complete real list.
	 */
	for (i = 0; i < 3; i++)
		probe_refs[i] = kref_read(&(i == 2 ? &f->cci->dev :
					 get_cpu_device(4 * i))->kobj.kref);
	KUNIT_EXPECT_EQ(test, mt6797_test_cpu_probe(f->cpu, -1), -EAGAIN);
	KUNIT_EXPECT_EQ(test, mt6797_test_cpu_probe(f->cpu, 4), -ENOMEM);
	KUNIT_EXPECT_EQ(test, mt6797_test_cci_probe(f->cci), -EAGAIN);
	KUNIT_EXPECT_EQ(test, mt6797_test_cci_remove(f->cci), 0);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ(test, kref_read(&(i == 2 ? &f->cci->dev :
					 get_cpu_device(4 * i))->kobj.kref), probe_refs[i]);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i, &guard, &vote), 1);
		KUNIT_EXPECT_EQ(test, vote, 0);
		KUNIT_EXPECT_GE(test, guard, 1060000 + 20000 * i);
	}
	mt6797_provider_status(status);
	KUNIT_EXPECT_EQ(test, status->active, 0UL);
	KUNIT_EXPECT_EQ(test, status->adopted, 0UL);
	refs = kref_read(&get_cpu_device(0)->kobj.kref);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(get_cpu_device(0), 0, &cookie[0]), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(get_cpu_device(0), 0, &cookie[1]), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs + 1);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 2);
	KUNIT_EXPECT_GE(test, vote, 1060000);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, cookie[0]), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs + 1);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, cookie[1]), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, cookie[1]), -ESTALE);

	/* Two real CPUs race for the same core-backed ownership. */
	init_completion(&start);
	for (i = 0; i < 2; i++) {
		tasks[i].start = &start;
		init_completion(&tasks[i].done);
		threads[i] = kthread_run(acquire_thread, &tasks[i], "combined-acquire");
		if (IS_ERR(threads[i])) {
			complete_all(&start);
			if (i)
				kthread_stop(threads[0]);
			KUNIT_FAIL(test, "thread allocation failed");
			return;
		}
	}
	complete_all(&start);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&tasks[i].done, 5 * HZ), 0UL);
		kthread_stop(threads[i]);
		KUNIT_EXPECT_EQ(test, tasks[i].ret, 0);
	}
	KUNIT_EXPECT_NE(test, tasks[0].cookie, tasks[1].cookie);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs + 1);
	for (i = 0; i < 2; i++)
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, tasks[i].cookie), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs);
	/* Reentrant OPP invalidation on the actual regulator notifier stack. */
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_regulator_notifier(0, &f->vote_nb, true), 0);
	i = mt6797_consumer_acquire(get_cpu_device(0), 0, &cookie[0]);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_regulator_notifier(0, &f->vote_nb, false), 0);
	KUNIT_EXPECT_EQ(test, i, -ESTALE);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);

	/* Real NVMEM lookup is repeated: invalid and changed bins fail closed. */
	generation = status->preflight.generation;
	f->bin = 32;
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	mt6797_provider_status(status);
	KUNIT_EXPECT_EQ(test, status->preflight.error, -EINVAL);
	KUNIT_EXPECT_GT(test, status->preflight.generation, generation);
	f->bin = 1;
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	mt6797_provider_status(status);
	KUNIT_EXPECT_EQ(test, status->preflight.error, -ESTALE);
	f->bin = 0;
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
	/* Remove/re-register the real NVMEM supplier, preserving OPP policy. */
	nvmem_unregister(nvmem);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), -EPROBE_DEFER);
	nvmem = nvmem_register(&nc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(nvmem));
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
	/* CCF lookup must defer while the OF supplier is absent. */
	np = of_find_node_by_path("/test-clock-3");
	of_clk_del_provider(np);
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), -EPROBE_DEFER);
	KUNIT_ASSERT_EQ(test, of_clk_add_hw_provider(np, of_clk_hw_simple_get, clocks[3]), 0);
	of_node_put(np);
	mt6797_preflight_test_event();
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);

	/* Drain actual raw endpoint devres while a real vote is owned. */
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(get_cpu_device(0), 0, &cookie[0]), 0);
	devres_release_group(&f->rail[0]->dev, f->group[0]);
	KUNIT_EXPECT_LT(test, mt6797_consumer_release(0, cookie[0]), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 2);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs + 1);
	KUNIT_EXPECT_LT(test, mt6797_consumer_recover(), 0);
	f->group[0] = devres_open_group(&f->rail[0]->dev, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->group[0]);
	KUNIT_ASSERT_EQ(test, mt6797_test_buck(f->rail[0], f->map[0], rail_np[0]), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), 0);
	KUNIT_EXPECT_EQ(test, kref_read(&get_cpu_device(0)->kobj.kref), refs);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(0, &guard, &vote), 1);
	mutex_lock(&f->observer_lock);
	check_rails(f);
	mutex_unlock(&f->observer_lock);
	combined_recovery_matrix(test, f, rail_np[0]);
	combined_clock_matrix(test, f);
	combined_transaction_matrix(test, f);
	combined_observer_boundary(test, f);
	kunit_release_action(test, observer_shutdown, f);
	/* Exercise both retained callbacks after the assertion context is disarmed. */
	regmap_write(f->map[0], DA9211_REG_VBUCKA_A, f->buck[DA9211_REG_VBUCKA_A]);
	regmap_write(f->map[1], 0xf9c, f->sram[1]);
	KUNIT_EXPECT_PTR_EQ(test, f->test, NULL);
}

static struct kunit_case combined_cases[] = {
	KUNIT_CASE(combined_discovery),
	{}
};

static struct kunit_suite combined_suite = {
	.name = "mt6797-combined",
	.test_cases = combined_cases,
};
kunit_test_suite(combined_suite);
