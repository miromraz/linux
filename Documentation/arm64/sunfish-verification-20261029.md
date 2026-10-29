# Sunfish handset verification, 2026-10-29

The llumik-built release kernel boots on the maintainer's Pixel 4a with matching
modules. Nineteen automated checks passed before and after a successful RTC-driven
s2idle suspend/resume. Automatic return to the preserved September system was
also demonstrated after failed trial boots. This validates the existing custom
handset layout; it does not validate a fresh stock-userdata installation.

## Tested kernel identity

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
from the compiler command name. Its bytes differ from the llumik build, and the
OBS binary itself has not been boot-tested in this run. Its embedded
`source_commit` is null; source identity was checked separately in OBS's
`_scmsync.obsinfo`. Treat that missing embedded identity as a packaging follow-up.

Sources: [OBS source identity](https://api.opensuse.org/public/source/home:realni/linux-sm7150/_scmsync.obsinfo),
[build status](https://api.opensuse.org/public/build/home:realni/_result?package=linux-sm7150),
[published repository](https://download.opensuse.org/repositories/home:/realni/Debian_13/).

A sanitized recipient image, its first boot and stock-userdata recovery procedure
remain outstanding. See [installation status](sunfish-installation.md).
