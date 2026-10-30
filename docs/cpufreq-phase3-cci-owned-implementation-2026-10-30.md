# CCI owned target staging — 2026-10-30

Base: b188a477b6358e208d00bf6b8930bb962a48571d. First focused correction:
37852db5dc00. Admission remains CLOSED. This is a staged token/actual-target
implementation, NOT completion of CCI devfreq accounting, the requested full
failure matrix, or operational acceptance. A NEW independent review is next.
No subagents, physical hardware, HP, device locks, flashing, playback, capture,
or DCO trailers. All compilation, execution and checkpatch ran on llumik.
Unrelated commits and supplied untracked review documents were preserved.

## Passive correction

get_target_freq_with_cpufreq now immediately rejects a missing required policy;
later present or intentionally omitted CPUs cannot erase the error. A newly
present required policy without a subscribed parent record returns EAGAIN rather
than silently contributing nothing. Governor restart is required to collect that
new record; this is not an automatic policy-hotplug subscription implementation.
The aggregate also checks the live policy's cur_unknown. START imports unknown
state and does not seed historical policy.cur as a current observation. Existing
cleanup releases its notifier and OPP references when the initial target fails.
READ_ONCE imports the terminal flag; this increment does not redesign the older
START/list-publication concurrency protocol or introduce policy replay.

The real START/aggregate/STOP fixture now removes the earlier policy after START
with later required-present, intentionally omitted, and default-classifier CPUs.
It asserts failure with no child call and successful calculation after restoring
the policy. Removing a collected record with a policy still present asserts
EAGAIN. Tests deliver terminal, STOP, then START against cur_unknown and assert
no extra child target and empty cleanup. Fixture-authorized known state permits
fresh START; a terminal followed by a known same-rate POST retries the child.
This synthetic restoration is not production authorization to recover a CPU.
Two additional initial unknown START configurations reject with EIO. Existing
later-listener and original CPU-error assertions still run. The actual Gemini
OF policy classifier's requested additional focused parser matrix remains open;
injected classification is not represented as coverage of that parser.

## Actual CCI target and token

mtk_ccifreq_target dispatches protected cookies to mtk_ccifreq_owned_target.
This is production target code, exercised directly by the fixture, rather than
an independent replacement transaction implementation. The Gemini probe still
releases its acquired lease and returns EAGAIN before devfreq registration.
A complete admitted initialization/profile binding is deliberately NOT provided
by this staging commit: enabling this gate now would be incorrect.

The registry retains the CCI PLL in preflight_acquire, as it already did for
CPU0/CPU4; the synthetic discovery fixture follows the same rule. Token begin
now permits the three registered users, still requiring the matching live lease,
registered device, owned regulator vote, parent identity, converted rail state,
CCF claims and OPP reservations. A72/buckB is not added. Invalid user 3 remains
rejected. No caller-supplied regulator handle or voltage replaces registry votes.

The callback serializes with callback_lock, never reg_lock, across token begin,
prepare, switch and consuming finish/abort. Deferred OPP puts therefore cannot
reenter a held regulator mutex. Recursive target acquisition refuses rather than
waiting. Release takes callback_lock before consuming its lease. These boundaries
still require outer devfreq/device lifetime exclusion; arbitrary concurrent
unbind/free is not established by this staging fixture.

Old/intermediate/target votes, dynamic intermediate requirements, park-before-
round, parent/rate verification, real regulator request rollback ownership and
unknown retention reuse the reviewed token. The callback caches outcome.rate
(zero for unknown), retained ownership, original errno and retry eligibility;
a nonretryable failure latches dvfs_fault. Begin refusals leave the clock cache
unchanged and retain the preparation errno. No clock restoration, rail rollback,
recovery or ownership retirement is attempted on failure. The non-cookie target
and its legacy descriptor behavior are unchanged.

Inspection also found a shared-token coverage issue: a lower rounded frequency
need not imply a lower OPP voltage. txn_switch now checks the rounded frequency's
live ceiling voltage under the existing reservation before PLL programming and
rejects a voltage above the reserved target requirement. A lookup failure leaves
the already-verified intermediate outcome. This conservative check can refuse a
logically removed OPP despite retained reservation data; no availability claim
for every such removal/rounding combination is made. Existing CPU removal and
rounding regressions pass, but a dedicated non-monotonic rounded-voltage test is
still needed. This is a source-review finding, not a newly reproduced hardware
failure.

## Newly executed evidence, and its limits

On the existing real regulator/CCF/OPP provider fixture, operational_clock_token
now runs for CCI user 2 in addition to CPU0/CPU4. Its new CCI execution covers
first-parent veto and retry, up/down with intermediate above both endpoints,
reserved external setters and OPP writers, owned vote retention, and concurrent
lease release/provider drain while the token is active. These are TOKEN-level
CCI tests, not CCI callback lifetime tests.

Within that CCI setup, mt6797_test_cci_targets invokes the actual production
mtk_ccifreq_target three times with persistent callback state: vetoed up request,
successful retry up, and down. It asserts actual mux rates, retry/fault state,
retained voltage and the real regulator-core owned request. The intermediate is 300 versus endpoint rates
100/200, and the final down vote remains 1.08 V rather than the endpoint 1.04 V.
The helper borrows the fixture's existing lease and supplies its initial cache;
it does not validate a production CCI initializer.

Final evidence: outer artifacts/cpufreq-phase3-cci-owned-20261030/. Remote source
and incremental outputs: llumik:/home/u/gpda-phase3-20261030, gpda-combined:trixie.
W=1 ARM64 Image and UML builds pass; final logs contain no compiler warning/error
matches. ARM64 provider: 23 cases passing, with new CCI iterations inside the
existing transition_clock_contract case. Seven combined OF configurations pass.
UML: five suites, 34 cases passing; it executes the new user-2 token iteration,
but not the CONFIG_REGULATOR_MT6797_COMBINED_KUNIT_TEST CCI callback or passive
matrix. Existing counts are regression totals, not counts of new CCI cases.
No searched warning/BUG/not-ok/circular-lock/inconsistent-lock/internal-error
patterns in final runtime logs. Code checkpatch: zero errors/warnings.

cci-run.sh records QEMU commands with 60/45-second timeouts. UML uses TMPDIR=/tmp,
seccomp unconfined and a 60-second timeout. ARM64 retains PROVE_LOCKING and
DEBUG_LOCK_ALLOC. Source and binary/config/DTB manifests accompany the logs;
this identifies incremental artifacts, not a clean full-tree reproducible build.

* ARM64 Image: ca63ac675d1459e339102afd81b32da378333a0244d3c32379ee9c0e20317e62.
* UML linux: dcfc60fa0c722964383c39af5f87cd8733742f37b6fde3b0961ab526807bdb08.

Development failures exposed the missing retained CCI PLL and a missing mutex
initialization in the extraction fixture. Both were fixed before these final
runs. No passing result from the failed binaries is used as final evidence.

## Required next implementation before thermal acceptance

The full requested CCI operational scope remains unfinished. devfreq_set_target
still publishes old-rate POST on target errors; this commit does NOT claim that
CCI partial or unknown outcomes can safely use it. Implement an explicit opt-in
outcome/terminal contract and truthful listed/unlisted/unknown residence accounting
before registering the protected profile. Complete the initializer and outer
consumer-remove lifetime contract. Keep legacy devfreq behavior compatible.

Then execute actual CCI target/core/passive tests for rounding, every clock/raw
failure boundary, partial voltage, changed/unknown latching, original errno,
CPU POST-driven entry after CPU reservation disposal, same-rate passive retries,
later CPU listeners, distinct peers, provider detach, OPP mutation and consumer
remove. Existing CPU callback tests and new CCI token tests do not substitute for
that matrix. Dedicated selected-voltage tests and the Gemini classifier tests
above also remain required. No full CCI target/core/passive acceptance is claimed.

After that foundation is independently reviewed, the thermal scope must define
explicit containment for unknown and latched partial CPU/CCI outcomes, keep
protection until a separately authorized recovery proves safety, and test the
terminal listeners and policy lifecycle. Historical frequency, scheduler scale
or protection-needed flags are not thermal action. Final review, A72/buckB,
physical audio and hardware acceptance remain separate gates.

Complete staged checkpatch reports only the generic new-document MAINTAINERS
advisory (zero errors, one warning); code-only checkpatch is clean. No maintainer
assignment changes are needed for this implementation report.
