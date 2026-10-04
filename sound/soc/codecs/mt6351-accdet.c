// SPDX-License-Identifier: GPL-2.0
//
// mt6351-accdet.c  --  ALSA SoC MT6351 accessory detection driver
//
// Copyright (C) 2026 Miroslav Mraz <miroslav.mraz@techmania.cz>
//
// Based on MediaTek's GPL ACCDET driver for the MT6351 PMIC
// (drivers/misc/mediatek/accdet/mt6797/accdet.c), Copyright (C) 2015
// MediaTek Inc.
//
// The MT6351 PMIC embeds an older generation of the MediaTek ACCDET
// (accessory detection) block than the MT6359 handled by mt6359-accdet.c:
// it exposes a single internal EINT whose polarity is flipped in software to
// track plug in/out, a CON0..CON25 register array, and drives the micbias
// through the audio codec analog registers (AUDENC_ANA_CON10/11).  The
// accessory voltage used for button decoding is read through the PMIC AUXADC
// (channel MT6351_AUXADC_ACCDET) via the IIO consumer API.

#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "mt6351-accdet.h"

static const struct snd_soc_component_driver mt6351_accdet_soc_driver = {
	.name = "mt6351-accdet",
};

static void mt6351_accdet_jack_report(struct mt6351_accdet *priv)
{
	if (!priv->jack)
		return;

	snd_soc_jack_report(priv->jack, priv->jack_type | priv->btn_type,
			    MT6351_ACCDET_JACK_MASK);
}

/* Read the accessory voltage on the AUXADC channel, in millivolts. */
static int mt6351_accdet_read_voltage(struct mt6351_accdet *priv)
{
	int val, ret;

	ret = iio_read_channel_raw(priv->adc, &val);
	if (ret < 0)
		return ret;

	/* the raw sample is taken against a 1.8 V reference */
	return val * MT6351_ACCDET_AUXADC_MV / MT6351_ACCDET_AUXADC_RES;
}

static void mt6351_accdet_check_button(struct mt6351_accdet *priv,
				       unsigned int v)
{
	struct mt6351_accdet_data *d = &priv->data;

	if (d->four_key_mode) {
		if (v < d->four_key.down && v >= d->four_key.up)
			priv->btn_type = SND_JACK_BTN_1;
		else if (v < d->four_key.up && v >= d->four_key.voice)
			priv->btn_type = SND_JACK_BTN_2;
		else if (v < d->four_key.voice && v >= d->four_key.mid)
			priv->btn_type = SND_JACK_BTN_3;
		else if (v < d->four_key.mid)
			priv->btn_type = SND_JACK_BTN_0;
	} else {
		if (v < d->three_key.down && v >= d->three_key.up)
			priv->btn_type = SND_JACK_BTN_1;
		else if (v < d->three_key.up && v >= d->three_key.mid)
			priv->btn_type = SND_JACK_BTN_2;
		else if (v < d->three_key.mid)
			priv->btn_type = SND_JACK_BTN_0;
	}
}

/*
 * The comparator A/B state (ACCDET_CON14[7:6]) tells headphone from headset
 * and, while a headset is plugged, a button press from a release.
 */
static void mt6351_accdet_check_jack(struct mt6351_accdet *priv)
{
	unsigned int val;
	int v;

	regmap_read(priv->regmap, MT6351_ACCDET_CON14, &val);
	priv->accdet_status =
		(val >> ACCDET_STATE_AB_SHIFT) & ACCDET_STATE_AB_MASK;

	switch (priv->accdet_status) {
	case 0:
		if (priv->jack_type == SND_JACK_HEADSET) {
			v = mt6351_accdet_read_voltage(priv);
			if (v >= 0)
				mt6351_accdet_check_button(priv, v);
		} else {
			priv->jack_type = SND_JACK_HEADPHONE;
		}
		break;
	case 1:
		if (priv->jack_type == SND_JACK_HEADSET) {
			/* button released */
			priv->btn_type = 0;
		} else {
			priv->jack_type = SND_JACK_HEADSET;
			regmap_write(priv->regmap, MT6351_ACCDET_CON9,
				     priv->data.deb.debounce3 * 30);
		}
		break;
	default:
		priv->jack_type = 0;
		priv->btn_type = 0;
		break;
	}
}

/* Enable the ACCDET unit to run cable/button detection while plugged in. */
static void mt6351_accdet_enable(struct mt6351_accdet *priv)
{
	regmap_write(priv->regmap, MT6351_TOP_CKPDN_CON2_CLR, ACCDET_CK_PDN);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON2,
			   ACCDET_SWCTRL_IDLE_EN, ACCDET_SWCTRL_IDLE_EN);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON2,
			   ACCDET_SWCTRL_EN, ACCDET_SWCTRL_EN);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON1,
			   ACCDET_EN, ACCDET_EN);
}

/* Stop cable/button detection, keep the EINT PWM alive for plug detection. */
static void mt6351_accdet_disable(struct mt6351_accdet *priv)
{
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON1, ACCDET_EN, 0);
	regmap_write(priv->regmap, MT6351_ACCDET_CON2, ACCDET_EINT_PWM_EN);
}

static void mt6351_accdet_handle_eint(struct mt6351_accdet *priv)
{
	unsigned int val;
	int ret;

	/*
	 * The EINT is level triggered and only fires on one polarity, so flip
	 * it to catch the opposite transition next.  The board uses the
	 * default active-low polarity: set the polarity bit when plugging in,
	 * clear it when plugging out.
	 */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_POL,
			   priv->plugged ? 0 : ACCDET_EINT_IRQ_POL);

	/* clear the EINT interrupt, preserving the polarity bit */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_CLR, ACCDET_EINT_IRQ_CLR);
	ret = regmap_read_poll_timeout(priv->regmap, MT6351_ACCDET_CON12, val,
				       !(val & ACCDET_EINT_IRQ_STATUS),
				       0, 1000);
	if (ret)
		dev_warn(priv->dev, "EINT clear timed out\n");
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_CLR, 0);

	if (!priv->plugged) {
		priv->plugged = true;
		/* short EINT debounce so a plug-out is seen quickly */
		regmap_update_bits(priv->regmap, MT6351_ACCDET_CON15,
				   ACCDET_EINT_DEB_MASK, ACCDET_EINT_DEB_OUT);
		mt6351_accdet_enable(priv);
	} else {
		priv->plugged = false;
		regmap_update_bits(priv->regmap, MT6351_ACCDET_CON15,
				   ACCDET_EINT_DEB_MASK, ACCDET_EINT_DEB_IN);
		mt6351_accdet_disable(priv);
		priv->jack_type = 0;
		priv->btn_type = 0;
		mt6351_accdet_jack_report(priv);
	}
}

static irqreturn_t mt6351_accdet_irq(int irq, void *data)
{
	struct mt6351_accdet *priv = data;
	unsigned int sts, val;
	int ret;

	mutex_lock(&priv->res_lock);
	regmap_read(priv->regmap, MT6351_ACCDET_CON12, &sts);

	if ((sts & ACCDET_IRQ_STATUS) && !(sts & ACCDET_EINT_IRQ_STATUS)) {
		/* clear the accessory interrupt, preserving the polarity bit */
		regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
				   ACCDET_IRQ_CLR, ACCDET_IRQ_CLR);
		ret = regmap_read_poll_timeout(priv->regmap,
					       MT6351_ACCDET_CON12, val,
					       !(val & ACCDET_IRQ_STATUS),
					       0, 1000);
		if (ret)
			dev_warn(priv->dev, "IRQ clear timed out\n");
		regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
				   ACCDET_IRQ_CLR, 0);

		mt6351_accdet_check_jack(priv);
		if (priv->plugged)
			mt6351_accdet_jack_report(priv);
	} else if (sts & ACCDET_EINT_IRQ_STATUS) {
		mt6351_accdet_handle_eint(priv);
	}

	mutex_unlock(&priv->res_lock);

	return IRQ_HANDLED;
}

static void mt6351_accdet_init(struct mt6351_accdet *priv)
{
	struct mt6351_accdet_data *d = &priv->data;

	/* enable the ACCDET clock and reset the block */
	regmap_write(priv->regmap, MT6351_TOP_CKPDN_CON2_CLR, ACCDET_CK_PDN);
	regmap_write(priv->regmap, MT6351_TOP_RST_CON0_SET, ACCDET_RST);
	regmap_write(priv->regmap, MT6351_TOP_RST_CON0_CLR, ACCDET_RST);

	/* PWM width/threshold are programmed as (value - 1) */
	regmap_write(priv->regmap, MT6351_ACCDET_CON3, d->deb.pwm_width - 1);
	regmap_write(priv->regmap, MT6351_ACCDET_CON4, d->deb.pwm_thresh - 1);
	regmap_write(priv->regmap, MT6351_ACCDET_CON2, ACCDET_SWCTRL_EN);
	regmap_write(priv->regmap, MT6351_ACCDET_CON5,
		     d->deb.fall_delay << 15 | d->deb.rise_delay);

	regmap_write(priv->regmap, MT6351_ACCDET_CON6, d->deb.debounce0);
	regmap_write(priv->regmap, MT6351_ACCDET_CON7, d->deb.debounce1);
	regmap_write(priv->regmap, MT6351_ACCDET_CON9, d->deb.debounce3);
	regmap_write(priv->regmap, MT6351_ACCDET_CON10, ACCDET_DEBOUNCE4_VAL);

	/* clear then enable the accessory and EINT interrupts */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_CLR, 0);
	regmap_write(priv->regmap, MT6351_INT_CON0_SET,
		     ACCDET_INT_EN | ACCDET_EINT_INT_EN);

	/* analog setup: pull-low, micbias reference, internal EINT connection */
	regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON11,
			   ACCDET_ANA_PULLLOW, ACCDET_ANA_PULLLOW);
	regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON10,
			   ACCDET_MICBIAS_VREF_MASK | ACCDET_MICBIAS_LOWPEN,
			   d->mic_vol << ACCDET_MICBIAS_VREF_SHIFT |
			   ACCDET_MICBIAS_LOWPEN);
	regmap_write(priv->regmap, MT6351_ACCDET_CON0, ACCDET_CON0_RSV_VAL);
	regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON11,
			   ACCDET_EINT_CON_EN, ACCDET_EINT_CON_EN);

	if (d->mic_mode == MT6351_ACCDET_MODE_LOW_COST) {
		regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON11,
				   ACCDET_ANA_MODE26, ACCDET_ANA_MODE26);
	} else if (d->mic_mode == MT6351_ACCDET_MODE_LOW_COST_BIAS) {
		regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON11,
				   ACCDET_ANA_MODE26, ACCDET_ANA_MODE26);
		regmap_update_bits(priv->regmap, MT6351_AUDENC_ANA_CON10,
				   ACCDET_MICBIAS_MODE6, ACCDET_MICBIAS_MODE6);
	}

	/* start in the unplugged state: only the EINT path runs */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON15,
			   ACCDET_EINT_DEB_MASK, ACCDET_EINT_DEB_IN);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON15,
			   ACCDET_EINT_PWM_THRESH, ACCDET_EINT_PWM_THRESH);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON2,
			   ACCDET_SWCTRL_EN, 0);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON2,
			   ACCDET_EINT_PWM_EN, ACCDET_EINT_PWM_EN);
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON1,
			   ACCDET_EINT_EN, ACCDET_EINT_EN);

	/*
	 * Route the accessory sense of the ACCDET comparator to AUXADC
	 * channel 5 so the voltage read for button decoding reflects it.
	 */
	regmap_update_bits(priv->regmap, MT6351_AUXADC_ACCDET,
			   ACCDET_AUXADC_AUTO_SET, ACCDET_AUXADC_AUTO_SET);
}

static int mt6351_accdet_parse_dt(struct mt6351_accdet *priv)
{
	struct mt6351_accdet_data *d = &priv->data;
	struct device_node *node;
	u32 deb[7];
	u32 key_mode = 0;

	node = of_get_child_by_name(priv->dev->parent->of_node, "accdet");
	if (!node)
		return -EINVAL;

	if (of_property_read_u32(node, "mediatek,mic-vol", &d->mic_vol))
		d->mic_vol = 7;
	if (of_property_read_u32(node, "mediatek,mic-mode", &d->mic_mode))
		d->mic_mode = MT6351_ACCDET_MODE_ACC;
	if (of_property_read_u32(node, "mediatek,plugout-debounce",
				 &d->plugout_deb))
		d->plugout_deb = 1;

	/*
	 * Defaults taken from the vendor DTS (aeon6797_6m_n.dts
	 * "headset-mode-setting"); a missing property must not leave the PWM
	 * values at zero, which would underflow the (value - 1) programming.
	 */
	d->deb.pwm_width = 0x500;
	d->deb.pwm_thresh = 0x500;
	d->deb.fall_delay = 1;
	d->deb.rise_delay = 0x1f0;
	d->deb.debounce0 = 0x800;
	d->deb.debounce1 = 0x800;
	d->deb.debounce3 = 0x20;
	if (!of_property_read_u32_array(node, "mediatek,pwm-deb-setting",
					deb, ARRAY_SIZE(deb))) {
		d->deb.pwm_width = deb[0];
		d->deb.pwm_thresh = deb[1];
		d->deb.fall_delay = deb[2];
		d->deb.rise_delay = deb[3];
		d->deb.debounce0 = deb[4];
		d->deb.debounce1 = deb[5];
		d->deb.debounce3 = deb[6];
	}

	of_property_read_u32(node, "mediatek,key-mode", &key_mode);
	if (key_mode == 1) {
		u32 four[5] = { 0, 58, 121, 192, 400 };

		d->four_key_mode = true;
		of_property_read_u32_array(node, "mediatek,four-key-thr",
					   four, ARRAY_SIZE(four));
		d->four_key.mid = four[1];
		d->four_key.voice = four[2];
		d->four_key.up = four[3];
		d->four_key.down = four[4];
	} else {
		u32 three[4] = { 0, 80, 220, 400 };

		of_property_read_u32_array(node, "mediatek,three-key-thr",
					   three, ARRAY_SIZE(three));
		d->three_key.mid = three[1];
		d->three_key.up = three[2];
		d->three_key.down = three[3];
	}

	of_node_put(node);

	return 0;
}

int mt6351_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack)
{
	struct mt6351_accdet *priv = snd_soc_component_get_drvdata(component);

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	mutex_lock(&priv->res_lock);
	priv->jack = jack;
	mutex_unlock(&priv->res_lock);

	mt6351_accdet_jack_report(priv);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6351_accdet_enable_jack_detect);

static int mt6351_accdet_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct mt6351_accdet *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = mt6397->regmap;

	priv->adc = devm_iio_channel_get(&pdev->dev, "accdet");
	if (IS_ERR(priv->adc))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->adc),
				     "failed to get accdet ADC channel\n");

	ret = mt6351_accdet_parse_dt(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to parse DT\n");

	mutex_init(&priv->res_lock);

	priv->accdet_irq = platform_get_irq(pdev, 0);
	if (priv->accdet_irq < 0)
		return priv->accdet_irq;
	ret = devm_request_threaded_irq(&pdev->dev, priv->accdet_irq, NULL,
					mt6351_accdet_irq, IRQF_ONESHOT,
					"ACCDET_IRQ", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET IRQ\n");

	priv->eint_irq = platform_get_irq(pdev, 1);
	if (priv->eint_irq < 0)
		return priv->eint_irq;
	ret = devm_request_threaded_irq(&pdev->dev, priv->eint_irq, NULL,
					mt6351_accdet_irq, IRQF_ONESHOT,
					"ACCDET_EINT", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET EINT\n");

	platform_set_drvdata(pdev, priv);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6351_accdet_soc_driver,
					      NULL, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register component\n");

	mt6351_accdet_init(priv);
	mt6351_accdet_jack_report(priv);

	return 0;
}

static struct platform_driver mt6351_accdet_driver = {
	.driver = {
		.name = "mt6351-accdet",
	},
	.probe = mt6351_accdet_probe,
};
module_platform_driver(mt6351_accdet_driver);

MODULE_DESCRIPTION("MT6351 ALSA SoC accessory detection driver");
MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
