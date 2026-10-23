// SPDX-License-Identifier: GPL-2.0
/*
 * camss-vfe-170.c
 *
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module v170
 *
 * Copyright (C) 2020-2021 Linaro Ltd.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <kunit/static_stub.h>

#include "camss.h"
#include "camss-vfe.h"

#define CREATE_TRACE_POINTS
#include "camss-vfe-17x-trace.h"

#define VFE_GLOBAL_RESET_CMD			(0x018)
#define		GLOBAL_RESET_CMD_CORE		BIT(0)
#define		GLOBAL_RESET_CMD_CAMIF		BIT(1)
#define		GLOBAL_RESET_CMD_BUS		BIT(2)
#define		GLOBAL_RESET_CMD_BUS_BDG	BIT(3)
#define		GLOBAL_RESET_CMD_REGISTER	BIT(4)
#define		GLOBAL_RESET_CMD_PM		BIT(5)
#define		GLOBAL_RESET_CMD_BUS_MISR	BIT(6)
#define		GLOBAL_RESET_CMD_TESTGEN	BIT(7)
#define		GLOBAL_RESET_CMD_DSP		BIT(8)
#define		GLOBAL_RESET_CMD_IDLE_CGC	BIT(9)
#define		GLOBAL_RESET_CMD_RDI0		BIT(10)
#define		GLOBAL_RESET_CMD_RDI1		BIT(11)
#define		GLOBAL_RESET_CMD_RDI2		BIT(12)
#define		GLOBAL_RESET_CMD_RDI3		BIT(13)
#define		GLOBAL_RESET_CMD_VFE_DOMAIN	BIT(30)
#define		GLOBAL_RESET_CMD_RESET_BYPASS	BIT(31)

#define VFE_MODULE_CGC_OVERRIDE(n)		(0x02c + (n) * 4) /* lens, stats, color, zoom, bus */
#define VFE_MODULE_LENS_EN			(0x040)
#define		LENS_EN_DEMUX			BIT(2)
#define		LENS_EN_DEMO			BIT(10)
#define VFE_MODULE_STATS_EN			(0x044)
#define VFE_MODULE_COLOR_EN			(0x048)
#define		COLOR_EN_RGB_LUT		BIT(3)
#define VFE_MODULE_ZOOM_EN			(0x04c)
#define		ZOOM_EN_CST			BIT(0)
#define		ZOOM_EN_SCALE_VID		BIT(3)
#define		ZOOM_EN_CROP_VID		BIT(4)

#define VFE_CORE_CFG				(0x050)
#define		CFG_PIXEL_PATTERN_RGRGRG	(0x0)
#define		CFG_PIXEL_PATTERN_GRGRGR	(0x1)
#define		CFG_PIXEL_PATTERN_BGBGBG	(0x2)
#define		CFG_PIXEL_PATTERN_GBGBGB	(0x3)
#define		CFG_PIXEL_PATTERN_YCBYCR	(0x4)
#define		CFG_PIXEL_PATTERN_YCRYCB	(0x5)
#define		CFG_PIXEL_PATTERN_CBYCRY	(0x6)
#define		CFG_PIXEL_PATTERN_CRYCBY	(0x7)
#define		CFG_COMPOSITE_REG_UPDATE_EN	BIT(4)

#define VFE_IRQ_CMD				(0x058)
#define		CMD_GLOBAL_CLEAR		BIT(0)

#define VFE_IRQ_MASK_0					(0x05c)
#define		MASK_0_CAMIF_SOF			BIT(0)
#define		MASK_0_CAMIF_EOF			BIT(1)
#define		MASK_0_RDI_REG_UPDATE(n)		BIT((n) + 5)
#define		MASK_0_IMAGE_MASTER_n_PING_PONG(n)	BIT((n) + 8)
#define		MASK_0_IMAGE_COMPOSITE_DONE_n(n)	BIT((n) + 25)
#define		MASK_0_RESET_ACK			BIT(31)

#define VFE_IRQ_MASK_1					(0x060)
#define		MASK_1_CAMIF_ERROR			BIT(0)
#define		MASK_1_VIOLATION			BIT(7)
#define		MASK_1_BUS_BDG_HALT_ACK			BIT(8)
#define		MASK_1_IMAGE_MASTER_n_BUS_OVERFLOW(n)	BIT((n) + 9)
#define		MASK_1_RDI_SOF(n)			BIT((n) + 29)

#define VFE_IRQ_CLEAR_0					(0x064)
#define VFE_IRQ_CLEAR_1					(0x068)

#define VFE_IRQ_STATUS_0				(0x06c)
#define		STATUS_0_CAMIF_SOF			BIT(0)
#define		STATUS_0_PIX_REG_UPDATE			BIT(4)
#define		STATUS_0_RDI_REG_UPDATE(n)		BIT((n) + 5)
#define		STATUS_0_IMAGE_MASTER_PING_PONG(n)	BIT((n) + 8)
#define		STATUS_0_IMAGE_COMPOSITE_DONE(n)	BIT((n) + 25)
#define		STATUS_0_RESET_ACK			BIT(31)

#define VFE_IRQ_STATUS_1				(0x070)
#define		STATUS_1_CAMIF_ERROR			BIT(0)
#define		STATUS_1_VIOLATION			BIT(7)
#define		STATUS_1_BUS_BDG_HALT_ACK		BIT(8)
#define		STATUS_1_RDI_SOF(n)			BIT((n) + 27)

#define VFE_VIOLATION_STATUS			(0x07c)

#define VFE_CAMIF_CMD				(0x478)
#define		CMD_CLEAR_CAMIF_STATUS		BIT(2)

#define VFE_CAMIF_CFG				(0x47c)
#define		CFG_VSYNC_SYNC_EDGE		(0)
#define			VSYNC_ACTIVE_HIGH	(0)
#define			VSYNC_ACTIVE_LOW	(1)
#define		CFG_HSYNC_SYNC_EDGE		(1)
#define			HSYNC_ACTIVE_HIGH	(0)
#define			HSYNC_ACTIVE_LOW	(1)
#define		CFG_VFE_SUBSAMPLE_ENABLE	BIT(4)
#define		CFG_BUS_SUBSAMPLE_ENABLE	BIT(5)
#define		CFG_VFE_OUTPUT_EN		BIT(6)
#define		CFG_BUS_OUTPUT_EN		BIT(7)
#define		CFG_BINNING_EN			BIT(9)
#define		CFG_FRAME_BASED_EN		BIT(10)
#define		CFG_RAW_CROP_EN			BIT(22)

#define VFE_CAMIF_SKIP_PERIOD			(0x490)
#define VFE_CAMIF_IRQ_SUBSAMPLE_PATTERN		(0x49c)
#define VFE_CAMIF_EPOCH_IRQ			(0x4a0)
#define VFE_CAMIF_RAW_CROP_WIDTH_CFG		(0xce4)
#define VFE_CAMIF_RAW_CROP_HEIGHT_CFG		(0xce8)

#define VFE_REG_UPDATE_CMD			(0x4ac)
#define		REG_UPDATE_PIX			BIT(0)
#define		REG_UPDATE_RDI(n)		BIT(1 + (n))

/* Titan 170 pixel pipeline, offsets from the stock cam_vfe170.h / titan170_ife.h */
#define VFE_DEMUX_CFG				(0x560)
#define VFE_DEMUX_GAIN_0			(0x564)
#define VFE_DEMUX_GAIN_1			(0x568)
#define VFE_DEMUX_RIGHT_GAIN_0			(0x56c)
#define VFE_DEMUX_RIGHT_GAIN_1			(0x570)
#define VFE_DEMUX_EVEN_CFG			(0x574)
#define VFE_DEMUX_ODD_CFG			(0x578)
#define		DEMUX_GAIN_UNITY		(0x400) /* Q10 */

#define VFE_DEMO_CFG				(0x6f8)
#define		DEMO_CFG_DYN_G_CLAMP_EN		BIT(8)
#define VFE_DEMO_WB_LEFT_CFG_0			(0x6fc) /* G [11:0], B [27:16] */
#define VFE_DEMO_WB_LEFT_CFG_1			(0x700) /* R [11:0] */
#define VFE_DEMO_WB_LEFT_OFFSET_CFG_0		(0x704)
#define VFE_DEMO_WB_LEFT_OFFSET_CFG_1		(0x708)
#define		WB_GAIN_UNITY			(0x80) /* U12Q7 */
#define VFE_DEMO_INTERP_COEFF_CFG		(0x71c)
#define VFE_DEMO_INTERP_CLASSIFIER_0		(0x720)

#define VFE_RGB_LUT_CFG				(0x798)

#define VFE_SCALE_VID_Y				(0xa3c)
#define VFE_SCALE_VID_CBCR			(0xa68)
#define		SCALE_CFG			(0x00)
#define			SCALE_CFG_H_MN_EN	BIT(0)
#define			SCALE_CFG_V_MN_EN	BIT(1)
#define		SCALE_H				(0x04)
#define		SCALE_V				(0x18)
/* per direction, from SCALE_H / SCALE_V */
#define		SCALE_IMAGE_SIZE		(0x00)
#define		SCALE_PHASE			(0x04)
#define			SCALE_PHASE_INTERP_RESO	28
#define		SCALE_STRIPE_0			(0x08)
#define		SCALE_STRIPE_1			(0x0c)
#define		SCALE_PAD			(0x10)
#define		SCALE_INTERP_SHIFT		14

#define VFE_DMI_CFG				(0xc24)
#define		DMI_CFG_AUTO_INC_EN		BIT(8)
#define		DMI_RAM_SEL_RGBLUT_CH0_BANK0	(0x1a)
#define		DMI_RAM_SEL_RGBLUT_CH1_BANK0	(0x1c)
#define		DMI_RAM_SEL_RGBLUT_CH2_BANK0	(0x1e)
#define VFE_DMI_ADDR				(0xc28)
#define VFE_DMI_DATA_LO				(0xc30)
#define		GAMMA_LUT_ENTRIES		64
#define		GAMMA_LUT_DELTA_SHIFT		10

#define VFE_FULL_OUT_Y				(0xe0c)
#define VFE_FULL_OUT_C				(0xe2c)
#define		OUT_CROP_RND_CLAMP_CFG		(0x00)
#define			OUT_CROP_EN		BIT(9)
#define			OUT_ROUND_EN		BIT(10)
#define			OUT_CLAMP_EN		BIT(11)
#define		OUT_CROP_LINE_CFG		(0x04)
#define		OUT_CROP_PIXEL_CFG		(0x08)
#define		OUT_CLAMP_CFG			(0x0c)
#define			OUT_CLAMP_MAX_SHIFT	20
#define		OUT_ROUNDING_CFG		(0x10)
#define			OUT_ROUND_INTERLEAVED	BIT(0)
#define			OUT_ROUND_PATTERN(n)	((n) << 1)
#define			OUT_ROUND_OFF_BITS(n)	((n) << 3)

#define VFE_COLOR_XFORM_CH(n)			(0xf30 + (n) * 0x10)
#define		CST_COEFF_CFG_0			(0x00)
#define		CST_COEFF_CFG_1			(0x04)
#define		CST_OFFSET_CFG			(0x08)
#define		CST_CLAMP_CFG			(0x0c)

#define VFE_BUS_COMP_MASK(n)		(0x2010 + (n) * 4)
#define VFE_BUS_IRQ_BANKS		3
#define VFE_BUS_IRQ_MASK(n)		(0x2044 + (n) * 4)
#define VFE_BUS_IRQ_CLEAR(n)		(0x2050 + (n) * 4)
#define VFE_BUS_IRQ_STATUS(n)		(0x205c + (n) * 4)
#define		STATUS0_COMP_RESET_DONE		BIT(0)
#define		STATUS0_COMP_REG_UPDATE0_DONE	BIT(1)
#define		STATUS0_COMP_REG_UPDATE1_DONE	BIT(2)
#define		STATUS0_COMP_REG_UPDATE2_DONE	BIT(3)
#define		STATUS0_COMP_REG_UPDATE3_DONE	BIT(4)
#define		STATUS0_COMP_REG_UPDATE_DONE(n)	BIT((n) + 1)
#define		STATUS0_COMP0_BUF_DONE		BIT(5)
#define		STATUS0_COMP1_BUF_DONE		BIT(6)
#define		STATUS0_COMP2_BUF_DONE		BIT(7)
#define		STATUS0_COMP3_BUF_DONE		BIT(8)
#define		STATUS0_COMP4_BUF_DONE		BIT(9)
#define		STATUS0_COMP5_BUF_DONE		BIT(10)
#define		STATUS0_COMP_BUF_DONE(n)	BIT((n) + 5)
#define		STATUS0_COMP_ERROR		BIT(11)
#define		STATUS0_COMP_OVERWRITE		BIT(12)
#define		STATUS0_OVERFLOW		BIT(13)
#define		STATUS0_VIOLATION		BIT(14)
/* WM_CLIENT_BUF_DONE defined for buffers 0:19 */
#define		STATUS1_WM_CLIENT_BUF_DONE(n)		BIT(n)
#define		STATUS1_EARLY_DONE			BIT(24)
#define		STATUS2_DUAL_COMP0_BUF_DONE		BIT(0)
#define		STATUS2_DUAL_COMP1_BUF_DONE		BIT(1)
#define		STATUS2_DUAL_COMP2_BUF_DONE		BIT(2)
#define		STATUS2_DUAL_COMP3_BUF_DONE		BIT(3)
#define		STATUS2_DUAL_COMP4_BUF_DONE		BIT(4)
#define		STATUS2_DUAL_COMP5_BUF_DONE		BIT(5)
#define		STATUS2_DUAL_COMP_BUF_DONE(n)		BIT(n)
#define		STATUS2_DUAL_COMP_ERROR			BIT(6)
#define		STATUS2_DUAL_COMP_OVERWRITE		BIT(7)

#define VFE_BUS_IRQ_CLEAR_GLOBAL		(0x2068)

#define VFE_BUS_WM_DEBUG_STATUS_CFG		(0x226c)
#define		DEBUG_STATUS_CFG_STATUS0(n)	BIT(n)
#define		DEBUG_STATUS_CFG_STATUS1(n)	BIT(8 + (n))

#define VFE_BUS_WM_ADDR_SYNC_FRAME_HEADER	(0x2080)

#define VFE_BUS_WM_ADDR_SYNC_NO_SYNC		(0x2084)
#define		BUS_VER2_MAX_CLIENTS (24)
#define		WM_ADDR_NO_SYNC_DEFAULT_VAL \
				((1 << BUS_VER2_MAX_CLIENTS) - 1)

#define VFE_BUS_WM_CGC_OVERRIDE			(0x200c)
#define		WM_CGC_OVERRIDE_ALL		(0xFFFFF)

#define VFE_BUS_WM_TEST_BUS_CTRL		(0x211c)

#define VFE_BUS_WM_STATUS0(n)			(0x2200 + (n) * 0x100)
#define VFE_BUS_WM_STATUS1(n)			(0x2204 + (n) * 0x100)
#define VFE_BUS_WM_CFG(n)			(0x2208 + (n) * 0x100)
#define		WM_CFG_EN			(0)
#define		WM_CFG_MODE			(1)
#define			MODE_QCOM_PLAIN	(0)
#define			MODE_MIPI_RAW	(1)
#define		WM_CFG_VIRTUALFRAME		(2)
#define VFE_BUS_WM_HEADER_ADDR(n)		(0x220c + (n) * 0x100)
#define VFE_BUS_WM_HEADER_CFG(n)		(0x2210 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_ADDR(n)		(0x2214 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_ADDR_OFFSET(n)		(0x2218 + (n) * 0x100)
#define VFE_BUS_WM_BUFFER_WIDTH_CFG(n)		(0x221c + (n) * 0x100)
#define		WM_BUFFER_DEFAULT_WIDTH		(0xFF01)

#define VFE_BUS_WM_BUFFER_HEIGHT_CFG(n)		(0x2220 + (n) * 0x100)
#define VFE_BUS_WM_PACKER_CFG(n)		(0x2224 + (n) * 0x100)

#define VFE_BUS_WM_STRIDE(n)			(0x2228 + (n) * 0x100)
#define		WM_STRIDE_DEFAULT_STRIDE	(0xFF01)

#define VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(n)	(0x2248 + (n) * 0x100)
#define VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(n)	(0x224c + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PERIOD(n)		(0x2250 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PATTERN(n)		(0x2254 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_INC(n)			(0x2258 + (n) * 0x100)

/* FULL output: fixed write masters and composite group on Titan 170 */
#define VFE_PIX_WM_Y			3
#define VFE_PIX_WM_C			4
#define VFE_PIX_COMP_GRP		0
#define		WM_CFG_LINE_MODE_EN	(1 << WM_CFG_EN)

#define		PACKER_FMT_PLAIN_8_LSB_MSB_10	(0xe)
#define VFE_BUS_WM_BURST_LIMIT(n)		(0x225c + (n) * 0x100)

static inline void vfe_reg_set(struct vfe_device *vfe, u32 reg, u32 set_bits)
{
	u32 bits = readl_relaxed(vfe->base + reg);

	writel_relaxed(bits | set_bits, vfe->base + reg);
}

static void vfe_global_reset(struct vfe_device *vfe)
{
	u32 reset_bits = GLOBAL_RESET_CMD_CORE		|
			 GLOBAL_RESET_CMD_CAMIF		|
			 GLOBAL_RESET_CMD_BUS		|
			 GLOBAL_RESET_CMD_BUS_BDG	|
			 GLOBAL_RESET_CMD_REGISTER	|
			 GLOBAL_RESET_CMD_TESTGEN	|
			 GLOBAL_RESET_CMD_DSP		|
			 GLOBAL_RESET_CMD_IDLE_CGC	|
			 GLOBAL_RESET_CMD_RDI0		|
			 GLOBAL_RESET_CMD_RDI1		|
			 GLOBAL_RESET_CMD_RDI2		|
			 GLOBAL_RESET_CMD_RDI3;

	writel_relaxed(BIT(31), vfe->base + VFE_IRQ_MASK_0);

	/* Make sure IRQ mask has been written before resetting */
	wmb();

	writel_relaxed(reset_bits, vfe->base + VFE_GLOBAL_RESET_CMD);
}

static void vfe_bus_common_init(struct vfe_device *vfe)
{
	u32 val;

	/*Set Debug Registers*/
	val = DEBUG_STATUS_CFG_STATUS0(1) |
	      DEBUG_STATUS_CFG_STATUS0(7);
	writel_relaxed(val, vfe->base + VFE_BUS_WM_DEBUG_STATUS_CFG);

	/* BUS_WM_INPUT_IF_ADDR_SYNC_FRAME_HEADER */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_ADDR_SYNC_FRAME_HEADER);

	/* no clock gating at bus input */
	val = WM_CGC_OVERRIDE_ALL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_CGC_OVERRIDE);

	writel_relaxed(0x0, vfe->base + VFE_BUS_WM_TEST_BUS_CTRL);

	/* if addr_no_sync has default value then config the addr no sync reg */
	val = WM_ADDR_NO_SYNC_DEFAULT_VAL;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_ADDR_SYNC_NO_SYNC);
}

static void vfe_wm_start(struct vfe_device *vfe, u8 wm, struct vfe_line *line)
{
	u32 val;

	vfe_bus_common_init(vfe);

	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(wm));

	val = WM_BUFFER_DEFAULT_WIDTH;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));

	val = 0;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_PACKER_CFG(wm)); // XXX 1 for PLAIN8?

	/* Configure stride for RDIs */
	val = WM_STRIDE_DEFAULT_STRIDE;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_STRIDE(wm));

	/* Enable WM */
	val = 1 << WM_CFG_EN |
	      MODE_MIPI_RAW << WM_CFG_MODE;
	writel_relaxed(val, vfe->base + VFE_BUS_WM_CFG(wm));
}

static void vfe_wm_stop(struct vfe_device *vfe, u8 wm)
{
	/* Disable WM */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(wm));
}

static void vfe_wm_update(struct vfe_device *vfe, u8 wm, u32 addr,
			  struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	u32 stride = pix->plane_fmt[0].bytesperline;

	writel_relaxed(addr, vfe->base + VFE_BUS_WM_IMAGE_ADDR(wm));
	writel_relaxed(stride * pix->height, vfe->base + VFE_BUS_WM_FRAME_INC(wm));
}

static u32 vfe_reg_update_bit(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	if (vfe_line_is_pix(&vfe->line[line_id]))
		return REG_UPDATE_PIX;

	return REG_UPDATE_RDI(line_id);
}

static void vfe_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	vfe->reg_update |= vfe_reg_update_bit(vfe, line_id);

	/* Enforce ordering between previous reg writes and reg update */
	wmb();

	writel_relaxed(vfe->reg_update, vfe->base + VFE_REG_UPDATE_CMD);

	/* Enforce ordering between reg update and subsequent reg writes */
	wmb();
}

static inline void vfe_reg_update_clear(struct vfe_device *vfe,
					enum vfe_line_id line_id)
{
	vfe->reg_update &= ~vfe_reg_update_bit(vfe, line_id);
}

static void vfe_enable_irq_common(struct vfe_device *vfe)
{
	vfe_reg_set(vfe, VFE_IRQ_MASK_0, ~0u);
	vfe_reg_set(vfe, VFE_IRQ_MASK_1, ~0u);

	writel_relaxed(~0u, vfe->base + VFE_BUS_IRQ_MASK(0));
	writel_relaxed(~0u, vfe->base + VFE_BUS_IRQ_MASK(1));
	writel_relaxed(~0u, vfe->base + VFE_BUS_IRQ_MASK(2));
}

static void vfe_isr_halt_ack(struct vfe_device *vfe)
{
	complete(&vfe->halt_complete);
}

static void vfe_isr_read(struct vfe_device *vfe, u32 *status0, u32 *status1)
{
	*status0 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_0);
	*status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_1);

	writel_relaxed(*status0, vfe->base + VFE_IRQ_CLEAR_0);
	writel_relaxed(*status1, vfe->base + VFE_IRQ_CLEAR_1);

	/* Enforce ordering between IRQ Clear and Global IRQ Clear */
	wmb();
	writel_relaxed(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
}

static void vfe_violation_read(struct vfe_device *vfe)
{
	u32 violation = readl_relaxed(vfe->base + VFE_VIOLATION_STATUS);

	pr_err_ratelimited("VFE: violation = 0x%08x\n", violation);
}

/*
 * Titan 170 PIX path: CSID IPP -> CAMIF -> demux -> demosaic/WB -> linear
 * RGB LUT -> CST (BT.601 full range) -> MNDS -> crop/round/clamp -> FULL NV12
 * on WM3 (Y) and WM4 (CbCr), completed through bus composite group 0.
 *
 * Register layout from the stock Titan 170 kernel (cam_vfe170.h,
 * cam_vfe_bus_ver2.c) and the generated titan170_ife.h. Image quality blocks
 * (BLS, LSC, CCM, gamma curve, stats) are left off; the output is a fixed
 * transport proof, not tuned.
 */

struct vfe_pix_bayer {
	u32 code;
	u8 core_pattern;
	u8 demux_even;
	u8 demux_odd;
};

static const struct vfe_pix_bayer vfe_pix_bayer[] = {
	{ MEDIA_BUS_FMT_SRGGB10_1X10, CFG_PIXEL_PATTERN_RGRGRG, 0xc9, 0xac },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, CFG_PIXEL_PATTERN_GRGRGR, 0x9c, 0xca },
	{ MEDIA_BUS_FMT_SBGGR10_1X10, CFG_PIXEL_PATTERN_BGBGBG, 0xca, 0x9c },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, CFG_PIXEL_PATTERN_GBGBGB, 0xac, 0xc9 },
};

/* BT.601 full range RGB -> YCbCr, Q10, CST input channel order G, B, R */
static const s16 vfe_pix_cst[3][3] = {
	{  601,  117,  306 },	/* Y */
	{ -339,  512, -173 },	/* Cb */
	{ -429,  -83,  512 },	/* Cr */
};

static const struct vfe_pix_bayer *vfe_pix_find_bayer(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vfe_pix_bayer); i++)
		if (vfe_pix_bayer[i].code == code)
			return &vfe_pix_bayer[i];

	return NULL;
}

static void vfe_pix_scale_dir(struct vfe_device *vfe, u32 reg, u32 in, u32 out)
{
	u32 ratio = in / out;
	u32 reso = ratio < 4 ? 3 : ratio < 8 ? 2 : ratio < 16 ? 1 : 0;
	u32 phase = div_u64((u64)in << (reso + SCALE_INTERP_SHIFT), out);

	writel_relaxed((in - 1) | (out - 1) << 16, vfe->base + reg + SCALE_IMAGE_SIZE);
	writel_relaxed(phase | reso << SCALE_PHASE_INTERP_RESO,
		       vfe->base + reg + SCALE_PHASE);
	writel_relaxed(0, vfe->base + reg + SCALE_STRIPE_0);
	writel_relaxed(0, vfe->base + reg + SCALE_STRIPE_1);
	/* single stripe: no skip, no padding, input size n-1 */
	writel_relaxed(in - 1, vfe->base + reg + SCALE_PAD);
}

static void vfe_pix_scale(struct vfe_device *vfe, u32 base, u32 in_w, u32 in_h,
			  u32 out_w, u32 out_h)
{
	writel_relaxed(SCALE_CFG_H_MN_EN | SCALE_CFG_V_MN_EN,
		       vfe->base + base + SCALE_CFG);
	vfe_pix_scale_dir(vfe, base + SCALE_H, in_w, out_w);
	vfe_pix_scale_dir(vfe, base + SCALE_V, in_h, out_h);
}

/* 10-bit pipeline to 8-bit output, full output window */
static void vfe_pix_out_rnd_clamp(struct vfe_device *vfe, u32 base, u32 width,
				  u32 height, bool interleaved)
{
	writel_relaxed(OUT_CROP_EN | OUT_ROUND_EN | OUT_CLAMP_EN,
		       vfe->base + base + OUT_CROP_RND_CLAMP_CFG);
	writel_relaxed(height - 1, vfe->base + base + OUT_CROP_LINE_CFG);
	writel_relaxed(width - 1, vfe->base + base + OUT_CROP_PIXEL_CFG);
	writel_relaxed(255 << OUT_CLAMP_MAX_SHIFT, vfe->base + base + OUT_CLAMP_CFG);
	writel_relaxed((interleaved ? OUT_ROUND_INTERLEAVED : 0) |
		       OUT_ROUND_PATTERN(3) | OUT_ROUND_OFF_BITS(10 - 8),
		       vfe->base + base + OUT_ROUNDING_CFG);
}

/*
 * Linear RGB LUT: whatever the internal bit depth after demosaic, CST sees
 * a well defined 10-bit input. Each entry is a 10-bit base and 10-bit delta.
 */
static void vfe_pix_rgb_lut_linear(struct vfe_device *vfe)
{
	static const u8 ram_sel[] = {
		DMI_RAM_SEL_RGBLUT_CH0_BANK0,
		DMI_RAM_SEL_RGBLUT_CH1_BANK0,
		DMI_RAM_SEL_RGBLUT_CH2_BANK0,
	};
	const u32 step = 1024 / GAMMA_LUT_ENTRIES;
	unsigned int c, i;

	for (c = 0; c < ARRAY_SIZE(ram_sel); c++) {
		writel_relaxed(DMI_CFG_AUTO_INC_EN | ram_sel[c],
			       vfe->base + VFE_DMI_CFG);
		writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
		for (i = 0; i < GAMMA_LUT_ENTRIES; i++) {
			u32 base = i * step;
			u32 delta = i == GAMMA_LUT_ENTRIES - 1 ? 1023 - base : step;

			writel_relaxed(base | delta << GAMMA_LUT_DELTA_SHIFT,
				       vfe->base + VFE_DMI_DATA_LO);
		}
	}
	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	/* bank 0 on all three channels */
	writel_relaxed(0, vfe->base + VFE_RGB_LUT_CFG);
}

static void vfe_pix_config(struct vfe_device *vfe, const struct vfe_pix_bayer *bayer,
			   u32 w, u32 h)
{
	unsigned int i;

	/* clock gating off for bring-up, as the stock HAL does */
	for (i = 0; i < 5; i++)
		writel_relaxed(~0u, vfe->base + VFE_MODULE_CGC_OVERRIDE(i));

	writel_relaxed(bayer->core_pattern, vfe->base + VFE_CORE_CFG);

	/* CAMIF: whole frame, no subsampling */
	writel_relaxed(CMD_CLEAR_CAMIF_STATUS, vfe->base + VFE_CAMIF_CMD);
	writel_relaxed(0xf | 0xf << 8, vfe->base + VFE_CAMIF_SKIP_PERIOD);
	writel_relaxed(1, vfe->base + VFE_CAMIF_IRQ_SUBSAMPLE_PATTERN);
	writel_relaxed(0x00140014, vfe->base + VFE_CAMIF_EPOCH_IRQ);
	writel_relaxed(w - 1, vfe->base + VFE_CAMIF_RAW_CROP_WIDTH_CFG);
	writel_relaxed(h - 1, vfe->base + VFE_CAMIF_RAW_CROP_HEIGHT_CFG);
	writel_relaxed(CFG_VFE_OUTPUT_EN | CFG_BUS_OUTPUT_EN | CFG_RAW_CROP_EN,
		       vfe->base + VFE_CAMIF_CFG);

	/* demux: Bayer period 1, unity gains, no black level */
	writel_relaxed(1, vfe->base + VFE_DEMUX_CFG);
	writel_relaxed(DEMUX_GAIN_UNITY | DEMUX_GAIN_UNITY << 16,
		       vfe->base + VFE_DEMUX_GAIN_0);
	writel_relaxed(DEMUX_GAIN_UNITY | DEMUX_GAIN_UNITY << 16,
		       vfe->base + VFE_DEMUX_GAIN_1);
	writel_relaxed(DEMUX_GAIN_UNITY | DEMUX_GAIN_UNITY << 16,
		       vfe->base + VFE_DEMUX_RIGHT_GAIN_0);
	writel_relaxed(DEMUX_GAIN_UNITY | DEMUX_GAIN_UNITY << 16,
		       vfe->base + VFE_DEMUX_RIGHT_GAIN_1);
	writel_relaxed(bayer->demux_even, vfe->base + VFE_DEMUX_EVEN_CFG);
	writel_relaxed(bayer->demux_odd, vfe->base + VFE_DEMUX_ODD_CFG);

	/* demosaic with its reset interpolation defaults, unity WB */
	writel_relaxed(DEMO_CFG_DYN_G_CLAMP_EN, vfe->base + VFE_DEMO_CFG);
	writel_relaxed(0x00008000, vfe->base + VFE_DEMO_INTERP_COEFF_CFG);
	writel_relaxed(0x08000066, vfe->base + VFE_DEMO_INTERP_CLASSIFIER_0);
	writel_relaxed(WB_GAIN_UNITY | WB_GAIN_UNITY << 16,
		       vfe->base + VFE_DEMO_WB_LEFT_CFG_0);
	writel_relaxed(WB_GAIN_UNITY, vfe->base + VFE_DEMO_WB_LEFT_CFG_1);
	writel_relaxed(0, vfe->base + VFE_DEMO_WB_LEFT_OFFSET_CFG_0);
	writel_relaxed(0, vfe->base + VFE_DEMO_WB_LEFT_OFFSET_CFG_1);

	vfe_pix_rgb_lut_linear(vfe);

	for (i = 0; i < 3; i++) {
		u32 ch = VFE_COLOR_XFORM_CH(i);

		writel_relaxed((vfe_pix_cst[i][0] & 0x1fff) |
			       (vfe_pix_cst[i][1] & 0x1fff) << 16,
			       vfe->base + ch + CST_COEFF_CFG_0);
		writel_relaxed(vfe_pix_cst[i][2] & 0x1fff,
			       vfe->base + ch + CST_COEFF_CFG_1);
		/* chroma centred at 512 in the 10-bit domain */
		writel_relaxed((i ? 512 : 0) << 16, vfe->base + ch + CST_OFFSET_CFG);
		writel_relaxed(1023 << 16, vfe->base + ch + CST_CLAMP_CFG);
	}

	/* luma 1:1, chroma 4:2:0 */
	vfe_pix_scale(vfe, VFE_SCALE_VID_Y, w, h, w, h);
	vfe_pix_scale(vfe, VFE_SCALE_VID_CBCR, w, h, w / 2, h / 2);

	/* chroma is interleaved CbCr: w samples per line, h / 2 lines */
	vfe_pix_out_rnd_clamp(vfe, VFE_FULL_OUT_Y, w, h, false);
	vfe_pix_out_rnd_clamp(vfe, VFE_FULL_OUT_C, w, h / 2, true);

	writel_relaxed(LENS_EN_DEMUX | LENS_EN_DEMO, vfe->base + VFE_MODULE_LENS_EN);
	writel_relaxed(0, vfe->base + VFE_MODULE_STATS_EN);
	writel_relaxed(COLOR_EN_RGB_LUT, vfe->base + VFE_MODULE_COLOR_EN);
	writel_relaxed(ZOOM_EN_CST | ZOOM_EN_SCALE_VID | ZOOM_EN_CROP_VID,
		       vfe->base + VFE_MODULE_ZOOM_EN);
}

static void vfe_pix_wm_config(struct vfe_device *vfe, u8 wm, u32 width,
			     u32 height, u32 stride)
{
	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(wm));
	writel_relaxed(width, vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));
	writel_relaxed(height, vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));
	writel_relaxed(PACKER_FMT_PLAIN_8_LSB_MSB_10,
		       vfe->base + VFE_BUS_WM_PACKER_CFG(wm));
	writel_relaxed(stride, vfe->base + VFE_BUS_WM_STRIDE(wm));
	writel_relaxed(stride * height, vfe->base + VFE_BUS_WM_FRAME_INC(wm));
	/* every frame written, every frame signalled */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_FRAMEDROP_PERIOD(wm));
	writel_relaxed(1, vfe->base + VFE_BUS_WM_FRAMEDROP_PATTERN(wm));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(wm));
	writel_relaxed(1, vfe->base + VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(wm));
}

/* Program a frame's plane addresses into the next slot of both WMs */
static void vfe_pix_queue_addr(struct vfe_device *vfe, struct vfe_pix_staging *st,
			       u8 idx)
{
	dma_addr_t y = st->frame[idx].dma;

	writel_relaxed(y, vfe->base + VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_Y));
	writel_relaxed(y + st->c_offset, vfe->base + VFE_BUS_WM_IMAGE_ADDR(VFE_PIX_WM_C));
}

/* A free staging frame for the hardware, else the drop frame. Locked. */
static u8 vfe_pix_take_frame(struct vfe_pix_staging *st)
{
	unsigned long idx = find_first_bit(&st->free, VFE_PIX_STAGING_NUM);

	if (idx >= VFE_PIX_STAGING_NUM)
		return VFE_PIX_DROP;

	clear_bit(idx, &st->free);
	return idx;
}

/* DMA frame lifetime, KUnit static stub points */
static void *vfe_pix_frame_alloc(struct vfe_device *vfe, size_t size, dma_addr_t *dma)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_frame_alloc, vfe, size, dma);

	return dma_alloc_noncoherent(vfe->camss->dev, size, dma, DMA_FROM_DEVICE,
				     GFP_KERNEL);
}

static void vfe_pix_frame_free(struct vfe_device *vfe, size_t size,
			       struct vfe_pix_frame *f)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_frame_free, vfe, size, f);

	dma_free_noncoherent(vfe->camss->dev, size, f->cpu, f->dma, DMA_FROM_DEVICE);
}

/*
 * DMA frames left to an unconfirmed reset. Kept for the life of the system:
 * the allocations (pages and IOVA mappings) and a reference on the device
 * are never released, and the module pins itself so that an unload cannot
 * tear down the device and its IOMMU domain. With titan170_pix the driver
 * also suppresses sysfs bind/unbind (see camss_init()). This does not make
 * kexec, or removal of the platform device by other means, safe; neither
 * happens in normal operation of the A52 build this targets.
 */
struct vfe_pix_quarantine {
	struct list_head list;
	struct device *dev;
	size_t size;
	struct vfe_pix_frame frame[VFE_PIX_STAGING_NUM + 1];
};

static LIST_HEAD(vfe_pix_quarantine_list);
static DEFINE_MUTEX(vfe_pix_quarantine_lock);

/*
 * vfe_pix_release_frames - Free the DMA frames, or retain them for good
 * @quarantine: the bus was not confirmed idle; the hardware may still
 *              write to any of them, so none may ever be reused or freed
 */
static void vfe_pix_release_frames(struct vfe_device *vfe, struct vfe_pix_staging *st,
				   bool quarantine)
{
	struct vfe_pix_quarantine *q;
	unsigned int i;

	if (!quarantine) {
		for (i = 0; i < ARRAY_SIZE(st->frame); i++)
			if (st->frame[i].cpu)
				vfe_pix_frame_free(vfe, st->size, &st->frame[i]);
		memset(st->frame, 0, sizeof(st->frame));
		return;
	}

	mutex_lock(&vfe_pix_quarantine_lock);
	if (list_empty(&vfe_pix_quarantine_list))
		__module_get(THIS_MODULE);	/* never put */

	/* ponytail: on allocation failure the frames are still never freed */
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (q) {
		q->dev = get_device(vfe->camss->dev);
		q->size = st->size;
		memcpy(q->frame, st->frame, sizeof(q->frame));
		list_add_tail(&q->list, &vfe_pix_quarantine_list);
	}
	for (i = 0; i < ARRAY_SIZE(st->frame); i++)
		if (st->frame[i].cpu)
			vfe->quarantined += st->size;
	mutex_unlock(&vfe_pix_quarantine_lock);

	memset(st->frame, 0, sizeof(st->frame));
}

/*
 * vfe_pix_copy - Copy a completed staging frame into a vb2 buffer
 *
 * The PIX video node uses vb2-vmalloc with MMAP/READ only: its buffers are
 * CPU memory with no device mapping and need no cache maintenance. The
 * staging frame is synced for the CPU before reading. DMA-BUF import is not
 * offered: an exporter's CPU access can wait on fences without bound, and
 * this worker must always finish for STREAMOFF to complete.
 */
static int vfe_pix_copy(struct vfe_device *vfe, struct vfe_pix_staging *st,
			struct vfe_pix_frame *f, struct camss_buffer *buf)
{
	void *dst = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_copy, vfe, st, f, buf);

	if (!dst)
		return -ENOMEM;

	dma_sync_single_for_cpu(vfe->camss->dev, f->dma, st->size, DMA_FROM_DEVICE);
	memcpy(dst, f->cpu, st->size);

	return 0;
}

/*
 * Hand a staging or drop frame to the device: before its address is first
 * published (the allocator leaves CPU cache lines dirty) and after every
 * copy. A KUnit static stub point.
 */
static void vfe_pix_frame_to_device(struct vfe_device *vfe, size_t size,
				    struct vfe_pix_frame *f)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_frame_to_device, vfe, size, f);

	dma_sync_single_for_device(vfe->camss->dev, f->dma, size, DMA_FROM_DEVICE);
}

/* Fail the vb2 queue after a fault. A KUnit static stub point. */
static void vfe_pix_signal_fault(struct vfe_line *line)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_signal_fault, line);

	vb2_queue_error(&line->video_out.vb2_q);
}

/* vb2 completion, a KUnit static stub point */
static void vfe_pix_return_buf(struct camss_buffer *buf, enum vb2_buffer_state state)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_pix_return_buf, buf, state);

	vb2_buffer_done(&buf->vb.vb2_buf, state);
}

/*
 * vfe_pix_copy_work - Deliver completed frames, oldest first
 *
 * Each frame goes to the oldest queued vb2 buffer, or is dropped if none
 * is queued. Only then does the staging frame become available to the
 * hardware again.
 */
static void vfe_pix_copy_work(struct work_struct *work)
{
	struct vfe_pix_staging *st = container_of(work, struct vfe_pix_staging, copy_work);
	struct vfe_output *output = container_of(st, struct vfe_output, pix);
	struct vfe_line *line = container_of(output, struct vfe_line, output);
	struct vfe_device *vfe = to_vfe(line);
	struct camss_buffer *buf;
	struct vfe_pix_frame *f;
	unsigned long flags;
	u8 idx;
	int ret;

	for (;;) {
		spin_lock_irqsave(&vfe->output_lock, flags);
		if (output->state != VFE_OUTPUT_ON || st->faulted || !st->nready) {
			spin_unlock_irqrestore(&vfe->output_lock, flags);
			return;
		}
		idx = st->ready[0];
		memmove(&st->ready[0], &st->ready[1], --st->nready);
		buf = vfe_buf_get_pending(output);
		spin_unlock_irqrestore(&vfe->output_lock, flags);

		f = &st->frame[idx];
		if (buf) {
			ret = vfe_pix_copy(vfe, st, f, buf);
			buf->vb.vb2_buf.timestamp = f->timestamp;
			buf->vb.sequence = f->sequence;
			vfe_pix_return_buf(buf, ret || f->error ? VB2_BUF_STATE_ERROR :
							   VB2_BUF_STATE_DONE);
		} else {
			dev_dbg_ratelimited(vfe->camss->dev,
					    "VFE%u PIX: frame %u had no buffer\n",
					    vfe->id, f->sequence);
		}

		vfe_pix_frame_to_device(vfe, st->size, f);

		spin_lock_irqsave(&vfe->output_lock, flags);
		set_bit(idx, &st->free);
		spin_unlock_irqrestore(&vfe->output_lock, flags);
	}
}

/*
 * vfe_pix_enable - Configure and arm the PIX path
 *
 * Runs before the CSID IPP and the sensor start (the video node starts the
 * pipeline from the VFE upstream); CSID port 3 and VFE line exclusivity keep
 * the IPP of this pipeline the only pixel source, so all writes land before
 * the first SOF.
 */
static int vfe_pix_enable(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	struct vfe_pix_staging *st = &output->pix;
	const struct v4l2_pix_format_mplane *pix = &line->video_out.active_fmt.fmt.pix_mp;
	const struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	const struct v4l2_mbus_framefmt *src = &line->fmt[MSM_VFE_PAD_SRC];
	const struct vfe_pix_bayer *bayer = vfe_pix_find_bayer(sink->code);
	struct vfe_pix_frame frame[VFE_PIX_STAGING_NUM + 1] = {};
	u32 w = sink->width, h = sink->height;
	u32 bpl = pix->plane_fmt[0].bytesperline;
	unsigned long flags;
	unsigned int i;
	size_t size;
	int ret;

	/* The copy fills exactly this much of the vb2 plane */
	if (!bayer || src->width != w || src->height != h ||
	    pix->pixelformat != V4L2_PIX_FMT_NV12 || pix->num_planes != 1 ||
	    pix->width != w || pix->height != h || w % 16 || h % 2 ||
	    bpl < w || bpl % 16 ||
	    (u64)bpl * h * 3 / 2 > pix->plane_fmt[0].sizeimage) {
		dev_err(vfe->camss->dev,
			"VFE%u PIX: unsupported layout %ux%u code 0x%x bpl %u size %u\n",
			vfe->id, w, h, sink->code, bpl, pix->plane_fmt[0].sizeimage);
		return -EINVAL;
	}

	size = (size_t)bpl * h * 3 / 2;
	for (i = 0; i < ARRAY_SIZE(frame); i++) {
		frame[i].cpu = vfe_pix_frame_alloc(vfe, size, &frame[i].dma);
		if (!frame[i].cpu) {
			ret = -ENOMEM;
			goto error_free;
		}
		/* clean the allocator's dirty lines before any address is published */
		vfe_pix_frame_to_device(vfe, size, &frame[i]);
	}

	INIT_WORK(&st->copy_work, vfe_pix_copy_work);

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (output->state > VFE_OUTPUT_RESERVED ||
	    vfe->wm_output_map[VFE_PIX_WM_Y] != VFE_LINE_NONE ||
	    vfe->wm_output_map[VFE_PIX_WM_C] != VFE_LINE_NONE) {
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		dev_err(vfe->camss->dev, "VFE%u PIX: output busy\n", vfe->id);
		ret = -EBUSY;
		goto error_free;
	}

	memcpy(st->frame, frame, sizeof(frame));
	st->size = size;
	st->c_offset = bpl * h;
	st->free = GENMASK(VFE_PIX_STAGING_NUM - 1, 0);
	st->nready = 0;
	st->frame_error = false;
	st->faulted = false;
	output->wm_num = 2;
	output->wm_idx[0] = VFE_PIX_WM_Y;
	output->wm_idx[1] = VFE_PIX_WM_C;
	vfe->wm_output_map[VFE_PIX_WM_Y] = line->id;
	vfe->wm_output_map[VFE_PIX_WM_C] = line->id;
	output->state = VFE_OUTPUT_ON;
	output->sequence = 0;
	output->wait_reg_update = 0;

	vfe_pix_config(vfe, bayer, w, h);

	vfe_bus_common_init(vfe);
	vfe_pix_wm_config(vfe, VFE_PIX_WM_Y, w, h, bpl);
	vfe_pix_wm_config(vfe, VFE_PIX_WM_C, w, h / 2, bpl);

	/* Two frames' addresses in flight, both driver owned */
	for (i = 0; i < 2; i++) {
		st->hw[i] = vfe_pix_take_frame(st);
		vfe_pix_queue_addr(vfe, st, st->hw[i]);
	}

	/* Arm only once both address slots of both planes are valid */
	writel_relaxed(BIT(VFE_PIX_WM_Y) | BIT(VFE_PIX_WM_C),
		       vfe->base + VFE_BUS_COMP_MASK(VFE_PIX_COMP_GRP));
	wmb();
	writel_relaxed(WM_CFG_LINE_MODE_EN, vfe->base + VFE_BUS_WM_CFG(VFE_PIX_WM_Y));
	writel_relaxed(WM_CFG_LINE_MODE_EN, vfe->base + VFE_BUS_WM_CFG(VFE_PIX_WM_C));

	vfe_reg_update(vfe, line->id);

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;

error_free:
	/* never handed to the hardware */
	while (i--)
		vfe_pix_frame_free(vfe, size, &frame[i]);
	return ret;
}

/*
 * vfe_pix_buf_done - Composite done: both planes of the head frame landed
 *
 * Exactly one completion per frame. A staging frame goes to the copy
 * worker; the drop frame is only counted. The next slot gets a free staging
 * frame, or the drop frame while stopping or when all are being copied.
 */
static void vfe_pix_buf_done(struct vfe_device *vfe)
{
	struct vfe_output *output = &vfe->line[VFE_LINE_PIX].output;
	struct vfe_pix_staging *st = &output->pix;
	unsigned long flags;
	u64 ts = ktime_get_ns();
	u8 head;

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (output->state != VFE_OUTPUT_ON &&
	    output->state != VFE_OUTPUT_STOPPING) {
		spin_unlock_irqrestore(&vfe->output_lock, flags);
		return;
	}

	head = st->hw[0];
	if (head != VFE_PIX_DROP && output->state == VFE_OUTPUT_ON && !st->faulted) {
		st->frame[head].sequence = output->sequence;
		st->frame[head].timestamp = ts;
		st->frame[head].error = st->frame_error;
		st->ready[st->nready++] = head;
		/* under the lock: vfe_pix_disable() cancels after STOPPING */
		queue_work(system_highpri_wq, &st->copy_work);
	}
	st->frame_error = false;
	output->sequence++;

	st->hw[0] = st->hw[1];
	st->hw[1] = output->state == VFE_OUTPUT_ON && !st->faulted ?
		    vfe_pix_take_frame(st) : VFE_PIX_DROP;
	vfe_pix_queue_addr(vfe, st, st->hw[1]);

	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

/* Bus errors after which completions may no longer match address slots */
#define VFE_PIX_BUS_FAULT	(STATUS0_COMP_ERROR | STATUS0_COMP_OVERWRITE | \
				 STATUS0_OVERFLOW | STATUS0_VIOLATION)

/*
 * vfe_pix_error - Bus or CAMIF error on the PIX line
 *
 * A CAMIF error or VFE violation fails the frame in progress. A bus
 * overflow, violation or composite error/overwrite can mean a completion
 * was lost or merged, after which a completion no longer proves which
 * staging frame the hardware finished: fail closed. No completed frame is
 * read any more, the hardware only gets the drop frame, and the queue is
 * failed so that userspace stops; the frames are freed or retained at stop
 * depending on the reset.
 */
static void vfe_pix_error(struct vfe_device *vfe, u32 status1, u32 bus_status0)
{
	struct vfe_line *line = &vfe->line[VFE_LINE_PIX];
	struct vfe_output *output = &line->output;
	struct vfe_pix_staging *st = &output->pix;
	bool fault = false;
	unsigned long flags;

	spin_lock_irqsave(&vfe->output_lock, flags);
	if (output->state == VFE_OUTPUT_ON) {
		st->frame_error = true;
		if ((bus_status0 & VFE_PIX_BUS_FAULT) && !st->faulted) {
			st->faulted = true;
			st->nready = 0;
			fault = true;
		}
	}
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	if (fault)
		vfe_pix_signal_fault(line);

	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PIX: %s status1 0x%08x bus0 0x%08x\n", vfe->id,
			    fault ? "fault, capture stopped:" : "error", status1, bus_status0);
}

/*
 * vfe_isr - VFE module interrupt handler
 * @irq: Interrupt line
 * @dev: VFE device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t vfe_isr(int irq, void *dev)
{
	struct vfe_device *vfe = dev;
	bool pix = vfe_line_is_pix(&vfe->line[VFE_LINE_PIX]);
	u32 status0, status1, vfe_bus_status[VFE_BUS_IRQ_BANKS];
	int i, wm, rdi_num;

	status0 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_0);
	status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS_1);

	writel_relaxed(status0, vfe->base + VFE_IRQ_CLEAR_0);
	writel_relaxed(status1, vfe->base + VFE_IRQ_CLEAR_1);

	/* The bus has three IRQ banks, independent of the line count */
	for (i = 0; i < VFE_BUS_IRQ_BANKS; i++) {
		vfe_bus_status[i] = readl_relaxed(vfe->base + VFE_BUS_IRQ_STATUS(i));
		writel_relaxed(vfe_bus_status[i], vfe->base + VFE_BUS_IRQ_CLEAR(i));
	}

	/* Enforce ordering between IRQ reading and interpretation */
	wmb();

	writel_relaxed(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
	writel_relaxed(1, vfe->base + VFE_BUS_IRQ_CLEAR_GLOBAL);

	if (pix)
		trace_camss_pix_irq(vfe->id, status0, status1, vfe_bus_status[0],
				    vfe_bus_status[1],
				    READ_ONCE(vfe->line[VFE_LINE_PIX].output.pix.hw[0]),
				    READ_ONCE(vfe->line[VFE_LINE_PIX].output.pix.hw[1]));

	if (status0 & STATUS_0_RESET_ACK)
		vfe->isr_ops.reset_ack(vfe);

	/* With a real PIX line, line 3 is not an RDI */
	rdi_num = pix ? VFE_LINE_PIX : vfe->res->line_num;

	for (i = VFE_LINE_RDI0; i < rdi_num; i++)
		if (status0 & STATUS_0_RDI_REG_UPDATE(i))
			vfe->isr_ops.reg_update(vfe, i);

	for (i = VFE_LINE_RDI0; i < rdi_num; i++)
		if (status0 & STATUS_1_RDI_SOF(i))
			vfe->isr_ops.sof(vfe, i);

	if (pix) {
		if (status0 & STATUS_0_PIX_REG_UPDATE)
			vfe->isr_ops.reg_update(vfe, VFE_LINE_PIX);

		if ((status1 & (STATUS_1_CAMIF_ERROR | STATUS_1_VIOLATION)) ||
		    (vfe_bus_status[0] & (STATUS0_COMP_ERROR |
					  STATUS0_COMP_OVERWRITE |
					  STATUS0_OVERFLOW |
					  STATUS0_VIOLATION)))
			vfe_pix_error(vfe, status1, vfe_bus_status[0]);

		if (vfe_bus_status[0] & STATUS0_COMP_BUF_DONE(VFE_PIX_COMP_GRP))
			vfe_pix_buf_done(vfe);
	}

	for (wm = 0; wm < MSM_VFE_IMAGE_MASTERS_NUM; wm++) {
		/* PIX planes complete through their composite group only */
		if (pix && vfe->wm_output_map[wm] == VFE_LINE_PIX)
			continue;
		if (status0 & BIT(9))
			if (vfe_bus_status[1] & STATUS1_WM_CLIENT_BUF_DONE(wm))
				vfe->isr_ops.wm_done(vfe, wm);
	}

	return IRQ_HANDLED;
}

/*
 * vfe_halt - Trigger halt on VFE module and wait to complete
 * @vfe: VFE device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_halt(struct vfe_device *vfe)
{
	/* rely on vfe_disable_output() to stop the VFE */
	return 0;
}

static int vfe_get_output(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output;
	unsigned long flags;
	int wm_idx;

	spin_lock_irqsave(&vfe->output_lock, flags);

	output = &line->output;
	if (output->state > VFE_OUTPUT_RESERVED) {
		dev_err(vfe->camss->dev, "Output is running\n");
		goto error;
	}

	output->wm_num = 1;

	/*
	 * RDIn is hardwired to WMn (stock cam_vfe_bus_ver2.c). First-free
	 * allocation would hand an RDI another line's client, e.g. a PIX
	 * plane master.
	 */
	wm_idx = line->id;
	if (vfe->wm_output_map[wm_idx] != VFE_LINE_NONE) {
		dev_err(vfe->camss->dev, "Can not reserve wm\n");
		goto error;
	}
	vfe->wm_output_map[wm_idx] = line->id;
	output->wm_idx[0] = wm_idx;

	output->drop_update_idx = 0;

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return 0;

error:
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	return -EINVAL;
}

/*
 * vfe_enable - Enable streaming on VFE line
 * @line: VFE line
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_enable(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	int ret;

	mutex_lock(&vfe->stream_lock);

	/*
	 * A global reset is the only stop this backend has and it takes down
	 * every line, so with the pixel path enabled one line (PIX or RDI)
	 * owns the VFE at a time. Reserved here, under stream_lock, in either
	 * start order. ponytail: whole-VFE exclusivity until lines can be
	 * stopped individually.
	 */
	if (vfe_line_is_pix(&vfe->line[VFE_LINE_PIX])) {
		ret = 0;
		if (READ_ONCE(vfe->wedged))
			ret = -EIO;
		else if (vfe->stream_count)
			ret = -EBUSY;
		if (ret) {
			mutex_unlock(&vfe->stream_lock);
			dev_err(vfe->camss->dev, "VFE%u: %s\n", vfe->id,
				ret == -EIO ? "wedged, reboot required" :
				"one line streams at a time");
			return ret;
		}
		if (vfe_line_is_pix(line))
			vfe->pix_active = true;
	}

	if (!vfe->stream_count)
		vfe_enable_irq_common(vfe);

	vfe->stream_count++;

	mutex_unlock(&vfe->stream_lock);

	if (vfe_line_is_pix(line)) {
		ret = vfe_pix_enable(line);
		if (ret < 0) {
			mutex_lock(&vfe->stream_lock);
			vfe->pix_active = false;
			vfe->stream_count--;
			mutex_unlock(&vfe->stream_lock);
			return ret;
		}

		vfe->was_streaming = 1;

		return 0;
	}

	ret = vfe_get_output(line);
	if (ret < 0)
		goto error_get_output;

	ret = vfe_enable_output_v2(line);
	if (ret < 0)
		goto error_enable_output;

	vfe->was_streaming = 1;

	return 0;

error_enable_output:
	vfe_put_output(line);

error_get_output:
	mutex_lock(&vfe->stream_lock);

	vfe->stream_count--;

	mutex_unlock(&vfe->stream_lock);

	return ret;
}

/*
 * vfe_isr_sof - Process start of frame interrupt
 * @vfe: VFE Device
 * @line_id: VFE line
 */
static void vfe_isr_sof(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	/* nop */
}

/*
 * vfe_isr_reg_update - Process reg update interrupt
 * @vfe: VFE Device
 * @line_id: VFE line
 */
static void vfe_isr_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	struct vfe_output *output;
	unsigned long flags;

	spin_lock_irqsave(&vfe->output_lock, flags);
	vfe->res->hw_ops->reg_update_clear(vfe, line_id);

	output = &vfe->line[line_id].output;

	if (output->wait_reg_update) {
		output->wait_reg_update = 0;
		complete(&output->reg_update);
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

/*
 * vfe_isr_wm_done - Process write master done interrupt
 * @vfe: VFE Device
 * @wm: Write master id
 */
static void vfe_isr_wm_done(struct vfe_device *vfe, u8 wm)
{
	struct vfe_line *line = &vfe->line[vfe->wm_output_map[wm]];
	struct camss_buffer *ready_buf;
	struct vfe_output *output;
	unsigned long flags;
	u32 index;
	u64 ts = ktime_get_ns();

	spin_lock_irqsave(&vfe->output_lock, flags);

	if (vfe->wm_output_map[wm] == VFE_LINE_NONE) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Received wm done for unmapped index\n");
		goto out_unlock;
	}
	output = &vfe->line[vfe->wm_output_map[wm]].output;

	ready_buf = output->buf[0];
	if (!ready_buf) {
		dev_err_ratelimited(vfe->camss->dev,
				    "Missing ready buf %d!\n", output->state);
		goto out_unlock;
	}

	ready_buf->vb.vb2_buf.timestamp = ts;
	ready_buf->vb.sequence = output->sequence++;

	index = 0;
	output->buf[0] = output->buf[1];
	if (output->buf[0])
		index = 1;

	output->buf[index] = vfe_buf_get_pending(output);

	if (output->buf[index])
		vfe_wm_update(vfe, output->wm_idx[0], output->buf[index]->addr[0], line);
	else
		output->gen2.active_num--;

	spin_unlock_irqrestore(&vfe->output_lock, flags);

	vb2_buffer_done(&ready_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	return;

out_unlock:
	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

/* RESET_ACK status and its clear, KUnit static stub points */
static bool vfe_reset_ack_status(struct vfe_device *vfe)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_reset_ack_status, vfe);

	return readl(vfe->base + VFE_IRQ_STATUS_0) & STATUS_0_RESET_ACK;
}

static void vfe_reset_ack_clear(struct vfe_device *vfe)
{
	KUNIT_STATIC_STUB_REDIRECT(vfe_reset_ack_clear, vfe);

	writel_relaxed(STATUS_0_RESET_ACK, vfe->base + VFE_IRQ_CLEAR_0);
	writel(CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);
}

/*
 * vfe_reset_170 - Serialized global reset (titan170_pix)
 *
 * Used for every reset of a PIX capable VFE: stream stop and power-up. The
 * ISR is kept off the line for the whole sequence, so only this function
 * consumes RESET_ACK: drain a stale acknowledge and require it to clear,
 * issue the reset, poll for a fresh acknowledge, clear it.
 *
 * A reset that is not acknowledged leaves the bus state unknown, and a
 * late acknowledge could later pass for a new one. Fail closed: the VFE is
 * wedged until reboot and no further reset is attempted.
 */
static int vfe_reset_170(struct vfe_device *vfe)
{
	bool ack;
	int ret;

	if (READ_ONCE(vfe->wedged))
		return -EIO;

	disable_irq(vfe->irq);

	vfe_reset_ack_clear(vfe);
	if (vfe_reset_ack_status(vfe)) {
		ret = -EIO;
		dev_crit(vfe->camss->dev, "VFE%u: stale reset acknowledge does not clear\n",
			 vfe->id);
		goto out;
	}

	vfe->res->hw_ops->global_reset(vfe);

	ret = read_poll_timeout(vfe_reset_ack_status, ack, ack, 100,
				VFE_RESET_TIMEOUT_US, false, vfe);
	if (!ret)
		vfe_reset_ack_clear(vfe);
	else
		dev_crit(vfe->camss->dev, "VFE%u: reset not acknowledged\n", vfe->id);

out:
	if (ret)
		WRITE_ONCE(vfe->wedged, true);
	enable_irq(vfe->irq);

	return ret;
}

/*
 * vfe_pix_disable - Stop the PIX line
 *
 * Once STOPPING is set under output_lock no IRQ queues copy work, so the
 * cancel below leaves none behind. vb2 buffers are never DMA targets (the
 * node uses vb2-vmalloc), so they can always be returned. The
 * driver frames can be freed only once a reset confirmed the bus idle;
 * otherwise they are abandoned for good and the VFE refuses to stream until
 * a power-up reset is acknowledged.
 */
static int vfe_pix_disable(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct vfe_output *output = &line->output;
	struct vfe_pix_staging *st = &output->pix;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vfe->output_lock, flags);
	output->state = VFE_OUTPUT_STOPPING;
	writel_relaxed(0, vfe->base + VFE_BUS_COMP_MASK(VFE_PIX_COMP_GRP));
	writel_relaxed(0, vfe->base + VFE_CAMIF_CFG);
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(VFE_PIX_WM_Y));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(VFE_PIX_WM_C));
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	/* a copy in progress completes its vb2 buffer; no new ones start */
	cancel_work_sync(&st->copy_work);

	ret = vfe_reset(vfe);

	spin_lock_irqsave(&vfe->output_lock, flags);
	output->state = VFE_OUTPUT_IDLE;
	st->nready = 0;
	spin_unlock_irqrestore(&vfe->output_lock, flags);

	vfe_pix_release_frames(vfe, st, ret);
	vfe_put_output(line);

	mutex_lock(&vfe->stream_lock);
	vfe->pix_active = false;
	vfe->stream_count--;
	mutex_unlock(&vfe->stream_lock);

	if (ret)
		dev_crit(vfe->camss->dev,
			 "VFE%u PIX: reset not confirmed, %zu bytes of DMA frames retained, VFE disabled until reboot\n",
			 vfe->id, vfe->quarantined);

	return ret;
}

static int vfe_disable_170(struct vfe_line *line)
{
	if (vfe_line_is_pix(line))
		return vfe_pix_disable(line);

	return vfe_disable(line);
}

static const struct vfe_isr_ops vfe_isr_ops_170 = {
	.reset_ack = vfe_isr_reset_ack,
	.halt_ack = vfe_isr_halt_ack,
	.reg_update = vfe_isr_reg_update,
	.sof = vfe_isr_sof,
	.comp_done = vfe_isr_comp_done,
	.wm_done = vfe_isr_wm_done,
};

static const struct camss_video_ops vfe_video_ops_170 = {
	.queue_buffer = vfe_queue_buffer_v2,
	.flush_buffers = vfe_flush_buffers,
};

static void vfe_subdev_init(struct device *dev, struct vfe_device *vfe)
{
	vfe->isr_ops = vfe_isr_ops_170;
	vfe->video_ops = vfe_video_ops_170;
}

const struct vfe_hw_ops vfe_ops_170 = {
	.global_reset = vfe_global_reset,
	.hw_version = vfe_hw_version,
	.isr_read = vfe_isr_read,
	.isr = vfe_isr,
	.pm_domain_off = vfe_pm_domain_off,
	.pm_domain_on = vfe_pm_domain_on,
	.reg_update_clear = vfe_reg_update_clear,
	.reg_update = vfe_reg_update,
	.reset = vfe_reset_170,
	.subdev_init = vfe_subdev_init,
	.vfe_disable = vfe_disable_170,
	.vfe_enable = vfe_enable,
	.vfe_halt = vfe_halt,
	.violation_read = vfe_violation_read,
	.vfe_wm_start = vfe_wm_start,
	.vfe_wm_stop = vfe_wm_stop,
	.vfe_wm_update = vfe_wm_update,
};

#if IS_ENABLED(CONFIG_VIDEO_QCOM_CAMSS_KUNIT_TEST)
#include "camss-vfe-17x-test.c"
#endif
