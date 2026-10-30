// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <kunit/resource.h>
#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/clk/mediatek-dvfs.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include "mt6797-combined-test.h"
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
	bool hold_use;
	struct completion entered, resume;
	u64 generation;
	int result;
	struct completion done;
};

static int sample_thread(void *data)
{
	struct sample_task *task = data;
	int uv[2];

	if (task->hold_use)
		task->result = mt6797_preflight_test_hold_provider(task->generation,
				&task->entered, &task->resume);
	else
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

static void provider_inflight_case(struct kunit *test, bool hold_use)
{
	struct provider_fixture f[2] = { { .uv = 1100000 }, { .uv = 1150000 } };
	struct mt6797_rail_io io = { .read = fixture_read };
	struct platform_device *dev[2] = {};
	struct mt6797_provider_status status;
	struct sample_task sample = { .hold_use = hold_use };
	struct completion cancelled;
	struct detach_task detach;
	struct task_struct *reader = NULL, *remover = NULL;
	int i, ret;

	init_completion(&sample.done);
	init_completion(&sample.entered);
	init_completion(&sample.resume);
	init_completion(&cancelled);
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
	f[0].pause = !hold_use;
	if (hold_use)
		mt6797_preflight_test_drain_observer(&cancelled);
	reader = kthread_run(sample_thread, &sample, "rail-read");
	KUNIT_EXPECT_FALSE(test, IS_ERR(reader));
	if (IS_ERR(reader))
		goto out;
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(hold_use ?
				&sample.entered : &f[0].entered, HZ), 0UL);
	detach.dev = dev[0];
	remover = kthread_run(detach_thread, &detach, "rail-detach");
	KUNIT_EXPECT_FALSE(test, IS_ERR(remover));
	if (IS_ERR(remover))
		goto out;
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.started, HZ), 0UL);
	if (hold_use)
		KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&cancelled, HZ), 0UL);
	KUNIT_EXPECT_FALSE(test, completion_done(&detach.done));
	complete_all(&sample.resume);
	complete(&f[0].resume);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&sample.done, HZ), 0UL);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&detach.done, HZ), 0UL);
	KUNIT_EXPECT_EQ(test, sample.result, 0);
	mt6797_provider_status(&status);
	KUNIT_EXPECT_EQ(test, status.attached, BIT(MT6797_SRAM));
	KUNIT_EXPECT_GT(test, status.engine.generation, sample.generation);
out:
	complete_all(&sample.resume);
	complete_all(&f[0].resume);
	if (!IS_ERR_OR_NULL(reader))
		wait_for_completion(&sample.done);
	if (!IS_ERR_OR_NULL(remover)) {
		wait_for_completion(&detach.done);
		dev[0] = NULL;
	}
	mt6797_preflight_test_drain_observer(NULL);
	for (i = 0; i < 2; i++)
		if (!IS_ERR_OR_NULL(dev[i]))
			platform_device_unregister(dev[i]);
	mt6797_preflight_test_flush();
}

static void provider_inflight(struct kunit *test)
{
	provider_inflight_case(test, false);
}

static void provider_transaction_drain(struct kunit *test)
{
	provider_inflight_case(test, true);
}

struct lifecycle_clock {
	struct clk_hw hw;
	unsigned long rate;
	bool fail, abort, pause;
	bool round_up, round_down, unparked;
	u8 *mux_parent;
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
	struct lifecycle_clock *c = container_of(hw, struct lifecycle_clock, hw);

	/* The external-handle rejection probe uses a distinct 400 kHz request. */
	if (c->mux_parent && req->rate != 400000 && *c->mux_parent != 1)
		c->unparked = true;
	if (c->round_up)
		req->rate *= 2;
	if (c->round_down && req->rate == 200000)
		req->rate = 150000;
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

struct conversion_endpoint;

struct conversion_fixture {
	struct kunit *test;
	struct conversion_endpoint *replacement;
	struct platform_device *dev[3], *rail[2];
	struct clk *clk, *pll;
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

	bool fail = ++f->writes == f->fail_write;

	if (!fail || f->commit_error)
		f->uv[e->id] = uv;
	for (int i = 0; i < 3; i++)
		KUNIT_EXPECT_GE(f->test, f->uv[0], f->live_floor[i]);
	KUNIT_EXPECT_GE(f->test, f->uv[0], 1040000);
	KUNIT_EXPECT_LE(f->test, f->uv[0], f->uv[1]);
	KUNIT_EXPECT_LE(f->test, f->uv[1] - f->uv[0], 160000);
	return fail ? -EIO : 0;
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
	if (!IS_ERR_OR_NULL(f->pll))
		clk_unregister_fixed_rate(f->pll);
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

#include "mt6797-direct-test.h"

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
	e = kunit_kcalloc(test, 3, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	f->replacement = &e[2];
	f->test = test;
	f->uv[0] = 1120000;
	f->uv[1] = 1150000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	f->pll = clk_register_fixed_rate(NULL, "conversion-pll", NULL, 0, 100);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->pll));
	f->clk = clk_register_fixed_rate(NULL, "conversion-clock", "conversion-pll", 0, 100);
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
	for (i = 0; i < 3; i++)
		direct_devices[i] = dev[i];
	direct_sharing_error = 0;
	for (i = 0; i < 2; i++) {
		int id = failure == 4 ? 1 - i : i;
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[i], .step_uv = id ? 25000 : 10000,
		};

		e[i].fixture = f;
		e[i].id = id;
		f->rail[i] = platform_device_register_simple("conversion-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[i]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[i]->dev, id, &io), 0);
		mt6797_preflight_test_flush();
		mt6797_provider_status(&status);
		if (!i && failure >= 4) {
			struct direct_cpu pending = {};

			KUNIT_EXPECT_EQ(test, direct_cpu_init(&pending, 0), -EPROBE_DEFER);
			KUNIT_EXPECT_EQ(test, pending.rail_cookie, 0ULL);
			KUNIT_EXPECT_EQ(test, direct_cci_probe(f->dev[2]), -EPROBE_DEFER);
			KUNIT_EXPECT_EQ(test, f->writes, 0);
		}
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
	if (failure >= 4) {
		direct_callbacks(test, f, dev, failure);
		if (failure == 9) {
			/* Queue sampling at the replacement cleanup boundary. The action
			 * must drain it and detach before KUnit frees endpoint storage.
			 */
			mt6797_preflight_test_event();
			kunit_release_action(test, conversion_cleanup, f);
			mt6797_provider_status(&status);
			KUNIT_EXPECT_EQ(test, status.attached, 0UL);
		}
		return;
	}
	for (i = 0; i < 3; i++) {
		refs[i] = kref_read(&dev[i]->kobj.kref);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[i], i, &cookie[i]), 0);
		KUNIT_EXPECT_GE(test, f->uv[0], f->live_floor[i]);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(i, &guard, &vote), 2);
		KUNIT_EXPECT_EQ(test, vote, f->live_floor[i]);
		KUNIT_EXPECT_EQ(test, kref_read(&dev[i]->kobj.kref), refs[i] + 1);
	}
	/* Exercise the exported token against the owned regulator and all three
	 * real OPP tables. The fixed clock covers no-change completion; changing
	 * rate callback integration has a separate mutable-clock fixture.
	 */
	if (!failure) {
		struct mt6797_consumer_txn *txn, *nested;
		struct mt6797_txn_outcome outcome;
		int writes = f->writes;

		for (int stage = 1; stage <= 11; stage++) {
			mt6797_txn_test_fail_acquisition(stage);
			KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_begin(0, cookie[0],
								100, &txn), -EIO);
			KUNIT_EXPECT_PTR_EQ(test, txn, NULL);
			mt6797_txn_test_fail_acquisition(0);
			KUNIT_EXPECT_EQ(test, dev_pm_opp_disable(dev[0], 100), 0);
			KUNIT_EXPECT_EQ(test, dev_pm_opp_enable(dev[0], 100), 0);
			mt6797_preflight_test_flush();
			KUNIT_EXPECT_EQ(test, kref_read(&dev[0]->kobj.kref), refs[0] + 1);
		}
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_begin(3, cookie[2], 100, &txn),
				-EINVAL);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_begin(0, 0xdead, 100, &txn),
				-ESTALE);
		/* Failure after CCF acquisition and one snapshot: exact unwind is
		 * checked by the successful retry and subsequent OPP mutation.
		 */
		KUNIT_EXPECT_LT(test, mt6797_consumer_txn_begin(1, cookie[1], 999, &txn), 0);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_txn_begin(0, cookie[0], 100, &txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_begin(1, cookie[1], 100, &nested),
				-EBUSY);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(0, cookie[0]), -EBUSY);
		KUNIT_EXPECT_EQ(test, dev_pm_opp_disable(dev[0], 100), -EBUSY);
		KUNIT_EXPECT_EQ(test, dev_pm_opp_get_opp_count(dev[0]), 1);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_switch(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_finish(txn, &outcome), 0);
		KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_TARGET);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, f->live_floor[0]);
		KUNIT_EXPECT_EQ(test, f->writes, writes + 2);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_txn_begin(1, cookie[1], 100, &txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
		mt6797_preflight_test_event();
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_validate(txn), -ESTALE);
		mt6797_consumer_txn_abort(txn, -ESTALE, &outcome);
		KUNIT_EXPECT_FALSE(test, outcome.retry);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, f->live_floor[1]);
		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(1, &guard, &vote), 2);
		KUNIT_EXPECT_EQ(test, vote, f->live_floor[1]);
		mt6797_preflight_test_flush();
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
		DECLARE_COMPLETION_ONSTACK(start);
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

static void direct_cpu_cci_lifetime(struct kunit *test)
{
	ownership_run(test, 4);
}

static void direct_cci_probe_error(struct kunit *test)
{
	ownership_run(test, 5);
}

static void direct_cci_remove_retained(struct kunit *test)
{
	ownership_run(test, 6);
}

static void direct_cpu_first_vote_error(struct kunit *test)
{
	ownership_run(test, 7);
}

static void direct_cpu_notifier_stale(struct kunit *test)
{
	ownership_run(test, 8);
}

static void direct_cci_provider_rebind(struct kunit *test)
{
	ownership_run(test, 9);
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
	e = kunit_kcalloc(test, 3, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	f->test = test;
	f->uv[0] = 1040000;
	f->uv[1] = 1100000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	f->pll = clk_register_fixed_rate(NULL, "conversion-pll", NULL, 0, 100);
	KUNIT_ASSERT_FALSE(test, IS_ERR(f->pll));
	f->clk = clk_register_fixed_rate(NULL, "conversion-clock", "conversion-pll", 0, 100);
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

struct transition_mux {
	struct clk_hw hw;
	u8 parent;
	unsigned int writes, fail_at;
	int *live_floor;
	unsigned long *pll_rate;
	bool commit_error;
};

static u8 transition_parent(struct clk_hw *hw)
{
	return container_of(hw, struct transition_mux, hw)->parent;
}

static int transition_reparent(struct clk_hw *hw, u8 index)
{
	struct transition_mux *m = container_of(hw, struct transition_mux, hw);

	bool fail = ++m->writes == m->fail_at;

	if (fail && !m->commit_error)
		return -EIO;
	m->parent = index;
	if (m->live_floor)
		*m->live_floor = index ? 1080000 :
			(*m->pll_rate == 200000 ? 1100000 : 1040000);
	return fail ? -EIO : 0;
}

static const struct clk_ops transition_mux_ops = {
	.determine_rate = __clk_mux_determine_rate,
	.get_parent = transition_parent,
	.set_parent = transition_reparent,
};

static void transition_unregister(void *data)
{
	clk_hw_unregister(data);
}

static int transition_veto(struct notifier_block *nb, unsigned long event,
			   void *data)
{
	return event == PRE_RATE_CHANGE ? NOTIFY_BAD : NOTIFY_OK;
}

struct reserved_clock_listener {
	struct notifier_block nb;
	struct device *dev;
	struct kunit *test;
	unsigned int events;
};

static int reserved_clock_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct reserved_clock_listener *l = container_of(nb, struct reserved_clock_listener, nb);
	struct dev_pm_opp *opp;
	unsigned long rate = 100;

	l->events++;
	opp = dev_pm_opp_find_freq_ceil(l->dev, &rate);
	KUNIT_EXPECT_FALSE(l->test, IS_ERR(opp));
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	KUNIT_EXPECT_EQ(l->test, dev_pm_opp_adjust_voltage(l->dev, rate,
							 900000, 900000, 900000), -EBUSY);
	KUNIT_EXPECT_EQ(l->test, dev_pm_opp_disable(l->dev, rate), -EBUSY);
	return NOTIFY_OK;
}

static void reserved_clock_contract(struct kunit *test, struct clk_hw *mux,
		struct clk_hw *pll, struct clk_hw *inter)
{
	struct platform_device *pdev;
	struct opp_table *table;
	struct dev_pm_opp_requirements *snapshot = NULL;
	struct clk *owned[3] = {};
	struct clk_hw *hw[] = { mux, pll, inter };
	struct reserved_clock_listener listener = {
		.nb.notifier_call = reserved_clock_event, .test = test,
	};
	unsigned long rate = 100;
	int i, ret, claims = 0;
	bool attempted;

	pdev = platform_device_register_simple("reserved-clock-test", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	ret = dev_pm_opp_add(&pdev->dev, 100, 800000);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto device;
	table = dev_pm_opp_get_opp_table(&pdev->dev);
	if (IS_ERR(table)) {
		KUNIT_FAIL(test, "missing OPP table");
		goto opps;
	}
	ret = dev_pm_opp_requirements_get(table, &rate, 1, &snapshot);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto table;
	for (i = 0; i < ARRAY_SIZE(owned); i++) {
		owned[i] = clk_hw_get_clk(hw[i], "reserved-clock-owner");
		KUNIT_EXPECT_FALSE(test, IS_ERR(owned[i]));
		if (IS_ERR(owned[i]))
			goto clocks;
		ret = clk_rate_exclusive_get(owned[i]);
		KUNIT_EXPECT_EQ(test, ret, 0);
		if (ret)
			goto clocks;
		claims++;
	}
	listener.dev = &pdev->dev;
	ret = clk_notifier_register(owned[0], &listener.nb);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto clocks;
	KUNIT_EXPECT_EQ(test, clk_set_rate(pll->clk, 200), -EBUSY);
	KUNIT_EXPECT_EQ(test, clk_set_parent(mux->clk, inter->clk), -EBUSY);
	/* Mux ancestry still protects the PLL before parking. */
	KUNIT_EXPECT_EQ(test, clk_round_rate(owned[1], 200), 100L);
	KUNIT_EXPECT_EQ(test, clk_set_parent(owned[0], owned[2]), 0);
	KUNIT_EXPECT_TRUE(test, clk_is_match(clk_get_parent(owned[0]), owned[2]));
	KUNIT_EXPECT_EQ(test, clk_round_rate(owned[1], 200), 200L);
	KUNIT_EXPECT_EQ(test, clk_set_rate(owned[1], 200), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(owned[1]), 200UL);
	KUNIT_EXPECT_EQ(test, clk_set_parent(owned[0], owned[1]), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(owned[0]), 200UL);
	KUNIT_EXPECT_GT(test, listener.events, 0U);
	ret = clk_rate_exclusive_get(mux->clk);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test, clk_set_parent_tracked(owned[0], owned[2],
							  &attempted), -EBUSY);
		KUNIT_EXPECT_FALSE(test, attempted);
		clk_rate_exclusive_put(mux->clk);
	}
	KUNIT_EXPECT_EQ(test, clk_set_parent(owned[0], owned[2]), 0);
	KUNIT_EXPECT_EQ(test, clk_set_rate(owned[1], 100), 0);
	KUNIT_EXPECT_EQ(test, clk_set_parent(owned[0], owned[1]), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(owned[0]), 100UL);
	clk_notifier_unregister(owned[0], &listener.nb);
clocks:
	while (claims)
		clk_rate_exclusive_put(owned[--claims]);
	for (i = 0; i < ARRAY_SIZE(owned); i++)
		if (!IS_ERR_OR_NULL(owned[i]))
			clk_put(owned[i]);
	dev_pm_opp_requirements_put(snapshot);
table:
	dev_pm_opp_put_opp_table(table);
opps:
	dev_pm_opp_remove_all_dynamic(&pdev->dev);
device:
	platform_device_unregister(pdev);
}

static int token_release_thread(void *arg)
{
	struct ownership_thread *t = arg;

	complete(t->start);
	t->ret = mt6797_consumer_release(t->user, t->cookie);
	complete(&t->done);
	return 0;
}

static void operational_clock_token(struct kunit *test, struct clk *mux,
		struct clk *pll, struct clk *inter, unsigned int user)
{
	struct conversion_fixture *f;
	struct conversion_endpoint *e;
	struct mt6797_provider_status status;
	struct mt6797_consumer_txn *txn;
	struct mt6797_txn_outcome outcome;
	struct reserved_clock_listener listener = {
		.nb.notifier_call = reserved_clock_event, .test = test,
	};
	struct device *dev[3];
	struct clk *clocks[3][2] = { { inter, inter }, { inter, inter }, { inter, inter } };
	struct clk *external;
	struct task_struct *remover, *releaser;
	struct detach_task detach = {};
	struct ownership_thread release = {};
	struct completion cancelled, started;
	struct notifier_block veto = { .notifier_call = transition_veto };
	u64 cookie;
	int i, ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	e = kunit_kcalloc(test, 2, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	clocks[user][0] = mux;
	f->test = test;
	f->uv[0] = 1120000;
	f->uv[1] = 1200000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	KUNIT_ASSERT_EQ(test, clk_set_parent(mux, inter), 0);
	KUNIT_ASSERT_EQ(test, clk_set_rate(pll, 100), 0);
	KUNIT_ASSERT_EQ(test, clk_set_parent(mux, pll), 0);
	for (i = 0; i < 3; i++) {
		f->live_floor[i] = 1040000;
		f->dev[i] = platform_device_register_simple("token-user", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->dev[i]));
		dev[i] = &f->dev[i]->dev;
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 100, 1040000), 0);
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 200, 1100000), 0);
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 300, 1080000), 0);
	}
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start_clocks(dev, clocks), 0);
	f->started = true;
	for (i = 0; i < 2; i++) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[i], .step_uv = i ? 25000 : 10000,
		};

		e[i].fixture = f;
		e[i].id = i;
		f->rail[i] = platform_device_register_simple("token-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[i]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[i]->dev, i, &io), 0);
	}
	mt6797_preflight_test_flush();
	mt6797_provider_status(&status);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_convert(dev, inter, &status.preflight), 0);
	mt6797_preflight_test_flush();
	KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[user], user, &cookie), 0);
	listener.dev = dev[user];
	KUNIT_ASSERT_EQ(test, clk_notifier_register(mux, &listener.nb), 0);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
	if (user == 2)
		mt6797_test_cci_targets(test, dev[user], cookie, mux);
#endif
	external = clk_hw_get_clk(__clk_get_hw(pll), NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(external));
	KUNIT_ASSERT_EQ(test, clk_notifier_register(mux, &veto), 0);
	ret = mt6797_consumer_txn_begin(user, cookie, 200, &txn);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_switch(txn), -EBUSY);
		mt6797_consumer_txn_abort(txn, -EBUSY, &outcome);
		KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_OLD);
		KUNIT_EXPECT_TRUE(test, outcome.retry);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1100000);
	}
	clk_notifier_unregister(mux, &veto);
	mt6797_preflight_test_flush();
	ret = mt6797_consumer_txn_begin(user, cookie, 200, &txn);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test, clk_set_rate(external, 200), -EBUSY);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_switch(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_validate(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_finish(txn, &outcome), 0);
		KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_TARGET);
		KUNIT_EXPECT_EQ(test, outcome.rate, 200UL);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1100000);
		KUNIT_EXPECT_GT(test, listener.events, 0U);
	}
	/* A second operation uses the same owned vote and ends at a lower OPP. */
	mt6797_preflight_test_flush();
	ret = mt6797_consumer_txn_begin(user, cookie, 100, &txn);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_switch(txn), 0);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_finish(txn, &outcome), 0);
		KUNIT_EXPECT_EQ(test, outcome.rate, 100UL);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1080000);
	}
	clk_put(external);
	clk_notifier_unregister(mux, &listener.nb);
	mt6797_preflight_test_flush();
	KUNIT_ASSERT_EQ(test, mt6797_consumer_txn_begin(user, cookie, 200, &txn), 0);
	KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_prepare(txn), 0);
	init_completion(&started);
	init_completion(&release.done);
	release.start = &started;
	release.user = user;
	release.cookie = cookie;
	releaser = kthread_run(token_release_thread, &release, "token-release");
	if (IS_ERR(releaser)) {
		mt6797_consumer_txn_abort(txn, PTR_ERR(releaser), &outcome);
		KUNIT_FAIL(test, "release thread creation failed");
		return;
	}
	wait_for_completion(&started);
	KUNIT_EXPECT_FALSE(test, completion_done(&release.done));
	init_completion(&cancelled);
	init_completion(&detach.started);
	init_completion(&detach.done);
	detach.dev = f->rail[1];
	mt6797_preflight_test_drain_observer(&cancelled);
	remover = kthread_run(detach_thread, &detach, "token-detach");
	if (!IS_ERR(remover)) {
		wait_for_completion(&cancelled);
		KUNIT_EXPECT_FALSE(test, completion_done(&detach.done));
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_validate(txn), -ESTALE);
	}
	mt6797_consumer_txn_abort(txn, -ESTALE, &outcome);
	KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1100000);
	if (!IS_ERR(remover)) {
		wait_for_completion(&detach.done);
		kthread_stop(remover);
		f->rail[1] = NULL;
	}
	wait_for_completion(&release.done);
	kthread_stop(releaser);
	mt6797_preflight_test_drain_observer(NULL);
	kunit_release_action(test, conversion_cleanup, f);
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
struct accounting_waiter {
	struct cpufreq_policy *policy;
	struct completion started, done;
};

static int accounting_wait(void *data)
{
	struct accounting_waiter *w = data;
	DEFINE_WAIT(wait);

	prepare_to_wait(&w->policy->transition_wait, &wait, TASK_UNINTERRUPTIBLE);
	complete(&w->started);
	while (READ_ONCE(w->policy->transition_ongoing)) {
		schedule();
		prepare_to_wait(&w->policy->transition_wait, &wait, TASK_UNINTERRUPTIBLE);
	}
	finish_wait(&w->policy->transition_wait, &wait);
	complete(&w->done);
	return 0;
}

struct callback_release {
	void *info;
	struct completion started, done;
};

static int callback_release_thread(void *data)
{
	struct callback_release *r = data;

	complete(&r->started);
	down_write(&mt6797_test_cpu_policy(r->info)->rwsem);
	mt6797_test_cpu_release(r->info);
	up_write(&mt6797_test_cpu_policy(r->info)->rwsem);
	complete_all(&r->done);
	return 0;
}

struct accounting_observer {
	struct notifier_block nb;
	unsigned int pre, post, rate, terminal;
	int error;
	bool resource;
	struct callback_release *release;
	struct accounting_waiter waiter;
	struct task_struct *task;
	struct kunit *test;
	void *info;
};

static int accounting_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct accounting_observer *o = container_of(nb, struct accounting_observer, nb);
	struct cpufreq_freqs *freqs = data;

	if (event == CPUFREQ_PRECHANGE) {
		o->pre++;
		KUNIT_EXPECT_FALSE(o->test, o->resource);
		o->resource = true;
		if (!o->task) {
			o->waiter.policy = freqs->policy;
			init_completion(&o->waiter.started);
			init_completion(&o->waiter.done);
			o->task = kthread_run(accounting_wait, &o->waiter, "cpufreq-waiter");
			KUNIT_EXPECT_FALSE(o->test, IS_ERR(o->task));
			if (!IS_ERR(o->task))
				wait_for_completion(&o->waiter.started);
		}
		KUNIT_EXPECT_EQ(o->test, mt6797_test_cpu_reenter(o->info), -EBUSY);
	}
	if (event == CPUFREQ_TERMINAL) {
		struct cpufreq_terminal *terminal = data;

		o->terminal++;
		o->error = terminal->error;
		KUNIT_EXPECT_TRUE(o->test, o->resource);
		o->resource = false;
		KUNIT_EXPECT_TRUE(o->test, terminal->protection_needed);
		KUNIT_EXPECT_LT(o->test, terminal->error, 0);
		KUNIT_EXPECT_TRUE(o->test, terminal->policy->cur_unknown);
		KUNIT_EXPECT_TRUE(o->test, mt6797_test_cpu_unlocked(o->info));
		KUNIT_EXPECT_EQ(o->test, mt6797_test_cpu_reenter(o->info), -EIO);
		/* Completion must reach lower-priority listeners regardless. */
		return NOTIFY_BAD;
	}
	if (event == CPUFREQ_POSTCHANGE) {
		if (o->release)
			KUNIT_EXPECT_FALSE(o->test, completion_done(&o->release->done));
		KUNIT_EXPECT_TRUE(o->test, o->resource);
		o->resource = false;
		o->post++;
		o->rate = freqs->new;
		KUNIT_EXPECT_TRUE(o->test, mt6797_test_cpu_unlocked(o->info));
		KUNIT_EXPECT_EQ(o->test, mt6797_test_cpu_reenter(o->info), -EBUSY);
	}
	return NOTIFY_OK;
}

struct callback_veto {
	struct notifier_block nb;
	unsigned int nth, calls;
};

static int callback_veto_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct callback_veto *v = container_of(nb, struct callback_veto, nb);

	if (event == PRE_RATE_CHANGE && ++v->calls == v->nth)
		return NOTIFY_BAD;
	return NOTIFY_OK;
}

static int callback_vote_veto(struct notifier_block *nb, unsigned long event, void *data)
{
	return event == REGULATOR_EVENT_PRE_VOLTAGE_CHANGE ? NOTIFY_BAD : NOTIFY_OK;
}

struct callback_opp_listener {
	struct notifier_block nb;
	struct kunit *test;
	void *info;
	unsigned int removes, writers;
};

static int callback_opp_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct callback_opp_listener *l = container_of(nb, struct callback_opp_listener, nb);
	struct mt6797_txn_outcome outcome;

	if (event == OPP_EVENT_ADJUST_VOLTAGE) {
		l->writers++;
		KUNIT_EXPECT_EQ(l->test, mt6797_test_cpu_target(l->info, 200, &outcome), -EAGAIN);
	}
	if (event == OPP_EVENT_REMOVE) {
		l->removes++;
		KUNIT_EXPECT_FALSE(l->test, mt6797_test_cpu_unlocked(l->info));
		KUNIT_EXPECT_EQ(l->test, mt6797_test_cpu_reenter(l->info), -EBUSY);
	}
	return NOTIFY_OK;
}

struct callback_injection {
	struct conversion_fixture *f;
	struct clk *external;
	struct detach_task detach;
	struct completion cancelled;
	struct task_struct *remover;
	int failure, writes;
	struct callback_release release;
	struct task_struct *releaser;
	struct device *cpu_dev;
	struct callback_opp_listener *opp;
	struct ownership_thread peer;
	struct completion peer_started;
	struct task_struct *peer_task;
};

static void callback_inject(void *data, int phase)
{
	struct callback_injection *injection = data;
	struct conversion_fixture *f = injection->f;
	int failure = injection->failure;

	KUNIT_EXPECT_EQ(f->test, clk_set_rate(injection->external, 400000), -EBUSY);
	if (failure == 20 && phase == 0) {
		init_completion(&injection->peer_started);
		init_completion(&injection->peer.done);
		injection->peer.start = &injection->peer_started;
		injection->peer_task = kthread_run(token_release_thread,
						   &injection->peer, "peer-withdraw");
		KUNIT_ASSERT_FALSE(f->test, IS_ERR(injection->peer_task));
		wait_for_completion(&injection->peer_started);
		KUNIT_EXPECT_FALSE(f->test, completion_done(&injection->peer.done));
	}
	if (failure == 19 && phase == 0) {
		dev_pm_opp_remove(injection->cpu_dev, 200000);
		KUNIT_EXPECT_EQ(f->test, injection->opp->removes, 0U);
		KUNIT_EXPECT_EQ(f->test, dev_pm_opp_get_opp_count(injection->cpu_dev), 2);
	}
	if ((failure == 7 || failure == 8) && phase == 1) {
		f->fail_write = f->writes + 1;
		f->commit_error = failure == 8;
	}
	if ((failure == 9 && phase == 0) || (failure == 10 && phase == 1))
		mt6797_preflight_test_event();
	if ((failure == 11 && phase == 0) || (failure == 12 && phase == 1)) {
		init_completion(&injection->release.started);
		init_completion(&injection->release.done);
		injection->releaser = kthread_run(callback_release_thread,
						  &injection->release, "callback-release");
		KUNIT_ASSERT_FALSE(f->test, IS_ERR(injection->releaser));
		wait_for_completion(&injection->release.started);
		KUNIT_EXPECT_FALSE(f->test, completion_done(&injection->release.done));
		init_completion(&injection->cancelled);
		init_completion(&injection->detach.started);
		init_completion(&injection->detach.done);
		injection->detach.dev = f->rail[1];
		mt6797_preflight_test_drain_observer(&injection->cancelled);
		injection->remover = kthread_run(detach_thread, &injection->detach,
						"callback-detach");
		if (!IS_ERR(injection->remover)) {
			wait_for_completion(&injection->cancelled);
			KUNIT_EXPECT_FALSE(f->test, completion_done(&injection->detach.done));
		}
		injection->writes = f->writes;
	}
}

static void cpu_callback_case(struct kunit *test, struct clk *mux,
		struct clk *pll, struct clk *inter, unsigned int user, int failure)
{
	struct conversion_fixture *f;
	struct conversion_endpoint *e;
	struct mt6797_provider_status status;
	struct mt6797_txn_outcome outcome;
	struct device *dev[3];
	struct clk *clocks[3][2] = { { inter, inter }, { inter, inter }, { inter, inter } };
	struct callback_veto veto = { .nb.notifier_call = callback_veto_event };
	struct notifier_block vote_veto = { .notifier_call = callback_vote_veto };
	struct clk_lookup *cpu_lookup, *inter_lookup;
	struct transition_mux *m = container_of(__clk_get_hw(mux), struct transition_mux, hw);
	struct lifecycle_clock *p = container_of(__clk_get_hw(pll), struct lifecycle_clock, hw);
	struct callback_injection *injection;
	struct reserved_clock_listener listener = {
		.nb.notifier_call = reserved_clock_event, .test = test,
	};
	struct callback_opp_listener opp = {
		.nb = { .notifier_call = callback_opp_event, .priority = INT_MAX },
		.test = test,
	};
	struct accounting_observer observer = {
		.nb = { .notifier_call = accounting_event, .priority = -1 },
	};
	struct accounting_observer later = {
		.nb = { .notifier_call = accounting_event, .priority = -2 },
	};
	void *info, *passive;
	struct cpufreq_policy *policy;
	u64 old_cookie;
	int i, ret;

	injection = kunit_kzalloc(test, sizeof(*injection), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, injection);
	KUNIT_ASSERT_EQ(test, clk_set_rate(inter, 300000), 0);
	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	e = kunit_kcalloc(test, 2, sizeof(*e), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, e);
	clocks[user][0] = mux;
	f->test = test;
	f->uv[0] = 1120000;
	f->uv[1] = 1200000;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, conversion_cleanup, f), 0);
	KUNIT_ASSERT_EQ(test, clk_set_parent(mux, inter), 0);
	KUNIT_ASSERT_EQ(test, clk_set_rate(pll, 100000), 0);
	KUNIT_ASSERT_EQ(test, clk_set_parent(mux, pll), 0);
	for (i = 0; i < 3; i++) {
		int intermediate_uv = failure == 20 && i == (user ^ 1) ? 1110000 : 1080000;

		f->live_floor[i] = i == user ? 1040000 :
			failure == 20 && i == (user ^ 1) ? 1110000 : 1080000;
		f->dev[i] = platform_device_register_simple("token-user", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->dev[i]));
		dev[i] = &f->dev[i]->dev;
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 100000, 1040000), 0);
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 200000, 1100000), 0);
		KUNIT_ASSERT_EQ(test, dev_pm_opp_add(dev[i], 300000, intermediate_uv), 0);
	}
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_start_clocks(dev, clocks), 0);
	f->started = true;
	for (i = 0; i < 2; i++) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[i], .step_uv = i ? 25000 : 10000,
		};

		e[i].fixture = f;
		e[i].id = i;
		f->rail[i] = platform_device_register_simple("token-rail", i, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[i]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[i]->dev, i, &io), 0);
	}
	mt6797_preflight_test_flush();
	mt6797_provider_status(&status);
	KUNIT_ASSERT_EQ(test, mt6797_preflight_test_convert(dev, inter, &status.preflight), 0);
	mt6797_preflight_test_flush();
	cpu_lookup = clkdev_create(mux, "cpu", "%s", dev_name(dev[user]));
	inter_lookup = clkdev_create(inter, "intermediate", "%s", dev_name(dev[user]));
	KUNIT_ASSERT_NOT_NULL(test, cpu_lookup);
	KUNIT_ASSERT_NOT_NULL(test, inter_lookup);
	info = mt6797_test_cpu_open(dev[user], user);
	KUNIT_EXPECT_FALSE(test, IS_ERR(info));
	if (IS_ERR(info))
		goto cleanup;
	policy = mt6797_test_cpu_policy(info);
	old_cookie = mt6797_test_cpu_cookie(info);
	observer.test = test;
	observer.info = info;
	later.test = test;
	later.info = info;
	passive = mt6797_test_passive_open(dev[2], user * 4);
	KUNIT_ASSERT_FALSE(test, IS_ERR(passive));
	KUNIT_ASSERT_NOT_NULL(test, policy->stats);
	mt6797_test_passive_fail(passive, failure == 18);
	if (failure == 17) {
		struct clk *extra = clk_hw_get_clk(__clk_get_hw(mux), NULL);

		KUNIT_ASSERT_FALSE(test, IS_ERR(extra));
		KUNIT_EXPECT_EQ(test, clk_rate_exclusive_get(extra), 0);
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), -EBUSY);
		KUNIT_EXPECT_TRUE(test, outcome.retry);
		KUNIT_EXPECT_EQ(test, outcome.rate, 100000UL);
		clk_rate_exclusive_put(extra);
		clk_put(extra);
		mt6797_preflight_test_flush();
	}
	if (failure == 1 || failure == 14) {
		veto.nth = failure == 1 ? 1 : 2;
		clk_notifier_register(mux, &veto.nb);
	}
	if (failure == 15)
		mt6797_preflight_test_regulator_notifier(user, &vote_veto, true);
	if (!failure) {
		int writes = f->writes;

		mt6797_preflight_test_hold(true);
		mt6797_preflight_test_event();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), -EAGAIN);
		KUNIT_EXPECT_EQ(test, f->writes, writes);
		mt6797_preflight_test_hold(false);
		mt6797_preflight_test_flush();
	}
	opp.info = info;
	if (!failure || failure == 19)
		KUNIT_ASSERT_EQ(test, dev_pm_opp_register_notifier(dev[user], &opp.nb), 0);
	if (!failure) {
		int writes = f->writes;

		/* All acquisition failure stages, including each reserved table. */
		for (i = 1; i <= 11; i++) {
			mt6797_txn_test_fail_acquisition(i);
			KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), -EIO);
			mt6797_txn_test_fail_acquisition(0);
			KUNIT_EXPECT_EQ(test, f->writes, writes);
			KUNIT_EXPECT_TRUE(test, mt6797_test_cpu_unlocked(info));
		}
		KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(dev[user], 200000,
								1110000, 1110000, 1110000), 0);
		KUNIT_EXPECT_EQ(test, opp.writers, 1U);
		KUNIT_EXPECT_EQ(test, f->writes, writes);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(dev[user], 200000,
								1100000, 1100000, 1100000), 0);
		KUNIT_EXPECT_EQ(test, opp.writers, 2U);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 100, &outcome), 0);
		mt6797_preflight_test_flush();
	}
	p->mux_parent = &m->parent;
	p->unparked = false;
	p->round_up = failure == 13;
	p->round_down = failure == 16;
	if (failure == 2)
		p->fail = true;
	if (failure == 3 || failure == 6)
		m->fail_at = m->writes + 2;
	if (failure == 4 || failure == 5) {
		f->fail_write = f->writes + 1;
		f->commit_error = failure == 5;
	}
	m->commit_error = failure == 6;
	m->live_floor = &f->live_floor[user];
	m->pll_rate = &p->rate;
	if (failure == 7 || failure == 8) {
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), 0);
		mt6797_preflight_test_flush();
	}
	listener.dev = dev[user];
	clk_notifier_register(mux, &listener.nb);
	injection->external = clk_hw_get_clk(__clk_get_hw(pll), NULL);
	KUNIT_ASSERT_FALSE(test, IS_ERR(injection->external));
	injection->f = f;
	injection->failure = failure;
	if (failure == 20) {
		injection->peer.user = user ^ 1;
		KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[user ^ 1], user ^ 1,
							      &injection->peer.cookie), 0);
		mt6797_preflight_test_flush();
	}
	injection->cpu_dev = dev[user];
	injection->opp = &opp;
	injection->release.info = info;
	if (failure == 11 || failure == 12) {
		observer.release = &injection->release;
		later.release = &injection->release;
	}
	KUNIT_ASSERT_EQ(test, cpufreq_register_notifier(&observer.nb,
				CPUFREQ_TERMINAL_NOTIFIER), 0);
	KUNIT_ASSERT_EQ(test, cpufreq_register_notifier(&later.nb,
							CPUFREQ_TERMINAL_NOTIFIER), 0);
	mt6797_test_cpu_hook(callback_inject, injection);
	mt6797_test_switch_hook(callback_inject, injection);
	/* Model the framework lifetime exclusion through the complete POST chain. */
	down_write(&policy->rwsem);
	ret = mt6797_test_cpu_target(info, failure == 7 || failure == 8 ? 100 : 200,
				     &outcome);
	up_write(&policy->rwsem);
	mt6797_test_cpu_hook(NULL, NULL);
	mt6797_test_switch_hook(NULL, NULL);
	if (!IS_ERR_OR_NULL(observer.task)) {
		KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&observer.waiter.done, HZ), 0UL);
		kthread_stop(observer.task);
	}
	if (!IS_ERR_OR_NULL(later.task)) {
		KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&later.waiter.done, HZ), 0UL);
		kthread_stop(later.task);
	}
	KUNIT_EXPECT_EQ(test, ret, outcome.error);
	KUNIT_EXPECT_EQ(test, observer.pre, 1U);
	KUNIT_EXPECT_EQ(test, later.pre, observer.pre);
	KUNIT_EXPECT_EQ(test, later.post, observer.post);
	KUNIT_EXPECT_EQ(test, later.rate, observer.rate);
	KUNIT_EXPECT_EQ(test, later.terminal, observer.terminal);
	KUNIT_EXPECT_EQ(test, observer.terminal, failure == 3 || failure == 6 ? 1U : 0U);
	KUNIT_EXPECT_FALSE(test, observer.resource);
	KUNIT_EXPECT_FALSE(test, later.resource);
	KUNIT_EXPECT_EQ(test, observer.post, failure == 3 || failure == 6 ? 0U : 1U);
	if (observer.post)
		KUNIT_EXPECT_EQ(test, observer.rate, (unsigned int)(outcome.rate / 1000));
	if (failure == 11 || failure == 12) {
		wait_for_completion(&injection->release.done);
		kthread_stop(injection->releaser);
		if (!IS_ERR(injection->remover)) {
			wait_for_completion(&injection->detach.done);
			kthread_stop(injection->remover);
			f->rail[1] = NULL;
		}
		KUNIT_EXPECT_EQ(test, f->writes, injection->writes);
		mt6797_preflight_test_drain_observer(NULL);
	}
	if (failure == 4 || failure == 5 || failure == 7 || failure == 8) {
		int guard, vote;

		KUNIT_EXPECT_EQ(test, mt6797_preflight_test_requests(user, &guard, &vote), 2);
		KUNIT_EXPECT_EQ(test, vote, outcome.retained_uv);
	}
	KUNIT_EXPECT_FALSE(test, policy->transition_ongoing);
	KUNIT_EXPECT_PTR_EQ(test, policy->transition_task, NULL);
	if (failure == 3 || failure == 6) {
		KUNIT_EXPECT_TRUE(test, policy->cur_unknown);
		KUNIT_EXPECT_EQ(test, policy->cur, 100U);
		KUNIT_EXPECT_EQ(test, observer.error, ret);
		KUNIT_EXPECT_EQ(test, later.error, ret);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_error(passive), ret);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_calls(passive), 0U);
		KUNIT_EXPECT_EQ(test, mt6797_test_stats_rate(policy), 0U);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_rate(passive), 0UL);
	} else {
		KUNIT_EXPECT_FALSE(test, policy->cur_unknown);
		KUNIT_EXPECT_EQ(test, policy->cur, (unsigned int)(outcome.rate / 1000));
		KUNIT_EXPECT_EQ(test, mt6797_test_stats_rate(policy),
				failure == 16 ? 0U : policy->cur);
		if (policy->cur != 100 || failure == 7 || failure == 8)
			KUNIT_EXPECT_EQ(test, mt6797_test_passive_rate(passive), outcome.rate);
	}
	if (failure == 18) {
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_calls(passive), 1U);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_error(passive), -EIO);
		mt6797_test_passive_fail(passive, false);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), 0);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_calls(passive), 2U);
		KUNIT_EXPECT_EQ(test, mt6797_test_passive_error(passive), 0);
		KUNIT_EXPECT_EQ(test, observer.pre, 2U);
		KUNIT_EXPECT_EQ(test, observer.post, 2U);
		KUNIT_EXPECT_EQ(test, later.pre, 2U);
		KUNIT_EXPECT_EQ(test, later.post, 2U);
		KUNIT_EXPECT_EQ(test, later.rate, 200U);
		KUNIT_EXPECT_EQ(test, observer.rate, 200U);
		KUNIT_EXPECT_FALSE(test, policy->transition_ongoing);
	} else if (failure == 20) {
		KUNIT_EXPECT_EQ(test, ret, 0);
		wait_for_completion(&injection->peer.done);
		kthread_stop(injection->peer_task);
		KUNIT_EXPECT_EQ(test, injection->peer.ret, 0);
		KUNIT_EXPECT_GE(test, f->uv[0], 1110000);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 100, &outcome), 0);
	} else if (failure == 19) {
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, opp.removes, 1U);
		KUNIT_EXPECT_EQ(test, outcome.rate, 200000UL);
		KUNIT_EXPECT_EQ(test, dev_pm_opp_add(dev[user], 200000, 1100000), 0);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 100, &outcome), 0);
	} else if (failure == 16) {
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, outcome.rate, 150000UL);
	} else if (!failure || failure == 17) {
		KUNIT_EXPECT_EQ(test, ret, 0);
		KUNIT_EXPECT_EQ(test, outcome.rate, 200000UL);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1100000);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), 0);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 100, &outcome), 0);
		KUNIT_EXPECT_EQ(test, outcome.rate, 100000UL);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv, 1080000);
	} else {
		KUNIT_EXPECT_LT(test, ret, 0);
		KUNIT_EXPECT_EQ(test, outcome.retry, failure == 1 || failure == 15);
		if (failure == 2 || failure == 13 || failure == 14)
			KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_INTERMEDIATE);
		else if (failure == 3 || failure == 6) {
			KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_UNKNOWN);
			KUNIT_EXPECT_EQ(test, outcome.rate, 0UL);
		} else if (failure == 7 || failure == 8 || failure == 10 || failure == 12)
			KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_TARGET);
		else
			KUNIT_EXPECT_EQ(test, outcome.path, MT6797_TXN_OLD);
		KUNIT_EXPECT_EQ(test, outcome.retained_uv,
				failure == 4 || failure == 5 || failure == 15 ? 1080000 : 1100000);
		if (failure >= 3 && failure <= 8) {
			mt6797_provider_status(&status);
			KUNIT_EXPECT_TRUE(test, status.engine.fault);
		}
		if (failure == 1 || failure == 15) {
			if (failure == 1)
				clk_notifier_unregister(mux, &veto.nb);
			else
				mt6797_preflight_test_regulator_notifier(user, &vote_veto, false);
			mt6797_preflight_test_flush();
			KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 200, &outcome), 0);
		} else {
			KUNIT_EXPECT_EQ(test, mt6797_test_cpu_target(info, 100, &outcome), -EIO);
		}
	}
	if (failure == 14)
		clk_notifier_unregister(mux, &veto.nb);
	KUNIT_EXPECT_FALSE(test, p->unparked);
	clk_put(injection->external);
	clk_notifier_unregister(mux, &listener.nb);
	p->mux_parent = NULL;
	p->round_up = false;
	p->round_down = false;
	p->fail = false;
	m->fail_at = 0;
	m->live_floor = NULL;
	m->commit_error = false;
	cpufreq_unregister_notifier(&later.nb, CPUFREQ_TERMINAL_NOTIFIER);
	cpufreq_unregister_notifier(&observer.nb, CPUFREQ_TERMINAL_NOTIFIER);
	if (failure == 11 || failure == 12) {
		struct mt6797_rail_io io = {
			.read = conversion_read, .write = conversion_write,
			.data = &e[1], .step_uv = 25000,
		};
		struct mt6797_consumer_txn *stale = NULL;
		u64 cookie;

		f->rail[1] = platform_device_register_simple("token-rail", 1, NULL, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(f->rail[1]));
		KUNIT_ASSERT_EQ(test, mt6797_provider_attach(&f->rail[1]->dev, 1, &io), 0);
		mt6797_preflight_test_flush();
		KUNIT_EXPECT_EQ(test, mt6797_consumer_acquire(dev[user], user, &cookie), -EBUSY);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_recover(), 0);
		KUNIT_ASSERT_EQ(test, mt6797_consumer_acquire(dev[user], user, &cookie), 0);
		KUNIT_EXPECT_NE(test, cookie, old_cookie);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(user, old_cookie), -ESTALE);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_txn_begin(user, old_cookie, 200000,
								&stale), -ESTALE);
		KUNIT_EXPECT_PTR_EQ(test, stale, NULL);
		KUNIT_EXPECT_EQ(test, mt6797_consumer_release(user, cookie), 0);
	}
	mt6797_test_stats_contract(test, policy);
	if (!failure || failure == 19)
		dev_pm_opp_unregister_notifier(dev[user], &opp.nb);
	mt6797_test_passive_close(passive);
	mt6797_test_cpu_close(info);
cleanup:
	clkdev_drop(inter_lookup);
	clkdev_drop(cpu_lookup);
	kunit_release_action(test, conversion_cleanup, f);
}
#endif

static void transition_clock_contract(struct kunit *test)
{
	struct lifecycle_clock *pll, *inter;
	struct transition_mux *mux;
	const struct clk_hw *parents[2];
	struct clk_init_data init = { .ops = &lifecycle_ops };
	struct clk *output, *source, *temporary;
	struct notifier_block veto = { .notifier_call = transition_veto };
	bool unchanged;

	pll = kunit_kzalloc(test, sizeof(*pll), GFP_KERNEL);
	inter = kunit_kzalloc(test, sizeof(*inter), GFP_KERNEL);
	mux = kunit_kzalloc(test, sizeof(*mux), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pll);
	KUNIT_ASSERT_NOT_NULL(test, inter);
	KUNIT_ASSERT_NOT_NULL(test, mux);
	pll->rate = 100;
	inter->rate = 300;
	init.name = "transition-pll";
	pll->hw.init = &init;
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &pll->hw), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
		transition_unregister, &pll->hw), 0);
	init.name = "transition-intermediate";
	inter->hw.init = &init;
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &inter->hw), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
		transition_unregister, &inter->hw), 0);
	parents[0] = &pll->hw;
	parents[1] = &inter->hw;
	init.name = "transition-mux";
	init.ops = &transition_mux_ops;
	init.parent_hws = parents;
	init.num_parents = 2;
	mux->hw.init = &init;
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &mux->hw), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
		transition_unregister, &mux->hw), 0);
	output = mux->hw.clk;
	source = pll->hw.clk;
	temporary = inter->hw.clk;
	KUNIT_ASSERT_EQ(test, clk_notifier_register(output, &veto), 0);
	KUNIT_EXPECT_NE(test, mtk_dvfs_switch_clock_tracked(output, source,
		 temporary, 200, &unchanged), 0);
	KUNIT_EXPECT_TRUE(test, unchanged);
	KUNIT_EXPECT_EQ(test, mux->writes, 0U);
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 100UL);
	KUNIT_EXPECT_EQ(test, clk_notifier_unregister(output, &veto), 0);

	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 200), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 200UL);
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 100), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 100UL);
	mux->fail_at = mux->writes + 1;
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock_tracked(output, source,
		 temporary, 200, &unchanged), -EIO);
	/* A provider error is uncertain even when the model did not commit. */
	KUNIT_EXPECT_FALSE(test, unchanged);
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 100UL);
	mux->fail_at = 0;
	pll->fail = true;
	/* CCF can consume a provider set_rate error; acceptance must be checked. */
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 200), -EIO);
	KUNIT_EXPECT_TRUE(test, clk_is_match(clk_get_parent(output), temporary));
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 300UL);
	pll->fail = false;
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 200), 0);
	mux->fail_at = mux->writes + 2;
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 100), -EIO);
	KUNIT_EXPECT_TRUE(test, clk_is_match(clk_get_parent(output), temporary));
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 300UL);
	mux->fail_at = 0;
	KUNIT_EXPECT_EQ(test, mtk_dvfs_switch_clock(output, source, temporary, 100), 0);
	KUNIT_EXPECT_EQ(test, clk_get_rate(output), 100UL);
	reserved_clock_contract(test, &mux->hw, &pll->hw, &inter->hw);
	operational_clock_token(test, output, source, temporary, 0);
	operational_clock_token(test, output, source, temporary, 1);
	operational_clock_token(test, output, source, temporary, 2);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
	for (unsigned int user = 0; user < 2; user++)
		for (int failure = 0; failure < 21; failure++)
			cpu_callback_case(test, output, source, temporary, user, failure);
	mt6797_test_legacy_completion(test);
#endif
}

struct requirements_listener {
	struct notifier_block nb;
	struct opp_table *table;
	struct kunit *test;
	unsigned int events;
};

static int requirements_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct requirements_listener *l = container_of(nb, struct requirements_listener, nb);
	struct dev_pm_opp_requirements *snapshot = NULL;
	unsigned long rate = 100;

	if (event != OPP_EVENT_ADJUST_VOLTAGE && event != OPP_EVENT_DISABLE)
		return NOTIFY_OK;
	l->events++;
	/* Committed writer is still notifying: never wait on driver reg_lock. */
	KUNIT_EXPECT_EQ(l->test, dev_pm_opp_requirements_get(l->table, &rate, 1,
							 &snapshot), -EAGAIN);
	dev_pm_opp_requirements_put(snapshot);
	return NOTIFY_OK;
}

static void requirements_reservation(struct kunit *test)
{
	struct platform_device *pdev;
	struct opp_table *table;
	struct dev_pm_opp_requirements *snapshot = NULL, *second = NULL;
	struct dev_pm_opp_requirement value;
	struct dev_pm_opp *opp;
	unsigned long rates[] = { 100, 150, 200 }, lookup = 150;
	struct requirements_listener listener = {
		.nb.notifier_call = requirements_event, .test = test,
	};
	int ret;

	pdev = platform_device_register_simple("requirements-test", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, unregister_device, pdev), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, remove_opps, &pdev->dev), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 100, 800000), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 200, 900000), 0);
	table = dev_pm_opp_get_opp_table(&pdev->dev);
	KUNIT_ASSERT_FALSE(test, IS_ERR(table));
	listener.table = table;
	ret = dev_pm_opp_register_notifier(&pdev->dev, &listener.nb);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto put_table;
	ret = dev_pm_opp_requirements_get(table, rates, ARRAY_SIZE(rates), &snapshot);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto unregister;
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_read(snapshot, 1, &value), 0);
	KUNIT_EXPECT_EQ(test, value.rate, 200UL);
	KUNIT_EXPECT_EQ(test, value.uv, 900000UL);
	/* A lookup on the reserving task must complete, without table-lock recursion. */
	opp = dev_pm_opp_find_freq_ceil(&pdev->dev, &lookup);
	KUNIT_EXPECT_FALSE(test, IS_ERR(opp));
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(&pdev->dev, 100,
						      850000, 840000, 860000), -EBUSY);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_disable(&pdev->dev, 100), -EBUSY);
	KUNIT_EXPECT_EQ(test, listener.events, 0U);
	dev_pm_opp_requirements_put(snapshot);
	snapshot = NULL;
	KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(&pdev->dev, 100,
						      850000, 840000, 860000), 0);
	KUNIT_EXPECT_EQ(test, listener.events, 1U);
	ret = dev_pm_opp_requirements_get(table, rates, ARRAY_SIZE(rates), &snapshot);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto unregister;
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_read(snapshot, 0, &value), 0);
	KUNIT_EXPECT_EQ(test, value.uv, 850000UL);
	KUNIT_EXPECT_EQ(test, value.uv_min, 840000UL);
	KUNIT_EXPECT_EQ(test, value.uv_max, 860000UL);
	/* Logical removal cannot free accepted entries or permit new selection. */
	dev_pm_opp_remove_all_dynamic(&pdev->dev);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_read(snapshot, 0, &value), 0);
	KUNIT_EXPECT_EQ(test, value.uv, 850000UL);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_get(table, rates, 1, &second), -ERANGE);
	dev_pm_opp_requirements_put(second);
	dev_pm_opp_requirements_put(snapshot);
unregister:
	dev_pm_opp_unregister_notifier(&pdev->dev, &listener.nb);
put_table:
	dev_pm_opp_put_opp_table(table);
}

struct removal_listener {
	struct notifier_block nb;
	struct kunit *test;
	struct device *dev;
	unsigned int events;
};

static int removal_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct removal_listener *l = container_of(nb, struct removal_listener, nb);

	if (event == OPP_EVENT_REMOVE) {
		l->events++;
		/* Final puts notify without the table mutex held. */
		KUNIT_EXPECT_EQ(l->test, dev_pm_opp_get_opp_count(l->dev), 0);
	}
	return NOTIFY_OK;
}

static void requirements_single_remove(struct kunit *test)
{
	struct platform_device *pdev;
	struct opp_table *table;
	struct dev_pm_opp_requirements *a = NULL, *b = NULL;
	struct dev_pm_opp_requirement value;
	struct dev_pm_opp *opp;
	unsigned long rates[] = { 100, 200 }, bad[] = { 100, 300 };
	unsigned long rate, freq, uv;
	struct removal_listener listener = {
		.nb.notifier_call = removal_event, .test = test,
	};
	int ret;

	pdev = platform_device_register_simple("single-remove-test", -1, NULL, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(pdev));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, unregister_device, pdev), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, remove_opps, &pdev->dev), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 100, 800000), 0);
	KUNIT_ASSERT_EQ(test, dev_pm_opp_add(&pdev->dev, 200, 900000), 0);
	table = dev_pm_opp_get_opp_table(&pdev->dev);
	KUNIT_ASSERT_FALSE(test, IS_ERR(table));
	listener.dev = &pdev->dev;
	ret = dev_pm_opp_register_notifier(&pdev->dev, &listener.nb);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto put_table;
	ret = dev_pm_opp_requirements_get(table, rates, 2, &a);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto unregister;
	/* Failed multi-rate admission must not acquire temporary OPP refs. */
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_get(table, bad, 2, &b), -ERANGE);
	dev_pm_opp_requirements_put(b);
	b = NULL;
	dev_pm_opp_remove(&pdev->dev, 200);
	dev_pm_opp_remove(&pdev->dev, 200);
	KUNIT_EXPECT_EQ(test, listener.events, 0U);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_get_opp_count(&pdev->dev), 1);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_enable(&pdev->dev, 200), -ENODEV);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_adjust_voltage(&pdev->dev, 200,
						      950000, 950000, 950000), -ENODEV);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_add(&pdev->dev, 200, 900000), -EEXIST);
	opp = dev_pm_opp_find_freq_exact(&pdev->dev, 200, true);
	KUNIT_EXPECT_TRUE(test, IS_ERR(opp));
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	rate = 200;
	opp = dev_pm_opp_find_freq_ceil(&pdev->dev, &rate);
	KUNIT_EXPECT_TRUE(test, IS_ERR(opp));
	if (!IS_ERR(opp))
		dev_pm_opp_put(opp);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_get(table, rates, 2, &b), -ERANGE);
	dev_pm_opp_requirements_put(b);
	ret = dev_pm_opp_snapshot_lock(table, &rates[1], &freq, &uv, 1);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
	if (!ret)
		dev_pm_opp_snapshot_unlock(table);
	/* Remove-all must not consume the already removed entry's retained ref. */
	dev_pm_opp_remove_all_dynamic(&pdev->dev);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_get_opp_count(&pdev->dev), 0);
	KUNIT_EXPECT_EQ(test, listener.events, 0U);
	KUNIT_EXPECT_EQ(test, dev_pm_opp_requirements_read(a, 1, &value), 0);
	KUNIT_EXPECT_EQ(test, value.uv, 900000UL);
	dev_pm_opp_requirements_put(a);
	KUNIT_EXPECT_EQ(test, listener.events, 2U);
unregister:
	dev_pm_opp_unregister_notifier(&pdev->dev, &listener.nb);
put_table:
	dev_pm_opp_put_opp_table(table);
}

static struct kunit_case provider_cases[] = {
	KUNIT_CASE(requirements_single_remove),
	KUNIT_CASE(requirements_reservation),
	KUNIT_CASE(transition_clock_contract),
	KUNIT_CASE(direct_cpu_cci_lifetime),
	KUNIT_CASE(direct_cpu_first_vote_error),
	KUNIT_CASE(direct_cpu_notifier_stale),
	KUNIT_CASE(direct_cci_probe_error),
	KUNIT_CASE(direct_cci_remove_retained),
	KUNIT_CASE(direct_cci_provider_rebind),
	KUNIT_CASE(preflight_init_unwind),
	KUNIT_CASE(preflight_opp_core),
	KUNIT_CASE(preflight_lifecycle),
	KUNIT_CASE(preflight_setup_cleanup),
	KUNIT_CASE(preflight_blocked_cleanup),
	KUNIT_CASE(provider_orders),
	KUNIT_CASE(provider_invalid),
	KUNIT_CASE(provider_inflight),
	KUNIT_CASE(provider_transaction_drain),
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
