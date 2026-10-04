/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mt6351-accdet.h  --  ALSA SoC MT6351 accessory detection driver
 *
 * Copyright (C) 2026 Miroslav Mraz <miroslav.mraz@techmania.cz>
 *
 * Based on MediaTek's GPL ACCDET driver for the MT6351 PMIC
 * (drivers/misc/mediatek/accdet/mt6797/accdet.c), Copyright (C) 2015
 * MediaTek Inc.
 */

#ifndef __MT6351_ACCDET_H__
#define __MT6351_ACCDET_H__

#include <linux/mutex.h>
#include <sound/jack.h>

struct snd_soc_component;
struct snd_soc_jack;
struct regmap;
struct iio_channel;

/*
 * MT6351 PMIC register addresses in the PMIC wrapper (pwrap) address space.
 * Values taken from the vendor GPL 3.18 kernel
 * drivers/misc/mediatek/include/mt-plat/mt6797/include/mach/upmu_hw.h and
 * drivers/misc/mediatek/accdet/mt6797/reg_accdet.h.
 */
#define MT6351_TOP_CKPDN_CON2_SET	0x0248
#define MT6351_TOP_CKPDN_CON2_CLR	0x024a
#define MT6351_TOP_RST_CON0_SET		0x02aa
#define MT6351_TOP_RST_CON0_CLR		0x02ac
#define MT6351_INT_CON0_SET		0x02c4
#define MT6351_INT_CON0_CLR		0x02c6
#define MT6351_AUDENC_ANA_CON10		0x0d1c
#define MT6351_AUDENC_ANA_CON11		0x0d1e
#define MT6351_AUXADC_ACCDET		0x0ed8
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
/* MT6351_INT_CON0 / INT_STATUS0: PMIC interrupt enables */
#define ACCDET_INT_EN			BIT(12)
#define ACCDET_EINT_INT_EN		BIT(13)

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

/* MT6351_AUXADC_ACCDET: route the accessory sense to AUXADC channel 5 */
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

/* AUXADC reference voltage (mV) and resolution for the accessory channel */
#define MT6351_ACCDET_AUXADC_MV		1800
#define MT6351_ACCDET_AUXADC_RES	4096

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
	u32 plugout_deb;
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
	int accdet_irq;
	int eint_irq;
	struct mutex res_lock; /* serialises the shared ACCDET IRQ register */
	bool plugged;
	unsigned int jack_type;
	unsigned int btn_type;
	unsigned int accdet_status;
};

#if IS_ENABLED(CONFIG_SND_SOC_MT6351_ACCDET)
int mt6351_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack);
#else
static inline int
mt6351_accdet_enable_jack_detect(struct snd_soc_component *component,
				 struct snd_soc_jack *jack)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __MT6351_ACCDET_H__ */
