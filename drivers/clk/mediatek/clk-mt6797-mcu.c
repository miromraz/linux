// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 MCUMIXED clock driver
 *
 * The MCUMIXED block provides the per-cluster CPU PLLs and the muxes that
 * select each cluster's clock source. Only the Cortex-A53 little (LL) and
 * big (L) clusters and the CCI are handled here; the Cortex-A72 cluster PLL
 * is controlled through secure firmware and is out of scope.
 *
 * Register access does not take the vendor MCUMIXED hardware semaphore at
 * 0x11015440 (shared with ATF and SPM). That is safe while there is no SPM
 * idle/suspend path and the A72 cluster stays off; it must be revisited
 * before either is enabled.
 */

#include <dt-bindings/clock/mt6797-clk.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

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
 * Each ARMPLLDIV mux can select clk26m, its own ARMPLL, mainpll or univpll,
 * but the mainpll/univpll inputs are only valid with a TOPCKGEN alternate
 * clock this driver does not model, so only clk26m and the cluster's ARMPLL
 * are exposed. cpufreq parks the cluster on clk26m while the ARMPLL relocks.
 */
static const char * const armpll_ll_parents[] = { "clk26m", "armpll_ll" };
static const char * const armpll_l_parents[] = { "clk26m", "armpll_l" };
static const char * const ccipll_parents[] = { "clk26m", "ccipll" };

/* ARMPLLDIV_MUXSEL (0x270): LL = [3:2], L = [5:4], CCI = [7:6] */
static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_MCU_ARMPLLDIV_LL, "armpll_ll_sel", armpll_ll_parents, 0x270, 2, 2),
	MUX(CLK_MCU_ARMPLLDIV_L, "armpll_l_sel", armpll_l_parents, 0x270, 4, 2),
	MUX(CLK_MCU_ARMPLLDIV_CCI, "ccipll_sel", ccipll_parents, 0x270, 6, 2),
};

#define MT6797_MCU_NR_CLK	(CLK_MCU_ARMPLLDIV_CCI + 1)

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
