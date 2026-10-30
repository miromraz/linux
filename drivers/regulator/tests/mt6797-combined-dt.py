# SPDX-License-Identifier: GPL-2.0-only
"""Append synthetic Gemini supplies to QEMU virt's dumped DTS (eight CPUs)."""
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text()
text += '\n/ { compatible = "test,mt6797-combined", "planet,gemini-pda"; test-order = <0>;\n'
for i in range(6):
    text += f'c{i}: test-clock-{i} {{ #clock-cells = <0>; }};\n'
text += '''test-efuse { #address-cells = <1>; #size-cells = <1>;
    bin: speedbin@0 { reg = <0 1>; }; };
test-buck { compatible = "dlg,da9214"; regulators {
    proc: BUCKA { regulator-name = "synthetic-proc"; }; }; };
test-infra { compatible = "mediatek,mt6797-infracfg";
    sram: sram { compatible = "mediatek,mt6797-cpu-sram"; }; };
'''
for i in range(3):
    text += f'''opp{i}: opp-table-{i} {{ compatible = "operating-points-v2";
    opp-shared;
    opp-100 {{ opp-hz = /bits/ 64 <100>; opp-microvolt = <{1060000 + 20000*i}>; opp-supported-hw = <1>; }};
    opp-200 {{ opp-hz = /bits/ 64 <200>; opp-microvolt = <{1060000 + 20000*i}>; opp-supported-hw = <1>; }};
    }};\n'''
text += '''test-cci { compatible = "mediatek,mt6797-cci";
    operating-points-v2 = <&opp2>; nvmem-cells = <&bin>;
    nvmem-cell-names = "speed_grade"; clocks = <&c4>, <&c5>;
    clock-names = "cci", "intermediate";
    proc-supply = <&proc>; sram-supply = <&sram>; };
};\n'''
for i in range(8):
    g = i // 4
    text += f'''&{{/cpus/cpu@{i:x}}} {{
    operating-points-v2 = <&opp{g}>; nvmem-cells = <&bin>;
    nvmem-cell-names = "speed_grade"; clocks = <&c{2*g}>, <&c{2*g+1}>;
    clock-names = "cpu", "intermediate";
    cpu-supply = <&proc>; sram-supply = <&sram>;
}};\n'''
pathlib.Path(sys.argv[2]).write_text(text)
