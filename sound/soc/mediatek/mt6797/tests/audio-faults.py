#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Compile actual driver functions against fault-injection stubs. Run on llumik.

These tests exercise C control flow, not hardware or Linux IRQ concurrency.
"""
import pathlib
import re
import os
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[5]


def function(path, name):
    source = (root / path).read_text()
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


clock = function('sound/soc/mediatek/mt6797/mt6797-afe-clk.c',
                 'mt6797_afe_enable_clock')
bias = function('sound/soc/codecs/mt6351.c', 'mt6351_bias_update')
route = function('sound/soc/mediatek/mt6797/mt6797-mt6351.c',
                 'mt6797_update_outputs')
startup = function('sound/soc/mediatek/common/mtk-afe-fe-dai.c',
                   'mtk_afe_fe_startup')
shutdown = function('sound/soc/mediatek/common/mtk-afe-fe-dai.c',
                    'mtk_afe_fe_shutdown')
power_down = function('sound/soc/mediatek/mt6797/mt6797-afe-pcm.c',
                      'mt6797_afe_power_down')
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
struct regmap;
struct device { bool enabled; int usage, powered; int refs, busy, bound; struct { int status; } links; };
static bool pm_runtime_enabled(struct device *d) { return d->enabled; }
static int pm_runtime_put_sync_suspend(struct device *d) {
 assert(d->enabled && d->usage == 1); d->usage--; d->powered = 0; return 0;
}
static void pm_runtime_disable(struct device *d) {
 assert(!d->usage && !d->powered); d->enabled = false;
}
static int mt6797_afe_runtime_suspend(struct device *d) { d->powered = 0; return 0; }

#define BIT(x) (1U << (x))
#define dev_err(...) ((void)0)
#define CLK_INFRA_SYS_AUD 0
#define CLK_INFRA_SYS_AUD_26M 1
#define CLK_TOP_MUX_AUD 2
#define CLK_TOP_MUX_AUD_BUS 3
#define CLK_CLK26M 4
struct clk { int id; };
struct mt6797_afe_private { struct clk **clk; };
struct snd_pcm_hardware { int periods_max; };
struct snd_pcm_runtime {};
struct snd_pcm_substream { struct snd_pcm_runtime *runtime; int stream; };
struct snd_soc_pcm_runtime {};
struct memdata { int agent_disable_reg, agent_disable_shift; };
struct mtk_base_afe_memif {
 struct snd_pcm_substream *substream;
 const struct memdata *data;
 int irq_usage, const_irq; bool trigger_active, trigger_dirty;
};
struct mtk_base_afe {
 void *platform_priv, *dev, *regmap; bool hardware_dirty;
 struct mtk_base_afe_memif *memif;
 const struct snd_pcm_hardware *mtk_afe_hardware;
 int irqs_size;
 void (*synchronize_irq)(struct mtk_base_afe *);
};
struct snd_soc_dai { int id; struct mtk_base_afe *afe; };
static struct snd_soc_dai cpu;
#define SNDRV_PCM_TRIGGER_STOP 0
#define dev_err(...) ((void)0)
static int mtk_afe_fe_trigger(struct snd_pcm_substream *s, int c, struct snd_soc_dai *d) { return -5; }

#define snd_soc_substream_to_rtd(s) ((void *)0)
#define snd_soc_dai_get_drvdata(d) ((d)->afe)
#define snd_soc_rtd_to_cpu(r, n) (&cpu)
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 0
#define SNDRV_PCM_HW_PARAM_PERIODS 1
#define SNDRV_PCM_STREAM_CAPTURE 1
#define snd_soc_set_runtime_hwparams(...) ((void)0)
#define smp_store_release(p, v) (*(p) = (v))
#define WRITE_ONCE(p, v) ((p) = (v))
static int step, fail, refs[4], disabled[4], nd;
static int clk_prepare_enable(struct clk *c) {
 if (++step == fail) return -EIO;
 refs[c->id]++; return 0;
}
static int clk_set_parent(struct clk *c, struct clk *p) {
 return ++step == fail ? -EIO : 0;
}
static void clk_disable_unprepare(struct clk *c) {
 assert(refs[c->id] == 1); refs[c->id]--; disabled[nd++] = c->id;
}
static int constraint(void) { return ++step == fail ? -EINVAL : 0; }
#define snd_pcm_hw_constraint_step(...) constraint()
#define snd_pcm_hw_constraint_minmax(...) constraint()
#define snd_pcm_hw_constraint_integer(...) constraint()
static int occupied, drained;
static int mtk_dynamic_irq_acquire(struct mtk_base_afe *a) {
 if (occupied) return a->irqs_size;
 occupied = 1; return 0;
}
static int mtk_dynamic_irq_release(struct mtk_base_afe *a, int i) {
 assert(i == 0); occupied = 0; return 0;
}
static int mtk_regmap_update_bits(void *m, int r, int mask, int val, int shift) {
 return ++step == fail ? -EIO : 0;
}
static void drain(struct mtk_base_afe *a) {
 assert(!a->memif->substream); assert(a->memif->irq_usage == 0);
 assert(occupied); drained++;
}
#define RG_AUDMICBIAS1DCSW1NEN 2
#define RG_AUDMICBIAS1VREF 4
#define RG_AUDMICBIAS1LOWPEN 7
#define MT6351_AUDENC_ANA_CON10 0
struct mt6351_priv {
 struct device *dev; void *regmap; bool capture_bias, capture_dcc, bias_dirty, accdet_bias; unsigned int accdet_vref, accdet_low_power;
};
static unsigned int reg;
static bool partial_write;
static int regmap_write_bits(void *m, int r, unsigned int mask, unsigned int v) {
 if (fail) { if (partial_write) reg = (reg & ~mask) | v; return -EIO; }
 reg = (reg & ~mask) | v; return 0;
}
struct snd_soc_dapm_context {};
struct mt6797_mt6351_priv {
 void *card; bool speaker_enabled, headphone_enabled, headphone_present;
};
static bool speaker, headphone;
#define snd_soc_card_to_dapm(c) ((void *)0)
#define snd_soc_dapm_enable_pin_unlocked(d, p) (*(p)[0] == 0)
#undef snd_soc_dapm_enable_pin_unlocked
static void pin(const char *p, bool on) {
 if (p[0] == 'S') speaker = on; else headphone = on;
}
#define snd_soc_dapm_enable_pin_unlocked(d, p) pin(p, true)
#define snd_soc_dapm_disable_pin_unlocked(d, p) pin(p, false)
#define snd_soc_dapm_sync_unlocked(d) ((void)0)
'''
# These model the framework contracts; they do not execute driver-core devres.
stubs += r'''
#define SND_SOC_DAPM_PRE_PMU 1
#define SND_SOC_DAPM_POST_PMD 2
struct snd_soc_dapm_widget { void *dapm; };
struct snd_kcontrol {};
struct snd_soc_component {};
#define snd_soc_dapm_to_component(d) ((struct snd_soc_component *)(d))
#define snd_soc_component_get_drvdata(c) ((struct mt6351_priv *)(c))
#define mutex_lock(l) ((void)0)
#define mutex_unlock(l) ((void)0)
#define EPROBE_DEFER 517
#define DL_DEV_DRIVER_BOUND 2
#define DL_DEV_UNBINDING 3
#define DL_FLAG_AUTOREMOVE_CONSUMER 1
static struct mt6351_priv *registry;
static bool remove_at_lock;
#define list_for_each_entry(p, h, n) for (p = registry; p; p = NULL)
static struct device *get_device(struct device *d) { d->refs++; return d; }
static void put_device(struct device *d) { assert(d->refs > 0); d->refs--; }
static bool device_trylock(struct device *d) { if (d->busy) return false; d->busy = 1; if (remove_at_lock) registry = NULL; return true; }
static void device_unlock(struct device *d) { assert(d->busy); d->busy = 0; }
static bool device_is_bound(struct device *d) { assert(d->busy); return d->bound; }
static void *device_link_add(struct device *c, struct device *d, int flags) {
 assert(d->busy && d->bound && d->links.status == DL_DEV_DRIVER_BOUND); return d;
}
'''
bias += function('sound/soc/codecs/mt6351.c', 'mt_mic_bias_1_event')
bias += function('sound/soc/codecs/mt6351.c', 'mt_mic_bias_1_dcc_event')
bias += function('sound/soc/codecs/mt6351.c', 'mt6351_micbias_accdet')
main = r'''
int main(void) {
 struct device dev = { .enabled = true, .usage = 1, .powered = 1 };
 mt6797_afe_power_down(&dev);
 assert(!dev.enabled && !dev.usage && !dev.powered);
 dev.powered = 1; mt6797_afe_power_down(&dev); assert(!dev.powered);
 struct clk cs[5]; struct clk *cp[5];
 struct mt6797_afe_private ap = { .clk = cp };
 struct mtk_base_afe a = { .platform_priv = &ap };
 for (int i = 0; i < 5; i++) { cs[i].id = i; cp[i] = &cs[i]; }
 for (fail = 1; fail <= 5; fail++) {
  step = nd = 0;
  assert(mt6797_afe_enable_clock(&a) == -EIO);
  for (int i = 0; i < 4; i++) assert(!refs[i]);
  for (int i = 1; i < nd; i++) assert(disabled[i] < disabled[i-1]);
 }
 fail = step = 0; assert(!mt6797_afe_enable_clock(&a));
 for (int i = 0; i < 4; i++) assert(refs[i] == 1);
 struct memdata md = {0};
 struct mtk_base_afe_memif mem = { .data = &md, .irq_usage = -1 };
 struct snd_pcm_hardware hw = { .periods_max = 8 };
 struct snd_pcm_runtime runtime;
 struct snd_pcm_substream stream = { .runtime = &runtime, .stream = 1 };
 a.memif = &mem; a.mtk_afe_hardware = &hw; a.irqs_size = 1;
 a.synchronize_irq = drain; cpu.afe = &a;
 for (fail = 1; fail <= 4; fail++) {
  step = 0; assert(mtk_afe_fe_startup(&stream, &cpu) < 0);
  assert(!mem.substream && mem.irq_usage == -1 && !occupied);
 }
 fail = step = 0; occupied = 1;
 assert(mtk_afe_fe_startup(&stream, &cpu) == -EBUSY);
 assert(!mem.substream && mem.irq_usage == -1);
 occupied = 0;
 assert(!mtk_afe_fe_startup(&stream, &cpu));
 assert(mem.substream == &stream && occupied);
 mtk_afe_fe_shutdown(&stream, &cpu);
 assert(drained == 1 && !occupied && !mem.substream && mem.irq_usage == -1);
 mem.irq_usage = 0; mem.substream = &stream; mem.trigger_dirty = true; occupied = 1;
 mtk_afe_fe_shutdown(&stream, &cpu);
 assert(mem.irq_usage == 0 && occupied && !mem.substream && mem.trigger_dirty);
 assert(mtk_afe_fe_startup(&stream, &cpu) == -EIO);
 mem.trigger_dirty = false; occupied = 0;
 struct mt6351_priv b = {0};
 reg = 1; /* DAPM power bit must be preserved. */
 assert(!mt6351_bias_update(&b, false, true, 5, 1)); assert(reg == 0xd1);
 assert(!mt6351_bias_update(&b, true, true, 5, 1)); assert(reg == 0x71);
 fail = 1;
 assert(mt6351_bias_update(&b, false, true, 5, 1) == -EIO);
 assert(!b.capture_bias && b.bias_dirty && reg == 0x71);
 fail = 0;
 assert(!mt6351_bias_update(&b, false, true, 5, 1)); assert(reg == 0xd1);
 assert(!mt6351_bias_update(&b, false, false, 0, 0)); assert(reg == 1);
 assert(!mt6351_bias_update(&b, false, true, 7, 3)); assert(reg == 0x1f5);
 assert(!mt6351_bias_update(&b, true, true, 7, 3)); assert(reg == 0x75);
 assert(!mt6351_bias_update(&b, false, true, 7, 3)); assert(reg == 0x1f5);
 /* DAPM does not abort on event failure. Check actual callback intent. */
 struct snd_soc_dapm_widget w = { .dapm = &b };
 fail = 1;
 assert(mt_mic_bias_1_event(&w, 0, SND_SOC_DAPM_PRE_PMU) == -EIO);
 assert(b.capture_bias && b.bias_dirty);
 assert(mt_mic_bias_1_event(&w, 0, SND_SOC_DAPM_POST_PMD) == -EIO);
 assert(!b.capture_bias && b.bias_dirty);
 fail = 0;
 assert(!mt_mic_bias_1_dcc_event(&w, 0, SND_SOC_DAPM_PRE_PMU));
 assert(!mt6351_bias_update(&b, false, false, 0, 0)); assert(reg == 5);
 assert(!mt_mic_bias_1_dcc_event(&w, 0, SND_SOC_DAPM_POST_PMD)); assert(reg == 1);
 /* Fixed IRQ with no drain retains the legacy publication behavior. */
 mem.const_irq = 1; mem.irq_usage = 0; mem.substream = &stream;
 a.synchronize_irq = 0; mtk_afe_fe_shutdown(&stream, &cpu);
 assert(mem.substream == &stream && mem.irq_usage == 0);
 /* Device references survive removal; priv is looked up again. */
 struct device supplier = { .bound = 1, .links.status = DL_DEV_DRIVER_BOUND };
 struct device consumer = {0}; b.dev = &supplier; registry = &b;
 supplier.busy = 1;
 assert(mt6351_micbias_accdet(&consumer, 0, true, 5, 1) == -EPROBE_DEFER);
 supplier.busy = 0; supplier.bound = 0;
 assert(mt6351_micbias_accdet(&consumer, 0, true, 5, 1) == -EPROBE_DEFER);
 supplier.bound = 1; supplier.links.status = DL_DEV_UNBINDING;
 assert(mt6351_micbias_accdet(&consumer, 0, true, 5, 1) == -EPROBE_DEFER);
 supplier.links.status = DL_DEV_DRIVER_BOUND;
 remove_at_lock = true;
 assert(mt6351_micbias_accdet(&consumer, 0, true, 5, 1) == -EPROBE_DEFER);
 assert(!supplier.refs && !supplier.busy);
 remove_at_lock = false; registry = &b;
 fail = 1; partial_write = true;
 assert(mt6351_micbias_accdet(&consumer, 0, true, 5, 1) == -EIO);
 assert(!b.accdet_bias && b.bias_dirty && !supplier.refs && !supplier.busy);
 fail = 0; partial_write = false;
 assert(!mt6351_micbias_accdet(&consumer, 0, true, 5, 1));
 supplier.busy = 1; supplier.links.status = DL_DEV_UNBINDING;
 fail = 1;
 assert(mt6351_micbias_accdet(&consumer, 0, false, 0, 0) == -EIO);
 assert(!b.accdet_bias && b.bias_dirty);
 assert(supplier.busy); supplier.busy = 0;
 fail = 0;
 assert(!mt_mic_bias_1_event(&w, 0, SND_SOC_DAPM_PRE_PMU)); assert(reg == 0x71);
 assert(!mt_mic_bias_1_event(&w, 0, SND_SOC_DAPM_POST_PMD)); assert(reg == 1);
 assert(!supplier.refs && !supplier.busy);
 struct mt6797_mt6351_priv p = {0};
 mt6797_update_outputs(&p); assert(!speaker && !headphone);
 for (int sp = 0; sp < 2; sp++) for (int hp = 0; hp < 2; hp++)
 for (int jack = 0; jack < 2; jack++) {
  p.speaker_enabled = sp; p.headphone_enabled = hp; p.headphone_present = jack;
  mt6797_update_outputs(&p);
  assert(speaker == (sp && !jack)); assert(headphone == (hp && jack));
 }
 puts("PASS: PM cleanup, clock faults, FE failures/exhaustion/drain ordering, bias ownership, routing truth table");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / 'faults.c'
    source.write_text(stubs + clock + startup + shutdown + bias + route + power_down + main)
    binary = pathlib.Path(tmp) / 'faults'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=undefined,address',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Fault every operation of the real ACCDET handlers, including poll errors.
accpath = 'sound/soc/codecs/mt6351-accdet.c'
accsrc = (root / accpath).read_text()
acc = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef unsigned int u32;
#define BIT(n) (1U << (n))
#define SND_JACK_HEADSET 3
#define SND_JACK_HEADPHONE 1
#define SND_JACK_BTN_0 16
#define SND_JACK_BTN_1 32
#define SND_JACK_BTN_2 64
#define SND_JACK_BTN_3 128
#define IRQ_HANDLED 1
typedef int irqreturn_t;
struct mutex {};
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define dev_err(...) ((void)0)
static int step, fail, failcode, reports, masked;
static unsigned int registers[4096];
static int operation(void) { return ++step == fail ? failcode : 0; }
static int regmap_write(void *map, unsigned int r, unsigned int v) {
 int err = operation(); if (err) return err; registers[r] = v; return 0;
}
static int regmap_update_bits(void *map, unsigned int r, unsigned int m, unsigned int v) {
 return regmap_write(map, r, (registers[r] & ~m) | v);
}
static int regmap_read(void *map, unsigned int r, unsigned int *v) {
 int err = operation(); if (err) return err; *v = registers[r]; return 0;
}
/* Poll is a bounded framework operation, injected as bus error or timeout. */
#define regmap_read_poll_timeout(m,r,v,cond,delay,timeout) operation()
static void disable_irq_nosync(int irq) { masked++; }
'''
acc += accsrc[accsrc.index('#define MT6351_TOP_CKPDN'):accsrc.index('static void mt6351_accdet_jack_report')]
acc += r'''
static void mt6351_accdet_jack_report(struct mt6351_accdet *p) { reports++; }
static int mt6351_accdet_read_voltage(struct mt6351_accdet *p) { return 0; }
'''
for name in ['mt6351_accdet_check_button', 'mt6351_accdet_check_jack',
             'mt6351_accdet_enable', 'mt6351_accdet_disable',
             'mt6351_accdet_handle_eint', 'mt6351_accdet_irq',
             'mt6351_accdet_init', 'mt6351_accdet_quiesce']:
    acc += function(accpath, name)
acc += r'''
int main(void) {
 struct mt6351_accdet p = {0};
 p.data.deb.pwm_width = p.data.deb.pwm_thresh = 1;
 failcode = -EIO; fail = 0; step = 0;
 assert(!mt6351_accdet_init(&p)); int init_steps = step;
 for (int f = 1; f <= init_steps; f++) {
  step = 0; fail = f; assert(mt6351_accdet_init(&p) == -EIO);
 }
 for (int plugged = 0; plugged < 2; plugged++) {
  p.plugged = plugged; fail = 0; step = 0;
  assert(!mt6351_accdet_handle_eint(&p)); int steps = step;
  for (int code = 0; code < 2; code++) for (int f = 1; f <= steps; f++) {
   p.plugged = plugged; p.jack_type = 3; p.btn_type = 16;
   step = reports = 0; fail = f; failcode = code ? -ETIMEDOUT : -EIO;
   assert(mt6351_accdet_handle_eint(&p) == failcode);
   assert(p.plugged == plugged && p.jack_type == 3 && p.btn_type == 16);
   assert(!reports);
  }
 }
 for (int eint = 0; eint < 2; eint++) {
  memset(&p, 0, sizeof(p)); p.plugged = true;
  registers[MT6351_ACCDET_CON12] = eint ? ACCDET_EINT_IRQ_STATUS : ACCDET_IRQ_STATUS;
  fail = 0; step = 0; mt6351_accdet_irq(1, &p); int steps = step;
  for (int f = 1; f <= steps; f++) {
   memset(&p, 0, sizeof(p)); p.plugged = true; p.jack_type = 3;
   registers[MT6351_ACCDET_CON12] = eint ? ACCDET_EINT_IRQ_STATUS : ACCDET_IRQ_STATUS;
   step = reports = masked = 0; fail = f; failcode = -EIO;
   mt6351_accdet_irq(1, &p);
   assert(p.irq_failed && p.plugged && p.jack_type == 3 && !reports && masked == 1);
   /* The other IRQ stops without further bus access or invented report. */
   int prior = step; mt6351_accdet_irq(2, &p); assert(step == prior && masked == 2);
  }
 }
 for (int f = 1; f <= 3; f++) {
  fail = f; step = 0; mt6351_accdet_quiesce(&p); assert(step == 3);
 }
 puts("PASS: actual ACCDET init, EINT, IRQ and cleanup fault paths (modeled regmap/IRQ core)");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / 'accdet.c'
    source.write_text(acc)
    binary = pathlib.Path(tmp) / 'accdet'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=undefined,address',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Execute the actual ISR and shutdown on two pthreads. These locks model the
# PCM stream lock and synchronize_irq, not Linux hardirq scheduling/lockdep.
irqtest = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#define BIT(n) (1U << (n))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
/* Actual register definitions, enums, descriptor type and table below. */
typedef int irqreturn_t;
struct snd_pcm_substream { pthread_mutex_t lock; bool running; int callbacks; };
struct mtk_base_afe_irq { const struct mtk_base_irq_data *irq_data; };
struct memdata { int agent_disable_reg, agent_disable_shift; };
struct mtk_base_afe_memif {
 _Atomic(struct snd_pcm_substream *) substream; int irq_usage; bool const_irq, trigger_active, trigger_dirty;
 struct memdata *data;
};
struct mtk_base_afe {
 struct mtk_base_afe_memif *memif; struct mtk_base_afe_irq *irqs;
 int irqs_size; void *regmap; void (*synchronize_irq)(struct mtk_base_afe *);
};
struct snd_soc_dai { int id; struct mtk_base_afe *afe; };
struct snd_soc_pcm_runtime {};
static struct snd_soc_dai cpu;
#define SNDRV_PCM_TRIGGER_STOP 0
#define dev_err(...) ((void)0)
static int mtk_afe_fe_trigger(struct snd_pcm_substream *s, int c, struct snd_soc_dai *d) { return -5; }

#define snd_soc_substream_to_rtd(s) ((void *)0)
#define snd_soc_dai_get_drvdata(d) ((d)->afe)
#define snd_soc_rtd_to_cpu(r,n) (&cpu)
#define smp_load_acquire(p) atomic_load_explicit(p, memory_order_acquire)
#define WRITE_ONCE(p,v) ((p) = (v))
#define READ_ONCE(p) (p)
static unsigned int status, enabled, acked;
static int step, fail;
static pthread_t irq_thread;
static pthread_barrier_t observed, resume_irq, draining;
static bool pause_irq;
static bool irq_done, released, sequential_close;
static int closing_slot = 4;
static void lock_stream(struct snd_pcm_substream *s) {
 if (pause_irq) {
  pause_irq = false;
  pthread_barrier_wait(&observed); pthread_barrier_wait(&resume_irq);
 }
 pthread_mutex_lock(&s->lock);
}
#define snd_pcm_stream_lock_irqsave(s,f) lock_stream(s)
#define snd_pcm_stream_unlock_irqrestore(s,f) pthread_mutex_unlock(&(s)->lock)
static bool snd_pcm_running(struct snd_pcm_substream *s) { return s->running; }
static void snd_pcm_period_elapsed_under_stream_lock(struct snd_pcm_substream *s) {
 assert(s->running); s->callbacks++;
}
static int regmap_read(void *m, int r, unsigned int *v) {
 if (++step == fail) return -5;
 *v = r == AFE_IRQ_MCU_STATUS ? status : enabled; return 0;
}
static int regmap_write(void *m, int r, unsigned int v) {
 assert(r == AFE_IRQ_MCU_CLR); if (++step == fail) return -5;
 acked |= v; status &= ~v; return 0;
}
static int mtk_regmap_update_bits(void *m, int r, int mask, int val, int shift) { return 0; }
static int mtk_dynamic_irq_release(struct mtk_base_afe *a, int slot) {
 assert(irq_done); assert(slot == closing_slot); released = true; return 0;
}
static void drain_irq(struct mtk_base_afe *a) {
 assert(!atomic_load(&a->memif[cpu.id].substream));
 assert(a->memif[cpu.id].irq_usage == closing_slot);
 if (sequential_close) return;
 pthread_barrier_wait(&draining);
 pthread_join(irq_thread, NULL); assert(irq_done);
}
'''
# Never duplicate the production descriptor table in this harness.
pcm_source = (root / 'sound/soc/mediatek/mt6797/mt6797-afe-pcm.c').read_text()
common_source = (root / 'sound/soc/mediatek/mt6797/mt6797-afe-common.h').read_text()
base_source = (root / 'sound/soc/mediatek/common/mtk-base-afe.h').read_text()
actual = (root / 'sound/soc/mediatek/mt6797/mt6797-reg.h').read_text()
actual += common_source[common_source.index('enum {'):common_source.index('struct clk;')]
actual += re.search(r'struct mtk_base_irq_data \{.*?\n};', base_source, re.S)[0]
actual += re.search(r'static const struct mtk_base_irq_data irq_data.*?\n};', pcm_source, re.S)[0]
irqtest = actual + irqtest
irqtest += function(os.getenv('REPRO_V3_SOURCE', 'sound/soc/mediatek/mt6797/mt6797-afe-pcm.c'),
                    'mt6797_afe_irq_handler') + shutdown
irqtest += r'''
static void *run_irq(void *a) {
 mt6797_afe_irq_handler(0, a); irq_done = true; return NULL;
}
static void *close_fe(void *s) { mtk_afe_fe_shutdown(s, &cpu); return NULL; }
int main(void) {
 struct snd_pcm_substream streams[2] = {
  { .lock = PTHREAD_MUTEX_INITIALIZER, .running = false },
  { .lock = PTHREAD_MUTEX_INITIALIZER, .running = true }
 };
 struct memdata md = {0};
 struct mtk_base_afe_memif mem[MT6797_MEMIF_NUM] = {
  { .substream = &streams[0], .irq_usage = 0, .data = &md },
  { .substream = &streams[1], .irq_usage = 1, .data = &md }
 };
 struct mtk_base_afe_irq slots[MT6797_IRQ_NUM];
 for (int i = 0; i < MT6797_IRQ_NUM; i++) slots[i].irq_data = &irq_data[i];
 struct mtk_base_afe a = { .memif = mem, .irqs = slots, .irqs_size = MT6797_IRQ_NUM,
                          .synchronize_irq = drain_irq };
 cpu.afe = &a;
 /* Independent register oracle: ON={0,1,2,3,14}, status/route/W1C={0,1,2,3,6}. */
 const unsigned int expected[5] = {1, 2, 4, 8, 64};
 struct snd_pcm_substream all[5];
 for (int i = 0; i < 5; i++) {
  assert(irq_data[i].irq_en_shift == (i == 4 ? 14 : i));
  assert(BIT(irq_data[i].irq_clr_shift) == expected[i]);
  all[i] = (struct snd_pcm_substream){ .lock = PTHREAD_MUTEX_INITIALIZER, .running = true };
  mem[i].substream = &all[i]; mem[i].irq_usage = i; mem[i].data = &md;
 }
 for (int selection = 1; selection < 32; selection++) {
  unsigned int bits = 0;
  for (int i = 0; i < 5; i++) { all[i].callbacks = 0; if (selection & BIT(i)) bits |= expected[i]; }
  status = bits; enabled = 0x7f; acked = 0;
  mt6797_afe_irq_handler(0, &a);
#ifdef REPRO_V3
  if (selection == 18) {
   printf("IRQ7 running: callbacks=%d, other FE callbacks=%d, pending=0x%x\n", all[4].callbacks, all[1].callbacks, status);
   assert(!all[4].callbacks && all[1].callbacks == 1 && status == 0x40);
   return 0;
  }
#else
  assert(!status && acked == bits);
  for (int i = 0; i < 5; i++) assert(all[i].callbacks == !!(selection & BIT(i)));
#endif
 }
 for (int i = 0; i < 5; i++) {
  all[i].running = false; all[i].callbacks = 0;
  status = expected[i]; acked = 0;
  mt6797_afe_irq_handler(0, &a);
  assert(!status && acked == expected[i] && !all[i].callbacks);
  all[i].running = true; enabled = 0; status = expected[i];
  mt6797_afe_irq_handler(0, &a);
  assert(!status && !all[i].callbacks);
 }
 /* A failed read or W1C leaves pending state and suppresses that callback. */
 for (int f = 1; f <= 15; f++) {
  fail = f; step = 0; enabled = 0x7f; status = 0x4f;
  for (int i = 0; i < 5; i++) all[i].callbacks = 0;
  mt6797_afe_irq_handler(0, &a);
  int target = (f - 1) / 3;
  assert(status == expected[target] && !all[target].callbacks);
  for (int i = 0; i < 5; i++) if (i != target) assert(all[i].callbacks == 1);
 }
 fail = 0;
 for (int i = 0; i < MT6797_MEMIF_NUM; i++) mem[i].substream = NULL;
 mem[0].substream = &streams[0]; mem[0].irq_usage = 4;
 mem[1].substream = &streams[1]; mem[1].irq_usage = 1;
 status = enabled = 0x42;
 assert(mt6797_afe_irq_handler(0, &a) == IRQ_HANDLED);
 assert(!streams[0].callbacks && streams[1].callbacks == 1 && !status);
 pthread_barrier_init(&observed, NULL, 2);
 pthread_barrier_init(&resume_irq, NULL, 2);
 pthread_barrier_init(&draining, NULL, 2);
 pause_irq = true; status = 0x42;
 pthread_create(&irq_thread, NULL, run_irq, &a);
 pthread_barrier_wait(&observed);
 pthread_t closer; pthread_create(&closer, NULL, close_fe, &streams[0]);
 pthread_barrier_wait(&draining);
 assert(!released && mem[0].irq_usage == 4);
 pthread_barrier_wait(&resume_irq); pthread_join(closer, NULL);
 assert(released && mem[0].irq_usage == -1 && !atomic_load(&mem[0].substream));
 assert(!streams[0].callbacks && streams[1].callbacks == 2);
 mem[0].substream = &streams[0]; mem[0].irq_usage = 4;
 streams[0].running = true; status = 0x42;
 mt6797_afe_irq_handler(0, &a);
 assert(!status && streams[0].callbacks == 1 && streams[1].callbacks == 3);
 sequential_close = true;
 for (int closing = 0; closing < 5; closing++) {
  for (int i = 0; i < 5; i++) {
   mem[i].substream = &all[i]; mem[i].irq_usage = i; all[i].callbacks = 0;
  }
  cpu.id = closing; closing_slot = closing; released = false;
  mtk_afe_fe_shutdown(&all[closing], &cpu);
  assert(released && mem[closing].irq_usage == -1 && !mem[closing].substream);
  status = 0x4f & ~expected[closing]; enabled = 0x7f;
  mt6797_afe_irq_handler(0, &a);
  assert(!status && !all[closing].callbacks);
  for (int i = 0; i < 5; i++) if (i != closing) assert(all[i].callbacks == 1);
 }
 puts("PASS: actual ISR all five descriptors, 31 active combinations, fault/pending, IRQ7 reuse; multi-FE stopped/running state and paused IRQ vs shutdown (pthread model)");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = pathlib.Path(tmp) / 'irq.c'
    if os.getenv('REPRO_V3'):
        irqtest = '#define REPRO_V3\n' + irqtest.replace('irq_data->irq_status_shift', 'irq_data->irq_en_shift')
    source.write_text(irqtest)
    binary = pathlib.Path(tmp) / 'irq'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-pthread',
                    '-fsanitize=undefined,address', str(source), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
