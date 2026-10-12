// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6797 Cortex-A72 ("big"/CL2) cluster power-on sequencer
 *
 * Plain PSCI CPU_ON of the two Cortex-A72 cores (cpu@200/201) hangs the SoC.
 * On a stock PSCI/TF-A that is a firmware bug: EL3 is supposed to power a CPU
 * up before releasing it. The MT6797 vendor 3.18 BSP works around its own thin
 * PSCI by running a pre-CPU_ON power sequence from the kernel
 * (arch/arm64/kernel/psci.c cpu_power_on_buck()). This phone ships the vendor's
 * closed ATF, which this port cannot replace, so the same sequence has to live
 * on the Linux side until it can be moved into TF-A's pwr_domain_on(), which is
 * where it belongs upstream.
 *
 * The sequence (vendor cpu_power_on_buck(), register details confirmed against
 * the MT6797 live-resource map) runs once, when the first Cortex-A72 core is
 * brought online, from a CPU-hotplug prepare callback on the control CPU:
 *
 *   1. SPM 0x218 |= BIT(0)                       (CL2 power request latch)
 *   2. dummy read of the iDVFS PLL control word  (0x102224a0)
 *   3. assert the TOPRGU PWRAP-SPI-CTL reset      (latch, swsysrst bit 11)
 *   4. enable DA9214 buck B (vproc-big) and wait 1 ms
 *   5. SPM 0x290 &= ~0x3                          (clear B external buck iso)
 *   6. deassert the PWRAP-SPI-CTL reset           (unlatch)
 *   7. udelay(240); set the B SRAM LDO to 1.10 V via the iDVFS SIP; udelay(240)
 *   8. pre-configure MP2 synchronous DCM          (0x10222274 bits 6:0)
 *
 * PSCI CPU_ON then releases the core. The vendor runs step 8 after CPU_ON, but
 * it is static clock-domain configuration in the always-on MCUSYS window, so it
 * is pre-set here to keep the whole sequence in one prepare callback.
 *
 * Powering the cluster off (vendor cpu_power_off_buck()) is deliberately not
 * implemented: the vendor warns TF-A CPU_OFF of the last Cortex-A72 can hang,
 * so there is no teardown callback and the cores are expected to stay online
 * for the lifetime of the boot.
 */

#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

/* iDVFS SIP call, SMC64 ID (vendor mt_idvfs.h). Argument is mV * 100. */
#define MTK_SIP_IDVFS_SRAMLDOSET	0xC20003BFUL
#define MT6797_CL2_SRAM_LDO_UV		1100000

/* SPM (syscon@10006000) offsets. */
#define SPM_CL2_PWR_CON			0x218
#define SPM_CL2_PWR_REQ			BIT(0)
#define SPM_EXT_BUCK_ISO		0x290
#define SPM_EXT_BUCK_ISO_B		GENMASK(1, 0)

/* MCUCFG2 / iDVFS window (reg[0], base 0x10222000). */
#define IDVFS_PLL_CON			0x4a0	/* dummy read, part of the latch */
#define MCUCFG_SYNC_DCM_MP2		0x274
#define MCUCFG_SYNC_DCM_MP2_MASK	GENMASK(6, 0)
#define MCUCFG_SYNC_DCM_MP2_ON		BIT(0)
#define MCUCFG_SYNC_DCM_MP2_TOG		BIT(1)
#define MCUCFG_SYNC_DCM_MP2_DIV_SEL4	(0x3 << 2)

#define MT6797_CL2_FIRST_CPU		8	/* cpu@200 */
#define MT6797_CL2_LAST_CPU		9	/* cpu@201 */

struct mt6797_cl2 {
	struct device *dev;
	struct regmap *spm;
	void __iomem *mcucfg;
	struct reset_control *pwrap_rst;
	struct regulator *vproc;
	enum cpuhp_state hp_state;
	bool powered;
};

static struct mt6797_cl2 *mt6797_cl2;

static int mt6797_cl2_power_on(struct mt6797_cl2 *cl2)
{
	u32 val;
	int ret;

	/* 1. request CL2 power. */
	regmap_update_bits(cl2->spm, SPM_CL2_PWR_CON,
			   SPM_CL2_PWR_REQ, SPM_CL2_PWR_REQ);

	/* 2. dummy read of the iDVFS PLL control word. */
	readl(cl2->mcucfg + IDVFS_PLL_CON);

	/* 3. latch the PWRAP SPI controller reset. */
	reset_control_assert(cl2->pwrap_rst);

	/* 4. bring up buck B (vproc-big) and let it settle. */
	ret = regulator_enable(cl2->vproc);
	if (ret) {
		reset_control_deassert(cl2->pwrap_rst);
		return dev_err_probe(cl2->dev, ret, "failed to enable vproc-big\n");
	}
	usleep_range(1000, 1500);

	/* 5. release the Cortex-A72 external-buck isolation. */
	regmap_update_bits(cl2->spm, SPM_EXT_BUCK_ISO, SPM_EXT_BUCK_ISO_B, 0);

	/* 6. unlatch the PWRAP SPI controller reset. */
	reset_control_deassert(cl2->pwrap_rst);

	/* 7. raise the B SRAM LDO to 1.10 V through the iDVFS firmware. */
	usleep_range(240, 300);
	{
		struct arm_smccc_res res;

		arm_smccc_smc(MTK_SIP_IDVFS_SRAMLDOSET,
			      MT6797_CL2_SRAM_LDO_UV / 10, 0, 0, 0, 0, 0, 0,
			      &res);
		if ((long)res.a0 < 0) {
			regulator_disable(cl2->vproc);
			return dev_err_probe(cl2->dev, -EIO,
					     "iDVFS SRAM LDO set failed\n");
		}
	}
	usleep_range(240, 300);

	/* 8. pre-configure the MP2 synchronous DCM (two-step toggle). */
	val = readl(cl2->mcucfg + MCUCFG_SYNC_DCM_MP2) & ~MCUCFG_SYNC_DCM_MP2_MASK;
	val |= MCUCFG_SYNC_DCM_MP2_ON | MCUCFG_SYNC_DCM_MP2_DIV_SEL4;
	writel(val | MCUCFG_SYNC_DCM_MP2_TOG, cl2->mcucfg + MCUCFG_SYNC_DCM_MP2);
	writel(val, cl2->mcucfg + MCUCFG_SYNC_DCM_MP2);

	return 0;
}

static int mt6797_cl2_cpu_prepare(unsigned int cpu)
{
	struct mt6797_cl2 *cl2 = mt6797_cl2;

	if (!cl2 || cpu < MT6797_CL2_FIRST_CPU || cpu > MT6797_CL2_LAST_CPU)
		return 0;

	/* Power the cluster only for its first core; the second just joins. */
	if (cl2->powered)
		return 0;

	if (mt6797_cl2_power_on(cl2))
		return -EIO;

	cl2->powered = true;

	return 0;
}

static int mt6797_cl2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_cl2 *cl2;
	int ret;

	cl2 = devm_kzalloc(dev, sizeof(*cl2), GFP_KERNEL);
	if (!cl2)
		return -ENOMEM;

	cl2->dev = dev;

	cl2->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(cl2->spm))
		return dev_err_probe(dev, PTR_ERR(cl2->spm),
				     "failed to get SPM syscon\n");

	cl2->mcucfg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cl2->mcucfg))
		return dev_err_probe(dev, PTR_ERR(cl2->mcucfg),
				     "failed to map MCUCFG2\n");

	cl2->pwrap_rst = devm_reset_control_get_exclusive(dev, "pwrap");
	if (IS_ERR(cl2->pwrap_rst))
		return dev_err_probe(dev, PTR_ERR(cl2->pwrap_rst),
				     "failed to get PWRAP reset\n");

	cl2->vproc = devm_regulator_get(dev, "vproc");
	if (IS_ERR(cl2->vproc))
		return dev_err_probe(dev, PTR_ERR(cl2->vproc),
				     "failed to get vproc-big\n");

	mt6797_cl2 = cl2;

	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
					"soc/mt6797-cl2:prepare",
					mt6797_cl2_cpu_prepare, NULL);
	if (ret < 0) {
		mt6797_cl2 = NULL;
		return dev_err_probe(dev, ret, "failed to add cpuhp state\n");
	}
	cl2->hp_state = ret;

	return 0;
}

static const struct of_device_id mt6797_cl2_of_match[] = {
	{ .compatible = "mediatek,mt6797-a72-power" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6797_cl2_of_match);

static struct platform_driver mt6797_cl2_driver = {
	.probe = mt6797_cl2_probe,
	.driver = {
		.name = "mt6797-cpu-cl2",
		.of_match_table = mt6797_cl2_of_match,
		/* Owns a CPU-hotplug state for live cores; do not allow unbind. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_cl2_driver);

MODULE_DESCRIPTION("MediaTek MT6797 Cortex-A72 cluster power-on sequencer");
MODULE_LICENSE("GPL");
