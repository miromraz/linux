// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/drivers/devfreq/governor_passive.c
 *
 * Copyright (C) 2016 Samsung Electronics
 * Author: Chanwoo Choi <cw00.choi@samsung.com>
 * Author: MyungJoo Ham <myungjoo.ham@samsung.com>
 */

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/devfreq-governor.h>
#include <linux/units.h>

/**
 * struct devfreq_cpu_data - Hold the per-cpu data
 * @node:	list node
 * @dev:	reference to cpu device.
 * @first_cpu:	the cpumask of the first cpu of a policy.
 * @opp_table:	reference to cpu opp table.
 * @cur_freq:	the current frequency of the cpu.
 * @min_freq:	the min frequency of the cpu.
 * @max_freq:	the max frequency of the cpu.
 *
 * This structure stores the required cpu_data of a cpu.
 * This is auto-populated by the governor.
 */
struct devfreq_cpu_data {
	struct list_head node;

	struct device *dev;
	unsigned int first_cpu;

	struct opp_table *opp_table;
	unsigned int cur_freq;
	bool update_pending;
	bool unknown;
	int update_error;
	unsigned int min_freq;
	unsigned int max_freq;
};

static struct devfreq_cpu_data *
get_parent_cpu_data(struct devfreq_passive_data *p_data,
		    struct cpufreq_policy *policy)
{
	struct devfreq_cpu_data *parent_cpu_data;

	if (!p_data || !policy)
		return NULL;

	list_for_each_entry(parent_cpu_data, &p_data->cpu_data_list, node)
		if (parent_cpu_data->first_cpu == cpumask_first(policy->related_cpus))
			return parent_cpu_data;

	return NULL;
}

static void delete_parent_cpu_data(struct devfreq_passive_data *p_data)
{
	struct devfreq_cpu_data *parent_cpu_data, *tmp;
	LIST_HEAD(retired);

	mutex_lock(&p_data->this->lock);
	list_splice_init(&p_data->cpu_data_list, &retired);
	mutex_unlock(&p_data->this->lock);

	list_for_each_entry_safe(parent_cpu_data, tmp, &retired, node) {
		list_del(&parent_cpu_data->node);

		if (parent_cpu_data->opp_table)
			dev_pm_opp_put_opp_table(parent_cpu_data->opp_table);

		kfree(parent_cpu_data);
	}
}

static unsigned long get_target_freq_by_required_opp(struct device *p_dev,
						struct opp_table *p_opp_table,
						struct opp_table *opp_table,
						unsigned long *freq)
{
	struct dev_pm_opp *opp = NULL, *p_opp = NULL;
	unsigned long target_freq;

	if (!p_dev || !p_opp_table || !opp_table || !freq)
		return 0;

	p_opp = devfreq_recommended_opp(p_dev, freq, 0);
	if (IS_ERR(p_opp))
		return 0;

	opp = dev_pm_opp_xlate_required_opp(p_opp_table, opp_table, p_opp);
	dev_pm_opp_put(p_opp);

	if (IS_ERR(opp))
		return 0;

	target_freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	return target_freq;
}

static int get_target_freq_with_cpufreq(struct devfreq *devfreq,
					unsigned long *target_freq)
{
	struct devfreq_passive_data *p_data =
				(struct devfreq_passive_data *)devfreq->data;
	struct devfreq_cpu_data *parent_cpu_data;
	struct cpufreq_policy *policy;
	unsigned long cpu, cpu_cur, cpu_min, cpu_max, cpu_percent;
	unsigned long dev_min, dev_max;
	unsigned long freq = 0;
	int ret = 0;

	for_each_online_cpu(cpu) {
		ret = cpufreq_policy_required(cpu);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = 0;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			return -EINVAL;

		parent_cpu_data = get_parent_cpu_data(p_data, policy);
		if (!parent_cpu_data) {
			cpufreq_cpu_put(policy);
			return -EAGAIN;
		}

		if (parent_cpu_data->unknown || READ_ONCE(policy->cur_unknown)) {
			cpufreq_cpu_put(policy);
			return -EIO;
		}

		/* Get target freq via required opps */
		cpu_cur = parent_cpu_data->cur_freq * HZ_PER_KHZ;
		freq = get_target_freq_by_required_opp(parent_cpu_data->dev,
					parent_cpu_data->opp_table,
					devfreq->opp_table, &cpu_cur);
		if (freq) {
			*target_freq = max(freq, *target_freq);
			cpufreq_cpu_put(policy);
			continue;
		}

		/* Use interpolation if required opps is not available */
		devfreq_get_freq_range(devfreq, &dev_min, &dev_max);

		cpu_min = parent_cpu_data->min_freq;
		cpu_max = parent_cpu_data->max_freq;
		cpu_cur = parent_cpu_data->cur_freq;

		cpu_cur = clamp(cpu_cur, cpu_min, cpu_max);
		cpu_percent = cpu_max == cpu_min ? 100 :
			((cpu_cur - cpu_min) * 100) / (cpu_max - cpu_min);
		freq = dev_min + mult_frac(dev_max - dev_min, cpu_percent, 100);

		*target_freq = max(freq, *target_freq);
		cpufreq_cpu_put(policy);
	}

	return ret;
}

static int get_target_freq_with_devfreq(struct devfreq *devfreq,
					unsigned long *freq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq *parent_devfreq = (struct devfreq *)p_data->parent;
	unsigned long child_freq = ULONG_MAX;
	int i, count;

	/* Get target freq via required opps */
	child_freq = get_target_freq_by_required_opp(parent_devfreq->dev.parent,
						parent_devfreq->opp_table,
						devfreq->opp_table, freq);
	if (child_freq)
		goto out;

	/* Use interpolation if required opps is not available */
	for (i = 0; i < parent_devfreq->max_state; i++)
		if (parent_devfreq->freq_table[i] == *freq)
			break;

	if (i == parent_devfreq->max_state)
		return -EINVAL;

	if (i < devfreq->max_state) {
		child_freq = devfreq->freq_table[i];
	} else {
		count = devfreq->max_state;
		child_freq = devfreq->freq_table[count - 1];
	}

out:
	*freq = child_freq;

	return 0;
}

static int devfreq_passive_get_target_freq(struct devfreq *devfreq,
					   unsigned long *freq)
{
	struct devfreq_passive_data *p_data =
				(struct devfreq_passive_data *)devfreq->data;
	struct devfreq_cpu_data *cpu;
	int ret;

	if (!p_data)
		return -EINVAL;

	if (p_data->parent_type == CPUFREQ_PARENT_DEV)
		list_for_each_entry(cpu, &p_data->cpu_data_list, node)
			if (cpu->unknown)
				return -EIO;

	/*
	 * If the devfreq device with passive governor has the specific method
	 * to determine the next frequency, should use the get_target_freq()
	 * of struct devfreq_passive_data.
	 */
	if (p_data->get_target_freq)
		return p_data->get_target_freq(devfreq, freq);

	switch (p_data->parent_type) {
	case DEVFREQ_PARENT_DEV:
		ret = get_target_freq_with_devfreq(devfreq, freq);
		break;
	case CPUFREQ_PARENT_DEV:
		ret = get_target_freq_with_cpufreq(devfreq, freq);
		break;
	default:
		ret = -EINVAL;
		dev_err(&devfreq->dev, "Invalid parent type\n");
		break;
	}

	return ret;
}

static int cpufreq_passive_notifier_call(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct devfreq_passive_data *p_data =
			container_of(nb, struct devfreq_passive_data, nb);
	struct devfreq *devfreq = (struct devfreq *)p_data->this;
	struct devfreq_cpu_data *parent_cpu_data;
	struct cpufreq_freqs *freqs = ptr;
	int ret;

	if (event != CPUFREQ_TERMINAL && event != CPUFREQ_POSTCHANGE)
		return NOTIFY_DONE;

	mutex_lock(&devfreq->lock);
	if (event == CPUFREQ_TERMINAL) {
		struct cpufreq_terminal *terminal = ptr;

		parent_cpu_data = get_parent_cpu_data(p_data, terminal->policy);
		if (parent_cpu_data) {
			parent_cpu_data->unknown = true;
			parent_cpu_data->update_pending = true;
			parent_cpu_data->update_error = terminal->error;
		}
		goto out;
	}
	if (!freqs)
		goto out;
	parent_cpu_data = get_parent_cpu_data(p_data, freqs->policy);
	if (!parent_cpu_data || (parent_cpu_data->cur_freq == freqs->new &&
				 !parent_cpu_data->update_pending))
		goto out;

	parent_cpu_data->cur_freq = freqs->new;
	parent_cpu_data->unknown = false;
	if (p_data->cpu_starting) {
		parent_cpu_data->update_pending = true;
		goto out;
	}
	if (devfreq->profile->target_outcome && devfreq->outcome.error &&
	    !devfreq->outcome.retry)
		goto out;
	ret = devfreq_update_target(devfreq, freqs->new);
	parent_cpu_data->update_pending = !!ret;
	parent_cpu_data->update_error = ret;
out:
	mutex_unlock(&devfreq->lock);
	/* Child errors must not suppress later CPU completion listeners. */
	return NOTIFY_DONE;
}

static int cpufreq_passive_unregister_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	int ret;

	if (p_data->nb.notifier_call) {
		ret = cpufreq_unregister_notifier(&p_data->nb,
					CPUFREQ_TERMINAL_NOTIFIER);
		if (ret < 0)
			return ret;
	}

	delete_parent_cpu_data(p_data);

	return 0;
}

static int cpufreq_passive_register_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct device *dev = devfreq->dev.parent;
	struct opp_table *opp_table = NULL;
	struct devfreq_cpu_data *parent_cpu_data;
	struct cpufreq_policy *policy;
	struct device *cpu_dev;
	unsigned int cpu;
	unsigned long flags;
	int ret;

	p_data->cpu_data_list
		= (struct list_head)LIST_HEAD_INIT(p_data->cpu_data_list);

	p_data->cpu_starting = true;
	p_data->nb.notifier_call = cpufreq_passive_notifier_call;
	ret = cpufreq_register_notifier(&p_data->nb, CPUFREQ_TERMINAL_NOTIFIER);
	if (ret) {
		dev_err(dev, "failed to register cpufreq notifier\n");
		p_data->nb.notifier_call = NULL;
		return ret;
	}

	for_each_possible_cpu(cpu) {
		ret = cpufreq_policy_required(cpu);
		if (ret < 0)
			goto err;
		if (!ret)
			continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			ret = -EPROBE_DEFER;
			goto err;
		}

		mutex_lock(&devfreq->lock);
		parent_cpu_data = get_parent_cpu_data(p_data, policy);
		mutex_unlock(&devfreq->lock);
		if (parent_cpu_data) {
			cpufreq_cpu_put(policy);
			continue;
		}

		parent_cpu_data = kzalloc_obj(*parent_cpu_data);
		if (!parent_cpu_data) {
			ret = -ENOMEM;
			goto err_put_policy;
		}

		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev) {
			dev_err(dev, "failed to get cpu device\n");
			ret = -ENODEV;
			goto err_free_cpu_data;
		}

		opp_table = dev_pm_opp_get_opp_table(cpu_dev);
		if (IS_ERR(opp_table)) {
			dev_err(dev, "failed to get opp_table of cpu%d\n", cpu);
			ret = PTR_ERR(opp_table);
			goto err_free_cpu_data;
		}

		parent_cpu_data->dev = cpu_dev;
		parent_cpu_data->opp_table = opp_table;
		parent_cpu_data->first_cpu = cpumask_first(policy->related_cpus);
		/*
		 * cur is committed AFTER POST delivery. Wait for that commit, then
		 * exclude a new transition through snapshot and list publication.
		 * Never wait while holding the mutex needed by a POST callback.
		 */
		for (;;) {
			if (READ_ONCE(policy->transition_task) == current) {
				ret = -EBUSY;
				dev_pm_opp_put_opp_table(opp_table);
				goto err_free_cpu_data;
			}
			wait_event(policy->transition_wait,
				   !READ_ONCE(policy->transition_ongoing));
			mutex_lock(&devfreq->lock);
			spin_lock_irqsave(&policy->transition_lock, flags);
			if (!policy->transition_ongoing)
				break;
			spin_unlock_irqrestore(&policy->transition_lock, flags);
			mutex_unlock(&devfreq->lock);
		}
		/* A terminal policy's cur is history, never an initial observation. */
		parent_cpu_data->unknown = READ_ONCE(policy->cur_unknown);
		parent_cpu_data->cur_freq = parent_cpu_data->unknown ? 0 : policy->cur;
		parent_cpu_data->min_freq = policy->cpuinfo.min_freq;
		parent_cpu_data->max_freq = policy->cpuinfo.max_freq;

		list_add_tail(&parent_cpu_data->node, &p_data->cpu_data_list);
		spin_unlock_irqrestore(&policy->transition_lock, flags);
		mutex_unlock(&devfreq->lock);
		cpufreq_cpu_put(policy);
	}

	if (list_empty(&p_data->cpu_data_list)) {
		ret = -ENODEV;
		goto err;
	}
	mutex_lock(&devfreq->lock);
	p_data->cpu_starting = false;
	ret = devfreq_update_target(devfreq, 0L);
	mutex_unlock(&devfreq->lock);
	if (ret) {
		dev_err(dev, "failed to update the frequency\n");
		goto err;
	}

	return ret;

err_free_cpu_data:
	kfree(parent_cpu_data);
err_put_policy:
	cpufreq_cpu_put(policy);
err:
	cpufreq_passive_unregister_notifier(devfreq);
	p_data->nb.notifier_call = NULL;
	return ret;
}

static int devfreq_passive_notifier_call(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct devfreq_passive_data *data
			= container_of(nb, struct devfreq_passive_data, nb);
	struct devfreq *devfreq = (struct devfreq *)data->this;
	struct devfreq *parent = (struct devfreq *)data->parent;
	struct devfreq_freqs *freqs = (struct devfreq_freqs *)ptr;
	unsigned long freq = freqs->new;
	int ret = 0;

	mutex_lock_nested(&devfreq->lock, SINGLE_DEPTH_NESTING);
	switch (event) {
	case DEVFREQ_PRECHANGE:
		if (parent->previous_freq > freq)
			ret = devfreq_update_target(devfreq, freq);

		break;
	case DEVFREQ_POSTCHANGE:
		if (parent->previous_freq < freq)
			ret = devfreq_update_target(devfreq, freq);
		break;
	}
	mutex_unlock(&devfreq->lock);

	if (ret < 0)
		dev_warn(&devfreq->dev,
			"failed to update devfreq using passive governor\n");

	return NOTIFY_DONE;
}

static int devfreq_passive_unregister_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq *parent = (struct devfreq *)p_data->parent;
	struct notifier_block *nb = &p_data->nb;

	return devfreq_unregister_notifier(parent, nb, DEVFREQ_TRANSITION_NOTIFIER);
}

static int devfreq_passive_register_notifier(struct devfreq *devfreq)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	struct devfreq *parent = (struct devfreq *)p_data->parent;
	struct notifier_block *nb = &p_data->nb;

	if (!parent)
		return -EPROBE_DEFER;

	nb->notifier_call = devfreq_passive_notifier_call;
	return devfreq_register_notifier(parent, nb, DEVFREQ_TRANSITION_NOTIFIER);
}

static int devfreq_passive_event_handler(struct devfreq *devfreq,
					 unsigned int event, void *data)
{
	struct devfreq_passive_data *p_data
			= (struct devfreq_passive_data *)devfreq->data;
	int ret = 0;

	if (!p_data)
		return -EINVAL;

	p_data->this = devfreq;

	switch (event) {
	case DEVFREQ_GOV_START:
		if (p_data->parent_type == DEVFREQ_PARENT_DEV)
			ret = devfreq_passive_register_notifier(devfreq);
		else if (p_data->parent_type == CPUFREQ_PARENT_DEV)
			ret = cpufreq_passive_register_notifier(devfreq);
		break;
	case DEVFREQ_GOV_STOP:
		if (p_data->parent_type == DEVFREQ_PARENT_DEV)
			WARN_ON(devfreq_passive_unregister_notifier(devfreq));
		else if (p_data->parent_type == CPUFREQ_PARENT_DEV)
			WARN_ON(cpufreq_passive_unregister_notifier(devfreq));
		break;
	default:
		break;
	}

	return ret;
}

static struct devfreq_governor devfreq_passive = {
	.name = DEVFREQ_GOV_PASSIVE,
	.flags = DEVFREQ_GOV_FLAG_IMMUTABLE,
	.get_target_freq = devfreq_passive_get_target_freq,
	.event_handler = devfreq_passive_event_handler,
};

static int __init devfreq_passive_init(void)
{
	return devfreq_add_governor(&devfreq_passive);
}
subsys_initcall(devfreq_passive_init);

static void __exit devfreq_passive_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_passive);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);
}
module_exit(devfreq_passive_exit);

MODULE_AUTHOR("Chanwoo Choi <cw00.choi@samsung.com>");
MODULE_AUTHOR("MyungJoo Ham <myungjoo.ham@samsung.com>");
MODULE_DESCRIPTION("DEVFREQ Passive governor");
MODULE_LICENSE("GPL v2");

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include "../regulator/mt6797-combined-test.h"
#include <kunit/test.h>

struct passive_fixture {
	struct devfreq df;
	struct devfreq_passive_data data;
	struct devfreq_cpu_data cpu;
	struct devfreq_dev_profile profile;
	unsigned long requested;
	unsigned int calls;
	bool fail;
	unsigned long table[4];
	u64 time[4];
	unsigned int transitions[16];
	void *saved_data;
};

static int passive_fixture_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct passive_fixture *f = dev_get_drvdata(dev);

	f->requested = *freq;
	f->calls++;
	return f->fail ? -EIO : 0;
}

static int passive_fixture_freq(struct devfreq *df, unsigned long *freq)
{
	/* CPU kHz to the synthetic CCI's Hz, using the normal passive override. */
	*freq *= 1000;
	return 0;
}

void *mt6797_test_passive_open(struct device *dev, unsigned int cpu)
{
	struct passive_fixture *f = kzalloc_obj(*f);
	int ret;

	if (!f)
		return ERR_PTR(-ENOMEM);
	f->saved_data = dev_get_drvdata(dev);
	dev_set_drvdata(dev, f);
	f->df.dev.parent = dev;
	f->df.data = &f->data;
	f->df.profile = &f->profile;
	f->df.governor = &devfreq_passive;
	f->df.freq_table = f->table;
	f->table[0] = 100000;
	f->table[1] = 150000;
	f->table[2] = 200000;
	f->table[3] = 300000;
	f->df.max_state = 4;
	f->df.scaling_min_freq = 100000;
	f->df.scaling_max_freq = 300000;
	f->df.stats.time_in_state = f->time;
	f->df.stats.trans_table = f->transitions;
	mutex_init(&f->df.lock);
	srcu_init_notifier_head(&f->df.transition_notifier_list);
	f->profile.target = passive_fixture_target;
	f->data.this = &f->df;
	f->data.get_target_freq = passive_fixture_freq;
	INIT_LIST_HEAD(&f->data.cpu_data_list);
	f->cpu.first_cpu = cpu;
	f->cpu.cur_freq = 100;
	list_add(&f->cpu.node, &f->data.cpu_data_list);
	f->data.nb.notifier_call = cpufreq_passive_notifier_call;
	ret = cpufreq_register_notifier(&f->data.nb, CPUFREQ_TERMINAL_NOTIFIER);
	if (ret) {
		dev_set_drvdata(dev, f->saved_data);
		srcu_cleanup_notifier_head(&f->df.transition_notifier_list);
		kfree(f);
		return ERR_PTR(ret);
	}
	return f;
}

void mt6797_test_passive_fail(void *data, bool fail)
{
	struct passive_fixture *f = data;

	f->fail = fail;
}

int mt6797_test_passive_error(void *data)
{
	struct passive_fixture *f = data;

	return f->cpu.update_error;
}

unsigned int mt6797_test_passive_calls(void *data)
{
	struct passive_fixture *f = data;

	return f->calls;
}

unsigned long mt6797_test_passive_rate(void *data)
{
	struct passive_fixture *f = data;

	return f->requested;
}

void mt6797_test_passive_close(void *data)
{
	struct passive_fixture *f = data;

	cpufreq_unregister_notifier(&f->data.nb, CPUFREQ_TERMINAL_NOTIFIER);
	dev_set_drvdata(f->df.dev.parent, f->saved_data);
	srcu_cleanup_notifier_head(&f->df.transition_notifier_list);
	kfree(f);
}

/* Real governor START/STOP, including policy lookup, OPP refs and target. */
void mt6797_test_passive_registration(struct kunit *test, struct device *dev,
				      int expected, unsigned int parents)
{
	struct passive_fixture *f = mt6797_test_passive_open(dev, 0);
	struct devfreq_cpu_data *cpu;
	unsigned int count = 0;
	int ret;

	KUNIT_ASSERT_FALSE(test, IS_ERR(f));
	cpufreq_unregister_notifier(&f->data.nb, CPUFREQ_TERMINAL_NOTIFIER);
	list_del(&f->cpu.node);
	f->data.get_target_freq = NULL;
	f->data.parent_type = CPUFREQ_PARENT_DEV;
	ret = devfreq_passive_event_handler(&f->df, DEVFREQ_GOV_START, NULL);
	KUNIT_EXPECT_EQ(test, ret, expected);
	list_for_each_entry(cpu, &f->data.cpu_data_list, node)
		count++;
	if (!ret) {
		KUNIT_EXPECT_EQ(test, count, parents);
		KUNIT_EXPECT_GT(test, f->calls, 0U);
		mt6797_test_passive_hide_first(true);
		mutex_lock(&f->df.lock);
		count = f->calls;
		KUNIT_EXPECT_EQ(test, devfreq_update_target(&f->df, 0), -EINVAL);
		KUNIT_EXPECT_EQ(test, f->calls, count);
		mt6797_test_passive_hide_first(false);
		KUNIT_EXPECT_EQ(test, devfreq_update_target(&f->df, 0), 0);
		cpu = list_first_entry(&f->data.cpu_data_list,
				      struct devfreq_cpu_data, node);
		list_del(&cpu->node);
		KUNIT_EXPECT_EQ(test, devfreq_update_target(&f->df, 0), -EAGAIN);
		list_add(&cpu->node, &f->data.cpu_data_list);
		mutex_unlock(&f->df.lock);
		{
			struct cpufreq_policy *policy = cpufreq_cpu_get(0);
			struct cpufreq_terminal terminal = { .policy = policy,
				.error = -EIO, .protection_needed = true };
			struct cpufreq_freqs freqs = { .policy = policy,
				.old = policy->cur, .new = policy->cur };

			count = f->calls;
			policy->cur_unknown = true;
			cpufreq_passive_notifier_call(&f->data.nb, CPUFREQ_TERMINAL,
						      &terminal);
			KUNIT_EXPECT_EQ(test, f->calls, count);
			KUNIT_EXPECT_EQ(test, devfreq_passive_event_handler(&f->df,
						DEVFREQ_GOV_STOP, NULL), 0);
			KUNIT_EXPECT_EQ(test, devfreq_passive_event_handler(&f->df,
						DEVFREQ_GOV_START, NULL), -EIO);
			KUNIT_EXPECT_EQ(test, f->calls, count);
			KUNIT_EXPECT_TRUE(test, list_empty(&f->data.cpu_data_list));
			/* Fixture-authorized known policy; no production recovery API. */
			policy->cur_unknown = false;
			KUNIT_EXPECT_EQ(test, devfreq_passive_event_handler(&f->df,
						DEVFREQ_GOV_START, NULL), 0);
			cpufreq_passive_notifier_call(&f->data.nb, CPUFREQ_TERMINAL,
						      &terminal);
			count = f->calls;
			cpufreq_passive_notifier_call(&f->data.nb, CPUFREQ_POSTCHANGE,
						      &freqs);
			KUNIT_EXPECT_EQ(test, f->calls, count + 1);
			KUNIT_EXPECT_EQ(test, devfreq_passive_event_handler(&f->df,
						DEVFREQ_GOV_STOP, NULL), 0);
			cpufreq_cpu_put(policy);
		}
	} else {
		KUNIT_EXPECT_EQ(test, count, 0U);
		KUNIT_EXPECT_EQ(test, f->calls, 0U);
	}
	dev_set_drvdata(dev, f->saved_data);
	srcu_cleanup_notifier_head(&f->df.transition_notifier_list);
	kfree(f);
}
#endif
