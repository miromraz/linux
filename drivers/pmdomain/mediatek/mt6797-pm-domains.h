/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6797_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6797_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mt6797-power.h>

/*
 * MT6797 power domain support
 */

#define MT6797_PWR_STATUS		0x0180
#define MT6797_PWR_STATUS_2ND		0x0184

#define MT6797_SPM_VDE_PWR_CON		0x0300
#define MT6797_SPM_VEN_PWR_CON		0x0304
#define MT6797_SPM_ISP_PWR_CON		0x0308
#define MT6797_SPM_DIS_PWR_CON		0x030c
#define MT6797_SPM_MJC_PWR_CON		0x0310
#define MT6797_SPM_AUDIO_PWR_CON	0x0314
#define MT6797_SPM_MFG_ASYNC_PWR_CON	0x0334
#define MT6797_SPM_MFG_PWR_CON		0x0338
#define MT6797_SPM_MFG_SRAM_CON		0x033c
#define MT6797_SPM_MFG_CORE0_PWR_CON	0x0340
#define MT6797_SPM_MFG_CORE1_PWR_CON	0x0344
#define MT6797_SPM_MFG_CORE2_PWR_CON	0x0348
#define MT6797_SPM_MFG_CORE3_PWR_CON	0x034c

/* INFRA_TOPAXI_PROTECTEN bits, per vendor clk-mt6797-pg.c */
#define MT6797_TOP_AXI_PROT_EN_MM	(BIT(1) | BIT(2))	/* DIS_PROT_MASK */
#define MT6797_TOP_AXI_PROT_EN_MFG	BIT(21)			/* MFG_PROT_MASK */

static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6797[] = {
	BUS_PROT_BLOCK_INFRA
};

static const struct scpsys_domain_data scpsys_domain_data_mt6797[] = {
	[MT6797_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = MT6797_SPM_VDE_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
	[MT6797_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = BIT(21),
		.ctl_offs = MT6797_SPM_VEN_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
	},
	[MT6797_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(5),
		.ctl_offs = MT6797_SPM_ISP_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(9, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
	},
	[MT6797_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = BIT(3),
		.ctl_offs = MT6797_SPM_DIS_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6797_TOP_AXI_PROT_EN_MM),
		},
	},
	[MT6797_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(24),
		.ctl_offs = MT6797_SPM_AUDIO_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
	},
	[MT6797_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(13),
		.ctl_offs = MT6797_SPM_MFG_ASYNC_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
	},
	[MT6797_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(12),
		.ctl_offs = MT6797_SPM_MFG_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		/* SRAM power-down control and ack live in a separate register */
		.sram_pdn_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_ack_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_pdn_bits = GENMASK(1, 0),
		.sram_pdn_ack_bits = GENMASK(17, 16),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6797_TOP_AXI_PROT_EN_MFG),
		},
	},
	[MT6797_POWER_DOMAIN_MFG_CORE0] = {
		.name = "mfg_core0",
		.sta_mask = BIT(11),
		.ctl_offs = MT6797_SPM_MFG_CORE0_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		/* SRAM power-down ack lives in the shared MFG SRAM register */
		.sram_ack_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(20),
	},
	[MT6797_POWER_DOMAIN_MFG_CORE1] = {
		.name = "mfg_core1",
		.sta_mask = BIT(10),
		.ctl_offs = MT6797_SPM_MFG_CORE1_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_ack_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(21),
	},
	[MT6797_POWER_DOMAIN_MFG_CORE2] = {
		.name = "mfg_core2",
		.sta_mask = BIT(9),
		.ctl_offs = MT6797_SPM_MFG_CORE2_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_ack_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(22),
	},
	[MT6797_POWER_DOMAIN_MFG_CORE3] = {
		.name = "mfg_core3",
		.sta_mask = BIT(8),
		.ctl_offs = MT6797_SPM_MFG_CORE3_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_ack_offs = MT6797_SPM_MFG_SRAM_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(23),
	},
	[MT6797_POWER_DOMAIN_MJC] = {
		.name = "mjc",
		.sta_mask = BIT(20),
		.ctl_offs = MT6797_SPM_MJC_PWR_CON,
		.pwr_sta_offs = MT6797_PWR_STATUS,
		.pwr_sta2nd_offs = MT6797_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
	},
};

static const struct scpsys_soc_data mt6797_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6797,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6797),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6797,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6797),
};

#endif /* __SOC_MEDIATEK_MT6797_PM_DOMAINS_H */
