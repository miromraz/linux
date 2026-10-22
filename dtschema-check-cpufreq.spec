# dt_binding_check (schema validation) for the MT6797 cpufreq-series bindings.
# dtschema's dt-doc-validate is the meta-schema validator that
# 'make dt_binding_check' runs per binding; this package runs it over the
# three DT schema files the up/cpufreq-mt6797 series touches, on an openSUSE
# Tumbleweed build root (Debian:13 has no python3-dtschema; OBS roots are
# offline so pip is unavailable).
Name:           dtschema-check-cpufreq
Version:        1
Release:        0
Summary:        dt-doc-validate over the MT6797 cpufreq-series DT bindings
License:        GPL-2.0-only
BuildArch:      noarch

# python3-dtschema is Provided by python313-dtschema (home:Jianhua:devel-tools),
# which pulls jsonschema / ruamel.yaml / pylibfdt / rfc3987 as rpm deps.
BuildRequires:  python3-dtschema
BuildRequires:  dtc

Source0:        mediatek,mt6797-mcumixed.yaml
Source1:        mediatek,cci.yaml
Source2:        mediatek,mt6797-cpu-sram.yaml
# The kernel's regulator core schema, only so mt6797-cpu-sram's
# "$ref: regulator.yaml#" resolves in this standalone harness.
Source3:        regulator.core.yaml

%description
Build-time dt-doc-validate of the three DT schema files touched by the MT6797
cpufreq series. Fails the build if the checker reports any issue; the checker
log tail is visible in the build log and the full log ships in the package.

%prep
# nothing to unpack; the schema files are used directly from SOURCES

%build
set -o pipefail
echo "=== dtschema version ==="
dt-validate --version 2>&1 || true
: > dtcheck.log
rc=0
# dt-doc-validate derives the schema base path from each file's $id, so the
# files live under a bindings/ tree whose layout matches the $id
# (schemas/<subdir>/<name>), and -u maps the schemas URL prefix to it so
# "$ref: regulator.yaml#" resolves to the kernel core schema shipped here.
mkdir -p bindings/clock bindings/interconnect bindings/regulator
cp "%{SOURCE0}" bindings/clock/
cp "%{SOURCE1}" bindings/interconnect/
cp "%{SOURCE2}" bindings/regulator/
cp "%{SOURCE3}" bindings/regulator/regulator.yaml
for rel in "clock/mediatek,mt6797-mcumixed.yaml" \
           "interconnect/mediatek,cci.yaml" \
           "regulator/mediatek,mt6797-cpu-sram.yaml"; do
    echo "########## dt-doc-validate $rel ##########" | tee -a dtcheck.log
    if ! dt-doc-validate -u bindings "bindings/$rel" >>dtcheck.log 2>&1; then
        rc=1
    fi
done
echo "=================== dt-doc-validate log ==================="
cat dtcheck.log
echo "==========================================================="
# A clean dt-doc-validate run prints nothing per file; any output is a finding.
if [ -s dtcheck.log ] && grep -Eiv '^#+ dt-doc-validate ' dtcheck.log | grep -Eq '[^[:space:]]'; then
    echo "### dt-doc-validate reported issues on the series bindings -- failing build"
    rc=1
fi
if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "### dt-doc-validate clean for all three series bindings"

%install
install -D -m0644 dtcheck.log %{buildroot}%{_docdir}/%{name}/dtcheck.log

%files
%dir %{_docdir}/%{name}
%{_docdir}/%{name}/dtcheck.log

%changelog
* Fri Oct 23 2026 Miroslav Mraz <mraz.miro@seznam.cz> - 1-0
- dt-doc-validate CI for the MT6797 cpufreq-series DT bindings.
