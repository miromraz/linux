// SPDX-License-Identifier: GPL-2.0-only
/*
 *  drivers/cpufreq/cpufreq_stats.c
 *
 *  Copyright (C) 2003-2004 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *  (C) 2004 Zou Nan hai <nanhai.zou@intel.com>.
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>

struct cpufreq_stats {
	unsigned int total_trans;
	unsigned long long last_time;
	unsigned int max_state;
	unsigned int state_num;
	unsigned int last_index;
	unsigned int last_freq;
	u64 *time_in_state;
	unsigned int *freq_table;
	unsigned int *trans_table;

	/* Deferred reset */
	unsigned int reset_pending;
	unsigned long long reset_time;
};

static void cpufreq_stats_update(struct cpufreq_stats *stats,
				 unsigned long long time)
{
	unsigned long long cur_time = local_clock();

	if (stats->last_index < stats->state_num)
		stats->time_in_state[stats->last_index] += cur_time - time;
	stats->last_time = cur_time;
}

static void cpufreq_stats_reset_table(struct cpufreq_stats *stats)
{
	unsigned int count = stats->max_state;

	memset(stats->time_in_state, 0, count * sizeof(u64));
	memset(stats->trans_table, 0, count * count * sizeof(int));
	stats->last_time = local_clock();
	stats->total_trans = 0;

	/* Adjust for the time elapsed since reset was requested */
	WRITE_ONCE(stats->reset_pending, 0);
	/*
	 * Prevent the reset_time read from being reordered before the
	 * reset_pending accesses in cpufreq_stats_record_transition().
	 */
	smp_rmb();
	cpufreq_stats_update(stats, READ_ONCE(stats->reset_time));
}

static ssize_t show_total_trans(struct cpufreq_policy *policy, char *buf)
{
	struct cpufreq_stats *stats = policy->stats;

	if (READ_ONCE(stats->reset_pending))
		return sprintf(buf, "%d\n", 0);
	else
		return sprintf(buf, "%u\n", stats->total_trans);
}
cpufreq_freq_attr_ro(total_trans);

static ssize_t show_time_in_state(struct cpufreq_policy *policy, char *buf)
{
	struct cpufreq_stats *stats = policy->stats;
	bool pending = READ_ONCE(stats->reset_pending);
	unsigned long long time;
	ssize_t len = 0;
	int i;

	for (i = 0; i < stats->state_num; i++) {
		if (pending) {
			if (i == stats->last_index) {
				/*
				 * Prevent the reset_time read from occurring
				 * before the reset_pending read above.
				 */
				smp_rmb();
				time = local_clock() - READ_ONCE(stats->reset_time);
			} else {
				time = 0;
			}
		} else {
			time = stats->time_in_state[i];
			if (i == stats->last_index)
				time += local_clock() - stats->last_time;
		}

		len += sprintf(buf + len, "%u %llu\n", stats->freq_table[i],
			       nsec_to_clock_t(time));
	}
	return len;
}
cpufreq_freq_attr_ro(time_in_state);

/* We don't care what is written to the attribute */
static ssize_t store_reset(struct cpufreq_policy *policy, const char *buf,
			   size_t count)
{
	struct cpufreq_stats *stats = policy->stats;

	/*
	 * Defer resetting of stats to cpufreq_stats_record_transition() to
	 * avoid races.
	 */
	WRITE_ONCE(stats->reset_time, local_clock());
	/*
	 * The memory barrier below is to prevent the readers of reset_time from
	 * seeing a stale or partially updated value.
	 */
	smp_wmb();
	WRITE_ONCE(stats->reset_pending, 1);

	return count;
}
cpufreq_freq_attr_wo(reset);

static ssize_t show_trans_table(struct cpufreq_policy *policy, char *buf)
{
	struct cpufreq_stats *stats = policy->stats;
	bool pending = READ_ONCE(stats->reset_pending);
	ssize_t len = 0;
	int i, j, count;

	len += sysfs_emit_at(buf, len, "   From  :    To\n");
	len += sysfs_emit_at(buf, len, "         : ");
	for (i = 0; i < stats->state_num; i++) {
		if (len >= PAGE_SIZE - 1)
			break;
		len += sysfs_emit_at(buf, len, "%9u ", stats->freq_table[i]);
	}
	if (len >= PAGE_SIZE - 1)
		return PAGE_SIZE - 1;

	len += sysfs_emit_at(buf, len, "\n");

	for (i = 0; i < stats->state_num; i++) {
		if (len >= PAGE_SIZE - 1)
			break;

		len += sysfs_emit_at(buf, len, "%9u: ", stats->freq_table[i]);

		for (j = 0; j < stats->state_num; j++) {
			if (len >= PAGE_SIZE - 1)
				break;

			if (pending)
				count = 0;
			else
				count = stats->trans_table[i * stats->max_state + j];

			len += sysfs_emit_at(buf, len, "%9u ", count);
		}
		if (len >= PAGE_SIZE - 1)
			break;
		len += sysfs_emit_at(buf, len, "\n");
	}

	if (len >= PAGE_SIZE - 1) {
		pr_warn_once("cpufreq transition table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}
	return len;
}
cpufreq_freq_attr_ro(trans_table);

static struct attribute *default_attrs[] = {
	&total_trans.attr,
	&time_in_state.attr,
	&reset.attr,
	&trans_table.attr,
	NULL
};
static const struct attribute_group stats_attr_group = {
	.attrs = default_attrs,
	.name = "stats"
};

static int freq_table_get_index(struct cpufreq_stats *stats, unsigned int freq)
{
	int index;
	for (index = 0; index < stats->state_num; index++)
		if (stats->freq_table[index] == freq)
			return index;
	return -1;
}

void cpufreq_stats_free_table(struct cpufreq_policy *policy)
{
	struct cpufreq_stats *stats = policy->stats;

	/* Already freed */
	if (!stats)
		return;

	pr_debug("%s: Free stats table\n", __func__);

	sysfs_remove_group(&policy->kobj, &stats_attr_group);
	kfree(stats->time_in_state);
	kfree(stats);
	policy->stats = NULL;
}

void cpufreq_stats_create_table(struct cpufreq_policy *policy)
{
	unsigned int count;
	struct cpufreq_stats *stats;
	unsigned int alloc_size;
	struct cpufreq_frequency_table *pos;

	count = cpufreq_table_count_valid_entries(policy);
	if (!count)
		return;

	/* stats already initialized */
	if (policy->stats)
		return;

	stats = kzalloc_obj(*stats);
	if (!stats)
		return;

	alloc_size = count * sizeof(int) + count * sizeof(u64);

	alloc_size += count * count * sizeof(int);

	/* Allocate memory for time_in_state/freq_table/trans_table in one go */
	stats->time_in_state = kzalloc(alloc_size, GFP_KERNEL);
	if (!stats->time_in_state)
		goto free_stat;

	stats->freq_table = (unsigned int *)(stats->time_in_state + count);

	stats->trans_table = stats->freq_table + count;

	stats->max_state = count;

	/* Find valid-unique entries */
	cpufreq_for_each_valid_entry(pos, policy->freq_table)
		if (policy->freq_table_sorted != CPUFREQ_TABLE_UNSORTED ||
		    freq_table_get_index(stats, pos->frequency) == -1)
			stats->freq_table[stats->state_num++] = pos->frequency;

	stats->last_time = local_clock();
	stats->last_freq = policy->cur_unknown ? 0 : policy->cur;
	stats->last_index = stats->last_freq ?
		freq_table_get_index(stats, stats->last_freq) : -1;

	policy->stats = stats;
	if (!sysfs_create_group(&policy->kobj, &stats_attr_group))
		return;

	/* We failed, release resources */
	policy->stats = NULL;
	kfree(stats->time_in_state);
free_stat:
	kfree(stats);
}

void cpufreq_stats_record_transition(struct cpufreq_policy *policy,
				     unsigned int new_freq)
{
	struct cpufreq_stats *stats = policy->stats;
	int old_index, new_index;

	if (unlikely(!stats))
		return;

	if (unlikely(READ_ONCE(stats->reset_pending)))
		cpufreq_stats_reset_table(stats);

	old_index = stats->last_index;
	new_index = new_freq ? freq_table_get_index(stats, new_freq) : -1;

	if (stats->last_freq == new_freq)
		return;

	/* Unlisted/unknown time must not be charged to the previous OPP. */
	cpufreq_stats_update(stats, stats->last_time);
	stats->last_index = new_index;
	stats->last_freq = new_freq;
	if (old_index >= 0 && new_index >= 0)
		stats->trans_table[old_index * stats->max_state + new_index]++;
	if (new_freq)
		stats->total_trans++;
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include "../regulator/mt6797-combined-test.h"
#include <kunit/test.h>

unsigned int mt6797_test_stats_rate(struct cpufreq_policy *policy)
{
	struct cpufreq_stats *stats = policy->stats;

	if (!stats || stats->last_index >= stats->state_num)
		return 0;
	return stats->freq_table[stats->last_index];
}

/* Exercise the real accounting/reset paths; no synthetic frequency rows. */
void mt6797_test_stats_contract(struct kunit *test, struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table table[] = {
		{ .frequency = 200 }, { .frequency = 100 },
		{ .frequency = 100 }, { .frequency = CPUFREQ_TABLE_END },
	};
	struct cpufreq_frequency_table *saved = policy->freq_table;
	unsigned int sorted = policy->freq_table_sorted;
	struct cpufreq_stats *stats;
	u64 residence;
	unsigned int i;

	cpufreq_stats_free_table(policy);
	policy->freq_table = table;
	policy->freq_table_sorted = CPUFREQ_TABLE_UNSORTED;
	cpufreq_stats_create_table(policy);
	policy->freq_table = saved;
	policy->freq_table_sorted = sorted;
	stats = policy->stats;
	KUNIT_ASSERT_NOT_NULL(test, stats);
	KUNIT_EXPECT_EQ(test, stats->state_num, 2U);
	KUNIT_EXPECT_EQ(test, freq_table_get_index(stats, 0), -1);
	cpufreq_stats_record_transition(policy, 100);
	store_reset(policy, "", 0);
	cpufreq_stats_record_transition(policy, 150);
	residence = stats->time_in_state[1];
	cpufreq_stats_record_transition(policy, 175);
	cpufreq_stats_record_transition(policy, 175);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 2U);
	KUNIT_EXPECT_EQ(test, stats->last_freq, 175U);
	KUNIT_EXPECT_EQ(test, stats->time_in_state[1], residence);
	cpufreq_stats_record_transition(policy, 200);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 3U);
	for (i = 0; i < stats->max_state * stats->max_state; i++)
		KUNIT_EXPECT_EQ(test, stats->trans_table[i], 0U);
	cpufreq_stats_record_transition(policy, 100);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 4U);
	KUNIT_EXPECT_EQ(test, stats->trans_table[1], 1U);
	cpufreq_stats_record_transition(policy, 150);
	store_reset(policy, "", 0);
	cpufreq_stats_record_transition(policy, 175);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 1U);
	for (i = 0; i < stats->state_num; i++)
		KUNIT_EXPECT_EQ(test, stats->time_in_state[i], 0ULL);
	cpufreq_stats_record_transition(policy, 0);
	KUNIT_EXPECT_EQ(test, stats->last_index, (unsigned int)-1);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 1U);
	cpufreq_stats_record_transition(policy, 0);
	cpufreq_stats_record_transition(policy, 100);
	KUNIT_EXPECT_EQ(test, stats->total_trans, 2U);
}
#endif
