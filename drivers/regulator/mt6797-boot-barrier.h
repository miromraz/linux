/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_BOOT_BARRIER_H
#define __MT6797_BOOT_BARRIER_H

#include <linux/errno.h>
#include <linux/of.h>

/*
 * Stage one of Gemini's shared-rail handover: neither provider may change
 * its boot state until all live users have durable replacement ownership.
 * There is deliberately no release operation yet. This barrier is a policy
 * of the actual CPU supply, not an electrical dependency between the rails.
 * Reprobe cannot release it, and withdrawal of consumer votes cannot bypass
 * it. No saved voltage or temporary consumer handle is used as ownership.
 */
static inline bool mt6797_boot_barrier_supply(struct device_node *provider,
					     const char *supply)
{
	struct device_node *cpu, *node;
	bool match;

	if (!provider || !of_machine_is_compatible("planet,gemini-pda"))
		return false;
	cpu = of_get_cpu_node(0, NULL);
	node = of_parse_phandle(cpu, supply, 0);
	match = node == provider;
	of_node_put(node);
	of_node_put(cpu);
	return match;
}

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
