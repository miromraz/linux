/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_REGULATOR_MT6797_H
#define _LINUX_REGULATOR_MT6797_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

struct mt6797_consumer_txn;
enum mt6797_txn_path {
	MT6797_TXN_OLD, MT6797_TXN_INTERMEDIATE, MT6797_TXN_TARGET,
	MT6797_TXN_UNKNOWN,
};
struct mt6797_txn_outcome {
	int error, retained_uv;
	enum mt6797_txn_path path;
	unsigned long rate;
	bool retry;
};

/* Protected lifetimes only. These calls never grant operational admission. */
#if IS_REACHABLE(CONFIG_REGULATOR_MT6797_PROVIDER)
/* Same-task process-context API. End consumes the token, outside driver locks
 * because final OPP puts can notify. Clocks/providers must remain registered.
 * CPU0/CPU4 registry users only; admission is separately gated.
 */
int mt6797_consumer_txn_begin(unsigned int user, u64 cookie,
		unsigned long target_hz, struct mt6797_consumer_txn **txn);
int mt6797_consumer_txn_validate(struct mt6797_consumer_txn *txn);
int mt6797_consumer_txn_prepare(struct mt6797_consumer_txn *txn);
/* Parks and verifies before rounding the retained PLL, then programs/rejoins. */
int mt6797_consumer_txn_switch(struct mt6797_consumer_txn *txn);
int mt6797_consumer_txn_finish(struct mt6797_consumer_txn *txn,
		struct mt6797_txn_outcome *outcome);
void mt6797_consumer_txn_abort(struct mt6797_consumer_txn *txn, int error,
		struct mt6797_txn_outcome *outcome);
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
