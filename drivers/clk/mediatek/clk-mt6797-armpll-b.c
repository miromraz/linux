// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 Cortex-A72 ("big") cluster ARMPLL clock
 *
 * Unlike the Cortex-A53 cluster PLLs (clk-mt6797-mcu.c), the A72 cluster PLL
 * is not in the MCUMIXED register map: it lives inside the iDVFS block and is
 * reachable only through secure-firmware SMC calls. The MT6797 ships the
 * vendor's closed ATF, which this port cannot replace, so the PLL is driven
 * exactly as the vendor 3.18 BSP does, through the iDVFS SIP calls
 * (drivers/misc/mediatek/base/power/mt6797/mt_idvfs.{c,h}: BigiDVFSPllSetFreq
 * -> SMC 0xC20003B8, BigiDVFSPllGetFreq -> SMC 0xC20003BA, both in MHz).
 * Exposing it as a clock lets the generic mediatek-cpufreq driver run A72
 * DVFS with the same flow it uses for the A53 clusters.
 *
 * Two clocks are registered:
 *   index 0 "armpll_b": the SMC-backed PLL; .set_rate/.recalc_rate call the
 *                       SIP. The PLL is powered and relocked glitchlessly by
 *                       firmware, so it has no Linux gate (always reported
 *                       enabled) and no parent.
 *   index 1 "armpll_b_mux" (the cpu clock): a mux of {clk26m, armpll_b}.
 *                       mediatek-cpufreq parks the CPU clock on the
 *                       intermediate (clk26m) while it reprograms the PLL.
 *                       The real A72 clock mux is firmware-internal and the
 *                       SMC relock is glitchless, so this mux only needs to
 *                       track the selected parent in software.
 */

#include <linux/arm-smccc.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* iDVFS SIP calls, SMC64 IDs (vendor mt_idvfs.h). Freq argument is in MHz. */
#define MTK_SIP_IDVFS_PLLSETFREQ	0xC20003B8UL
#define MTK_SIP_IDVFS_PLLGETFREQ	0xC20003BAUL

/* Vendor BigiDVFSPllSetFreq accepts 250..3000 MHz; the OPP tables stay well
 * inside this. Keep a sane boot default for recalc before the first set.
 */
#define MT6797_ARMPLL_B_FMIN_MHZ	250U
#define MT6797_ARMPLL_B_FMAX_MHZ	3000U
/*
 * The rate the firmware power-on leaves the PLL at (vendor logs report the
 * cluster comes up at ~845 MHz, which is also an OPP), reported until cpufreq
 * programs the first setpoint.
 */
#define MT6797_ARMPLL_B_DEFAULT_HZ	845000000UL

/* Provider cells. */
#define CLK_ARMPLL_B		0
#define CLK_ARMPLL_B_MUX	1
#define CLK_ARMPLL_B_NR		2

struct mt6797_armpll_b {
	struct clk_hw pll_hw;
	struct clk_hw mux_hw;
	unsigned long rate;	/* last rate programmed through the SIP */
	u8 mux_index;
};

static struct mt6797_armpll_b *pll_to_data(struct clk_hw *hw)
{
	return container_of(hw, struct mt6797_armpll_b, pll_hw);
}

static struct mt6797_armpll_b *mux_to_data(struct clk_hw *hw)
{
	return container_of(hw, struct mt6797_armpll_b, mux_hw);
}

static unsigned long mt6797_armpll_b_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	/*
	 * BigiDVFSPllGetFreq is only valid while the cluster is powered, and
	 * returns 0 otherwise; it is not safe to poll the iDVFS block when the
	 * A72 cores are off. The cached value mirrors the firmware state (the
	 * only way the rate changes under Linux is .set_rate), so report it.
	 */
	return pll_to_data(hw)->rate;
}

static int mt6797_armpll_b_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	unsigned int mhz = DIV_ROUND_CLOSEST_ULL(req->rate, 1000000);

	mhz = clamp(mhz, MT6797_ARMPLL_B_FMIN_MHZ, MT6797_ARMPLL_B_FMAX_MHZ);
	req->rate = (unsigned long)mhz * 1000000UL;

	return 0;
}

static int mt6797_armpll_b_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	struct mt6797_armpll_b *data = pll_to_data(hw);
	unsigned int mhz = DIV_ROUND_CLOSEST_ULL(rate, 1000000);
	struct arm_smccc_res res;

	mhz = clamp(mhz, MT6797_ARMPLL_B_FMIN_MHZ, MT6797_ARMPLL_B_FMAX_MHZ);

	arm_smccc_smc(MTK_SIP_IDVFS_PLLSETFREQ, mhz, 0, 0, 0, 0, 0, 0, &res);
	if ((long)res.a0 < 0)
		return -EIO;

	data->rate = (unsigned long)mhz * 1000000UL;

	return 0;
}

/* Firmware owns the gate; the clock is always usable once the cluster is on. */
static const struct clk_ops mt6797_armpll_b_ops = {
	.recalc_rate	= mt6797_armpll_b_recalc_rate,
	.determine_rate	= mt6797_armpll_b_determine_rate,
	.set_rate	= mt6797_armpll_b_set_rate,
};

static u8 mt6797_armpll_b_mux_get_parent(struct clk_hw *hw)
{
	return mux_to_data(hw)->mux_index;
}

static int mt6797_armpll_b_mux_set_parent(struct clk_hw *hw, u8 index)
{
	mux_to_data(hw)->mux_index = index;

	return 0;
}

/* Software-only mux: no register, so it never forces a reparent for a rate. */
static const struct clk_ops mt6797_armpll_b_mux_ops = {
	.get_parent		= mt6797_armpll_b_mux_get_parent,
	.set_parent		= mt6797_armpll_b_mux_set_parent,
	.determine_rate		= clk_hw_determine_rate_no_reparent,
};

static int mt6797_armpll_b_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clk_data;
	struct mt6797_armpll_b *data;
	int ret;
	const struct clk_init_data pll_init = {
		.name = "armpll_b",
		.ops = &mt6797_armpll_b_ops,
		.num_parents = 0,
	};
	/* Index 0 clk26m is the "intermediate", index 1 the PLL (default). */
	const char * const mux_parents[] = { "clk26m", "armpll_b" };
	const struct clk_init_data mux_init = {
		.name = "armpll_b_mux",
		.ops = &mt6797_armpll_b_mux_ops,
		.parent_names = mux_parents,
		.num_parents = ARRAY_SIZE(mux_parents),
		/* A live cpufreq clock: do not let it idle off. */
		.flags = CLK_IS_CRITICAL,
	};

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, CLK_ARMPLL_B_NR),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	data->rate = MT6797_ARMPLL_B_DEFAULT_HZ;
	data->mux_index = 1;

	data->pll_hw.init = &pll_init;
	ret = devm_clk_hw_register(dev, &data->pll_hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register armpll_b\n");

	data->mux_hw.init = &mux_init;
	ret = devm_clk_hw_register(dev, &data->mux_hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register armpll_b mux\n");

	clk_data->num = CLK_ARMPLL_B_NR;
	clk_data->hws[CLK_ARMPLL_B] = &data->pll_hw;
	clk_data->hws[CLK_ARMPLL_B_MUX] = &data->mux_hw;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static const struct of_device_id mt6797_armpll_b_of_match[] = {
	{ .compatible = "mediatek,mt6797-armpll-b" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6797_armpll_b_of_match);

static struct platform_driver mt6797_armpll_b_driver = {
	.probe = mt6797_armpll_b_probe,
	.driver = {
		.name = "clk-mt6797-armpll-b",
		.of_match_table = mt6797_armpll_b_of_match,
		/* Backs a live cpufreq policy; do not allow unbind. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_armpll_b_driver);

MODULE_DESCRIPTION("MediaTek MT6797 Cortex-A72 SMC-backed ARMPLL clock");
MODULE_LICENSE("GPL");
