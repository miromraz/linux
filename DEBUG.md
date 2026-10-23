# OBS packaging for integ/next15

Goal: a proper OBS package that builds the real-device kernel integ/next15
(tip bc72d6e5c0de) via scmsync, modelled on obs/gemini / linux-gemini-next12.

## Findings (2026-10-23)

- integ/next15 ALREADY carries the full real-device Debian packaging:
  `debian/{arch,changelog,control,copyright,rules,rules.vars}` are byte-identical
  (same blob SHAs) to obs/gemini, plus its own `arch/arm64/configs/gemini.config`
  (blob 5f9b111). Added by commit eda06f861c40 "deb-pkg: add Gemini PDA Debian
  packaging for OBS builds". So no packaging commit has to be invented — only a
  scmsync branch + OBS package pinned at the tip.
- debian/rules build-arch: `make defconfig gemini.config && make olddefconfig &&
  make headers all`; packaging via scripts/package/builddeb.
- DTB set: builddeb runs `make dtbs_install` (gated on CONFIG_OF_EARLY_FLATTREE +
  boot/dts dir; gemini.config has CONFIG_ARCH_MEDIATEK=y and the Makefile has
  `dtb-$(CONFIG_ARCH_MEDIATEK) += mt6797-gemini-pda.dtb`), so the Gemini DTB is
  built and installed into the linux-image .deb under
  /usr/lib/linux-image-7.1.3-mt6797/mediatek/. Verify on the built binaries.
- KERNELRELEASE 7.1.3-mt6797; package source name linux-gemini.

## Plan / actions

- branch obs/gemini-next15 = integ/next15 tip (bc72d6e5c0de) + this DEBUG.md log.
- OBS package home:realni/linux-gemini-next15, scmsync pinned to the pushed tip
  (SHA pin, matching linux-gemini-next12 convention).
- Trigger one build, poll `osc results` every ~10 min (timeout-guarded) until a
  terminal state. Fixups via fixup commits (max 2 retries), re-pin scmsync.

## Constraints honoured

- No local compiles / packing (scmsync server-side import only).
- No force-push of existing remote branches; obs/gemini-next15 is new.
- integ/next11 untouched.
- Assisted-by trailer only (no Signed-off-by / Co-Authored-By).
