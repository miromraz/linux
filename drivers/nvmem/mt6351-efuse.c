// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6351 PMIC eFuse (OTP) read-only NVMEM provider
 *
 * The MT6351 PMIC stores factory calibration (e.g. the ACCDET AUXADC offset)
 * in an on-die OTP array that is read through an OTP engine reachable over the
 * PMIC wrapper, via the parent mt6397 MFD regmap. Each 16-bit word occupies
 * two OTP physical-address rows, so word n is read from row 2*n.
 *
 * The register sequence and timing mirror MediaTek's GPL 3.18 kernel
 * (drivers/misc/mediatek/power/mt6797/pmic.c, pmic_Read_Efuse_HPOffset()).
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* PMIC-wrapper register addresses (16-bit PMIC register space). */
#define MT6351_TOP_CKPDN_CON3		0x024c
#define MT6351_RG_EFUSE_CK_PDN		BIT(2)
#define MT6351_TOP_CKHWEN_CON0		0x0282
#define MT6351_RG_EFUSE_CK_PDN_HWEN	BIT(6)
#define MT6351_OTP_CON0			0x0c00
#define MT6351_RG_OTP_PA		GENMASK(5, 0)
#define MT6351_OTP_CON8			0x0c10
#define MT6351_RG_OTP_RD_TRIG		BIT(0)
#define MT6351_OTP_CON11		0x0c16
#define MT6351_RG_OTP_RD_SW		BIT(0)
#define MT6351_OTP_CON12		0x0c18	/* 16-bit data out */
#define MT6351_OTP_CON13		0x0c1a
#define MT6351_RG_OTP_RD_BUSY		BIT(0)

/* OTP addressing is 6 bits of rows, two rows per word -> 32 words. */
#define MT6351_EFUSE_NR_WORDS		32
#define MT6351_EFUSE_SIZE		(MT6351_EFUSE_NR_WORDS * sizeof(u16))

struct mt6351_efuse {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;	/* serialises the multi-register OTP read */
};

static int mt6351_efuse_read_word(struct mt6351_efuse *e, unsigned int word,
				  u16 *out)
{
	struct regmap *regmap = e->regmap;
	unsigned int val;
	int ret;

	/* Enable the eFuse controller clock. */
	ret = regmap_update_bits(regmap, MT6351_TOP_CKHWEN_CON0,
				 MT6351_RG_EFUSE_CK_PDN_HWEN, 0);
	if (ret)
		goto out;
	ret = regmap_update_bits(regmap, MT6351_TOP_CKPDN_CON3,
				 MT6351_RG_EFUSE_CK_PDN, 0);
	if (ret)
		goto out;

	/* Software read mode, select the OTP row (two rows per word). */
	ret = regmap_update_bits(regmap, MT6351_OTP_CON11,
				 MT6351_RG_OTP_RD_SW, MT6351_RG_OTP_RD_SW);
	if (ret)
		goto out;
	ret = regmap_update_bits(regmap, MT6351_OTP_CON0, MT6351_RG_OTP_PA,
				 FIELD_PREP(MT6351_RG_OTP_PA, word * 2));
	if (ret)
		goto out;

	/* Toggle the read trigger: it is edge triggered. */
	fsleep(100);
	ret = regmap_read(regmap, MT6351_OTP_CON8, &val);
	if (ret)
		goto out;
	ret = regmap_update_bits(regmap, MT6351_OTP_CON8, MT6351_RG_OTP_RD_TRIG,
				 (val & MT6351_RG_OTP_RD_TRIG) ? 0 : MT6351_RG_OTP_RD_TRIG);
	if (ret)
		goto out;

	/* Wait for the engine to finish. */
	fsleep(300);
	ret = regmap_read_poll_timeout(regmap, MT6351_OTP_CON13, val,
				       !(val & MT6351_RG_OTP_RD_BUSY),
				       1000, 100000);
	if (ret) {
		dev_err(e->dev, "timed out reading OTP word %u\n", word);
		goto out;
	}

	/* The data register needs ~1 ms to settle after busy clears. */
	fsleep(1000);
	ret = regmap_read(regmap, MT6351_OTP_CON12, &val);
	if (ret)
		goto out;
	*out = val;
out:
	/* Always restore clock gating, best effort, without masking ret. */
	regmap_update_bits(regmap, MT6351_TOP_CKPDN_CON3,
			   MT6351_RG_EFUSE_CK_PDN, MT6351_RG_EFUSE_CK_PDN);
	regmap_update_bits(regmap, MT6351_TOP_CKHWEN_CON0,
			   MT6351_RG_EFUSE_CK_PDN_HWEN,
			   MT6351_RG_EFUSE_CK_PDN_HWEN);
	return ret;
}

static int mt6351_efuse_read(void *priv, unsigned int offset, void *val,
			     size_t bytes)
{
	struct mt6351_efuse *e = priv;
	u8 *buf = val;
	int ret = 0;

	mutex_lock(&e->lock);
	while (bytes) {
		unsigned int boff = offset % sizeof(u16);
		u16 word;

		ret = mt6351_efuse_read_word(e, offset / sizeof(u16), &word);
		if (ret)
			break;

		/* OTP words are exposed little-endian. */
		while (boff < sizeof(u16) && bytes) {
			*buf++ = word >> (8 * boff);
			boff++;
			offset++;
			bytes--;
		}
	}
	mutex_unlock(&e->lock);

	return ret;
}

static int mt6351_efuse_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397 = dev_get_drvdata(pdev->dev.parent);
	struct nvmem_config config = {
		.dev = &pdev->dev,
		.name = "mt6351-efuse",
		.id = NVMEM_DEVID_NONE,
		.type = NVMEM_TYPE_OTP,
		.read_only = true,
		.size = MT6351_EFUSE_SIZE,
		.word_size = sizeof(u8),
		.stride = sizeof(u8),
		.reg_read = mt6351_efuse_read,
	};
	struct mt6351_efuse *e;
	int ret;

	e = devm_kzalloc(&pdev->dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	e->dev = &pdev->dev;
	e->regmap = mt6397->regmap;

	ret = devm_mutex_init(&pdev->dev, &e->lock);
	if (ret)
		return ret;

	config.priv = e;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(&pdev->dev, &config));
}

static const struct of_device_id mt6351_efuse_of_match[] = {
	{ .compatible = "mediatek,mt6351-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_efuse_of_match);

static const struct platform_device_id mt6351_efuse_id[] = {
	{ "mt6351-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(platform, mt6351_efuse_id);

static struct platform_driver mt6351_efuse_driver = {
	.driver = {
		.name = "mt6351-efuse",
		.of_match_table = mt6351_efuse_of_match,
	},
	.probe = mt6351_efuse_probe,
	.id_table = mt6351_efuse_id,
};
module_platform_driver(mt6351_efuse_driver);

MODULE_DESCRIPTION("MediaTek MT6351 PMIC eFuse NVMEM driver");
MODULE_AUTHOR("Miroslav Mraz <miroslav.mraz@techmania.cz>");
MODULE_LICENSE("GPL");
