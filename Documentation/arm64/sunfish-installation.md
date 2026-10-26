# Installing on another Pixel 4a: preparation and release status

Updated 2026-10-26. Target: **Google Pixel 4a, codename `sunfish`, unlocked
bootloader, Android userdata expendable**. Pixel 4a 5G is a different target.

## Current status

The public `sunfish-share` kernel and U-Boot branches are developer source
handoffs. There is **no verified recipient-ready installation image** attached
to them. This document records the intended installation path, read-only
preparation, and the checks needed before providing device-write commands.

The current maintainer handset boots Linux from a custom `linux` partition.
That demonstrates the port on that handset, but does not validate installation
on a stock-layout phone. Its partition numbers, sizes, boot-slot policy and
Android slot-flip helper must not be copied to the recipient.

## Storage and boot plan

Use the stock outer `userdata` partition for the Linux disk image. The generic
U-Boot environment in the shared baseline already looks up that partition by
name and exposes its nested GPT through `blkmap`. No outer GPT repartitioning
or custom `linux` partition is part of this plan.

The Linux image is a disk image with its own partition table, not an Android
ext4 userdata filesystem. Its nested GPT needs **4096-byte logical sectors**,
matching the phone's UFS storage. It contains the ESP/EFI boot payload and
system partitions; first-boot root/home creation must be checked against the
available userdata capacity.

Writing it to userdata destroys the Android data there. Preserving Android's
system partitions does not make this a validated dual-boot installation:
Android expects its own userdata filesystem. The recipient has chosen the
wipe-data path; coexistence and returning to Android require separate work.

The boot chain is ABL -> the Pixel-specific U-Boot Android boot image -> an
ARM64 EFI/UKI payload containing the matching Linux kernel, initrd and DTB.
The exact boot-slot choice and persistence steps must be established during
the first installation test; do not overwrite both boot slots by default.

## Read-only preparation on the recipient's phone

Back up anything needed from Android before proceeding. An unlocked
bootloader does not mean the data has already been backed up. A later
factory-image restoration also erases data; see
[Google's factory image documentation](https://developers.google.com/android/images).
Bootloader unlocking itself performs a factory reset according to
[AOSP's bootloader documentation](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking).
No unlock operation is needed if this phone is already unlocked.

Use a recent Android platform-tools installation on the host. If Android is
running and adb access is authorized, collect:

```sh
adb shell getprop ro.product.device
adb shell getprop ro.build.fingerprint
adb shell getprop ro.boot.slot_suffix
adb shell getprop ro.boot.flash.locked
```

When the phone is already in ABL fastboot mode, collect:

```sh
fastboot getvar product
fastboot getvar current-slot
fastboot getvar unlocked
fastboot getvar partition-size:userdata
```

Expected product: `sunfish`. Confirm unlocked status on the bootloader screen
as well. A rejected or unsupported variable is not proof that a requirement
is satisfied. With multiple connected phones, use platform-tools' explicit
device selector before running any commands. Keep serial numbers and other
device identifiers out of public bug reports.

Record the Android build and current slot with the test report. The source
handoff does not yet establish a required firmware build for a fresh stock
installation. Before touching boot storage, obtain the correct stock recovery
files and demonstrate that the host can communicate with this phone in ABL.
Do not relock the bootloader while custom boot images are installed.

## Recipient image blockers found in the existing tooling

These are source-review findings; the presence of a profile in source is not
proof of the exact contents of a particular built image. The following
section separately records checks of the available image.

| Finding | Evidence in the existing BengalOS port | Required resolution |
| --- | --- | --- |
| Maintainer root SSH access | `debug-usbnet` embeds the maintainer's public key and installs it into root's authorized keys; its README says never to ship it. The local build wrapper explicitly includes this profile. | Exclude the maintainer's key. If USB SSH is required, provision only the recipient's chosen key and document the access policy. |
| Development account in the generic device profile | `device-sm7150` ships `seed-user.sh`, its service and a static wants symlink; it creates a fixed development account with a known PIN, bypassing the first-boot wizard. | Remove development-account seeding from the recipient profile and verify account creation on a fresh root filesystem. |
| System-image inputs not fully identified | The old system image lacks a complete source identity. New kernel packages now have unique versions, GCC 14 enforcement and embedded provenance. | Rebuild the system image from recorded recipe/package inputs and verify live OBS synchronization. |
| Existing flash helper is handset-specific | `sunfish-ums.sh` asserts the maintainer's custom partition 15, label `linux` and exact size. | Do not use it on a stock recipient phone. Validate a stock-userdata path separately. |
| Firmware/userspace are separate from these repos | Kernel and U-Boot alone do not provide device firmware, UCM, sensor services, camera stack or a root filesystem. | Supply a reproducible distribution recipe and a documented source for each dependency. |

The recipe revision inspected for these findings is
`sm7150` at `9aea084` in the maintainer's BengalOS recipes checkout. Some
build paths and source changes exist only locally. A third party cannot yet
rebuild the complete system image from the two shared repositories alone.

## Available image audit, 2026-10-26

The local candidate `BengalOS_0.26.1024.2.img` was copied to `llumik` and
inspected in a disposable container. It is **not approved for distribution to
the recipient**. Its SHA256 is:

```text
ff94ba4ed11473adef1e52be0e4df30b7f4340bf26b87c5f12a17f86d6822ecd
```

Recorded checks:

- Android sparse conversion completed; the expanded disk image is
  4911255552 bytes. `sfdisk --sector-size 4096 --verify` reported no GPT errors.
- The ESP is readable. The UKI's PE machine is `0xaa64` (ARM64), despite the
  filename ending in `_x86-64.efi`. Its DTB contains `google,sunfish`; its
  embedded release is `7.1.0-sm7150`, matching the module-directory name.
  Matching names alone do not establish that modules and kernel came from
  the same source build.
- The UKI uses `root=dissect` and `mount.usr=dissect`; no embedded
  `root=UUID=` was found. Its initrd is 325897872 bytes.
- Full EROFS file-decoding/integrity checking with `fsck.erofs --extract`
  passed. This does not validate first boot, dm-verity policy or hardware.
- **Confirmed blocker:** the image contains a nonempty maintainer debug
  authorized-keys file and the USB SSH setup script.
- **Confirmed blocker:** it contains the fixed development-account seeding
  script, service and static enablement symlink.
- `/usr/etc/machine-id` is empty, suitable for first-boot generation; it is
  not a copied nonempty handset identity.
- The full kernel and recipe source revisions of this artifact have not been
  established. No hardware boot test of this image was performed in this audit.

These content checks confirm the development-image blockers independently
of the source review. The image needs to be rebuilt from sanitized, recorded
inputs; a checksum does not make the current image recipient-ready.

## Checks before an image can be offered for installation

1. Build the selected source revisions on the build machine. Pin GCC 14 for
   the kernel and external U-Boot packaging tools. Give the artifacts a unique
   source/version identity and publish SHA256 checksums.
2. Inspect the **built image**, not just its recipes: ARM64 UKI, matching
   kernel/DTB/modules, 4 KiB nested GPT, expected partition capacity, usable
   first-boot access and no maintainer SSH key or development account.
   Machine ID and SSH host keys must be generated per installation. Check
   firmware licensing and avoid shipping one handset's modem EFS or other
   personal calibration/state as another handset's data.
3. Verify the U-Boot Android boot-image header and device tree for `sunfish`.
   Establish a temporary-boot test and recovery path on the target before
   choosing how to install it persistently.
4. Verify the target identity, unlocked state, current slot and userdata size.
   Only then provide write commands for the exact tested artifacts and layout.
5. Test first boot and a clean reboot, EFI discovery, initial account creation,
   display/touch, charging, Wi-Fi, SSH if offered, and recovery to ABL. Record
   which audio, camera and suspend checks actually pass on this handset.
6. Give the recipient the known-limits table from the kernel README, especially
   the unresolved cellular/GPS, video-encoding and deep-sleep behavior.

Passing image-content checks is not the same as booting on hardware. Until
the device test is recorded, any install attempt is a supervised porting test,
not a verified installation procedure.
