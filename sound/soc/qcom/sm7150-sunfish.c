// SPDX-License-Identifier: GPL-2.0-only
/*
 * sm7150-sunfish.c - ASoC machine driver for Google Pixel 4a (sunfish)
 *
 * Audio chain: Q6 AFE (MultiMedia1) -> Q6 I2S (TERTIARY_MI2S_RX) -> CS35L41
 * ADSP is used for Q6 AFE routing.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <uapi/linux/input-event-codes.h>

#include "common.h"

static int sunfish_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			    SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm7150_sunfish_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card;
	struct snd_soc_dai_link *link;
	int i, ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->dev = dev;
	card->owner = THIS_MODULE;

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	/* Add BE hw_params fixup for Q6 AFE backend links */
	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm) {
			link->be_hw_params_fixup = sunfish_be_hw_params_fixup;
			link->ignore_pmdown_time = 1;
			link->ignore_suspend = 1;
			link->nonatomic = 1;
		}
	}

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id sm7150_sunfish_device_id[] = {
	{ .compatible = "qcom,sm7150-sunfish-sndcard" },
	{}
};
MODULE_DEVICE_TABLE(of, sm7150_sunfish_device_id);

static struct platform_driver sm7150_sunfish_driver = {
	.probe = sm7150_sunfish_platform_probe,
	.driver = {
		.name = "qcom-sm7150-sunfish",
		.of_match_table = sm7150_sunfish_device_id,
	},
};
module_platform_driver(sm7150_sunfish_driver);

MODULE_DESCRIPTION("SM7150 Sunfish ASoC Machine Driver");
MODULE_AUTHOR("Mainline kernel contributors");
MODULE_LICENSE("GPL v2");
