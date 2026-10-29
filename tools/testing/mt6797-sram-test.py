#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract actual SRAM callbacks; synthetic regmap faults, NOT MMIO evidence.
Run this script with python3 on llumik.
The pthread lock models the rdev callback boundary, not regulator arbitration.
"""
import pathlib
import socket
import subprocess
import tempfile

if socket.gethostname().split('.')[0] != 'llumik':
    raise SystemExit('Run only on llumik')
root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'drivers/regulator/mt6797-cpu-sram-regulator.c').read_text()
callbacks = source[source.index('#define MT6797_CPULDO_CTRL_0'):
                   source.index('static const struct regulator_ops')]
callbacks += source[source.index('static int mt6797_cpu_sram_owned_read_unlocked'):
                    source.index('static int mt6797_cpu_sram_probe')]
barrier = (root / 'drivers/regulator/mt6797-boot-barrier.h').read_text()
barrier = barrier[barrier.index('static inline int'):barrier.index('#endif')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
struct mutex { pthread_mutex_t lock; };
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define dev_err(...) ((void)0)
struct regmap {
    unsigned regs[3], reads, writes, fail_read, fail_write, fail_write2;
    unsigned corrupt_read, drop_write, delays[16], ndelay;
};
struct regulator_dev { struct regmap *regmap; void *data; };
#define rdev_get_drvdata(r) ((r)->data)
static struct regmap *active;
static void udelay(unsigned us) {
    assert(active->ndelay < 16);
    active->delays[active->ndelay++] = us;
}
static int regmap_read(struct regmap *m, unsigned reg, unsigned *value) {
    if (++m->reads == m->fail_read) return -EIO;
    *value = m->regs[(reg-0xf98)/4];
    if (m->reads == m->corrupt_read) *value ^= 1;
    return 0;
}
static int regmap_update_bits(struct regmap *m, unsigned reg, unsigned mask,
                              unsigned value) {
    ++m->writes;
    if (m->writes == m->fail_write || m->writes == m->fail_write2)
        return -EIO;
    if (m->writes == m->drop_write) return 0;
    m->regs[(reg-0xf98)/4] = (m->regs[(reg-0xf98)/4] & ~mask) | (value & mask);
    return 0;
}
static int regulator_enable_regmap(struct regulator_dev *r) {
    return regmap_update_bits(r->regmap, 0xf98, 255, 255);
}
static int regulator_disable_regmap(struct regulator_dev *r) {
    return regmap_update_bits(r->regmap, 0xf98, 255, 0);
}
static int regulator_is_enabled_regmap(struct regulator_dev *r) {
    unsigned value;
    int ret = regmap_read(r->regmap, 0xf98, &value);
    return ret ? ret : (value & 255) == 255;
}
'''
tests = r'''
static struct regmap m;
static struct mt6797_cpu_sram state = {
    .rail_lock = { PTHREAD_MUTEX_INITIALIZER }, .regmap = &m,
};
static struct regulator_dev r = {&m, &state};
static unsigned packed(unsigned code) { return code * 0x01010101U; }
static void reset(unsigned code) {
    m = (struct regmap){.regs = {255, 0xa0b0c0d0U | packed(code),
                                   0xe0f08090U | packed(code)}};
    state.unresolved = false;
    state.boot_barrier = false;
    active = &m;
}
static void restored(void) {
    assert(m.regs[1] == (0xa0b0c0d0U | packed(9)));
    assert(m.regs[2] == (0xe0f08090U | packed(9)));
    assert(!state.unresolved);
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *writer(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) {
        pthread_mutex_lock(&lock);
        m.ndelay = 0;
        assert(!mt6797_cpu_sram_set_voltage_sel(&r, i % 9));
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
static void *reader(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 10000; i++) {
        pthread_mutex_lock(&lock);
        assert(mt6797_cpu_sram_get_voltage_sel(&r) >= 0);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
int main(void) {
    reset(9);
    state.boot_barrier = true;
    assert(!mt6797_cpu_sram_set_voltage_sel(&r, 2));
    assert(mt6797_cpu_sram_set_voltage_sel(&r, 1) == -EBUSY);
    assert(mt6797_cpu_sram_set_voltage_sel(&r, 3) == -EBUSY);
    assert(!mt6797_cpu_sram_enable(&r));
    assert(mt6797_cpu_sram_disable(&r) == -EBUSY);
    assert(!m.writes && !m.ndelay);
    m.regs[0] = 0;
    assert(mt6797_cpu_sram_set_voltage_sel(&r, 2) == -EIO);
    assert(mt6797_cpu_sram_enable(&r) == -EIO);
    assert(!m.writes && !m.ndelay);
    reset(9);
    assert(!mt6797_cpu_sram_disable(&r));
    assert(!m.regs[0]);
    for (unsigned c = 0; c < 16; c++) {
        reset(c);
        int expected = c == 0 ? 2 : c < 7 ? -EINVAL : (int)c - 7;
        assert(mt6797_cpu_sram_get_voltage_sel(&r) == expected);
        if (expected < 0) {
            assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EINVAL);
            assert(m.writes == 0);
        }
    }
    for (unsigned pattern = 0; pattern < 256; pattern++) {
        reset(9);
        for (unsigned f = 0; f < 8; f++)
            if (pattern & (1U << f)) m.regs[1+f/4] &= ~(15U << (8*(f%4)));
        assert(mt6797_cpu_sram_get_voltage_sel(&r) == 2);
    }
    for (unsigned f = 0; f < 8; f++) {
        for (unsigned c = 0; c < 16; c++) {
            reset(9);
            unsigned shift = 8*(f%4);
            m.regs[1+f/4] = (m.regs[1+f/4] & ~(15U << shift)) | (c << shift);
            int expected = c == 0 || c == 9 ? 2 : -EINVAL;
            assert(mt6797_cpu_sram_get_voltage_sel(&r) == expected);
            if (expected < 0) {
                assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EINVAL);
                assert(!m.writes);
            }
        }
    }
    reset(9);
    assert(mt6797_cpu_sram_set_voltage_sel(&r, 9) == -EINVAL);
    assert(mt6797_cpu_sram_set_voltage_sel(&r, ~0U) == -EINVAL);
    assert(!m.writes && !m.reads);
    for (unsigned from = 0; from < 9; from++) {
        for (unsigned to = 0; to < 9; to++) {
            reset(from+7);
            assert(!mt6797_cpu_sram_set_voltage_sel(&r, to));
            assert(m.regs[1] == (0xa0b0c0d0U | packed(to+7)));
            assert(m.regs[2] == (0xe0f08090U | packed(to+7)));
            unsigned delta = (from > to ? from-to : to-from) * 2500;
            unsigned delay = from == to ? 0 : to > from ?
                max((delta+1249)/1250+5, 25U) : max(delta*2/625+5, 25U);
            assert(m.ndelay == 1 && m.delays[0] == delay);
        }
    }
    for (unsigned fail = 1; fail <= 6; fail++) {
        reset(9); m.fail_read = fail;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
        restored();
        if (fail <= 3) assert(!m.writes && !m.ndelay);
        else assert(m.delays[m.ndelay-1] == 69);
    }
    for (unsigned fail = 1; fail <= 2; fail++) {
        reset(9); m.fail_write = fail;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
        restored();
        assert(m.ndelay == 2 && m.delays[0] == 69 && m.delays[1] == 69);
    }
    for (unsigned fail = 3; fail <= 4; fail++) {
        reset(9); m.fail_write = 2; m.fail_write2 = fail;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
        assert(state.unresolved);
        assert(mt6797_cpu_sram_get_voltage_sel(&r) == -EIO);
        unsigned writes = m.writes;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 2) == -EIO);
        assert(m.writes == writes);
        unsigned reads = m.reads, waits = m.ndelay;
        assert(mt6797_cpu_sram_enable(&r) == -EIO);
        assert(m.writes == writes && m.reads == reads && m.ndelay == waits);
    }
    for (unsigned fail = 4; fail <= 6; fail++) {
        reset(9); m.fail_write = 2; m.fail_read = fail;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
        assert(state.unresolved);
    }
    for (unsigned read = 5; read <= 6; read++) {
        reset(9); m.corrupt_read = read;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) < 0);
        restored();
    }
    for (unsigned drop = 1; drop <= 2; drop++) {
        reset(9); m.drop_write = drop;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) < 0);
        restored();
    }
    for (unsigned read = 5; read <= 6; read++) {
        reset(9); m.fail_write = 2; m.corrupt_read = read;
        assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
        assert(state.unresolved);
    }
    reset(0); m.regs[2] |= packed(9); m.fail_write = 2;
    assert(mt6797_cpu_sram_set_voltage_sel(&r, 8) == -EIO);
    assert(!state.unresolved);
    assert(m.regs[1] == 0xa0b0c0d0U);
    assert(m.regs[2] == (0xe0f08090U | packed(9)));
    for (unsigned enable = 0; enable < 256; enable++) {
        reset(9); m.regs[0] = enable;
        if (enable && enable != 255) {
            assert(mt6797_cpu_sram_get_voltage_sel(&r) == -EINVAL);
            assert(mt6797_cpu_sram_enable(&r) == -EIO);
            assert(!m.writes);
        } else {
            assert(mt6797_cpu_sram_get_voltage_sel(&r) == 2);
            assert(!mt6797_cpu_sram_set_voltage_sel(&r, 8));
            assert(!mt6797_cpu_sram_enable(&r));
            assert(m.regs[0] == 255);
        }
    }
    /* All-off failures: a failed verification can leave the bank on. */
    for (unsigned fault = 0; fault < 3; fault++) {
        reset(9); m.regs[0] = 0xabcdef00;
        if (fault == 0) m.fail_write = 1;
        if (fault == 1) m.fail_read = 4;
        if (fault == 2) m.drop_write = 1;
        assert(mt6797_cpu_sram_enable(&r) == -EIO);
        assert(m.regs[0] == (fault == 1 ? 0xabcdefffU : 0xabcdef00U));
        assert(m.ndelay == (fault == 0 ? 0 : 1));
        if (m.ndelay) assert(m.delays[0] == 69);
        restored();
        m.fail_write = m.fail_read = m.drop_write = 0;
        assert(!mt6797_cpu_sram_enable(&r));
        assert(m.regs[0] == 0xabcdefffU);
        assert(m.delays[m.ndelay-1] == 69);
        restored();
    }
    for (unsigned code = 1; code < 7; code++) {
        reset(code); m.regs[0] = 0;
        assert(mt6797_cpu_sram_enable(&r) == -EIO);
        assert(!m.writes && !m.ndelay && !m.regs[0]);
    }
    reset(9);
    assert(mt6797_cpu_sram_owned_read(&state) == 1050000);
    for (unsigned mask = 0; mask < 255; mask++) {
        reset(9); m.regs[0] = mask;
        assert(mt6797_cpu_sram_owned_read(&state) == -EIO);
    }
    for (unsigned field = 0; field < 8; field++) {
        reset(9); m.regs[1 + field / 4] ^= 1U << (8 * (field % 4));
        assert(mt6797_cpu_sram_owned_read(&state) == -EINVAL);
    }
    reset(9);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, NULL));
    assert(!pthread_create(&b, NULL, reader, NULL));
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
    puts("PASS: codes/fields, aliases, selectors, bits, faults/recovery, timing, enable, serialized callbacks");
    puts("Synthetic regmap errors only; no firmware, physical MMIO or regulator arbitration exercised.");
}
'''
with tempfile.TemporaryDirectory(prefix='mt6797-sram-') as tmp:
    src = pathlib.Path(tmp) / 'test.c'
    exe = pathlib.Path(tmp) / 'test'
    src.write_text(prelude + barrier + callbacks + tests)
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-sign-compare', '-pthread', '-fsanitize=undefined,address',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
