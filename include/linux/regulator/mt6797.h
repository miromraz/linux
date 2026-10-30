/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_REGULATOR_MT6797_H
#define _LINUX_REGULATOR_MT6797_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

/* Protected lifetimes only. These calls never grant operational admission. */
#if IS_REACHABLE(CONFIG_REGULATOR_MT6797_PROVIDER)
int mt6797_consumer_acquire(struct device *dev, unsigned int user, u64 *cookie);
int mt6797_consumer_release(unsigned int user, u64 cookie);
/* Explicit process-context retry after all leases are released. Queue from
 * notifiers; never call on a regulator/clock/OPP notification stack.
 * Same consumer devices and speed bin are required. Never grants admission.
 */
int mt6797_consumer_recover(void);
#else
static inline int mt6797_consumer_recover(void)
{
	return -EPROBE_DEFER;
}
static inline int mt6797_consumer_acquire(struct device *dev, unsigned int user,
					u64 *cookie)
{
	return -EPROBE_DEFER;
}
static inline int mt6797_consumer_release(unsigned int user, u64 cookie)
{
	return -EPROBE_DEFER;
}
#endif
#endif
