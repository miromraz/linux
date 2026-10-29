#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual topology code with mocked OF lookups; run only on llumik."""
import pathlib
import socket
import subprocess
import tempfile

if socket.gethostname().split('.')[0] != 'llumik':
    raise SystemExit('Run only on llumik')
root = pathlib.Path(__file__).resolve().parents[2]
s = (root / 'drivers/regulator/mt6797-provider.c').read_text()
s = s[s.index('/* Accept either'):s.index('EXPORT_SYMBOL_GPL(mt6797_boot_barrier_supply);')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#define MT6797_RAILS 2
#define MT6797_PROC 0
#define MT6797_SRAM 1
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
struct device_node {
    const char *name, *compat;
    bool enabled;
    struct device_node *parent, *supply[4];
    int count[4], refs;
};
static struct device_node cpu[2], cci, proc, sram, group, chip, other;
static struct device_node *rails[2];
static int owner_lock;
static bool gemini = true;
static void mutex_lock(int *m) { assert(!*m); *m = 1; }
static void mutex_unlock(int *m) { assert(*m); *m = 0; }
static int prop(const char *n) {
    const char *names[] = {"proc-supply", "cpu-supply", "sram-supply", "mem-supply"};
    for (int i = 0; i < 4; i++) if (!strcmp(n, names[i])) return i;
    assert(0); return -1;
}
static struct device_node *of_node_get(struct device_node *n) { if (n) n->refs++; return n; }
static void of_node_put(struct device_node *n) { if (n) { assert(n->refs > 0); n->refs--; } }
static bool of_device_is_available(struct device_node *n) { return n && n->enabled; }
static bool of_property_present(struct device_node *n, const char *p) { return n->count[prop(p)]; }
static int of_property_count_u32_elems(struct device_node *n, const char *p) { return n->count[prop(p)]; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int index) {
    assert(!index); return of_node_get(n->supply[prop(p)]);
}
static struct device_node *of_get_cpu_node(int id, void *unused) {
    (void)unused; return of_node_get(&cpu[id == 4]);
}
static struct device_node *of_find_compatible_node(void *a, void *b, const char *c) {
    (void)a; (void)b; (void)c; return of_node_get(&cci);
}
static struct device_node *of_get_parent(struct device_node *n) { return n ? of_node_get(n->parent) : NULL; }
static bool of_node_name_eq(struct device_node *n, const char *v) { return n && !strcmp(n->name, v); }
static bool of_device_is_compatible(struct device_node *n, const char *v) { return n && !strcmp(n->compat, v); }
static bool of_machine_is_compatible(const char *v) { (void)v; return gemini; }
'''
tests = r'''
static void reset(void) {
    of_node_put(rails[0]); of_node_put(rails[1]); rails[0] = rails[1] = NULL;
    proc = (struct device_node){.name="BUCKA", .enabled=true, .parent=&group};
    sram = (struct device_node){.compat="mediatek,mt6797-cpu-sram", .enabled=true};
    group = (struct device_node){.name="regulators", .parent=&chip};
    chip = (struct device_node){.compat="dlg,da9214", .enabled=true};
    for (int i=0; i<2; i++) cpu[i] = (struct device_node){.enabled=true,
        .supply={NULL,&proc,NULL,&sram}, .count={0,1,0,1}};
    cci = (struct device_node){.enabled=true, .supply={&proc,NULL,&sram,NULL}, .count={1,0,1,0}};
}
int main(void) {
    reset();
    assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == 1);
    assert(mt6797_boot_barrier_supply(&sram,"mem-supply") == 1);
    assert(mt6797_boot_barrier_supply(&other,"cpu-supply") == -EINVAL);
    for (int user=0; user<3; user++) for (int rail=0; rail<2; rail++) {
        struct device_node *n = user == 2 ? &cci : &cpu[user];
        int p = rail*2 + (user != 2), alias = rail*2 + (user == 2);
        reset(); n->count[alias]=1; n->supply[alias]=n->supply[p];
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == 1);
        n->supply[alias]=&other;
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
        reset(); n->count[p]=0;
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
        reset(); n->count[p]=2;
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
        reset(); n->supply[p]=NULL;
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
        reset(); n->enabled=false;
        assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
    }
    reset(); proc.name="BUCKB";
    assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
    reset(); chip.compat="dlg,da9211";
    assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == -EINVAL);
    reset(); assert(mt6797_boot_barrier_supply(&proc,"cpu-supply") == 1);
    other=proc; other.refs=0;
    cpu[0].supply[1]=cpu[1].supply[1]=cci.supply[0]=&other;
    assert(mt6797_boot_barrier_supply(&other,"cpu-supply") == -ESTALE);
    gemini=false;
    assert(mt6797_boot_barrier_supply(NULL,"cpu-supply") == 0);
    reset();
    puts("PASS: actual topology functions; OF lookup/refcount mocks, no real OF tree");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = pathlib.Path(tmp) / 'test.c'
    exe = pathlib.Path(tmp) / 'test'
    src.write_text(prelude + s + tests)
    subprocess.run(['gcc', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
