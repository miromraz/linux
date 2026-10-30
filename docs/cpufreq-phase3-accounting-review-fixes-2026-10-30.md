# Accounting review corrections — 2026-10-30

Base: 91b72f6a36893fe0349783c4da57cf31d140beb2.
Admission remains CLOSED. This is a **partial correction increment**, not
completion of the requested independent-review correction scope. Two findings
are corrected below. Unknown terminal compatibility, omitted-policy discovery,
and the mandatory callback concurrency/lifetime matrix remain outstanding.
No production CCI target, thermal, A72/buckB or admission changes are made.

## Completed corrections

### Passive notifier delivery and retry

The real CPU-parent passive notifier returns NOTIFY_DONE after a failed devfreq
update. A child target errno is not a notifier result: in particular, returning
-EIO sets NOTIFY_STOP_MASK and suppresses subsequent CPU listeners. The observed
CPU rate remains cached, update_pending remains set, and update_error retains
the original child error. Pending/error publication occurs under devfreq->lock.
The diagnostic includes that errno. A subsequent same-rate POST retries the
child; successful execution clears both the pending bit and error. This does
not change the CPU target's return value or claim the failing child succeeded.
No notifier_from_errno conversion is used.

Two actual CPUFreq observers now register below passive (priorities -1 and -2).
For both protected CPU users, scenario 18 asserts CPU success, child -EIO, PRE
and actual-rate POST delivery to both observers, a successful same-rate child
retry, cleared child error and cleared core transition state. Observers remain
registered through retry. Their POST handlers exercise the existing production
callback-lock availability and recursive target rejection checks. The child
uses the existing real passive/devfreq framework and deterministic synthetic
profile; it does not execute the operational CCI rail target. This is two later
observers, not a second passive consumer.

### Distinct unlisted rates in CPUFreq statistics

Statistics retain last_freq separately from last_index. Equal unlisted indices
no longer conflate different known rates. Zero explicitly denotes an unknown
observation and always selects the invalid index. Lookup scans state_num only;
deduplication advances state_num as entries are inserted, so duplicate unsorted
entries cannot expose zero-filled capacity as a frequency row.

A changed nonzero observation increments total_trans. Repeated equal rates and
unknown observations do not increment it. A subsequent known observation after
unknown increments it; this counts the new observation, not a measured physical
transition across the unknown interval. Only transitions with two listed
endpoints increment the matrix. Unlisted and unknown residence stays
unattributed. No fabricated frequency, table row, or valid index is introduced.
Deferred reset retains the last observed rate/index while clearing accumulated
counters and charging only a valid listed residence interval.

Tests use the real stats allocation, recording, deferred-reset and free paths
on the actual framework policy, with an unsorted duplicate table. They assert
listed 100 -> unlisted 150 -> unlisted 175 -> repeated 175 -> listed 200,
known totals, listed matrix updates, unchanged listed residence while unlisted,
reset while unlisted, zero/unknown invalid indexing, repeated unknown, and
subsequent listed observation. They run after callback assertions in the
existing 38 CPU-user/scenario iterations, not as 38 independent KUnit cases.
These are generic stats paths; no other CPUFreq driver is booted in this fixture.

## Validation and provenance

All builds, runtime suites and checkpatch run on llumik, using the existing
/home/u/gpda-phase3-20261030 source and incremental ARM64/UML outputs in the
gpda-combined:trixie container. No dev-machine build or test suite, physical
hardware, HP, device locks, flashing, playback, capture or subagents were used.
No DCO is added. Existing checkout and explicit staging only.

Retained evidence: outer workspace
artifacts/cpufreq-phase3-accounting-review-fixes-20261030/.
W=1 ARM64 Image and UML builds, the ARM64 23-case provider suite, seven combined
QEMU configurations, UML regression suites and strict checkpatch are recorded
there. Final ARM64 provider results are 23 passed, seven combined runs pass,
and UML passes its five existing suites (34 cases). Strict checkpatch reports
zero errors, warnings or checks. Build logs contain no compiler warning/error
matches; runtime logs contain no searched warning/BUG/lock inversion/oops
matches. The runner uses finite timeouts. Source, binary, configuration and DTB
SHA256 manifests identify the exact incremental inputs/outputs. This does not
claim clean full-tree reproducibility, arbitrary physical fault behavior or
hardware acceptance. Existing lockdep-enabled ARM64 configuration is retained.

History inspection confirms raw passive errno predates this increment in
8cd9520d35a6, whereas the conflated unlisted stats path entered in 91b72f6a3689.
No model provenance is inferred. The independent findings were static;
post-fix execution verifies the added assertions, without claiming a fresh
pre-fix failing run.

Final ARM64 Image SHA256:
5247257acf577e708c1f9b1b0a0d02d1e23ceb344d825e4937ac15321addf3a0.
Final UML linux SHA256:
1c0b1dd71160b053061b6280288fb72a4df14bd885170f355df6f479337e070f.

## Still required before scope completion

* Define and enforce the audited unknown-terminal listener contract, with a
  tracked PRE resource, terminal cleanup, waiter release and recursion tests.
  The existing no-numeric-POST helper remains unchanged and is not accepted as
  generic listener compatibility.
* Distinguish deliberately omitted CPU parents from genuinely deferred parents
  through real passive registration/target paths. Shared-group, omitted-policy,
  deferred-policy and zero-parent tests are not added by this increment.
* Complete simultaneous consumer release/provider drain at both callback pause
  points, maintaining external policy/info lifetime through POST, cancellation
  before later raw authorization, joined waiters and stale-cookie rebind tests.
* Complete distinct peer withdrawal with per-write floor assertions, committed
  OPP-writer notification exclusion, logical removal/deferred final REMOVE,
  failed multi-table unwind, PRE/final-put recursion, and external setter
  rejection while parked and between PLL programming and reconnect.
* Add the associated other-driver/non-Gemini regression coverage for any new
  core terminal mechanism. This increment does not add such a mechanism.

Underlying token and combined discovery tests are regression evidence, not
substitutes for those missing callback-level interleavings. The separate CCI
operational transition, CCI partial-outcome accounting, preparation-error
availability, terminal/thermal containment, A72/buckB and physical acceptance
requirements remain open. New independent review can assess the two completed
corrections; it must not infer acceptance of the remaining requested scope.
