// SPDX-License-Identifier: GPL-2.0-only
/* Immutable production ownership. No conversion/admission entry point. */
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "mt6797-provider.h"

static DEFINE_MUTEX(owner_lock);
static struct mt6797_rail_coordinator owner = {
	.lock = __MUTEX_INITIALIZER(owner.lock),
};
static struct device_node *rails[MT6797_RAILS];
static unsigned long attached;
static BLOCKING_NOTIFIER_HEAD(events);
static void preflight_work(struct work_struct *work);
static DECLARE_WORK(preflight_job, preflight_work);
static bool preflight_active;
static void preflight_notify(struct work_struct *work);
static DECLARE_WORK(notify_job, preflight_notify);
static bool stopping;
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
static bool fixture_active;
#endif
static struct mt6797_preflight_result preflight = { .error = -EPROBE_DEFER };

/* owner_lock serializes invalidation with publication and status snapshots. */
static void preflight_invalidate(void)
{
	preflight.generation++;
	preflight.error = -EAGAIN;
	preflight.provider_generation = owner.generation;
	if (!stopping)
		schedule_work(&notify_job);
	if (preflight_active)
		schedule_work(&preflight_job);
}

/* Accept either spelling, but never accept conflicting or broken aliases. */
static struct device_node *supply(struct device_node *user, const char *a,
				  const char *b)
{
	struct device_node *first = NULL, *second = NULL;

	if (!user || !of_device_is_available(user))
		return ERR_PTR(-EINVAL);
	if (of_property_present(user, a)) {
		if (of_property_count_u32_elems(user, a) != 1)
			return ERR_PTR(-EINVAL);
		first = of_parse_phandle(user, a, 0);
		if (!first)
			return ERR_PTR(-EINVAL);
	}
	if (of_property_present(user, b)) {
		if (of_property_count_u32_elems(user, b) != 1) {
			of_node_put(first);
			return ERR_PTR(-EINVAL);
		}
		second = of_parse_phandle(user, b, 0);
		if (!second || (first && first != second)) {
			of_node_put(first);
			of_node_put(second);
			return ERR_PTR(-EINVAL);
		}
	}
	if (!first)
		first = of_node_get(second);
	of_node_put(second);
	return first ?: ERR_PTR(-EINVAL);
}

/* Reject legacy aliases and duplicate physical matches on Gemini. This is
 * called for BOTH children before registration, including an unguarded B.
 * Checking each child separately preserves an unrelated B when A is malformed.
 */
int mt6797_provider_buck_match(struct device_node *provider, unsigned int id)
{
	struct device_node *parent, *child, *users[3], *np;
	const char *name = id ? "BUCKB" : "BUCKA", *match;
	const char * const props[] = { "cpu-supply", "proc-supply" };
	int i, j, count = 0, ret = 0;

	if (!of_machine_is_compatible("planet,gemini-pda"))
		return 0;
	if (!provider || id > 1 || !of_device_is_available(provider) ||
	    !of_node_name_eq(provider, name) ||
	    of_property_present(provider, "regulator-compatible"))
		return -EINVAL;
	parent = of_get_parent(provider);
	for_each_child_of_node(parent, child) {
		if (of_property_read_string(child, "regulator-compatible", &match))
			match = child->name;
		if (!strcmp(match, name))
			count++;
	}
	of_node_put(parent);
	if (count != 1)
		return -EINVAL;
	if (!id)
		return 0;
	/* Even a canonically named B must not receive a shared user's phandle.
	 * Inspect each alias independently: malformed A must not hide a B alias.
	 */
	users[0] = of_get_cpu_node(0, NULL);
	users[1] = of_get_cpu_node(4, NULL);
	users[2] = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-cci");
	for (i = 0; i < ARRAY_SIZE(users); i++) {
		for (j = 0; j < ARRAY_SIZE(props); j++) {
			np = of_parse_phandle(users[i], props[j], 0);
			if (np == provider)
				ret = -EINVAL;
			of_node_put(np);
		}
		of_node_put(users[i]);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_buck_match);

/* owner_lock protects retained identities, including across absent peers. */
static int topology(void)
{
	struct device_node *users[3], *found[2] = {}, *np, *parent, *chip;
	int i, j, ret = 0;

	users[0] = of_get_cpu_node(0, NULL);
	users[1] = of_get_cpu_node(4, NULL);
	users[2] = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-cci");
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 2; j++) {
			np = j ? supply(users[i], "sram-supply", "mem-supply") :
				supply(users[i], "proc-supply", "cpu-supply");
			if (IS_ERR(np)) {
				ret = PTR_ERR(np);
				goto out;
			}
			if (!of_device_is_available(np) ||
			    (found[j] && found[j] != np))
				ret = -EINVAL;
			if (!found[j])
				found[j] = of_node_get(np);
			of_node_put(np);
			if (ret)
				goto out;
		}
	}
	parent = of_get_parent(found[0]);
	chip = of_get_parent(parent);
	if (mt6797_provider_buck_match(found[0], 0) ||
	    !of_node_name_eq(parent, "regulators") ||
	    !of_device_is_compatible(chip, "dlg,da9214") ||
	    !of_device_is_available(chip) ||
	    !of_device_is_compatible(found[1], "mediatek,mt6797-cpu-sram"))
		ret = -EINVAL;
	of_node_put(chip);
	of_node_put(parent);
	parent = of_get_parent(found[1]);
	if (!of_device_is_compatible(parent, "mediatek,mt6797-infracfg") ||
	    !of_device_is_available(parent))
		ret = -EINVAL;
	of_node_put(parent);
	for (j = 0; !ret && j < 2; j++)
		if (rails[j] && rails[j] != found[j])
			ret = -ESTALE;
	if (!ret)
		for (j = 0; j < 2; j++)
			if (!rails[j])
				rails[j] = of_node_get(found[j]);
out:
	for (i = 0; i < 3; i++)
		of_node_put(users[i]);
	for (j = 0; j < 2; j++)
		of_node_put(found[j]);
	return ret;
}

/* Only this worker owns these references. Consumers remain blocked before
 * configuring OPPs, so none can clear the retained supported-hw configuration.
 */
struct preflight_user {
	struct device *dev;
	struct clk *clk[2];
	struct notifier_block opp_nb, clk_nb[2];
	int token;
	bool table, opp_notify, clk_notify[2];
	u8 bin;
};
static struct preflight_user pf_users[MT6797_RAIL_USERS];

static int preflight_opp_event(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	mutex_lock(&owner_lock);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return NOTIFY_OK;
}

static int preflight_clock_event(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	mutex_lock(&owner_lock);
	/* clk_get_rate() in the worker takes CCF's prepare lock, so it waits
	 * for this operation even when CCF emits no POST after a failed write.
	 * Generation validation rejects a new PRE during measurement.
	 */
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return NOTIFY_OK;
}

static void preflight_release(void)
{
	int i, j;

	for (i = MT6797_RAIL_USERS - 1; i >= 0; i--) {
		struct preflight_user *u = &pf_users[i];

		if (!u->dev)
			continue;
		for (j = 0; j < 2; j++) {
			if (u->clk_notify[j])
				clk_notifier_unregister(u->clk[j], &u->clk_nb[j]);
			clk_put(u->clk[j]);
		}
		if (u->opp_notify)
			dev_pm_opp_unregister_notifier(u->dev, &u->opp_nb);
		if (u->table)
			dev_pm_opp_of_remove_table(u->dev);
		if (u->token > 0)
			dev_pm_opp_clear_config(u->token);
		put_device(u->dev);
		memset(u, 0, sizeof(*u));
	}
}

static int preflight_acquire(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	int i, j, ret;

	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct preflight_user *u = &pf_users[i];
		struct dev_pm_opp_config config = {};
		struct device *dev;
		u8 bin;
		unsigned int hw;

		if (i < 2) {
			dev = get_device(get_cpu_device(i * 4));
		} else {
			np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-cci");
			pdev = of_find_device_by_node(np);
			of_node_put(np);
			dev = pdev ? &pdev->dev : NULL;
		}
		if (!dev)
			return -EPROBE_DEFER;
		if (u->dev) {
			put_device(dev);
			if (u->dev != dev)
				return -ESTALE;
		} else {
			u->dev = dev;
		}
		ret = nvmem_cell_read_u8(u->dev, "speed_grade", &bin);
		if (ret)
			return ret;
		/* FUNC_CODE_1 is four bits. Never shift an unchecked efuse. */
		ret = mt6797_preflight_hw(bin, &hw);
		if (ret)
			return ret;
		/* Never recreate a filtered table: that would restore disabled OPPs
		 * and discard adjusted voltages. A changed bin needs explicit
		 * policy reconciliation, which this immutable owner cannot grant.
		 */
		if (u->token > 0 && u->bin != bin)
			return -ESTALE;
		u->bin = bin;
		if (i && u->bin != pf_users[0].bin)
			return -EINVAL;
		config.supported_hw = &hw;
		config.supported_hw_count = 1;
		ret = u->token > 0 ? u->token :
			dev_pm_opp_set_config(u->dev, &config);
		if (ret < 0)
			return ret;
		u->token = ret;
		ret = u->table ? 0 : dev_pm_opp_of_add_table(u->dev);
		if (ret)
			return ret;
		u->table = true;
		if (!u->opp_notify) {
			u->opp_nb.notifier_call = preflight_opp_event;
			ret = dev_pm_opp_register_notifier(u->dev, &u->opp_nb);
			if (ret)
				return ret;
			u->opp_notify = true;
		}
		for (j = 0; j < 2; j++) {
			struct clk *clk = clk_get(u->dev, j ? "intermediate" :
						 (i == 2 ? "cci" : "cpu"));

			if (IS_ERR(clk))
				return PTR_ERR(clk);
			if (u->clk[j] && !clk_is_match(u->clk[j], clk)) {
				/* Replace only the clock reference, never the OPP policy.
				 * Unregistration drains callbacks outside owner_lock.
				 */
				if (u->clk_notify[j])
					clk_notifier_unregister(u->clk[j], &u->clk_nb[j]);
				u->clk_notify[j] = false;
				clk_put(u->clk[j]);
				u->clk[j] = NULL;
			}
			if (u->clk[j])
				clk_put(clk);
			else
				u->clk[j] = clk;
			if (u->clk_notify[j])
				continue;
			u->clk_nb[j].notifier_call = preflight_clock_event;
			ret = clk_notifier_register(u->clk[j], &u->clk_nb[j]);
			if (ret)
				return ret;
			u->clk_notify[j] = true;
		}
	}
	return 0;
}

/* Shared with the focused OPP-core fixture; neither helper grants admission. */
int mt6797_preflight_hw(u8 bin, unsigned int *hw)
{
	if (bin > 15)
		return -EINVAL;
	*hw = BIT(bin);
	return 0;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_hw);

int mt6797_preflight_ceiling(struct device *dev, unsigned long rate,
			    unsigned long *freq, unsigned long *uv)
{
	struct dev_pm_opp *opp;
	unsigned long lower = rate;

	if (!rate)
		return -EINVAL;
	/* A ceiling alone would silently accept an unknown sub-table rate. */
	opp = dev_pm_opp_find_freq_floor(dev, &lower);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);
	opp = dev_pm_opp_find_freq_ceil(dev, &rate);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	*freq = rate;
	*uv = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	return !*uv || *uv > INT_MAX ? -EINVAL : 0;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_ceiling);

/* No rounding, programming, clock enable, regulator handles or admission. */
static int preflight_measure(struct mt6797_preflight_result *r)
{
	int i, j;

	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		r->speedbin[i] = pf_users[i].bin;
		for (j = 0; j < 2; j++) {
			unsigned long rate = clk_get_rate(pf_users[i].clk[j]);
			int ret;

			r->rate[i][j] = rate;
			ret = mt6797_preflight_ceiling(pf_users[i].dev, rate,
						     &r->ceiling[i][j], &r->uv[i][j]);
			if (ret)
				return ret;
		}
	}
	return mt6797_provider_sample(r->provider_generation, r->rail_uv);
}

static void preflight_work(struct work_struct *work)
{
	struct mt6797_preflight_result result = {};
	int ret;

	mutex_lock(&owner_lock);
	if (!preflight_active) {
		mutex_unlock(&owner_lock);
		return;
	}
	result.generation = preflight.generation;
	result.provider_generation = owner.generation;
	ret = topology();
	mutex_unlock(&owner_lock);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
	if (fixture_active) {
		ret = 0;
	} else
#endif
	{
		if (!ret)
			ret = preflight_acquire();
	}
	if (!ret)
		ret = preflight_measure(&result);
	result.error = ret;
	mutex_lock(&owner_lock);
	if (result.generation == preflight.generation &&
	    result.provider_generation == owner.generation && preflight_active) {
		preflight = result;
		schedule_work(&notify_job);
	}
	mutex_unlock(&owner_lock);
}

/* Match consumer nodes and their clock/NVMEM supplier ancestry. No device
 * references or framework locks are borrowed from the notification payload.
 */
static bool preflight_node_contains(struct device_node *np,
				    struct device_node *supplier)
{
	for (; np; np = np->parent)
		if (np == supplier)
			return true;
	return false;
}

static bool preflight_bus_relevant(struct device *dev)
{
	struct device_node *users[3], *cell;
	struct of_phandle_args args;
	bool relevant = false;
	int i, j;

	if (!dev->of_node)
		return false;
	users[0] = of_get_cpu_node(0, NULL);
	users[1] = of_get_cpu_node(4, NULL);
	users[2] = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-cci");
	for (i = 0; i < ARRAY_SIZE(users); i++) {
		relevant |= preflight_node_contains(users[i], dev->of_node);
		for (j = 0; !of_parse_phandle_with_args(users[i], "clocks",
						      "#clock-cells", j, &args); j++) {
			relevant |= preflight_node_contains(args.np, dev->of_node);
			of_node_put(args.np);
		}
		for (j = 0; (cell = of_parse_phandle(users[i], "nvmem-cells", j)); j++) {
			relevant |= preflight_node_contains(cell, dev->of_node);
			of_node_put(cell);
		}
		of_node_put(users[i]);
	}
	return relevant;
}

/* Supplier arrival/removal wakes even without a cpufreq/devfreq driver.
 * No framework calls on driver-core or NVMEM notification stacks.
 */
static int preflight_nvmem_event(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	/* NVMEM payload is opaque. Re-read the mandatory cells on this hint;
	 * retain every table/configuration, including on transient read errors.
	 */
	mutex_lock(&owner_lock);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return NOTIFY_OK;
}

static int preflight_supplier_event(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	if (!preflight_bus_relevant(data))
		return NOTIFY_DONE;
	mutex_lock(&owner_lock);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return NOTIFY_OK;
}
static struct notifier_block supplier_nb = {
	.notifier_call = preflight_supplier_event,
};
static struct notifier_block cpu_nb = {
	.notifier_call = preflight_supplier_event,
};
static struct notifier_block nvmem_nb = {
	.notifier_call = preflight_nvmem_event,
};

int mt6797_boot_barrier_supply(struct device_node *provider, const char *name)
{
	int id = !strcmp(name, "cpu-supply") ? MT6797_PROC : MT6797_SRAM;
	int ret;

	if (!of_machine_is_compatible("planet,gemini-pda"))
		return 0;
	mutex_lock(&owner_lock);
	ret = topology();
	if (!ret)
		ret = provider == rails[id] ? 1 : -EINVAL;
	mutex_unlock(&owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_boot_barrier_supply);

struct attachment {
	enum mt6797_rail_id id;
};

static void detach(void *data)
{
	struct attachment *a = data;

	mutex_lock(&owner_lock);
	/* Engine detach drains raw calls before provider devres can be freed. */
	mt6797_rail_detach(&owner, a->id);
	attached &= ~BIT(a->id);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	schedule_work(&notify_job);
}

/* An immutable endpoint cannot accidentally expose the engine's converter. */
static int blocked_write(void *data, int uv)
{
	return -EBUSY;
}

int mt6797_provider_attach(struct device *dev, enum mt6797_rail_id id,
			   const struct mt6797_rail_io *io)
{
	struct attachment *a;
	struct mt6797_rail_io guarded;
	int ret;

	if ((unsigned int)id >= MT6797_RAILS || !io || !io->read)
		return -EINVAL;
	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->id = id;
	guarded = *io;
	guarded.write = blocked_write;
	mutex_lock(&owner_lock);
	ret = io->read(io->data);
	if (ret >= 0)
		ret = mt6797_rail_attach(&owner, id, &guarded);
	if (!ret) {
		attached |= BIT(id);
		preflight_invalidate();
	}
	mutex_unlock(&owner_lock);
	if (ret) {
		devm_kfree(dev, a);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, detach, a);
	if (!ret)
		schedule_work(&notify_job);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_attach);

/* Lock order: owner -> engine -> provider. Provider callbacks never enter
 * the owner/engine, and raw operations never call regulator or OPP APIs.
 */
int mt6797_provider_sample(u64 generation, int uv[MT6797_RAILS])
{
	int ret = 0, i;

	mutex_lock(&owner_lock);
	mutex_lock(&owner.lock);
	if (generation != owner.generation) {
		ret = -ESTALE;
		goto out;
	}
	if (attached != GENMASK(1, 0)) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	for (i = 0; i < MT6797_RAILS; i++) {
		uv[i] = owner.rail[i].read(owner.rail[i].data);
		if (uv[i] < 0) {
			ret = uv[i];
			break;
		}
	}
out:
	mutex_unlock(&owner.lock);
	mutex_unlock(&owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_sample);

void mt6797_provider_status(struct mt6797_provider_status *status)
{
	mutex_lock(&owner_lock);
	mt6797_rail_status(&owner, &status->engine);
	status->attached = attached;
	status->preflight = preflight;
	status->readiness = attached == GENMASK(1, 0) ? -EAGAIN : -EPROBE_DEFER;
	mutex_unlock(&owner_lock);
}
EXPORT_SYMBOL_GPL(mt6797_provider_status);

/* A single delivery worker serializes coalesced snapshots. Callbacks run
 * without owner or supplier locks, and may query status or queue work. They
 * must not synchronously unregister themselves from this blocking chain.
 * New invalidations during delivery queue another run (no lost wakeup).
 */
static void preflight_notify(struct work_struct *work)
{
	struct mt6797_provider_status status;

	mt6797_provider_status(&status);
	blocking_notifier_call_chain(&events, 0, &status);
}

int mt6797_provider_subscribe(struct notifier_block *nb)
{
	int ret = blocking_notifier_chain_register(&events, nb);

	/* Register before replay: concurrent events can duplicate, never be lost.
	 * Callbacks may snapshot; they must not register/unregister subscribers.
	 */
	if (!ret)
		schedule_work(&notify_job);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_subscribe);

int mt6797_provider_unsubscribe(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&events, nb);
}
EXPORT_SYMBOL_GPL(mt6797_provider_unsubscribe);

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
/* Focused framework fixture: replaces ONLY discovery, not measurement,
 * notification, generation checking or publication. Never enabled on Gemini.
 * The caller owns dynamic OPPs and clock references until stop returns.
 */
int mt6797_preflight_test_start(struct device *dev, struct clk *clk)
{
	int i, j, ret;

	if (of_machine_is_compatible("planet,gemini-pda"))
		return -EBUSY;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct preflight_user *u = &pf_users[i];

		u->dev = dev;
		u->opp_nb.notifier_call = preflight_opp_event;
		ret = dev_pm_opp_register_notifier(dev, &u->opp_nb);
		if (ret)
			goto fail;
		u->opp_notify = true;
		for (j = 0; j < 2; j++) {
			u->clk[j] = clk;
			u->clk_nb[j].notifier_call = preflight_clock_event;
			ret = clk_notifier_register(clk, &u->clk_nb[j]);
			if (ret)
				goto fail;
			u->clk_notify[j] = true;
		}
	}
	mutex_lock(&owner_lock);
	fixture_active = true;
	preflight_active = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return 0;
fail:
	mt6797_preflight_test_stop();
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_start);

void mt6797_preflight_test_flush(void)
{
	flush_work(&preflight_job);
	flush_work(&notify_job);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_flush);

void mt6797_preflight_test_hint(struct device *dev)
{
	preflight_supplier_event(NULL, BUS_NOTIFY_ADD_DEVICE, dev);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_hint);

void mt6797_preflight_test_stop(void)
{
	int i, j;

	mutex_lock(&owner_lock);
	preflight_active = false;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	cancel_work_sync(&preflight_job);
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct preflight_user *u = &pf_users[i];

		for (j = 0; j < 2; j++)
			if (u->clk_notify[j])
				clk_notifier_unregister(u->clk[j], &u->clk_nb[j]);
		if (u->opp_notify)
			dev_pm_opp_unregister_notifier(u->dev, &u->opp_nb);
		memset(u, 0, sizeof(*u));
	}
	fixture_active = false;
	flush_work(&notify_job);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_stop);
#endif

static int __init mt6797_provider_init(void)
{
	int ret;

	if (!of_machine_is_compatible("planet,gemini-pda"))
		return 0;
	ret = bus_register_notifier(&platform_bus_type, &supplier_nb);
	if (ret)
		return ret;
	ret = bus_register_notifier(&cpu_subsys, &cpu_nb);
	if (ret) {
		bus_unregister_notifier(&platform_bus_type, &supplier_nb);
		return ret;
	}
	ret = nvmem_register_notifier(&nvmem_nb);
	if (ret) {
		bus_unregister_notifier(&cpu_subsys, &cpu_nb);
		bus_unregister_notifier(&platform_bus_type, &supplier_nb);
		return ret;
	}
	mutex_lock(&owner_lock);
	preflight_active = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return 0;
}
module_init(mt6797_provider_init);

static void __exit mt6797_provider_exit(void)
{
	if (of_machine_is_compatible("planet,gemini-pda")) {
		bus_unregister_notifier(&platform_bus_type, &supplier_nb);
		bus_unregister_notifier(&cpu_subsys, &cpu_nb);
		nvmem_unregister_notifier(&nvmem_nb);
	}
	mutex_lock(&owner_lock);
	preflight_active = false;
	stopping = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	cancel_work_sync(&preflight_job);
	preflight_release();
	cancel_work_sync(&notify_job);
	of_node_put(rails[0]);
	of_node_put(rails[1]);
}
module_exit(mt6797_provider_exit);
MODULE_LICENSE("GPL");
