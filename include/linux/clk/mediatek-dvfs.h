/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CLK_MEDIATEK_DVFS_H
#define _LINUX_CLK_MEDIATEK_DVFS_H

#include <linux/clk.h>
#include <linux/errno.h>

/*
 * The caller must own sufficient voltage for old, intermediate and target
 * rates before entry. This helper never restores a possibly faster old PLL,
 * nor releases that voltage ownership. CCF acceptance is the provider
 * contract, not independent electrical readback. Firmware refusal needs its
 * own provider contract and is deliberately outside this helper.
 */
static inline int mtk_dvfs_switch_clock(struct clk *mux, struct clk *pll,
				      struct clk *intermediate,
				      unsigned long target)
{
	long rounded;
	int ret;

	if (!pll || !target || clk_is_match(pll, intermediate))
		return -EINVAL;

	rounded = clk_round_rate(pll, target);
	if (rounded <= 0)
		return rounded ?: -EINVAL;
	if (rounded > target)
		return -ERANGE;

	ret = clk_set_parent(mux, intermediate);
	if (ret)
		return ret;
	if (!clk_is_match(clk_get_parent(mux), intermediate) ||
	    clk_get_rate(mux) != clk_get_rate(intermediate))
		return -EIO;

	ret = clk_set_rate(pll, target);
	if (ret)
		return ret;
	if (clk_get_rate(pll) != rounded)
		return -EIO;

	ret = clk_set_parent(mux, pll);
	if (ret)
		return ret;
	if (!clk_is_match(clk_get_parent(mux), pll) ||
	    clk_get_rate(mux) != rounded)
		return -EIO;

	return 0;
}
#endif
