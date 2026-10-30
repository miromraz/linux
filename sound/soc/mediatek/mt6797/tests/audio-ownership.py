#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Production stop/finalization -> actual ALSA release/free, libc allocator.

Normal powered MMIO and bounded synthetic faults are separate from the
permanent-stuck counterexample. No kernel DMA, VMA or IRQ race proof.
"""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[5]
native = (root / 'sound/core/pcm_native.c').read_text()
memory = (root / 'sound/core/pcm_memory.c').read_text()


def function(source, name):
    start = re.search(r'^(?:static )?(?:int|void) ' + name + r'\(', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


base = root / 'sound/soc/mediatek'
fe = (base / 'common/mtk-afe-fe-dai.c').read_text()
pcm = (base / 'mt6797/mt6797-afe-pcm.c').read_text()
header = (base / 'common/mtk-base-afe.h').read_text()
source = (base / 'mt6797/mt6797-reg.h').read_text()
source += r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define dev_err(...) ((void)0)
#define SNDRV_PCM_TRIGGER_START 1
#define SNDRV_PCM_TRIGGER_RESUME 2
#define SNDRV_PCM_TRIGGER_STOP 3
#define SNDRV_PCM_TRIGGER_SUSPEND 4
#define SNDRV_PCM_STATE_OPEN 0
#define PCM_RUNTIME_CHECK(s) 0
#define kfree free
typedef int snd_pcm_state_t;
struct snd_card {};
struct snd_pcm { struct snd_card *card; };
struct snd_dma_buffer { void *area; };
struct snd_pcm_substream;
struct snd_pcm_runtime {
 struct snd_pcm_substream *trigger_master;
 bool stop_operating; int state; unsigned int rate, period_size;
 void *dma_area; struct snd_dma_buffer *dma_buffer_p;
};
struct pcm_ops { int (*trigger)(struct snd_pcm_substream *, int);
 int (*hw_free)(struct snd_pcm_substream *); int (*close)(struct snd_pcm_substream *); };
struct snd_pcm_substream { struct snd_pcm_runtime *runtime; struct pcm_ops *ops;
 struct snd_pcm *pcm; struct snd_dma_buffer dma_buffer;
 bool managed_buffer_alloc; int ref_count, hw_opened, latency_pm_qos_req;
 void (*pcm_release)(struct snd_pcm_substream *);
};
static bool dma_live, freed, closed, detached, adversarial;
static int synced, resources;
static bool snd_pcm_running(struct snd_pcm_substream *s) { return dma_live; }
static void do_free_pages(struct snd_card *c, struct snd_dma_buffer *b) {
 assert(dma_live == adversarial && synced); free(b->area); freed=true;
}
static void snd_pcm_set_runtime_buffer(struct snd_pcm_substream *s, struct snd_dma_buffer *b) {
 s->runtime->dma_buffer_p=b; s->runtime->dma_area=b ? b->area : NULL;
}
static void snd_pcm_sync_stop(struct snd_pcm_substream *s, bool irq) {}
static bool cpu_latency_qos_request_active(int *p) { return false; }
static void cpu_latency_qos_remove_request(int *p) {}
static void snd_pcm_detach_substream(struct snd_pcm_substream *s) { detached=true; }
'''
for name in ('mtk_base_memif_data', 'mtk_base_irq_data', 'mtk_base_afe_memif'):
    source += re.search(r'struct ' + name + r' \{.*?\n};', header, re.S)[0] if name != 'mtk_base_afe_memif' else ''
source += r'''
struct mtk_base_afe_memif {
 const struct mtk_base_memif_data *data; int irq_usage;
 bool trigger_active, trigger_dirty, config_dirty, dram_resource;
};
struct mtk_base_afe_irq { const struct mtk_base_irq_data *irq_data; };
struct mt6797_afe_private { bool clocks_enabled; int irq; };
struct mtk_base_afe {
 void *dev, *regmap, *platform_priv; int memif_size, irqs_size, memif_lock;
 bool hardware_dirty, shutting_down;
 struct mtk_base_afe_memif *memif; struct mtk_base_afe_irq *irqs;
 int (*memif_stop)(struct mtk_base_afe *, int);
 int (*memif_start_check)(struct mtk_base_afe *, int);
 void (*synchronize_irq)(struct mtk_base_afe *);
 int (*irq_fs)(struct snd_pcm_substream *, unsigned int);
 int (*release_dram_resource)(void *);
};
struct snd_soc_pcm_runtime {};
struct snd_soc_dai { struct mtk_base_afe *afe; int id; };
static struct snd_soc_dai cpu;
#define snd_soc_substream_to_rtd(s) ((void *)0)
#define snd_soc_dai_get_drvdata(d) ((d)->afe)
#define snd_soc_rtd_to_cpu(r,n) (&cpu)
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; } while (0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l)); *(l)=0; } while (0)
struct platform_device { struct mtk_base_afe *afe; };
#define platform_get_drvdata(p) ((p)->afe)
static unsigned int regs[8192];
static int mode, writes, reads, global_off, start_fault;
static void live(void) { dma_live=!!(regs[AFE_DAC_CON0] & BIT(1)); }
static int regmap_update_bits(void *m, int r, unsigned int mask, unsigned int v) {
 if (r==AFE_DAC_CON0 && mask==BIT(1) && !v) {
  writes++;
  if (mode==4 || (mode==1 && writes==1) || (mode==9 && writes<=2)) return -EIO;
  if (mode==8 || (mode==7 && writes==1)) return 0;
 }
 if (r==AFE_IRQ_MCU_CON && mode==5 && !v) return -EIO;
 regs[r]=(regs[r]&~mask)|v; live();
 if (r==AFE_DAC_CON0 && mask==AFE_ON_MASK_SFT && !v) {
  assert(synced && !(regs[r]&0x1fe)); global_off++;
 }
 if (r==AFE_DAC_CON0 && mask==BIT(1)) {
  if (v && start_fault) return -EIO;
  if (!v && mode==2 && writes==1) return -EIO;
 }
 return 0;
}
static int regmap_read(void *m, int r, unsigned int *v) {
 reads++; if (mode==3 && reads==1) return -EIO;
 *v=regs[r]; return 0;
}
static int regmap_write(void *m, int r, unsigned int v) {
 if (r==AFE_IRQ_MCU_CLR) regs[AFE_IRQ_MCU_STATUS]&=~v;
 else regs[r]=v;
 return 0;
}
static int mtk_regmap_update_bits(void *m,int r,unsigned int mask,unsigned int v,int shift) {
 return regmap_update_bits(m,r,mask<<shift,v<<shift);
}
#define mtk_regmap_write regmap_write
static int mtk_memif_set_enable(struct mtk_base_afe *a,int id) {
 return regmap_update_bits(a->regmap,AFE_DAC_CON0,BIT(a->memif[id].data->enable_shift),BIT(a->memif[id].data->enable_shift));
}
static int mtk_memif_set_disable(struct mtk_base_afe *a,int id) { abort(); }
static int rate(struct snd_pcm_substream *s,unsigned int r) { return 10; }
static int release_dram(void *d) { assert(synced && dma_live==adversarial && resources==1); resources--; return 0; }
static void drain(struct mtk_base_afe *a) { assert(!a->memif_lock); synced++; }
static void mt6797_afe_sync_irq(struct mtk_base_afe *a) { drain(a); }
#define regmap_read_poll_timeout(m,r,v,cond,delay,bound) 0
'''
common = (base / 'mt6797/mt6797-afe-common.h').read_text()
source += common[common.index('enum {'):common.index('struct clk;')]
for table in ('memif_data', 'irq_data'):
    source += re.search(r'static const struct [^\n]* ' + table + r'\[.*?\n};', pcm, re.S)[0]
for name in ('mt6797_memif_validate', 'mt6797_memif_start_check', 'mt6797_memif_stop'):
    source += function(pcm, name)
for name in ('mtk_afe_fe_stop', 'mtk_afe_fe_hw_free', 'mtk_afe_fe_trigger_locked', 'mtk_afe_fe_trigger'):
    source += function(fe, name)
source += function(pcm, 'mt6797_afe_pcm_shutdown')
source += function(memory, 'snd_pcm_lib_free_pages')
source += function(native, 'do_hw_free')
source += function(native, 'snd_pcm_do_stop')
source += r'''
static void snd_pcm_drop(struct snd_pcm_substream *s) { snd_pcm_do_stop(s,0); }
static int trigger(struct snd_pcm_substream *s, int cmd) { return mtk_afe_fe_trigger(s,cmd,&cpu); }
static int hw_free(struct snd_pcm_substream *s) { return mtk_afe_fe_hw_free(s,&cpu); }
static int close_pcm(struct snd_pcm_substream *s) { assert(freed && dma_live==adversarial); closed=true; return 0; }
'''
source += function(native, 'snd_pcm_release_substream')
source += r'''
int main(void) {
 struct snd_card card; struct snd_pcm pcm={.card=&card};
 struct pcm_ops ops={.trigger=trigger,.hw_free=hw_free,.close=close_pcm};
 struct snd_pcm_runtime rt={.state=1,.rate=48000,.period_size=240};
 struct snd_pcm_substream s={.runtime=&rt,.ops=&ops,.pcm=&pcm,.managed_buffer_alloc=true};
 struct mtk_base_memif_data data[8]; struct mtk_base_afe_memif mem[8]={0};
 struct mtk_base_irq_data irq[5]={0}; struct mtk_base_afe_irq slots[5];
 struct mt6797_afe_private priv={.clocks_enabled=true};
 struct mtk_base_afe a={.memif=mem,.memif_size=8,.irqs=slots,.irqs_size=5,
  .platform_priv=&priv,.memif_stop=mt6797_memif_stop,.memif_start_check=mt6797_memif_start_check,
  .irq_fs=rate,.synchronize_irq=drain,.release_dram_resource=release_dram};
 cpu.afe=&a; rt.trigger_master=&s;
 unsigned int all_mask=0;
 for (int i=0;i<8;i++) { data[i]=memif_data[i]; mem[i].data=&data[i]; mem[i].irq_usage=i%5; assert(!mt6797_memif_validate(&a,i)); assert(!(all_mask&BIT(data[i].enable_shift))); all_mask|=BIT(data[i].enable_shift); }
 for (int i=0;i<5;i++) { irq[i]=irq_data[i]; slots[i].irq_data=&irq[i]; }
 for (int scenario=0;scenario<10;scenario++) {
  mode=scenario; writes=reads=synced=0; adversarial=(mode==4 || mode==8);
  freed=closed=detached=false; s.ref_count=s.hw_opened=1;
  resources=1; mem[0].dram_resource=true;
  mem[0].irq_usage=scenario==6?-1:0; mem[0].trigger_active=true;
  mem[0].trigger_dirty=mem[0].config_dirty=false;
  regs[AFE_DAC_CON0]=BIT(1)|BIT(2)|1; regs[AFE_IRQ_MCU_CON]=3; live();
  struct snd_dma_buffer *b=calloc(1,sizeof(*b)); assert(b); b->area=malloc(4096); assert(b->area);
  snd_pcm_set_runtime_buffer(&s,b);
  snd_pcm_release_substream(&s);
  assert(freed && closed && detached && !rt.dma_area && !resources);
  assert((regs[AFE_DAC_CON0]&BIT(2)) && (regs[AFE_IRQ_MCU_CON]&BIT(1)));
  assert(mem[0].trigger_active==adversarial);
  assert(writes<=4 && reads<=4);
  if (!scenario) assert(writes==2 && reads==2);
  if (scenario==9) assert(writes==3 && reads==1);
  if (adversarial) puts("OUTSIDE PROVIDER CONTRACT: permanently rejected disable still frees live modeled DMA; no containment claimed");
  if (scenario==5 || scenario==6) assert(!dma_live && mem[0].trigger_dirty);
 }
 mode=0; mem[0].irq_usage=0; mem[0].trigger_dirty=mem[0].config_dirty=false;
 /* Failed START after enable mutation must roll back using production helper. */
 start_fault=1; assert(trigger(&s,1)==-EIO && !dma_live && !mem[0].trigger_dirty); start_fault=0;
 assert(!trigger(&s,1) && dma_live);
 /* Explicit HW_FREE does not rely on ALSA running state or STOP. */
 assert(!hw_free(&s) && !dma_live); assert(!hw_free(&s));
 mem[0].config_dirty=false;
 priv.clocks_enabled=false; assert(trigger(&s,1)==-EIO); priv.clocks_enabled=true;
 a.hardware_dirty=true; assert(trigger(&s,1)==-EIO); a.hardware_dirty=false;
 data[0].enable_shift=-1; assert(trigger(&s,1)==-EINVAL); data[0].enable_shift=1;
 assert(!trigger(&s,1));
 /* All FE shutdown; CPU handler drained before global AFE off. */
 regs[AFE_DAC_CON0]=all_mask|1; regs[AFE_IRQ_MCU_CON]=0x400f;
 struct platform_device p={.afe=&a}; synced=0;
 mt6797_afe_pcm_shutdown(&p);
 assert(a.shutting_down && !(regs[AFE_DAC_CON0]&(all_mask|1)) && !regs[AFE_IRQ_MCU_EN]);
 assert(!(regs[AFE_IRQ_MCU_CON]&0x400f) && synced && global_off==1 && priv.clocks_enabled);
 assert(trigger(&s,1)==-EIO); assert(!hw_free(&s));
 /* IRQ cleanup failure must not restore slot-reuse eligibility. */
 mode=5; mt6797_afe_pcm_shutdown(&p);
 for (int i=0;i<8;i++) assert(mem[i].trigger_dirty && !mem[i].trigger_active);
 assert(global_off==1);
 puts("PASS: production disable/readback, bounded retry, pre-free finalizer, real ALSA release/free, shared FE, IRQ-only/invalid-slot, failed START, double HW_FREE, shutdown and START guards");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / 'ownership.c'
    path.write_text(source)
    binary = pathlib.Path(tmp) / 'ownership'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
