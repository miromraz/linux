// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include <sound/cs35l41.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "usb_offload_utils.h"
#include "sdw.h"

#define MI2S_BCLK_RATE		1536000
#define SEC_TDM_BCLK_RATE	1536000	/* 2 slots x 16 bit @ 48 kHz */
#define TER_TDM_BCLK_RATE	1536000	/* 2 slots x 16 bit @ 48 kHz */

/*
 * RT5514 clocking. Values mirror the codec enums/rate in
 * sound/soc/codecs/rt5514.h (RT5514_SCLK_S_PLL1, RT5514_PLL1_S_BCLK); that
 * codec header is not exported, so define them locally. The board wires no
 * MCLK to the RT5514, so its 12.288 MHz sysclk is derived from the TDM BCLK
 * via the codec PLL.
 */
#define RT5514_PLL1_S_BCLK	1
#define RT5514_SCLK_S_PLL1	1
#define RT5514_SYSCLK_RATE	12288000

struct sm8250_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack usb_offload_jack;
	bool usb_offload_jack_setup;
	struct snd_soc_jack dp_jack;
	bool jack_setup;
};

static int sm8250_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX:
		return qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);
	case USB_RX:
		return qcom_snd_usb_offload_jack_setup(rtd, &data->usb_offload_jack,
						       &data->usb_offload_jack_setup);
	default:
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
	}
}

static void sm8250_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == USB_RX)
		qcom_snd_usb_offload_jack_remove(rtd,
						 &data->usb_offload_jack_setup);

}

static int sm8250_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8250_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case SECONDARY_TDM_RX_0: {
		/* sunfish: stereo CS35L41 amps on secondary TDM, 2 slots x 16 bit
		 * @ 48 kHz (matches the stock sec TDM config: internal sync,
		 * inverted fsync, 1 bit clock data delay). */
		int j;

		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_TDM_IBIT,
			SEC_TDM_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);

		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			unsigned int slot[1];

			snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_BC_FC |
				SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_NB_IF);
			/* CS35L41 PLL refclk = SCLK (BCLK) so the amp powers up */
			snd_soc_dai_set_sysclk(codec_dai, CS35L41_CLKID_SCLK,
				SEC_TDM_BCLK_RATE, SND_SOC_CLOCK_IN);
			snd_soc_component_set_sysclk(codec_dai->component,
				CS35L41_CLKID_SCLK, 0, SEC_TDM_BCLK_RATE,
				SND_SOC_CLOCK_IN);
			/* EAR amp on slot 0, SPK amp on slot 1 */
			slot[0] = (codec_dai->component->name_prefix &&
				   !strcmp(codec_dai->component->name_prefix, "EAR"))
				   ? 0 : 1;
			snd_soc_dai_set_channel_map(codec_dai, 0, NULL, 1, slot);
		}
		break;
	}
	case TERTIARY_TDM_TX_0:
		/* sunfish: RT5514 mic on tertiary TDM, capture, 2 slots x
		 * 16 bit @ 48 kHz (same stock TDM config as the sec TDM). */
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_TER_TDM_IBIT,
			TER_TDM_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
		break;
	case QUINARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	default:
		break;
	}

	return qcom_snd_sdw_startup(substream);
}

static int sm8250_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	unsigned int tdm_offset[2] = { 0, 2 };
	int ret;

	switch (cpu_dai->id) {
	case SECONDARY_TDM_RX_0:
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3, 2, 16);
		if (ret < 0) {
			dev_err(rtd->dev, "failed to set tdm slots: %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL, 2,
						  tdm_offset);
		if (ret < 0) {
			dev_err(rtd->dev, "failed to set channel map: %d\n",
				ret);
			return ret;
		}
		break;
	case TERTIARY_TDM_TX_0: {
		/* tert TDM TX id is odd -> q6afe uses the tx mask/slots.
		 * RT5514 drives 2 x 16 bit slots into the SoC; the AFE slot
		 * mapping wants per-channel BYTE offsets in the frame (0 and
		 * 2 for 16-bit slots), same as the sec TDM speaker path. */
		unsigned int slot[2] = { 0, 2 };
		struct snd_soc_dai *codec_dai;
		int j;

		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0x3, 0, 2, 16);
		if (ret < 0) {
			dev_err(rtd->dev, "failed to set tdm slots: %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai, 2, slot, 0, NULL);
		if (ret < 0) {
			dev_err(rtd->dev, "failed to set channel map: %d\n",
				ret);
			return ret;
		}

		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			/* No MCLK on this board: lock the RT5514 PLL to the
			 * TDM bit clock and use it as the 12.288 MHz sysclk. */
			snd_soc_dai_set_pll(codec_dai, 0, RT5514_PLL1_S_BCLK,
				TER_TDM_BCLK_RATE, RT5514_SYSCLK_RATE);
			snd_soc_dai_set_sysclk(codec_dai, RT5514_SCLK_S_PLL1,
				RT5514_SYSCLK_RATE, SND_SOC_CLOCK_IN);
			snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_BC_FC |
				SND_SOC_DAIFMT_DSP_A | SND_SOC_DAIFMT_NB_IF);
			snd_soc_dai_set_tdm_slot(codec_dai, 0x3, 0, 2, 16);
		}
		break;
	}
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_ops sm8250_be_ops = {
	.startup = sm8250_snd_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_params = sm8250_snd_hw_params,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = &sm8250_be_ops;
		}
	}
}

static int sm8250_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8250_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	sm8250_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sm8250_dt_match[] = {
	{ .compatible = "fairphone,fp4-sndcard", .data = "sm7225" },
	{ .compatible = "fairphone,fp5-sndcard", .data = "qcm6490" },
	{ .compatible = "qcom,qrb2210-sndcard", .data = "qcm2290" },
	{ .compatible = "qcom,qrb4210-rb2-sndcard", .data = "sm4250" },
	{ .compatible = "qcom,qrb5165-rb5-sndcard", .data = "sm8250" },
	{ .compatible = "qcom,sm8250-sndcard", .data = "sm8250" },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8250_dt_match);

static struct platform_driver snd_sm8250_driver = {
	.probe  = sm8250_platform_probe,
	.driver = {
		.name = "snd-sm8250",
		.of_match_table = snd_sm8250_dt_match,
	},
};
module_platform_driver(snd_sm8250_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SM8250 ASoC Machine Driver");
MODULE_LICENSE("GPL");
