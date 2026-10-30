# Phase3 transition increment — 2026-10-30

## Status: incomplete implementation, admission CLOSED

This is an initial clock-acceptance/CCI-containment increment, **not the
requested completed CPU0/CPU4/CCI recovery and accounting implementation**.
It must not be used to enable Gemini CPUFreq or CCI publication. Independent
review is still required. A fault latch is not a thermal response or the final
recovery policy. No hardware, HP, device locks, flashing, playback or capture.
No subagents. All compilation, checkpatch and execution ran on llumik.

The exact accepted cumulative fixture patch was first committed as
`a581d4ad2b0d`. Before committing, its SHA256 was verified as
`546cd230d407795c2df344b936218ec348277b956f78097157658f3435ff5046`.
All 15 staged blobs matched live files, and all 93,686 entries of the accepted
source manifest matched. No unrelated report or invented sign-off was included.
Approved audio/MFD/keys/NVMEM commits and unrelated documents remain intact.

## Implemented behavior

`mtk_dvfs_switch_clock()` performs the normal CCF intermediate/PLL/final-parent
sequence without rollback writes. It verifies the intermediate parent and
rate, the PLL's accepted rounded rate, and the final parent and rate. It
rejects rounding above the requested target before touching clocks: callers
have not established a voltage requirement for that faster rate. Rounding down
uses the ordinary provider contract. This is not electrical readback.

CPU protected leases and non-firmware CPU platforms use that helper. The
unprotected firmware branch remains unchanged, including its existing tolerance
and unresolved synchronization semantics. In particular, no A72 boot rate is
invented. CCI uses the same helper and no longer blindly reparents to a possibly
failed PLL or reduces voltage to cached intermediate/old values after an error.
Its dynamic tracking error branches also no longer issue unchecked restoration
writes. Regulator core restores requests on failure, not physical rails.

CCI now contains an error by rejecting further requests and invalidating its
private cache. Its OPP notifier propagates voltage errors as notifier results.
This is deliberately only interim containment. Returning an error does NOT
make devfreq's previous-frequency accounting correct. The changed CCI error
policy also applies to other SoCs; successful operation still uses their
existing dynamic voltage tracker. This broader failure-policy change needs
independent review and platform regression coverage before adoption.

## Executed evidence and exact limits

Evidence: outer workspace `artifacts/cpufreq-phase3-transition-20261030/`.
Remote tree and binaries: `llumik:/home/u/gpda-phase3-20261030/`.
Source was copied from the accepted fixture snapshot, with the four code files
overlaid by rsync. Output trees were copied from accepted outputs; this is not
a clean-output reproducibility claim. Changing container mount paths caused a
broad rebuild, followed by final incremental consistency builds.

* W=1 ARM64 Image link and UML link passed using gpda-kbuild:trixie.
* Focused `mt6797-provider.transition_clock_contract` passed.
* Full UML boot passed 31 cases across the five existing suites.
* Seven normal ARM64 Group4 QEMU cases passed using gpda-combined:trixie.
* Checkpatch: zero errors, one generic new-file MAINTAINERS warning.
* Existing ARM64 sys.c/traps.c override-initializer warnings and the UML
  command-line null-format warning remain; final incremental logs are clean.

The new test executes the actual shared helper with real CCF providers. It
covers up/down, an intermediate rate above both endpoints, first mux rejection,
PLL provider rejection, final mux rejection, and later successful transitions
from the intermediate parent. The PLL case reproduces CCF consuming the
provider's set_rate error and verifies that rate acceptance prevents reconnecting
it. Faults model reject-without-commit providers. Commit-then-error mux hardware
outside CCF's cached-parent contract is not claimed supported. An initial test
setup lacked the mux determine_rate callback and failed registration; it was
corrected before final runs.

These are **not** tests of full CPUFreq/devfreq target callbacks, passive CCI
notifications, target regulator failures, operational OPP races or operational
peer requests. Existing Group4 tests cover protected ownership only. No new
KASAN operational matrix or hardware/electrical proof is claimed.

Selected source hashes and binary/config hashes are retained in source.sha256
and binary.sha256. UML linux SHA256:
`67fba37858aec1ec90f059cfb5a2a741a15648180f983758b8350ced2a2045b2`.
ARM64 Image SHA256:
`37804dac20bfe09b89df4571010f008a22c56635afab034cc7c9721f0b246f67`.

## Outstanding implementation blockers to admission

1. Protected initialization still deliberately returns before operational
   driver state/publication. Connect actual registry-owned votes to operational
   target requests; never substitute the aggregate voltage for owned requests.
   Gemini alone needs the validated fixed-SRAM path, without dynamic loops.
2. Refresh old/intermediate/target OPP requirements and exclude clock/OPP
   mutation across preparation and switching. Existing snapshot locking covers
   raw writes, not arbitrary notifier-producing clock calls. Holding OPP table
   mutexes through clock notifications risks recursive OPP deadlock; simply
   releasing them loses mutation exclusion. A transaction reservation contract
   with explicit notifier behavior is still needed.
3. Define bounded known-old, known-target, safe-intermediate and unknown
   outcomes with retained conservative votes and verified recovery. Neither
   the new CCI latch nor the existing CPU latch completes this requirement.
4. CPUFreq core emits fictional rollback notifications on target errors;
   devfreq core similarly posts the old frequency. Private zero caches do not
   fix either. Implement supported driver-owned CPUFreq notification and a
   truthful devfreq error outcome contract, with scheduler/statistics and
   passive-governor tests. Do not return fake success to avoid these paths.
5. Execute the requested full production target/regulator/core-notification
   matrix, including first-request rollback, partial writes, refreshed OPPs,
   peers and repeated recovered operations. Then obtain independent review.

Thermal terminal containment is the next separate scope once transitions and
accounting are complete. No global reboot hook was added. A72/buckB firmware
ownership, status and initial synchronization remain separate and unresolved.
