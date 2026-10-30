# CPUFreq terminal listener contract — 2026-10-30

Base: e70629ed463624ad1a7fe786ddcb0d6dc5b223a9. Admission remains CLOSED.
The preceding POST-listener and unlisted-statistics fixes are preserved.
No CCI operational target, thermal response, A72/buckB, hardware acceptance,
flashing, HP, device locking, playback or capture is included. No subagents or
DCO trailers were used. Builds, execution and checkpatch run only on llumik.

## Supported terminal contract

An unknown-capable driver declares CPUFREQ_TERMINAL_NOTIFICATION. Registration
refuses existing legacy transition listeners; legacy listener registration is
also refused while that driver is published. Both directions use the existing
fast-switch mutex. An audited listener opts in using CPUFREQ_TERMINAL_NOTIFIER
for registration and unregistration. It receives ordinary PRE/POST for known
outcomes, or PRE/TERMINAL for unknown outcomes. It must not stop PRE/POST delivery
and must dispose PRE resources at either completion. Register/unregister still
obey the notifier API lifetime rules, including no synchronous self-unregister.
This is an explicitly bounded listener contract, not an adaptation of every
legacy architecture, framebuffer or I2C listener. Unsupported listeners cause
registration failure rather than being silently included.

TERMINAL carries a policy, the original target error and protection_needed=true;
it has no frequency fields. Delivery walks the SRCU-protected registered chain
and ignores stop results, so a terminal disposal cannot suppress another one.
The driver disposes its token/claims and callback lock first. Core then marks
cur_unknown and stops statistics attribution before delivering TERMINAL. It
retains the historical cur and scheduler scale, clears transition_task and
transition_ongoing under transition_lock after delivery, and wakes waiters.
No numeric POST, zero-frequency success, old-rate reversal or recovery is
invented. The CPU callback still returns its original error. Protection-needed
is an explicit terminal outcome, not a thermal implementation or authorization
to resume targets.

Passive opts into this contract. TERMINAL preserves its historical observation,
marks it unknown/pending, records the original error, and makes no child target
call. Subsequent target calculation refuses unknown parents, including before
an optional custom mapping override. Known POST and the already-fixed child
failure/retry behavior are preserved.

## Omitted policies

The optional non-sleeping driver policy_required callback distinguishes an
explicit omission (0), a required supplier (1), and an unavailable/invalid
topology (negative errno). Without a driver declaration, all CPUs remain
required; an absent policy continues to defer registration. The Gemini owned
descriptor excludes CPU8+, identifies representatives CPU0/CPU4, and distinguishes
an entirely absent group OPP description from a partial or mismatched one.
Missing CPU devices/nodes/representatives defer; mismatched OPP phandles fail.
Non-Gemini descriptors do not supply this callback.

Real passive registration and target aggregation consult that declaration.
Shared policies remain deduplicated. Zero managed parents fail with -ENODEV.
Registration failure now unregisters its notifier and releases collected OPP
references, including a failed initial target. It does not leave a partial
subscription behind.

## Finite execution matrix

The ARM64 provider suite still has 23 KUnit cases. Its actual CPU callback case
now loops two CPU users through 21 scenarios (42 iterations), with additional
attempts inside scenarios. These are not 42 separately enumerated KUnit cases.
The actual regulator, CCF, OPP, CPUFreq and passive/devfreq framework paths run
against deterministic synthetic providers, without a production CCI rail target.

* Two listeners retain a tracked PRE resource and dispose it on POST/TERMINAL.
  The earlier terminal listener deliberately returns NOTIFY_BAD; both still
  receive the same unknown completion. Both exercise recursive target rejection.
  Two actual transition waiters are queued during PRE and joined after core
  completion. Unknown tests assert original passive error and zero child calls.
* Existing prepare/after-switch drain cases now launch consumer release too.
  The fixture retains policy/info and holds the framework policy rwsem through
  the complete callback/POST chain. Release waits on that external lifetime
  exclusion; both POST observers assert it remains pending. Drain cancellation
  precedes the next authorization; no later raw writes occur. Release and drain
  threads join before rebind. Provider rebind requires recovery before a fresh
  lease; old-cookie release and token admission fail after the new lease exists.
* A peer has a distinct 1.11 V OPP requirement, versus CCI's 1.08 V requirement.
  Its lease withdrawal starts after CPU prepare and cannot finish while the
  transaction owns exclusion. Existing conversion_write assertions check all
  live CPU/CCI/peer floors on every write. Withdrawal joins, the peer floor
  remains protected, and a subsequent CPU request succeeds. Synthetic peers
  still share the intermediate clock; the voltage requirement and lease are
  distinct, not a claim of independent physical clock characterization.
* All eleven token acquisition failure stages now enter through the actual CPU
  callback and core notifications, including failures after reserved tables.
  They assert no writes and released callback exclusion; later entry succeeds.
* A highest-priority OPP voltage listener enters the actual CPU/core target
  synchronously while the committed writer is still in notification, before
  registry invalidation. Reservation refusal returns -EAGAIN without raw writes.
  This is deterministic same-stack writer exclusion, not a scheduler race.
  A separate scenario logically removes a reserved OPP after prepare, verifies
  it disappears from lookup/count before REMOVE, then verifies deferred final
  REMOVE under callback exclusion and recursive -EBUSY. Re-add and a later
  real callback succeed after disposal.
* New token hooks run outside CCF notification/scope while parked and after PLL
  programming but before reconnect. An external PLL handle's setter is rejected
  at both boundaries; the existing prepare/after-switch probes remain.
* Real passive START/target/STOP runs with CPU0–3 and CPU4–7 policy groups: both
  managed, the second deliberately omitted, required-but-missing, unavailable
  topology, no managed parents, and default non-Gemini behavior with present or
  missing parents. Real CPUFreq lookup, OPP refs, deduplication, interpolation,
  child targets and failure cleanup execute. Classification is injected in this
  fixture; existing seven combined OF configurations remain separate grouping
  and probe regression evidence, not an OF-to-terminal end-to-end boot.
* An isolated legacy descriptor exercises actual begin/end success and the
  existing numeric failure/reversal sequence, with ordinary listener registration.
  Terminal-mode ordinary listener registration is independently rejected.

The longer execution exposed an existing timing-dependent dirty-preflight test:
its worker could finish before the asserted refusal. A KUnit-only worker hold
makes that assertion deterministic and resumes reconciliation before retry.
The new lifetime fixture also distinguishes no-begin rejection from completed
transitions when checking scheduler scale; a rejected target must retain scale.

## Validation and provenance

Evidence is retained in the outer workspace directory
artifacts/cpufreq-phase3-terminal-contract-20261030/. Previous approved logs are
untouched. Remote incremental W=1 ARM64 Image and UML builds use the existing
/home/u/gpda-phase3-20261030 source/output directories in gpda-combined:trixie.
The runner has finite QEMU timeouts and explicitly fails on runtime warnings.
The inherited negated-grep checks did not reliably fail under set -e; the new
runner uses explicit conditional exits. Final runs also exclude the intermediate
fixture lock-class and stack-frame warnings found during development. ARM64 retains PROVE_LOCKING and
DEBUG_LOCK_ALLOC. UML does not compile the ARM64 combined callback branch.
Source and output SHA256 manifests identify the exact selected source,
configuration, Image, UML executable and combined DTBs; this is incremental
artifact identity, not a clean full-tree reproducibility claim.

Remaining operational gates are unchanged: closed admission, actual CCI target
and partial-outcome accounting/preparation availability, thermal containment,
A72/buckB and physical acceptance. The finite synthetic results do not prove
arbitrary provider unload, allocation failures or arbitrary hardware behavior.

Final validation: ARM64 provider 23/23, seven combined configurations passing,
and UML five suites/34 cases passing. Final build logs contain no compiler
warning/error matches; final runtime logs contain no searched warning, BUG,
lockdep, circular-lock or oops patterns. Strict code checkpatch is clean.
ARM64 Image SHA256:
25d3d52c10a58f6d44fff46e32afb93102de0c43c36bc6e1fea82e5cc105d3aa.
UML linux SHA256:
96d8d47d8d6ada790dc671e9f742237b66eb38ca96bc2ea8045f8213d319ea75.

The complete staged patch's strict checkpatch has only the generic new-file
MAINTAINERS advisory; the code-only patch has zero errors/warnings/checks.
The report and fixture runner need no new subsystem maintainer assignment.
