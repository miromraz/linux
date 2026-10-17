// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>

/*
 * CPUs whose firmware advertises PSCI CPU_OFF but cannot actually power them
 * down (listed in the PSCI node's "mediatek,broken-cpu-off-cpus" phandle
 * list). Issuing CPU_OFF to such a CPU wedges it instead of parking it and
 * hangs the SoC; these CPUs are therefore never offlined and never CPU_OFF'd.
 * Parsed once at init so the hotplug/crash paths only test a cpumask and never
 * walk the device tree (which takes devtree_lock and could deadlock in NMI /
 * crash context).
 */
static cpumask_t psci_cpu_off_broken_mask __ro_after_init;

static void __init psci_init_cpu_off_broken(void)
{
	struct device_node *np, *cpu_np;
	int i, n;

	np = of_find_node_with_property(NULL, "mediatek,broken-cpu-off-cpus");
	if (!np)
		return;

	n = of_count_phandle_with_args(np, "mediatek,broken-cpu-off-cpus", NULL);
	for (i = 0; i < n; i++) {
		int cpu;

		cpu_np = of_parse_phandle(np, "mediatek,broken-cpu-off-cpus", i);
		if (!cpu_np)
			continue;

		cpu = of_cpu_node_to_id(cpu_np);
		of_node_put(cpu_np);
		if (cpu >= 0)
			cpumask_set_cpu(cpu, &psci_cpu_off_broken_mask);
	}

	of_node_put(np);
}

/*
 * True if any currently-online CPU cannot be powered off by firmware. A regular
 * kexec offlines every secondary CPU before the jump, which such a CPU cannot
 * survive, so the kexec load is refused (see machine_kexec_prepare()).
 */
bool psci_has_broken_cpu_off_online(void)
{
	return cpumask_intersects(cpu_online_mask, &psci_cpu_off_broken_mask);
}

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	static bool parsed;

	if (!parsed) {
		psci_init_cpu_off_broken();
		parsed = true;
	}

	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err && err != -EPERM)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool psci_cpu_off_broken(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, &psci_cpu_off_broken_mask);
}

static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu) && !psci_cpu_off_broken(cpu);
}

static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	/* Firmware cannot power this CPU down; CPU_OFF would hang it */
	if (psci_cpu_off_broken(cpu))
		return -EOPNOTSUPP;

	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	u32 state;

	/*
	 * Firmware cannot power this CPU down. Return without issuing CPU_OFF so
	 * the caller drops into cpu_park_loop(). The regular hotplug path never
	 * reaches here for such a CPU (cpu_disable() refuses it first); this
	 * guards the crash-stop path (__cpu_try_die()).
	 */
	if (psci_cpu_off_broken(cpu))
		return;

	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);
}

static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

