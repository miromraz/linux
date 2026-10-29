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

/* Events are hints to take a fresh snapshot, never admission tokens. */
struct mt6797_provider_status {
	struct mt6797_rail_status engine;
	unsigned long attached;
	int readiness;
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
/* Callbacks can overlap and run during probe/remove under driver-core locks.
 * Queue framework work; never recursively subscribe/unsubscribe or return STOP.
 * Unsubscribe drains callbacks; owners must then cancel their queued work.
 */
int mt6797_provider_subscribe(struct notifier_block *nb);
int mt6797_provider_unsubscribe(struct notifier_block *nb);
#endif
