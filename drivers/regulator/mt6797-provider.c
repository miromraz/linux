// SPDX-License-Identifier: GPL-2.0-only
/* Production conversion ownership; consumer admission remains blocked. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/regulator/mt6797.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include "mt6797-provider.h"
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
#include "internal.h"
#endif

static DEFINE_MUTEX(owner_lock);
/* Serializes reference acquisition/replacement, never taken by notifiers. */
static DEFINE_MUTEX(transaction_lock);
/* transaction_lock owns the token, owner_lock protects cancellation/scope. */
struct mt6797_consumer_txn {
	struct task_struct *task;
	unsigned int user, claims;
	u64 generation;
	struct clk *clocks[7], *pll;
	struct dev_pm_opp_requirements *requirements[MT6797_RAIL_USERS];
	struct dev_pm_opp_requirement selected[MT6797_RAIL_USERS][3];
	int floor[MT6797_RAIL_USERS], retained_uv;
	unsigned long target, rate, scope_old, scope_new;
	bool cancelled, scope, expected[MT6797_RAIL_USERS][2];
	bool prepared, use, failed;
	enum mt6797_txn_path path;
};
static struct mt6797_consumer_txn *active_txn;

static struct device *consumer_owner[MT6797_RAIL_USERS];
static struct mt6797_rail_coordinator owner = {
	.lock = __MUTEX_INITIALIZER(owner.lock),
};
static struct device_node *rails[MT6797_RAILS];
static unsigned long attached;
/* owner_lock admits uses and cancels them before detach waits without locks. */
static unsigned long detaching;
static unsigned int provider_uses;
static DECLARE_WAIT_QUEUE_HEAD(provider_idle);

static int provider_use_get(u64 generation)
{
	int ret = 0;

	mutex_lock(&owner_lock);
	if (attached != GENMASK(1, 0))
		ret = -EPROBE_DEFER;
	else if (detaching || generation != owner.generation)
		ret = -ESTALE;
	else
		provider_uses++;
	mutex_unlock(&owner_lock);
	return ret;
}

static void provider_use_put(void)
{
	mutex_lock(&owner_lock);
	if (!--provider_uses)
		wake_up_all(&provider_idle);
	mutex_unlock(&owner_lock);
}

static BLOCKING_NOTIFIER_HEAD(events);
static void preflight_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(preflight_job, preflight_work);
static bool preflight_active;
/* Supplier teardown is irreversible until ownership is explicitly rebuilt. */
static bool supplier_revoked;
static void preflight_notify(struct work_struct *work);
static DECLARE_WORK(notify_job, preflight_notify);
static bool stopping;
static unsigned int sources;
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
static bool fixture_active;
static struct completion *drain_observer;
#endif
static struct mt6797_preflight_result preflight = { .error = -EPROBE_DEFER };

/* owner_lock serializes invalidation with publication and status snapshots. */
static void preflight_hint(void)
{
	preflight.generation++;
	preflight.error = -EAGAIN;
	preflight.provider_generation = owner.generation;
	if (!stopping)
		schedule_work(&notify_job);
	if (preflight_active)
		mod_delayed_work(system_wq, &preflight_job, 0);
}

static void preflight_invalidate(void)
{
	if (active_txn)
		active_txn->cancelled = true;
	preflight_hint();
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
	struct clk *clk[2], *pll;
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
	struct mt6797_consumer_txn *t;
	struct clk_notifier_data *change = data;
	bool expected = false;
	int i, j;

	mutex_lock(&owner_lock);
	t = active_txn;
	if (t && t->scope && t->task == current &&
	    (event == PRE_RATE_CHANGE || event == POST_RATE_CHANGE ||
	     event == ABORT_RATE_CHANGE) &&
	    change->old_rate == t->scope_old && change->new_rate == t->scope_new)
		for (i = 0; i < MT6797_RAIL_USERS; i++)
			for (j = 0; j < 2; j++)
				if (nb == &pf_users[i].clk_nb[j] && t->expected[i][j])
					expected = true;
	if (expected)
		preflight_hint();
	else
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
		clk_put(u->pll);
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
		if (!u->pll) {
			struct clk *parent = clk_get_parent(u->clk[0]);

			if (parent) {
				u->pll = clk_hw_get_clk(__clk_get_hw(parent), NULL);
				if (IS_ERR(u->pll)) {
					ret = PTR_ERR(u->pll);
					u->pll = NULL;
					return ret;
				}
			}
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

/* Private logical supplies share one physical writer. Regulator core owns
 * the guard and consumer requests; the engine arbitrates the three domains.
 * No devres object or consumer driver callback is retained here.
 */
struct protected_supply {
	struct regulator_desc desc;
	struct regulator_dev *rdev;
	struct regulator *guard, *vote;
	int guard_uv, vote_uv;
	unsigned int users;
};
static struct protected_supply protected[MT6797_RAIL_USERS];
/* Published under owner_lock; never take transaction_lock in status/notifiers. */
static unsigned long active_leases, retained_leases;
static int ownership_error[MT6797_RAIL_USERS];
static u64 ownership_generation[MT6797_RAIL_USERS];

static void ownership_publish(int user, int error)
{
	int i;

	mutex_lock(&owner_lock);
	active_leases = 0;
	retained_leases = 0;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		if (protected[i].users)
			active_leases |= BIT(i);
		else if (protected[i].vote)
			retained_leases |= BIT(i);
		if (user < 0 || user == i) {
			ownership_error[i] = error;
			ownership_generation[i] = preflight.generation;
		}
	}
	mutex_unlock(&owner_lock);
}

static struct device *lease_device;
static struct regulator_dev *sram_rdev;
static struct regulator *sram_guard;

static int lease_sram_read(struct regulator_dev *rdev)
{
	int uv[MT6797_RAILS], ret;
	u64 generation;

	mutex_lock(&owner_lock);
	generation = owner.generation;
	mutex_unlock(&owner_lock);
	ret = mt6797_provider_sample(generation, uv);
	return ret ?: uv[MT6797_SRAM];
}

static const struct regulator_ops lease_sram_ops = {
	.get_voltage = lease_sram_read,
};
static const struct regulator_desc lease_sram_desc = {
	.name = "mt6797-sram-protection", .type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE, .ops = &lease_sram_ops,
	.n_voltages = 1, .fixed_uV = 1200000,
};
static u64 next_cookie;
struct consumer_lease {
	struct list_head node;
	u64 cookie;
	unsigned int user;
};
static LIST_HEAD(consumer_leases);
static struct task_struct *lease_task;
/* Valid under transaction_lock; each setter reserves the framework afresh. */
static struct mt6797_preflight_result *lease_snapshot;
static bool lease_setting;
static int preflight_transaction(struct preflight_user *users,
				 struct mt6797_preflight_result *r,
				 int user, struct device *consumer,
				 int request, bool returning);

static int txn_write(struct mt6797_consumer_txn *t, int request);

static int lease_write(struct regulator_dev *rdev, int min_uv, int max_uv,
		       unsigned int *selector)
{
	struct protected_supply *s = rdev_get_drvdata(rdev);
	int ret;

	if (active_txn && current == active_txn->task)
		return s - protected == active_txn->user ?
			txn_write(active_txn, min_uv) : -ESTALE;
	if (current != lease_task || !lease_snapshot || lease_setting)
		return -ESTALE;
	/* Regulator notifications run without clock/table reservations. Take a
	 * fresh snapshot inside the setter, after PRE notification, and release
	 * it before the core delivers POST or ABORT notification.
	 */
	lease_setting = true;
	ret = preflight_transaction(pf_users, lease_snapshot, s - protected,
				    NULL, max(min_uv, 1040000), false);
	lease_setting = false;
	return ret;
}

static int lease_read(struct regulator_dev *rdev)
{
	int uv[MT6797_RAILS], ret;
	u64 generation;

	mutex_lock(&owner_lock);
	generation = owner.generation;
	mutex_unlock(&owner_lock);
	ret = mt6797_provider_sample(generation, uv);
	return ret ?: uv[MT6797_PROC];
}

static const struct regulator_ops lease_ops = {
	.set_voltage = lease_write,
	.get_voltage = lease_read,
};

static int lease_supplies(void)
{
	static const char * const names[] = {
		"mt6797-cpu0-protection", "mt6797-cpu4-protection",
		"mt6797-cci-protection",
	};
	struct regulator_init_data init = {
		.constraints = {
			.min_uV = 1040000, .max_uV = 1200000,
			.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		},
	};
	int i, ret, uv[MT6797_RAILS];

	if (!lease_device) {
		lease_device = root_device_register("mt6797-protection");
		if (IS_ERR(lease_device)) {
			ret = PTR_ERR(lease_device);
			lease_device = NULL;
			return ret;
		}
		/* Live clocks outlast consumer unbind. Their guards must also
		 * outlast module teardown, including retained-error paths.
		 */
		__module_get(THIS_MODULE);
	}
	ret = mt6797_provider_sample(lease_snapshot->provider_generation, uv);
	if (ret)
		return ret;
	if (!sram_rdev) {
		struct regulator_init_data fixed = {
			.constraints = { .min_uV = 1200000, .max_uV = 1200000 },
		};
		struct regulator_config config = {
			.dev = lease_device, .init_data = &fixed,
		};

		sram_rdev = regulator_register(lease_device, &lease_sram_desc, &config);
		if (IS_ERR(sram_rdev)) {
			ret = PTR_ERR(sram_rdev);
			sram_rdev = NULL;
			return ret;
		}
	}
	if (!sram_guard) {
		sram_guard = regulator_get(NULL, lease_sram_desc.name);
		if (IS_ERR(sram_guard)) {
			ret = PTR_ERR(sram_guard);
			sram_guard = NULL;
			return ret;
		}
	}
	ret = regulator_set_voltage(sram_guard, 1200000, 1200000);
	if (ret)
		return ret;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct protected_supply *s = &protected[i];
		struct regulator_config config = {
			.dev = lease_device, .init_data = &init, .driver_data = s,
		};

		if (!s->rdev) {
			s->desc.name = names[i];
			s->desc.type = REGULATOR_VOLTAGE;
			s->desc.owner = THIS_MODULE;
			s->desc.ops = &lease_ops;
			s->rdev = regulator_register(lease_device, &s->desc, &config);
			if (IS_ERR(s->rdev)) {
				ret = PTR_ERR(s->rdev);
				s->rdev = NULL;
				return ret;
			}
		}
		if (!s->guard) {
			s->guard = regulator_get(NULL, names[i]);
			if (IS_ERR(s->guard)) {
				ret = PTR_ERR(s->guard);
				s->guard = NULL;
				return ret;
			}
		}
		if (!s->guard_uv && !s->vote) {
			/* Install every boot request without lowering boot voltage. */
			ret = regulator_set_voltage(s->guard, uv[0], 1200000);
			if (ret)
				return ret;
			s->guard_uv = uv[0];
		}
	}
	return 0;
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include "mt6797-combined-test.h"

static void (*combined_transfer_hook)(void *, unsigned int, bool, int);
static void *combined_transfer_data;

void mt6797_test_transfer_hook(void (*hook)(void *, unsigned int, bool, int), void *data)
{
	mutex_lock(&transaction_lock);
	combined_transfer_hook = hook;
	combined_transfer_data = data;
	mutex_unlock(&transaction_lock);
}
#endif

static int lease_transfer(unsigned int user, int request, bool returning)
{
	struct protected_supply *s = &protected[user];
	struct regulator *replacement;
	int ret;

	if (returning) {
		ret = regulator_set_voltage(s->guard, max(request, s->vote_uv), 1200000);
		if (ret)
			return ret;
		s->guard_uv = max(request, s->vote_uv);
		/* The consumer vote still exists while replacement is verified. */
	} else {
		if (s->vote)
			return -EBUSY;
		replacement = regulator_get(NULL, s->desc.name);
		if (IS_ERR(replacement))
			return PTR_ERR(replacement);
		ret = regulator_set_voltage(replacement, request, 1200000);
		if (ret) {
			regulator_put(replacement);
			return ret;
		}
		s->vote = replacement;
		s->vote_uv = request;
	}
	/* An unchanged core request may skip the setter. Revalidate after all
	 * notifier delivery before discarding any existing ownership.
	 */
	ret = lease_write(s->rdev, max(s->guard_uv, s->vote_uv), 1200000, NULL);
	if (ret)
		return ret;
	if (returning) {
		regulator_put(s->vote);
		s->vote = NULL;
		s->vote_uv = 0;
	} else {
		ret = regulator_set_voltage(s->guard, 0, 1200000);
		if (ret)
			return ret;
		s->guard_uv = 0;
		return lease_write(s->rdev, s->vote_uv, 1200000, NULL);
	}
	return 0;
}

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

/* Framework reservation precedes owner/engine/provider locks. Table refs are
 * obtained before locking any table; no framework calls occur in conversion.
 * The normal CPU/CCI drivers remain gated, including after success.
 */
static int preflight_transaction(struct preflight_user *users,
				 struct mt6797_preflight_result *r,
				 int user, struct device *consumer,
				 int request, bool returning)
{
	struct opp_table *tables[MT6797_RAIL_USERS] = {};
	int floor[MT6797_RAIL_USERS];
	int i, j, ret = 0, clocks = 0, locked = 0;

	/* Includes framework notification and gaps between individual raw calls.
	 * Teardown from this same notification stack is unsupported; queue it.
	 */
	ret = provider_use_get(r->provider_generation);
	if (ret)
		return ret;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		tables[i] = dev_pm_opp_get_opp_table(users[i].dev);
		if (IS_ERR(tables[i])) {
			ret = PTR_ERR(tables[i]);
			tables[i] = NULL;
			goto out;
		}
		for (j = 0; j < i; j++)
			if (tables[j] == tables[i]) {
				ret = -EINVAL;
				goto out;
			}
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		for (j = 0; j < 2; j++) {
			ret = clk_rate_exclusive_get(users[i].clk[j]);
			if (ret)
				goto out;
			clocks++;
			r->rate[i][j] = clk_get_rate(users[i].clk[j]);
		}
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		ret = dev_pm_opp_snapshot_lock(tables[i], r->rate[i],
					       r->ceiling[i], r->uv[i], 2);
		if (ret)
			goto out;
		locked++;
		floor[i] = max(r->uv[i][0], r->uv[i][1]);
	}
	if (user >= 0 && users == pf_users && !lease_setting) {
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
		if (combined_transfer_hook)
			combined_transfer_hook(combined_transfer_data, user, returning, 0);
#endif
		while (locked)
			dev_pm_opp_snapshot_unlock(tables[--locked]);
		while (clocks) {
			clocks--;
			clk_rate_exclusive_put(users[clocks / 2].clk[clocks % 2]);
		}
		lease_task = current;
		lease_snapshot = r;
		ret = lease_supplies();
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
		if (!ret && combined_transfer_hook)
			combined_transfer_hook(combined_transfer_data, user, returning, 1);
#endif
		if (!ret)
			ret = lease_transfer(user, returning ? max(floor[user], 1040000) :
					     request, returning);
		lease_task = NULL;
		lease_snapshot = NULL;
		goto out;
	}
	mutex_lock(&owner_lock);
	if (!preflight_active || detaching || (supplier_revoked && user != -2) ||
	    r->generation != preflight.generation ||
	    r->provider_generation != owner.generation)
		ret = -ESTALE;
	else if (user == -2) {
		for (i = 0; i < MT6797_RAIL_USERS; i++)
			floor[i] = max(floor[i], max(protected[i].guard_uv,
						   protected[i].vote_uv));
		ret = mt6797_rail_recover(&owner, r->provider_generation, floor);
		if (!ret)
			supplier_revoked = false;
	} else if (user < 0)
		ret = mt6797_rail_prepare(&owner, r->provider_generation, floor);
	else if (users == pf_users && lease_setting)
		ret = mt6797_rail_replace(&owner, r->provider_generation, user,
					  request, floor);
	else if (consumer_owner[user] && consumer_owner[user] != consumer)
		ret = -EBUSY;
	else if (returning && !consumer_owner[user])
		ret = -ENOENT;
	else {
		ret = mt6797_rail_replace(&owner, r->provider_generation, user,
			returning ? max(floor[user], 1040000) : request, floor);
		if (!ret) {
			if (returning)
				consumer_owner[user] = NULL;
			else
				consumer_owner[user] = consumer;
		}
	}
	mutex_unlock(&owner_lock);
out:
	while (locked)
		dev_pm_opp_snapshot_unlock(tables[--locked]);
	while (clocks) {
		clocks--;
		clk_rate_exclusive_put(users[clocks / 2].clk[clocks % 2]);
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		if (tables[i])
			dev_pm_opp_put_opp_table(tables[i]);
	provider_use_put();
	return ret;
}

static int preflight_convert(struct preflight_user *users,
			     struct mt6797_preflight_result *r)
{
	return preflight_transaction(users, r, -1, NULL, 0, false);
}

/* Cookies distinguish independent local lifetimes, including same-device ABA.
 * The registry owns votes even when the last local release cannot transfer.
 */
int mt6797_consumer_acquire(struct device *dev, unsigned int user, u64 *cookie)
{
	struct mt6797_preflight_result result;
	struct protected_supply *s;
	struct consumer_lease *lease;
	int request, ret;

	if (!dev || !cookie || user >= MT6797_RAIL_USERS)
		return -EINVAL;
	*cookie = 0;
	lease = kzalloc_obj(*lease);
	if (!lease)
		return -ENOMEM;
	mutex_lock(&owner_lock);
	if (active_txn && active_txn->task == current) {
		mutex_unlock(&owner_lock);
		kfree(lease);
		return -EBUSY;
	}
	mutex_unlock(&owner_lock);
	mutex_lock(&transaction_lock);
	s = &protected[user];
	mutex_lock(&owner_lock);
	result = preflight;
	ret = !preflight_active || supplier_revoked ? -EPROBE_DEFER : result.error;
	mutex_unlock(&owner_lock);
	if (ret)
		goto out;
	if (pf_users[user].dev != dev) {
		ret = -EINVAL;
		goto out;
	}
	if (s->users)
		goto acquired;
	if (s->vote) {
		ret = -EBUSY;
		goto out;
	}
	request = max_t(unsigned long, 1040000,
			max(result.uv[user][0], result.uv[user][1]));
	ret = preflight_transaction(pf_users, &result, user, dev, request, false);
	if (s->vote) {
		mutex_lock(&owner_lock);
		consumer_owner[user] = get_device(dev);
		mutex_unlock(&owner_lock);
	}
	if (ret)
		goto out;
acquired:
	lease->cookie = ++next_cookie;
	lease->user = user;
	list_add(&lease->node, &consumer_leases);
	s->users++;
	*cookie = lease->cookie;
	lease = NULL;
out:
	ownership_publish(user, ret);
	mutex_unlock(&transaction_lock);
	kfree(lease);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_acquire);

int mt6797_consumer_release(unsigned int user, u64 cookie)
{
	struct mt6797_preflight_result result;
	struct protected_supply *s;
	struct device *dev;
	struct consumer_lease *lease, *found = NULL;
	int ret = 0;

	if (user >= MT6797_RAIL_USERS || !cookie)
		return -EINVAL;
	mutex_lock(&owner_lock);
	if (active_txn && active_txn->task == current) {
		mutex_unlock(&owner_lock);
		return -EBUSY;
	}
	mutex_unlock(&owner_lock);
	mutex_lock(&transaction_lock);
	s = &protected[user];
	list_for_each_entry(lease, &consumer_leases, node)
		if (lease->user == user && lease->cookie == cookie) {
			found = lease;
			break;
		}
	if (!found) {
		ret = -ESTALE;
		goto out;
	}
	list_del(&found->node);
	kfree(found);
	if (--s->users)
		goto out;
	mutex_lock(&owner_lock);
	result = preflight;
	dev = consumer_owner[user];
	mutex_unlock(&owner_lock);
	ret = preflight_transaction(pf_users, &result, user, dev, 0, true);
	/* A failed physical reconciliation may already have installed a guard.
	 * Keep identity and resources until the whole transaction succeeds.
	 */
	if (!ret) {
		mutex_lock(&owner_lock);
		consumer_owner[user] = NULL;
		mutex_unlock(&owner_lock);
		put_device(dev);
	}
out:
	ownership_publish(user, ret);
	mutex_unlock(&transaction_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_release);

/* owner_lock serializes cancellation with each raw authorization, including
 * final reduction. No framework notifier runs with this mutex held.
 */
static int txn_check(struct mt6797_consumer_txn *t)
{
	return active_txn != t || t->task != current || t->cancelled || t->failed ||
		detaching || supplier_revoked || t->generation != owner.generation ?
		-ESTALE : 0;
}

int mt6797_consumer_txn_validate(struct mt6797_consumer_txn *t)
{
	int ret;

	mutex_lock(&owner_lock);
	ret = txn_check(t);
	mutex_unlock(&owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_validate);

static int txn_write(struct mt6797_consumer_txn *t, int request)
{
	int ret;

	mutex_lock(&owner_lock);
	ret = txn_check(t);
	if (!ret)
		ret = mt6797_rail_replace(&owner, t->generation, t->user,
					  request, t->floor);
	if (ret)
		t->failed = true;
	mutex_unlock(&owner_lock);
	return ret;
}

static void txn_dispose(struct mt6797_consumer_txn *t)
{
	int i;

	/* Keep transaction_lock through notification-producing final puts.
	 * Notifiers only take owner_lock; recursive begin uses trylock.
	 */
	while (t->claims)
		clk_rate_exclusive_put(t->clocks[--t->claims]);
	if (t->pll)
		clk_put(t->pll);
	for (i = MT6797_RAIL_USERS - 1; i >= 0; i--)
		if (t->requirements[i])
			dev_pm_opp_requirements_put(t->requirements[i]);
	mutex_lock(&owner_lock);
	active_txn = NULL;
	mutex_unlock(&owner_lock);
	if (t->use)
		provider_use_put();
	kfree(t);
	mutex_unlock(&transaction_lock);
}

static int txn_claim(struct mt6797_consumer_txn *t, struct clk *clk)
{
	int i, ret;

	/* Deduplicate handles, not cores: distinct owner handles have independent
	 * CCF protection. pf_users owns these handles until transaction unlock.
	 */
	for (i = 0; i < t->claims; i++)
		if (t->clocks[i] == clk)
			return 0;
	ret = clk_rate_exclusive_get(clk);
	if (!ret)
		t->clocks[t->claims++] = clk;
	return ret;
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
static unsigned int txn_fail_stage;
static bool test_hold_preflight;

void mt6797_txn_test_fail_acquisition(unsigned int stage)
{
	txn_fail_stage = stage;
}
EXPORT_SYMBOL_GPL(mt6797_txn_test_fail_acquisition);
#endif

static int txn_acquired(void)
{
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
	if (txn_fail_stage && !--txn_fail_stage)
		return -EIO;
#endif
	return 0;
}

int mt6797_consumer_txn_begin(unsigned int user, u64 cookie,
		unsigned long target_hz, struct mt6797_consumer_txn **txn)
{
	struct mt6797_consumer_txn *t;
	struct consumer_lease *lease;
	struct opp_table *tables[MT6797_RAIL_USERS] = {};
	unsigned long rates[MT6797_RAIL_USERS][9];
	unsigned int count[MT6797_RAIL_USERS] = {};
	unsigned int table_id[MT6797_RAIL_USERS], offset[MT6797_RAIL_USERS];
	unsigned int nr = 0;
	struct mt6797_rail_status status;
	struct clk *parent;
	bool found = false;
	int i, j, ret, uv[MT6797_RAILS];

	if (!txn || user >= MT6797_RAIL_USERS || !cookie || !target_hz)
		return -EINVAL;
	*txn = NULL;
	if (!mutex_trylock(&transaction_lock))
		return -EBUSY;
	t = kzalloc_obj(*t);
	if (!t) {
		mutex_unlock(&transaction_lock);
		return -ENOMEM;
	}
	t->task = current;
	t->user = user;
	t->target = target_hz;
	t->path = MT6797_TXN_OLD;
	list_for_each_entry(lease, &consumer_leases, node)
		if (lease->user == user && lease->cookie == cookie)
			found = true;
	mutex_lock(&owner_lock);
	ret = !found || !protected[user].vote || !protected[user].users ||
		consumer_owner[user] != pf_users[user].dev ? -ESTALE : 0;
	if (!ret && (!preflight_active || supplier_revoked || preflight.error))
		ret = -EAGAIN;
	t->generation = owner.generation;
	active_txn = t;
	mutex_unlock(&owner_lock);
	if (ret)
		goto fail;
	ret = provider_use_get(t->generation);
	if (ret)
		goto fail;
	t->use = true;
	ret = txn_acquired();
	if (ret)
		goto fail;
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		for (j = 0; j < 2; j++) {
			ret = txn_claim(t, pf_users[i].clk[j]);
			if (!ret)
				ret = txn_acquired();
			if (ret)
				goto fail;
		}
	parent = clk_get_parent(pf_users[user].clk[0]);
	if (!parent || !pf_users[user].pll ||
	    !clk_is_match(parent, pf_users[user].pll) ||
	    clk_is_match(parent, pf_users[user].clk[1])) {
		ret = -EINVAL;
		goto fail;
	}
	t->pll = clk_hw_get_clk(__clk_get_hw(parent), NULL);
	if (IS_ERR(t->pll)) {
		ret = PTR_ERR(t->pll);
		t->pll = NULL;
		goto fail;
	}
	ret = txn_claim(t, t->pll);
	if (!ret)
		ret = txn_acquired();
	if (ret)
		goto fail;
	/* Collect and deduplicate shared tables before reserving any of them. */
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct opp_table *table = dev_pm_opp_get_opp_table(pf_users[i].dev);
		unsigned int id, at;

		if (IS_ERR(table)) {
			ret = PTR_ERR(table);
			goto fail;
		}
		for (id = 0; id < nr; id++)
			if (tables[id] == table)
				break;
		if (id == nr)
			tables[nr++] = table;
		else
			dev_pm_opp_put_opp_table(table);
		table_id[i] = id;
		at = count[id];
		offset[i] = at;
		rates[id][at] = clk_get_rate(pf_users[i].clk[0]);
		rates[id][at + 1] = clk_get_rate(pf_users[i].clk[1]);
		rates[id][at + 2] = i == user ? target_hz : rates[id][at];
		count[id] += 3;
		if (i == user)
			t->rate = rates[id][at];
	}
	for (i = 0; i < nr; i++) {
		ret = dev_pm_opp_requirements_get(tables[i], rates[i], count[i],
						&t->requirements[i]);
		dev_pm_opp_put_opp_table(tables[i]);
		tables[i] = NULL;
		if (!ret)
			ret = txn_acquired();
		if (ret)
			goto fail;
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		for (j = 0; j < 3; j++)
			dev_pm_opp_requirements_read(t->requirements[table_id[i]],
						offset[i] + j, &t->selected[i][j]);
		t->floor[i] = max(t->selected[i][0].uv, t->selected[i][1].uv);
	}
	ret = mt6797_consumer_txn_validate(t);
	if (ret)
		goto fail;
	ret = mt6797_provider_sample(t->generation, uv);
	if (ret)
		goto fail;
	mt6797_rail_status(&owner, &status);
	if (uv[MT6797_SRAM] != 1200000 || status.fault) {
		ret = -ERANGE;
		goto fail;
	}
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		if (uv[MT6797_PROC] < t->floor[i] || status.floor[i] < t->floor[i]) {
			ret = -ERANGE;
			goto fail;
		}
	if (protected[user].vote_uv < t->floor[user]) {
		ret = -ERANGE;
		goto fail;
	}
	t->retained_uv = protected[user].vote_uv;
	*txn = t;
	return 0;
fail:
	for (i = 0; i < nr; i++)
		if (tables[i])
			dev_pm_opp_put_opp_table(tables[i]);
	txn_dispose(t);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_begin);

static int txn_vote(struct mt6797_consumer_txn *t, int uv)
{
	struct protected_supply *s = &protected[t->user];
	int ret = mt6797_consumer_txn_validate(t);

	if (ret)
		return ret;
	ret = regulator_set_voltage(s->vote, uv, 1200000);
	if (!ret) {
		s->vote_uv = uv;
		t->retained_uv = uv;
		/* Core may skip an identical request; still validate the live rail. */
		ret = txn_write(t, uv);
	}
	return ret;
}

int mt6797_consumer_txn_prepare(struct mt6797_consumer_txn *t)
{
	int ret = txn_vote(t, max_t(int, t->retained_uv,
		max_t(int, t->floor[t->user], t->selected[t->user][2].uv)));

	if (!ret)
		t->prepared = true;
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_prepare);

/* Only the selected mux may emit events for these explicit calls. No blanket
 * task exemption: peer/intermediate events or mismatched rates cancel.
 */
static int txn_scope(struct mt6797_consumer_txn *t, unsigned long old,
		     unsigned long new)
{
	int i, j, ret;

	for (i = 0; i < MT6797_RAIL_USERS; i++)
		for (j = 0; j < 2; j++)
			t->expected[i][j] = clk_is_match(pf_users[i].clk[j],
						       pf_users[t->user].clk[0]);
	mutex_lock(&owner_lock);
	ret = txn_check(t);
	if (!ret) {
		t->scope_old = old;
		t->scope_new = new;
		t->scope = true;
	}
	mutex_unlock(&owner_lock);
	return ret;
}

static void txn_unscope(struct mt6797_consumer_txn *t)
{
	mutex_lock(&owner_lock);
	t->scope = false;
	mutex_unlock(&owner_lock);
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
#include "mt6797-combined-test.h"
static void (*switch_hook)(void *data, int phase);
static void *switch_data;

void mt6797_test_switch_hook(void (*hook)(void *, int), void *data)
{
	switch_hook = hook;
	switch_data = data;
}
#endif

int mt6797_consumer_txn_switch(struct mt6797_consumer_txn *t)
{
	struct clk *mux = pf_users[t->user].clk[0];
	struct clk *intermediate = pf_users[t->user].clk[1];
	unsigned long rate = clk_get_rate(intermediate), selected_rate;
	struct dev_pm_opp *opp;
	unsigned long selected_uv;
	bool attempted = false;
	long rounded;
	int ret;

	if (!t->prepared || t->path != MT6797_TXN_OLD)
		return -EINVAL;
	if (t->target == t->rate) {
		ret = mt6797_consumer_txn_validate(t);
		if (!ret)
			t->path = MT6797_TXN_TARGET;
		return ret;
	}
	ret = txn_scope(t, t->rate, rate);
	if (ret)
		return ret;
	ret = clk_set_parent_tracked(mux, intermediate, &attempted);
	txn_unscope(t);
	if (ret) {
		if (attempted)
			t->path = MT6797_TXN_UNKNOWN;
		return ret < 0 ? ret : -EBUSY;
	}
	t->path = MT6797_TXN_UNKNOWN;
	if (!clk_is_match(clk_get_parent(mux), intermediate) || clk_get_rate(mux) != rate)
		return -EIO;
	t->path = MT6797_TXN_INTERMEDIATE;
	t->rate = rate;
	ret = mt6797_consumer_txn_validate(t);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
	if (switch_hook)
		switch_hook(switch_data, 2);
#endif
	/* Active mux ancestry no longer protects the PLL. Round ONLY here. */
	rounded = clk_round_rate(t->pll, t->target);
	if (rounded <= 0 || rounded > t->selected[t->user][2].rate)
		return -ERANGE;
	/* A lower rate need not have a lower voltage in a non-monotonic table.
	 * The reservation excludes writers; reject a removed/uncovered OPP or
	 * a voltage above the final owned vote before programming the PLL.
	 */
	selected_rate = rounded;
	opp = dev_pm_opp_find_freq_ceil(pf_users[t->user].dev, &selected_rate);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	selected_uv = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	if (selected_uv > t->selected[t->user][2].uv)
		return -ERANGE;
	ret = txn_scope(t, clk_get_rate(t->pll), rounded);
	if (ret)
		return ret;
	ret = clk_set_rate(t->pll, rounded);
	txn_unscope(t);
	if (ret || clk_get_rate(t->pll) != rounded)
		return ret ?: -EIO;
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST)
	if (switch_hook)
		switch_hook(switch_data, 3);
#endif
	ret = txn_scope(t, rate, rounded);
	if (ret)
		return ret;
	ret = clk_set_parent_tracked(mux, t->pll, &attempted);
	txn_unscope(t);
	if (ret) {
		if (attempted)
			t->path = MT6797_TXN_UNKNOWN;
		return ret < 0 ? ret : -EBUSY;
	}
	t->path = MT6797_TXN_UNKNOWN;
	if (!clk_is_match(clk_get_parent(mux), t->pll) || clk_get_rate(mux) != rounded)
		return -EIO;
	t->path = MT6797_TXN_TARGET;
	t->rate = rounded;
	return mt6797_consumer_txn_validate(t);
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_switch);

void mt6797_consumer_txn_abort(struct mt6797_consumer_txn *t, int error,
		struct mt6797_txn_outcome *outcome)
{
	*outcome = (struct mt6797_txn_outcome) {
		.error = error, .retained_uv = t->retained_uv, .path = t->path,
		.rate = t->path == MT6797_TXN_UNKNOWN ? 0 : t->rate,
		.retry = !t->cancelled && !t->failed && t->path == MT6797_TXN_OLD,
	};
	if (t->path == MT6797_TXN_UNKNOWN) {
		/* Cached CCF topology cannot prove a failed mux write's outcome.
		 * Block fresh reservations and guard retirement until recovery.
		 */
		mutex_lock(&owner_lock);
		supplier_revoked = true;
		mutex_lock(&owner.lock);
		owner.fault = true;
		owner.requirements_valid = false;
		mutex_unlock(&owner.lock);
		preflight_invalidate();
		mutex_unlock(&owner_lock);
	}
	/* Never withdraw ownership or attempt voltage rollback on abort. */
	txn_dispose(t);
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_abort);

int mt6797_consumer_txn_finish(struct mt6797_consumer_txn *t,
		struct mt6797_txn_outcome *outcome)
{
	int ret = mt6797_consumer_txn_validate(t);

	if (!ret && t->path != MT6797_TXN_TARGET)
		ret = -EINVAL;
	if (!ret) {
		t->floor[t->user] = max(t->selected[t->user][1].uv,
				       t->selected[t->user][2].uv);
		ret = txn_vote(t, max(t->floor[t->user], 1040000));
	}
	mt6797_consumer_txn_abort(t, ret, outcome);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_txn_finish);

/* Explicit process-context retry after all local leases have been released.
 * No consumed cookie is revived. Device replacement is rejected by acquire;
 * existing OPP configuration/adjustments remain owned by the registry.
 */
int mt6797_consumer_recover(void)
{
	struct mt6797_preflight_result result;
	int i, ret = 0;

	mutex_lock(&owner_lock);
	if (active_txn && active_txn->task == current) {
		mutex_unlock(&owner_lock);
		return -EBUSY;
	}
	mutex_unlock(&owner_lock);
	mutex_lock(&transaction_lock);
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		if (protected[i].users) {
			ret = -EBUSY;
			goto out;
		}
	}
	mutex_lock(&owner_lock);
	result = preflight;
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
	if (!ret)
		ret = preflight_transaction(pf_users, &result, -2, NULL, 0, false);
	for (i = 0; !ret && i < MT6797_RAIL_USERS; i++) {
		struct device *dev;

		if (!protected[i].vote)
			continue;
		ret = preflight_transaction(pf_users, &result, i,
					   pf_users[i].dev, 0, true);
		if (ret)
			break;
		mutex_lock(&owner_lock);
		dev = consumer_owner[i];
		consumer_owner[i] = NULL;
		mutex_unlock(&owner_lock);
		put_device(dev);
	}
	mutex_lock(&owner_lock);
	if (result.generation == preflight.generation &&
	    result.provider_generation == owner.generation) {
		result.error = ret;
		preflight = result;
	}
	mutex_unlock(&owner_lock);
out:
	ownership_publish(-1, ret);
	mutex_unlock(&transaction_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consumer_recover);

static void preflight_work(struct work_struct *work)
{
	struct mt6797_preflight_result result = {};
	int ret;

	mutex_lock(&transaction_lock);
	mutex_lock(&owner_lock);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
	if (test_hold_preflight) {
		mutex_unlock(&owner_lock);
		mutex_unlock(&transaction_lock);
		return;
	}
#endif
	if (!preflight_active) {
		mutex_unlock(&owner_lock);
		mutex_unlock(&transaction_lock);
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
	if (!ret
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
	    && !fixture_active
#endif
	   )
		ret = preflight_convert(pf_users, &result);
	if (!ret)
		ret = mt6797_provider_sample(result.provider_generation, result.rail_uv);
	result.error = ret;
	mutex_lock(&owner_lock);
	if (result.generation == preflight.generation &&
	    result.provider_generation == owner.generation && preflight_active) {
		preflight = result;
		/* A table reader can contend without emitting an invalidation. */
		if (ret == -EAGAIN)
			mod_delayed_work(system_wq, &preflight_job, msecs_to_jiffies(20));
		schedule_work(&notify_job);
	}
	mutex_unlock(&owner_lock);
	mutex_unlock(&transaction_lock);
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
	if (event == NVMEM_REMOVE || event == NVMEM_CELL_REMOVE ||
	    event == NVMEM_LAYOUT_REMOVE)
		supplier_revoked = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	return NOTIFY_OK;
}

static int preflight_supplier_event(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct device *dev = data;

	if (event == BUS_NOTIFY_UNBIND_DRIVER && dev->of_node &&
	    of_device_is_compatible(dev->of_node, "mediatek,mt6797-cci"))
		return NOTIFY_DONE;
	if (!preflight_bus_relevant(data))
		return NOTIFY_DONE;
	mutex_lock(&owner_lock);
	if (event == BUS_NOTIFY_UNBIND_DRIVER || event == BUS_NOTIFY_DEL_DEVICE)
		supplier_revoked = true;
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
	/* Cancel admission first. Existing users still own callable endpoints. */
	detaching |= BIT(a->id);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
	if (drain_observer)
		complete(drain_observer);
#endif
	wait_event(provider_idle, !READ_ONCE(provider_uses));
	mutex_lock(&owner_lock);
	/* This action is registered after the raw resources it protects. */
	mt6797_rail_detach(&owner, a->id);
	attached &= ~BIT(a->id);
	preflight_invalidate();
	mutex_unlock(&owner_lock);
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
	/* Legacy read-only fixtures remain immutable. */
	if (!guarded.write)
		guarded.write = blocked_write;
	mutex_lock(&owner_lock);
	ret = io->read(io->data);
	if (ret >= 0)
		ret = mt6797_rail_attach(&owner, id, &guarded);
	if (!ret) {
		detaching &= ~BIT(id);
		attached |= BIT(id);
		preflight_invalidate();
	}
	mutex_unlock(&owner_lock);
	if (ret) {
		devm_kfree(dev, a);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, detach, a);
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
	int i;

	mutex_lock(&owner_lock);
	mt6797_rail_status(&owner, &status->engine);
	status->converted = status->engine.ready && !preflight.error;
	status->active = active_leases;
	status->retained = retained_leases;
	memcpy(status->ownership_error, ownership_error, sizeof(ownership_error));
	memcpy(status->ownership_generation, ownership_generation,
	       sizeof(ownership_generation));
	status->adopted = 0;
	for (i = 0; i < MT6797_RAIL_USERS; i++)
		if (consumer_owner[i])
			status->adopted |= BIT(i);
	status->admission = status->converted ? MT6797_ADMISSION_PHASE3_BLOCKED :
		MT6797_ADMISSION_PROTECTED;
	status->proc_min_uv = status->converted ? 1040000 : 0;
	status->proc_max_uv = status->converted ? 1200000 : 0;
	status->fixed_sram_uv = status->converted ? 1200000 : 0;
	/* Conversion is not operational admission. Group4 owns that handoff. */
	status->engine.ready = false;
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
	mutex_lock(&owner_lock);
	if (!ret && !stopping)
		schedule_work(&notify_job);
	mutex_unlock(&owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_subscribe);

int mt6797_provider_unsubscribe(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&events, nb);
}
EXPORT_SYMBOL_GPL(mt6797_provider_unsubscribe);

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
/* Pause after real lifetime admission, without an engine or registry lock. */
int mt6797_preflight_test_hold_provider(u64 generation, struct completion *entered,
		struct completion *resume)
{
	int ret = provider_use_get(generation);
	int uv[MT6797_RAILS];

	complete(entered);
	if (ret)
		return ret;
	wait_for_completion(resume);
	ret = mt6797_provider_sample(generation, uv);
	provider_use_put();
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_hold_provider);

void mt6797_preflight_test_drain_observer(struct completion *cancelled)
{
	drain_observer = cancelled;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_drain_observer);

/* Focused framework fixture: replaces ONLY discovery, not measurement,
 * notification, generation checking or publication. Never enabled on Gemini.
 * The caller owns dynamic OPPs and clock references until stop returns.
 */
int mt6797_preflight_test_convert(struct device **dev, struct clk *clk,
				  struct mt6797_preflight_result *result)
{
	struct preflight_user users[MT6797_RAIL_USERS] = {};
	int i;

	if (of_machine_is_compatible("planet,gemini-pda"))
		return -EPERM;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		users[i].dev = dev[i];
		users[i].clk[0] = clk;
		users[i].clk[1] = clk;
	}
	return preflight_convert(users, result);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_convert);

int mt6797_preflight_test_handover(struct device **dev, struct clk *clk,
		struct mt6797_preflight_result *result, unsigned int user,
		int request, bool returning)
{
	struct preflight_user users[MT6797_RAIL_USERS] = {};
	int i;

	if (of_machine_is_compatible("planet,gemini-pda"))
		return -EPERM;
	if (user >= MT6797_RAIL_USERS)
		return -EINVAL;
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		users[i].dev = dev[i];
		users[i].clk[0] = clk;
		users[i].clk[1] = clk;
	}
	return preflight_transaction(users, result, user, dev[user], request, returning);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_handover);

static bool preflight_test_observable(void)
{
	return fixture_active ||
		(IS_ENABLED(CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST) &&
		 of_machine_is_compatible("test,mt6797-combined"));
}

int mt6797_preflight_test_regulator_notifier(unsigned int user,
					    struct notifier_block *nb, bool add)
{
	int ret;

	if (user >= MT6797_RAIL_USERS || !preflight_test_observable())
		return -EINVAL;
	mutex_lock(&transaction_lock);
	if (!protected[user].guard)
		ret = -ENOENT;
	else if (add)
		ret = regulator_register_notifier(protected[user].guard, nb);
	else
		ret = regulator_unregister_notifier(protected[user].guard, nb);
	mutex_unlock(&transaction_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_regulator_notifier);

/* Observe regulator-core requests, not the coordinator's cached floors. */
int mt6797_preflight_test_requests(unsigned int user, int *guard, int *vote)
{
	struct protected_supply *s;
	int count;

	if (user >= MT6797_RAIL_USERS || !preflight_test_observable())
		return -EINVAL;
	mutex_lock(&transaction_lock);
	s = &protected[user];
	*guard = s->guard ? s->guard->voltage[PM_SUSPEND_ON].min_uV : 0;
	*vote = s->vote ? s->vote->voltage[PM_SUSPEND_ON].min_uV : 0;
	count = !!s->guard + !!s->vote;
	mutex_unlock(&transaction_lock);
	return count;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_requests);

int mt6797_preflight_test_start_clocks(struct device **dev, struct clk *clocks[3][2])
{
	int i, j, ret;

	if (of_machine_is_compatible("planet,gemini-pda"))
		return -EBUSY;
	/* A new isolated fixture has no live clocks or retained raw endpoints. */
	mutex_lock(&owner_lock);
	if (!attached && !preflight_active) {
		memset(owner.floor, 0, sizeof(owner.floor));
		owner.fault = false;
		supplier_revoked = false;
	}
	mutex_unlock(&owner_lock);
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct preflight_user *u = &pf_users[i];

		u->dev = dev[i];
		u->opp_nb.notifier_call = preflight_opp_event;
		ret = dev_pm_opp_register_notifier(dev[i], &u->opp_nb);
		if (ret)
			goto fail;
		u->opp_notify = true;
		for (j = 0; j < 2; j++) {
			u->clk[j] = clocks[i][j];
			u->clk_nb[j].notifier_call = preflight_clock_event;
			ret = clk_notifier_register(u->clk[j], &u->clk_nb[j]);
			if (ret)
				goto fail;
			u->clk_notify[j] = true;
		}
		if (!u->pll) {
			struct clk *parent = clk_get_parent(u->clk[0]);

			if (parent) {
				u->pll = clk_hw_get_clk(__clk_get_hw(parent), NULL);
				if (IS_ERR(u->pll)) {
					ret = PTR_ERR(u->pll);
					u->pll = NULL;
					goto fail;
				}
			}
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
EXPORT_SYMBOL_GPL(mt6797_preflight_test_start_clocks);

int mt6797_preflight_test_start_users(struct device **dev, struct clk *clk)
{
	struct clk *clocks[3][2] = { { clk, clk }, { clk, clk }, { clk, clk } };

	return mt6797_preflight_test_start_clocks(dev, clocks);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_start_users);

int mt6797_preflight_test_start(struct device *dev, struct clk *clk)
{
	struct device *users[] = { dev, dev, dev };

	return mt6797_preflight_test_start_users(users, clk);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_start);

void mt6797_preflight_test_flush(void)
{
	flush_delayed_work(&preflight_job);
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
	cancel_delayed_work_sync(&preflight_job);
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct preflight_user *u = &pf_users[i];

		for (j = 0; j < 2; j++)
			if (u->clk_notify[j])
				clk_notifier_unregister(u->clk[j], &u->clk_nb[j]);
		clk_put(u->pll);
		if (u->opp_notify)
			dev_pm_opp_unregister_notifier(u->dev, &u->opp_nb);
		memset(u, 0, sizeof(*u));
	}
	mutex_lock(&transaction_lock);
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct protected_supply *s = &protected[i];
		struct device *dev;

		if (!s->rdev)
			continue;
		regulator_put(s->vote);
		regulator_put(s->guard);
		regulator_unregister(s->rdev);
		mutex_lock(&owner_lock);
		dev = consumer_owner[i];
		consumer_owner[i] = NULL;
		mutex_unlock(&owner_lock);
		put_device(dev);
		memset(s, 0, sizeof(*s));
	}
	regulator_put(sram_guard);
	sram_guard = NULL;
	if (sram_rdev)
		regulator_unregister(sram_rdev);
	sram_rdev = NULL;
	if (lease_device) {
		root_device_unregister(lease_device);
		lease_device = NULL;
		module_put(THIS_MODULE);
	}
	while (!list_empty(&consumer_leases)) {
		struct consumer_lease *lease;

		lease = list_first_entry(&consumer_leases, struct consumer_lease, node);
		list_del(&lease->node);
		kfree(lease);
	}
	mutex_lock(&owner_lock);
	memset(consumer_owner, 0, sizeof(consumer_owner));
	mutex_unlock(&owner_lock);
	mutex_unlock(&transaction_lock);
	fixture_active = false;
	flush_work(&notify_job);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_stop);
#endif

/* Close scheduling before draining sources. Unregister waits for callbacks,
 * but only cancel_work_sync waits for work those callbacks already queued.
 * Failed module init must use exactly the same boundary as ordinary exit.
 */
static void preflight_shutdown(void)
{
	int i;

	mutex_lock(&owner_lock);
	preflight_active = false;
	stopping = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	if (sources & BIT(2))
		nvmem_unregister_notifier(&nvmem_nb);
	if (sources & BIT(1))
		bus_unregister_notifier(&cpu_subsys, &cpu_nb);
	if (sources & BIT(0))
		bus_unregister_notifier(&platform_bus_type, &supplier_nb);
	sources = 0;
	cancel_delayed_work_sync(&preflight_job);
	mutex_lock(&transaction_lock);
	preflight_release();
	/* Raw providers depend on this module: shutdown follows their detach. */
	for (i = 0; i < MT6797_RAIL_USERS; i++) {
		struct device *dev;

		mutex_lock(&owner_lock);
		dev = consumer_owner[i];
		consumer_owner[i] = NULL;
		mutex_unlock(&owner_lock);
		put_device(dev);
	}
	mutex_unlock(&transaction_lock);
	cancel_work_sync(&notify_job);
}

static int preflight_init_sources(int fail_after, void (*step)(void))
{
	int ret;

	mutex_lock(&owner_lock);
	stopping = false;
	mutex_unlock(&owner_lock);
	ret = bus_register_notifier(&platform_bus_type, &supplier_nb);
	if (ret)
		goto fail;
	sources |= BIT(0);
	if (step)
		step();
	if (fail_after == 1) {
		ret = -EFAULT;
		goto fail;
	}
	ret = bus_register_notifier(&cpu_subsys, &cpu_nb);
	if (ret)
		goto fail;
	sources |= BIT(1);
	if (step)
		step();
	if (fail_after == 2) {
		ret = -EFAULT;
		goto fail;
	}
	ret = nvmem_register_notifier(&nvmem_nb);
	if (ret)
		goto fail;
	sources |= BIT(2);
	if (step)
		step();
	if (fail_after == 3) {
		ret = -EFAULT;
		goto fail;
	}
	mutex_lock(&owner_lock);
	preflight_active = true;
	preflight_invalidate();
	mutex_unlock(&owner_lock);
	if (fail_after == 4) {
		ret = -EFAULT;
		goto fail;
	}
	return 0;
fail:
	preflight_shutdown();
	return ret;
}

#if IS_ENABLED(CONFIG_REGULATOR_MT6797_PROVIDER_KUNIT_TEST)
int mt6797_preflight_test_init_failure(int fail_after, void (*step)(void))
{
	int ret;

	if (of_machine_is_compatible("planet,gemini-pda") ||
	    fail_after < 1 || fail_after > 4)
		return -EINVAL;
	ret = preflight_init_sources(fail_after, step);
	/* Non-Gemini fixtures may continue using the inactive registry. */
	mutex_lock(&owner_lock);
	stopping = false;
	mutex_unlock(&owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_init_failure);

void mt6797_preflight_test_hold(bool hold)
{
	if (hold)
		cancel_delayed_work_sync(&preflight_job);
	mutex_lock(&owner_lock);
	test_hold_preflight = hold;
	mutex_unlock(&owner_lock);
	if (!hold)
		schedule_delayed_work(&preflight_job, 0);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_hold);

void mt6797_preflight_test_event(void)
{
	preflight_nvmem_event(NULL, 0, NULL);
}
EXPORT_SYMBOL_GPL(mt6797_preflight_test_event);
#endif

static int __init mt6797_provider_init(void)
{
	if (!of_machine_is_compatible("planet,gemini-pda"))
		return 0;
	return preflight_init_sources(0, NULL);
}
module_init(mt6797_provider_init);

static void __exit mt6797_provider_exit(void)
{
	preflight_shutdown();
	of_node_put(rails[0]);
	of_node_put(rails[1]);
}
module_exit(mt6797_provider_exit);
MODULE_LICENSE("GPL");
