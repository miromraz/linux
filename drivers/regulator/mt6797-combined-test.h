/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_COMBINED_TEST_H
#define MT6797_COMBINED_TEST_H
/* A stack marker is removed on success and synchronously released on error. */
static inline void mt6797_test_probe_destroyed(void *data)
{
	*(bool *)data = true;
}

struct device;
struct device_node;
struct platform_device;
struct regmap;
int mt6797_test_buck(struct platform_device *pdev, struct regmap *map,
		     struct device_node *np);
int mt6797_test_sram(struct platform_device *pdev);
int mt6797_test_cpu_probe(struct platform_device *pdev, int fail_cpu);
int mt6797_test_cci_probe(struct platform_device *pdev);
int mt6797_test_cci_remove(struct platform_device *pdev);
void mt6797_test_transfer_hook(void (*hook)(void *, unsigned int, bool, int), void *data);
#endif
