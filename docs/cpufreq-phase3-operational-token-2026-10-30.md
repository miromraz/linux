# Operational CPU reservation token, 2026-10-30

Base: 60432f6f49acf359ac7456bc7fabfb59bef42676. This implements the smaller
operational token prerequisite requested after the earlier incomplete CPU task.
Admission remains closed. No mediatek-cpufreq callback or initialization is
changed. Independent review of the base and this implementation precedes the
next actual CPU target hookup.

## Callable production interface

Declared in include/linux/regulator/mt6797.h, exported by mt6797-provider.c:

```
int mt6797_consumer_txn_begin(unsigned int user, u64 cookie,
        unsigned long target_hz, struct mt6797_consumer_txn **txn);
int mt6797_consumer_txn_validate(struct mt6797_consumer_txn *txn);
int mt6797_consumer_txn_prepare(struct mt6797_consumer_txn *txn);
int mt6797_consumer_txn_switch(struct mt6797_consumer_txn *txn);
int mt6797_consumer_txn_finish(struct mt6797_consumer_txn *txn,
        struct mt6797_txn_outcome *outcome);
void mt6797_consumer_txn_abort(struct mt6797_consumer_txn *txn, int error,
        struct mt6797_txn_outcome *outcome);
```

Use a live cookie returned by mt6797_consumer_acquire for registry CPU user 0
(CPU0 group) or 1 (CPU4 group). User 2/CCI is rejected; its existing vote remains
protected as a peer. No caller-supplied voltage or regulator identity substitutes
for the registry's owned regulator vote. Begin validates the cookie, registered
device, live vote, live requirements, converted rail state and fixed 1.2 V SRAM.
The registry captures a retained PLL handle during clock discovery; begin rejects
a parent different from that identity or an already parked mux. A token retains
its own PLL handle and exactly one exclusive claim per distinct participating
handle, including all peer/intermediate clocks. Shared OPP tables are deduplicated
and old/intermediate/target requirements are copied with the production OPP API.

All token calls are process-context, same-task calls. A successful begin requires
exactly one finish or abort, which consumes the token even on error. Outcome must
be non-NULL. A consumed token must not be used again. Begin is nonwaiting on the
registry transaction mutex and returns -EBUSY for competing or recursive calls.
The mutex pins registry ownership and configuration across the token. Provider
active-use accounting pins orderly raw supplier teardown across all framework
calls and the gaps between them. No OPP-table, owner or coordinator mutex is held
over CCF/regulator calls or their synchronous notifications.

Call begin and end outside notifier-sensitive driver locks: failed begin can
release already acquired OPP reservations, and end can deliver deferred REMOVE.
The transaction mutex remains held through those puts (the finishing exclusion).
Registry notifiers never take it. If the future callback uses reg_lock, it must
arrange its begin/failure/end boundaries accordingly rather than holding reg_lock
through a final put. No supplier may synchronously unbind from its own active
notification stack. Clock providers must remain registered; CCF exclusivity is
not a provider-unregister pin. Arbitrary clock-provider removal is unsupported.

Prepare installs max(existing owned vote, old/intermediate/target requirements)
through the existing real regulator handle. Its raw setter consumes immutable
requirements instead of reacquiring legacy snapshots. Even identical requests
are validated through the coordinator after regulator notification delivery.
Live old requirements, rather than a future target floor, authorize preparation.
Raw authorization and cancellation serialize under owner_lock. A failed raw
step poisons that token against further operations.

Switch is an optional production operation for the future callback: park and
verify the mux, **round the stable PLL after parking**, reject nonpositive or
above-reserved rounding, program and verify the PLL, reconnect and verify. A
same-rate request validates without touching clocks. Each CCF call has a scope
matching task, registered affected core identities, event kind, old rate and new
rate. Expected events invalidate future preflight sampling but do not cancel the
current token. Other events cancel it. Scope is cleared on return, including
no-op calls and PRE veto without ABORT. There is no blanket task exemption.
Tracked parent attempts distinguish a pre-write veto from an uncertain write.

Finish requires a known target and reduces only this consumer's owned request,
retaining the intermediate floor. Cancellation and authorization of the final
raw reduction share owner_lock. Abort does not reduce a vote or release its
owner. Both report error, known-old/intermediate/target/unknown, accepted rate
(zero when unknown), retained owned vote and retry eligibility. An uncertain mux
outcome also faults the coordinator and revokes supplier readiness so ordinary
acquisition/guard retirement cannot trust cached topology. Recovery is a separate
explicit operation, not something a target callback may invoke speculatively.
A prepare failure retains the previously committed vote; errno is not a claim
that hardware rolled back. The coordinator's fault state covers uncertain raw
errors. No automatic clock restoration is attempted.

Consumer release from another task waits for token completion before consuming
the cookie. Same-task acquire/release/recover returns -EBUSY instead of deadlocking.
Provider detach cancels, waits without owner/engine locks, and only then detaches
raw endpoints. End wakes that waiter after all retained accesses and final puts.

## Executed coverage

Production operations are exercised within existing provider KUnit cases, not
through a replacement token implementation. Test-only hooks inject failure after
successful acquisitions and configure synthetic retained suppliers/clocks.

- production_ownership: invalid user/stale cookie, failure after each of eleven
  acquisition checkpoints (provider use, six registry clock claims, PLL claim,
  three OPP reservations), NULL failure result, balanced device references,
  unblocked OPP mutation after unwind, subsequent successful begin; reentrant
  begin/release rejection; reservation-time OPP disable rejection and lookup;
  same-rate prepare/switch/finish; external invalidation and abort with retained
  real regulator request. The existing detached-provider tests also remain.
- transition_clock_contract now runs the exported token separately for CPU0
  and CPU4 using real CCF mux/PLL/intermediate clocks, real OPP tables, registry
  leases and regulator-core votes. It covers first-parent PRE veto followed by
  retry, up/down transactions with intermediate above both endpoint rates,
  external distinct PLL handle rejection, expected self events, synchronous CCF
  notifier OPP lookup and rejected OPP mutations, truthful finish rate/vote, and
  abort retaining a raised preparation vote.
- In each CPU fixture, a release thread starts while the token is active and
  remains blocked. A separate detach thread publishes cancellation and remains
  blocked until abort. The fixture observes stale validation and retained vote,
  then joins both threads before freeing endpoints. The VM timeout bounds a
  locking failure; teardown is never performed synchronously on the token stack.
- Existing requirements removal/writer tests, CCF rejection tests and seven
  ARM64 combined scenarios run as regression coverage. They do not establish
  actual operational CPU callback behavior.

Allocation failure itself is not exhaustively fault-injected; acquisition
checkpoint injection exercises the real unwind after acquired resources. The
new token changing-clock tests do not yet cover every PLL/reconnect/raw-error
permutation from the broader callback acceptance matrix. Existing helper tests
are not represented as token coverage for those permutations. No actual
CPUFreq/passive-CCI notification boundary or callback accounting is claimed.

## Validation and evidence

All compilation, test execution and checkpatch ran on llumik in the existing
/home/u/gpda-phase3-20261030 tree, using gpda-kbuild:trixie and
gpda-combined:trixie. Nothing was built, tested or linted on dev. No subagents,
physical devices, HP, device locks, flashing, playback or capture were used.

Commands: make ARCH=um O=/work/uml W=1 -j8; make ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu- O=/work/arm64 W=1 -j8 Image;
TMPDIR=/tmp timeout 60 ./uml/linux kunit.enable=1 kunit_shutdown=halt;
scripts/checkpatch.pl --no-signoff /work/operational-code.patch.
operational-matrix.sh boots seven eight-CPU QEMU scenarios with 45-second
external timeouts and the retained matrix DTBs. UML runs with Docker seccomp
unconfined. Builds are incremental, not a clean full-tree reproducibility claim.

Artifacts use the operational-* prefix remotely and are copied to outer
artifacts/cpufreq-phase3-operational-token-20261030/. Source, binary/config and
DTB manifests identify the tested inputs. The source manifest covers the four
changed code files, not the entire inherited tree. Final logs record W=1 builds,
UML, QEMU and checkpatch. During development, the first compile caught a signed
max expression and the first lifecycle run corrected an assertion that had
mistakenly expected no raw validation writes. Final logs describe the fixed run.

History inspection: ed6dbcab3744 introduced the retained requirements/drain
primitives; 60432f6f49ac corrected retained removal. The old bind-time
preflight_transaction remains for its original path. This change adds a distinct
operational reservation branch to the existing lease setter; it does not replace
the CPU callback with a userspace or test-only workaround.

Next task: initialize and hook the actual CPU0/CPU4 target callback to this API,
with callback-level cancellation/error tests and the POST/passive-CCI boundary.
CPUFreq/devfreq accounting, thermal containment, CCI operational conversion,
full dynamic OPP configuration, arbitrary provider unload, A72/buckB, DCO and
hardware/audio readiness remain separate gates. The pre-existing non-Gemini CCI
preparation-rejection retry and committed-OPP notification test gaps remain open.

Final results: both W=1 builds passed without compiler warning/error matches;
34 UML cases across five suites passed; seven QEMU combined scenarios passed;
checkpatch reported zero errors and zero warnings. No warning/BUG/lockdep-pattern
matches were observed in the final runtime logs. Binary SHA256 identities:

- UML: 37ace4aba7958ff8297c5f52ebd6c1f0d2cecb5f35e46a8a43f03a29f0554ffa
- ARM64 Image: 5a5e7fc3d9a3fd124b1147cabf505ba24dc8b26f88d3ae5be2aae26d31af696b
