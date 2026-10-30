# CPUFreq outcome accounting implementation — 2026-10-30

Base: bddb4f6ee57c200a54eb95c54cf36f342c267f8e. Admission remains CLOSED.
Actual framework accounting is implemented, but the requested complete finite
callback lifetime/concurrency matrix is NOT complete. Independent review is
required. This is neither operational acceptance nor a thermal recovery policy.
No subagents, physical hardware, HP, device locks, flashing, playback, capture or
DCO. All compilation, execution and checkpatch ran on llumik.

## Production semantics

The protected Gemini descriptor is separate from the existing MediaTek driver.
It uses the supported driver-owned `target` API and ASYNC_NOTIFICATION flag;
this flag does not require execution on another thread. NEED_UPDATE_LIMITS
makes even equal-rate requests reach the contained driver's fault checks. The
core's target_index same-rate shortcut otherwise returns zero before the
callback can reject a latched fault. Legacy SoCs/A72 retain their descriptor,
flags and target_index behavior. Probe still returns EAGAIN before Gemini
registration; a cookie is mandatory for the protected callback.

Each admitted attempt calls cpufreq_freq_transition_begin with old confirmed
and requested rates. It executes the existing token callback, which disposes
its token, claims, OPP references and callback lock before returning. Only then
is framework completion delivered, allowing passive devfreq to run without CPU
ownership/provider/OPP locks. Synchronous recursive target entry is rejected
before waiting on transition_ongoing. Framework policy lifetime remains the
outer lifetime guarantee, as in the accepted callback foundation.

For a known final rate, freqs.new is the accepted rate, not the OPP request.
cpufreq_freq_transition_end(..., 0) emits the real POST, updates policy.cur,
records stats and invokes the scheduler scale hook. Its zero argument means
“do not manufacture a reverse transition”; the target callback separately
returns the ORIGINAL token errno. Thus a retained target after failed voltage
reduction, or a retained intermediate after PLL refusal, is accounted at that
rate while the governor receives failure. A clean unchanged veto reports the
old rate and permits the existing retry. Raw/partial failures remain latched;
no new recovery authorization or voltage rollback is invented.

Unknown mux outcomes cannot be represented by a truthful numeric POST. The new
cpufreq_freq_transition_end_unknown consumes the matching begin, releases the
transition lock/task and wakes waiters without POST/reversal/scheduler update.
policy.cur remains explicitly last-confirmed history, marked cur_unknown;
scaling_cur_freq and cpuinfo_cur_freq return EIO, quick_get returns its existing
unavailable sentinel, and further framework targets return EIO. No zero rate
is published as a transition. Statistics stop charging the last confirmed OPP.
The last scheduler scale remains historical: this is containment pending the
separate terminal/thermal response, not meaningful frequency invariance after
an unknown outcome. Existing notifiers receive no invented numerical completion;
review must assess this explicit unknown-completion API before admission.

The protected get callback reads the token-accepted cache rather than inferring
an uncertain mux state from CCF's cached topology. Successful below-request
rounding is preserved in POST, cur and get. OPP/table frequencies remain voltage
coverage/request ceilings, not a claim that the provider accepted their exact
values. cpuinfo limits remain that request envelope; initial-frequency forcing
and EM registration from the legacy descriptor are not enabled on this new
path. Dynamic canonicalization of OPPs, changing rounding behavior and accurate
hardware minimum-limit characterization remain outside this increment. These
limits need review before enabling the descriptor on physical hardware.

CPUFreq stats previously ignored transitions to an unlisted rate and kept
charging the previous table entry. They now close that interval, mark the
unlisted index, and resume charging only when a listed rate is observed again.
Unlisted/unknown residence is deliberately unattributed; it is not silently
assigned to an OPP. Known transitions increment total_trans, while the matrix
only counts pairs with two listed endpoints. The existing reset/update path
now also guards an unlisted initial/current index against out-of-bounds writes.
No fabricated OPP row is added.

The passive governor now keeps the observed CPU rate even if its CCI update
fails, and tracks a pending update so a subsequent same-rate POST retries it.
Previously restoring the CPU cache misrepresented the CPU; simply retaining
it without a pending bit would suppress the retry. Interpolation clamps CPU
rates to the policy envelope and handles equal min/max without unsigned
underflow or division by zero. These are generic passive correctness fixes,
not a conversion of the production CCI target to operational ownership.

## Executed framework fixture

The existing CPU0/CPU4 callback matrix now calls __cpufreq_driver_target with
an isolated driver descriptor and a real allocated CPUFreq policy. It runs the
actual PRE/POST chain, stats, policy update and ARM64 scheduler hook. The
fixture refuses to replace an existing platform driver; it is intended only
for the isolated synthetic KUnit image, not concurrent userspace policy access.
It temporarily supplies/restores the scheduler frequency reference and scale.

A notifier observer checks exactly one PRE and one actual-rate POST per known
attempt, no POST for unknown, original outcome errno, completed transition
state, and rejection of recursive target entry. In POST it verifies callback
and regulator mutexes are available. Raw failure cases also inspect the actual
regulator-core request, not just outcome.retained_uv. Known rates and stats
indices are checked, including unlisted rounded success and unknown state.
Scheduler scale is asserted against the actual final rate; unknown preserves
the prior scale without a fabricated update.

The passive fixture registers the REAL cpufreq_passive_notifier_call in that
chain. It runs devfreq_update_target, the passive get-target override, and the
real devfreq PRE/profile-target/POST path. A deterministic synthetic CCI profile
records requested Hz; it does not perform the still-gated operational CCI rail
transaction. Existing durable CCI/peer votes and per-write floor/envelope checks
remain in the regulator fixture. This tests actual passive callback/core
execution, not merely an expected notification list.

Nineteen scenarios per CPU user run within transition_clock_contract (38 loop
iterations, not 38 independent KUnit policies/suite cases). The prior sixteen
injections remain, now with framework assertions. Added cases cover accepted
150 kHz rounding from a 200 kHz request; a separate extra mux-exclusive claim
refusal/release/successful retry; and a failing synthetic CCI target followed by
successful same-rate passive retry. CPU success remains success when that
separate CCI target fails. The expected CCI failure diagnostic is retained.

Initialization coverage remains split: the callback fixture calls token info
and policy initialization, bypassing production OF representative/group lookup.
Existing direct/combined tests cover discovery separately. This is not an
end-to-end OF-discovered operational callback policy or every initialization
allocation/clock-failure permutation.

## Validation and provenance

Evidence is in outer-workspace artifacts/cpufreq-phase3-accounting-20261030/.
Remote source/outputs: llumik:/home/u/gpda-phase3-20261030. The retained runner
records QEMU invocations and finite timeouts. W=1 ARM64 Image and UML links
passed; final logs have no compiler warning/error matches. Checkpatch reports
zero errors/warnings. ARM64 provider suite passes all 23 cases, including the
38 accounting iterations. All seven combined discovery configurations pass.
UML passes the existing five suites (34 cases); UML does not execute the ARM64
combined callback fixture. Final runtime logs have no warning/BUG/circular-lock
or kernel-oops matches. The ARM64 image retains PROVE_LOCKING/DEBUG_LOCK_ALLOC.

Build/test development caught the real target_index no-op limitation, a fixture
missing its scheduler reference, table validation before installing the test
driver, the request-observer's handle-count return contract, and a descriptor
name-length warning. A later scheduler-assertion build lacked the KUnit helper
header; it was corrected before the final build and all final executions. UML
initially rejected executable memory on /dev/shm; the successful invocation
uses TMPDIR=/tmp. Passing results above refer only to final source/binaries.

Selected-source and binary/config/DTB SHA256 manifests identify the artifacts.
Builds are incremental on retained outputs, not clean full-tree reproducibility.
ARM64 Image: 73f6a3789c0c6a8e35102bb9e88e5dc30e185527d27506c304e2e6fa1cef79b4.
UML linux: d067a560c01dfdeecb1baf506be24d90cd19095bd884434228fffd6b2f1dd6b7.

## Remaining acceptance work — not waived

The independent review's simultaneous callback consumer-release/provider-drain
races at both pause points, stale cookie after rebind, distinct peer withdrawal,
committed OPP writer paused in notification, logical removal/deferred REMOVE,
and multi-table unwind under callback lifetime exclusion still lack the asked
callback-level extensions. Underlying token tests are not substituted for them.
External setter checks while parked and between programming/reconnect also
remain below callback-level coverage. No claim of a completed finite matrix.

Unknown completion semantics, unlisted statistics and the request-envelope vs
accepted-rate policy need independent review. Terminal containment, production
CCI operational tokens, CCI's own partial-failure accounting, thermal response,
A72/buckB, arbitrary provider unload, dynamic policy-table refresh and physical
acceptance remain separate gates. Admission must stay closed. Related legacy
CCI preparation-error availability behavior remains tracked in the preceding
contract review; this patch does not claim to resolve it.
