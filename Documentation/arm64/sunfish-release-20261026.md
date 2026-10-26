# Packaged sunfish release validation, 2026-10-26

Status: image and headers packages, generic/handset U-Boot and slotflip EFI
payloads built on `llumik`. **No phone deployment or hardware test:** the user
deferred phone access. This is not a complete recipient-ready system image.

## Kernel packages

Tested packaging source: `d33103b46cbbf81f7f7e448c0653d653b8538610`.
Runtime baseline: `473d9fb95156d759f0082bbbfee2b791202ffb41`.
The release/share documentation added afterwards does not change these sources.
Target and host compiler major: GCC 14 (Debian 14.4.0).
Package version: `7.1.0-sm7150-20261026.3+git473d9fb95156`.
Kernel/module release remains `7.1.0-sm7150`.

| Package | Bytes | SHA256 |
| --- | --- | --- |
| `linux-image-7.1.0-sm7150` | 23166064 | `854caf0497a27bbf022b54df102d0d68158cee9957e5a8e45fd8e9cf75b75666` |
| `linux-headers-7.1.0-sm7150` | 9199808 | `22546c43771c3ecee3b9937e14eea2781de63fd565ca877f5cc5a1e10d4dff35` |

Validation passed:

- Full Debian cross-build, including headers. Both packages are arm64 and
  carry the new version, which sorts newer than `7.1.0-sm7150-1`.
- Embedded source/compiler provenance matches the tested source and compilers.
- Packaged config and DTB match their recorded hashes; the packaged ARM64 EFI
  kernel matches the build output.
- All 764 installed modules have the expected vermagic release.
- All six ELF helper programs in the headers package target ARM64.
- Wrong GCC majors and a wrong target architecture are rejected before
  compiling. Gitless OBS metadata is accepted; malformed metadata is rejected.

The full package test exposed two additional cross-header issues, now fixed:
the helper rebuild selected unversioned GCC, and target libc development
headers were undeclared. `KDEB_HEADER_CC` now supplies the pinned compiler
without changing the generic packager's default for other users.

The successful package invocation emitted one dpkg-architecture warning
because its initial environment still selected the host's generic CC before
debian/rules selected the pinned target compiler. The actual kernel and header
tools were verified as ARM64/GCC 14. The README's invocation sets CC before
dpkg-buildpackage to avoid that diagnostic. The kernel and DT compiler logs
contain no warnings.

## U-Boot and Android helper

Tested U-Boot source: `74ef2f79e147bd728a409a9fbe500d44b18cb49a`.
Both variants used GCC 14 for target and host tools, and mkbootimg pinned to
`17cea80bd5af64e45cdf9e263cad7555030e0e86`.

| Variant | SHA256 |
| --- | --- |
| Generic, stock `userdata` lookup | `cd84cbaa9dc6a2acf9574e94e9d797c3ef32b405dcca1ff85de34b795cf9c897` |
| Opt-in maintainer handset, custom `linux` lookup | `0fa4065cbbde822260a19520e9a31da234f85f7b479fd86c9a2cc73f057dab94` |

Each image is 622592 bytes. Compiled default environments were extracted and
compared with their respective source files. The generic variant has no
Android menu entry. Both Android boot-image headers and appended DTBs passed
structural checks. Each build retains the same three host utility
const-qualification warnings recorded in the earlier U-Boot build.

The new helper was built off-phone from the matching packaged kernel source
and trusted Debian ARM64 runtime packages. Its canonical locations are
`EFI/sunfish/slotflip.efi` and `EFI/sunfish/slotflip-test.efi`; matching ESP-root
copies support the old flashed menu during migration.

| Payload | SHA256 |
| --- | --- |
| Android B transition | `7f790a87d3f1e7e65f1c0106d797e7647c81b360cb4bcbb36fde7efd3bce9017` |
| Probe, no boot-control writes | `be0415e7e63c591c1eca36e8ed001dd8f12eabeed41785db0fc247b138bd95dc` |

Each payload is 42120192 bytes. ARM64 PE headers, embedded kernel hashes,
distinct `slotflip=b` / `slotflip=test` command lines and compatibility copies
were verified. Eight mocked-device policy/initramfs tests passed. The helper
manifest records kernel/DTB/stub/runtime hashes; the runtime package versions
and checksums are kept with the local build artifacts.

## Still required

- Confirm that live OBS has synchronized the aligned Git branch and published
  the new package version; source-metadata write access is needed to force a
  stale scmsync refresh.
- Once the phone is online, save its boot images and EFI state, install the
  helper, and test its no-write probe before the Android transition.
- Test boot, return/recovery, and hardware with matching kernel/modules.
- Build a separate sanitized recipient system image. The old development
  image still contains the maintainer's access/account configuration.

The old GitHub OBS integration targets `linux-sunfish` on
`sunfish-venus-v7.2`. Its workflow/webhook returns do not establish a refresh
of `linux-sm7150`. The prior Git OBS head was `50629265abff`; the public OBS
export inspected during this work was older still, `c20b2d53b9b6`.
OBS credential requests timed out, so no live metadata refresh or successful
native OBS build/publication is claimed by this record. The new version was
built and inspected on `llumik`.
