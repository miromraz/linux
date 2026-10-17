// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 MCUMIXED clock driver
 *
 * The MCUMIXED block provides the per-cluster CPU PLLs and the muxes that
 * select each cluster's clock source. The Cortex-A53 little (LL) and big (L)
 * clusters and the CCI use the register-mapped ARMPLLs and ARMPLLDIV muxes
 * directly.
 *
 * The Cortex-A72 ("big") cluster ARMPLL is also exposed here even though it
 * is not in the MCUMIXED register map: it lives inside the iDVFS block and is
 * reachable only through secure-firmware SMC calls (the MT6797 ships the
 * vendor's closed ATF, which this port cannot replace, so the PLL is driven
 * exactly as the vendor 3.18 BSP does through the iDVFS SIP calls). Its CPU
 * clock mux, by contrast, is the B-cluster field of the same shared ARMPLLDIV
 * register (0x270) this block owns, so it is a normal MCUMIXED cpumux. Keeping
 * both in one driver lets the shared register's regmap serialise the A72 mux
 * switch with the A53 muxes, and lets the generic mediatek-cpufreq driver run
 * A72 DVFS with the flow it already uses for the A53 clusters.
 *
 * Register access does not take the vendor MCUMIXED hardware semaphore at
 * 0x11015440 (shared with ATF and SPM). That is safe while there is no SPM
 * idle/suspend path; it must be revisited before one is enabled.
 */

#include <dt-bindings/clock/mt6797-clk.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk-cpumux.h"
#include "clk-mtk.h"
#include "clk-pll.h"

/* Highest Cortex-A53 cluster frequency the vendor programs (L cluster, SB). */
#define MT6797_MCU_PLL_FMAX	(2002UL * MHZ)
/*
 * Keep the VCO at or above 1 GHz so mtk_pll_calc_values() never selects a
 * post divider larger than /2; the vendor reaches lower frequencies with the
 * (unmodelled) ARMPLLDIV_CKDIV divider instead, so the OPP tables stop above
 * the point where /4 would be needed.
 */
#define MT6797_MCU_PLL_FMIN	(1000UL * MHZ)

/* CON1 bit 31 is the PLL "change" bit. */
#define MT6797_MCU_PCW_CHG_BIT	31

/*
 * Program the post divider, pcw and the change bit of an ARMPLL in a single
 * write. The generic mtk_pll_set_rate_regs() writes the new pcw and then does
 * readl(CON1) | CHG; on MCUMIXED that read can return the stale value (the
 * vendor wraps every 0x1001Axxx write with ndelay(200), see
 * drivers/misc/.../freqhopping/mt6797/mt_freqhopping.c), which would write the
 * old pcw back with CHG set and leave the PLL at its previous rate while
 * cpufreq lowers the voltage. A single write avoids the read-after-write,
 * exactly as the vendor adjust_armpll_dds() does. The read-modify-write
 * preserves CON1 bit 30, an undocumented bit the vendor also keeps.
 */
static int mt6797_mcu_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct mtk_clk_pll *pll = to_mtk_clk_pll(hw);
	u32 pcw = 0, postdiv, con1;

	mtk_pll_calc_values(pll, &pcw, &postdiv, rate, parent_rate);

	/*
	 * cpufreq parks the cluster on clk26m (a cpumux regmap write to 0x270)
	 * right before this; honour the MCUMIXED "first read after write" delay
	 * before reading CON1, as the vendor reg_write() does.
	 */
	ndelay(200);
	con1 = readl(pll->pcw_addr);
	con1 &= ~(POSTDIV_MASK << pll->data->pd_shift);
	con1 |= (ffs(postdiv) - 1) << pll->data->pd_shift;
	con1 &= ~GENMASK(pll->data->pcw_shift + pll->data->pcwbits - 1,
			 pll->data->pcw_shift);
	con1 |= pcw << pll->data->pcw_shift;
	con1 |= BIT(MT6797_MCU_PCW_CHG_BIT);
	writel(con1, pll->pcw_addr);

	udelay(20);

	return 0;
}

static const struct clk_ops mt6797_mcu_pll_ops = {
	.is_prepared	= mtk_pll_is_prepared,
	.prepare	= mtk_pll_prepare,
	.unprepare	= mtk_pll_unprepare,
	.recalc_rate	= mtk_pll_recalc_rate,
	.determine_rate	= mtk_pll_determine_rate,
	.set_rate	= mt6797_mcu_pll_set_rate,
};

#define PLL(_id, _name, _reg, _pwr_reg, _pd_reg, _pcw_reg) {	\
		.id = _id,					\
		.name = _name,					\
		.reg = _reg,					\
		.pwr_reg = _pwr_reg,				\
		.en_mask = 0,					\
		.flags = PLL_AO,				\
		.ops = &mt6797_mcu_pll_ops,			\
		.fmin = MT6797_MCU_PLL_FMIN,			\
		.fmax = MT6797_MCU_PLL_FMAX,			\
		.pcwbits = 21,					\
		.pd_reg = _pd_reg,				\
		.pd_shift = 24,					\
		.tuner_reg = 0,					\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = 0,					\
	}

/*
 * CON0 is at the base offset, CON1 (holding both the post-divider and the
 * pcw) at +4, and PWR_CON0 at +0xc, matching the vendor register map
 * (ARMCAXPLL{0,1,2}_CON1 = 0x204/0x214/0x224).
 */
static const struct mtk_pll_data mcu_plls[] = {
	PLL(CLK_MCU_ARMPLL_LL, "armpll_ll", 0x200, 0x20c, 0x204, 0x204),
	PLL(CLK_MCU_ARMPLL_L, "armpll_l", 0x210, 0x21c, 0x214, 0x214),
	PLL(CLK_MCU_CCIPLL, "ccipll", 0x220, 0x22c, 0x224, 0x224),
};

/*
 * Cortex-A72 ("big") cluster ARMPLL, driven through the iDVFS SIP calls
 * (vendor mt_idvfs.h: BigiDVFSPllSetFreq -> SMC 0xC20003B8, BigiDVFSPllGetFreq
 * -> SMC 0xC20003BA, both taking/returning the frequency in MHz). The PLL is
 * powered and relocked by firmware, so it has no Linux gate and no parent;
 * cpufreq parks the cluster on clk26m (through the ARMPLLDIV mux below) while
 * the SMC relocks it, exactly as the A53 clusters do on this register.
 */
#define MTK_SIP_IDVFS_PLLSETFREQ	0xC20003B8UL
#define MTK_SIP_IDVFS_PLLGETFREQ	0xC20003BAUL

/* Vendor BigiDVFSPllSetFreq accepts 250..3000 MHz; the OPP tables stay well
 * inside this range.
 */
#define MT6797_ARMPLL_B_FMIN_MHZ	250U
#define MT6797_ARMPLL_B_FMAX_MHZ	3000U

static unsigned long mt6797_armpll_b_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct arm_smccc_res res;
	s32 mhz;

	/*
	 * Read the live rate from firmware rather than caching one. There is
	 * no reliable value to report before the cluster is powered, and a
	 * board only wires this clock to a CPU once its firmware has powered
	 * the cluster, so any consumer that reaches here sees a live cluster.
	 * BigiDVFSPllGetFreq returns the frequency in MHz, or 0 / a negative
	 * status when the cluster is off; report 0 for anything out of range
	 * instead of a fabricated rate.
	 */
	arm_smccc_smc(MTK_SIP_IDVFS_PLLGETFREQ, 0, 0, 0, 0, 0, 0, 0, &res);
	mhz = (s32)res.a0;
	if (mhz < (s32)MT6797_ARMPLL_B_FMIN_MHZ ||
	    mhz > (s32)MT6797_ARMPLL_B_FMAX_MHZ)
		return 0;

	return (unsigned long)mhz * 1000000UL;
}

static int mt6797_armpll_b_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	unsigned long rate = clamp(req->rate, req->min_rate, req->max_rate);
	unsigned int mhz = DIV_ROUND_CLOSEST_ULL(rate, 1000000);

	/*
	 * The SIP programs an integer number of MHz. Round the request to the
	 * nearest MHz within the caller's [min_rate, max_rate] window and the
	 * PLL's own frequency range.
	 */
	mhz = clamp(mhz, MT6797_ARMPLL_B_FMIN_MHZ, MT6797_ARMPLL_B_FMAX_MHZ);
	req->rate = (unsigned long)mhz * 1000000UL;

	return 0;
}

static int mt6797_armpll_b_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	unsigned int mhz = DIV_ROUND_CLOSEST_ULL(rate, 1000000);
	struct arm_smccc_res res;

	mhz = clamp(mhz, MT6797_ARMPLL_B_FMIN_MHZ, MT6797_ARMPLL_B_FMAX_MHZ);

	arm_smccc_smc(MTK_SIP_IDVFS_PLLSETFREQ, mhz, 0, 0, 0, 0, 0, 0, &res);
	/*
	 * The iDVFS SIP returns an SMCCC-style 32-bit status (the vendor casts
	 * the result to int). Any negative value -- NOT_SUPPORTED, INVALID_
	 * PARAMETER or a firmware error -- means the rate was not programmed.
	 */
	if ((s32)res.a0 < 0)
		return -EIO;

	return 0;
}

static const struct clk_ops mt6797_armpll_b_ops = {
	.recalc_rate	= mt6797_armpll_b_recalc_rate,
	.determine_rate	= mt6797_armpll_b_determine_rate,
	.set_rate	= mt6797_armpll_b_set_rate,
};

static int mt6797_register_armpll_b(struct device *dev,
				    struct clk_hw_onecell_data *clk_data)
{
	static const struct clk_init_data init = {
		.name		= "armpll_b",
		.ops		= &mt6797_armpll_b_ops,
		.num_parents	= 0,
	};
	struct clk_hw *hw;
	int ret;

	/*
	 * No SMC is issued at probe: the cluster may still be off here (it is
	 * powered by the board/bootloader, not by this driver), so there is no
	 * meaningful rate to read yet. .recalc_rate reads the live rate when a
	 * consumer first needs it.
	 */
	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;

	hw->init = &init;
	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	clk_data->hws[CLK_MCU_ARMPLL_B] = hw;

	return 0;
}

/*
 * Each ARMPLLDIV mux can select clk26m, its own ARMPLL, mainpll or univpll,
 * but the mainpll/univpll inputs are only valid with a TOPCKGEN alternate
 * clock this driver does not model, so for the Cortex-A53 and CCI muxes only
 * clk26m and the cluster's ARMPLL are exposed (the A72 mux lists all four,
 * see its comment). cpufreq parks the cluster on clk26m while the ARMPLL
 * relocks.
 */
static const char * const armpll_ll_parents[] = { "clk26m", "armpll_ll" };
static const char * const armpll_l_parents[] = { "clk26m", "armpll_l" };
static const char * const ccipll_parents[] = { "clk26m", "ccipll" };

/*
 * The A72 mux is read back by the generic cpumux .get_parent, which returns
 * the raw 2-bit ARMPLLDIV field. The bootloader can leave that field at any
 * of the four hardware selector values, so all four are listed -- clk26m,
 * the A72 ARMPLL, mainpll and univpll, encoded 0..3 as the vendor ARMPLLDIV
 * semantics define (vendor mt_cpufreq.c TOP_CKMUXSEL_{CLKSQ,ARMPLL,MAINPLL,
 * UNIVPLL}) -- so .get_parent never indexes out of range. The cpufreq flow
 * only ever selects clk26m and armpll_b; mainpll/univpll additionally need a
 * TOPCKGEN alternate this driver does not program, as for the A53 muxes, and
 * so are never selected here.
 */
static const char * const armpll_b_parents[] = {
	"clk26m", "armpll_b", "mainpll", "univpll"
};

/* ARMPLLDIV_MUXSEL (0x270): B = [1:0], LL = [3:2], L = [5:4], CCI = [7:6] */
static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_MCU_ARMPLLDIV_LL, "armpll_ll_sel", armpll_ll_parents, 0x270, 2, 2),
	MUX(CLK_MCU_ARMPLLDIV_L, "armpll_l_sel", armpll_l_parents, 0x270, 4, 2),
	MUX(CLK_MCU_ARMPLLDIV_CCI, "ccipll_sel", ccipll_parents, 0x270, 6, 2),
	MUX(CLK_MCU_ARMPLLDIV_B, "armpll_b_sel", armpll_b_parents, 0x270, 0, 2),
};

#define MT6797_MCU_NR_CLK	(CLK_MCU_ARMPLLDIV_B + 1)

static int clk_mt6797_mcu_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int ret;

	clk_data = mtk_alloc_clk_data(MT6797_MCU_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_clk_register_plls(&pdev->dev, mcu_plls, ARRAY_SIZE(mcu_plls),
				    clk_data);
	if (ret)
		goto free_clk_data;

	/* Register "armpll_b" before the cpumuxes: the B mux is its consumer. */
	ret = mt6797_register_armpll_b(&pdev->dev, clk_data);
	if (ret)
		goto unregister_plls;

	ret = mtk_clk_register_cpumuxes(&pdev->dev, node, cpu_muxes,
					ARRAY_SIZE(cpu_muxes), clk_data);
	if (ret)
		goto unregister_plls;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto unregister_cpumuxes;

	platform_set_drvdata(pdev, clk_data);

	return 0;

unregister_cpumuxes:
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
unregister_plls:
	mtk_clk_unregister_plls(mcu_plls, ARRAY_SIZE(mcu_plls), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);

	return ret;
}

static void clk_mt6797_mcu_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
	mtk_clk_unregister_plls(mcu_plls, ARRAY_SIZE(mcu_plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt6797_mcu[] = {
	{ .compatible = "mediatek,mt6797-mcumixed" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6797_mcu);

static struct platform_driver clk_mt6797_mcu_drv = {
	.probe = clk_mt6797_mcu_probe,
	.remove = clk_mt6797_mcu_remove,
	.driver = {
		.name = "clk-mt6797-mcu",
		.of_match_table = of_match_clk_mt6797_mcu,
		/* These clocks back a live cpufreq policy; do not allow unbind. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(clk_mt6797_mcu_drv);

MODULE_DESCRIPTION("MediaTek MT6797 MCUMIXED clock driver");
MODULE_LICENSE("GPL");
