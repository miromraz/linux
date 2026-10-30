// SPDX-License-Identifier: GPL-2.0-only
/* Included by mtk-pmic-keys.c to exercise the actual private handler. */
#include <kunit/test.h>
#include <linux/irq_sim.h>
#include <linux/kthread.h>

struct pmic_key_test {
	struct mtk_pmic_keys keys;
	struct mt6397_chip chip;
	struct irq_domain *parent;
	struct input_handler handler;
	struct input_handle handle;
	struct completion drained;
	u16 regs[0x10000];
	int fail_reg;
	int error;
	unsigned int reads;
	unsigned int events;
	unsigned int syncs;
	bool watch_drain;
	bool block_read;
	struct completion read_started;
	struct completion read_continue;
	struct completion irq_done;
};

static int pmic_key_read(void *context, unsigned int reg, unsigned int *val)
{
	struct pmic_key_test *t = context;

	if (reg == t->fail_reg) {
		t->reads++;
		if (t->error)
			return t->error; /* Deliberately leave the output untouched. */
	}
	*val = t->regs[reg];
	if (reg == t->fail_reg && t->block_read) {
		t->block_read = false;
		complete(&t->read_started);
		wait_for_completion(&t->read_continue);
	}
	if (t->watch_drain && reg == t->chip.int_status[3])
		complete(&t->drained);
	return 0;
}

static int pmic_key_write(void *context, unsigned int reg, unsigned int val)
{
	struct pmic_key_test *t = context;
	unsigned int i;

	for (i = 0; i < t->chip.num_irq_regs; i++) {
		if (reg == t->chip.int_status[i]) {
			t->regs[reg] &= ~val;
			return 0;
		}
	}
	t->regs[reg] = val;
	return 0;
}

static const struct regmap_config pmic_key_regmap = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_read = pmic_key_read,
	.reg_write = pmic_key_write,
};

static void pmic_key_event(struct input_handle *handle, unsigned int type,
			   unsigned int code, int value)
{
	struct pmic_key_test *t = handle->private;

	if (type == EV_KEY)
		t->events++;
	if (type == EV_SYN && code == SYN_REPORT)
		t->syncs++;
}

static int pmic_key_init(struct kunit *test)
{
	struct pmic_key_test *t;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	test->priv = t;
	t->fail_reg = -1;
	init_completion(&t->drained);
	init_completion(&t->read_started);
	init_completion(&t->read_continue);
	init_completion(&t->irq_done);
	t->keys.dev = root_device_register("pmic-key-read-test");
	if (IS_ERR(t->keys.dev))
		return PTR_ERR(t->keys.dev);
	t->keys.regmap = regmap_init(t->keys.dev, NULL, t, &pmic_key_regmap);
	if (IS_ERR(t->keys.regmap)) {
		ret = PTR_ERR(t->keys.regmap);
		goto device;
	}
	t->keys.input_dev = input_allocate_device();
	if (!t->keys.input_dev) {
		ret = -ENOMEM;
		goto map;
	}
	t->keys.input_dev->name = "pmic-key-read-test";
	input_set_capability(t->keys.input_dev, EV_KEY, KEY_POWER);
	input_set_capability(t->keys.input_dev, EV_KEY, KEY_HOME);
	ret = input_register_device(t->keys.input_dev);
	if (ret) {
		input_free_device(t->keys.input_dev);
		goto map;
	}
	t->handler.event = pmic_key_event;
	INIT_LIST_HEAD(&t->handler.h_list);
	t->handle.dev = t->keys.input_dev;
	t->handle.handler = &t->handler;
	t->handle.name = "pmic-key-observer";
	t->handle.private = t;
	ret = input_register_handle(&t->handle);
	if (ret)
		goto input;
	ret = input_open_device(&t->handle);
	if (!ret)
		return 0;
	input_unregister_handle(&t->handle);
input:
	input_unregister_device(t->keys.input_dev);
map:
	regmap_exit(t->keys.regmap);
device:
	root_device_unregister(t->keys.dev);
	return ret;
}

static void pmic_key_exit(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;

	if (t->keys.keys[0].keys)
		mtk_pmic_key_stop(&t->keys.keys[0]);
	input_close_device(&t->handle);
	input_unregister_handle(&t->handle);
	input_unregister_device(t->keys.input_dev);
	regmap_exit(t->keys.regmap);
	root_device_unregister(t->keys.dev);
}

struct pmic_key_variant {
	const char *name;
	const struct mtk_pmic_regs *regs;
};

static const struct pmic_key_variant pmic_key_variants[] = {
	{ "mt6323", &mt6323_regs },
	{ "mt6331", &mt6331_regs },
	{ "mt6351", &mt6351_regs },
	{ "mt6357", &mt6357_regs },
	{ "mt6358", &mt6358_regs },
	{ "mt6359", &mt6359_regs },
	{ "mt6397", &mt6397_regs },
};

static void pmic_key_description(const struct pmic_key_variant *v, char *desc)
{
	strscpy(desc, v->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(pmic_key_variant, pmic_key_variants, pmic_key_description);

static void pmic_key_check_sample(struct kunit *test,
				  struct mtk_pmic_keys_info *info,
				  int irq, bool prior, int error)
{
	struct pmic_key_test *t = test->priv;
	unsigned int events, syncs, reads;

	t->error = 0;
	t->regs[t->fail_reg] = prior ? 0 : info->regs->deb_mask;
	mtk_pmic_keys_irq_handler_thread(irq, info);
	events = t->events;
	syncs = t->syncs;
	reads = t->reads;
	t->error = error;
	KUNIT_EXPECT_EQ(test, mtk_pmic_keys_irq_handler_thread(irq, info), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, t->reads, reads + 1);
	KUNIT_EXPECT_EQ(test, t->events, events);
	KUNIT_EXPECT_EQ(test, t->syncs, syncs);
	KUNIT_EXPECT_EQ(test, !!test_bit(info->keycode, t->keys.input_dev->key), prior);
	/* A later valid sample must recover either transition. */
	t->error = 0;
	t->regs[t->fail_reg] = prior ? info->regs->deb_mask : 0;
	mtk_pmic_keys_irq_handler_thread(irq, info);
	KUNIT_EXPECT_EQ(test, t->events, events + 1);
	KUNIT_EXPECT_EQ(test, t->syncs, syncs + 1);
	KUNIT_EXPECT_EQ(test, !!test_bit(info->keycode, t->keys.input_dev->key), !prior);
}

static void pmic_key_read_faults(struct kunit *test)
{
	const struct pmic_key_variant *v = test->param_value;
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info info = { .keys = &t->keys, .irq = 1 };
	unsigned int key, prior, edge, errors;
	const int failures[] = { -EIO, -ETIMEDOUT };

	mtk_pmic_key_init(&info);
	for (key = 0; key < MTK_PMIC_MAX_KEY_COUNT; key++) {
		info.regs = &v->regs->keys_regs[key];
		info.keycode = key ? KEY_HOME : KEY_POWER;
		info.irq_r = v->regs->key_release_irq ? 2 : 0;
		t->fail_reg = info.regs->deb_reg;
		for (prior = 0; prior < 2; prior++) {
			for (edge = 0; edge < (info.irq_r ? 2 : 1); edge++) {
				for (errors = 0; errors < ARRAY_SIZE(failures); errors++)
					pmic_key_check_sample(test, &info,
							      edge ? info.irq_r : info.irq,
							      prior, failures[errors]);
			}
		}
	}
}

static void pmic_key_parent_run(struct kunit *test, bool recover)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info info = {
		.keys = &t->keys,
		.regs = &mt6351_regs.keys_regs[0],
		.keycode = KEY_POWER,
	};
	unsigned int child;
	int ret;

	mtk_pmic_key_init(&info);
	t->parent = irq_domain_create_sim(NULL, 1);
	KUNIT_ASSERT_FALSE(test, IS_ERR(t->parent));
	t->chip.irq = irq_create_mapping(t->parent, 0);
	t->chip.dev = t->keys.dev;
	t->chip.regmap = t->keys.regmap;
	t->chip.chip_id = MT6351_CHIP_ID;
	if (!devres_open_group(t->keys.dev, t, GFP_KERNEL)) {
		KUNIT_FAIL(test, "devres group allocation failed");
		goto domain;
	}
	ret = mt6397_irq_init(&t->chip);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto group;
	/* MT6351 power-key press is bank zero, bit zero. */
	child = irq_create_mapping(t->chip.irq_domain, 0);
	ret = request_threaded_irq(child, NULL, mtk_pmic_keys_irq_handler_thread,
				   IRQF_ONESHOT, "pmic-key-child", &info);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto group;
	t->fail_reg = info.regs->deb_reg;
	if (recover) {
		info.can_resample = true;
		mtk_pmic_keys_irq_handler_thread(1, &info);
		t->regs[t->fail_reg] = info.regs->deb_mask;
		t->events = 0;
		t->syncs = 0;
		t->reads = 0;
	}
	t->error = -ETIMEDOUT;
	t->regs[t->chip.int_status[0]] = BIT(0);
	t->watch_drain = true;
	ret = irq_set_irqchip_state(t->chip.irq, IRQCHIP_STATE_PENDING, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&t->drained, HZ), 0UL);
	synchronize_irq(t->chip.irq);
	KUNIT_EXPECT_EQ(test, t->reads, 1U);
	KUNIT_EXPECT_EQ(test, t->events, 0U);
	KUNIT_EXPECT_EQ(test, t->syncs, 0U);
	KUNIT_EXPECT_EQ(test, !!test_bit(KEY_POWER, t->keys.input_dev->key), recover);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_status[0]], (u16)0);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->chip.irq_retry_work));
	if (recover) {
		t->error = 0;
		flush_delayed_work(&info.retry_work);
		KUNIT_EXPECT_EQ(test, t->events, 1U);
		KUNIT_EXPECT_FALSE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
		goto free_child;
	}
	/* Baseline case still supplies another delivery. */
	t->error = 0;
	reinit_completion(&t->drained);
	t->regs[t->chip.int_status[0]] = BIT(0);
	irq_set_irqchip_state(t->chip.irq, IRQCHIP_STATE_PENDING, true);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&t->drained, HZ), 0UL);
	synchronize_irq(t->chip.irq);
	KUNIT_EXPECT_EQ(test, t->events, 1U);
	KUNIT_EXPECT_EQ(test, t->syncs, 1U);
	KUNIT_EXPECT_TRUE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
free_child:
	free_irq(child, &info);
	mtk_pmic_key_stop(&info);
group:
	devres_release_group(t->keys.dev, t);
domain:
	irq_dispose_mapping(t->chip.irq);
	irq_domain_remove_sim(t->parent);
}

static void pmic_key_parent_fault(struct kunit *test)
{
	pmic_key_parent_run(test, false);
}

static void pmic_key_parent_resample(struct kunit *test)
{
	pmic_key_parent_run(test, true);
}

/* All recovery cases use the production work item, not a retry model. */
static struct mtk_pmic_keys_info *pmic_key_recovery_init(struct kunit *test,
							 const struct mtk_pmic_regs *regs)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = &t->keys.keys[0];

	info->keys = &t->keys;
	info->regs = &regs->keys_regs[0];
	info->keycode = KEY_POWER;
	info->can_resample = regs->can_resample;
	mtk_pmic_key_init(info);
	dev_set_drvdata(t->keys.dev, &t->keys);
	t->fail_reg = info->regs->deb_reg;
	return info;
}

static void pmic_key_resample_release(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, &mt6351_regs);

	mtk_pmic_keys_irq_handler_thread(1, info);
	KUNIT_ASSERT_TRUE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
	t->error = -EIO;
	t->regs[t->fail_reg] = info->regs->deb_mask;
	mtk_pmic_keys_irq_handler_thread(2, info);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&info->retry_work));
	KUNIT_EXPECT_EQ(test, t->events, 1U);
	t->error = 0;
	/* Wait for the scheduled recovery; inject no further physical IRQ. */
	flush_delayed_work(&info->retry_work);
	KUNIT_EXPECT_FALSE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
	KUNIT_EXPECT_EQ(test, t->events, 2U);
	KUNIT_EXPECT_EQ(test, t->syncs, 2U);
	KUNIT_EXPECT_FALSE(test, info->pending);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&info->retry_work));
}

static void pmic_key_resample_faults(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, &mt6351_regs);
	unsigned int i;

	/* Unknown initial physical state: errors must not create any event. */
	t->error = -ETIMEDOUT;
	mtk_pmic_keys_irq_handler_thread(1, info);
	KUNIT_EXPECT_EQ(test, info->retry_ms, 100U);
	for (i = 0; i < 12; i++) {
		flush_delayed_work(&info->retry_work);
		KUNIT_EXPECT_LE(test, info->retry_ms, 5000U);
		KUNIT_EXPECT_TRUE(test, info->pending);
		KUNIT_EXPECT_EQ(test, t->events, 0U);
	}
	KUNIT_EXPECT_EQ(test, info->retry_ms, 5000U);
	KUNIT_EXPECT_FALSE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
	t->error = 0;
	flush_delayed_work(&info->retry_work);
	KUNIT_EXPECT_EQ(test, t->events, 1U);
	KUNIT_EXPECT_EQ(test, info->retry_ms, 0U);
}

static void pmic_key_resample_new_irq(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, &mt6351_regs);
	unsigned int reads;

	t->error = -EIO;
	mtk_pmic_keys_irq_handler_thread(2, info);
	t->error = 0;
	mtk_pmic_keys_irq_handler_thread(1, info);
	reads = t->reads;
	/* A worker already dispatched but waiting for lock sees !pending. */
	mtk_pmic_key_retry(&info->retry_work.work);
	KUNIT_EXPECT_EQ(test, t->reads, reads);
	KUNIT_EXPECT_EQ(test, t->events, 1U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&info->retry_work));
}

static void pmic_key_resample_lifetime(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, &mt6351_regs);
	unsigned int reads, events;

	mtk_pmic_keys_irq_handler_thread(1, info);
	t->error = -EIO;
	mtk_pmic_keys_irq_handler_thread(2, info);
	mtk_pmic_keys_suspend(t->keys.dev);
	reads = t->reads;
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&info->retry_work));
	mtk_pmic_keys_irq_handler_thread(2, info);
	mtk_pmic_key_retry(&info->retry_work.work);
	KUNIT_EXPECT_EQ(test, t->reads, reads);
	t->error = 0;
	t->regs[t->fail_reg] = info->regs->deb_mask;
	mtk_pmic_keys_resume(t->keys.dev);
	flush_delayed_work(&info->retry_work);
	KUNIT_EXPECT_FALSE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
	t->error = -EIO;
	mtk_pmic_keys_irq_handler_thread(1, info);
	/* Exercise the managed teardown action used before input unregister. */
	KUNIT_ASSERT_NOT_NULL(test, devres_open_group(t->keys.dev, t, GFP_KERNEL));
	KUNIT_ASSERT_EQ(test, devm_add_action_or_reset(t->keys.dev,
						       mtk_pmic_keys_stop, &t->keys), 0);
	devres_release_group(t->keys.dev, t);
	reads = t->reads;
	events = t->events;
	t->error = 0;
	mtk_pmic_keys_irq_handler_thread(1, info);
	mtk_pmic_key_retry(&info->retry_work.work);
	mtk_pmic_keys_resume(t->keys.dev);
	KUNIT_EXPECT_EQ(test, t->reads, reads);
	KUNIT_EXPECT_EQ(test, t->events, events);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&info->retry_work));
}

static void pmic_key_resample_variant(struct kunit *test)
{
	const struct pmic_key_variant *v = test->param_value;
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, v->regs);

	t->error = -EIO;
	mtk_pmic_keys_irq_handler_thread(2, info);
	KUNIT_EXPECT_EQ(test, delayed_work_pending(&info->retry_work),
			v->regs == &mt6351_regs);
	KUNIT_EXPECT_EQ(test, t->events, 0U);
}

static int pmic_key_irq_task(void *data)
{
	struct mtk_pmic_keys_info *info = data;
	struct pmic_key_test *t = container_of(info->keys, struct pmic_key_test, keys);

	mtk_pmic_keys_irq_handler_thread(2, data);
	complete(&t->irq_done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static void pmic_key_resample_concurrent_irq(struct kunit *test)
{
	struct pmic_key_test *t = test->priv;
	struct mtk_pmic_keys_info *info = pmic_key_recovery_init(test, &mt6351_regs);
	struct task_struct *task;
	unsigned long started;

	t->error = -EIO;
	mtk_pmic_keys_irq_handler_thread(1, info);
	t->error = 0;
	t->block_read = true;
	mod_delayed_work(system_wq, &info->retry_work, 0);
	started = wait_for_completion_timeout(&t->read_started, HZ);
	if (!started) {
		complete(&t->read_continue);
		KUNIT_FAIL(test, "worker did not start");
		return;
	}
	/* Worker has captured press; release IRQ must publish after it. */
	t->regs[t->fail_reg] = info->regs->deb_mask;
	task = kthread_run(pmic_key_irq_task, info, "pmic-key-irq-test");
	complete(&t->read_continue);
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&t->irq_done, HZ), 0UL);
	kthread_stop(task);
	flush_delayed_work(&info->retry_work);
	KUNIT_EXPECT_FALSE(test, test_bit(KEY_POWER, t->keys.input_dev->key));
	KUNIT_EXPECT_EQ(test, t->events, 2U);
	KUNIT_EXPECT_FALSE(test, info->pending);
}

static struct kunit_case pmic_key_cases[] = {
	KUNIT_CASE_PARAM(pmic_key_read_faults, pmic_key_variant_gen_params),
	KUNIT_CASE(pmic_key_parent_fault),
	KUNIT_CASE(pmic_key_parent_resample),
	KUNIT_CASE(pmic_key_resample_concurrent_irq),
	KUNIT_CASE(pmic_key_resample_release),
	KUNIT_CASE(pmic_key_resample_faults),
	KUNIT_CASE(pmic_key_resample_new_irq),
	KUNIT_CASE(pmic_key_resample_lifetime),
	KUNIT_CASE_PARAM(pmic_key_resample_variant, pmic_key_variant_gen_params),
	{}
};

static struct kunit_suite pmic_key_suite = {
	.name = "mtk-pmic-key-read",
	.init = pmic_key_init,
	.exit = pmic_key_exit,
	.test_cases = pmic_key_cases,
};

kunit_test_suite(pmic_key_suite);
