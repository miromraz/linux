# Pixel 4a kernel build record, 2026-10-26

Status: **Image, DTBs and modules built and inspected; hardware boot not tested**.
This does not validate Debian packaging or a complete installation image.

The build ran in a disposable Debian sid container on `llumik`. No build or
test ran on `dev`.

| Input | Identity |
| --- | --- |
| Source commit | `7cee974cdd9e59dd937c14b71e6e11f4d9124acc` |
| Runtime baseline | `473d9fb95156d759f0082bbbfee2b791202ffb41` (the source commit above adds only documentation) |
| Compiler | `aarch64-linux-gnu-gcc-14 (Debian 14.4.0-2) 14.4.0` |
| Config | `sunfish_defconfig bengalos.config` |
| Final release | `7.1.0-sm7150` |
| Final module count | 764 |

The full initial build completed without compiler or device-tree warnings,
but produced `7.1.0-sm7150+`. `scripts/setlocalversion` appends `+` for an
untagged checkout unless `LOCALVERSION` is explicitly set, even with
`CONFIG_LOCALVERSION_AUTO=n`. The build was then rerun with `LOCALVERSION=`;
the Image was rebuilt and all 764 module files were checked with `modinfo`.
Every module's vermagic release matches the final release above.

The corrected commands are:

```sh
make O=../kernel-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 LOCALVERSION= sunfish_defconfig bengalos.config
make O=../kernel-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=aarch64-linux-gnu-gcc-14 LOCALVERSION= -j4 Image modules dtbs
```

Final output hashes:

| Output | Bytes | SHA256 |
| --- | --- | --- |
| `arch/arm64/boot/Image` | 38971904 | `9ed367748d01afa63ebabe6e359bbbe3bfbf02408a73decc6e636bfff824caf7` |
| `arch/arm64/boot/dts/qcom/sm7150-google-sunfish.dtb` | 101844 | `50cb3d4bd1ebba3ffb933d035a657a929d97f1d33ab35cd225d8dd0e4a304ace` |
| `.config` | 247816 | `c77e131c3248c4fa5087b0a5b34065441c720061cfe3cedc8848e1d3eb9fd0d0` |

Structural checks also confirmed the ARM64 Image magic, a valid DTB magic
and the `google,sunfish` compatible in that DTB. No compiler or device-tree
warnings were found in either build log.

The DTB also matches the available BengalOS 0.26.1024.2 image's embedded DTB.
That does not establish the source revision or module compatibility of that
image's kernel. The separate [installation audit](sunfish-installation.md)
documents why that image remains unsuitable for the recipient.

These hashes identify this build, not a promise that builds at other times
will be byte-identical. The package version and module release are reused
across source revisions; keep the full source SHA and matching artifacts
together. No runtime, suspend, radio, audio or camera validation is claimed
for these newly built binaries.
