# Devfreq START deadlock and status fixes — 2026-10-30

Base: `5162fe49e1728a880ca528dd95b10e1ea2152834`, plus the supplied
unstaged framework test delta. Admission remains **CLOSED**. This is a focused
implementation and self-review; independent review is next. No subagents,
physical hardware, HP, device locks, flashing, playback, capture, thermal or
A72 implementation. No DCO or hardware acceptance is asserted.

Read both the devfreq outcome independent review and full CCI chain test report
in full, and `/home/realni/AGENTS.md`. History identifies the outcome/publication
change in the base commit and the old status helper in imported `8cd9520d35a6`;
no model provenance is inferred. All compilation, execution and checkpatch ran
on llumik. Local work was source editing, Git operations and artifact copying.

## Production contracts and fixes

CPUFreq now records tasks executing PRE, POST and TERMINAL notification delivery
using stack-owned contexts on a short spinlock-protected list. No allocation or
sleep occurs under that spinlock; no notifier or SRCU drain runs under it.
Contexts support nested calls and actual completion tasks, rather than assuming
that `policy->transition_task` is also the task delivering an asynchronous POST.
Passive START queries this context **before registration**, rejecting recursive
entry with `-EBUSY`. Thus a later START error cannot unregister from its own CPU
notifier read section. The existing non-notifier same-task check remains.

Ordinary START still subscribes before sampling, waits for policy commit without
the child mutex, and snapshots/publishes under child mutex plus transition lock.
The original correction for POST-before-policy-cur and wait-versus-new-transition
races is preserved. Normal failure and STOP still unregister/drain before parent
list detach/OPP puts; successful unregister now clears the callback pointer,
including on ordinary STOP. This prevents stale subscription state across reuse.
No deferred cleanup can outlive the devfreq object. General recursive STOP or
arbitrary concurrent lifecycle operations are not supported or advertised.
Other governors' notifier registration and target behavior are unchanged.

For opt-in outcome profiles, `devfreq_update_stats()` restores the authoritative
`previous_freq` after `get_dev_status()`, including callback error. Known
observations remain numeric; unknown remains zero, never a resurrected historical
frequency. Callback busy/total and private_data semantics are preserved, as is
the exact status errno; target errno/outcome remain untouched. This deliberately
does not discard a status callback's writes on failure: legacy behavior permits
those writes. Legacy profiles continue to own the complete status object.

Source contracts checked: simple-ondemand returns status errors before consuming
statistics and explicitly treats zero current_frequency as unknown; Tegra's
custom governor consumes private_data from its callback and uses a legacy
profile. The target path writes previous_freq and last_status together under the
existing devfreq mutex. Initial previous_freq is initialized from initial_freq.
The helper is used under the existing governor serialization contract, not a new
independent hardware-observation API. Reset does not invent a known observation.

## Regression scope

The original cumulative test-only patch is retained byte-for-byte separately as
`artifacts/cpufreq-phase3-devfreq-deadlock-status-fixes-20261030/preserved-test-delta.patch`
in the outer repository; the earlier cumulative.patch and all baseline logs are
untouched. Focused reset, legacy-error, status and recursive-START regressions
are included with the fixes for review. Other untracked review reports are not
staged or committed.

The previously bounded deadlock test now exercises PRE, POST and TERMINAL
recursive START, asserts `-EBUSY`, empty parent list, no callback subscription,
no child target, restored policy refcount and completed transitions. Its fixture
clears its own manually unregistered callback before START, matching subscription
state. The direct same-task control remains separate. The callback context must
also be absent after completion. An external VM timeout remains mandatory.

A finite lifetime regression pauses a real CPU POST in the child target, starts
normal STOP on a second task, checks STOP does not complete while the callback is
held, then releases it, joins both tasks, checks empty parents/cleared notifier,
and restarts. It runs in successful registration matrix configurations, with
bounded completion waits and an outer VM timeout. This tests framework lifetime,
not removal of a real CCI consumer. Tests retain reset and legacy target-error
coverage. Status coverage asserts all four unknown/175 x success/-EIO cells,
load counters, private_data, original target errno, plus legacy status writes on
success/error. Fixtures remain synthetic; no initialized CCI claim follows.

## Evidence

Baseline evidence is the retained full-chain-test directory and its report:
four status assertion failures, recursive START exit 124 at 30 seconds with the
blocked `synchronize_srcu` stack, and a passing direct control. It stands as the
deterministic BEFORE; no baseline reproduction was overwritten.

AFTER outputs: `llumik:/home/u/gpda-devfreq-fixes-20261030`, copied to the outer
`artifacts/cpufreq-phase3-devfreq-deadlock-status-fixes-20261030/` directory.
Normal outputs reuse the previous incremental ARM64/UML output trees, with new
logs in the new directory. Source is `/dev/shm/gpda-cci-tests-5162/src` with eight
explicit overlays. Verification checks 93,788 unchanged blobs against the exact
base and hashes all eight overlays. These are source-identified incremental
builds, not a claim of clean full-tree reproducibility. KASAN uses a separate
fresh output `/dev/shm/gpda-devfreq-kasan`.

An initial build-container attempt lacked QEMU (VM exit 127, not a kernel test
result). Execution was corrected to the existing `gpda-combined` image,
`sha256:6ba6d52c62d0f84ccd1144b89a6f1fccf7844bbbef7085230d75ede736e9ad2e`.
Final logs and manifests identify the successful runs.

## Remaining full-chain acceptance

The next integration fixture requires an actual protected CCI initializer and
teardown path (still absent behind probe's lease-release/`-EAGAIN` gate), selected
only by a KUnit admission hook while production admission stays closed. It must
acquire and initialize the real lease, clocks, outcome cache, profile, governor
and devfreq lifetime, then drive CPU0/CPU4 POST through passive into that actual
CCI object, with two later CPU observers and no retained CPU transaction lock.
A hand-built devfreq or a test-only invented initializer does not establish this.

Then cover rounding/non-monotonic voltage, intermediate/unknown outcomes, original
errno and ownership, same-rate retry/latched refusal, prepare/switch versus
consumer/provider removal, stale-cookie rebind, peer withdrawal, OPP recursion
and deferred work lifetime. Deterministic pauses around START publication,
POST-before-cur commit and a new transition between wait and locking also remain
unimplemented; preserving the algorithm and passing sequential tests is not
proof of those interleavings. The new STOP drain test closes only its bounded
framework race. No full-chain, thermal containment, A72/buckB or physical audio
acceptance is claimed.

## Related fixture defect found during final verification

One final normal recursive VM run failed an earlier assertion in
`transition_clock_contract` at provider-test.c:1939: expected readiness `-EAGAIN`,
observed contention `-EBUSY`. The START regression itself completed. This is
retained in `preflight-readiness-race.log`, not silently counted as passing.
History shows the original assertion in `bddb4f6ee57c` and the later hold fix in
`b188a477b6358`. Source review found a remaining scheduling window: the event
queues the held preflight worker, which still takes transaction_lock before
checking the hold flag, while transaction begin uses trylock. The minimal
fixture correction flushes that worker after the event, while hold remains set,
before asserting readiness. It neither accepts another errno nor alters
production behavior. Subsequent full validation uses this correction.

## Final validation results

* W=1 ARM64 Image, UML, and separate fresh KASAN ARM64 Image builds succeed.
  This is not a warning-free-build claim: the fresh KASAN log retains existing
  ARM64 syscall override-initializer and passive state kernel-doc warnings.
* Normal ARM64: outcome 3/3; status contract 1/1 (all four cells plus legacy
  compatibility); provider/direct 23/23; provider/recursive 23/23. All exit 0.
  Recursive START prints four bounded `-16` returns and transition completion.
* UML: seven suites, 38/38 cases pass, no skipped cases. The ARM64-specific
  CPUFreq reentry hook is not exercised by UML.
* Seven combined OF/NVMEM/provider configurations (0:0, 1:0, 0:1 through 0:5)
  pass. DTBs remain the hashed immutable inputs from the earlier matrix.
* KASAN_GENERIC plus PROVE_LOCKING/DEBUG_LOCK_ALLOC: outcome/status 4/4,
  direct provider 23/23, recursive provider 23/23. All complete within the
  60-second VM bounds, including STOP drain/restart. Normal VMs use 30/45-second
  bounds. Runtime-summary checks completion and absence of `not ok`, BUG,
  WARNING, KASAN and circular-lock markers in all final logs; exit 0 alone is
  not treated as KUnit success.
* Strict code checkpatch: 0 errors, 0 warnings, 0 checks, 623 lines checked.
  All eight source hashes match local files; unchanged base-blob verification
  passes. Configs, commands, complete logs and DTB/source/binary manifests are
  retained with the artifacts. Final binaries are additionally retained under
  the remote evidence directory's `binaries/` rather than only build outputs.

Final SHA256 identities:

| Binary | SHA256 |
| --- | --- |
| ARM64 Image | `9f85e07cb4a4628dfb6e1930a1d0e0c19135c17832b5db546c27256eec8ad25c` |
| UML | `bf9c238e6ac251f6ff9ace2eb458858f81e60027d8c917b174b4cc0c91971c7f` |
| KASAN ARM64 Image | `1d263bd48dc62b3e399dda4c427960c0fb6b49ee905542bfcf37f2cfd08f8914` |

See `source-verification.log`, `binary-final.sha256`, `kasan.sha256`,
`dtb.sha256`, `runtime-summary.txt`, `validate-final.sh`, `kasan-run.sh`,
`regress.sh`, and `summarize.py` in the retained evidence directory. Self-review
covered callback context lifetime, absence of synchronous unregister on the
recursive rejection path, preserved publication/normal-drain ordering, status
callers and legacy error behavior. Independent review remains required.
