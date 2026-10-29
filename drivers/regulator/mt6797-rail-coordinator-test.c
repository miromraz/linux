// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include "mt6797-rail-coordinator.h"

struct rail_fixture {
	struct mt6797_rail_coordinator coordinator;
	struct kunit *test;
	int uv[2];
	int writes;
	int fail_at;
};

struct rail_endpoint {
	struct rail_fixture *fixture;
	int id;
};

static int rail_read(void *data)
{
	struct rail_endpoint *e = data;

	return e->fixture->uv[e->id];
}

static int rail_write(void *data, int uv)
{
	struct rail_endpoint *e = data;
	struct rail_fixture *f = e->fixture;
	int i;

	if (++f->writes == f->fail_at)
		return -EIO;
	f->uv[e->id] = uv;
	/* Assert every physical write, including intermediate conversion states. */
	KUNIT_EXPECT_LE(f->test, f->uv[0], f->uv[1]);
	KUNIT_EXPECT_LE(f->test, f->uv[1] - f->uv[0], 160000);
	KUNIT_EXPECT_LE(f->test, f->uv[1], 1200000);
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		KUNIT_EXPECT_GE(f->test, f->uv[0], f->coordinator.floor[i]);
	return 0;
}

static void rail_conversion(struct kunit *test)
{
	int p, s, order;
	const int floor[3] = { 1040000, 1040000, 1040000 };

	for (order = 0; order < 2; order++) {
		for (p = 1040000; p <= 1200000; p += 10000) {
			for (s = 1050000; s <= 1200000; s += 25000) {
				struct rail_fixture f = { .test = test, .uv = { p, s } };
				struct rail_endpoint e[2] = { { &f, 0 }, { &f, 1 } };
				struct mt6797_rail_io io[2] = {
					{ rail_read, rail_write, &e[0], 10000 },
					{ rail_read, rail_write, &e[1], 25000 },
				};
				int ret;

				mt6797_rail_init(&f.coordinator);
				KUNIT_ASSERT_EQ(test, mt6797_rail_attach(&f.coordinator,
					order, &io[order]), 0);
				KUNIT_EXPECT_EQ(test, mt6797_rail_prepare(&f.coordinator,
					f.coordinator.generation, floor), -EPROBE_DEFER);
				KUNIT_EXPECT_EQ(test, f.writes, 0);
				KUNIT_ASSERT_EQ(test, mt6797_rail_attach(&f.coordinator,
					order ^ 1, &io[order ^ 1]), 0);
				ret = mt6797_rail_prepare(&f.coordinator,
					f.coordinator.generation, floor);
				if (p > s || s - p > 160000) {
					KUNIT_EXPECT_EQ(test, ret, -ERANGE);
					KUNIT_EXPECT_EQ(test, f.writes, 0);
				} else {
					KUNIT_EXPECT_EQ(test, ret, 0);
					KUNIT_EXPECT_EQ(test, f.uv[1], 1200000);
					KUNIT_EXPECT_TRUE(test, f.coordinator.converted);
				}
				mutex_destroy(&f.coordinator.lock);
			}
		}
	}
}

static void rail_lifetime(struct kunit *test)
{
	struct rail_fixture f = { .test = test, .uv = { 1040000, 1100000 } };
	struct rail_endpoint e[2] = { { &f, 0 }, { &f, 1 } };
	struct mt6797_rail_io io[2] = {
		{ rail_read, rail_write, &e[0], 10000 },
		{ rail_read, rail_write, &e[1], 25000 },
	};
	const int floor[3] = { 1040000, 1040000, 1040000 };
	u64 generation;

	mt6797_rail_init(&f.coordinator);
	KUNIT_ASSERT_EQ(test, mt6797_rail_attach(&f.coordinator, 0, &io[0]), 0);
	KUNIT_ASSERT_EQ(test, mt6797_rail_attach(&f.coordinator, 1, &io[1]), 0);
	generation = f.coordinator.generation;
	KUNIT_ASSERT_EQ(test, mt6797_rail_prepare(&f.coordinator, generation, floor), 0);
	KUNIT_ASSERT_EQ(test, mt6797_rail_vote(&f.coordinator, generation, 2, 1111000), 0);
	KUNIT_EXPECT_EQ(test, f.uv[0], 1120000);
	KUNIT_EXPECT_EQ(test, f.coordinator.floor[2], 1111000);
	mt6797_rail_detach(&f.coordinator, 0);
	KUNIT_EXPECT_EQ(test, f.coordinator.floor[2], 1111000);
	KUNIT_EXPECT_EQ(test, mt6797_rail_vote(&f.coordinator, generation, 2, 1200000),
			-ESTALE);
	KUNIT_ASSERT_EQ(test, mt6797_rail_attach(&f.coordinator, 0, &io[0]), 0);
	generation = f.coordinator.generation;
	KUNIT_ASSERT_EQ(test, mt6797_rail_prepare(&f.coordinator, generation, floor), 0);
	f.fail_at = f.writes + 1;
	KUNIT_EXPECT_EQ(test, mt6797_rail_vote(&f.coordinator, generation, 2, 1200000),
			-EIO);
	KUNIT_EXPECT_EQ(test, f.coordinator.floor[2], 1111000);
	KUNIT_EXPECT_TRUE(test, f.coordinator.fault);
	KUNIT_EXPECT_EQ(test, mt6797_rail_prepare(&f.coordinator, generation, floor), -EIO);
	mutex_destroy(&f.coordinator.lock);
}

static struct kunit_case rail_cases[] = {
	KUNIT_CASE(rail_conversion),
	KUNIT_CASE(rail_lifetime),
	{}
};

static struct kunit_suite rail_suite = {
	.name = "mt6797-rail-coordinator",
	.test_cases = rail_cases,
};
kunit_test_suite(rail_suite);
MODULE_LICENSE("GPL");
