/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT6351_MICBIAS_H
#define __MT6351_MICBIAS_H

struct regmap;
struct device;
int mt6351_micbias_accdet(struct device *consumer, struct regmap *regmap,
			  bool enable, unsigned int vref,
			  unsigned int low_power);

#endif
