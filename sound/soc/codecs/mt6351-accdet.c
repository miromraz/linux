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

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <sound/jack.h>
#include <sound/soc.h>

/*
 * MT6351 PMIC register addresses in the PMIC wrapper (pwrap) address space.
 * Values taken from the vendor GPL 3.18 kernel
 * drivers/misc/mediatek/include/mt-plat/mt6797/include/mach/upmu_hw.h and
 * drivers/misc/mediatek/accdet/mt6797/reg_accdet.h.
 */
#define MT6351_TOP_CKPDN_CON2_CLR	0x024a
#define MT6351_TOP_RST_CON0_SET		0x02aa
#define MT6351_TOP_RST_CON0_CLR		0x02ac
#define MT6351_AUDENC_ANA_CON10		0x0d1c
#define MT6351_AUDENC_ANA_CON11		0x0d1e
/* ACCDET AUXADC auto-sample control (vendor MT6351_AUXADC_ACCDET register) */
#define MT6351_AUXADC_ACCDET_AUTO	0x0ed8
#define MT6351_ACCDET_CON0		0x0f46
#define MT6351_ACCDET_CON1		0x0f48
#define MT6351_ACCDET_CON2		0x0f4a
#define MT6351_ACCDET_CON3		0x0f4c
#define MT6351_ACCDET_CON4		0x0f4e
#define MT6351_ACCDET_CON5		0x0f50
#define MT6351_ACCDET_CON6		0x0f52
#define MT6351_ACCDET_CON7		0x0f54
#define MT6351_ACCDET_CON9		0x0f58
#define MT6351_ACCDET_CON10		0x0f5a
#define MT6351_ACCDET_CON12		0x0f5e
#define MT6351_ACCDET_CON14		0x0f62
#define MT6351_ACCDET_CON15		0x0f64

/* MT6351_TOP_CKPDN_CON2: accessory detection clock gate */
#define ACCDET_CK_PDN			BIT(9)
/* MT6351_TOP_RST_CON0: accessory detection soft reset */
#define ACCDET_RST			BIT(4)

/* MT6351_ACCDET_CON0: reserved analog control, vendor init value */
#define ACCDET_CON0_RSV_VAL		0x0010
/* MT6351_ACCDET_CON1: accessory detection control */
#define ACCDET_EN			BIT(0)
#define ACCDET_EINT_EN			BIT(2)
/* MT6351_ACCDET_CON2: comparator/vth/micbias PWM software control */
#define ACCDET_SWCTRL_EN		0x07
#define ACCDET_EINT_PWM_EN		BIT(3)
#define ACCDET_SWCTRL_IDLE_EN		(0x07 << 4)
/* MT6351_ACCDET_CON10: auxadc connection debounce, vendor value (2 ms) */
#define ACCDET_DEBOUNCE4_VAL		0x42
/* MT6351_ACCDET_CON12: interrupt status and clear */
#define ACCDET_IRQ_STATUS		BIT(0)
#define ACCDET_EINT_IRQ_STATUS		BIT(2)
#define ACCDET_IRQ_CLR			BIT(8)
#define ACCDET_EINT_IRQ_CLR		BIT(10)
#define ACCDET_EINT_IRQ_POL		BIT(15)
/* MT6351_ACCDET_CON14: comparator A/B state */
#define ACCDET_STATE_AB_SHIFT		6
#define ACCDET_STATE_AB_MASK		0x03
/* MT6351_ACCDET_CON15: EINT (plug) PWM and debounce control */
#define ACCDET_EINT_DEB_MASK		(0x07 << 4)
#define ACCDET_EINT_DEB_IN		0x60	/* 256 ms, wait for plug-in */
#define ACCDET_EINT_DEB_OUT		0x00	/* 1 ms, wait for plug-out */
#define ACCDET_EINT_PWM_THRESH		0x400

/* MT6351_AUXADC_ACCDET_AUTO: route the accessory sense to AUXADC channel 5 */
#define ACCDET_AUXADC_AUTO_SET		BIT(0)

/* MT6351_AUDENC_ANA_CON10: micbias1 reference and low-power control */
#define ACCDET_MICBIAS_VREF_MASK	(0x07 << 4)
#define ACCDET_MICBIAS_VREF_SHIFT	4
#define ACCDET_MICBIAS_LOWPEN		BIT(7)
#define ACCDET_MICBIAS_MODE6		0x0104
/* MT6351_AUDENC_ANA_CON11: accessory analog detection control */
#define ACCDET_ANA_PULLLOW		0x0f
#define ACCDET_EINT_CON_EN		BIT(11)
#define ACCDET_ANA_MODE26		0x08c0

/* mic-mode values (mediatek,mic-mode) */
#define MT6351_ACCDET_MODE_ACC		1
#define MT6351_ACCDET_MODE_LOW_COST	2
#define MT6351_ACCDET_MODE_LOW_COST_BIAS 6

#define MT6351_ACCDET_JACK_MASK		(SND_JACK_HEADPHONE | \
					 SND_JACK_HEADSET | \
					 SND_JACK_BTN_0 | \
					 SND_JACK_BTN_1 | \
					 SND_JACK_BTN_2 | \
					 SND_JACK_BTN_3)

struct mt6351_accdet_deb {
	u32 pwm_width;
	u32 pwm_thresh;
	u32 fall_delay;
	u32 rise_delay;
	u32 debounce0;
	u32 debounce1;
	u32 debounce3;
};

struct mt6351_accdet_three_key {
	u32 mid;
	u32 up;
	u32 down;
};

struct mt6351_accdet_four_key {
	u32 mid;
	u32 voice;
	u32 up;
	u32 down;
};

struct mt6351_accdet_data {
	u32 mic_vol;
	u32 mic_mode;
	u32 eint_pol;
	bool four_key_mode;
	struct mt6351_accdet_deb deb;
	struct mt6351_accdet_three_key three_key;
	struct mt6351_accdet_four_key four_key;
};

struct mt6351_accdet {
	struct snd_soc_jack *jack;
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *adc;
	struct mt6351_accdet_data data;
	struct mutex res_lock; /* serialises the shared ACCDET IRQ register */
	int cali_offset; /* efuse AUXADC offset in mV, subtracted before decode */
	bool plugged;
	unsigned int jack_type;
	unsigned int btn_type;
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

	ret = iio_read_channel_processed(priv->adc, &val);
	if (ret < 0)
		return ret;

	/*
	 * Subtract the factory AUXADC offset the vendor applies before button
	 * decoding (drivers/misc/mediatek/accdet/mt6797/accdet.c
	 * Accdet_PMIC_IMM_GetOneChannelValue(): vol_val -= accdet_auxadc_offset).
	 */
	val -= priv->cali_offset;

	return max(val, 0);
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
	unsigned int val, ab;
	int v;

	regmap_read(priv->regmap, MT6351_ACCDET_CON14, &val);
	ab = (val >> ACCDET_STATE_AB_SHIFT) & ACCDET_STATE_AB_MASK;

	switch (ab) {
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
			/* AB=11 debounce lengthened while a headset is in */
			regmap_write(priv->regmap, MT6351_ACCDET_CON9,
				     priv->data.deb.debounce3 * 30);
		}
		break;
	case 2:
		/*
		 * Transient comparator state: the vendor driver ignores it in
		 * EINT mode and lets the EINT own plug-out, so do nothing.
		 */
		break;
	default: /* AB = 3 */
		/*
		 * The vendor driver explicitly does not emit a plug-out event
		 * here ("do not send plug out event in plug out"); only the
		 * EINT reports unplug.  Just drop any pending button.
		 */
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
	 * it to catch the opposite transition next.  Which level means
	 * "plugged" comes from mediatek,eint-level-pol (0 = active low,
	 * the board default).
	 */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_POL,
			   (priv->data.eint_pol ? priv->plugged : !priv->plugged) ?
			   ACCDET_EINT_IRQ_POL : 0);

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
		/*
		 * Restore the plug-in (AB=11) debounce the vendor re-applies on
		 * every plug-out; otherwise it stays at the x30 value written
		 * when a headset was detected and later insertions are slow.
		 */
		regmap_write(priv->regmap, MT6351_ACCDET_CON9,
			     priv->data.deb.debounce3);
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

	/*
	 * Make sure both interrupt-clear bits are released.  The accessory and
	 * EINT PMIC interrupts themselves are unmasked by the mt6397 irqchip
	 * when the threaded handlers are requested, so do not poke INT_CON0
	 * here.
	 */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_IRQ_CLR | ACCDET_EINT_IRQ_CLR, 0);

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

	/* prime the EINT polarity so it fires on the first plug-in */
	regmap_update_bits(priv->regmap, MT6351_ACCDET_CON12,
			   ACCDET_EINT_IRQ_POL,
			   d->eint_pol ? ACCDET_EINT_IRQ_POL : 0);

	/*
	 * Route the accessory sense of the ACCDET comparator to AUXADC
	 * channel 5 so the voltage read for button decoding reflects it.
	 */
	regmap_update_bits(priv->regmap, MT6351_AUXADC_ACCDET_AUTO,
			   ACCDET_AUXADC_AUTO_SET, ACCDET_AUXADC_AUTO_SET);
}

/*
 * Read the factory AUXADC offset from the PMIC efuse. The vendor driver takes
 * OTP word 3, extracts bits [12:5] and halves them to get the offset in mV
 * (reg_accdet.h RG_OTP_PA_ADDR_WORD_INDEX/RG_OTP_PA_ACCDET_BIT_SHIFT,
 * accdet.c accdet_pmic_Read_Efuse_HPOffset()). The nvmem cell exposes that
 * word little-endian. The cell is optional: without it the offset stays 0.
 */
static int mt6351_accdet_read_cali(struct mt6351_accdet *priv)
{
	struct device *dev = priv->dev;
	struct nvmem_cell *cell;
	size_t len;
	u8 *buf;

	cell = devm_nvmem_cell_get(dev, "accdet-offset");
	if (IS_ERR(cell)) {
		/* The cell is optional; only its absence leaves the offset 0. */
		if (PTR_ERR(cell) == -ENOENT) {
			dev_dbg(dev, "no accdet-offset efuse cell, using offset 0\n");
			return 0;
		}
		if (PTR_ERR(cell) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		/* Any other error is non-fatal: fall back to offset 0. */
		dev_warn(dev, "failed to get accdet-offset efuse cell (%pe), using offset 0\n",
			 cell);
		return 0;
	}

	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf)) {
		if (PTR_ERR(buf) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_warn(dev, "failed to read accdet-offset efuse (%pe), using offset 0\n",
			 buf);
		return 0;
	}

	if (len >= sizeof(u16)) {
		u16 word = buf[0] | (buf[1] << 8);

		priv->cali_offset = ((word >> 5) & 0xff) / 2;
		dev_dbg(dev, "accdet AUXADC offset %d mV\n", priv->cali_offset);
	}
	kfree(buf);
	return 0;
}

static void mt6351_accdet_parse_dt(struct mt6351_accdet *priv)
{
	struct mt6351_accdet_data *d = &priv->data;
	struct device *dev = priv->dev;
	u32 deb[7];
	u32 key_mode = 0;

	if (device_property_read_u32(dev, "mediatek,mic-vol", &d->mic_vol))
		d->mic_vol = 7;
	if (device_property_read_u32(dev, "mediatek,mic-mode", &d->mic_mode))
		d->mic_mode = MT6351_ACCDET_MODE_ACC;
	/* 0 (active low) is the board default EINT "plugged" level */
	device_property_read_u32(dev, "mediatek,eint-level-pol", &d->eint_pol);

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
	if (!device_property_read_u32_array(dev, "mediatek,pwm-deb-setting",
					   deb, ARRAY_SIZE(deb))) {
		d->deb.pwm_width = deb[0];
		d->deb.pwm_thresh = deb[1];
		d->deb.fall_delay = deb[2];
		d->deb.rise_delay = deb[3];
		d->deb.debounce0 = deb[4];
		d->deb.debounce1 = deb[5];
		d->deb.debounce3 = deb[6];
	}

	device_property_read_u32(dev, "mediatek,key-mode", &key_mode);
	if (key_mode == 1) {
		u32 four[5] = { 0, 58, 121, 192, 400 };

		d->four_key_mode = true;
		device_property_read_u32_array(dev, "mediatek,four-key-thr",
					       four, ARRAY_SIZE(four));
		d->four_key.mid = four[1];
		d->four_key.voice = four[2];
		d->four_key.up = four[3];
		d->four_key.down = four[4];
	} else {
		u32 three[4] = { 0, 80, 220, 400 };

		device_property_read_u32_array(dev, "mediatek,three-key-thr",
					       three, ARRAY_SIZE(three));
		d->three_key.mid = three[1];
		d->three_key.up = three[2];
		d->three_key.down = three[3];
	}
}

/*
 * The machine driver lists this component as a card aux_dev and hands the
 * headset jack in through the generic set_jack path, so the card core owns the
 * component lifetime and the jack is cleared on teardown (jack == NULL).
 */
static int mt6351_accdet_set_jack(struct snd_soc_component *component,
				  struct snd_soc_jack *jack, void *data)
{
	struct mt6351_accdet *priv = snd_soc_component_get_drvdata(component);

	if (jack) {
		snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);
	}

	mutex_lock(&priv->res_lock);
	priv->jack = jack;
	mutex_unlock(&priv->res_lock);

	if (jack)
		mt6351_accdet_jack_report(priv);

	return 0;
}

static void mt6351_accdet_component_remove(struct snd_soc_component *component)
{
	struct mt6351_accdet *priv = snd_soc_component_get_drvdata(component);

	/* drop the jack before the card frees it, so the IRQ never uses it */
	mutex_lock(&priv->res_lock);
	priv->jack = NULL;
	mutex_unlock(&priv->res_lock);
}

static const struct snd_soc_component_driver mt6351_accdet_soc_driver = {
	.name = "mt6351-accdet",
	.set_jack = mt6351_accdet_set_jack,
	.remove = mt6351_accdet_component_remove,
};

static int mt6351_accdet_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct mt6351_accdet *priv;
	int accdet_irq, eint_irq, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = mt6397->regmap;

	priv->adc = devm_iio_channel_get(&pdev->dev, "accdet");
	if (IS_ERR(priv->adc))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->adc),
				     "failed to get accdet ADC channel\n");

	mt6351_accdet_parse_dt(priv);
	ret = mt6351_accdet_read_cali(priv);
	if (ret)
		return ret;

	ret = devm_mutex_init(&pdev->dev, &priv->res_lock);
	if (ret)
		return ret;

	accdet_irq = platform_get_irq_byname(pdev, "accdet_irq");
	if (accdet_irq < 0)
		return accdet_irq;

	eint_irq = platform_get_irq_byname(pdev, "accdet_eint");
	if (eint_irq < 0)
		return eint_irq;

	platform_set_drvdata(pdev, priv);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6351_accdet_soc_driver,
					      NULL, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register component\n");

	/*
	 * Reset the block to a known unplugged state and clear any pending
	 * interrupt before unmasking, so a plug left in across a rebind does
	 * not desync the software plug state.
	 */
	mt6351_accdet_init(priv);

	ret = devm_request_threaded_irq(&pdev->dev, accdet_irq, NULL,
					mt6351_accdet_irq, IRQF_ONESHOT,
					"accdet_irq", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET IRQ\n");

	ret = devm_request_threaded_irq(&pdev->dev, eint_irq, NULL,
					mt6351_accdet_irq, IRQF_ONESHOT,
					"accdet_eint", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET EINT\n");

	return 0;
}

static const struct of_device_id mt6351_accdet_of_match[] = {
	{ .compatible = "mediatek,mt6351-accdet" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_accdet_of_match);

static const struct platform_device_id mt6351_accdet_id[] = {
	{ "mt6351-accdet" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6351_accdet_id);

static struct platform_driver mt6351_accdet_driver = {
	.driver = {
		.name = "mt6351-accdet",
		.of_match_table = mt6351_accdet_of_match,
	},
	.probe = mt6351_accdet_probe,
	.id_table = mt6351_accdet_id,
};
module_platform_driver(mt6351_accdet_driver);

MODULE_DESCRIPTION("MT6351 ALSA SoC accessory detection driver");
MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
