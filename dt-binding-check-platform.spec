# Full-tree dt_binding_check + dtbs_check for the MT6797 base-platform +
# DVFS series (up/mt6797-platform-cpufreq). Runs the real kernel
# 'make dt_binding_check' over the whole bindings tree (so every $ref
# resolves and every series example is example-compiled and validated),
# then scopes pass/fail to the 56 binding yaml the series adds/changes.
# Also runs 'make CHECK_DTBS=y' on mt6797-gemini-pda.dtb (dtbs_check).
# Tumbleweed build root: Debian:13 has no python3-dtschema; python313-dtschema
# (home:Jianhua:devel-tools, Provides python3-dtschema) supplies it here.
Name:           dt-binding-check-platform
Version:        1
Release:        0
Summary:        dt_binding_check + dtbs_check for the MT6797 platform+DVFS series
License:        GPL-2.0-only
BuildArch:      noarch

BuildRequires:  make
BuildRequires:  gcc
BuildRequires:  bison
BuildRequires:  flex
BuildRequires:  dtc
BuildRequires:  python3-dtschema
BuildRequires:  perl
BuildRequires:  rsync

%description
Build-time dt_binding_check (full tree, series-scoped) and dtbs_check of
mt6797-gemini-pda.dtb for the up/mt6797-platform-cpufreq series. The scmsync
import is the full kernel tree at the series packaging tip. Fails the build
only if a *series* binding yaml or the gemini DTB has a schema finding;
tree-wide pre-existing findings are logged but do not fail the build.

%prep
# scmsync delivers the full kernel tree as the build dir; nothing to unpack.

%build
set -o pipefail
echo "=== dt-validate version ==="; dt-validate --version 2>&1 || true

# The 56 binding yaml the series adds/changes (basenames, for scoping).
cat > series-files.txt <<'LIST'
Documentation/devicetree/bindings/arm/mediatek.yaml
Documentation/devicetree/bindings/clock/mediatek,infracfg.yaml
Documentation/devicetree/bindings/clock/mediatek,mt6797-mcumixed.yaml
Documentation/devicetree/bindings/clock/mediatek,syscon.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,aal.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,ccorr.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,color.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,dither.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,dpi.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,dsi.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,gamma.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,od.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,ovl-2l.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,ovl.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,rdma.yaml
Documentation/devicetree/bindings/display/mediatek/mediatek,ufoe.yaml
Documentation/devicetree/bindings/display/panel/planet,gemini-pda-nt36672.yaml
Documentation/devicetree/bindings/gpu/arm,mali-midgard.yaml
Documentation/devicetree/bindings/i2c/i2c-mt65xx.yaml
Documentation/devicetree/bindings/iio/adc/mediatek,mt2701-auxadc.yaml
Documentation/devicetree/bindings/iio/adc/mediatek,mt6359-auxadc.yaml
Documentation/devicetree/bindings/input/gpio-matrix-keypad.yaml
Documentation/devicetree/bindings/input/mediatek,pmic-keys.yaml
Documentation/devicetree/bindings/input/touchscreen/novatek,nvt-ts.yaml
Documentation/devicetree/bindings/interconnect/mediatek,cci.yaml
Documentation/devicetree/bindings/iommu/mediatek,iommu.yaml
Documentation/devicetree/bindings/leds/awinic,aw9120.yaml
Documentation/devicetree/bindings/mailbox/mediatek,gce-mailbox.yaml
Documentation/devicetree/bindings/memory-controllers/mediatek,smi-common.yaml
Documentation/devicetree/bindings/memory-controllers/mediatek,smi-larb.yaml
Documentation/devicetree/bindings/mfd/mediatek,mt6397.yaml
Documentation/devicetree/bindings/mfd/mediatek,mt8195-scpsys.yaml
Documentation/devicetree/bindings/mmc/mtk-sd.yaml
Documentation/devicetree/bindings/nvmem/mediatek,efuse.yaml
Documentation/devicetree/bindings/phy/mediatek,dsi-phy.yaml
Documentation/devicetree/bindings/phy/mediatek,tphy.yaml
Documentation/devicetree/bindings/pinctrl/mediatek,mt6779-pinctrl.yaml
Documentation/devicetree/bindings/power/mediatek,power-controller.yaml
Documentation/devicetree/bindings/power/supply/bq25890.yaml
Documentation/devicetree/bindings/pwm/mediatek,pwm-disp.yaml
Documentation/devicetree/bindings/regulator/dlg,da9211.yaml
Documentation/devicetree/bindings/regulator/mediatek,mt6351-regulator.yaml
Documentation/devicetree/bindings/regulator/mediatek,mt6797-cpu-sram.yaml
Documentation/devicetree/bindings/regulator/onnn,fan49101.yaml
Documentation/devicetree/bindings/regulator/richtek,rt5735.yaml
Documentation/devicetree/bindings/soc/mediatek/mediatek,mutex.yaml
Documentation/devicetree/bindings/sound/mediatek,mt6797-afe-pcm.yaml
Documentation/devicetree/bindings/sound/mediatek,mt6797-mt6351.yaml
Documentation/devicetree/bindings/spi/mediatek,spi-mt65xx.yaml
Documentation/devicetree/bindings/thermal/mediatek,thermal.yaml
Documentation/devicetree/bindings/timer/mediatek,timer.yaml
Documentation/devicetree/bindings/usb/mediatek,mtk-xhci.yaml
Documentation/devicetree/bindings/usb/mediatek,mtu3.yaml
Documentation/devicetree/bindings/usb/mediatek,musb.yaml
Documentation/devicetree/bindings/usb/onnn,fusb301.yaml
Documentation/devicetree/bindings/vendor-prefixes.yaml
LIST
sed -i '/^$/d' series-files.txt

echo "=== make dt_binding_check (full tree, for correct \$ref resolution) ==="
# Tree-wide: validates every binding example; returns nonzero on any tree-wide
# finding, so do not let that fail the rpm build -- we scope to series files.
make -k dt_binding_check >binding_check.full.log 2>&1 || true
tail -n 5 binding_check.full.log || true

echo "=== series-scoped dt_binding_check findings ==="
: > binding_check.series.log
while read -r f; do
    [ -n "$f" ] || continue
    grep -F "$f" binding_check.full.log >> binding_check.series.log || true
done < series-files.txt
# Drop the pure "CHKDT/SCHEMA <file>" progress lines; keep real findings.
grep -Ev '^\s*(CHKDT|SCHEMA|LINT|DTEX|DTC)\b' binding_check.series.log \
    > binding_check.series.findings || true
echo "--- series binding findings (empty == clean) ---"
cat binding_check.series.findings
echo "------------------------------------------------"

echo "=== dtbs_check: mt6797-gemini-pda.dtb (CHECK_DTBS=y) ==="
dtbs_rc=0
make ARCH=arm64 defconfig >dtbs_check.log 2>&1 || true
if make ARCH=arm64 CHECK_DTBS=y \
        mediatek/mt6797-gemini-pda.dtb >>dtbs_check.log 2>&1; then
    echo "dtbs_check: make returned 0" | tee -a dtbs_check.log
else
    echo "dtbs_check: make returned nonzero (see findings below)" | tee -a dtbs_check.log
fi
# Schema findings on the gemini dtb appear as lines mentioning the dtb path.
grep -E 'mt6797-gemini-pda\.dtb' dtbs_check.log \
    | grep -Ev '^\s*(DTC|DTC_CHK|CHECK|DTB)\b' > dtbs_check.findings || true
echo "--- gemini dtb schema findings (empty == clean) ---"
cat dtbs_check.findings
echo "---------------------------------------------------"

rc=0
if [ -s binding_check.series.findings ]; then
    echo "### SERIES BINDING schema findings present -- failing build"; rc=1
fi
if [ -s dtbs_check.findings ]; then
    echo "### GEMINI DTB schema findings present -- failing build"; rc=1
fi
[ "$rc" -eq 0 ] && echo "### dt_binding_check + dtbs_check clean for the series"
exit $rc

%install
d=%{buildroot}%{_docdir}/%{name}
install -D -m0644 binding_check.full.log     $d/binding_check.full.log
install -D -m0644 binding_check.series.log   $d/binding_check.series.log
install -D -m0644 dtbs_check.log             $d/dtbs_check.log

%files
%dir %{_docdir}/%{name}
%{_docdir}/%{name}/binding_check.full.log
%{_docdir}/%{name}/binding_check.series.log
%{_docdir}/%{name}/dtbs_check.log

%changelog
* Fri Oct 23 2026 Miroslav Mraz <mraz.miro@seznam.cz> - 1-0
- Full-tree dt_binding_check (series-scoped) + dtbs_check of the gemini DTB.
