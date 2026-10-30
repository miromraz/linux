#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Source premises and explicit VMA/file/devres lifetime model; llumik only.

This does not execute Linux file references, IRQ races, or driver-core removal.
Production release/free callbacks are exercised in audio-ownership.py.
"""
import pathlib

root = pathlib.Path(__file__).resolve().parents[5]


def read(path):
    return (root / path).read_text()


pcm = read('sound/soc/mediatek/mt6797/mt6797-afe-pcm.c')
native = read('sound/core/pcm_native.c')
core = read('sound/core/init.c')
mmio = read('drivers/base/regmap/regmap-mmio.c')
platform = read('sound/soc/mediatek/common/mtk-afe-platform-driver.c')
assert 'devm_regmap_init_mmio(' in pcm
assert 'regmap_attach_dev' not in pcm and 'cache_only' not in pcm
regmap_config = pcm.split('mt6797_afe_regmap_config = {')[1].split('};')[0]
assert '.reg_stride = 4' in regmap_config and 'cache_type' not in regmap_config
assert 'ctx->clk = ERR_PTR(-ENODEV)' in mmio
probe = pcm.split('static int mt6797_afe_pcm_dev_probe')[1]
assert probe.index('pm_runtime_resume_and_get') < probe.index('devm_add_action_or_reset')
assert probe.index('devm_add_action_or_reset') < probe.index('devm_request_irq')
assert probe.index('devm_request_irq') < probe.index('devm_snd_soc_register_component')
assert 'snd_pcm_set_managed_buffer_all' in platform
assert 'atomic_read(&substream->mmap_count)' in native
assert 'atomic_inc(&substream->mmap_count)' in native
assert 'atomic_dec(&substream->mmap_count)' in native
assert 'snd_card_disconnect_sync(card->snd_card)' in read('sound/soc/soc-core.c')
assert 'list_empty(&card->files_list)' in core

# File/VMA references are an explicit model premise, not a new allocator ABI.
# Closing the fd with an extant mapping does not invoke final file release.
for mappings in (0, 1, 3):
    refs = 1 + mappings
    pm_refs = 1  # driver lifetime get before component exposure
    component = irq = memory = True
    events = ['probe_get', 'component_exposed', 'disconnect_stop']
    refs -= 1
    if refs:
        assert component and irq and memory and pm_refs == 1
        events.append('disconnect_waits_for_vma')
    while refs:
        refs -= 1
        events.append('unmap')
    events += ['release_stop', 'hw_free_stop_readback', 'irq_sync', 'managed_free']
    memory = False
    events += ['close_unpublish', 'irq_sync', 'slot_release', 'file_list_empty']
    component = False
    events.append('component_unregister')
    irq = False
    events.append('irq_devres')
    pm_refs -= 1
    events.append('power_down_put')
    assert not (memory or component or irq or pm_refs)
    assert events.index('hw_free_stop_readback') < events.index('managed_free')
    assert events.index('file_list_empty') < events.index('component_unregister')
    print('MODEL:', ' -> '.join(events))
print('PASS: source provider/managed allocation/PM/devres premises; explicit file/VMA removal model (not kernel concurrency validation)')
