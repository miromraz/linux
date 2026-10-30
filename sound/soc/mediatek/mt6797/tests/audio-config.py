#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extract production configuration helpers; compile/run only on llumik."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[5]
fe = (root / 'sound/soc/mediatek/common/mtk-afe-fe-dai.c').read_text()
header = (root / 'sound/soc/mediatek/common/mtk-base-afe.h').read_text()


def function(name):
    start = re.search(r'^(?:static )?int ' + name + r'\(', fe, re.M).start()
    end = fe.index('{', start) + 1
    depth = 1
    while depth:
        depth += (fe[end] == '{') - (fe[end] == '}')
        end += 1
    return fe[start:end]


source = r'''
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t dma_addr_t;
typedef int snd_pcm_format_t;
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x) >> 32))
#define AFE_BASE_END_OFFSET 8
#define dev_err(...) ((void)0)
#define SNDRV_PCM_FORMAT_S16_LE 1
#define SNDRV_PCM_FORMAT_U16_LE 2
#define SNDRV_PCM_FORMAT_S32_LE 3
#define SNDRV_PCM_FORMAT_U32_LE 4
#define SNDRV_PCM_FORMAT_S24_LE 5
#define SNDRV_PCM_FORMAT_U24_LE 6
'''
for name in ('mtk_base_memif_data', 'mtk_base_afe_memif'):
    source += re.search(r'struct ' + name + r' \{.*?\n};', header, re.S)[0]
source += r'''
struct mtk_base_afe { struct mtk_base_afe_memif *memif; void *regmap; int memif_lock; int (*memif_stop)(struct mtk_base_afe *,int); void (*synchronize_irq)(struct mtk_base_afe *); bool memif_32bit_supported, hardware_dirty; void *dev; int (*request_dram_resource)(void *); int (*release_dram_resource)(void *); };
static int step, fail;
static unsigned int regs[64];
static int mtk_regmap_write(void *m, int r, unsigned int v) {
 regs[r] = v; /* even failed writes may partially reach hardware */
 return ++step == fail ? -EIO : 0;
}
static int mtk_regmap_update_bits(void *m, int r, unsigned int mask, unsigned int v, int shift) {
 return mtk_regmap_write(m, r, (regs[r] & ~(mask << shift)) | ((v & mask) << shift));
}
'''
for name in ('mtk_memif_set_addr', 'mtk_memif_set_channel', 'mtk_memif_set_rate_fs', 'mtk_memif_set_format'):
    source += function(name)
source += r'''
#define dev_dbg(...) ((void)0)
#define __force
#define __iomem
#define memset_io memset
struct snd_soc_pcm_runtime {};
struct snd_pcm_runtime { unsigned char *dma_area; dma_addr_t dma_addr; size_t dma_bytes; };
struct snd_pcm_substream { struct snd_pcm_runtime *runtime; };
struct snd_pcm_hw_params {};
struct snd_soc_dai { struct mtk_base_afe *afe; int id; };
static struct snd_soc_dai cpu;
#define snd_soc_substream_to_rtd(s) ((void *)0)
#define snd_soc_dai_get_drvdata(d) ((d)->afe)
#define snd_soc_rtd_to_cpu(r,n) (&cpu)
#define params_channels(p) 2
#define params_rate(p) 48000
#define params_format(p) SNDRV_PCM_FORMAT_S16_LE
static int resources, request_error;
static int request(void *d) { if(request_error) return -EIO; resources++; return 0; }
static int release(void *d) { resources--; return 0; }
static int mtk_memif_set_rate_substream(struct snd_pcm_substream *s, int id, unsigned int rate) {
 return mtk_memif_set_rate_fs(cpu.afe,id,3);
}
'''
source += function('mtk_afe_fe_hw_params')
source += '''
#define spin_lock_irqsave(l,f) ((void)0)
#define spin_unlock_irqrestore(l,f) ((void)0)
/* This suite exercises non-opted-in configuration; finalizer in ownership suite. */
static int mtk_afe_fe_stop(struct mtk_base_afe *a, int id) { abort(); }
'''
source += function('mtk_afe_fe_hw_free')
source += r'''
int main(void) {
 struct mtk_base_memif_data d = {.reg_ofs_base=1, .reg_ofs_end=2,
 .reg_ofs_base_msb=3, .reg_ofs_end_msb=4, .msb_reg=5, .msb_end_reg=6,
 .quad_ch_reg=7, .quad_ch_mask=1, .int_odd_flag_reg=8, .mono_reg=9,
 .fs_reg=10, .fs_maskbit=15, .hd_reg=11, .hd_align_reg=12};
 struct mtk_base_afe_memif mem = {.data=&d};
 struct mtk_base_afe a = {.memif=&mem};
 for (int f=1; f<=6; f++) {
  mem.dma_addr=123; fail=f; step=0;
  assert(mtk_memif_set_addr(&a,0,NULL,0xfffffff0ULL,32)==-EIO);
  assert(step==f && mem.dma_addr==123);
  fail=step=0;
  assert(!mtk_memif_set_addr(&a,0,NULL,0xfffffff0ULL,32));
  assert(regs[1]==0xfffffff0 && regs[2]==15 && regs[3]==0 && regs[4]==1);
 }
 for (int f=1; f<=3; f++) {
  fail=f; step=0; assert(mtk_memif_set_channel(&a,0,1)==-EIO && step==f);
 }
 fail=1; step=0; assert(mtk_memif_set_rate_fs(&a,0,3)==-EIO);
 for (int f=1; f<=2; f++) {
  fail=f; step=0; assert(mtk_memif_set_format(&a,0,SNDRV_PCM_FORMAT_S32_LE)==-EIO && step==f);
 }
 fail=step=0; assert(mtk_memif_set_format(&a,0,99)==-EINVAL && !step);
 d.reg_ofs_base_msb=d.reg_ofs_end_msb=d.msb_end_reg=0;
 assert(mtk_memif_set_addr(&a,0,NULL,0xfffffff0ULL,32)==-ERANGE && !step);
 assert(mtk_memif_set_addr(&a,0,NULL,0x200000000ULL,32)==-ERANGE && !step);
 assert(mtk_memif_set_addr(&a,0,NULL,0,0)==-EINVAL && !step);
 assert(mtk_memif_set_addr(&a,0,NULL,UINT64_MAX-7,16)==-EINVAL && !step);
 d.msb_reg=0;
 assert(mtk_memif_set_addr(&a,0,NULL,0x100000000ULL,32)==-ERANGE && !step);
 unsigned char area[32];
 struct snd_pcm_runtime rt={.dma_area=area,.dma_addr=0x1000,.dma_bytes=32};
 struct snd_pcm_substream stream={.runtime=&rt}; struct snd_pcm_hw_params params;
 cpu.afe=&a; a.request_dram_resource=request; a.release_dram_resource=release;
 fail=step=0; assert(!mtk_afe_fe_hw_params(&stream,&params,&cpu));
 int writes=step;
 assert(resources==1 && !mem.config_dirty);
 assert(!mtk_afe_fe_hw_free(&stream,&cpu) && !resources && mem.config_dirty);
 for (int f=1; f<=writes; f++) {
  fail=f; step=0;
  assert(mtk_afe_fe_hw_params(&stream,&params,&cpu)==-EIO);
  assert(mem.config_dirty && !resources && step==f);
  assert(!mtk_afe_fe_hw_free(&stream,&cpu) && !resources);
  fail=step=0;
  assert(!mtk_afe_fe_hw_params(&stream,&params,&cpu) && !mem.config_dirty && resources==1);
  assert(!mtk_afe_fe_hw_free(&stream,&cpu) && !resources);
 }
 request_error=1; step=0;
 assert(mtk_afe_fe_hw_params(&stream,&params,&cpu)==-EIO && !step && !resources);
 request_error=0; mem.trigger_active=true;
 assert(mtk_afe_fe_hw_params(&stream,&params,&cpu)==-EIO && !step);
 puts("PASS: actual hw_params each write failure, resource unwind, hw_free, retry and active rejection");
 puts("PASS: each address/channel/rate/format write failure; retry, partial writes, end carry, 32/33-bit rejection and overflow");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp) / 'config.c'
    path.write_text(source)
    binary = pathlib.Path(tmp) / 'config'
    subprocess.run(['cc', '-std=gnu11', '-O2', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
