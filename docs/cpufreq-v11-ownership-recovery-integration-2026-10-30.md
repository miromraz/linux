# Protected ownership recovery — 2026-10-30

## Delivery boundary: partial, Group4 acceptance remains open

Base kernel: eda88cf81120f7920996329e84eb48067d954fb4, including approved
rail 0de3dbd9acfb, audio 56f8572d4f13 and input eda88cf81120. Read the full
independent ownership review and implementation report. Inspected ownership,
conversion/reservation and consumer initialization history. No approved sound,
MFD, input, CPU or CCI source was edited. No subagents, hardware, flashing,
playback, capture, locks, DCO or operational admission.

This increment implements and tests an explicit retained-vote recovery path.
It does **not** deliver the requested complete integration acceptance matrix.
Return for independent review of this bounded change, not Group4 approval.

## Recovery contract

`mt6797_consumer_recover()` is an explicit process-context operation. A caller
must first release every local lease. Active leases return EBUSY; no background
arrival event automatically recovers ownership. Never invoke it synchronously
on regulator/clock/OPP notifier stacks: queue process-context work instead.

Retry is appropriate after transient transport failure has ceased or suppliers
have reappeared. It remains fallible: no periodic retry loop, time limit, or
elapsed-time assumption grants permission. The registry continues owning all
retained handles and device references until verified transfer succeeds.

Production recovery rechecks topology, canonical consumer device identities,
mandatory NVMEM speed bins, clocks and existing OPP configuration. Same-device,
same-bin reappearance is supported; replacing a consumer device or changing
its bin returns ESTALE. Existing adjusted/disabled OPP tables are not destroyed
or recreated. Clock references can be reacquired through the existing registry
acquisition path. Replacement-consumer policy migration is still unsupported.

A fresh transaction reserves all six clock handles and all three OPP snapshots.
It checks both registry and raw-provider generations under the owner lock.
A read-only engine operation then requires enabled/valid selected banks via
raw reads, the full voltage envelope, SRAM at 1.20 V, and proc covering every
old engine floor, durable guard/vote request and fresh live/intermediate OPP
requirement. Only that proof clears the uncertain-write fault and supplier
revocation. Recovery does not write hardware to repair an unsafe measured state.
Missing providers, stale snapshots, bad reads, disabled/absent OPPs or unsafe
selected voltage leave protection owned and return an error.

After that proof, ordinary production return transactions install/validate
replacement guards while old real votes still exist. Only successful transfer
puts the vote and owner device reference. Partial success is intentional: an
already transferred peer remains guarded, while the failing peer remains
registry-owned and can be retried. A subsequent notification can invalidate the
transaction again; recovery is never a readiness override.

Consumed lease cookies remain permanently stale; recovery does not reset the
cookie counter or resurrect any cookie. Provider generation invalidates physical
proof independently of release-cookie accounting. Existing active cookies must
be released before recovery, and never authorize a physical transition.
Fresh acquisition after recovery allocates a new cookie. The non-devm registry
root, OPP configuration, clock references, guards and coordinator module pin
remain the explicit owners of live boot protection, including with no driver
bound. Ordinary module unload remains prohibited while that protection exists.

Status now distinguishes active leases from retained zero-user votes and
publishes the last ownership operation error and preflight generation per user.
These are observational snapshots, not admission tokens. A failed stale-cookie
operation can update its user's last-operation error. Guarded users have neither
active nor retained bits; `adopted` retains its prior reference-ownership meaning.

## Executed evidence

All compilation, execution and checkpatch ran on llumik in
`/home/u/gpda-recovery-integration-20261030`, using `gpda-kbuild:trixie`.
The source copy was updated from the combined local HEAD plus this patch.
Output directories were copied from the previous ownership run, with relevant
rail objects explicitly removed before rebuilding. This is incremental object
and UML link validation, not a full clean ARM64 Image/modules link or boot.

Evidence is retained in `artifacts/cpufreq-v11-recovery-integration-20261030/`:
commands, patch, build/run logs, selected-source hashes, binary/config hashes,
configurations and container identity. The selected-source manifest is not a
whole-tree reproducibility proof.

* Six rail/CPU/CCI ARM64 objects rebuilt with W=1.
* Nine relevant approved audio/MFD/input objects compiled with W=1 from the
  combined source snapshot; their source was not modified.
* UML W=1 linked and halted normally: 24 passing cases in five suites. Case
  count is unchanged, but production ownership cases gained actual new recovery
  API execution and assertions, rather than merely re-running old assertions.
* Checkpatch for the final code patch: zero errors, zero warnings.
* An intermediate test edit mistakenly added ownership-status expectations to
  the older direct-handover fixture; that assertion failure was corrected by
  limiting those expectations to the production ownership fixture.

## Acceptance mapping and limitations

| Requirement | Evidence / remaining gap |
| --- | --- |
| Retained commit-then-error ownership | Actual core vote/reference survives failed release; recovery transfers to guard and drops exactly one reference |
| Unsafe recovery readback | Non-1.20-V SRAM rejects recovery, preserves fault and both handles |
| Active lease exclusion | Actual recovery API returns EBUSY |
| Raw provider detach/rebind | Actual devres detach/reattach plus recovery, new acquisition and release execute |
| Stale cookies | Consumed-cookie rejection before and after recovery; new acquisitions succeed |
| Live floors/envelope | Existing synthetic physical-write assertions continue to run throughout actual core transfers |
| Concurrency | Existing competing acquisition threads and PRE OPP mutation execute; no new deterministic recovery POST/ABORT pause matrix |
| Direct CPU/CCI callbacks | **Not executed**; no callback-coverage claim |
| Shared CPU0/CPU4 grouping and partial probe unwind | **Not executed** |
| Combined OF/NVMEM/two actual raw adapters | **Not executed**; scalar raw endpoints remain injected |
| NVMEM and clock supplier rebind | Production recovery revalidation implemented, combined fixture absent |
| Device replacement | Explicit ESTALE containment; migration/recovery to a different consumer device absent |
| Allocation/module lifecycle sweep | Existing initialization unwind coverage only; no exhaustive new sweep |
| Full ARM64 kernel link | **Not performed** |

The existing UML configuration has SMP disabled and cannot represent the two
four-CPU groups as configured. UML has configurable SMP support in this tree,
so this is a limitation of the existing fixture, **not proof of a fundamental
framework impossibility**. A suitable SMP/OF fixture or another architecture's
KUnit execution environment, with real CPU device enumeration, is still needed.
No synthetic duplicate algorithm or source assertion is substituted for that
missing callback coverage. CCI's ordinary protected remove remains unreachable
following the deliberately failed production probe; a direct test wrapper must
explicitly distinguish that hook execution from a successful bound lifecycle.

Registration still uses private logical regulators with no upstream supply
links, no boot/always-on enable, and no `apply_uV` physical normalization. Raw
providers depend on the coordinator module; logical guards do not request the
raw provider's public supply. No new dependency edge or registration cycle is
introduced by recovery. This is source analysis, not combined registration-order
runtime proof.

## Remaining Phase3 task (admission stays closed)

Before any publication of CPU0/CPU4/CCI callbacks, implement one explicit final
state/result contract for preparation, intermediate reparent, PLL programming,
final reparent and final voltage failure, including commit-then-error and failed
recovery. Reserve/refresh intermediate OPP requirements. Retain all conservative
old/intermediate/target votes until actual source, rate and selected rails are
verified. Report actual or explicitly unknown final state through cpufreq,
devfreq/passive consumers and thermal accounting, with bounded thermal action
when ordinary transitions are blocked. Bring the older CCI recovery paths up
to that common contract. A72/B firmware acceptance, initial synchronization and
recovery are separate work and admission decisions. Complete the integration
and concurrency rows above before declaring Group4 complete. No hardware
readiness follows from this increment.
