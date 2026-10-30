# SPDX-License-Identifier: GPL-2.0-only
"""Extract protected callback bodies verbatim; fail closed on source drift.

The generated unit runs against real kernel frameworks. Only architecture/OF
CPU discovery is injected by the fixture. No ownership algorithm is copied.
"""
import pathlib
import sys

cpu = pathlib.Path(sys.argv[1]).read_text()
cci = pathlib.Path(sys.argv[2]).read_text()


def block(source, marker):
    start = source.index(marker) + len(marker)
    assert source[start - 1] == "{"
    depth = 1
    end = start
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end - 1]


print("/* SPDX-License-Identifier: GPL-2.0-only */")
print("/* Generated from production sources. Do not edit. */")
print("static int direct_cpu_init(struct direct_cpu *info, int cpu) {")
print("int ret;")
print(block(cpu, 'if (cpu < 8 && of_machine_is_compatible("planet,gemini-pda")) {'))
print("}")
print("static void direct_cpu_release(struct direct_cpu *info) {")
release = cpu[cpu.index("static void mtk_cpu_dvfs_info_release("):]
print("if (info->rail_cookie) {")
print(block(release, "if (info->rail_cookie) {"))
print("}\n}")
print("static void mtk_ccifreq_release_protection(struct mtk_ccifreq_drv *drv) {")
print(block(cci, "static void mtk_ccifreq_release_protection(struct mtk_ccifreq_drv *drv)\n{"))
print("}")
probe = cci[cci.index("static int mtk_ccifreq_probe("):]
print("static int direct_cci_probe(struct platform_device *pdev) {")
print("struct device *dev = &pdev->dev; struct mtk_ccifreq_drv *drv; int ret;")
print(probe[probe.index('\tdrv = devm_kzalloc'):probe.index('\tif (of_machine_is_compatible')])
print(block(probe, 'if (of_machine_is_compatible("planet,gemini-pda")) {'))
print("}")
remove = cci[cci.index("static void mtk_ccifreq_remove("):]
print("static void direct_cci_remove(struct platform_device *pdev) {")
print("struct mtk_ccifreq_drv *drv = platform_get_drvdata(pdev);")
print("if (drv->rail_cookie) {")
print(block(remove, "if (drv->rail_cookie) {"))
print("}\n}")
