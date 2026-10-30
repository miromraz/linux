# CPU owned path: missing reservation contract, 2026-10-30

Status: NOT IMPLEMENTED. Phase3 admission remains CLOSED.

Inspected base `2033381315bdc30e2038dd6e1bc39e80df782a87`. This report is
not a CPU implementation, test result, or acceptance claim. No source changes,
hardware operations, builds, tests, or lint were performed. No new CPU callback
matrix or source/binary identity exists for this task. Existing untracked review
documents were left untouched. No DCO or hardware acceptance is asserted.

Read the owned-transactions report, independent clock-acceptance review, and
outer Group4 acceptance review. Inspected history including `d50f92317fb8`
(bind-time reservations), `0de3dbd9acfb` (persistent core requests),
`445db88db285` (retained-owner recovery), and `2033381315bd` (clock rejection).
These establish lifetime protection, not a transaction-wide operational API.

## Exact missing contract

The existing APIs cannot express an owned operational transaction that both
excludes OPP/provider mutation and permits synchronous framework notification.
This is a source-level contract finding, not a reproduced runtime failure.
Implementing a new reservation contract is necessary; this is not evidence that
such an extension is impossible or that the user prohibited core changes.

* `include/linux/regulator/mt6797.h` exports only
  `mt6797_consumer_acquire()`, `mt6797_consumer_release()`, and
  `mt6797_consumer_recover()`. There is no operational begin/update/end or
  reservation token. The regulator handles in `protected_supply` are private
  to `drivers/regulator/mt6797-provider.c`. A cookie does not expose a vote.
* `preflight_transaction()` reserves clock handles and OPP snapshots for a
  bounded raw rail operation. Before `lease_transfer()` enters regulator core,
  it explicitly releases every OPP lock and clock reservation. `lease_write()`
  reacquires them after PRE notification. This correctly avoids holding OPP
  locks through regulator notification, but supplies no exclusion spanning
  preparation, park, PLL programming, reconnect, and final vote reduction.
* `dev_pm_opp_snapshot_lock()` in `drivers/opp/core.c` acquires the actual
  table mutex, retaining it until `dev_pm_opp_snapshot_unlock()`. A synchronous
  CCF notifier calling a frequency lookup reaches `_opp_table_find_key()`,
  which locks that same nonrecursive mutex. Holding the snapshot through the
  CCF operation therefore permits self-deadlock. This is the precise lock
  dependency; no deliberately deadlocking VM was run.
* Releasing the snapshot is not equivalent exclusion:
  `dev_pm_opp_adjust_voltage()` commits supplies under the table mutex,
  unlocks, then notifies. A concurrent mutation can commit before the registry
  observes an invalidation. The notifier cannot veto or roll back that commit.
  Taking a second snapshot only detects some races; it does not close the
  interval between validation and a clock write.
* `preflight_clock_event()` unconditionally advances `preflight.generation`
  through `preflight_invalidate()`. An owned clock change has no token that
  distinguishes it from an unrelated change. Reusing the preparation snapshot
  after a delivered clock event fails the generation comparison in
  `preflight_transaction()`. Suppressing every event from the current task
  would also suppress unrelated mutations made by synchronous callbacks.
* Holding `transaction_lock` alone does not exclude supplier removal:
  `detach()` takes `owner_lock` and drains individual raw operations through
  `mt6797_rail_detach()`, without taking `transaction_lock`. Conversely,
  holding `owner_lock` across CCF calls self-deadlocks on
  `preflight_clock_event()`. Provider lifetime exclusion must participate in
  the new reservation contract without blocking recursively in notification.

These paths rule out simply wrapping the current CPU clock helper in the
existing Group4 locks. They do not justify silently weakening exclusion to
post-operation generation checks.

## Ownership and callback integration still required

`mtk_cpu_dvfs_info_init()` returns immediately after acquire for Gemini CPU0/4.
It does not initialize the operational clocks, regulator handles, or reg_lock
on that branch. `mtk_cpufreq_set_target()` still uses the legacy voltage path;
the `rail_cookie` condition at the clock helper is not an owned transaction.
The production admission check remains unchanged.

The new registry operation must validate the cookie and shared group identity,
retain stable PLL identity, and snapshot old/intermediate/target requirements.
It must distinguish the currently live requirements from the future preparation
request: `mt6797_rail_replace()` rejects an already undervolted live requirement,
so treating a higher future target as already live incorrectly rejects an upshift.
The operation must preserve all peer floors, including CCI, at every raw step.

`regulator_set_voltage_unlocked()` restores the caller's old request after
balance failure. It also skips the setter for an identical request. Neither
case proves electrical rollback or a freshly validated floor. The existing
`lease_transfer()` explicitly calls `lease_write()` after core operations to
cover no-op validation. An operational extension needs equivalent validation
and truthful guard/vote bookkeeping, including a physically committed failure.

`mt6797_consumer_recover()` rejects any active consumer with EBUSY. It cannot
serve as recovery for an active CPU transaction. Clock-provider uncertainty
must retain protection and an unknown/terminal-needed outcome unless a bounded
recovery can verify the actual resulting path under the provider contract.
CCF cached topology alone cannot resolve commit-then-error mux programming.

## Required implementation and evidence, not executed cases

First provide a mutation reservation that permits OPP reads during synchronous
notification, rejects or defers OPP writers before mutation, distinguishes
authorized clock operations from recursive unrelated changes, and pins provider
lifetime. It must cover all relevant mutation entry points, not only voltage
adjustment. Then integrate the registry-owned requests directly into the
CPU0/CPU4 target branch with an explicit hardware outcome structure and an
explicit KUnit-only invocation behind closed operational admission.

The requested success up/down, intermediate-above-endpoints, preparation/parent/
PLL/reconnect/voltage-down failure, restored failed request, peer floor,
OPP/generation invalidation, rejection/retry, and both shared-group cases remain
UNEXECUTED. They must exercise the compiled callback with real CCF, OPP and
regulator core, checking every write and the final model hardware outcome.
The prior 31-case UML and Group4 matrices are not evidence for this missing path.
W=1, checkpatch, bounded UML/QEMU execution and source/binary manifests must run
on llumik after implementation, never on dev.

CCI target conversion remains a later task, but its existing retained floor
must participate in the CPU reservation. CPUFreq outcome accounting, passive
CCI notification behavior, thermal containment and A72/buckB synchronization
remain separate closed gates. No additional CCI cleanup was performed.
