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
 * Some closed EL3 firmware advertises the PSCI CPU_OFF function but cannot
 * actually power a given CPU down. On the MediaTek MT6797 the Cortex-A72 "B"
 * cluster is powered up out-of-band by the bootloader, and the vendor TF-A it
 * ships (there is no upstream replacement) has no pwr_domain_off for it, yet
 * still exposes standard CPU_OFF. Issuing CPU_OFF to such a CPU wedges it in
 * EL3 and hangs the SoC until the watchdog resets it.
 *
 * There is no PSCI-level or firmware-level way to discover this, so the
 * affected CPUs are keyed off the SoC root compatible: on a matching machine
 * every CPU in the named MPIDR affinity-level-1 cluster is flagged. The set is
 * computed once at init into a cpumask, so the hotplug and crash paths only
 * test a mask and never walk the device tree (which takes devtree_lock and
 * could deadlock in NMI/crash context). Flagged CPUs are kept non-removable
 * and are never CPU_OFF'd.
 *
 * This is deliberately narrow and self-contained -- no DT binding and no
 * generic PSCI property -- because the proper home for the power sequence is
 * firmware (TF-A pwr_domain_off); this only works around an unreplaceable
 * firmware's limitation on one SoC.
 */
struct psci_cpu_off_broken_soc {
	const char	*compatible;
	u32		aff1;	/* MPIDR affinity level 1 of the broken cluster */
};

static const struct psci_cpu_off_broken_soc psci_cpu_off_broken_socs[] __initconst = {
	{ "mediatek,mt6797", 2 },	/* Cortex-A72 "B" cluster (cpu@200/@201) */
	{ /* sentinel */ }
};

static cpumask_t psci_cpu_off_broken_mask __ro_after_init;

static void __init psci_init_cpu_off_broken(void)
{
	const struct psci_cpu_off_broken_soc *soc;
	unsigned int cpu;

	for (soc = psci_cpu_off_broken_socs; soc->compatible; soc++) {
		if (!of_machine_is_compatible(soc->compatible))
			continue;

		/*
		 * This runs from cpu_psci_cpu_init() during smp_init_cpus(),
		 * before the CPUs are added to the possible mask, so iterate
		 * the logical map directly (of_parse_and_init_cpus() has
		 * already populated it).
		 */
		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			u64 mpidr = cpu_logical_map(cpu);

			if (mpidr == INVALID_HWID)
				continue;
			if (MPIDR_AFFINITY_LEVEL(mpidr, 1) == soc->aff1)
				cpumask_set_cpu(cpu, &psci_cpu_off_broken_mask);
		}
	}

	if (!cpumask_empty(&psci_cpu_off_broken_mask))
		pr_info("firmware cannot CPU_OFF CPUs %*pbl; keeping them non-removable\n",
			cpumask_pr_args(&psci_cpu_off_broken_mask));
}

/*
 * True if any currently-online CPU cannot be powered off by firmware. A regular
 * kexec offlines every secondary CPU before the jump, which such a CPU cannot
 * survive, so the kexec load is refused (see machine_kexec_prepare()).
 */
bool psci_broken_cpu_off_online(void)
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

