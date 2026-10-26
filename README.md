# Linux for the Google Pixel 4a (sunfish)

Developer handoff, 2026-10-26. This is a work-in-progress SM7150 kernel port,
not a complete phone distribution or an installer.

For installation on another phone, see the
[stock-layout installation status and preparation guide](Documentation/arm64/sunfish-installation.md).
The target is an unlocked Pixel 4a with permission to erase Android userdata.
No recipient-ready system image is published by this branch.

## Start here

```sh
git clone --single-branch --branch sunfish-share https://github.com/miromraz/linux.git
cd linux
```

This branch adds handoff documentation to `release/sunfish-0.26.1024.1`,
commit `473d9fb95156d759f0082bbbfee2b791202ffb41`. It does not change kernel
behavior. The source version is **Linux 7.1.0**. Its base is
[`sm7150-mainline/linux`, branch `v7.2`](https://github.com/sm7150-mainline/linux/tree/v7.2),
commit `fd78d17954a86ea6ceb0c535e618ea129e09f8dd`; that branch name is the
port's revision, not the Linux version.

| Branch | Purpose |
| --- | --- |
| `sunfish-share` | Documented starting point for reviewing or developing this port. |
| `release/sunfish-0.26.1024.1` | Frozen source snapshot used as this branch's baseline. |
| `obs/sm7150` | OBS packaging branch; as checked on 2026-10-26, five fixes behind the release snapshot. |
| `bengalos/sm7150` | Older distribution integration branch. |
| `sunfish-venus-v7.2`, `ctrl-nofollow`, other `fix/*` branches | Historical development and experiments; not the current handoff. |

The GitHub default branch may still be an older branch. Use the explicit
branch above rather than relying on a plain clone.

## Hardware status

These entries summarize previous hardware validation and the latest project
notes (2026-10-24). No new on-device validation was performed for this
documentation handoff, and the newest release fixes are not all confirmed
to be deployed on the phone.

| Feature | Recorded state |
| --- | --- |
| Display and touchscreen | Working; includes FTS5 and panel power-management fixes. |
| Wi-Fi, Bluetooth, NFC | Working with the corresponding firmware and userspace. |
| Speakers, headset, built-in microphone | Working with the matching UCM/userspace configuration; audio power management still needs work. |
| Sensors | Working with the device's sensor registry and compatible hexagonrpcd. |
| Battery and charging | Working, including 9 V charging support; recent fixes address SMB5 float voltage and reporting. Battery `current_now` is not a usable power measurement. |
| Cameras | Capture has been demonstrated on this port; depends on matching libcamera support/tuning. Camera quality and driver cleanup remain ongoing. |
| Video decode | H.264, HEVC, VP8 and VP9 verified, including cold boot; MPEG2 disabled. |
| Video encode | Unresolved SoC reset when an encoder session starts. The distribution blacklists `venus_enc`; that policy is outside this kernel repo. |
| Suspend | s2idle works, but full AOSS deep-sleep residency is not established. ADSP/LPASS votes and userspace audio activity remain blockers. |
| Cellular and GPS | Modem control services are reachable, but RF initialization fails. Working cellular service and GPS fixes are not established. |
| Fingerprint | Driver and device-tree support included; end-to-end operation is not established by the handoff evidence. |

## Build

Use **GCC 14**. Previous project builds with GCC 12 did not boot on this
device; the cause is unresolved. The Debian build dependency currently
names an unversioned cross compiler, so installing dependencies alone does
not guarantee the required compiler version.

For an arm64 cross build on a Linux build machine, install a GCC 14
`aarch64-linux-gnu` toolchain plus the standard kernel dependencies
(`make`, `bc`, `bison`, `flex`, OpenSSL/ELF development headers, Python 3,
and the dependencies described in the upstream build documentation).
Then, from the repository root:

```sh
aarch64-linux-gnu-gcc-14 --version
make O=../sunfish-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 LOCALVERSION= sunfish_defconfig bengalos.config
make O=../sunfish-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 LOCALVERSION= -j"$(nproc)" Image modules dtbs
```

Relevant outputs:

- `../sunfish-build/arch/arm64/boot/Image`
- `../sunfish-build/arch/arm64/boot/dts/qcom/sm7150-google-sunfish.dtb`
- Kernel modules, which must be installed for the **same kernel build**.

Keep `LOCALVERSION=` on subsequent build and module-install commands too.
Despite `CONFIG_LOCALVERSION_AUTO=n`, an untagged Git checkout otherwise adds
`+` to the release string. The explicit empty value keeps it at
`7.1.0-sm7150`, matching the packaging convention; that name alone still
does not identify a unique source build.

The in-tree `debian/` directory supports OBS package builds. It merges
`sunfish_defconfig` and `bengalos.config` and emits packages for
`7.1.0-sm7150`. Package versioning is not yet a unique identifier for each
source revision: keep the full Git SHA with every build artifact.

A fresh GCC 14 build completed on `llumik` on 2026-10-26: Image, DTBs and all
764 configured modules, with no compiler or DT warnings. The initial build
exposed the `LOCALVERSION=` requirement above; after correcting it, the Image
was rebuilt and all module release strings checked against `7.1.0-sm7150`.
See the [build record](Documentation/arm64/sunfish-build-20261026.md).
No hardware boot test or recipient system-image build was performed. In the
maintainer's environment, builds and test suites run on `llumik`, never on `dev`.

## Boot and userspace

The companion bootloader is the
[SM7150 Tauchgang U-Boot fork](https://github.com/sm7150-mainline/u-boot/tree/tauchgang),
baseline `f8c04469e8284aa2f1691a30aa3b7dc59fa0b8cd`.
The phone uses ABL to launch U-Boot, then an EFI/UKI Linux boot path.

Firmware is not included. The port uses device-matching Qualcomm and Google
firmware extracted from Android, including DSP, modem, GPU and video firmware,
plus audio firmware/tuning. A kernel build alone does not provide the working
userspace, UCM profiles, sensor services, camera stack or firmware layout.

The maintainer's handset has a **custom partition layout**: Linux is in a
partition named `linux`, separate from Android `userdata`. That is not a
stock Pixel 4a layout. Do not copy partition numbers or raw disk-write
commands from another handset. This repository does not provide a verified
installation procedure for a stock phone.

## Review priorities

1. Resolve branch selection and align the OBS source with the intended release;
   retain the release snapshot as a rollback point.
2. Verify the five release-only lens and charger fixes on the target image.
3. Make GCC 14 selection and source-revision identification explicit in the
   package build, then validate with a full build on the build machine.
4. Address LPASS runtime power management together with the userspace audio
   pins; validate audio and suspend together before claiming deep sleep.
5. Diagnose encoder startup separately from the working decoder. The old
   claim that encoding necessarily requires CVP is not established.
6. Continue modem RF diagnosis before adding another GPS userspace path.

The history contains experiments, reverts and follow-up fixes. This branch
preserves that history and attribution. It is a development handoff, not
a cleaned, subsystem-separated upstream submission series.

For general kernel documentation, see [Documentation](Documentation/) and
the original upstream [README](README).
