# CPU token task: incomplete implementation, 2026-10-30

Base: ed6dbcab3744126bc0e18ea122eff9b2c5fc8e95. **The requested operational
token and actual CPU0/CPU4 callback task is NOT complete.** This change fixes
only the independently reviewable OPP removal and failed-acquisition cleanup
issues. No precise contract blocker to the operational task was established.
Do not accept this as the requested CPU implementation or open admission.

Read the full reservation-independent review, reserved-CPU implementation
report, transaction-contract review, and applicable /home/realni/AGENTS.md.
No subagents, hardware, HP, locks on devices, flashing, playback, capture or
userspace workaround. Builds, tests and checkpatch ran exclusively on llumik.
No DCO or hardware acceptance is asserted. Unrelated changes were preserved.

## Implemented source changes

- `dev_pm_opp_remove` marks the selected entry removed under the table mutex,
  before releasing its owner reference. Repeated removal cannot consume a
  retained reader's reference or the corresponding dynamic table reference.
- `_opp_get_next` claims removal under that same mutex. `_opp_remove_all`
  no longer marks outside the lock, so competing single/all removals cannot
  independently claim the same owner reference.
- Both removal paths clear a matching `suspend_opp` pointer under the mutex.
  `dev_pm_opp_get_suspend_opp_freq` reads that pointer under the same mutex.
- `_opp_table_find_key`, `_opp_table_find_opp_key`, `_get_opp_count`,
  `dev_pm_opp_snapshot_lock` and maximum-voltage-latency enumeration exclude
  retained removed entries. CPUFreq table construction uses count and ceiling
  lookup, so a newly constructed table no longer republishes these entries.
- `_opp_set_availability` and `dev_pm_opp_adjust_voltage` cannot mutate removed
  entries. `_opp_is_duplicate` rejects re-addition while the removed entry is
  retained with -EEXIST, instead of reporting an existing live duplicate that
  the addition path would translate to success.
- `dev_pm_opp_requirements_get` validates all selections under the table mutex
  before taking any entry references. Failed partial selection thus cannot
  drop a final temporary reference and synchronously emit REMOVE on a caller's
  driver-lock stack. Successful reservations and their final puts retain the
  existing contract: copied accepted values survive logical removal, and final
  notification-producing puts must run outside notifier-sensitive locks.

The new `requirements_single_remove` real OPP-core KUnit case covers accepted
A surviving single removal, repeated removal, new B rejection, ordinary lookup
and count rejection, stale mutation/re-add rejection, legacy snapshot rejection,
failed multi-rate acquisition, subsequent remove-all, and exactly two deferred
REMOVE notifications. Those callbacks perform OPP count lookup to check that
the table mutex is not held during delivery.

History inspection identifies the single-removal behavior in the inherited
8cd9520d35a6 Linux import; ed6dbcab3744 added the requirements reservation that
makes the retained-entry case relevant here. No model provenance is inferred.
The stale-selection defect was source-confirmed; no baseline failing binary
was executed. The new fixed-source regression passed at runtime.

## Exact executed evidence

Remote: llumik:/home/u/gpda-phase3-20261030/token-*.
Local: outer workspace artifacts/cpufreq-phase3-cpu-token-20261030/.
`token-code.patch` is the tested code delta. `token-source.sha256` identifies
both changed source files. `token-binary.sha256` records binaries and configs;
`token-dtb.sha256` records the QEMU matrix DTBs. These are incremental builds
in the retained source/output trees, not a clean full-tree reproducibility
attestation. The selected-source manifest does not hash inherited sources.

Commands inside gpda-kbuild:trixie, with the remote directory mounted at /work:

- `make ARCH=um O=/work/uml W=1 -j8`
- `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/work/arm64 W=1 -j8 Image`
- `TMPDIR=/tmp timeout 60 ./uml/linux kunit.enable=1 kunit_shutdown=halt`
  from /work, with Docker seccomp unconfined.
- `scripts/checkpatch.pl --no-signoff /work/token-code.patch`

Both W=1 builds passed with no compiler warning/error matches. UML passed
34 cases across five suites, including 23 provider cases and the new removal
case. No failures/skips or warning/BUG/lockdep-pattern matches were observed.
The first UML launch failed before boot because /dev/shm is noexec; setting
TMPDIR=/tmp allowed the successful run. Its final log replaces that failed
launch log; this is an environment retry, not a passing first attempt.

`token-matrix.sh`, run in gpda-combined:trixie, booted seven existing eight-CPU
QEMU cases with external 45-second timeouts (0:0, 1:0, 0:1 through 0:5). All
passed and powered down; no runner warning/BUG/lockdep-pattern matches were
found. Checkpatch: zero errors, zero warnings. These existing combined tests
are regressions only; they do not invoke a new operational CPU callback.

UML binary SHA256:
6e357be5bb9def2990c7c9102e7eb760fa06e9952e420b2eb5b58c922ff287d5

ARM64 Image SHA256:
fbd2f8fe3ae5d87138191d9b64d3659ff1605cb714906ed9a3ff3a4116c72f4c

## Required work still absent

`mt6797_consumer_txn_begin/prepare/switch/finish` do not exist. The production
Gemini initialization early return and `mtk_cpufreq_set_target` are unchanged.
The round-before-park helper hazard remains; no callback ordering fix or new
callback matrix is claimed. Operational copied-requirement lease writes,
scoped clock-event authorization, conservative owned preparation/final votes,
truthful structured outcomes, cancellation/final-reduction serialization,
finishing exclusion, and real CPU callback tests remain required together.

The fixed-table/configuration prerequisite contract remains in force. This
change does not provide general dynamic OPP reconfiguration, refresh previously
published CPUFreq tables, or provide immediate logical-removal notification.
REMOVE still occurs on final put. CPUFreq accounting, thermal containment, CCI
operational integration, arbitrary clock-provider unload, and A72/buckB remain
separate gates. Existing non-Gemini CCI preparation-rejection retry and
committed-OPP-notification test gaps remain open. A NEW independent review is
required; this partial commit is not operational acceptance.
