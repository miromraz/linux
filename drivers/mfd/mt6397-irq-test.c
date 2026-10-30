// SPDX-License-Identifier: GPL-2.0-only
/* Included here to exercise the actual private irqchip/control callbacks. */
#include <kunit/test.h>
#include <linux/irq_sim.h>
#include <linux/irqdesc.h>
#include <linux/kthread.h>

struct mt6397_irq_test {
	struct mt6397_chip chip;
	struct irq_domain *parent;
	struct irq_chip parent_chip;
	u16 regs[0x10000];
	int fail_reg;
	int read_errors;
	int write_errors;
	atomic_t delivered;
	bool block_write;
	bool posted;
	bool pending;
	bool fail_completion;
	bool self_disable;
	unsigned int pending_reg;
	unsigned int pending_val;
	unsigned int writes;
	struct completion entered;
	struct completion release;
	struct completion removing;
	struct completion removed;
};

static bool mt6397_test_error(int *count)
{
	if (!*count)
		return false;
	if (*count > 0)
		--*count;
	return true;
}

static void mt6397_test_apply(struct mt6397_irq_test *t, unsigned int reg,
			      unsigned int val)
{
	unsigned int i;

	for (i = 0; i < t->chip.num_irq_regs; i++) {
		if (reg == t->chip.int_status[i]) {
			t->regs[reg] &= ~val;
			return;
		}
	}
	t->regs[reg] = val;
}

static int mt6397_test_read(void *context, unsigned int reg, unsigned int *val)
{
	struct mt6397_irq_test *t = context;

	if (t->pending) {
		t->pending = false;
		mt6397_test_apply(t, t->pending_reg, t->pending_val);
		/* Completion can fail even after the side effect happened. */
		if (t->fail_completion) {
			t->fail_completion = false;
			return -EIO;
		}
	}
	if (reg == t->fail_reg && mt6397_test_error(&t->read_errors))
		return -EIO;
	*val = t->regs[reg];
	return 0;
}

static int mt6397_test_write(void *context, unsigned int reg, unsigned int val)
{
	struct mt6397_irq_test *t = context;

	t->writes++;
	if (t->block_write) {
		t->block_write = false;
		complete(&t->entered);
		wait_for_completion(&t->release);
	}
	if (reg == t->fail_reg && mt6397_test_error(&t->write_errors))
		return -EIO;
	if (t->posted) {
		t->pending = true;
		t->pending_reg = reg;
		t->pending_val = val;
	} else {
		mt6397_test_apply(t, reg, val);
	}
	return 0;
}

static const struct regmap_config mt6397_test_regmap = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_read = mt6397_test_read,
	.reg_write = mt6397_test_write,
};

static irqreturn_t mt6397_test_child(int irq, void *data)
{
	struct mt6397_irq_test *t = data;

	atomic_inc(&t->delivered);
	if (t->self_disable)
		disable_irq_nosync(irq);
	return IRQ_HANDLED;
}

static int mt6397_test_wake(struct irq_data *data, unsigned int on)
{
	return 0;
}

static int mt6397_test_wake_error(struct irq_data *data, unsigned int on)
{
	return -EIO;
}

static int mt6397_test_init(struct kunit *test)
{
	struct mt6397_irq_test *t;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	test->priv = t;
	init_completion(&t->entered);
	init_completion(&t->release);
	init_completion(&t->removing);
	init_completion(&t->removed);
	t->fail_reg = -1;
	t->chip.dev = root_device_register("mt6397-irq-test");
	if (IS_ERR(t->chip.dev))
		return PTR_ERR(t->chip.dev);
	t->parent = irq_domain_create_sim(NULL, 1);
	if (IS_ERR(t->parent)) {
		ret = PTR_ERR(t->parent);
		goto device;
	}
	t->chip.irq = irq_create_mapping(t->parent, 0);
	t->parent_chip = *irq_get_chip(t->chip.irq);
	t->parent_chip.irq_set_wake = mt6397_test_wake;
	irq_set_chip(t->chip.irq, &t->parent_chip);
	t->chip.chip_id = MT6351_CHIP_ID;
	t->chip.regmap = regmap_init(t->chip.dev, NULL, t, &mt6397_test_regmap);
	if (IS_ERR(t->chip.regmap)) {
		ret = PTR_ERR(t->chip.regmap);
		goto domain;
	}
	if (!devres_open_group(t->chip.dev, t, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto map;
	}
	ret = mt6397_irq_init(&t->chip);
	if (!ret)
		return 0;
	devres_release_group(t->chip.dev, t);
map:
	regmap_exit(t->chip.regmap);
domain:
	irq_dispose_mapping(t->chip.irq);
	irq_domain_remove_sim(t->parent);
device:
	root_device_unregister(t->chip.dev);
	return ret;
}

static void mt6397_test_exit(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;

	devres_release_group(t->chip.dev, t);
	regmap_exit(t->chip.regmap);
	irq_dispose_mapping(t->chip.irq);
	irq_domain_remove_sim(t->parent);
	root_device_unregister(t->chip.dev);
}

/* Advance the policy deadline deliberately; flush alone must not bypass it. */
static void mt6397_test_retry(struct mt6397_irq_test *t)
{
	mutex_lock(&t->chip.irqlock);
	t->chip.irq_retry_at = jiffies;
	mutex_unlock(&t->chip.irqlock);
	flush_delayed_work(&t->chip.irq_retry_work);
	synchronize_irq(t->chip.irq);
}

static void mt6397_test_masks(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct irq_data d = { .chip_data = &t->chip, .hwirq = 17 };

	t->fail_reg = t->chip.int_con[1];
	t->write_errors = 1;
	mt6397_irq_lock(&d);
	mt6397_irq_enable(&d);
	mt6397_irq_sync_unlock(&d);
	KUNIT_EXPECT_EQ(test, t->chip.irq_masks_cur[1], (u16)BIT(1));
	KUNIT_EXPECT_EQ(test, t->chip.irq_masks_cache[1], (u16)0);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_masks_valid & BIT(1));
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_EQ(test, t->regs[t->fail_reg], (u16)BIT(1));
	KUNIT_EXPECT_EQ(test, t->chip.irq_masks_cache[1], (u16)BIT(1));
	KUNIT_EXPECT_EQ(test, t->chip.irq_retries, 0U);
	mt6397_irq_lock(&d);
	mt6397_irq_disable(&d);
	mt6397_irq_sync_unlock(&d);
	KUNIT_EXPECT_EQ(test, t->regs[t->fail_reg], (u16)0);
}

static void mt6397_test_delivery(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	unsigned int irqs[2], i;
	int ret;

	for (i = 0; i < 2; i++) {
		irqs[i] = irq_create_mapping(t->chip.irq_domain, i * 16);
		KUNIT_ASSERT_NE(test, irqs[i], 0U);
		ret = request_threaded_irq(irqs[i], NULL, mt6397_test_child, 0, "test-child", t);
		KUNIT_ASSERT_EQ(test, ret, 0);
		t->regs[t->chip.int_status[i]] = 1;
	}
	/* First W1C fails: first child may be delivered twice, none lost. */
	t->fail_reg = t->chip.int_status[0];
	t->write_errors = 1;
	KUNIT_EXPECT_EQ(test, mt6397_irq_thread(t->chip.irq, &t->chip), IRQ_HANDLED);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	mt6397_test_retry(t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 3);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	/* A failed read must retain both banks for the recovery scan. */
	for (i = 0; i < 2; i++)
		t->regs[t->chip.int_status[i]] = 1;
	t->read_errors = 1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	mt6397_test_retry(t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 5);
	/* The IRQ core rejects disabled child actions, but status is drained. */
	disable_irq(irqs[0]);
	for (i = 0; i < 2; i++)
		t->regs[t->chip.int_status[i]] = 1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 6);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_status[i]], (u16)0);
		free_irq(irqs[i], t);
		irq_dispose_mapping(irqs[i]);
	}
}

static void mt6397_test_persistent_error(struct kunit *test, int mode)
{
	struct mt6397_irq_test *t = test->priv;
	unsigned int i;
	int ret;

	struct irq_data d = { .chip_data = &t->chip, .hwirq = 0 };

	t->fail_reg = mode == 1 ? t->chip.int_con[0] : t->chip.int_status[0];
	if (mode == 0)
		t->read_errors = -1;
	else
		t->write_errors = -1;
	if (mode == 1) {
		mt6397_irq_lock(&d);
		mt6397_irq_enable(&d);
		mt6397_irq_sync_unlock(&d);
	} else {
		t->regs[t->fail_reg] = 1;
		mt6397_irq_thread(t->chip.irq, &t->chip);
	}
	for (i = 0; i < MT6397_IRQ_RETRIES; i++)
		mt6397_test_retry(t);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_EQ(test, t->chip.irq_retries, (unsigned int)MT6397_IRQ_RETRIES);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t->chip.irq_retry_work));
	ret = irq_set_irqchip_state(t->chip.irq, IRQCHIP_STATE_PENDING, true);
	KUNIT_EXPECT_EQ(test, ret, 0);
	synchronize_irq(t->chip.irq);
	KUNIT_EXPECT_EQ(test, irq_to_desc(t->chip.irq)->depth, 1U);
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_EXPECT_EQ(test, notifier_to_errno(ret), -EBUSY);
	mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&t->chip.irq_retry_work));
	/* An early/stale queued worker cannot turn slow probing into a storm. */
	i = t->writes;
	flush_delayed_work(&t->chip.irq_retry_work);
	KUNIT_EXPECT_EQ(test, t->writes, i);
	KUNIT_EXPECT_EQ(test, t->chip.irq_retries, (unsigned int)MT6397_IRQ_RETRIES);
	t->read_errors = 0;
	t->write_errors = 0;
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_EQ(test, t->chip.irq_retries, 0U);
	KUNIT_EXPECT_EQ(test, irq_to_desc(t->chip.irq)->depth, 0U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->chip.irq_retry_work));
}

static void mt6397_test_persistent_read(struct kunit *test)
{
	mt6397_test_persistent_error(test, 0);
}

static void mt6397_test_persistent_mask(struct kunit *test)
{
	mt6397_test_persistent_error(test, 1);
}

static void mt6397_test_persistent_ack(struct kunit *test)
{
	mt6397_test_persistent_error(test, 2);
}

static void mt6397_test_pm_error(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	int ret;

	t->fail_reg = t->chip.int_con[2];
	t->write_errors = 1;
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_EXPECT_EQ(test, notifier_to_errno(ret), -EIO);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_suspended);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_EQ(test, ret, NOTIFY_OK);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
}

static int mt6397_test_remove_thread(void *data)
{
	struct mt6397_irq_test *t = data;

	complete(&t->removing);
	devm_release_action(t->chip.dev, mt6397_irq_cleanup, &t->chip);
	complete(&t->removed);
	return 0;
}

static void mt6397_test_remove(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct task_struct *task;

	t->fail_reg = t->chip.int_status[0];
	t->read_errors = 1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	t->chip.irq_retry_at = jiffies;
	t->block_write = true;
	mod_delayed_work(system_freezable_wq, &t->chip.irq_retry_work, 0);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&t->entered, HZ), 0UL);
	task = kthread_create(mt6397_test_remove_thread, t, "mfd-irq-remove");
	if (IS_ERR(task)) {
		complete(&t->release);
		KUNIT_FAIL(test, "could not start teardown thread");
		return;
	}
	get_task_struct(task);
	wake_up_process(task);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&t->removing, HZ), 0UL);
	complete(&t->release);
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&t->removed, HZ), 0UL);
	kthread_stop(task);
	put_task_struct(task);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_stopping);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->chip.irq_retry_work));
}

static void mt6397_test_initial_error(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct mt6397_chip chip = {
		.dev = t->chip.dev, .regmap = t->chip.regmap,
		.chip_id = MT6351_CHIP_ID, .irq = t->chip.irq,
	};

	t->fail_reg = MT6351_INT_CON0;
	t->write_errors = 1;
	KUNIT_EXPECT_EQ(test, mt6397_irq_init(&chip), -EIO);
	KUNIT_EXPECT_PTR_EQ(test, chip.irq_domain, NULL);
}

static void mt6397_test_posted(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct irq_data d = { .chip_data = &t->chip, .hwirq = 49 };
	int ret;

	t->posted = true;
	t->fail_completion = true;
	mt6397_irq_lock(&d);
	mt6397_irq_enable(&d);
	mt6397_irq_sync_unlock(&d);
	KUNIT_EXPECT_EQ(test, t->chip.irq_masks_valid, (u8)0);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->pending);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_con[3]], (u16)BIT(1));
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	/* Final W1C completion error after side effect must still contain. */
	t->regs[t->chip.int_status[3]] = BIT(1);
	t->fail_completion = true;
	ret = mt6397_irq_handle_reg(&t->chip, t->chip.int_status[3], 48);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_status[3]], (u16)0);
}

static void mt6397_test_queued_exhaustion(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct irq_data d = { .chip_data = &t->chip, .hwirq = 0 };
	unsigned long deadline;
	unsigned int writes;

	t->fail_reg = t->chip.int_con[0];
	t->write_errors = -1;
	mt6397_irq_lock(&d);
	mt6397_irq_enable(&d);
	mt6397_irq_sync_unlock(&d);
	/* Hold fifth worker in a real bus transaction, queue another invocation. */
	t->chip.irq_retries = MT6397_IRQ_RETRIES - 1;
	t->chip.irq_retry_at = jiffies;
	t->block_write = true;
	mod_delayed_work(system_freezable_wq, &t->chip.irq_retry_work, 0);
	if (!wait_for_completion_timeout(&t->entered, HZ)) {
		complete(&t->release);
		KUNIT_FAIL(test, "worker did not enter bus transaction");
		return;
	}
	queue_delayed_work(system_freezable_wq, &t->chip.irq_retry_work, 0);
	complete(&t->release);
	flush_work(&t->chip.irq_retry_work.work);
	mutex_lock(&t->chip.irqlock);
	deadline = t->chip.irq_retry_at;
	writes = t->writes;
	mutex_unlock(&t->chip.irqlock);
	/* Real child callbacks update intent but cannot bypass containment. */
	mt6397_irq_lock(&d);
	mt6397_irq_disable(&d);
	mt6397_irq_sync_unlock(&d);
	t->write_errors = 0;
	flush_delayed_work(&t->chip.irq_retry_work);
	KUNIT_EXPECT_EQ(test, t->chip.irq_retry_at, deadline);
	KUNIT_EXPECT_EQ(test, t->writes, writes);
	KUNIT_EXPECT_EQ(test, t->chip.irq_retries, (unsigned int)MT6397_IRQ_RETRIES);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_EQ(test, irq_to_desc(t->chip.irq)->depth, 1U);
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
}

static void mt6397_test_nested_disable(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	unsigned int irq = irq_create_mapping(t->chip.irq_domain, MT6351_IRQ_RTC);
	int ret;

	KUNIT_ASSERT_NE(test, irq, 0U);
	ret = request_threaded_irq(irq, NULL, mt6397_test_child, 0, "test-rtc", t);
	KUNIT_ASSERT_EQ(test, ret, 0);
	t->fail_reg = t->chip.int_status[0];
	t->read_errors = 1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	t->self_disable = true;
	t->regs[t->chip.int_status[MT6351_IRQ_RTC / 16]] = BIT(MT6351_IRQ_RTC % 16);
	mt6397_test_retry(t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 1);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_con[MT6351_IRQ_RTC / 16]], (u16)0);
	free_irq(irq, t);
	irq_dispose_mapping(irq);
}

static void mt6397_test_suspend(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	struct irq_data d = { .chip_data = &t->chip, .hwirq = 0 };
	int ret;

	mt6397_irq_lock(&d);
	mt6397_irq_enable(&d);
	mt6397_irq_sync_unlock(&d);
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_ASSERT_EQ(test, ret, NOTIFY_OK);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_con[0]], (u16)0);
	t->fail_reg = t->chip.int_status[0];
	t->read_errors = 1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&t->chip.irq_retry_work));
	/* Failed wake disarm stays contained and is retried by the worker. */
	t->parent_chip.irq_set_wake = mt6397_test_wake_error;
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_POST_SUSPEND, NULL);
	KUNIT_EXPECT_EQ(test, notifier_to_errno(ret), -EIO);
	mt6397_test_retry(t);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_wake_enabled);
	t->parent_chip.irq_set_wake = mt6397_test_wake;
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_wake_enabled);
	KUNIT_EXPECT_EQ(test, t->regs[t->chip.int_con[0]], (u16)1);
	/* Wake arm failure also rolls back suspended intent. */
	t->parent_chip.irq_set_wake = mt6397_test_wake_error;
	ret = mt6397_irq_pm_notifier(&t->chip.pm_nb, PM_SUSPEND_PREPARE, NULL);
	KUNIT_EXPECT_EQ(test, notifier_to_errno(ret), -EIO);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_suspended);
	mt6397_test_retry(t);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
}

static void mt6397_test_critical_recovery(struct kunit *test)
{
	struct mt6397_irq_test *t = test->priv;
	const unsigned int sources[] = {
		MT6351_IRQ_PWRKEY, MT6351_IRQ_RTC, MT6351_IRQ_THR_H,
		MT6351_IRQ_VCORE_OC, MT6351_IRQ_CHRDET, MT6351_IRQ_FG_BAT_H,
	};
	unsigned int irqs[ARRAY_SIZE(sources)], i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(sources); i++) {
		irqs[i] = irq_create_mapping(t->chip.irq_domain, sources[i]);
		KUNIT_ASSERT_NE(test, irqs[i], 0U);
		ret = request_threaded_irq(irqs[i], NULL, mt6397_test_child,
					   0, "critical-test", t);
		KUNIT_ASSERT_EQ(test, ret, 0);
		t->regs[t->chip.int_status[sources[i] / 16]] |= BIT(sources[i] % 16);
	}
	t->fail_reg = t->chip.int_status[0];
	t->read_errors = -1;
	mt6397_irq_thread(t->chip.irq, &t->chip);
	for (i = 0; i < MT6397_IRQ_RETRIES; i++)
		mt6397_test_retry(t);
	/* Later banks remain serviceable during contained probes. */
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 3);
	KUNIT_EXPECT_TRUE(test, t->chip.irq_disabled);
	KUNIT_EXPECT_TRUE(test, time_after(t->chip.irq_retry_at, jiffies + 4 * HZ));
	t->read_errors = 0;
	mt6397_test_retry(t);
	KUNIT_EXPECT_EQ(test, atomic_read(&t->delivered), 6);
	KUNIT_EXPECT_FALSE(test, t->chip.irq_disabled);
	for (i = 0; i < ARRAY_SIZE(sources); i++) {
		free_irq(irqs[i], t);
		irq_dispose_mapping(irqs[i]);
	}
}

static struct kunit_case mt6397_irq_cases[] = {
	KUNIT_CASE(mt6397_test_critical_recovery),
	KUNIT_CASE(mt6397_test_suspend),
	KUNIT_CASE(mt6397_test_masks),
	KUNIT_CASE(mt6397_test_posted),
	KUNIT_CASE(mt6397_test_queued_exhaustion),
	KUNIT_CASE(mt6397_test_nested_disable),
	KUNIT_CASE(mt6397_test_delivery),
	KUNIT_CASE(mt6397_test_persistent_read),
	KUNIT_CASE(mt6397_test_persistent_mask),
	KUNIT_CASE(mt6397_test_persistent_ack),
	KUNIT_CASE(mt6397_test_pm_error),
	KUNIT_CASE(mt6397_test_remove),
	KUNIT_CASE(mt6397_test_initial_error),
	{}
};

static struct kunit_suite mt6397_irq_suite = {
	.name = "mt6397-irq-recovery",
	.init = mt6397_test_init,
	.exit = mt6397_test_exit,
	.test_cases = mt6397_irq_cases,
};

kunit_test_suite(mt6397_irq_suite);
