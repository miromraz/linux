// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Linaro Ltd.
 * Author: Pi-Cheng Chen <pi-cheng.chen@linaro.org>
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/mt6797.h>
#include <linux/units.h>

struct mtk_cpufreq_platform_data {
	int min_volt_shift;
	int max_volt_shift;
	int proc_max_volt;
	int sram_min_volt;
	int sram_max_volt;
	bool ccifreq_supported;
	/*
	 * The cluster ARMPLL is driven by firmware (an SMC/SiP call) rather than
	 * by register writes, so clk_set_rate() cannot report a rate the firmware
	 * silently refused. Only such a platform needs the post-set rate readback
	 * in .set_target() and the initial-OPP sync in .init below; register-PLL
	 * SoCs are left untouched.
	 */
	bool firmware_pll;
	bool allow_unmanaged;

};

/*
 * The struct mtk_cpu_dvfs_info holds necessary information for doing CPU DVFS
 * on each CPU power/clock domain of Mediatek SoCs. Each CPU cluster in
 * Mediatek SoCs has two voltage inputs, Vproc and Vsram. In some cases the two
 * voltage inputs need to be controlled under a hardware limitation:
 * 100mV < Vsram - Vproc < 200mV
 *
 * When scaling the clock frequency of a CPU clock domain, the clock source
 * needs to be switched to another stable PLL clock temporarily until
 * the original PLL becomes stable at target frequency.
 */
struct mtk_cpu_dvfs_info {
	u64 rail_cookie;
	unsigned int rail_user;
	struct cpumask cpus;
	struct device *cpu_dev;
	struct device *cci_dev;
	struct regulator *proc_reg;
	struct regulator *sram_reg;
	struct clk *cpu_clk;
	struct clk *inter_clk;
	struct list_head list_head;
	int intermediate_voltage;
	bool need_voltage_tracking;
	int vproc_on_boot;
	int pre_vproc;
	/* Avoid race condition for regulators between notify and policy */
	struct mutex reg_lock;
	struct notifier_block opp_nb;
	unsigned int opp_cpu;
	unsigned long current_freq;
	/* An uncertain transaction requires reinitialization, not a retry. */
	bool dvfs_fault;
	const struct mtk_cpufreq_platform_data *soc_data;
	int vtrack_max;
	bool ccifreq_bound;
	int opp_token;
};

static struct platform_device *cpufreq_pdev;

static LIST_HEAD(dvfs_info_list);

static struct mtk_cpu_dvfs_info *mtk_cpu_dvfs_info_lookup(int cpu)
{
	struct mtk_cpu_dvfs_info *info;

	list_for_each_entry(info, &dvfs_info_list, list_head) {
		if (cpumask_test_cpu(cpu, &info->cpus))
			return info;
	}

	return NULL;
}

static int mtk_cpufreq_voltage_tracking(struct mtk_cpu_dvfs_info *info,
					int new_vproc)
{
	const struct mtk_cpufreq_platform_data *soc_data = info->soc_data;
	struct regulator *proc_reg = info->proc_reg;
	struct regulator *sram_reg = info->sram_reg;
	int pre_vproc, pre_vsram, new_vsram, vsram, vproc, ret;
	int retry = info->vtrack_max;

	pre_vproc = regulator_get_voltage(proc_reg);
	if (pre_vproc < 0) {
		dev_err(info->cpu_dev,
			"invalid Vproc value: %d\n", pre_vproc);
		return pre_vproc;
	}

	pre_vsram = regulator_get_voltage(sram_reg);
	if (pre_vsram < 0) {
		dev_err(info->cpu_dev, "invalid Vsram value: %d\n", pre_vsram);
		return pre_vsram;
	}

	new_vsram = clamp(new_vproc + soc_data->min_volt_shift,
			  soc_data->sram_min_volt, soc_data->sram_max_volt);

	do {
		if (pre_vproc <= new_vproc) {
			vsram = clamp(pre_vproc + soc_data->max_volt_shift,
				      soc_data->sram_min_volt, new_vsram);
			ret = regulator_set_voltage(sram_reg, vsram,
						    soc_data->sram_max_volt);

			if (ret)
				return ret;

			if (vsram == soc_data->sram_max_volt ||
			    new_vsram == soc_data->sram_min_volt)
				vproc = new_vproc;
			else
				vproc = vsram - soc_data->min_volt_shift;

			ret = regulator_set_voltage(proc_reg, vproc,
						    soc_data->proc_max_volt);
			if (ret)
				return ret;
		} else {
			vproc = max(new_vproc,
				    pre_vsram - soc_data->max_volt_shift);
			ret = regulator_set_voltage(proc_reg, vproc,
						    soc_data->proc_max_volt);
			if (ret)
				return ret;

			if (vproc == new_vproc)
				vsram = new_vsram;
			else
				vsram = max(new_vsram,
					    vproc + soc_data->min_volt_shift);

			ret = regulator_set_voltage(sram_reg, vsram,
						    soc_data->sram_max_volt);
			if (ret)
				return ret;
		}

		pre_vproc = vproc;
		pre_vsram = vsram;

		if (--retry < 0) {
			dev_err(info->cpu_dev,
				"over loop count, failed to set voltage\n");
			return -EINVAL;
		}
	} while (vproc != new_vproc || vsram != new_vsram);

	return 0;
}

static int mtk_cpufreq_set_voltage(struct mtk_cpu_dvfs_info *info, int vproc)
{
	const struct mtk_cpufreq_platform_data *soc_data = info->soc_data;
	int ret;

	if (info->dvfs_fault)
		return -EIO;

	if (info->need_voltage_tracking)
		ret = mtk_cpufreq_voltage_tracking(info, vproc);
	else
		ret = regulator_set_voltage(info->proc_reg, vproc,
					    soc_data->proc_max_volt);
	if (!ret) {
		info->pre_vproc = vproc;
	} else {
		/* A regulator error can follow partially completed tracking. */
		info->pre_vproc = 0;
		info->current_freq = 0;
		info->dvfs_fault = true;
	}

	return ret;
}

static bool is_ccifreq_ready(struct mtk_cpu_dvfs_info *info)
{
	struct device_link *sup_link;

	if (info->ccifreq_bound)
		return true;

	sup_link = device_link_add(info->cpu_dev, info->cci_dev,
				   DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!sup_link) {
		dev_err(info->cpu_dev, "cpu%d: sup_link is NULL\n", info->opp_cpu);
		return false;
	}

	if (sup_link->supplier->links.status != DL_DEV_DRIVER_BOUND)
		return false;

	info->ccifreq_bound = true;

	return true;
}

static int mtk_cpufreq_set_target(struct cpufreq_policy *policy,
				  unsigned int index)
{
	struct cpufreq_frequency_table *freq_table = policy->freq_table;
	struct clk *cpu_clk = policy->clk;
	struct clk *armpll = clk_get_parent(cpu_clk);
	struct mtk_cpu_dvfs_info *info = policy->driver_data;
	struct device *cpu_dev = info->cpu_dev;
	struct dev_pm_opp *opp;
	long freq_hz;
	int vproc, pre_vproc, inter_vproc, target_vproc, ret;

	inter_vproc = info->intermediate_voltage;


	mutex_lock(&info->reg_lock);

	if (info->dvfs_fault) {
		ret = -EIO;
		goto out;
	}

	if (!armpll || clk_is_match(armpll, info->inter_clk)) {
		ret = -EIO;
		goto fault;
	}

	if (unlikely(info->pre_vproc <= 0))
		pre_vproc = regulator_get_voltage(info->proc_reg);
	else
		pre_vproc = info->pre_vproc;

	if (pre_vproc < 0) {
		dev_err(cpu_dev, "invalid Vproc value: %d\n", pre_vproc);
		ret = pre_vproc;
		goto out;
	}

	freq_hz = freq_table[index].frequency * 1000;

	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &freq_hz);
	if (IS_ERR(opp)) {
		dev_err(cpu_dev, "cpu%d: failed to find OPP for %ld\n",
			policy->cpu, freq_hz);
		ret = PTR_ERR(opp);
		goto out;
	}
	vproc = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	/*
	 * If MediaTek cci is supported but is not ready, we will use the value
	 * of max(target cpu voltage, booting voltage) to prevent high freqeuncy
	 * low voltage crash.
	 */
	if (info->soc_data->ccifreq_supported && !is_ccifreq_ready(info))
		vproc = max(vproc, info->vproc_on_boot);

	/*
	 * A peer's aggregate voltage is not this consumer's vote. Submit our
	 * preparation floor even when the rail already reads high. Retain the
	 * old requirement until the clock transition has completed.
	 */
	target_vproc = max(pre_vproc, max(inter_vproc, vproc));
	ret = mtk_cpufreq_set_voltage(info, target_vproc);
	if (ret) {
		dev_err(cpu_dev, "cpu%d: failed to prepare voltage!\n", policy->cpu);
		goto out;
	}

	/* Reparent the CPU clock to intermediate clock. */
	ret = clk_set_parent(cpu_clk, info->inter_clk);
	if (ret || !clk_is_match(clk_get_parent(cpu_clk), info->inter_clk)) {
		ret = ret ?: -EIO;
		dev_err(cpu_dev,
			"cpu%d: failed to re-parent cpu clock!\n", policy->cpu);
		goto fault;
	}

	/* Set the original PLL to target rate. */
	ret = clk_set_rate(armpll, freq_hz);
	if (ret) {
		dev_err(cpu_dev,
			"cpu%d: failed to scale cpu clock rate!\n", policy->cpu);
		goto fault;
	}

	/*
	 * Verify acceptance before reconnecting the PLL. Firmware may refuse
	 * an SMC, leaving its cached rate zero on the first transaction.
	 * Never invent a boot rate or reduce voltage on that uncertainty.
	 */
	if (info->soc_data->firmware_pll &&
	    abs((long)clk_get_rate(armpll) - freq_hz) >= HZ_PER_MHZ) {
		ret = -EIO;
		goto fault;
	}

	ret = clk_set_parent(cpu_clk, armpll);
	if (ret || !clk_is_match(clk_get_parent(cpu_clk), armpll)) {
		ret = ret ?: -EIO;
		goto fault;
	}

	/* Replace the preparation vote with our persistent target floor. */
	ret = mtk_cpufreq_set_voltage(info, vproc);
	if (ret) {
		dev_err(cpu_dev, "cpu%d: failed to set target voltage!\n", policy->cpu);
		/* Keep the lowered clock: voltage may already be reduced. */
		goto out;
	}

	info->current_freq = freq_hz;
	goto out;

fault:
	/*
	 * Retain max(old, intermediate, target) voltage. A failed clock
	 * operation may have taken effect; neither retune a possibly active
	 * PLL nor lower voltage without a verified recovery contract. Block
	 * later transitions and OPP notifiers from trusting stale state.
	 */
	info->pre_vproc = 0;
	info->current_freq = 0;
	info->dvfs_fault = true;
	dev_err(cpu_dev, "DVFS state uncertain; further transitions disabled\n");
out:
	mutex_unlock(&info->reg_lock);

	return ret;
}

static int mtk_cpufreq_opp_notifier(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct dev_pm_opp *opp = data;
	struct dev_pm_opp *new_opp;
	struct mtk_cpu_dvfs_info *info;
	unsigned long freq, volt;
	int ret = 0;

	info = container_of(nb, struct mtk_cpu_dvfs_info, opp_nb);

	if (event == OPP_EVENT_ADJUST_VOLTAGE) {
		freq = dev_pm_opp_get_freq(opp);

		mutex_lock(&info->reg_lock);
		if (info->current_freq == freq) {
			volt = dev_pm_opp_get_voltage(opp);
			ret = mtk_cpufreq_set_voltage(info, volt);
			if (ret)
				dev_err(info->cpu_dev,
					"failed to scale voltage: %d\n", ret);
		}
		mutex_unlock(&info->reg_lock);
	} else if (event == OPP_EVENT_DISABLE) {
		freq = dev_pm_opp_get_freq(opp);

		/* case of current opp item is disabled */
		if (info->current_freq == freq) {
			freq = 1;
			new_opp = dev_pm_opp_find_freq_ceil(info->cpu_dev,
							    &freq);
			if (IS_ERR(new_opp)) {
				dev_err(info->cpu_dev,
					"all opp items are disabled\n");
				ret = PTR_ERR(new_opp);
				return notifier_from_errno(ret);
			}

			dev_pm_opp_put(new_opp);

			struct cpufreq_policy *policy __free(put_cpufreq_policy)
				= cpufreq_cpu_get(info->opp_cpu);
			if (policy)
				cpufreq_driver_target(policy, freq / 1000,
						      CPUFREQ_RELATION_L);
		}
	}

	return notifier_from_errno(ret);
}

static struct device *of_get_cci(struct device *cpu_dev)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_parse_phandle(cpu_dev->of_node, "mediatek,cci", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	return &pdev->dev;
}

/*
 * Some SoCs ship several speed bins that share one OPP table but need
 * different voltages. When the CPU node carries a "speed-bin" nvmem cell,
 * use it as the opp-supported-hw version so the OPP core keeps only the
 * entries that match this part. The cell value is used directly as a bit
 * index, so the DT encodes the per-bin grouping in opp-supported-hw.
 */
static int mtk_cpu_opp_set_supported_hw(struct mtk_cpu_dvfs_info *info)
{
	struct device *cpu_dev = info->cpu_dev;
	struct dev_pm_opp_config config = {};
	unsigned int supported_hw;
	u8 bin;
	int ret;

	ret = nvmem_cell_read_u8(cpu_dev, "speed_grade", &bin);
	if (ret == -ENOENT || ret == -EOPNOTSUPP)
		return 0;	/* No speed binning (or nvmem) on this SoC. */
	if (ret)
		return ret;

	supported_hw = BIT(bin);
	config.supported_hw = &supported_hw;
	config.supported_hw_count = 1;

	ret = dev_pm_opp_set_config(cpu_dev, &config);
	if (ret < 0)
		return ret;

	info->opp_token = ret;

	return 0;
}

/* Preserve board mappings and supply aliases as well as DT supplies. */
static struct regulator *mtk_cpufreq_get_regulator(struct device *dev,
						   const char *name,
						   const char *legacy_name)
{
	struct regulator *reg = regulator_get_optional(dev, name);

	if (IS_ERR(reg) && PTR_ERR(reg) == -ENODEV)
		reg = regulator_get_optional(dev, legacy_name);

	return reg;
}

static int mtk_cpu_dvfs_info_init(struct mtk_cpu_dvfs_info *info, int cpu)
{
	struct device *cpu_dev;
	struct dev_pm_opp *opp;
	unsigned long rate;
	int ret;

	if (cpu < 8 && of_machine_is_compatible("planet,gemini-pda")) {
		int sibling;

		info->cpu_dev = get_cpu_device(cpu / 4 * 4);
		info->rail_user = cpu / 4;
		ret = dev_pm_opp_of_get_sharing_cpus(info->cpu_dev, &info->cpus);
		if (ret)
			return ret;
		if (!cpumask_test_cpu(cpu, &info->cpus))
			return -EINVAL;
		for_each_cpu(sibling, &info->cpus)
			if (sibling / 4 != info->rail_user)
				return -EINVAL;
		return mt6797_consumer_acquire(info->cpu_dev, info->rail_user,
					       &info->rail_cookie);
	}

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev)
		return dev_err_probe(cpu_dev, -ENODEV, "failed to get cpu%d device\n", cpu);
	info->cpu_dev = cpu_dev;

	info->ccifreq_bound = false;
	if (info->soc_data->ccifreq_supported) {
		info->cci_dev = of_get_cci(info->cpu_dev);
		if (IS_ERR(info->cci_dev))
			return dev_err_probe(cpu_dev, PTR_ERR(info->cci_dev),
					     "cpu%d: failed to get cci device\n",
					     cpu);
	}

	info->cpu_clk = clk_get(cpu_dev, "cpu");
	if (IS_ERR(info->cpu_clk)) {
		ret = PTR_ERR(info->cpu_clk);
		dev_err_probe(cpu_dev, ret, "cpu%d: failed to get cpu clk\n", cpu);
		goto out_put_cci_dev;
	}

	info->inter_clk = clk_get(cpu_dev, "intermediate");
	if (IS_ERR(info->inter_clk)) {
		ret = PTR_ERR(info->inter_clk);
		dev_err_probe(cpu_dev, ret,
			      "cpu%d: failed to get intermediate clk\n", cpu);
		goto out_free_mux_clock;
	}

	/* "cpu"/"mem" are the current supply names; "proc"/"sram" the old ones. */
	info->proc_reg = mtk_cpufreq_get_regulator(cpu_dev, "cpu", "proc");
	if (IS_ERR(info->proc_reg)) {
		ret = PTR_ERR(info->proc_reg);
		/* MT6797 boards may intentionally leave a cluster unmanaged. */
		if (ret == -ENODEV && info->soc_data->allow_unmanaged) {
			ret = 1; /* Successfully identified an unmanaged cluster. */
			goto out_free_inter_clock;
		}
		dev_err_probe(cpu_dev, ret,
			      "cpu%d: failed to get proc regulator\n", cpu);
		goto out_free_inter_clock;
	}

	ret = regulator_enable(info->proc_reg);
	if (ret) {
		dev_err_probe(cpu_dev, ret, "cpu%d: failed to enable vproc\n", cpu);
		goto out_free_proc_reg;
	}

	/* Both presence and absence of sram regulator are valid cases. */
	info->sram_reg = mtk_cpufreq_get_regulator(cpu_dev, "mem", "sram");
	if (IS_ERR(info->sram_reg)) {
		ret = PTR_ERR(info->sram_reg);
		if (ret != -ENODEV) {
			dev_err_probe(cpu_dev, ret,
				      "cpu%d: Failed to get sram regulator\n", cpu);
			goto out_disable_proc_reg;
		}

		info->sram_reg = NULL;
	} else {
		ret = regulator_enable(info->sram_reg);
		if (ret) {
			dev_err_probe(cpu_dev, ret, "cpu%d: failed to enable vsram\n", cpu);
			goto out_free_sram_reg;
		}
	}

	/* Get OPP-sharing information from "operating-points-v2" bindings */
	ret = dev_pm_opp_of_get_sharing_cpus(cpu_dev, &info->cpus);
	if (ret) {
		dev_err_probe(cpu_dev, ret,
			"cpu%d: failed to get OPP-sharing information\n", cpu);
		goto out_disable_sram_reg;
	}

	ret = mtk_cpu_opp_set_supported_hw(info);
	if (ret) {
		dev_err_probe(cpu_dev, ret,
			      "cpu%d: failed to set supported hw\n", cpu);
		goto out_disable_sram_reg;
	}

	ret = dev_pm_opp_of_cpumask_add_table(&info->cpus);
	if (ret) {
		dev_err_probe(cpu_dev, ret, "cpu%d: no OPP table\n", cpu);
		goto out_clear_opp_hw;
	}

	ret = clk_prepare_enable(info->cpu_clk);
	if (ret) {
		dev_err_probe(cpu_dev, ret, "cpu%d: failed to enable cpu clk\n", cpu);
		goto out_free_opp_table;
	}

	ret = clk_prepare_enable(info->inter_clk);
	if (ret) {
		dev_err_probe(cpu_dev, ret, "cpu%d: failed to enable inter clk\n", cpu);
		goto out_disable_mux_clock;
	}

	if (info->soc_data->ccifreq_supported) {
		info->vproc_on_boot = regulator_get_voltage(info->proc_reg);
		if (info->vproc_on_boot < 0) {
			ret = dev_err_probe(info->cpu_dev, info->vproc_on_boot,
					    "invalid Vproc value\n");
			goto out_disable_inter_clock;
		}
	}

	/* Search a safe voltage for intermediate frequency. */
	rate = clk_get_rate(info->inter_clk);
	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &rate);
	if (IS_ERR(opp)) {
		ret = dev_err_probe(cpu_dev, PTR_ERR(opp),
				    "cpu%d: failed to get intermediate opp\n", cpu);
		goto out_disable_inter_clock;
	}
	info->intermediate_voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);

	mutex_init(&info->reg_lock);
	info->current_freq = clk_get_rate(info->cpu_clk);

	info->opp_cpu = cpu;
	info->opp_nb.notifier_call = mtk_cpufreq_opp_notifier;
	ret = dev_pm_opp_register_notifier(cpu_dev, &info->opp_nb);
	if (ret) {
		dev_err_probe(cpu_dev, ret, "cpu%d: failed to register opp notifier\n", cpu);
		goto out_disable_inter_clock;
	}

	/*
	 * If SRAM regulator is present, software "voltage tracking" is needed
	 * for this CPU power domain.
	 */
	info->need_voltage_tracking = (info->sram_reg != NULL);

	/*
	 * We assume min voltage is 0 and tracking target voltage using
	 * min_volt_shift for each iteration.
	 * The vtrack_max is 3 times of expeted iteration count.
	 */
	info->vtrack_max = 3 * DIV_ROUND_UP(max(info->soc_data->sram_max_volt,
						info->soc_data->proc_max_volt),
					    info->soc_data->min_volt_shift);

	return 0;

out_disable_inter_clock:
	clk_disable_unprepare(info->inter_clk);

out_disable_mux_clock:
	clk_disable_unprepare(info->cpu_clk);

out_free_opp_table:
	dev_pm_opp_of_cpumask_remove_table(&info->cpus);

out_clear_opp_hw:
	dev_pm_opp_clear_config(info->opp_token);

out_disable_sram_reg:
	if (info->sram_reg)
		regulator_disable(info->sram_reg);

out_free_sram_reg:
	if (info->sram_reg)
		regulator_put(info->sram_reg);

out_disable_proc_reg:
	regulator_disable(info->proc_reg);

out_free_proc_reg:
	regulator_put(info->proc_reg);

out_free_inter_clock:
	clk_put(info->inter_clk);

out_free_mux_clock:
	clk_put(info->cpu_clk);

out_put_cci_dev:
	if (info->soc_data->ccifreq_supported)
		put_device(info->cci_dev);

	return ret;
}

static void mtk_cpu_dvfs_info_release(struct mtk_cpu_dvfs_info *info)
{
	if (info->rail_cookie) {
		mt6797_consumer_release(info->rail_user, info->rail_cookie);
		info->rail_cookie = 0;
		return;
	}
	regulator_disable(info->proc_reg);
	regulator_put(info->proc_reg);
	if (info->sram_reg) {
		regulator_disable(info->sram_reg);
		regulator_put(info->sram_reg);
	}
	clk_disable_unprepare(info->cpu_clk);
	clk_put(info->cpu_clk);
	clk_disable_unprepare(info->inter_clk);
	clk_put(info->inter_clk);
	dev_pm_opp_of_cpumask_remove_table(&info->cpus);
	dev_pm_opp_clear_config(info->opp_token);
	dev_pm_opp_unregister_notifier(info->cpu_dev, &info->opp_nb);
	if (info->soc_data->ccifreq_supported)
		put_device(info->cci_dev);
}

static int mtk_cpufreq_init(struct cpufreq_policy *policy)
{
	struct mtk_cpu_dvfs_info *info;
	struct cpufreq_frequency_table *freq_table;
	struct clk *parent;
	unsigned int cur_khz;
	int ret, idx, i;

	info = mtk_cpu_dvfs_info_lookup(policy->cpu);
	if (!info)
		return -ENODEV;

	ret = dev_pm_opp_init_cpufreq_table(info->cpu_dev, &freq_table);
	if (ret) {
		dev_err(info->cpu_dev,
			"failed to init cpufreq table for cpu%d: %d\n",
			policy->cpu, ret);
		return ret;
	}

	cpumask_copy(policy->cpus, &info->cpus);
	policy->freq_table = freq_table;
	policy->driver_data = info;
	policy->clk = info->cpu_clk;

	if (info->soc_data->firmware_pll) {
		/*
		 * .set_target() takes clk_get_parent(cpu_clk) as the cluster
		 * ARMPLL and calls clk_set_rate() on it, so the CPU clock must
		 * boot parented to the ARMPLL. Refuse a mux left on an
		 * undescribed source (reads back as no parent) or on the
		 * intermediate clock (clk26m, whose rate requests can propagate
		 * into a system PLL) -- either would retune the wrong clock.
		 */
		parent = clk_get_parent(policy->clk);
		if (!parent || clk_is_match(parent, info->inter_clk)) {
			dev_err(info->cpu_dev,
				"cpu%d: CPU clock not parented to the cluster PLL\n",
				policy->cpu);
			ret = -ENODEV;
			goto free_table;
		}

		/*
		 * cpufreq_online() uses CPUFREQ_NEED_INITIAL_FREQ_CHECK to force
		 * the CPU onto a listed OPP when it boots at an unlisted rate,
		 * but it BUG_ON()s if that forced transition fails. A
		 * firmware-backed ARMPLL can fail its first setpoint, so do the
		 * sync here, where a failure drops the policy cleanly. Program
		 * the lowest OPP at or above the boot rate (as
		 * CPUFREQ_RELATION_L would). The firmware PLL reads back as 0
		 * until its first .set_rate() (see clk-mt6797-mcu), so at init
		 * cur_khz is 0 and this always selects the lowest listed OPP.
		 */
		cur_khz = clk_get_rate(policy->clk) / 1000;
		idx = -1;
		for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
			if (freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
				continue;
			idx = i;
			if (freq_table[i].frequency >= cur_khz)
				break;
		}
		if (idx >= 0) {
			ret = mtk_cpufreq_set_target(policy, idx);
			if (ret) {
				dev_err(info->cpu_dev,
					"cpu%d: failed to apply an initial OPP: %d\n",
					policy->cpu, ret);
				goto free_table;
			}
		}
	}

	return 0;

free_table:
	dev_pm_opp_free_cpufreq_table(info->cpu_dev, &policy->freq_table);
	return ret;
}

static void mtk_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct mtk_cpu_dvfs_info *info = policy->driver_data;

	dev_pm_opp_free_cpufreq_table(info->cpu_dev, &policy->freq_table);
}

static struct cpufreq_driver mtk_cpufreq_driver = {
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK |
		 CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
		 CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = mtk_cpufreq_set_target,
	.get = cpufreq_generic_get,
	.init = mtk_cpufreq_init,
	.exit = mtk_cpufreq_exit,
	.register_em = cpufreq_register_em_with_opp,
	.name = "mtk-cpufreq",
};

static int mtk_cpufreq_probe(struct platform_device *pdev)
{
	const struct mtk_cpufreq_platform_data *data;
	struct mtk_cpu_dvfs_info *info, *tmp;
	int cpu, ret;

	data = dev_get_platdata(&pdev->dev);
	if (!data)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "failed to get mtk cpufreq platform data\n");

	for_each_present_cpu(cpu) {
		struct device *cpu_dev;

		/* Protected A53 initialization does not admit the A72/B path. */
		if (cpu >= 8 && of_machine_is_compatible("planet,gemini-pda"))
			continue;
		info = mtk_cpu_dvfs_info_lookup(cpu);
		if (info)
			continue;

		/* Only MT6797 explicitly supports unmanaged clusters. */
		cpu_dev = get_cpu_device(cpu);
		if (data->allow_unmanaged && cpu_dev &&
		    !of_property_present(cpu_dev->of_node, "operating-points-v2"))
			continue;

		info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
		if (!info) {
			ret = dev_err_probe(&pdev->dev, -ENOMEM,
					    "Failed to allocate dvfs_info\n");
			goto release_dvfs_info_list;
		}

		info->soc_data = data;
		ret = mtk_cpu_dvfs_info_init(info, cpu);
		if (ret == 1 && data->allow_unmanaged)
			continue;
		if (ret)
			goto release_dvfs_info_list;

		list_add(&info->list_head, &dvfs_info_list);
	}

	/* Protected initialization must never publish operational callbacks. */
	if (of_machine_is_compatible("planet,gemini-pda")) {
		ret = -EAGAIN;
		goto release_dvfs_info_list;
	}
	ret = cpufreq_register_driver(&mtk_cpufreq_driver);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to register mtk cpufreq driver\n");
		goto release_dvfs_info_list;
	}

	return 0;

release_dvfs_info_list:
	list_for_each_entry_safe(info, tmp, &dvfs_info_list, list_head) {
		mtk_cpu_dvfs_info_release(info);
		list_del(&info->list_head);
	}

	return ret;
}

static struct platform_driver mtk_cpufreq_platdrv = {
	.driver = {
		.name	= "mtk-cpufreq",
	},
	.probe		= mtk_cpufreq_probe,
};

static const struct mtk_cpufreq_platform_data mt2701_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1150000,
	.sram_min_volt = 0,
	.sram_max_volt = 1150000,
	.ccifreq_supported = false,
};

static const struct mtk_cpufreq_platform_data mt6797_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 300000,
	.proc_max_volt = 1200000,
	.sram_min_volt = 1000000,
	.sram_max_volt = 1200000,
	.ccifreq_supported = false,
	.firmware_pll = true,
	.allow_unmanaged = true,
};

static const struct mtk_cpufreq_platform_data mt7622_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1350000,
	.sram_min_volt = 0,
	.sram_max_volt = 1350000,
	.ccifreq_supported = false,
};

static const struct mtk_cpufreq_platform_data mt7623_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1300000,
	.ccifreq_supported = false,
};

static const struct mtk_cpufreq_platform_data mt7988_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 900000,
	.sram_min_volt = 0,
	.sram_max_volt = 1150000,
	.ccifreq_supported = true,
};

static const struct mtk_cpufreq_platform_data mt8183_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1150000,
	.sram_min_volt = 0,
	.sram_max_volt = 1150000,
	.ccifreq_supported = true,
};

static const struct mtk_cpufreq_platform_data mt8186_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 250000,
	.proc_max_volt = 1118750,
	.sram_min_volt = 850000,
	.sram_max_volt = 1118750,
	.ccifreq_supported = true,
};

static const struct mtk_cpufreq_platform_data mt8516_platform_data = {
	.min_volt_shift = 100000,
	.max_volt_shift = 200000,
	.proc_max_volt = 1310000,
	.sram_min_volt = 0,
	.sram_max_volt = 1310000,
	.ccifreq_supported = false,
};

/* List of machines supported by this driver */
static const struct of_device_id mtk_cpufreq_machines[] __initconst __maybe_unused = {
	{ .compatible = "mediatek,mt2701", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt2712", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt6797", .data = &mt6797_platform_data },
	{ .compatible = "mediatek,mt7622", .data = &mt7622_platform_data },
	{ .compatible = "mediatek,mt7623", .data = &mt7623_platform_data },
	{ .compatible = "mediatek,mt7988a", .data = &mt7988_platform_data },
	{ .compatible = "mediatek,mt8167", .data = &mt8516_platform_data },
	{ .compatible = "mediatek,mt817x", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt8173", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt8176", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt8183", .data = &mt8183_platform_data },
	{ .compatible = "mediatek,mt8186", .data = &mt8186_platform_data },
	{ .compatible = "mediatek,mt8365", .data = &mt2701_platform_data },
	{ .compatible = "mediatek,mt8516", .data = &mt8516_platform_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_cpufreq_machines);

static int __init mtk_cpufreq_driver_init(void)
{
	const struct mtk_cpufreq_platform_data *data;
	int err;

	data = of_machine_get_match_data(mtk_cpufreq_machines);
	if (!data) {
		pr_debug("Machine is not compatible with mtk-cpufreq\n");
		return -ENODEV;
	}

	err = platform_driver_register(&mtk_cpufreq_platdrv);
	if (err)
		return err;

	/*
	 * Since there's no place to hold device registration code and no
	 * device tree based way to match cpufreq driver yet, both the driver
	 * and the device registration codes are put here to handle defer
	 * probing.
	 */
	cpufreq_pdev = platform_device_register_data(NULL, "mtk-cpufreq", -1,
						     data, sizeof(*data));
	if (IS_ERR(cpufreq_pdev)) {
		pr_err("failed to register mtk-cpufreq platform device\n");
		platform_driver_unregister(&mtk_cpufreq_platdrv);
		return PTR_ERR(cpufreq_pdev);
	}

	return 0;
}
module_init(mtk_cpufreq_driver_init)

static void __exit mtk_cpufreq_driver_exit(void)
{
	platform_device_unregister(cpufreq_pdev);
	platform_driver_unregister(&mtk_cpufreq_platdrv);
}
module_exit(mtk_cpufreq_driver_exit)

MODULE_DESCRIPTION("MediaTek CPUFreq driver");
MODULE_AUTHOR("Pi-Cheng Chen <pi-cheng.chen@linaro.org>");
MODULE_LICENSE("GPL v2");
