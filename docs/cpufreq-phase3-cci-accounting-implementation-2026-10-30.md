# CCI accounting implementation work — 2026-10-30

Base: 9ec770fc34cbafaab5f7b54c3ed2baaea8231287. Admission remains CLOSED.
This is an INCOMPLETE implementation of the single integration task. It does
not satisfy the independent review's finite acceptance matrix. No initialized
production CCI binding or complete CPU POST -> passive -> actual CCI chain is
claimed. The preceding accepted subsystem commits are preserved. No subagents,
physical hardware, HP, device locks, flashing, playback, capture or DCO trailers.
All compilation, checkpatch and execution take place on llumik.

## Implemented framework semantics

A profile may supply target_outcome instead of target. The callback returns its
actual errno, independently of a devfreq_outcome containing the observed rate,
retry eligibility and protection-needed indication. A nonzero rate denotes a
known observation, including an unchanged, intermediate or rounded rate on error.
Zero denotes unknown, not zero-frequency hardware or a successful transition.
The core records this outcome before completion delivery; it never rewrites the
errno, synthesizes rollback or publishes old-rate POST on an opt-in failure.

DEVFREQ_OUTCOME_NOTIFIER explicitly admits listeners that dispose PRE resources
on POST or TERMINAL. Legacy listener registration on an opt-in profile fails
with EOPNOTSUPP. Known completions carry devfreq_freqs; TERMINAL carries the
outcome, with no numeric POST. Delivery continues past NOTIFY_BAD. Profile
selection is immutable over registration lifetime. No existing devfreq-parent
passive listener is silently opted in: its ordinary registration is rejected
for an outcome parent pending a separate audit. CPU-parent passive remains
usable. Legacy profiles retain their previous target/error notification path.

previous_freq and last_status.current_frequency record the known rate, or zero
as the internal unknown sentinel. cur_freq, target_freq and debugfs summary
print unknown rather than historical frequency. Monitor resume cannot overwrite
an opt-in outcome with a legacy get_cur_freq sample. Listed residence remains
in existing rows. Unlisted residence and unknown residence have explicit
aggregate counters; distinct unlisted numeric changes count as transitions,
repeated identical observations do not. Unknown boundaries have a separate
counter and do not invent transitions between listed rows. Reset clears the
new counters and resets the timestamp. Unlisted frequencies do not acquire
invented table rows; per-unlisted-frequency residence is not retained.

The CCI outcome adapter uses the existing owned target/token and copies its
result under callback exclusion. Missing leases cannot fall through to legacy
regulator code. Contended entry reports unknown rather than echoing the request.
The protected profile is presently used only by the CCI fixture; the production
probe still releases its lease and returns EAGAIN. This is deliberately not an
initializer or outer lifetime guarantee. The actual CCI fixture now exercises
first-parent veto, retry up and down through devfreq_set_target, rather than
calling only the target callback.

CPU passive preserves pending child error, records known CPU observations, and
will not retry a nonretryable opt-in child failure. Unknown CPU observations
continue to reject aggregation. START registers outside devfreq->lock, then
waits for each policy's transition completion before taking devfreq->lock and
the policy transition spinlock across snapshot/publication. A raced new
transition causes an event-driven wait/recheck, not timer polling. This also
covers CPUFreq's policy->cur commit AFTER POST. Same-task transition recursion
returns EBUSY. Callbacks share devfreq->lock for lookup/update. During START,
callbacks record observations without targeting an incomplete parent set.
STOP unregisters/drains callbacks before detaching the list under that mutex;
OPP puts happen outside it. No hotplug replay/subscription framework is added.

## Validation and limits

Remote incremental outputs use /home/u/gpda-phase3-20261030 and container
image gpda-combined:trixie. W=1 ARM64 Image and UML builds are required.
The UML configuration additionally enables PM_DEVFREQ and DEVFREQ_GOV_PASSIVE
so the new core test actually executes there. The devfreq-outcome KUnit case
runs real devfreq_set_target with synthetic outcome callbacks: listed success,
unlisted rounding, repeated unchanged failure, a distinct unlisted known error,
unknown failure, repeated unknown, and return to a known state. It checks errno,
previous_freq, listed/unlisted/unknown accounting and two observers whose
NOTIFY_BAD returns must not suppress completion. It is not a CCI failure matrix.

The existing ARM64 provider case now sends its three CCI attempts through core;
the remaining CPU/provider/combined cases are regression coverage, not new
full-chain acceptance. Retained artifacts are in outer
artifacts/cpufreq-phase3-cci-accounting-20261030/. The runner uses 60-second
provider/core/UML and 45-second combined VM timeouts. Source and binary/config/
DTB manifests identify incremental inputs and outputs, not a clean reproducible
full-tree build.

Development validation caught and corrected a lockdep inversion caused by
registering the CPU notifier while holding devfreq->lock, an oversized fixture
stack frame, and a local variable colliding with the kernel current macro.
Only the final source-matched run is evidence.

## Unfinished acceptance cells — do not infer completion

1. The initialized protected driver/profile, acquisition/defer/unwind matrix,
   actual classifier cases and device/devfreq removal lifetime are not built.
2. No deterministic paused START/STOP publication interleaving test exists yet;
   sequential START/STOP/restart regressions do not prove the concurrency fix.
3. CPU0/CPU4 POST -> passive -> actual CCI target/core/listeners is not connected.
   Existing CPU tests still use their synthetic child. Token and direct-core
   results cannot replace this acceptance cell.
4. Rounded CCI, non-monotonic selected-voltage rejection, PLL/reconnect/mux and
   raw-write failures are not run through that full chain. Existing token-level
   voltage/CCF/OPP/provider results retain their prior bounded meanings.
5. CCI consumer removal/provider drain at prepare and switch pauses, stale-cookie
   rebind, peer withdrawal and deferred OPP recursion lack the required outer
   driver lifetime/full-chain assertions.
6. Generic legacy behavior and stats reset need dedicated new assertions;
   regression results alone do not exhaust that contract.

No concrete external blocker prevents continuing this work; this report records
partial implementation rather than presenting the requested task as complete.
Complete these cells and obtain a NEW independent review before thermal work.
Thermal containment must then define action for latched partial and unknown
outcomes while retaining protection until separately authorized recovery.
A72/buckB and physical acceptance remain separate gates.

Final bounded results: both W=1 builds pass; no compiler warning/error matches.
ARM64 devfreq-outcome 1/1, provider 23/23 and seven combined configurations pass.
UML six suites/35 cases pass, including the new devfreq-outcome case. The final
runtime logs contain no searched warning, BUG, not-ok, circular-lock,
inconsistent-lock or internal-error patterns. Code checkpatch: zero errors,
zero warnings. These results do not close the unfinished cells above.

* ARM64 Image SHA256: a9b1c92afe124fa19d847a82f4850c479b84b34660247447cfc95e64adadee50.
* UML SHA256: d01fbf096c3e6a2a0b6d6c22e51b27ba34b2cee52d25a58e02c99b993dba6d6a.

The source manifest covers eight production/test source files and the retained
provider/combined runner. Binary manifests include both configs and eight DTBs.
