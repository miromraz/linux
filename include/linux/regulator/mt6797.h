/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_REGULATOR_MT6797_H
#define _LINUX_REGULATOR_MT6797_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct device;

/* Bind-time ownership exercise only; operational Phase3 admission is closed. */
#if IS_REACHABLE(CONFIG_REGULATOR_MT6797_PROVIDER)
int mt6797_consumer_bind_gate(struct device *dev, unsigned int user);
#else
static inline int mt6797_consumer_bind_gate(struct device *dev, unsigned int user)
{
	return -EPROBE_DEFER;
}
#endif
#endif
