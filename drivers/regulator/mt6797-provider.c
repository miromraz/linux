// SPDX-License-Identifier: GPL-2.0-only
/* Immutable production ownership. No conversion/admission entry point. */
#include <linux/module.h>
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
	mutex_unlock(&owner_lock);
	blocking_notifier_call_chain(&events, 0, NULL);
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
	if (!ret)
		attached |= BIT(id);
	mutex_unlock(&owner_lock);
	if (ret) {
		devm_kfree(dev, a);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, detach, a);
	if (!ret)
		blocking_notifier_call_chain(&events, 0, NULL);
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
	status->readiness = attached == GENMASK(1, 0) ? -EAGAIN : -EPROBE_DEFER;
	mutex_unlock(&owner_lock);
}
EXPORT_SYMBOL_GPL(mt6797_provider_status);

int mt6797_provider_subscribe(struct notifier_block *nb)
{
	int ret = blocking_notifier_chain_register(&events, nb);

	/* Register before replay: concurrent events can duplicate, never be lost.
	 * Callbacks may snapshot; they must not register/unregister subscribers.
	 */
	if (!ret)
		blocking_notifier_call_chain(&events, 0, NULL);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_provider_subscribe);

int mt6797_provider_unsubscribe(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&events, nb);
}
EXPORT_SYMBOL_GPL(mt6797_provider_unsubscribe);

static void __exit mt6797_provider_exit(void)
{
	of_node_put(rails[0]);
	of_node_put(rails[1]);
}
module_exit(mt6797_provider_exit);
MODULE_LICENSE("GPL");
