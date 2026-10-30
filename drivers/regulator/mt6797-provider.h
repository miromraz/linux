/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_PROVIDER_H
#define _MT6797_PROVIDER_H

#include <linux/device.h>
#include <linux/notifier.h>
#include "mt6797-rail-coordinator.h"

/* Read-only observation, never conversion or consumer admission. */
struct mt6797_preflight_result {
	u64 generation;
	u64 provider_generation;
	int error;
	unsigned long rate[MT6797_RAIL_USERS][2];
	unsigned long ceiling[MT6797_RAIL_USERS][2];
	unsigned long uv[MT6797_RAIL_USERS][2];
	u8 speedbin[MT6797_RAIL_USERS];
	int rail_uv[MT6797_RAILS];
};

enum mt6797_admission {
	MT6797_ADMISSION_PROTECTED,
	MT6797_ADMISSION_PHASE3_BLOCKED,
};

/* Events are hints to take a fresh snapshot, never admission tokens. */
struct mt6797_provider_status {
	struct mt6797_rail_status engine;
	unsigned long attached;
	int readiness;
	bool converted;
	unsigned long adopted;
	unsigned long active, retained;
	int ownership_error[MT6797_RAIL_USERS];
	u64 ownership_generation[MT6797_RAIL_USERS];
	enum mt6797_admission admission;
	/* Informational final policy, zero until conversion is verified. */
	int proc_min_uv, proc_max_uv, fixed_sram_uv;
	struct mt6797_preflight_result preflight;
};

int mt6797_preflight_hw(u8 bin, unsigned int *hw);
int mt6797_preflight_ceiling(struct device *dev, unsigned long rate,
			    unsigned long *freq, unsigned long *uv);
int mt6797_provider_buck_match(struct device_node *provider, unsigned int id);
int mt6797_boot_barrier_supply(struct device_node *provider, const char *supply);
int mt6797_provider_attach(struct device *dev, enum mt6797_rail_id id,
			   const struct mt6797_rail_io *io);
int mt6797_provider_sample(u64 generation, int uv[MT6797_RAILS]);
void mt6797_provider_status(struct mt6797_provider_status *status);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
struct clk;
struct completion;
void mt6797_txn_test_fail_acquisition(unsigned int stage);
int mt6797_preflight_test_hold_provider(u64 generation, struct completion *entered,
		struct completion *resume);
void mt6797_preflight_test_drain_observer(struct completion *cancelled);
int mt6797_preflight_test_handover(struct device **dev, struct clk *clk,
		struct mt6797_preflight_result *result, unsigned int user,
		int request, bool returning);
int mt6797_preflight_test_convert(struct device **dev, struct clk *clk,
				  struct mt6797_preflight_result *result);
int mt6797_preflight_test_init_failure(int fail_after, void (*step)(void));
void mt6797_preflight_test_event(void);
int mt6797_preflight_test_regulator_notifier(unsigned int user,
					    struct notifier_block *nb, bool add);
int mt6797_preflight_test_requests(unsigned int user, int *guard, int *vote);
int mt6797_preflight_test_start_clocks(struct device **dev, struct clk *clocks[3][2]);
int mt6797_preflight_test_start_users(struct device **dev, struct clk *clk);
int mt6797_preflight_test_start(struct device *dev, struct clk *clk);
void mt6797_preflight_test_stop(void);
void mt6797_preflight_test_flush(void);
void mt6797_preflight_test_hint(struct device *dev);
#endif
/* Callbacks run serially on a worker, with a borrowed status snapshot in data.
 * Events may coalesce; payload generation/error describe that snapshot only.
 * Queue framework work; never recursively subscribe/unsubscribe or return STOP.
 * Unsubscribe drains callbacks; owners must then cancel their queued work.
 */
int mt6797_provider_subscribe(struct notifier_block *nb);
int mt6797_provider_unsubscribe(struct notifier_block *nb);
#endif
