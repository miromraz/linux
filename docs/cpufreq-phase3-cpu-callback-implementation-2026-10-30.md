# CPU0/CPU4 callback implementation — 2026-10-30

Base: 0cec6b3736059ddaf3f5889ce3a8c4c209adecfa. Admission remains CLOSED.
No hardware, HP, device locks, flashing, playback, capture, DCO or readiness
claim. No subagents. Compilation, execution and checkpatch run only on llumik.

## Production mapping

- `mtk_cpu_dvfs_info_init` validates the Gemini representative device and
  within-group sharing mask before `mtk_cpu_dvfs_token_init`. The latter acquires
  a registry cookie, retains/enables both clocks, resolves the actual intermediate
  OPP voltage and current-rate OPP, initializes locks/state, and unwinds each
  acquired resource in reverse order. The registry retains ownership of filtered
  OPP configuration and the durable regulator request; initialization neither
  adds a duplicate OPP table nor installs the legacy voltage notifier.
- `mtk_cpufreq_policy_init` is the common production policy initializer, also
  called by `mtk_cpufreq_init`. It builds the real OPP-derived CPUFreq table and
  assigns clock, group mask and driver state. Gemini checks parent validity and
  nonzero initial rate without running the legacy firmware bootstrap.
- `mtk_cpufreq_set_target` selects the token branch only for a protected cookie.
  `mtk_cpufreq_token_target` performs begin, owned prepare, switch and consuming
  finish/abort. The accepted provider supplies copied actual live/intermediate/
  target requirements and selected-rate checks. PLL rounding remains after
  verified parking in `mt6797_consumer_txn_switch`.
- A notifier-independent `callback_lock` trylock serializes state, cookie and
  final puts. Begin/end never hold `reg_lock`. Recursive callback entry rejects;
  release waits through token disposal and state publication. Before returning,
  the token has ended and both driver locks are free. No rollback is attempted.
- Outcomes store accepted clock rate (zero for unknown) and retained ownership,
  not an assertion of electrical rollback. A proven unchanged retry remains
  usable. Partial, cancelled, unknown or failed raw outcomes latch `dvfs_fault`;
  subsequent callbacks reject. Begin rejection does not mutate callback state.
- Release drops the lease and enabled clock references without removing the
  registry's OPP table or durable peer/CCI floors. Probe still exits at the
  original `-EAGAIN` registration gate. Non-Gemini and A72/B target behavior is
  preserved; their policy setup uses the same original common implementation.

## Executable cases

`mt6797_test_cpu_open/target/close` are compiled only with the combined KUnit
configuration. They call production info initialization, policy initialization,
OPP frequency-table selection, the actual target callback, exit and release.
They do not register CPUFreq or override the deployment gate. Synthetic clock
and raw rail transports surround real regulator core, CCF, OPP and token code.

`cpu_callback_case` runs for each registry CPU user (CPU0 and CPU4) inside
`transition_clock_contract`, with sixteen scenarios per user:

| Case | Injection / assertion |
| --- | --- |
| 0 | Rejected stale preflight without writes, then up, same and down; intermediate rate above both endpoints; retained intermediate floor |
| 1 | First mux PRE veto, known-old retry, successful retry |
| 2 | PLL provider refusal/readback mismatch, known intermediate, no retry |
| 3 | Reconnect provider failure before application, unknown, retained vote |
| 4–5 | Raw prepare failure before / after application, old clock but poisoned rail, no retry |
| 6 | Mux reconnect commits then errors, unknown rate and coordinator fault |
| 7–8 | Final reduction failure before / after application, known lower target, retained request and rail fault |
| 9–10 | External generation cancellation after prepare / before finish |
| 11–12 | Asynchronous provider detach after prepare / before finish; cancellation observed, no subsequent raw writes, joined teardown |
| 13 | Above-reserved PLL rounding rejected after parking |
| 14 | Reconnect PRE veto, known intermediate |
| 15 | First regulator PRE veto, unchanged ownership and successful retry |

Every changing synthetic mux updates the fixture's current clock voltage
requirement. Every raw write checks all live floors and rail spread. Fixed SRAM
remains 1.2 V. Distinct external PLL handles attempt setters at callback phase
boundaries and must be rejected. Synchronous clock notifiers perform real OPP
lookup and verify voltage mutation/disable rejection during the reservation.
Rounding instrumentation distinguishes the deliberately rejected external probe
from callback PLL requests. Structured path, retry, retained vote and fault
assertions supplement errno checks.

The existing direct extraction suite now includes the production token info
initializer verbatim and injects only discovery/clock lookup. Combined OF probe
scenarios still enumerate and deduplicate CPU0/CPU4 groups, exercise allocation
unwind, and verify the closed gate. Existing token, direct, regulator and
coordinator suites remain present.

Two fixture corrections were necessary: isolated provider fixtures reset supplier
revocation only with no attached endpoints and no active preflight; the older
stack completion in `production_ownership` now uses DECLARE_COMPLETION_ONSTACK.
The latter was reproduced on ARM64 as lockdep disabling itself, despite passing
KTAP. History inspected: d50f92317fb8/0de3dbd9acfb bind-time ownership,
3f9612cc08bb/2033381315bd clock callback changes, and reviewed token HEAD.

## Validation limits and next review

This increment implements the actual callback, but does not claim every item
in the independent review's broader acceptance matrix has callback-level
execution. Remaining callback-specific coverage includes simultaneous consumer
release with provider drain, stale cookie after rebind, peer withdrawal after
prepare, committed OPP writer paused in notification, accepted removal/final
REMOVE and failed multi-table unwind under driver lifetime exclusion, and a
separate extra-exclusive-claim retry. Existing token/direct tests cover some
underlying mechanisms; those are not substitutes for callback-level evidence.

Actual CPUFreq POST/passive-CCI invocation and synthetic error-reversal accounting
are not executed by these KUnit entries. They exercise the real target callback
without publishing a CPUFreq policy. CPUFreq/devfreq accounting, CCI operational
conversion, thermal containment, dynamic policy-table refresh, arbitrary provider
unload and A72/buckB remain gates. New independent review is required; this is
not operational admission or a completed full acceptance-matrix claim.

Validation evidence is retained in the outer workspace under
`artifacts/cpufreq-phase3-cpu-callback-20261030/`. Builds are incremental W=1 ARM64
and UML in `/home/u/gpda-phase3-20261030` using the existing Docker images. ARM64
provider tests use a standard eight-CPU QEMU virt machine; the seven existing
combined scenarios use their retained DTBs. VM runs have external timeouts.
Selected-source, binary/config and DTB SHA256 manifests identify tested artifacts;
this is not a clean full-tree reproducibility claim.

Final run: W=1 ARM64/UML builds passed with no compiler warning/error matches;
34 UML cases across five suites passed; all 23 ARM64 provider cases passed,
including the 32 callback scenarios within transition_clock_contract; all seven
combined QEMU scenarios passed. Checkpatch: zero errors, zero warnings. Final
runtime logs have no warning/BUG/circular-lock/non-static-lock-key/lock-validator
shutdown matches. Both ARM64 lockdep options remain enabled. Local changed-source
hashes match the remote source manifest used for the final builds. Earlier
iterations caught extraction drift, fixture revocation leakage, the stack
completion annotation and rounding instrumentation counting the external probe;
these results refer to the corrected final run, not a passing first attempt.

- UML SHA256: dc4fdf63a757bfe557652e4c433c0e7e71962a6a3037f1234c858077fe322a0e
- ARM64 Image SHA256: 266d7d62b4becb23361f1797872500e4737e2b43d9a37b0a738dd105254dc210
