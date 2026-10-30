#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Actual trigger/PM callback fault tests with modeled MMIO; run on llumik."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[5]
base = root / 'sound/soc/mediatek'
pcm = (base / 'mt6797/mt6797-afe-pcm.c').read_text()
fe = (base / 'common/mtk-afe-fe-dai.c').read_text()


def function(source, name):
    start = re.search(r'^(?:static )?int ' + name + r'\(', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (base / 'mt6797/mt6797-reg.h').read_text()
source += re.search(r'struct mtk_base_irq_data \{.*?\n};',
                    (base / 'common/mtk-base-afe.h').read_text(), re.S)[0]
common = (base / 'mt6797/mt6797-afe-common.h').read_text()
source += common[common.index('enum {'):common.index('struct clk;')]
source += re.search(r'static const struct mtk_base_irq_data irq_data.*?\n};', pcm, re.S)[0]
source += r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#define BIT(n) (1U << (n))
#define dev_err(...) ((void)0)
#define SNDRV_PCM_TRIGGER_START 1
#define SNDRV_PCM_TRIGGER_RESUME 2
#define SNDRV_PCM_TRIGGER_STOP 3
#define SNDRV_PCM_TRIGGER_SUSPEND 4
typedef int snd_pcm_state_t;
struct snd_pcm_substream;
struct snd_pcm_runtime { unsigned int rate, period_size; struct snd_pcm_substream *trigger_master; bool stop_operating; };
struct pcm_ops { int (*trigger)(struct snd_pcm_substream *, int); int (*hw_free)(struct snd_pcm_substream *); };
struct snd_pcm_substream { struct snd_pcm_runtime *runtime; struct pcm_ops *ops; bool managed_buffer_alloc; };
static bool freed;
static bool snd_pcm_running(struct snd_pcm_substream *s) { return true; }
static void snd_pcm_sync_stop(struct snd_pcm_substream *s, bool sync) {}
static void snd_pcm_lib_free_pages(struct snd_pcm_substream *s) { freed = true; }

struct snd_soc_pcm_runtime {};
struct mtk_base_afe_memif { int irq_usage; bool trigger_active, trigger_dirty, config_dirty; };
struct mtk_base_afe_irq { const struct mtk_base_irq_data *irq_data; };
struct mt6797_afe_private { bool clocks_enabled; };
struct mtk_base_afe {
 void *regmap, *platform_priv;
 struct mtk_base_afe_memif *memif;
 struct mtk_base_afe_irq *irqs;
 int irqs_size, memif_size;
 bool hardware_dirty; int memif_lock;
 int (*memif_stop)(struct mtk_base_afe *, int);
 int (*memif_start_check)(struct mtk_base_afe *, int);
 int (*irq_fs)(struct snd_pcm_substream *, unsigned int);
};
struct device { struct mtk_base_afe *afe; };
struct snd_soc_dai { struct mtk_base_afe *afe; int id; };
static struct snd_soc_dai cpu;
#define snd_soc_substream_to_rtd(s) ((void *)0)
#define snd_soc_dai_get_drvdata(d) ((d)->afe)
#define snd_soc_rtd_to_cpu(r,n) (&cpu)
#define dev_get_drvdata(d) ((d)->afe)
static int step, fail, persistent, invalid, clocks, timeout;
static unsigned int regs[8192], dma;
static int operation(void) {
 step++;
 return (step == fail || (persistent && step >= fail)) ? -EIO : 0;
}
static int regmap_write(void *m, int r, unsigned int v) {
 int ret = operation();
 /* Model partial writes even on error. */
 if (r == AFE_IRQ_MCU_CLR) regs[AFE_IRQ_MCU_STATUS] &= ~v;
 else regs[r] = v;
 return ret;
}
static int regmap_update_bits(void *m, int r, unsigned int mask, unsigned int val) {
 return regmap_write(m, r, (regs[r] & ~mask) | val);
}
static int mtk_regmap_update_bits(void *m, int r, unsigned int mask, unsigned int val, int shift) {
 return regmap_update_bits(m, r, mask << shift, val << shift);
}
#define mtk_regmap_write regmap_write
static int mtk_memif_set_enable(struct mtk_base_afe *a, int id) {
 dma |= BIT(id); return operation();
}
static int mtk_memif_set_disable(struct mtk_base_afe *a, int id) {
 int ret = operation(); if (!ret) dma &= ~BIT(id); return ret;
}
static int rate(struct snd_pcm_substream *s, unsigned int r) { return invalid ? -EINVAL : 10; }
static int mt6797_afe_enable_clock(struct mtk_base_afe *a) {
 int ret = operation(); if (!ret) { assert(!clocks); clocks++; } return ret;
}
static int mt6797_afe_disable_clock(struct mtk_base_afe *a) {
 assert(clocks == 1); clocks--; return 0;
}
static int poll(void) { int ret = operation(); return ret ?: (timeout ? -ETIMEDOUT : 0); }
#define regmap_read_poll_timeout(m,r,v,cond,delay,bound) poll()
'''
source += '#define spin_lock_irqsave(l,f) ((void)0)\n#define spin_unlock_irqrestore(l,f) ((void)0)\n'
source += function(fe, 'mtk_afe_fe_stop')
source += function(fe, 'mtk_afe_fe_trigger_locked')
source += function(fe, 'mtk_afe_fe_trigger')
source += function(pcm, 'mt6797_afe_runtime_suspend')
source += function(pcm, 'mt6797_afe_runtime_resume')
core = (root / 'sound/core/pcm_native.c').read_text()
source += function(core, 'snd_pcm_do_stop')
source += function(core, 'do_hw_free')
source += r'''
static int stop_callback(struct snd_pcm_substream *s, int cmd) { return mtk_afe_fe_trigger(s, cmd, &cpu); }
static int free_callback(struct snd_pcm_substream *s) { return -EIO; }
int main(void) {
 struct mtk_base_afe_memif mem[5] = {0};
 struct mtk_base_afe_irq slots[5];
 struct mt6797_afe_private priv = {0};
 struct mtk_base_afe a = {.memif=mem, .irqs=slots, .irqs_size=5, .memif_size=5,
                         .platform_priv=&priv, .irq_fs=rate};
 struct device dev = {.afe=&a}; cpu.afe=&a;
 struct snd_pcm_runtime rt = {.rate=48000, .period_size=240};
 struct snd_pcm_substream s = {.runtime=&rt};
 for (int i=0; i<5; i++) { mem[i].irq_usage=i; slots[i].irq_data=&irq_data[i]; }
 for (int i=0; i<5; i++) {
  cpu.id=i; invalid=1; step=0;
  assert(mtk_afe_fe_trigger(&s, 1, &cpu)==-EINVAL && !step && !dma);
  invalid=0;
  for (int f=1; f<=5; f++) {
   fail=f; step=0;
   assert(mtk_afe_fe_trigger(&s, 1, &cpu)==-EIO);
   assert(!dma && !mem[i].trigger_active && !mem[i].trigger_dirty);
  }
  for (int f=1; f<=3; f++) {
   fail=0; step=0; assert(!mtk_afe_fe_trigger(&s, 1, &cpu));
   fail=f; step=0; assert(mtk_afe_fe_trigger(&s, 3, &cpu)==-EIO);
   assert(mem[i].trigger_dirty && mem[i].trigger_active == (f==1));
   assert(mtk_afe_fe_trigger(&s, 1, &cpu)==-EIO);
   fail=0; assert(!mtk_afe_fe_trigger(&s, 3, &cpu));
   assert(!mem[i].trigger_dirty && !dma);
  }
  fail=5; persistent=1; step=0;
  assert(mtk_afe_fe_trigger(&s, 1, &cpu)==-EIO && mem[i].trigger_dirty);
  persistent=fail=0; assert(!mtk_afe_fe_trigger(&s, 3, &cpu));
 }
 fail=step=0; assert(!mt6797_afe_runtime_resume(&dev) && clocks==1);
 for (int i=0; i<5; i++) { cpu.id=i; assert(!mtk_afe_fe_trigger(&s, 1, &cpu)); }
 assert(mt6797_afe_runtime_suspend(&dev)==-EBUSY && clocks==1);
 for (int i=0; i<5; i++) {
  cpu.id=i; assert(!mtk_afe_fe_trigger(&s, 4, &cpu));
  if (i<4) assert(mt6797_afe_runtime_suspend(&dev)==-EBUSY);
 }
 for (int f=1; f<=3; f++) {
  step=0; fail=f; assert(mt6797_afe_runtime_suspend(&dev)==-EIO);
  assert(clocks==1 && priv.clocks_enabled && !a.hardware_dirty);
 }
 fail=0; timeout=1; assert(mt6797_afe_runtime_suspend(&dev)==-ETIMEDOUT && clocks==1);
 timeout=0; persistent=1; fail=1; step=0;
 assert(mt6797_afe_runtime_suspend(&dev)==-EIO && a.hardware_dirty && clocks==1);
 cpu.id=0; assert(mtk_afe_fe_trigger(&s, 1, &cpu)==-EIO);
 persistent=fail=0; assert(!mt6797_afe_runtime_suspend(&dev) && !clocks);
 for (int f=1; f<=7; f++) {
  step=0; fail=f; assert(mt6797_afe_runtime_resume(&dev)==-EIO);
  assert(!clocks && !priv.clocks_enabled);
 }
 step=0; fail=2; persistent=1;
 assert(mt6797_afe_runtime_resume(&dev)==-EIO && clocks==1 && a.hardware_dirty);
 fail=persistent=0;
 assert(!mt6797_afe_runtime_resume(&dev) && clocks==1 && !a.hardware_dirty);
 assert(!mt6797_afe_runtime_suspend(&dev) && !clocks);
 mem[0].config_dirty=true; cpu.id=0; fail=step=0;
 assert(mtk_afe_fe_trigger(&s,1,&cpu)==-EIO && !step);
 mem[0].config_dirty=false;
 /* Actual core boundary: a callback errno does NOT prevent stop/free. */
 struct pcm_ops ops = {.trigger=stop_callback, .hw_free=free_callback};
 s.ops=&ops; s.managed_buffer_alloc=true; rt.trigger_master=&s;
 cpu.id=0; fail=step=0; assert(!mtk_afe_fe_trigger(&s, 1, &cpu));
 step=0; fail=1;
 assert(!snd_pcm_do_stop(&s, 0) && rt.stop_operating && mem[0].trigger_dirty);
 assert(do_hw_free(&s)==-EIO && freed);
 puts("CONFIRMED LIMIT: ALSA core discards STOP errno and frees managed pages despite hw_free error; persistent-DMA containment is unproven");
 puts("PASS: actual five-FE trigger and runtime suspend/resume faults, partial writes, retained ownership, retries");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / 'transactions.c'
    path.write_text(source)
    binary = pathlib.Path(tmp) / 'transactions'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=address,undefined',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
