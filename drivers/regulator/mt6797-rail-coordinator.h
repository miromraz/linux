/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_RAIL_COORDINATOR_H
#define _MT6797_RAIL_COORDINATOR_H

#include <linux/mutex.h>
#include <linux/types.h>

#define MT6797_RAIL_USERS 3

enum mt6797_rail_id { MT6797_PROC, MT6797_SRAM, MT6797_RAILS };

/* Raw provider operations: no regulator/OPP calls or notifications allowed.
 * write() must settle before returning, including on an uncertain failure.
 * read() returns the selected voltage and rejects disabled/invalid banks.
 */
struct mt6797_rail_io {
	int (*read)(void *data);
	int (*write)(void *data, int uv);
	void *data;
	int step_uv;
};

struct mt6797_rail_coordinator {
	struct mutex lock;
	struct mt6797_rail_io rail[MT6797_RAILS];
	u64 generation;
	int floor[MT6797_RAIL_USERS];
	bool requirements_valid;
	bool converted;
	bool fault;
};

/* A single synchronized observation; readiness never grants a clock change
 * without revalidation of generation by the subsequent operation.
 */
struct mt6797_rail_status {
	u64 generation;
	int floor[MT6797_RAIL_USERS];
	bool ready;
	bool fault;
};

void mt6797_rail_status(struct mt6797_rail_coordinator *c,
			struct mt6797_rail_status *status);
void mt6797_rail_init(struct mt6797_rail_coordinator *c);
int mt6797_rail_attach(struct mt6797_rail_coordinator *c,
		       enum mt6797_rail_id id, const struct mt6797_rail_io *io);
void mt6797_rail_detach(struct mt6797_rail_coordinator *c,
			enum mt6797_rail_id id);
/* Requirements must include live and intermediate ceiling OPPs for all users.
 * The caller owns clock/OPP admission until this operation completes.
 */
int mt6797_rail_prepare(struct mt6797_rail_coordinator *c, u64 generation,
		       const int floor[MT6797_RAIL_USERS]);
int mt6797_rail_vote(struct mt6797_rail_coordinator *c, u64 generation,
		    unsigned int user, int uv);
int mt6797_rail_replace(struct mt6797_rail_coordinator *c, u64 generation,
		       unsigned int user, int request,
		       const int required[MT6797_RAIL_USERS]);
#endif
