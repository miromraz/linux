// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include "mt6797-provider.h"

/* Actual OPP core and production selector, no replacement OPP functions.
 * Dynamic OPPs do not exercise OF supported-hw parsing or CPU-node mapping.
 */
static void preflight_opp_core(struct kunit *test)
{
	struct platform_device *pdev;
	struct dev_pm_opp_config config = {};
	unsigned long freq, uv;
	unsigned int hw;
	int bin, token;

	pdev = platform_device_register_simple("preflight-opp-test", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	for (bin = 0; bin < 16; bin++) {
		KUNIT_EXPECT_EQ(test, mt6797_preflight_hw(bin, &hw), 0);
		KUNIT_EXPECT_EQ(test, hw, (unsigned int)BIT(bin));
	}
	KUNIT_EXPECT_EQ(test, mt6797_preflight_hw(32, &hw), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_hw(16, &hw), -EINVAL);
	config.supported_hw = &hw;
	config.supported_hw_count = 1;
	token = dev_pm_opp_set_config(&pdev->dev, &config);
	KUNIT_ASSERT_GT(test, token, 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 100,
						     &freq, &uv), -ERANGE);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 100, 800000), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 200, 900000), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 0,
						     &freq, &uv), -EINVAL);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 99,
						     &freq, &uv), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 201,
						     &freq, &uv), -ERANGE);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 101,
						     &freq, &uv), 0);
	KUNIT_EXPECT_EQ(test, freq, 200UL);
	KUNIT_EXPECT_EQ(test, uv, 900000UL);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_disable(&pdev->dev, 200), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 101,
						     &freq, &uv), -ERANGE);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_enable(&pdev->dev, 200), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 100,
						     &freq, &uv), 0);
	KUNIT_EXPECT_EQ(test, uv, 800000UL);
	dev_pm_opp_remove_all_dynamic(&pdev->dev);
	dev_pm_opp_clear_config(token);
	/* Reinstallation after clearing must work on the same device. */
	token = dev_pm_opp_set_config(&pdev->dev, &config);
	KUNIT_ASSERT_GT(test, token, 0);
	dev_pm_opp_clear_config(token);
	platform_device_unregister(pdev);
}

struct provider_fixture {
	int uv;
	bool pause;
	struct completion entered;
	struct completion resume;
};

static int fixture_read(void *data)
{
	struct provider_fixture *f = data;

	if (f->pause) {
		complete(&f->entered);
		wait_for_completion(&f->resume);
	}
	return f->uv;
}

static int event_count(struct notifier_block *nb, unsigned long event, void *data)
{
	int *count = (int *)(nb + 1);
	struct mt6797_provider_status status;

	mt6797_provider_status(&status);
	(*count)++;
	return NOTIFY_OK;
}

static void provider_orders(struct kunit *test)
{
	struct { struct notifier_block nb; int count; } listener = {
		.nb.notifier_call = event_count,
	};
	struct provider_fixture f[2] = { { .uv = 1100000 }, { .uv = 1150000 } };
	struct mt6797_rail_io io[2] = {
		{ .read = fixture_read, .data = &f[0], .step_uv = 10000 },
		{ .read = fixture_read, .data = &f[1], .step_uv = 25000 },
	};
	struct mt6797_provider_status status;
	struct platform_device *dev[2];
	int order, id, uv[2], before;
	u64 generation, preflight_generation;

	KUNIT_ASSERT_EQ(test, mt6797_provider_subscribe(&listener.nb), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_GT(test, listener.count, 0);
	for (order = 0; order < 2; order++) {
		for (id = 0; id < 2; id++) {
			dev[id] = platform_device_register_simple("rail-fixture",
								 id, NULL, 0);
			KUNIT_ASSERT_FALSE(test, IS_ERR(dev[id]));
		}
		KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev[order]->dev,
						order, &io[order]), 0);
		mt6797_provider_status(&status);
		generation = status.engine.generation;
		preflight_generation = status.preflight.generation;
		KUNIT_EXPECT_EQ(test, status.attached, BIT(order));
		KUNIT_EXPECT_EQ(test, status.readiness, -EPROBE_DEFER);
		KUNIT_EXPECT_FALSE(test, status.engine.ready);
		KUNIT_EXPECT_EQ(test, mt6797_provider_sample(generation, uv), -EPROBE_DEFER);
		KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev[order]->dev,
						order, &io[order]), -EBUSY);
		KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev[!order]->dev,
						!order, &io[!order]), 0);
		KUNIT_EXPECT_EQ(test, mt6797_provider_sample(generation, uv), -ESTALE);
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
		KUNIT_EXPECT_GT(test, status.preflight.generation, preflight_generation);
		KUNIT_EXPECT_NE(test, status.preflight.error, 0);
		KUNIT_EXPECT_FALSE(test, status.engine.ready);
		KUNIT_EXPECT_EQ(test, mt6797_provider_sample(status.engine.generation, uv), 0);
		KUNIT_EXPECT_EQ(test, uv[0], f[0].uv);
		KUNIT_EXPECT_EQ(test, uv[1], f[1].uv);
		mt6797_preflight_test_flush();
		before = listener.count;
		platform_device_unregister(dev[!order]);
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.attached, BIT(order));
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_GT(test, listener.count, before);
		dev[!order] = platform_device_register_simple("rail-fixture", !order, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(dev[!order]));
		KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev[!order]->dev,
						!order, &io[!order]), 0);
		platform_device_unregister(dev[!order]);
		platform_device_unregister(dev[order]);
	}
	KUNIT_EXPECT_EQ(test, mt6797_provider_unsubscribe(&listener.nb), 0);
}

static void provider_invalid(struct kunit *test)
{
	struct provider_fixture f = { .uv = -EIO };
	struct mt6797_rail_io io = { .read = fixture_read, .data = &f, .step_uv = 25000 };
	struct mt6797_provider_status status;
	struct platform_device *dev;

	dev = platform_device_register_simple("rail-fixture", 0, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev->dev, MT6797_SRAM, &io), -EIO);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.attached, 0UL);
	platform_device_unregister(dev);
}

struct sample_task {
	u64 generation;
	int result;
	struct completion done;
};

static int sample_thread(void *data)
{
	struct sample_task *task = data;
	int uv[2];

	task->result = mt6797_provider_sample(task->generation, uv);
	complete(&task->done);
	return 0;
}

struct detach_task {
	struct platform_device *dev;
	struct completion started;
	struct completion done;
};

static int detach_thread(void *data)
{
	struct detach_task *task = data;

	complete(&task->started);
	platform_device_unregister(task->dev);
	complete(&task->done);
	return 0;
}

static void provider_inflight(struct kunit *test)
{
	struct provider_fixture f[2] = { { .uv = 1100000 }, { .uv = 1150000 } };
	struct mt6797_rail_io io = { .read = fixture_read };
	struct platform_device *dev[2];
	struct mt6797_provider_status status;
	struct sample_task sample;
	struct detach_task detach;
	struct task_struct *reader, *remover;
	int i;

	init_completion(&sample.done);
	init_completion(&detach.started);
	init_completion(&detach.done);
	for (i = 0; i < 2; i++) {
		init_completion(&f[i].entered);
		init_completion(&f[i].resume);
		dev[i] = platform_device_register_simple("rail-fixture", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(dev[i]));
		io.data = &f[i];
		io.step_uv = i ? 25000 : 10000;
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&dev[i]->dev, i, &io), 0);
	}
	mt6797_provider_status(&status);
	sample.generation = status.engine.generation;
	f[0].pause = true;
	reader = kthread_run(sample_thread, &sample, "rail-read");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&f[0].entered, HZ), 0UL);
	detach.dev = dev[0];
	remover = kthread_run(detach_thread, &detach, "rail-detach");
	KUNIT_ASSERT_FALSE(test, IS_ERR(remover));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.started, HZ), 0UL);
	KUNIT_EXPECT_FALSE(test, completion_done(&detach.done));
	complete(&f[0].resume);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&sample.done, HZ), 0UL);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, sample.result, 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.attached, BIT(MT6797_SRAM));
	KUNIT_EXPECT_GT(test, status.engine.generation, sample.generation);
	platform_device_unregister(dev[1]);
}

struct lifecycle_clock {
	struct clk_hw hw;
	unsigned long rate;
	bool fail, abort, pause;
	struct completion entered, resume;
	unsigned int pre, post, aborted;
	struct notifier_block nb;
};

static unsigned long lifecycle_recalc(struct clk_hw *hw, unsigned long parent)
{
	return container_of(hw, struct lifecycle_clock, hw)->rate;
}

static int lifecycle_determine(struct clk_hw *hw, struct clk_rate_request *req)
{
	return 0;
}

static int lifecycle_set(struct clk_hw *hw, unsigned long rate,
			 unsigned long parent)
{
	struct lifecycle_clock *c = container_of(hw, struct lifecycle_clock, hw);

	if (c->pause) {
		complete(&c->entered);
		wait_for_completion(&c->resume);
	}
	if (c->fail)
		return -EIO;
	c->rate = rate;
	return 0;
}

static int lifecycle_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct lifecycle_clock *c = container_of(nb, struct lifecycle_clock, nb);

	if (event == PRE_RATE_CHANGE) {
		c->pre++;
		if (c->abort)
			return NOTIFY_BAD;
	} else if (event == POST_RATE_CHANGE) {
		c->post++;
	} else if (event == ABORT_RATE_CHANGE) {
		c->aborted++;
	}
	return NOTIFY_OK;
}

static const struct clk_ops lifecycle_ops = {
	.recalc_rate = lifecycle_recalc,
	.determine_rate = lifecycle_determine,
	.set_rate = lifecycle_set,
};

struct lifecycle_listener {
	struct notifier_block nb;
	struct mt6797_provider_status last;
	unsigned int count;
};

static int lifecycle_public(struct notifier_block *nb, unsigned long event, void *data)
{
	struct lifecycle_listener *l = container_of(nb, struct lifecycle_listener, nb);
	struct mt6797_provider_status snapshot;

	/* Reentrant status query must not deadlock. Payload owns no live refs. */
	mt6797_provider_status(&snapshot);
	l->last = *(struct mt6797_provider_status *)data;
	l->count++;
	return NOTIFY_OK;
}

struct lifecycle_rate_task {
	struct clk *clk;
	struct completion done;
	int ret;
};

static int lifecycle_rate_thread(void *data)
{
	struct lifecycle_rate_task *task = data;

	task->ret = clk_set_rate(task->clk, 175);
	complete(&task->done);
	return 0;
}

/* Real CCF (including its unmatched PRE path), real OPP notifier/core,
 * production worker/registry and queued publication. Discovery is injected;
 * this is not an OF/NVMEM or regulator-adapter integration fixture.
 */
static void preflight_lifecycle(struct kunit *test)
{
	struct clk_init_data init = { .name = "preflight-clock", .ops = &lifecycle_ops };
	struct lifecycle_clock c = { .hw.init = &init, .rate = 100 };
	struct lifecycle_listener l = { .nb.notifier_call = lifecycle_public };
	struct provider_fixture f[2] = { { .uv = 1100000 }, { .uv = 1150000 } };
	struct mt6797_rail_io io = { .read = fixture_read };
	struct platform_device *dev, *rail[2];
	struct mt6797_provider_status status;
	struct clk *clk;
	struct lifecycle_rate_task task;
	struct task_struct *thread;
	u64 generation;
	int i;

	dev = platform_device_register_simple("preflight-lifecycle", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &c.hw), 0);
	clk = clk_hw_get_clk(&c.hw, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(clk));
	c.nb.notifier_call = lifecycle_event;
	KUNIT_ASSERT_EQ(test, clk_notifier_register(clk, &c.nb), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&dev->dev, 100, 800000), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&dev->dev, 200, 900000), 0);
	KUNIT_ASSERT_EQ(test, mt6797_provider_subscribe(&l.nb), 0);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start(&dev->dev, clk), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, -EPROBE_DEFER);
	for (i = 0; i < 2; i++) {
		rail[i] = platform_device_register_simple("lifecycle-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(rail[i]));
		io.data = &f[i];
		io.step_uv = i ? 25000 : 10000;
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&rail[i]->dev, i, &io), 0);
	}
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, 0);
	generation = l.last.preflight.generation;
	c.fail = true;
	/* CCF ignores set_rate's return and sends no terminal notification. */
	clk_set_rate(clk, 150);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, c.pre, 1U);
	KUNIT_EXPECT_EQ(test, c.post, 0U);
	KUNIT_EXPECT_EQ(test, c.aborted, 0U);
	KUNIT_EXPECT_GT(test, l.last.preflight.generation, generation);
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, 0);
	KUNIT_EXPECT_EQ(test, l.last.preflight.rate[0][0], 100UL);
	c.fail = false;
	KUNIT_EXPECT_EQ(test, clk_set_rate(clk, 150), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, c.post, 1U);
	KUNIT_EXPECT_EQ(test, l.last.preflight.uv[0][0], 900000UL);
	c.abort = true;
	KUNIT_EXPECT_LT(test, clk_set_rate(clk, 175), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, c.aborted, 1U);
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, 0);
	c.abort = false;
	KUNIT_ASSERT_EQ(test, dev_pm_opp_disable(&dev->dev, 200), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_LT(test, l.last.preflight.error, 0);
	generation = l.last.preflight.generation;
	mt6797_preflight_test_hint(&dev->dev);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.generation, generation);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_enable(&dev->dev, 200), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_adjust_voltage(&dev->dev, 200,
						     950000, 950000, 950000), 0);
	mt6797_preflight_test_hint(&dev->dev);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.uv[0][0], 950000UL);
	/* PRE runs under CCF's lock while an independent OPP mutation arrives.
	 * The worker must wait and publish the newest generation after both.
	 */
	init_completion(&c.entered);
	init_completion(&c.resume);
	init_completion(&task.done);
	task.clk = clk;
	c.pause = true;
	thread = kthread_run(lifecycle_rate_thread, &task, "preflight-rate");
	KUNIT_ASSERT_FALSE(test, IS_ERR(thread));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&c.entered, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(&dev->dev, 200,
						     975000, 975000, 975000), 0);
	complete(&c.resume);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&task.done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, task.ret, 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, 0);
	KUNIT_EXPECT_EQ(test, l.last.preflight.rate[0][0], 175UL);
	KUNIT_EXPECT_EQ(test, l.last.preflight.uv[0][0], 975000UL);
	c.pause = false;
	platform_device_unregister(rail[0]);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, -EPROBE_DEFER);
	rail[0] = platform_device_register_simple("lifecycle-rail", 0, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(rail[0]));
	io.data = &f[0];
	io.step_uv = 10000;
	KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&rail[0]->dev, 0, &io), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, l.last.preflight.error, 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.preflight.generation, l.last.preflight.generation);
	KUNIT_EXPECT_EQ(test, status.engine.generation, l.last.preflight.provider_generation);
	KUNIT_EXPECT_FALSE(test, status.engine.ready);
	KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
	mt6797_preflight_test_stop();
	mt6797_provider_unsubscribe(&l.nb);
	platform_device_unregister(rail[0]);
	platform_device_unregister(rail[1]);
	clk_notifier_unregister(clk, &c.nb);
	clk_put(clk);
	clk_hw_unregister(&c.hw);
	dev_pm_opp_remove_all_dynamic(&dev->dev);
	platform_device_unregister(dev);
}

static struct kunit_case provider_cases[] = {
	KUNIT_CASE(preflight_opp_core),
	KUNIT_CASE(preflight_lifecycle),
	KUNIT_CASE(provider_orders),
	KUNIT_CASE(provider_invalid),
	KUNIT_CASE(provider_inflight),
	{}
};
static struct kunit_suite provider_suite = {
	.name = "mt6797-provider",
	.test_cases = provider_cases,
};
kunit_test_suite(provider_suite);
MODULE_LICENSE("GPL");
