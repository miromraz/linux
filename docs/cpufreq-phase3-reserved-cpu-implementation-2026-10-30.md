# Reserved requirements and provider drain increment, 2026-10-30

Status: prerequisite implementation only; CPU0/CPU4 operational integration is
NOT implemented. Admission remains CLOSED. Base: 29c2fcc2420a. This does not
complete the requested CPU slice or its nine-part acceptance matrix. No
fundamental framework blocker was established. Independent review is still due.

Read the full transaction-contract review, CPU-owned-path report and preceding
Phase3 reports. No subagents, hardware, HP, device locks, flashing, playback or
capture. Builds, runtime tests and checkpatch ran only on llumik. Approved
unrelated commits and untracked reviews remain untouched. No DCO is asserted.

## Implemented contracts and callers

`dev_pm_opp_requirements_get(table, rates, count, &snapshot)` takes the table
mutex nonwaiting, selects available non-removed ceiling OPPs with the existing
lower-bound rule, copies rate/target/min/max voltage, retains each selected OPP
and the table, increments the reservation count, then releases the mutex.
`dev_pm_opp_requirements_read(snapshot, index, &value)` copies an immutable entry.
`dev_pm_opp_requirements_put(snapshot)` ends reservation and releases references;
it must run outside notifier-sensitive driver locks. Single-clock/single-supply
configuration is required; required-OPP/lazy-linking, interconnect and genpd
layouts are rejected. The caller must own immutable configuration for the
reservation lifetime; this API does not freeze configuration or membership.

Availability/voltage writers check reservations under the same mutex before
editing and return EBUSY without notification. Successful writers remain counted
through blocking notification; reservation acquisition then returns EAGAIN,
without waiting on a driver's reg_lock. Existing target-voltage-only equality
shortcut is preserved: this is not full min/max supply equality. Multiple
reservations are allowed; callers must deduplicate shared tables themselves.
Logical removal cannot free retained snapshot entries, and new reservations
exclude removed entries. Membership cancellation remains a registry obligation.

These are production OPP primitives. Their current callers are the focused
provider fixtures; the operational registry token caller is still missing.
Existing bind-time `dev_pm_opp_snapshot_lock` callers remain unchanged.

Provider `provider_use_get(generation)` / `provider_use_put()` now bracket the
entire existing `preflight_transaction`, including regulator callbacks and gaps
between raw accesses. Admission checks provider generation and attached/detaching
state under owner_lock. `detach` marks detaching and invalidates preflight under
owner_lock, releases it, waits for all admitted uses, and only then detaches the
engine endpoint. Raw operations still validate cancellation/generation under
owner_lock. Reattachment clears that endpoint's detaching bit. No owner or engine
lock is held while waiting. The existing devres action is registered after the
resources it protects. This extends bind-time transaction lifetime; it does not
claim clock-provider unregister safety or an operational CPU token.

Test-only hold/drain observation hooks exercise this same production active-use
mechanism. The combined fixture's old synchronous devres release from inside a
transaction was changed to a separate teardown task. The callback waits for
cancellation publication; cleanup joins the task, retaining its task reference.
Synchronous teardown of one's own active-use stack is unsupported, as specified
by the review. The first QEMU attempt timed out on precisely that old fixture
pattern; this was not hidden by weakening production draining.

## Reproduced clock integration hazard

With distinct owned mux, PLL and intermediate handles held exclusively,
`clk_round_rate(pll, target)` before parking returns the old PLL rate because
the active mux still protects its ancestry. The existing helper then parks,
programs the requested PLL rate, and reports EIO against its obsolete rounded
expectation. This was reproduced by the new real-CCF test. It is separate from
the illegal recursive CCF setter scenario.

The test verifies correct explicit ordering: park and verify parent, round the
PLL after parking, program and verify its rate, reconnect and verify output.
External handles cannot change rate/parent during claims. An extra exclusive
mux claim rejects reparenting before a provider attempt; removing it permits
retry. No new CCF framework or changes to non-Gemini/A72 helpers were introduced.
The operational token must use explicit per-call scopes and round after parking;
it must reject a rounded rate exceeding the reserved requirement before PLL
programming, retaining the intermediate protection on that error.

## Executed evidence and limits

Remote artifacts: `llumik:/home/u/gpda-phase3-20261030/reserved-*`.
Local copies: outer workspace `artifacts/cpufreq-phase3-reserved-20261030/`.
Incremental W=1 UML and ARM64 Image builds use existing output/config trees in
`gpda-kbuild:trixie`; this is not a clean reproducibility claim. The seven existing
combined QEMU cases use `gpda-combined:trixie`, eight CPUs, and a 45-second timeout
per VM. UML has a 60-second external timeout. Final source/config/binary hashes
and build/run/checkpatch logs are retained alongside the patch.

Final results: UML 33 cases passed across five suites (22 provider cases), no
failures/skips; seven QEMU cases passed and powered down, with no runner-matched
warning/BUG/lockdep failure. Final incremental W=1 build logs contain no compiler
warning/error matches. Checkpatch: zero errors, zero warnings. ARM64 config
retains PROVE_LOCKING and DEBUG_LOCK_ALLOC. No new KASAN run is claimed.
UML binary SHA256: `918b8eeb7cf904cd9917664fe50d8e7b3587ca94a5602d6eaf7cdee4680825fd`.
ARM64 Image SHA256: `de6bc2afee2308cb086111d079e39a71b912a7ed998ac010758985f7ffdfa843`.
The seven changed code files match the remote source manifest. This selected
manifest does not establish identity of every inherited source file.

Focused coverage:

- Real OPP lookup during reservation and synchronous CCF notification completes.
- Voltage adjustment and disable during reservation return EBUSY without events.
- A committed writer's notifier attempts reservation and gets EAGAIN; releasing
  the reservation permits adjustment, and copied min/max bounds are verified.
- Snapshot reads survive remove-all; new selection from removed entries fails.
- Real owned CCF claims permit explicit park/program/reconnect; external handles
  are rejected and extra-claim pre-write rejection can retry.
- Provider teardown pauses after cancellation and cannot complete while a real
  admitted use is paused outside all owner/engine locks. The endpoint remains
  callable until the use ends; detach then completes. Cleanup releases barriers.
- Existing ownership, retained requests and combined discovery regressions run.

This is not CPU target, passive CCI POST, thermal, physical PLL, or arbitrary
supplier-unload coverage. No operational vote/failure-outcome claim follows.

## Exact remaining integration

Implement `mt6797_consumer_txn_begin/prepare/switch/finish` in the registry,
with an opaque token validated against Group4 user/cookie, stable referenced PLL
identity, distinct retained clock claims, deduplicated requirements for all peers
and old/intermediate/target, and task/operation/event-specific authorization.
Extend `lease_write` to consume that token's copied live requirements rather
than reacquiring preflight snapshots. Retain real owned conservative requests
through every error and serialize cancellation with final reduction. Finish must
keep new admission excluded while final OPP puts run outside driver locks.
Extend the active-use scope across this entire token, not just today's preflight.

Then initialize full Gemini operational state in `mtk_cpu_dvfs_info_init` and
connect the actual CPU0/CPU4 branch of `mtk_cpufreq_set_target` to these APIs,
with KUnit invocation behind the closed gate. These driver functions are unchanged
here. Implement known-old/target/intermediate/unknown outcome and retry reporting,
fixed SRAM and verified floors, preserving the durable CCI vote. Execute all
remaining callback/failure/request-restoration/membership/passive-CCI cases from
the independent review. The non-Gemini CCI preparation-rejection and committed
OPP-notifier findings remain open. CPUFreq error accounting, thermal containment,
CCI operational upgrade and A72 ownership remain separate gates.
