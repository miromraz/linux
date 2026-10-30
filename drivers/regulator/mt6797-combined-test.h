/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_COMBINED_TEST_H
#define MT6797_COMBINED_TEST_H
/* A stack marker is removed on success and synchronously released on error. */
static inline void mt6797_test_probe_destroyed(void *data)
{
	*(bool *)data = true;
}

void mt6797_test_switch_hook(void (*hook)(void *, int), void *data);
struct cpufreq_policy;
struct cpufreq_driver;
struct cpufreq_policy *mt6797_test_policy_alloc(unsigned int cpu);
void mt6797_test_policy_free(struct cpufreq_policy *policy);
int mt6797_test_core_target(struct cpufreq_policy *policy,
			   struct cpufreq_driver *driver, unsigned int index);
struct kunit;
void mt6797_test_legacy_completion(struct kunit *test);
void mt6797_test_stats_contract(struct kunit *test, struct cpufreq_policy *policy);
unsigned int mt6797_test_stats_rate(struct cpufreq_policy *policy);
struct cpufreq_policy *mt6797_test_cpu_policy(void *data);
bool mt6797_test_cpu_unlocked(void *data);
void mt6797_test_cpu_release(void *data);
u64 mt6797_test_cpu_cookie(void *data);
int mt6797_test_cpu_reenter(void *data);
struct device;
void *mt6797_test_passive_open(struct device *dev, unsigned int cpu);
void mt6797_test_passive_close(void *data);
void mt6797_test_passive_hide_first(bool hide);
void mt6797_test_passive_registration(struct kunit *test, struct device *dev,
				      int expected, unsigned int parents);
void mt6797_test_passive_fail(void *data, bool fail);
int mt6797_test_passive_error(void *data);
unsigned int mt6797_test_passive_calls(void *data);
unsigned long mt6797_test_passive_rate(void *data);
struct clk;
void mt6797_test_cci_targets(struct kunit *test, struct device *dev,
			   u64 cookie, struct clk *mux);
struct device_node;
struct platform_device;
struct regmap;
int mt6797_test_buck(struct platform_device *pdev, struct regmap *map,
		     struct device_node *np);
int mt6797_test_sram(struct platform_device *pdev);
int mt6797_test_cpu_probe(struct platform_device *pdev, int fail_cpu);
int mt6797_test_cci_probe(struct platform_device *pdev);
int mt6797_test_cci_remove(struct platform_device *pdev);
void mt6797_test_transfer_hook(void (*hook)(void *, unsigned int, bool, int), void *data);
void mt6797_test_cpu_hook(void (*hook)(void *, int), void *data);
struct mt6797_txn_outcome;
void *mt6797_test_cpu_open(struct device *dev, unsigned int user);
int mt6797_test_cpu_target(void *data, unsigned int khz,
			   struct mt6797_txn_outcome *outcome);
void mt6797_test_cpu_close(void *data);
void mt6797_test_passive_reentry(struct kunit *test, struct device *dev,
				 struct cpufreq_policy *policy, bool from_notifier);
#endif
