# Protected CCI lifecycle — 2026-10-30

Base: `c384463d0d7383fb231b3e6dbd66f694dadd02bd`. Admission remains **CLOSED**.
This implements the missing production initializer and its devfreq lifetime;
it does not authorize real Gemini hardware operation. A NEW independent review
is required. No subagents, hardware, HP, device locks, flashing, playback,
capture, thermal implementation or A72 implementation were used.

Read the applicable instructions, initialized-chain prerequisite report in full,
owned/accounting implementation reports, owned independent review and accepted
c384 independent review. Existing reports are preserved and excluded from this
commit. History identifies the owned target in 9ec770fc34cb and outcome adapter
in 5162fe49e172; neither supplied the lifecycle. No model provenance is inferred.

## Production ownership and lifetime

`mtk_ccifreq_owned_init()` allocates real driver state in a devres group, acquires
its own registry user-2 lease, gets CCI/intermediate clocks, enables the CCI clock,
and retains the registry's existing filtered OPP table. It does not recreate the
OPP table or obtain legacy regulator handles. The registry already owns the real
regulator requests, PLL reference/claims, supported-hardware configuration and
OPP notifications. Duplicating legacy voltage programming or the legacy OPP
notifier here would violate that ownership contract.

The initializer reads the actual CCI rate and executes the owned transaction at
that rate before publishing drvdata. This supplies a real initial outcome, not
prefilled fixture statistics. It then registers a per-device outcome profile
with its observed initial frequency and real CPU-parent passive governor. START
may invoke the owned target before `drv->devfreq` is assigned; that target only
needs already-initialized driver state and its lease. Initializer errors retain
the original errno. No CPU policy/device link is added: protected CPU token
initialization precedes the legacy CCI-bound check, and CCI can defer until CPU
policies exist without creating a reverse binding dependency.

The normal Gemini probe has an explicit, permanently false operational-admission
predicate and a branch to this initializer. Its closed path still acquires and
returns protection and returns EAGAIN, preserving existing closed-gate tests.
There is no module parameter, DT property, or exported production gate override.
The non-Gemini regulator/profile path is unchanged.

Removal invokes real devfreq STOP without holding the driver callback mutex.
CPU notifier unregister drains before parent list destruction. The new optional
`devfreq_dev_profile.release_done` completion closes a second lifetime boundary:
final device release may be delayed after `devfreq_remove_device()`. It signals
only after all profile accesses, QoS notifier removal, OPP-table put, and devfreq
free; early add failures that never register a device signal it as well. The
initializer/remover waits before returning the lease or releasing parent devres.
The core now initializes SRCU before device registration, so registration or
statistics-allocation errors cannot clean up an uninitialized SRCU head. This
was a related static error-unwind finding in the old core; allocation failure
at every internal core allocation was not injected here.

Unwind and remove return registry ownership before disabling/dropping local
clock handles and releasing the devres group. When physical return fails, the
registry consumes the cookie and durably retains its vote, consumer identity,
clock/OPP resources and recovery responsibility, as in the existing release
contract. The local driver does not pretend that return succeeded or attempt
unsafe rollback. This path assumes serialized, sleepable driver lifecycle;
recursive removal from the driver's own notification stack is not supported.
An external holder that never releases its devfreq device reference can delay
unbind indefinitely; retaining protection is intentional in that case.

## Exact KUnit entrypoints for the subsequent full-chain tester

Declarations are in `drivers/regulator/mt6797-combined-test.h`, enabled only by
`CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST`:

* `mt6797_test_cci_init(pdev, 0)` calls the actual production initializer despite
  the production gate. The device must have no drvdata, be registry user 2,
  have live registered providers/CCF clocks and OPPs, and have `cci` and
  `intermediate` clock lookups. Real published CPU policies/CPU OPP tables are
  required by passive START. No lease, outcome, devfreq or statistics is supplied
  by the caller. A successful call leaves drvdata and devres owned by the driver.
* `mt6797_test_cci_devfreq(pdev)` returns the registered devfreq object for
  observation/listeners while the caller retains outer lifecycle exclusion.
  Use `get_device(&df->dev)`/`put_device()` for a deliberately retained device
  reference; a borrowed pointer alone is not an unbind pin.
* `mt6797_test_cci_fini(pdev)` calls production remove, drains and releases the
  initialized object and its resource group, and clears drvdata. Call once after
  successful init, outside CPU notifier callbacks. Failed init already unwinds.
* Failure stages 1–6 inject EIO after lease, clock lookups, clock enable,
  OPP-table reference, initial owned transaction, and successful devfreq START,
  respectively. Stage 7 selects an invalid timer to exercise actual core profile
  rejection. Stage 8 selects a missing governor; the exact negative errno comes
  from the module loader. Stage 9 injects the driver's fault latch before START,
  causing the actual owned target to reject START. These are failure injections,
  not claims that synthetic latch state arose from a physical failure.
* `mt6797_test_cci_callback_hook()` is a test-only observation hook inside the
  passive CPU notifier after the child mutex is dropped. `mt6797_test_srcu_hook()`
  observes unregister after unlink and immediately before/after synchronize_srcu.
  Tests filter the exact notifier, install hooks before starting tasks, join all
  tasks before clearing hooks, and must not run competing hook users.

The older `mt6797_test_cci_targets()` and synthetic lease-only remove helper
remain for their existing bounded tests. They are not the initialized entrypoints.

## Executed lifecycle coverage

The existing real registry/regulator/CCF/provider fixture now invokes the new
initializer without borrowing its token-test lease. `mt6797_test_cci_policies()`
provides published CPU0/CPU4 framework policies and real CPU OPP tables for this
bounded fixture, then restores them. The registered CCI profile, governor,
devfreq statistics, clocks and owned transaction are all production objects.

Inside `transition_clock_contract`, new assertions cover:

* Nine injected failure stages, original applicable errno, cleared drvdata,
  no active CCI lease, restored device references/CCI enable count, and a real
  returned regulator guard rather than a surviving consumer vote.
* Missing policies causing deferred START, followed by successful initialization.
* Actual notifier-registration EBUSY while CPUFreq fast switching is enabled,
  followed by disable and successful retry. No replacement notifier function.
* Successful real START and CPU0 transition completion through CPUFreq POST,
  passive aggregation, devfreq core and the owned CCI target. The resulting
  rate is 300 and selected persistent vote is 1.08 V in this fixture; the
  intermediate requirement exceeds the low endpoint's 1.04 V.
* Controlled teardown: pause the actual CCI passive callback inside its SRCU
  reader after dropping the child mutex. Start remove on another task and wait
  for the unregister hook immediately before synchronize_srcu. Assert the child
  mutex is available, synchronization/removal have not completed and the lease
  remains active. Release the callback and assert callback completion precedes
  synchronization completion. This does not use a sleep duration to infer drain.
* Keep an extra reference to the real devfreq device across STOP. After SRCU
  synchronization completes, assert removal has not completed and the lease is
  still active. Drop that reference, join both tasks, then check cleared drvdata,
  returned regulator guard, restored device references and clock enable count.

Completion waits are bounded (2/5 seconds), with an external 60-second VM bound.
Thread idle sleeps merely keep task handles available until kthread_stop; they
are not the ordering oracle. This closes the earlier child-mutex/timing ambiguity
for this initialized callback schedule. It does not prove every START publication
interleaving or arbitrary concurrent governor lifecycle operation.

## Evidence and limits

All builds, executions and checkpatch ran on llumik. Local activity was source
editing, Git operations and artifact transfer. Evidence is in outer
`artifacts/cpufreq-phase3-protected-cci-lifecycle-20261030/`, with full binaries
retained remotely at `llumik:/home/u/gpda-cci-lifecycle-20261030/binaries/`.
The source is `/dev/shm/gpda-cci-tests-5162/src`; outputs reuse the prior ARM64/UML
and separate KASAN trees. These are source-verified incremental builds, not a
clean-build reproducibility claim. Source verification checks 93,788 unchanged
blobs against c384 and hashes nine changed source/header files. Binary/config/DTB
manifests and runner scripts identify the actual inputs and outputs.

Development runs exposed an older extraction fixture's reliance on the first
textual `if (of_machine_is_compatible` location: an early probe condition caused
an empty allocation slice and uninitialized drv in generated tests. The final
probe condition avoids that ambiguous marker; the existing extraction fixture
runs again. Hardening that extractor to reject empty slices remains focused
follow-up. A missing scheduler frequency reference in the new synthetic policy
fixture caused a warning and was corrected. A missing-governor test initially
expected ENOENT; the real module loader returned ENOSYS. The final test accepts
the loader's negative errno and separately checks complete unwind. None of these
failed development runs is counted as passing evidence.

Full CPU-owned transition -> initialized CCI integration remains the subsequent
tester's job: this increment uses real CPU transition completion with fixture
policies, not a CPU token driving the new CCI object. CPU4-driven targets, two
later listeners, rounded/partial/unknown initialized outcomes, raw failure and
provider-removal interleavings, stale-cookie rebind and OPP mutation need that
full-chain matrix. Initializer allocation failures at every allocation site and
all exact OPP/CCF internal refcounts are not exhaustively fault-injected. Existing
combined OF/NVMEM supplier tests remain separate from the new initialized fixture.
Thermal containment, A72/buckB, hardware and physical audio acceptance remain
pending. Independent review of this new lifecycle/core-completion delta is next;
the accepted c384 fixes are not being reclassified by this report.

### Final bounded results

* W=1 ARM64 Image, UML and KASAN ARM64 Image builds succeed. Final normal/UML
  incremental logs contain no compiler warnings/errors. KASAN retains two
  frame-size warnings in existing `ownership_run` and `cpu_callback_case`
  fixtures (2112/2336 bytes); no warning-free KASAN build is claimed.
* ARM64 provider: 23/23 cases; outcome/status: 4/4 cases in two suites; seven
  combined OF configurations: each 1/1. New lifecycle assertions are inside an
  existing provider case; these totals are not counts of new lifecycle cases.
* UML: seven suites, 38/38 cases. Its configuration does not enable the combined
  CCI initializer hooks; UML is framework/provider regression evidence here.
* KASAN_GENERIC with lockdep: provider 23/23 and outcome/status 4/4, including
  initialized failure/unwind, CPU POST, SRCU drain and delayed device release.
* All final runtime logs reach shutdown with complete passing KTAP and no
  searched not-ok, BUG, WARNING, KASAN, circular-lock or internal-error markers.
  `runtime-summary.txt` records the exact per-log checks.
* Strict code checkpatch: zero errors, warnings or checks, 685 lines checked.
  The complete staged patch additionally produces the ordinary new-document
  MAINTAINERS advisory; no maintainer assignment is changed by this report.

Final binary SHA256:

* ARM64 Image: `9d74494c4cced9fdeb5842af7d69fede6f861af99b7239d73ec4a277155288f0`
* UML: `e55f99d0313f7b4c938ba2f58ee466863aa21908c165c1c9ac73cf2f3673edd5`
* KASAN ARM64 Image: `f03e772c31cdfc3acfed43f2f0167e71bbcbb24ff153dfcd9a16f67ccb97291c`
