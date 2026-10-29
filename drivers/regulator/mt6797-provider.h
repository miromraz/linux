/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_PROVIDER_H
#define _MT6797_PROVIDER_H

#include <linux/device.h>
#include <linux/notifier.h>
#include "mt6797-rail-coordinator.h"

/* Events are hints to take a fresh snapshot, never admission tokens. */
struct mt6797_provider_status {
	struct mt6797_rail_status engine;
	unsigned long attached;
	int readiness;
};

int mt6797_boot_barrier_supply(struct device_node *provider, const char *supply);
int mt6797_provider_attach(struct device *dev, enum mt6797_rail_id id,
			   const struct mt6797_rail_io *io);
int mt6797_provider_sample(u64 generation, int uv[MT6797_RAILS]);
void mt6797_provider_status(struct mt6797_provider_status *status);
int mt6797_provider_subscribe(struct notifier_block *nb);
int mt6797_provider_unsubscribe(struct notifier_block *nb);
#endif
