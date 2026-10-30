// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/resource.h>
#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kthread.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/mt6797.h>
#include <linux/regulator/consumer.h>
#include "mt6797-provider.h"

static void unregister_device(void *data)
{
	platform_device_unregister(data);
}

static void remove_opps(void *data)
{
	dev_pm_opp_remove_all_dynamic(data);
}

static void clear_config(void *data)
{
	dev_pm_opp_clear_config((long)data);
}

static void unsubscribe(void *data)
{
	mt6797_provider_unsubscribe(data);
}

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
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, unregister_device, pdev), 0);
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
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, clear_config,
						(void *)(long)token), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_ceiling(&pdev->dev, 100,
						     &freq, &uv), -ERANGE);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, remove_opps, &pdev->dev), 0);
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
	{
		struct opp_table *table = dev_pm_opp_get_opp_table(&pdev->dev);
		unsigned long rates[2] = { 100, 101 }, freqs[2], volts[2];
		int ret;

		KUNIT_ASSERT_FALSE(test, IS_ERR(table));
		ret = dev_pm_opp_snapshot_lock(table, rates, freqs, volts, 2);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (!ret) {
			KUNIT_EXPECT_EQ(test, volts[0], 800000UL);
			KUNIT_EXPECT_EQ(test, volts[1], 900000UL);
			KUNIT_EXPECT_EQ(test, dev_pm_opp_snapshot_lock(table,
				 rates, freqs, volts, 2), -EAGAIN);
			dev_pm_opp_snapshot_unlock(table);
		}
		rates[0] = 99;
		KUNIT_EXPECT_EQ(test, dev_pm_opp_snapshot_lock(table,
			 rates, freqs, volts, 2), -ERANGE);
		dev_pm_opp_put_opp_table(table);
	}
	kunit_release_action(test, remove_opps, &pdev->dev);
	kunit_release_action(test, clear_config, (void *)(long)token);
	/* Reinstallation after clearing must work on the same device. */
	token = dev_pm_opp_set_config(&pdev->dev, &config);
	KUNIT_ASSERT_GT(test, token, 0);
	dev_pm_opp_clear_config(token);
	kunit_release_action(test, unregister_device, pdev);
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
	struct { struct notifier_block nb; int count; } *listener;
	struct provider_fixture *f;
	struct mt6797_rail_io io[2] = {
		{ .read = fixture_read, .step_uv = 10000 },
		{ .read = fixture_read, .step_uv = 25000 },
	};
	struct mt6797_provider_status status;
	struct platform_device *dev[2];
	int order, id, uv[2], before;
	u64 generation, preflight_generation;

	listener = kunit_kzalloc(test, sizeof(*listener), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, listener);
	f = kunit_kcalloc(test, 2, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	listener->nb.notifier_call = event_count;
	f[0].uv = 1100000;
	f[1].uv = 1150000;
	io[0].data = &f[0];
	io[1].data = &f[1];
	KUNIT_ASSERT_EQ(test, mt6797_provider_subscribe(&listener->nb), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, unsubscribe,
						&listener->nb), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_GT(test, listener->count, 0);
	for (order = 0; order < 2; order++) {
		for (id = 0; id < 2; id++) {
			dev[id] = platform_device_register_simple("rail-fixture",
								 id, NULL, 0);
			KUNIT_ASSERT_FALSE(test, IS_ERR(dev[id]));
			KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
					unregister_device, dev[id]), 0);
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
		before = listener->count;
		kunit_release_action(test, unregister_device, dev[!order]);
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.attached, BIT(order));
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_GT(test, listener->count, before);
		dev[!order] = platform_device_register_simple("rail-fixture", !order, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(dev[!order]));
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
					unregister_device, dev[!order]), 0);
		KUNIT_EXPECT_EQ(test, mt6797_provider_attach(&dev[!order]->dev,
						!order, &io[!order]), 0);
		kunit_release_action(test, unregister_device, dev[!order]);
		kunit_release_action(test, unregister_device, dev[order]);
	}
	kunit_release_action(test, unsubscribe, &listener->nb);
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
	complete_all(&task->done);
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
	complete_all(&task->done);
	return 0;
}

static void provider_inflight(struct kunit *test)
{
	struct provider_fixture f[2] = { { .uv = 1100000 }, { .uv = 1150000 } };
	struct mt6797_rail_io io = { .read = fixture_read };
	struct platform_device *dev[2] = {};
	struct mt6797_provider_status status;
	struct sample_task sample;
	struct detach_task detach;
	struct task_struct *reader = NULL, *remover = NULL;
	int i, ret;

	init_completion(&sample.done);
	init_completion(&detach.started);
	init_completion(&detach.done);
	for (i = 0; i < 2; i++) {
		init_completion(&f[i].entered);
		init_completion(&f[i].resume);
	}
	for (i = 0; i < 2; i++) {
		dev[i] = platform_device_register_simple("rail-fixture", i, NULL, 0);
		KUNIT_EXPECT_FALSE(test, IS_ERR(dev[i]));
		if (IS_ERR(dev[i]))
			goto out;
		io.data = &f[i];
		io.step_uv = i ? 25000 : 10000;
		ret = mt6797_provider_attach(&dev[i]->dev, i, &io);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto out;
	}
	mt6797_provider_status(&status);
	sample.generation = status.engine.generation;
	f[0].pause = true;
	reader = kthread_run(sample_thread, &sample, "rail-read");
	KUNIT_EXPECT_FALSE(test, IS_ERR(reader));
	if (IS_ERR(reader))
		goto out;
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&f[0].entered, HZ), 0UL);
	detach.dev = dev[0];
	remover = kthread_run(detach_thread, &detach, "rail-detach");
	KUNIT_EXPECT_FALSE(test, IS_ERR(remover));
	if (IS_ERR(remover))
		goto out;
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.started, HZ), 0UL);
	KUNIT_EXPECT_FALSE(test, completion_done(&detach.done));
	complete(&f[0].resume);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&sample.done, HZ), 0UL);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, sample.result, 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.attached, BIT(MT6797_SRAM));
	KUNIT_EXPECT_GT(test, status.engine.generation, sample.generation);
out:
	complete_all(&f[0].resume);
	if (!IS_ERR_OR_NULL(reader))
		wait_for_completion(&sample.done);
	if (!IS_ERR_OR_NULL(remover)) {
		wait_for_completion(&detach.done);
		dev[0] = NULL;
	}
	for (i = 0; i < 2; i++)
		if (!IS_ERR_OR_NULL(dev[i]))
			platform_device_unregister(dev[i]);
	mt6797_preflight_test_flush();
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
	complete_all(&task->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

/* Real CCF (including its unmatched PRE path), real OPP notifier/core,
 * production worker/registry and queued publication. Discovery is injected;
 * this is not an OF/NVMEM or regulator-adapter integration fixture.
 */
struct lifecycle_fixture {
	struct lifecycle_clock c;
	struct lifecycle_listener l;
	struct provider_fixture f[2];
	struct platform_device *dev, *rail[2];
	struct clk *clk;
	struct lifecycle_rate_task task;
	struct task_struct *thread;
	bool hw_registered, clock_notify, subscribed, started;
};

static void lifecycle_cleanup(void *data)
{
	struct lifecycle_fixture *x = data;
	int i;

	/* Release the provider before joining a possibly blocked CCF caller. */
	complete_all(&x->c.resume);
	if (!IS_ERR_OR_NULL(x->thread))
		kthread_stop(x->thread);
	if (x->started)
		mt6797_preflight_test_stop();
	if (x->subscribed)
		mt6797_provider_unsubscribe(&x->l.nb);
	for (i = 0; i < 2; i++)
		if (!IS_ERR_OR_NULL(x->rail[i]))
			platform_device_unregister(x->rail[i]);
	if (x->clock_notify)
		clk_notifier_unregister(x->clk, &x->c.nb);
	if (!IS_ERR_OR_NULL(x->clk))
		clk_put(x->clk);
	if (x->hw_registered)
		clk_hw_unregister(&x->c.hw);
	if (!IS_ERR_OR_NULL(x->dev)) {
		dev_pm_opp_remove_all_dynamic(&x->dev->dev);
		platform_device_unregister(x->dev);
	}
	mt6797_preflight_test_flush();
}

static void run_preflight_lifecycle(struct kunit *test, int stop_at)
{
	struct clk_init_data init = { .name = "preflight-clock", .ops = &lifecycle_ops };
	struct lifecycle_fixture *x;
	struct mt6797_rail_io io = { .read = fixture_read };
	struct mt6797_provider_status status;
	u64 generation;
	int i;

	x = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, x);
	init_completion(&x->c.entered);
	init_completion(&x->c.resume);
	init_completion(&x->task.done);
	x->c.hw.init = &init;
	x->c.rate = 100;
	x->l.nb.notifier_call = lifecycle_public;
	x->f[0].uv = 1100000;
	x->f[1].uv = 1150000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				lifecycle_cleanup, x), 0);
	x->dev = platform_device_register_simple("preflight-lifecycle", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(x->dev));
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &x->c.hw), 0);
	x->hw_registered = true;
	x->clk = clk_hw_get_clk(&x->c.hw, NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(x->clk));
	x->c.nb.notifier_call = lifecycle_event;
	KUNIT_ASSERT_EQ(test, clk_notifier_register(x->clk, &x->c.nb), 0);
	x->clock_notify = true;
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&x->dev->dev, 100, 800000), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&x->dev->dev, 200, 900000), 0);
	KUNIT_ASSERT_EQ(test, mt6797_provider_subscribe(&x->l.nb), 0);
	x->subscribed = true;
	if (stop_at == 1) {
		kunit_release_action(test, lifecycle_cleanup, x);
		return;
	}
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start(&x->dev->dev, x->clk), 0);
	x->started = true;
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, -EPROBE_DEFER);
	for (i = 0; i < 2; i++) {
		x->rail[i] = platform_device_register_simple("lifecycle-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(x->rail[i]));
		io.data = &x->f[i];
		io.step_uv = i ? 25000 : 10000;
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&x->rail[i]->dev, i, &io), 0);
	}
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, 0);
	generation = x->l.last.preflight.generation;
	x->c.fail = true;
	/* CCF ignores set_rate's return and sends no terminal notification. */
	clk_set_rate(x->clk, 150);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->c.pre, 1U);
	KUNIT_EXPECT_EQ(test, x->c.post, 0U);
	KUNIT_EXPECT_EQ(test, x->c.aborted, 0U);
	KUNIT_EXPECT_GT(test, x->l.last.preflight.generation, generation);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, 0);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.rate[0][0], 100UL);
	x->c.fail = false;
	KUNIT_EXPECT_EQ(test, clk_set_rate(x->clk, 150), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->c.post, 1U);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.uv[0][0], 900000UL);
	x->c.abort = true;
	KUNIT_EXPECT_LT(test, clk_set_rate(x->clk, 175), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->c.aborted, 1U);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, 0);
	x->c.abort = false;
	KUNIT_ASSERT_EQ(test, dev_pm_opp_disable(&x->dev->dev, 200), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_LT(test, x->l.last.preflight.error, 0);
	generation = x->l.last.preflight.generation;
	mt6797_preflight_test_hint(&x->dev->dev);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.generation, generation);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_enable(&x->dev->dev, 200), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_adjust_voltage(&x->dev->dev, 200,
						     950000, 950000, 950000), 0);
	mt6797_preflight_test_hint(&x->dev->dev);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.uv[0][0], 950000UL);
	/* PRE runs under CCF's lock while an independent OPP mutation arrives.
	 * The worker must wait and publish the newest generation after both.
	 */
	init_completion(&x->c.entered);
	init_completion(&x->c.resume);
	init_completion(&x->task.done);
	x->task.clk = x->clk;
	x->c.pause = true;
	x->thread = kthread_run(lifecycle_rate_thread, &x->task, "preflight-rate");
	KUNIT_ASSERT_FALSE(test, IS_ERR(x->thread));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&x->c.entered, HZ), 0UL);
	if (stop_at == 2) {
		kunit_release_action(test, lifecycle_cleanup, x);
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.attached, 0UL);
		KUNIT_EXPECT_TRUE(test, completion_done(&x->task.done));
		return;
	}
	KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(&x->dev->dev, 200,
						     975000, 975000, 975000), 0);
	complete(&x->c.resume);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&x->task.done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, x->task.ret, 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, 0);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.rate[0][0], 175UL);
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.uv[0][0], 975000UL);
	x->c.pause = false;
	platform_device_unregister(x->rail[0]);
	x->rail[0] = NULL;
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, -EPROBE_DEFER);
	x->rail[0] = platform_device_register_simple("lifecycle-rail", 0, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(x->rail[0]));
	io.data = &x->f[0];
	io.step_uv = 10000;
	KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&x->rail[0]->dev, 0, &io), 0);
	mt6797_preflight_test_flush();
	KUNIT_EXPECT_EQ(test, x->l.last.preflight.error, 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.preflight.generation, x->l.last.preflight.generation);
	KUNIT_EXPECT_EQ(test, status.engine.generation, x->l.last.preflight.provider_generation);
	KUNIT_EXPECT_FALSE(test, status.engine.ready);
	KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
	kunit_release_action(test, lifecycle_cleanup, x);
}

static void preflight_lifecycle(struct kunit *test)
{
	run_preflight_lifecycle(test, 0);
}

static void preflight_setup_cleanup(struct kunit *test)
{
	run_preflight_lifecycle(test, 1);
}

static void preflight_blocked_cleanup(struct kunit *test)
{
	run_preflight_lifecycle(test, 2);
}

struct init_failure_fixture {
	struct notifier_block nb;
	struct completion entered, resume, done;
	struct task_struct *thread;
	int fail_after, ret;
	unsigned int calls;
	bool subscribed;
};

static int init_failure_notify(struct notifier_block *nb, unsigned long event,
			       void *data)
{
	struct init_failure_fixture *f = container_of(nb,
					struct init_failure_fixture, nb);

	f->calls++;
	complete(&f->entered);
	wait_for_completion(&f->resume);
	return NOTIFY_OK;
}

static int init_failure_thread(void *data)
{
	struct init_failure_fixture *f = data;

	f->ret = mt6797_preflight_test_init_failure(f->fail_after,
						mt6797_preflight_test_event);
	complete(&f->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static void init_failure_cleanup(void *data)
{
	struct init_failure_fixture *f = data;

	complete_all(&f->resume);
	if (!IS_ERR_OR_NULL(f->thread))
		kthread_stop(f->thread);
	if (f->subscribed)
		mt6797_provider_unsubscribe(&f->nb);
	mt6797_preflight_test_flush();
}

static void preflight_init_unwind(struct kunit *test)
{
	struct init_failure_fixture *f;
	unsigned int calls;
	int step;

	for (step = 1; step <= 4; step++) {
		f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, f);
		init_completion(&f->entered);
		init_completion(&f->resume);
		init_completion(&f->done);
		f->nb.notifier_call = init_failure_notify;
		f->fail_after = step;
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
					init_failure_cleanup, f), 0);
		KUNIT_ASSERT_EQ(test, mt6797_provider_subscribe(&f->nb), 0);
		f->subscribed = true;
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&f->entered, HZ), 0UL);
		f->thread = kthread_run(init_failure_thread, f, "preflight-init");
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->thread));
		/* Unwind cannot finish while queued delivery still owns storage. */
		KUNIT_EXPECT_EQ(test, wait_for_completion_timeout(&f->done, HZ / 10), 0UL);
		complete_all(&f->resume);
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&f->done, HZ), 0UL);
		KUNIT_EXPECT_EQ(test, f->ret,
			step >= 3 && !IS_ENABLED(CONFIG_NVMEM) ?
			-EOPNOTSUPP : -EFAULT);
		calls = f->calls;
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, f->calls, calls);
		kunit_release_action(test, init_failure_cleanup, f);
	}
}

struct conversion_fixture {
	struct kunit *test;
	struct platform_device *dev[3], *rail[2];
	struct clk *clk;
	int uv[2], writes, fail_write;
	int live_floor[3];
	bool started, commit_error;
};

struct conversion_endpoint {
	struct conversion_fixture *fixture;
	int id;
};

static int conversion_read(void *data)
{
	struct conversion_endpoint *e = data;

	return e->fixture->uv[e->id];
}

static int conversion_write(void *data, int uv)
{
	struct conversion_endpoint *e = data;
	struct conversion_fixture *f = e->fixture;

	if (++f->writes == f->fail_write) {
		if (f->commit_error)
			f->uv[e->id] = uv;
		return -EIO;
	}
	f->uv[e->id] = uv;
	for (int i = 0; i < 3; i++)
		KUNIT_EXPECT_GE(f->test, f->uv[0], f->live_floor[i]);
	KUNIT_EXPECT_GE(f->test, f->uv[0], 1040000);
	KUNIT_EXPECT_LE(f->test, f->uv[0], f->uv[1]);
	KUNIT_EXPECT_LE(f->test, f->uv[1] - f->uv[0], 160000);
	return 0;
}

static void conversion_cleanup(void *data)
{
	struct conversion_fixture *f = data;
	int i;

	if (f->started)
		mt6797_preflight_test_stop();
	for (i = 0; i < 2; i++)
		if (!IS_ERR_OR_NULL(f->rail[i]))
			platform_device_unregister(f->rail[i]);
	for (i = 0; i < 3; i++)
		if (!IS_ERR_OR_NULL(f->dev[i])) {
			dev_pm_opp_remove_all_dynamic(&f->dev[i]->dev);
			platform_device_unregister(f->dev[i]);
		}
	if (!IS_ERR_OR_NULL(f->clk))
		clk_unregister_fixed_rate(f->clk);
}

/* Production reservation, registry and engine, real CCF/OPP. Discovery and
 * transports are injected: this does not claim OF/NVMEM or adapter coverage.
 */
struct ownership_thread {
	struct device *dev;
	struct completion *start;
	struct completion done;
	u64 cookie;
	unsigned int user;
	int ret;
};

static int ownership_acquire_thread(void *arg)
{
	struct ownership_thread *t = arg;

	wait_for_completion(t->start);
	t->ret = mt6797_consumer_acquire(t->dev, t->user, &t->cookie);
	complete(&t->done);
	return 0;
}

struct ownership_notifier {
	struct notifier_block nb;
	struct device *dev;
};

static int ownership_invalidate(struct notifier_block *nb, unsigned long event,
				void *data)
{
	struct ownership_notifier *n = container_of(nb, struct ownership_notifier, nb);

	if (event == REGULATOR_EVENT_PRE_VOLTAGE_CHANGE) {
		/* Must not deadlock against a table snapshot held across core calls. */
		dev_pm_opp_adjust_voltage(n->dev, 100, 1070000, 1070000, 1070000);
		mt6797_preflight_test_event();
	}
	return NOTIFY_OK;
}

static void ownership_run(struct kunit *test, int failure)
{
	struct conversion_fixture *f;
	struct conversion_endpoint *e;
	struct mt6797_provider_status status;
	struct device *dev[3];
	int i, guard, vote;
	unsigned int refs[3];
	u64 cookie[3], shared;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	e = kunit_kcalloc(test, 2, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	f->test = test;
	f->uv[0] = 1120000;
	f->uv[1] = 1150000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	f->clk = clk_register_fixed_rate(NULL, "conversion-clock", NULL, 0, 100);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->clk));
	for (i = 0; i < 3; i++) {
		f->dev[i] = platform_device_register_simple("conversion-user", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->dev[i]));
		dev[i] = &f->dev[i]->dev;
		f->live_floor[i] = 1040000 + i * 20000;
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 100, f->live_floor[i]), 0);
	}
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start_users(dev, f->clk), 0);
	f->started = true;
	for (i = 0; i < 2; i++) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[i], .step_uv = i ? 25000 : 10000,
		};

		e[i].fixture = f;
		e[i].id = i;
		f->rail[i] = platform_device_register_simple("conversion-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[i]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[i]->dev, i, &io), 0);
		mt6797_preflight_test_flush();
		mt6797_provider_status(&status);
		if (!i)
			KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
				&status.preflight), -EPROBE_DEFER);
	}
	mt6797_provider_status(&status);
	status.preflight.generation--;
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), -ESTALE);
	KUNIT_EXPECT_EQ(test, f->writes, 0);
	mt6797_provider_status(&status);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), 0);
	KUNIT_EXPECT_GT(test, f->writes, 0);
	KUNIT_EXPECT_EQ(test, f->uv[1], 1200000);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_FALSE(test, status.engine.ready);
	KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, status.engine.floor[i], f->live_floor[i]);
	mt6797_preflight_test_flush();
	for (i = 0; i < 3; i++) {
		refs[i] = kref_read(&dev[i]->kobj.kref);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[i], i, &cookie[i]), 0);
		KUNIT_EXPECT_GE(test, f->uv[0], f->live_floor[i]);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i, &guard, &vote), 2);
		KUNIT_EXPECT_EQ(test, vote, f->live_floor[i]);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[i]->kobj.kref), refs[i] + 1);
	}
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), -EBUSY);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[0], 0, &shared), 0);
	KUNIT_EXPECT_NE(test, shared, cookie[0]);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_release(0, cookie[0]), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, cookie[0]), -ESTALE);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.adopted, 7UL);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_release(0, shared), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_release(1, cookie[1]), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_release(2, cookie[2]), 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.adopted, 0UL);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i, &guard, &vote), 1);
		KUNIT_EXPECT_GE(test, guard, f->live_floor[i]);
		KUNIT_EXPECT_EQ(test, vote, 0);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[i]->kobj.kref), refs[i]);
	}
	KUNIT_EXPECT_GE(test, f->uv[0], 1040000);
	/* Competing production acquisitions serialize complete core transactions. */
	{
		DECLARE_COMPLETION(start);
		struct ownership_thread t[2] = {};
		struct task_struct *task[2];

		for (i = 0; i < 2; i++) {
			t[i].dev = dev[i];
			t[i].user = i;
			t[i].start = &start;
			init_completion(&t[i].done);
			task[i] = kthread_create(ownership_acquire_thread, &t[i], "rail-acquire");
			if (IS_ERR(task[i])) {
				complete_all(&start);
				while (i--) {
					wait_for_completion(&t[i].done);
					put_task_struct(task[i]);
				}
				KUNIT_FAIL(test, "cannot create acquisition thread");
				return;
			}
			get_task_struct(task[i]);
			wake_up_process(task[i]);
		}
		complete_all(&start);
		for (i = 0; i < 2; i++) {
			wait_for_completion(&t[i].done);
			put_task_struct(task[i]);
		}
		for (i = 0; i < 2; i++) {
			KUNIT_ASSERT_EQ(test, t[i].ret, 0);
			KUNIT_ASSERT_EQ(test, mt6797_consumer_release(i, t[i].cookie), 0);
		}
	}
	if (failure == 3) {
		struct ownership_notifier n = {
			.nb.notifier_call = ownership_invalidate, .dev = dev[1],
		};
		int ret, writes = f->writes;

		KUNIT_ASSERT_EQ(test, mt6797_preflight_test_regulator_notifier(1, &n.nb, true), 0);
		ret = mt6797_consumer_acquire(dev[1], 1, &shared);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_regulator_notifier(1, &n.nb, false), 0);
		KUNIT_EXPECT_EQ(test, ret, -ESTALE);
		KUNIT_EXPECT_EQ(test, f->writes, writes);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(1, &guard, &vote), 1);
		KUNIT_EXPECT_GE(test, guard, f->live_floor[1]);
		return;
	}
	if (failure == 2) {
		f->fail_write = f->writes + 1;
		KUNIT_EXPECT_EQ(test, mt6797_consumer_acquire(dev[1], 1, &shared), -EIO);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(1, &guard, &vote), 1);
		KUNIT_EXPECT_GE(test, guard, f->live_floor[1]);
		KUNIT_EXPECT_GE(test, f->uv[0], f->live_floor[2]);
		return;
	}
	/* Late CCI binding gets a new lifetime; an old remove cannot release it. */
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[2], 2, &shared), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, cookie[2]), -ESTALE);
	if (failure == 1) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[0], .step_uv = 10000,
		};

		platform_device_unregister(f->rail[0]);
		f->rail[0] = NULL;
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, shared), -EPROBE_DEFER);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 2);
		KUNIT_EXPECT_EQ(test, vote, f->live_floor[2]);
		f->rail[0] = platform_device_register_simple("conversion-rail", 0, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[0]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[0]->dev, 0, &io), 0);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_consumer_acquire(dev[2], 2, &shared), -EBUSY);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_recover(), 0);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 1);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2]);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[2], 2, &shared), 0);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_release(2, shared), 0);
		return;
	}
	/* A failed last-owner transfer retains the actual core request. */
	f->fail_write = f->writes + 1;
	f->commit_error = true;
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, shared), -EIO);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 2);
	KUNIT_EXPECT_EQ(test, vote, f->live_floor[2]);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.adopted, BIT(2));
	KUNIT_EXPECT_EQ(test, status.retained, BIT(2));
	KUNIT_EXPECT_EQ(test, status.active, 0UL);
	KUNIT_EXPECT_EQ(test, status.ownership_error[2], -EIO);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2] + 1);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, shared), -ESTALE);
	KUNIT_EXPECT_GE(test, f->uv[0], f->live_floor[2]);
	/* Bad selected voltage must not clear the fault or retire a real vote. */
	f->uv[1] = 1175000;
	KUNIT_EXPECT_EQ(test, mt6797_consumer_recover(), -ERANGE);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 2);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_TRUE(test, status.engine.fault);
	f->uv[1] = 1200000;
	KUNIT_ASSERT_EQ(test, mt6797_consumer_recover(), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(2, &guard, &vote), 1);
	KUNIT_EXPECT_GE(test, guard, f->live_floor[2]);
	KUNIT_EXPECT_EQ(test, kref_read(&dev[2]->kobj.kref), refs[2]);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_release(2, shared), -ESTALE);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[2], 2, &shared), 0);
	KUNIT_ASSERT_EQ(test, mt6797_consumer_release(2, shared), 0);
}

static void production_ownership(struct kunit *test)
{
	ownership_run(test, 0);
}

static void ownership_provider_generation(struct kunit *test)
{
	ownership_run(test, 1);
}

static void ownership_first_vote_failure(struct kunit *test)
{
	ownership_run(test, 2);
}

static void ownership_notifier_generation(struct kunit *test)
{
	ownership_run(test, 3);
}

static void production_conversion(struct kunit *test)
{
	struct conversion_fixture *f;
	struct conversion_endpoint *e;
	struct mt6797_provider_status status;
	struct device *dev[3];
	int i, writes;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	e = kunit_kcalloc(test, 2, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	f->test = test;
	f->uv[0] = 1040000;
	f->uv[1] = 1100000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	f->clk = clk_register_fixed_rate(NULL, "conversion-clock", NULL, 0, 100);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->clk));
	for (i = 0; i < 3; i++) {
		f->dev[i] = platform_device_register_simple("conversion-user", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->dev[i]));
		dev[i] = &f->dev[i]->dev;
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 100, 1040000), 0);
	}
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start(dev[0], f->clk), 0);
	f->started = true;
	for (i = 0; i < 2; i++) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[i], .step_uv = i ? 25000 : 10000,
		};

		e[i].fixture = f;
		e[i].id = i;
		f->rail[i] = platform_device_register_simple("conversion-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[i]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[i]->dev, i, &io), 0);
		mt6797_preflight_test_flush();
		mt6797_provider_status(&status);
		if (!i)
			KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
				&status.preflight), -EPROBE_DEFER);
	}
	mt6797_provider_status(&status);
	status.preflight.generation--;
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), -ESTALE);
	KUNIT_EXPECT_EQ(test, f->writes, 0);
	mt6797_provider_status(&status);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), 0);
	KUNIT_EXPECT_GT(test, f->writes, 0);
	KUNIT_EXPECT_EQ(test, f->uv[1], 1200000);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_FALSE(test, status.engine.ready);
	KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, status.engine.floor[i], 1040000);
	/* Each real user request selects the actual 10 mV grid. Return must
	 * install the reserved live/intermediate requirement before ownership
	 * disappears. Repeating return proves the ownership state changed.
	 */
	for (i = 0; i < 3; i++) {
		KUNIT_ASSERT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
			&status.preflight, i, 1111000, false), 0);
		KUNIT_EXPECT_EQ(test, f->uv[0], 1120000);
		mt6797_provider_status(&status);
		KUNIT_EXPECT_EQ(test, status.adopted, BIT(i));
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
			&status.preflight, i, 1030000, false), -EINVAL);
		KUNIT_ASSERT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
			&status.preflight, i, 0, true), 0);
		KUNIT_EXPECT_EQ(test, f->uv[0], 1040000);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
			&status.preflight, i, 0, true), -ENOENT);
	}
	writes = f->writes;
	KUNIT_ASSERT_EQ(test, dev_pm_opp_disable(dev[1], 100), 0);
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), -ERANGE);
	KUNIT_EXPECT_EQ(test, f->writes, writes);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_enable(dev[1], 100), 0);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
		&status.preflight, 2, 1111000, false), 0);
	f->fail_write = f->writes + 1;
	f->commit_error = true;
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
		&status.preflight, 2, 0, true), -EIO);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.adopted, BIT(2));
	KUNIT_EXPECT_EQ(test, status.engine.floor[2], 1111000);
	KUNIT_EXPECT_EQ(test, f->uv[0], 1040000);
	KUNIT_EXPECT_TRUE(test, status.engine.fault);
	writes = f->writes;
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_handover(dev, f->clk,
		&status.preflight, 2, 0, true), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->writes, writes);
	platform_device_unregister(f->rail[1]);
	f->rail[1] = NULL;
	f->uv[1] = 1100000;
	f->rail[1] = platform_device_register_simple("conversion-rail", 1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[1]));
	{
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[1], .step_uv = 25000,
		};

		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[1]->dev,
			MT6797_SRAM, &io), 0);
	}
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), -ESTALE);
	mt6797_preflight_test_flush();
	mt6797_provider_status(&status);
	f->fail_write = f->writes + 1;
	KUNIT_EXPECT_EQ(test, mt6797_preflight_test_convert(dev, f->clk,
		&status.preflight), -EIO);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_TRUE(test, status.engine.fault);
	KUNIT_EXPECT_FALSE(test, status.converted);
	KUNIT_EXPECT_EQ(test, status.engine.floor[2], 1111000);
	KUNIT_EXPECT_EQ(test, status.readiness, -EAGAIN);
}

static struct kunit_case provider_cases[] = {
	KUNIT_CASE(preflight_init_unwind),
	KUNIT_CASE(preflight_opp_core),
	KUNIT_CASE(preflight_lifecycle),
	KUNIT_CASE(preflight_setup_cleanup),
	KUNIT_CASE(preflight_blocked_cleanup),
	KUNIT_CASE(provider_orders),
	KUNIT_CASE(provider_invalid),
	KUNIT_CASE(provider_inflight),
	KUNIT_CASE(production_ownership),
	KUNIT_CASE(ownership_provider_generation),
	KUNIT_CASE(ownership_first_vote_failure),
	KUNIT_CASE(ownership_notifier_generation),
	KUNIT_CASE(production_conversion),
	{}
};
static struct kunit_suite provider_suite = {
	.name = "mt6797-provider",
	.test_cases = provider_cases,
};
kunit_test_suite(provider_suite);
MODULE_LICENSE("GPL");
