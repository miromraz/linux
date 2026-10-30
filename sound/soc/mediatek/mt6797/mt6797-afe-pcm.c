// SPDX-License-Identifier: GPL-2.0
//
// Mediatek ALSA SoC AFE platform driver for 6797
//
// Copyright (c) 2018 MediaTek Inc.
// Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>

#include "mt6797-afe-common.h"
#include "mt6797-afe-clk.h"
#include "mt6797-interconnection.h"
#include "mt6797-reg.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-afe-fe-dai.h"

enum {
	MTK_AFE_RATE_8K = 0,
	MTK_AFE_RATE_11K = 1,
	MTK_AFE_RATE_12K = 2,
	MTK_AFE_RATE_384K = 3,
	MTK_AFE_RATE_16K = 4,
	MTK_AFE_RATE_22K = 5,
	MTK_AFE_RATE_24K = 6,
	MTK_AFE_RATE_130K = 7,
	MTK_AFE_RATE_32K = 8,
	MTK_AFE_RATE_44K = 9,
	MTK_AFE_RATE_48K = 10,
	MTK_AFE_RATE_88K = 11,
	MTK_AFE_RATE_96K = 12,
	MTK_AFE_RATE_174K = 13,
	MTK_AFE_RATE_192K = 14,
	MTK_AFE_RATE_260K = 15,
};

enum {
	MTK_AFE_DAI_MEMIF_RATE_8K = 0,
	MTK_AFE_DAI_MEMIF_RATE_16K = 1,
	MTK_AFE_DAI_MEMIF_RATE_32K = 2,
};

enum {
	MTK_AFE_PCM_RATE_8K = 0,
	MTK_AFE_PCM_RATE_16K = 1,
	MTK_AFE_PCM_RATE_32K = 2,
	MTK_AFE_PCM_RATE_48K = 3,
};

unsigned int mt6797_general_rate_transform(struct device *dev,
					   unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_RATE_8K;
	case 11025:
		return MTK_AFE_RATE_11K;
	case 12000:
		return MTK_AFE_RATE_12K;
	case 16000:
		return MTK_AFE_RATE_16K;
	case 22050:
		return MTK_AFE_RATE_22K;
	case 24000:
		return MTK_AFE_RATE_24K;
	case 32000:
		return MTK_AFE_RATE_32K;
	case 44100:
		return MTK_AFE_RATE_44K;
	case 48000:
		return MTK_AFE_RATE_48K;
	case 88200:
		return MTK_AFE_RATE_88K;
	case 96000:
		return MTK_AFE_RATE_96K;
	case 130000:
		return MTK_AFE_RATE_130K;
	case 176400:
		return MTK_AFE_RATE_174K;
	case 192000:
		return MTK_AFE_RATE_192K;
	case 260000:
		return MTK_AFE_RATE_260K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, use %d!!!\n",
			 __func__, rate, MTK_AFE_RATE_48K);
		return MTK_AFE_RATE_48K;
	}
}

static unsigned int dai_memif_rate_transform(struct device *dev,
					     unsigned int rate)
{
	switch (rate) {
	case 8000:
		return MTK_AFE_DAI_MEMIF_RATE_8K;
	case 16000:
		return MTK_AFE_DAI_MEMIF_RATE_16K;
	case 32000:
		return MTK_AFE_DAI_MEMIF_RATE_32K;
	default:
		dev_warn(dev, "%s(), rate %u invalid, use %d!!!\n",
			 __func__, rate, MTK_AFE_DAI_MEMIF_RATE_16K);
		return MTK_AFE_DAI_MEMIF_RATE_16K;
	}
}

unsigned int mt6797_rate_transform(struct device *dev,
				   unsigned int rate, int aud_blk)
{
	switch (aud_blk) {
	case MT6797_MEMIF_DAI:
	case MT6797_MEMIF_MOD_DAI:
		return dai_memif_rate_transform(dev, rate);
	default:
		return mt6797_general_rate_transform(dev, rate);
	}
}

static const struct snd_pcm_hardware mt6797_afe_hardware = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE |
		   SNDRV_PCM_FMTBIT_S24_LE |
		   SNDRV_PCM_FMTBIT_S32_LE,
	.period_bytes_min = 256,
	.period_bytes_max = 4 * 48 * 1024,
	.periods_min = 2,
	.periods_max = 256,
	.buffer_bytes_max = 8 * 48 * 1024,
	.fifo_size = 0,
};

static int mt6797_memif_fs(struct snd_pcm_substream *substream,
			   unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);
	int id = snd_soc_rtd_to_cpu(rtd, 0)->id;

	return mt6797_rate_transform(afe->dev, rate, id);
}

static int mt6797_irq_fs(struct snd_pcm_substream *substream, unsigned int rate)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);
	struct mtk_base_afe *afe = snd_soc_component_get_drvdata(component);

	return mt6797_general_rate_transform(afe->dev, rate);
}

#define MTK_PCM_RATES (SNDRV_PCM_RATE_8000_48000 |\
		       SNDRV_PCM_RATE_88200 |\
		       SNDRV_PCM_RATE_96000 |\
		       SNDRV_PCM_RATE_176400 |\
		       SNDRV_PCM_RATE_192000)

#define MTK_PCM_DAI_RATES (SNDRV_PCM_RATE_8000 |\
			   SNDRV_PCM_RATE_16000 |\
			   SNDRV_PCM_RATE_32000)

#define MTK_PCM_FORMATS (SNDRV_PCM_FMTBIT_S16_LE |\
			 SNDRV_PCM_FMTBIT_S24_LE |\
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver mt6797_memif_dai_driver[] = {
	/* FE DAIs: memory intefaces to CPU */
	{
		.name = "DL1",
		.id = MT6797_MEMIF_DL1,
		.playback = {
			.stream_name = "DL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "DL2",
		.id = MT6797_MEMIF_DL2,
		.playback = {
			.stream_name = "DL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "DL3",
		.id = MT6797_MEMIF_DL3,
		.playback = {
			.stream_name = "DL3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL1",
		.id = MT6797_MEMIF_VUL12,
		.capture = {
			.stream_name = "UL1",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL2",
		.id = MT6797_MEMIF_AWB,
		.capture = {
			.stream_name = "UL2",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL3",
		.id = MT6797_MEMIF_VUL,
		.capture = {
			.stream_name = "UL3",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MTK_PCM_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL_MONO_1",
		.id = MT6797_MEMIF_MOD_DAI,
		.capture = {
			.stream_name = "UL_MONO_1",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MTK_PCM_DAI_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
	{
		.name = "UL_MONO_2",
		.id = MT6797_MEMIF_DAI,
		.capture = {
			.stream_name = "UL_MONO_2",
			.channels_min = 1,
			.channels_max = 1,
			.rates = MTK_PCM_DAI_RATES,
			.formats = MTK_PCM_FORMATS,
		},
		.ops = &mtk_afe_fe_ops,
	},
};

/* dma widget & routes*/
static const struct snd_kcontrol_new memif_ul1_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN21,
				    I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul1_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN22,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN5,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH1", AFE_CONN5,
				    I_DL1_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH1", AFE_CONN5,
				    I_DL2_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH1", AFE_CONN5,
				    I_DL3_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul2_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN6,
				    I_ADDA_UL_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL1_CH2", AFE_CONN6,
				    I_DL1_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL2_CH2", AFE_CONN6,
				    I_DL2_CH2, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("DL3_CH2", AFE_CONN6,
				    I_DL3_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN9,
				    I_ADDA_UL_CH1, 1, 0),
};

static const struct snd_kcontrol_new memif_ul3_ch2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN10,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_mono_1_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN12,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN12,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_kcontrol_new memif_ul_mono_2_mix[] = {
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH1", AFE_CONN11,
				    I_ADDA_UL_CH1, 1, 0),
	SOC_DAPM_SINGLE_AUTODISABLE("ADDA_UL_CH2", AFE_CONN11,
				    I_ADDA_UL_CH2, 1, 0),
};

static const struct snd_soc_dapm_widget mt6797_memif_widgets[] = {
	/* memif */
	SND_SOC_DAPM_MIXER("UL1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch1_mix, ARRAY_SIZE(memif_ul1_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL1_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul1_ch2_mix, ARRAY_SIZE(memif_ul1_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch1_mix, ARRAY_SIZE(memif_ul2_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL2_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul2_ch2_mix, ARRAY_SIZE(memif_ul2_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL3_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch1_mix, ARRAY_SIZE(memif_ul3_ch1_mix)),
	SND_SOC_DAPM_MIXER("UL3_CH2", SND_SOC_NOPM, 0, 0,
			   memif_ul3_ch2_mix, ARRAY_SIZE(memif_ul3_ch2_mix)),

	SND_SOC_DAPM_MIXER("UL_MONO_1_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_mono_1_mix,
			   ARRAY_SIZE(memif_ul_mono_1_mix)),

	SND_SOC_DAPM_MIXER("UL_MONO_2_CH1", SND_SOC_NOPM, 0, 0,
			   memif_ul_mono_2_mix,
			   ARRAY_SIZE(memif_ul_mono_2_mix)),
};

static const struct snd_soc_dapm_route mt6797_memif_routes[] = {
	/* capture */
	{"UL1", NULL, "UL1_CH1"},
	{"UL1", NULL, "UL1_CH2"},
	{"UL1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL1_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL2", NULL, "UL2_CH1"},
	{"UL2", NULL, "UL2_CH2"},
	{"UL2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL2_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL3", NULL, "UL3_CH1"},
	{"UL3", NULL, "UL3_CH2"},
	{"UL3_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL3_CH2", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL_MONO_1", NULL, "UL_MONO_1_CH1"},
	{"UL_MONO_1_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL_MONO_1_CH1", "ADDA_UL_CH2", "ADDA Capture"},

	{"UL_MONO_2", NULL, "UL_MONO_2_CH1"},
	{"UL_MONO_2_CH1", "ADDA_UL_CH1", "ADDA Capture"},
	{"UL_MONO_2_CH1", "ADDA_UL_CH2", "ADDA Capture"},
};

static const struct snd_soc_component_driver mt6797_afe_pcm_dai_component = {
	.name = "mt6797-afe-pcm-dai",
};

static const struct mtk_base_memif_data memif_data[MT6797_MEMIF_NUM] = {
	[MT6797_MEMIF_DL1] = {
		.name = "DL1",
		.id = MT6797_MEMIF_DL1,
		.reg_ofs_base = AFE_DL1_BASE,
		.reg_ofs_cur = AFE_DL1_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = DL1_MODE_SFT,
		.fs_maskbit = DL1_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = DL1_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DL1_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DL1_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_DL2] = {
		.name = "DL2",
		.id = MT6797_MEMIF_DL2,
		.reg_ofs_base = AFE_DL2_BASE,
		.reg_ofs_cur = AFE_DL2_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = DL2_MODE_SFT,
		.fs_maskbit = DL2_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = DL2_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DL2_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DL2_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_DL3] = {
		.name = "DL3",
		.id = MT6797_MEMIF_DL3,
		.reg_ofs_base = AFE_DL3_BASE,
		.reg_ofs_cur = AFE_DL3_CUR,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = DL3_MODE_SFT,
		.fs_maskbit = DL3_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = DL3_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DL3_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DL3_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_VUL] = {
		.name = "VUL",
		.id = MT6797_MEMIF_VUL,
		.reg_ofs_base = AFE_VUL_BASE,
		.reg_ofs_cur = AFE_VUL_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = VUL_MODE_SFT,
		.fs_maskbit = VUL_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = VUL_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = VUL_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = VUL_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_AWB] = {
		.name = "AWB",
		.id = MT6797_MEMIF_AWB,
		.reg_ofs_base = AFE_AWB_BASE,
		.reg_ofs_cur = AFE_AWB_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = AWB_MODE_SFT,
		.fs_maskbit = AWB_MODE_MASK,
		.mono_reg = AFE_DAC_CON1,
		.mono_shift = AWB_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = AWB_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = AWB_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_VUL12] = {
		.name = "VUL12",
		.id = MT6797_MEMIF_VUL12,
		.reg_ofs_base = AFE_VUL_D2_BASE,
		.reg_ofs_cur = AFE_VUL_D2_CUR,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = VUL_DATA2_MODE_SFT,
		.fs_maskbit = VUL_DATA2_MODE_MASK,
		.mono_reg = AFE_DAC_CON0,
		.mono_shift = VUL_DATA2_DATA_SFT,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = VUL_DATA2_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = VUL_DATA2_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_DAI] = {
		.name = "DAI",
		.id = MT6797_MEMIF_DAI,
		.reg_ofs_base = AFE_DAI_BASE,
		.reg_ofs_cur = AFE_DAI_CUR,
		.fs_reg = AFE_DAC_CON0,
		.fs_shift = DAI_MODE_SFT,
		.fs_maskbit = DAI_MODE_MASK,
		.mono_reg = -1,
		.mono_shift = 0,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = DAI_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = DAI_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
	[MT6797_MEMIF_MOD_DAI] = {
		.name = "MOD_DAI",
		.id = MT6797_MEMIF_MOD_DAI,
		.reg_ofs_base = AFE_MOD_DAI_BASE,
		.reg_ofs_cur = AFE_MOD_DAI_CUR,
		.fs_reg = AFE_DAC_CON1,
		.fs_shift = MOD_DAI_MODE_SFT,
		.fs_maskbit = MOD_DAI_MODE_MASK,
		.mono_reg = -1,
		.mono_shift = 0,
		.enable_reg = AFE_DAC_CON0,
		.enable_shift = MOD_DAI_ON_SFT,
		.hd_reg = AFE_MEMIF_HD_MODE,
		.hd_shift = MOD_DAI_HD_SFT,
		.agent_disable_reg = -1,
		.msb_reg = -1,
	},
};

static const struct mtk_base_irq_data irq_data[MT6797_IRQ_NUM] = {
	[MT6797_IRQ_1] = {
		.id = MT6797_IRQ_1,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT1,
		.irq_cnt_shift = AFE_IRQ_MCU_CNT1_SFT,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT1_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ1_MCU_MODE_SFT,
		.irq_fs_maskbit = IRQ1_MCU_MODE_MASK,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ1_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ1_MCU_CLR_SFT,
		.irq_status_shift = IRQ1_MCU_CLR_SFT,
	},
	[MT6797_IRQ_2] = {
		.id = MT6797_IRQ_2,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT2,
		.irq_cnt_shift = AFE_IRQ_MCU_CNT2_SFT,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT2_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ2_MCU_MODE_SFT,
		.irq_fs_maskbit = IRQ2_MCU_MODE_MASK,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ2_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ2_MCU_CLR_SFT,
		.irq_status_shift = IRQ2_MCU_CLR_SFT,
	},
	[MT6797_IRQ_3] = {
		.id = MT6797_IRQ_3,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT3,
		.irq_cnt_shift = AFE_IRQ_MCU_CNT3_SFT,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT3_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ3_MCU_MODE_SFT,
		.irq_fs_maskbit = IRQ3_MCU_MODE_MASK,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ3_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ3_MCU_CLR_SFT,
		.irq_status_shift = IRQ3_MCU_CLR_SFT,
	},
	[MT6797_IRQ_4] = {
		.id = MT6797_IRQ_4,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT4,
		.irq_cnt_shift = AFE_IRQ_MCU_CNT4_SFT,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT4_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ4_MCU_MODE_SFT,
		.irq_fs_maskbit = IRQ4_MCU_MODE_MASK,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ4_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ4_MCU_CLR_SFT,
		.irq_status_shift = IRQ4_MCU_CLR_SFT,
	},
	[MT6797_IRQ_7] = {
		.id = MT6797_IRQ_7,
		.irq_cnt_reg = AFE_IRQ_MCU_CNT7,
		.irq_cnt_shift = AFE_IRQ_MCU_CNT7_SFT,
		.irq_cnt_maskbit = AFE_IRQ_MCU_CNT7_MASK,
		.irq_fs_reg = AFE_IRQ_MCU_CON,
		.irq_fs_shift = IRQ7_MCU_MODE_SFT,
		.irq_fs_maskbit = IRQ7_MCU_MODE_MASK,
		.irq_en_reg = AFE_IRQ_MCU_CON,
		.irq_en_shift = IRQ7_MCU_ON_SFT,
		.irq_clr_reg = AFE_IRQ_MCU_CLR,
		.irq_clr_shift = IRQ7_MCU_CLR_SFT,
		.irq_status_shift = IRQ7_MCU_CLR_SFT,
	},
};

static const struct regmap_config mt6797_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = AFE_MAX_REGISTER,
};

static irqreturn_t mt6797_afe_irq_handler(int irq_id, void *dev)
{
	struct mtk_base_afe *afe = dev;
	irqreturn_t handled = IRQ_NONE;
	int i;

	for (i = 0; i < MT6797_MEMIF_NUM; i++) {
		struct mtk_base_afe_memif *memif = &afe->memif[i];
		struct snd_pcm_substream *substream;
		unsigned int status, enabled, mask;
		unsigned long flags;
		int slot;

		/* Pairs with FE startup; shutdown drains before releasing slot. */
		substream = smp_load_acquire(&memif->substream);
		if (!substream)
			continue;
		snd_pcm_stream_lock_irqsave(substream, flags);
		slot = READ_ONCE(memif->irq_usage);
		if (slot < 0 || slot >= afe->irqs_size)
			goto unlock;
		/* Status, MCU routing and W1C share the clear bit, not ON. */
		mask = BIT(afe->irqs[slot].irq_data->irq_status_shift);
		/*
		 * Read and acknowledge under this stream's trigger lock. Never
		 * carry another FE's status across stop/start or slot reuse.
		 */
		if (regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status) ||
		    regmap_read(afe->regmap, AFE_IRQ_MCU_EN, &enabled) ||
		    !(status & mask))
			goto unlock;
		if (regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, mask))
			goto unlock;
		handled = IRQ_HANDLED;
		if ((enabled & mask) && snd_pcm_running(substream))
			snd_pcm_period_elapsed_under_stream_lock(substream);
unlock:
		snd_pcm_stream_unlock_irqrestore(substream, flags);
	}
	return handled;
}

static int mt6797_memif_validate(struct mtk_base_afe *afe, int id)
{
	const struct mtk_base_memif_data *data = afe->memif[id].data;

	if (data->enable_reg != AFE_DAC_CON0 ||
	    data->enable_shift <= AFE_ON_SFT || data->enable_shift >= 32)
		return -EINVAL;
	return 0;
}

static int mt6797_memif_start_check(struct mtk_base_afe *afe, int id)
{
	struct mt6797_afe_private *priv = afe->platform_priv;

	if (!priv->clocks_enabled || afe->hardware_dirty || afe->shutting_down)
		return -EIO;
	return mt6797_memif_validate(afe, id);
}

static int mt6797_memif_stop(struct mtk_base_afe *afe, int id)
{
	struct mt6797_afe_private *priv = afe->platform_priv;
	const struct mtk_base_memif_data *data = afe->memif[id].data;
	unsigned int value, mask;
	int attempt, ret;

	ret = mt6797_memif_validate(afe, id);
	if (ret)
		return ret;
	if (!priv->clocks_enabled)
		return -EIO;
	mask = BIT(data->enable_shift);
	/*
	 * Powered uncached MMIO: readback completes the disable access.
	 * This is not an independent outstanding-bus-transaction counter.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		ret = regmap_update_bits(afe->regmap, data->enable_reg, mask, 0);
		if (ret)
			continue;
		ret = regmap_read(afe->regmap, data->enable_reg, &value);
		if (!ret && !(value & mask))
			return 0;
		ret = ret ?: -EIO;
	}
	dev_err(afe->dev, "memif %d disable unconfirmed: %d (device fault)\n",
		id, ret);
	return ret;
}

static int mt6797_afe_runtime_suspend(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6797_afe_private *priv = afe->platform_priv;
	unsigned int value;
	int ret, restore, i;

	if (!priv->clocks_enabled)
		return 0;

	/* A failed FE stop must not silently lose its clock ownership. */
	for (i = 0; i < afe->memif_size; i++)
		if (afe->memif[i].trigger_active || afe->memif[i].trigger_dirty)
			return -EBUSY;

	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON_MASK_SFT, 0);
	if (ret)
		goto restore;
	ret = regmap_read_poll_timeout(afe->regmap, AFE_DAC_CON0, value,
				       !(value & AFE_ON_RETM_MASK_SFT), 10, 10000);
	if (ret)
		goto restore;
	/* W1C must be a write, not a read/modify/write. */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_STATUS_BITS);
	if (ret)
		goto restore;
	afe->hardware_dirty = false;
	ret = mt6797_afe_disable_clock(afe);
	if (!ret)
		priv->clocks_enabled = false;
	return ret;

restore:
	/* PM leaves us active on failure. Keep clocks and attempt restoration. */
	restore = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				     AFE_ON_MASK_SFT, AFE_ON_MASK_SFT);
	afe->hardware_dirty = !!restore;
	if (restore)
		dev_err(dev, "AFE suspend restoration failed: %d\n", restore);
	return ret;
}

static int mt6797_afe_runtime_resume(struct device *dev)
{
	struct mtk_base_afe *afe = dev_get_drvdata(dev);
	struct mt6797_afe_private *priv = afe->platform_priv;
	int ret, cleanup;

	if (!priv->clocks_enabled) {
		ret = mt6797_afe_enable_clock(afe);
		if (ret)
			return ret;
		priv->clocks_enabled = true;
	}

	/* irq signal to mcu only */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_EN, AFE_IRQ_MCU_EN_MASK_SFT);
	if (ret)
		goto err_disable_clock;

	/* force all memif use normal mode */
	ret = regmap_update_bits(afe->regmap, AFE_MEMIF_HDALIGN,
				 0x7ff << 16, 0x7ff << 16);
	if (ret)
		goto err_disable_clock;
	/* force cpu use normal mode when access sram data */
	ret = regmap_update_bits(afe->regmap, AFE_MEMIF_MSB,
				 CPU_COMPACT_MODE_MASK_SFT, 0);
	if (ret)
		goto err_disable_clock;
	/* force cpu use 8_24 format when writing 32bit data */
	ret = regmap_update_bits(afe->regmap, AFE_MEMIF_MSB,
				 CPU_HD_ALIGN_MASK_SFT, 0);
	if (ret)
		goto err_disable_clock;

	/* set all output port to 24bit */
	ret = regmap_update_bits(afe->regmap, AFE_CONN_24BIT,
				 0x3fffffff, 0x3fffffff);
	if (ret)
		goto err_disable_clock;

	/* enable AFE */
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				 AFE_ON_MASK_SFT,
				 0x1 << AFE_ON_SFT);
	if (ret)
		goto err_disable_clock;

	afe->hardware_dirty = false;
	return 0;

err_disable_clock:
	afe->hardware_dirty = true;
	cleanup = mt6797_afe_runtime_suspend(dev);
	if (cleanup) {
		afe->hardware_dirty = true;
		dev_err(dev, "AFE resume cleanup failed: %d\n", cleanup);
	}
	return ret;
}

static int mt6797_dai_memif_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mt6797_memif_dai_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mt6797_memif_dai_driver);

	dai->dapm_widgets = mt6797_memif_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mt6797_memif_widgets);
	dai->dapm_routes = mt6797_memif_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mt6797_memif_routes);
	return 0;
}

typedef int (*dai_register_cb)(struct mtk_base_afe *);
static const dai_register_cb dai_register_cbs[] = {
	mt6797_dai_adda_register,
	mt6797_dai_pcm_register,
	mt6797_dai_hostless_register,
	mt6797_dai_memif_register,
};

static void mt6797_afe_sync_irq(struct mtk_base_afe *afe)
{
	struct mt6797_afe_private *priv = afe->platform_priv;

	synchronize_irq(priv->irq);
}

static void mt6797_afe_power_down(void *data)
{
	struct device *dev = data;
	int ret;

	if (pm_runtime_enabled(dev)) {
		ret = pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
	} else {
		ret = mt6797_afe_runtime_suspend(dev);
	}
	if (ret < 0)
		dev_err(dev, "AFE power-down incomplete: %d\n", ret);
}

static void mt6797_afe_pcm_shutdown(struct platform_device *pdev)
{
	struct mtk_base_afe *afe = platform_get_drvdata(pdev);
	struct mt6797_afe_private *priv = afe->platform_priv;
	unsigned long flags;
	unsigned int value;
	int i, ret = 0, err;

	/*
	 * Never borrow substream pointers here: close may run concurrently.
	 * Triggers take stream lock then memif_lock; this path takes only
	 * memif_lock, and drains handlers after dropping it.
	 */
	spin_lock_irqsave(&afe->memif_lock, flags);
	afe->shutting_down = true;
	if (!priv->clocks_enabled) {
		spin_unlock_irqrestore(&afe->memif_lock, flags);
		return;
	}
	for (i = 0; i < afe->memif_size; i++) {
		err = mt6797_memif_stop(afe, i);
		if (!err)
			afe->memif[i].trigger_active = false;
		afe->memif[i].trigger_dirty = !!err;
		ret = ret ?: err;
	}
	for (i = 0; i < afe->irqs_size; i++) {
		const struct mtk_base_irq_data *data = afe->irqs[i].irq_data;

		err = regmap_update_bits(afe->regmap, data->irq_en_reg,
					 BIT(data->irq_en_shift), 0);
		ret = ret ?: err;
	}
	err = regmap_write(afe->regmap, AFE_IRQ_MCU_EN, 0);
	ret = ret ?: err;
	err = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, AFE_IRQ_STATUS_BITS);
	ret = ret ?: err;
	/* Do not release slots whose generator cleanup is unconfirmed. */
	if (ret)
		for (i = 0; i < afe->memif_size; i++)
			afe->memif[i].trigger_dirty = true;
	spin_unlock_irqrestore(&afe->memif_lock, flags);
	mt6797_afe_sync_irq(afe);
	if (!ret) {
		ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0, AFE_ON_MASK_SFT, 0);
		if (!ret)
			ret = regmap_read_poll_timeout(afe->regmap, AFE_DAC_CON0, value,
						       !(value & AFE_ON_RETM_MASK_SFT),
						       10, 10000);
	}
	/* Keep the lifetime PM hold; devres is not run by device_shutdown. */
	if (ret)
		dev_err(afe->dev, "AFE shutdown incomplete: %d\n", ret);
}

static int mt6797_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct mtk_base_afe *afe;
	struct mt6797_afe_private *afe_priv;
	struct device *dev;
	int i, irq_id, ret;

	afe = devm_kzalloc(&pdev->dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;

	afe->platform_priv = devm_kzalloc(&pdev->dev, sizeof(*afe_priv),
					  GFP_KERNEL);
	if (!afe->platform_priv)
		return -ENOMEM;

	afe_priv = afe->platform_priv;
	afe->dev = &pdev->dev;
	dev = afe->dev;

	/* initial audio related clock */
	ret = mt6797_init_clock(afe);
	if (ret) {
		dev_err(dev, "init clock error\n");
		return ret;
	}

	/* regmap init */
	afe->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(afe->base_addr))
		return PTR_ERR(afe->base_addr);

	afe->regmap = devm_regmap_init_mmio(&pdev->dev, afe->base_addr,
					    &mt6797_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return PTR_ERR(afe->regmap);

	/* init memif */
	afe->memif_size = MT6797_MEMIF_NUM;
	afe->memif = devm_kcalloc(dev, afe->memif_size, sizeof(*afe->memif),
				  GFP_KERNEL);
	if (!afe->memif)
		return -ENOMEM;

	for (i = 0; i < afe->memif_size; i++) {
		afe->memif[i].data = &memif_data[i];
		afe->memif[i].irq_usage = -1;
	}

	mutex_init(&afe->irq_alloc_lock);
	spin_lock_init(&afe->memif_lock);
	afe->memif_stop = mt6797_memif_stop;
	afe->memif_start_check = mt6797_memif_start_check;

	/* irq initialize */
	afe->irqs_size = MT6797_IRQ_NUM;
	afe->irqs = devm_kcalloc(dev, afe->irqs_size, sizeof(*afe->irqs),
				 GFP_KERNEL);
	if (!afe->irqs)
		return -ENOMEM;

	for (i = 0; i < afe->irqs_size; i++)
		afe->irqs[i].irq_data = &irq_data[i];

	/* request irq */
	irq_id = platform_get_irq(pdev, 0);
	if (irq_id < 0)
		return irq_id;


	/* init sub_dais */
	INIT_LIST_HEAD(&afe->sub_dais);

	for (i = 0; i < ARRAY_SIZE(dai_register_cbs); i++) {
		ret = dai_register_cbs[i](afe);
		if (ret) {
			dev_warn(afe->dev, "dai register i %d fail, ret %d\n",
				 i, ret);
			return ret;
		}
	}

	/* init dai_driver and component_driver */
	ret = mtk_afe_combine_sub_dai(afe);
	if (ret) {
		dev_warn(afe->dev, "mtk_afe_combine_sub_dai fail, ret %d\n",
			 ret);
		return ret;
	}

	afe->mtk_afe_hardware = &mt6797_afe_hardware;
	afe->memif_fs = mt6797_memif_fs;
	afe->irq_fs = mt6797_irq_fs;

	afe->runtime_resume = mt6797_afe_runtime_resume;
	afe->runtime_suspend = mt6797_afe_runtime_suspend;

	platform_set_drvdata(pdev, afe);

	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = mt6797_afe_runtime_resume(dev);
		if (ret)
			return ret;
	} else {
		ret = pm_runtime_resume_and_get(dev);
		if (ret) {
			pm_runtime_disable(dev);
			return ret;
		}
	}

	/* Registered before IRQ/components: devres releases those first. */
	ret = devm_add_action_or_reset(dev, mt6797_afe_power_down, dev);
	if (ret)
		return ret;
	afe_priv->irq = irq_id;
	afe->synchronize_irq = mt6797_afe_sync_irq;
	ret = devm_request_irq(dev, irq_id, mt6797_afe_irq_handler,
			       IRQF_TRIGGER_NONE, "asys-isr", afe);
	if (ret)
		return ret;

	/* register component */
	ret = devm_snd_soc_register_component(dev, &mtk_afe_pcm_platform,
					      NULL, 0);
	if (ret) {
		dev_warn(dev, "err_platform\n");
		return ret;
	}

	ret = devm_snd_soc_register_component(afe->dev,
					      &mt6797_afe_pcm_dai_component,
					      afe->dai_drivers,
					      afe->num_dai_drivers);
	if (ret) {
		dev_warn(dev, "err_dai_component\n");
		return ret;
	}

	return 0;
}

static const struct of_device_id mt6797_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6797-audio", },
	{},
};
MODULE_DEVICE_TABLE(of, mt6797_afe_pcm_dt_match);

static const struct dev_pm_ops mt6797_afe_pm_ops = {
	RUNTIME_PM_OPS(mt6797_afe_runtime_suspend,
		       mt6797_afe_runtime_resume, NULL)
};

static struct platform_driver mt6797_afe_pcm_driver = {
	.driver = {
		   .name = "mt6797-audio",
		   .of_match_table = mt6797_afe_pcm_dt_match,
		   .pm = pm_ptr(&mt6797_afe_pm_ops),
	},
	.probe = mt6797_afe_pcm_dev_probe,
	.shutdown = mt6797_afe_pcm_shutdown,
};

module_platform_driver(mt6797_afe_pcm_driver);

MODULE_DESCRIPTION("Mediatek ALSA SoC AFE platform driver for 6797");
MODULE_AUTHOR("KaiChieh Chuang <kaichieh.chuang@mediatek.com>");
MODULE_LICENSE("GPL v2");
