# Sunfish handset verification, 2026-10-29

Both the llumik-built release and the published OBS kernel boot on the
maintainer's physical Pixel 4a with matching modules. The llumik build passed
nineteen automated checks; the OBS build passed those checks plus verification of
all 764 module hashes. Both passed again after RTC-driven s2idle suspend/resume.
Automatic return to the preserved September system was also demonstrated after
failed trial boots. This validates the existing custom handset layout; it does
not validate a fresh stock-userdata installation.

## Tested llumik kernel identity

| Input | Identity |
| --- | --- |
| Compiled source | `d33103b46cbbf81f7f7e448c0653d653b8538610` |
| Runtime baseline | `473d9fb95156d759f0082bbbfee2b791202ffb41` |
| Package version | `7.1.0-sm7150-20261026.3+git473d9fb95156` |
| Target compiler | GCC 14.4.0, Debian `14.4.0-2` |
| Linker | GNU binutils `2.47.50.20261021` |
| Live GNU build ID | `753fc8b4eeeab8299a697a02418f501f86b5195d` |
| Image package SHA256 | `854caf0497a27bbf022b54df102d0d68158cee9957e5a8e45fd8e9cf75b75666` |

The build ID was read from `/sys/kernel/notes`, and the installed module package's
source/compiler provenance was checked. The unchanged `uname -r` value
`7.1.0-sm7150` alone would also match the older working kernel.

## Hardware results

- Wi-Fi SSH and USB SSH both worked on the release kernel and after resume.
- Bluetooth was powered with a valid controller address. Accelerometer availability
  was reported by SensorProxy. ADSP, CDSP and modem remote processors were running.
- Both cameras captured three 640x480 frames, discarded rather than saved.
- Venus hardware H.264 decoding reached EOS on a synthetic test stream. The
  decoder module was loaded and the encoder remained excluded by userspace policy.
- The display connector was connected; touchscreen, NFC, haptics, audio card and
  both speaker amplifiers had their expected drivers/devices. NFC polling services
  were active. These checks do not establish physical touchscreen or NFC tag use.
- A short low-amplitude speaker playback and a 150 ms haptics command completed.
  A one-second mono microphone capture returned 96000 bytes, discarded without
  saving audio. Perceived sound/vibration quality was not assessed remotely.
- A 15-second RTC wake test completed one s2idle suspend/resume, with unchanged
  boot ID, a success-count increment and no suspend failure. All nineteen checks,
  including both cameras and H.264 decoding, passed again afterward. This does
  not establish AOSS deep-sleep residency or idle battery life.
- No Oops, BUG, call trace or CPU-warning matches appeared in the checked kernel
  logs. The detached-initramfs cleanup service completed successfully.

The existing no-SIM modem setup failure, Wi-Fi invalid-frequency warnings and
uncalibrated camera-profile warnings were already present in the baseline.
Cellular service, GPS, video encoding, fingerprint authentication, 9 V charging,
focus calibration and full deep sleep are not certified by this run.

## Boot-image defect found and corrected

The initial trial cloned the working `/usr` filesystem into an inactive system
slot and changed its GPT label. Its root directory still carried the old
`user.validatefs.gpt_label` extended attribute. Persistent initrd logging showed
the release kernel mounting EROFS successfully, followed by `systemd-validatefs`
rejecting the new label. The subsequent reboot was an image-validation failure,
not evidence of a kernel panic or GCC regression.

Correcting that constraint and using the matching partition label and kernel
command-line filter allowed the **same** kernel/modules to boot. The repaired
filesystem passed full off-phone EROFS checking, an on-device hash check and
`systemd-validatefs` against a read-only mount before the successful boot.

When cloning an image for another slot, preserve its other validation constraints
but update `user.validatefs.gpt_label` deliberately. A checksum and successful
mount do not replace this validation. The earlier full-module initrd also changed
more than necessary; the successful diagnostic used the working initrd's 36 early
module names, resolved against the release package. An independent failure caused
by the larger module set was not established.

The diagnostic filesystem retains the maintainer's userspace and access settings.
It is a private handset test artifact, not an installation image to distribute.

## Recovery and U-Boot

The handset U-Boot image from source
`74ef2f79e147bd728a409a9fbe500d44b18cb49a`, SHA256
`0fa4065cbbde822260a19520e9a31da234f85f7b479fd86c9a2cc73f057dab94`,
was RAM-launched through ABL fastboot and booted the preserved Linux system.
Neither installed boot partition was flashed. The generic userdata profile was
built and inspected, but has not been boot-tested on a stock-layout recipient.

Boot partitions, boot-control metadata and EFI configuration were backed up before
testing. USB mass-storage recovery through the existing installed U-Boot restored
the working entry after the earlier loop. Later trials returned automatically.

EFI variable writes are unavailable on this handset. An explicit `default` pattern
kept selecting a counted entry even at zero attempts. The successful unattended
procedure instead used inspected automatic ordering, with no default/preferred
override: the trial was selected at one attempt, and the preserved working entry
at zero. Both states were checked before reboot, then actual fallback was observed.
The installed EFI loader was also exercised in an ARM VM on llumik with a failing
counted EFI application and successful next-boot fallback. A boot counter cannot
force a hung kernel to reset. The normal working default is restored after trials.

Both new slot-flip helpers were installed at their canonical `EFI/sunfish/` paths
and as identical ESP-root compatibility copies, after backing up the old copies.
The no-write probe, SHA256
`be0415e7e63c591c1eca36e8ed001dd8f12eabeed41785db0fc247b138bd95dc`, was selected
through a one-attempt EFI entry and returned automatically to the September
system. Boot-control metadata and both installed boot partitions retained their
pre-probe hashes. The probe's console policy message was not captured; this is
boot/return and no-persistent-write evidence, not an Android transition test.
The physical U-Boot Android menu selection and Android-side return policy remain
unverified. No Android slot switch was performed during these tests.

## Live OBS publication

On 2026-10-29, OBS source revision 13 exported
`9f9186def0ae3cbc3ba6ac60f3217eed15b58066` from `obs/sm7150`.
`Debian_13/aarch64` reported succeeded/published, and the public repository listed
both image and headers at `7.1.0-sm7150-20261026.3+git473d9fb95156`.

| OBS package | SHA256 |
| --- | --- |
| Image | `f6b6d84f3a9c925692286c43e857e2ea67ab382327cc33adfc327e67c5d2e132` |
| Headers | `05c72990f97c727574d98bfdfe5adafc91c4e0398bbc1f55dc9928a247eb57eb` |

The downloaded image package matched its published hash and reported GCC 14.2.0
for target and host. Its configuration matches the working September kernel apart
from the compiler command name. Its bytes differ from the llumik build. Its embedded
`source_commit` is null; source identity was checked separately in OBS's
`_scmsync.obsinfo`. Treat that missing embedded identity as a packaging follow-up.

### Published OBS package tested on the physical phone

The actual downloaded OBS package was then booted unattended on the Pixel 4a,
using its kernel, DTB and matching modules. The running kernel's GNU build ID was
read from `/sys/kernel/notes`; all 764 installed module files matched the hashes
of the extracted OBS package before and after suspend. This distinguishes the
OBS trial from both the September fallback and the separate llumik build, which
share the same `uname -r`.

| OBS trial input | Identity |
| --- | --- |
| Live GNU build ID | `8df9168e7075ff26c7c8d1fb9e14f1948a7bf4dd` |
| Target and host compiler | GCC 14.2.0, Debian `14.2.0-19` |
| Linker | GNU binutils `2.44` |
| Trial UKI SHA256 | `7b37e1570d9d9c47921f7baed5ddb54ff527ef158a654d42ed55089aff9ab51f` |
| Private trial `/usr` SHA256 | `66748b6e93801a5dd09b7f72b834018dd6cf60ca7636bae234e41c7192ea0737` |

Twenty checks passed before and after a 15-second RTC/s2idle cycle: kernel and
package identity, all module hashes, crash-log scan, ADSP/CDSP/modem state,
Wi-Fi address, Bluetooth power, accelerometer availability, display connection,
touchscreen binding, haptics device, NFC services, audio card/amplifiers, both
cameras, decoder-only policy and actual H.264 hardware decoding. USB and Wi-Fi
SSH worked before and after resume. Suspend success increased from zero to one,
failure stayed at zero, and the boot ID did not change. Speaker playback and a
150 ms haptics command completed. The same remote-verification limits listed above
apply; driver presence is not a physical touchscreen, NFC-tag or sound-quality test.

Microphone capture produced the expected 96000 bytes for one second of mono
48 kHz signed 16-bit audio, discarded without saving. However, `pw-record
--sample-count 48000` returned status 1 after reaching that count. The same
command reproduced the same byte count and status on the preserved September
system. Track this capture-tool termination behavior separately; this is data-flow
evidence, not a clean-exit or sound-quality certification.

The OBS and September boots also both logged a missing `regulatory.db`, a failed
Phosh first-boot importer, a Podman startup failure and a transient busy DRM
atomic commit. The importer remained failed in the user service manager.
These reproduced baseline userspace/image findings need triage for a recipient
image; passing the selected hardware checks does not make them resolved.

The trial used the corrected filesystem-label constraint and the 36 early module
names from the known working initrd, resolved against the OBS package. Its
inactive partition passed full hash readback and read-only `systemd-validatefs`
before boot. The GPT layout, Android boot partitions and boot-control metadata
retained their recorded values/hashes. No Android slot switch was performed.
After the hardware tests, a normal reboot skipped the exhausted one-attempt OBS
entry and automatically returned to the preserved September system. The original
working default was restored and the completed trial UKI retained outside the boot
menu. The phone was left running that preserved system with Wi-Fi and USB SSH.

Sources: [OBS source identity](https://api.opensuse.org/public/source/home:realni/linux-sm7150/_scmsync.obsinfo),
[build status](https://api.opensuse.org/public/build/home:realni/_result?package=linux-sm7150),
[published repository](https://download.opensuse.org/repositories/home:/realni/Debian_13/).

A sanitized recipient image, its first boot and stock-userdata recovery procedure
remain outstanding. See [installation status](sunfish-installation.md).

## U-Boot installation and Android round trip, 2026-10-30

The handset U-Boot image (SHA256
`0fa4065cbbde822260a19520e9a31da234f85f7b479fd86c9a2cc73f057dab94`) was written to
`boot_a` through ABL fastboot, serial `16101JEC206108`. The previous `boot_a`
(SHA256 `c0881e1a308cf67d59c83e0564921a6890a326eb0bf97e17361b10ca21587c77`) was
saved first for rollback. `boot_b` and `misc` were not written and kept their hashes.
`systemctl reboot bootloader` does not reach ABL fastboot on this handset; entering
fastboot needed Power and Volume Down.

After the flash, `boot_a` holds the new kernel payload and image ID, but its header
kept the previous image's ramdisk size, OS version and command line. Nothing on the
phone writes `boot_a`, so the merge happened in the ABL/fastboot flash path. ABL
boots it and U-Boot ignores those fields. Compare the payload, not the whole
partition, when checking an installed image.

Linux booted through the installed U-Boot when the menu timed out to its default.
The Android entry was exercised without a keypress: a one-shot `BootNext` pointing
at the ESP-root copy of the slot-flip helper was written into U-Boot's
`ubootefi.var` from Linux, after backing the file up. That copy is identical to
`EFI/sunfish/slotflip.efi` (SHA256
`7f790a87d3f1e7e65f1c0106d797e7647c81b360cb4bcbb36fde7efd3bce9017`), which the menu
entry loads. U-Boot ran the helper, and LineageOS 23.2 (`BP4A.251205.006`) booted
on slot B; adb answered 75 s after the reboot. A Magisk module installed on that
Android system had already set slot A active again. `adb reboot` returned to the
preserved Linux system in 51 s with slot A active, both slots successful, `BootNext`
consumed and `boot_b`/`misc` unchanged.

The return depends on that maintainer-installed Android module; a stock Android
install keeps booting slot B, and fastboot `--set-active=a` is then needed. The
physical menu keypress itself was not tested; it runs the same load command.
