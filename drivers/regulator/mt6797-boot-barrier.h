/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_BOOT_BARRIER_H
#define __MT6797_BOOT_BARRIER_H

#include <linux/errno.h>
#include <linux/of.h>

#include "mt6797-provider.h"

static inline int mt6797_boot_barrier_check(int enabled, int selected,
					    unsigned int requested)
{
	if (enabled < 0)
		return enabled;
	if (!enabled)
		return -EIO;
	if (selected < 0)
		return selected;
	return selected == requested ? 0 : -EBUSY;
}

#endif
