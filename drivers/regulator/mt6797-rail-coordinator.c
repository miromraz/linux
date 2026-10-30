// SPDX-License-Identifier: GPL-2.0-only
/* Provider-side durable votes for the Gemini A53/CCI rail pair. */
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/string.h>
#include "mt6797-rail-coordinator.h"

#define PROC_MIN 1040000
#define RAIL_MAX 1200000
#define MAX_SPREAD 160000

void mt6797_rail_init(struct mt6797_rail_coordinator *c)
{
	memset(c, 0, sizeof(*c));
	mutex_init(&c->lock);
}
EXPORT_SYMBOL_GPL(mt6797_rail_init);

void mt6797_rail_status(struct mt6797_rail_coordinator *c,
			struct mt6797_rail_status *status)
{
	mutex_lock(&c->lock);
	status->generation = c->generation;
	memcpy(status->floor, c->floor, sizeof(status->floor));
	status->ready = c->converted && c->requirements_valid && !c->fault;
	status->fault = c->fault;
	mutex_unlock(&c->lock);
}
EXPORT_SYMBOL_GPL(mt6797_rail_status);

int mt6797_rail_attach(struct mt6797_rail_coordinator *c,
		       enum mt6797_rail_id id, const struct mt6797_rail_io *io)
{
	int ret = 0;

	if ((unsigned int)id >= MT6797_RAILS || !io || !io->read || !io->write ||
	    io->step_uv != (id == MT6797_PROC ? 10000 : 25000))
		return -EINVAL;
	mutex_lock(&c->lock);
	if (c->rail[id].read) {
		ret = -EBUSY;
		goto out;
	}
	c->rail[id] = *io;
	c->generation++;
	c->converted = false;
	c->requirements_valid = false;
	/* A new provider must be revalidated; arrival never authorizes writes. */
out:
	mutex_unlock(&c->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_rail_attach);

void mt6797_rail_detach(struct mt6797_rail_coordinator *c,
			enum mt6797_rail_id id)
{
	if ((unsigned int)id >= MT6797_RAILS)
		return;
	mutex_lock(&c->lock);
	memset(&c->rail[id], 0, sizeof(c->rail[id]));
	c->generation++;
	c->converted = false;
	c->requirements_valid = false;
	/* Retain all floors across removal. No consumer teardown clears them. */
	mutex_unlock(&c->lock);
}
EXPORT_SYMBOL_GPL(mt6797_rail_detach);

static int mt6797_rail_read(struct mt6797_rail_coordinator *c, int uv[2])
{
	int i;

	for (i = 0; i < MT6797_RAILS; i++) {
		uv[i] = c->rail[i].read(c->rail[i].data);
		if (uv[i] < 0)
			return uv[i];
	}
	if (uv[0] < 840000 || uv[1] < 1000000 || uv[1] > RAIL_MAX || uv[0] > uv[1] ||
	    uv[1] - uv[0] > MAX_SPREAD ||
	    uv[0] % 10000 || uv[1] % 25000)
		return -ERANGE;
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		if (uv[0] < c->floor[i])
			return -ERANGE;
	return 0;
}

static int mt6797_rail_step(struct mt6797_rail_coordinator *c, int id,
			    int target, int uv[2])
{
	int ret;

	/* Check the proposed physical step before issuing any write. */
	if ((id == MT6797_PROC && target > uv[MT6797_SRAM]) ||
	    (id == MT6797_SRAM && target - uv[MT6797_PROC] > MAX_SPREAD))
		return -ERANGE;
	ret = c->rail[id].write(c->rail[id].data, target);
	if (!ret)
		ret = mt6797_rail_read(c, uv);
	if (!ret && uv[id] != target)
		ret = -EIO;
	if (ret) {
		c->fault = true;
		c->converted = false;
		c->requirements_valid = false;
	}
	return ret;
}

/* Recovery is read-only: never repair an undervolted or disabled live rail.
 * The caller reserves all clocks/OPPs and supplies durable core requests too.
 */
int mt6797_rail_recover(struct mt6797_rail_coordinator *c, u64 generation,
		       const int required[MT6797_RAIL_USERS])
{
	int uv[2], i, ret;

	mutex_lock(&c->lock);
	if (generation != c->generation) {
		ret = -ESTALE;
		goto out;
	}
	if (!c->rail[0].read || !c->rail[1].read) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	ret = mt6797_rail_read(c, uv);
	if (ret)
		goto out;
	if (uv[1] != RAIL_MAX || uv[0] < PROC_MIN) {
		ret = -ERANGE;
		goto out;
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		if (required[i] <= 0 || required[i] > uv[0]) {
			ret = -ERANGE;
			goto out;
		}
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		c->floor[i] = max(c->floor[i], required[i]);
	c->fault = false;
	c->converted = true;
	c->requirements_valid = true;
out:
	mutex_unlock(&c->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_rail_recover);

int mt6797_rail_prepare(struct mt6797_rail_coordinator *c, u64 generation,
		       const int floor[MT6797_RAIL_USERS])
{
	int uv[2], i, target, ret = 0;

	mutex_lock(&c->lock);
	if (generation != c->generation) {
		ret = -ESTALE;
		goto out;
	}
	if (!c->rail[0].read || !c->rail[1].read) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	if (c->fault) {
		ret = -EIO;
		goto out;
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		if (floor[i] <= 0 || floor[i] > RAIL_MAX) {
			ret = -ERANGE;
			goto out;
		}
	}
	/* Only an accepted request revokes readiness. Stale tokens and malformed
	 * requests must not invalidate a different, already validated owner.
	 */
	c->requirements_valid = false;
	c->converted = false;
	/* Keep the old requirements as well until live hardware is validated. */
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		c->floor[i] = max(c->floor[i], floor[i]);
	ret = mt6797_rail_read(c, uv);
	if (ret)
		goto out;
	/* Raise SRAM one grid step if possible, otherwise raise proc one step.
	 * Every state satisfies proc <= SRAM <= proc + 160 mV. No electrical
	 * supply relationship is assumed. Both loops are bounded by the grids.
	 */
	while (uv[MT6797_SRAM] < RAIL_MAX) {
		target = uv[MT6797_SRAM] + c->rail[MT6797_SRAM].step_uv;
		if (target - uv[MT6797_PROC] <= MAX_SPREAD)
			ret = mt6797_rail_step(c, MT6797_SRAM, target, uv);
		else
			ret = mt6797_rail_step(c, MT6797_PROC,
				uv[MT6797_PROC] + c->rail[MT6797_PROC].step_uv, uv);
		if (ret)
			goto out;
	}
	c->requirements_valid = true;
	c->converted = true;
out:
	mutex_unlock(&c->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_rail_prepare);

int mt6797_rail_vote(struct mt6797_rail_coordinator *c, u64 generation,
		    unsigned int user, int uv)
{
	int actual[2], target = PROC_MIN, i, ret;

	if (user >= MT6797_RAIL_USERS || uv < PROC_MIN || uv > RAIL_MAX)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (generation != c->generation) {
		ret = -ESTALE;
		goto out;
	}
	if (!c->converted || !c->requirements_valid || c->fault) {
		ret = -EBUSY;
		goto out;
	}
	/* The old user's vote remains until the replacement is verified. This
	 * primitive therefore only raises votes; returning/lowering ownership
	 * requires a separately validated live-clock transaction.
	 */
	if (uv < c->floor[user]) {
		ret = -EBUSY;
		goto out;
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		target = max(target, i == user ? uv : c->floor[i]);
	target = roundup(target, c->rail[MT6797_PROC].step_uv);
	ret = mt6797_rail_read(c, actual);
	if (ret) {
		c->requirements_valid = false;
		c->converted = false;
		goto out;
	}
	ret = mt6797_rail_step(c, MT6797_PROC, target, actual);
	if (!ret)
		c->floor[user] = uv;
out:
	mutex_unlock(&c->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_rail_vote);

/* The caller reserves every live/intermediate clock and OPP table throughout
 * this transaction. A lowered request is legal only against that fresh proof.
 * Failure retains accounting, even if an uncertain physical write committed.
 */
int mt6797_rail_replace(struct mt6797_rail_coordinator *c, u64 generation,
		       unsigned int user, int request,
		       const int required[MT6797_RAIL_USERS])
{
	int old[MT6797_RAIL_USERS], actual[2], target = PROC_MIN;
	int i, ret;

	if (user >= MT6797_RAIL_USERS || request < PROC_MIN || request > RAIL_MAX)
		return -EINVAL;
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		if (required[i] <= 0 || required[i] > RAIL_MAX)
			return -ERANGE;
	if (request < required[user])
		return -ERANGE;
	mutex_lock(&c->lock);
	if (generation != c->generation) {
		ret = -ESTALE;
		goto out;
	}
	if (!c->converted || !c->requirements_valid || c->fault) {
		ret = -EBUSY;
		goto out;
	}
	memcpy(old, c->floor, sizeof(old));
	ret = mt6797_rail_read(c, actual);
	if (ret || actual[MT6797_SRAM] != RAIL_MAX) {
		ret = ret ?: -ERANGE;
		c->requirements_valid = false;
		goto retain;
	}
	/* Never repair a newly discovered undervolted live clock by raising it. */
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		if (actual[MT6797_PROC] < required[i]) {
			ret = -ERANGE;
			c->requirements_valid = false;
			goto retain;
		}
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		target = max(target, i == user ? request : max(old[i], required[i]));
	target = roundup(target, c->rail[MT6797_PROC].step_uv);
	/* Raised requests are committed only after readback, so validate each
	 * physical step against the live requirements rather than the new vote.
	 */
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		c->floor[i] = i == user ? min(request, old[i]) : max(old[i], required[i]);
	ret = mt6797_rail_step(c, MT6797_PROC, target, actual);
	if (!ret) {
		c->floor[user] = request;
		goto out;
	}
retain:
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		c->floor[i] = max(old[i], required[i]);
out:
	mutex_unlock(&c->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_rail_replace);
MODULE_LICENSE("GPL");
