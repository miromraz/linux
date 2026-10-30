# Phase3 owned transactions — 2026-10-30

## Status: NOT IMPLEMENTED; admission CLOSED

This change is a prerequisite correction, not the requested owned-vote
operational transaction slice. CPU0/CPU4/CCI do not yet execute registry-owned
operational transactions. Do not accept this report as completion of that task
or the independent review's P2 matrix. No external access/build blocker was
encountered. The implementation scope remains unfinished.

Base: 3f9612cc08bbfcef728337d7ff9946ee2df08457. Read the full independent
clock-acceptance review and Group4 acceptance contracts. No subagents, hardware,
HP, device locks, flashing, playback or capture. Approved audio/MFD/key/NVMEM
files and production admission gates were not changed. No DCO is asserted.

## Implemented subset

CCF now offers `clk_set_parent_tracked()`. Its required output is false before
reparenting is attempted, including first-parent PRE veto, and true once
reparenting may have touched hardware. Provider failures are uncertain even
when the synthetic provider happens to reject without committing. The original
`clk_set_parent()` behavior is preserved through a wrapper. This is a per-call
programming boundary, not a transaction reservation, hardware observation, or
proof against mutation by other users/notifier callbacks.

The shared DVFS helper reports unchanged pre-write rejections. CCI retains its
successful preparation request and valid old cache for these rejections and
permits a subsequent target attempt. Positive CCF notifier refusals are mapped
to EBUSY at the helper boundary. Other failures still retain the existing fault
policy. This fixes the clock-preflight/first-parent-veto portion only: regulator
preparation errors cannot yet distinguish rejection before mutation from
partially committed dynamic tracking and still latch. The P2 is not fully closed.

CPU/CCI OPP voltage notifiers return NOTIFY_OK after recording local failure,
so an error does not stop later listeners. CPU now also blocks subsequent
transitions after failed current-OPP voltage reconciliation and refuses voltage
updates when already faulted. These corrections apply to non-Gemini platforms
because their OPP API has the same commit-before-notification semantics.
`dev_pm_opp_adjust_voltage()` still returns success after committing the table;
no claim is made that a reconciliation error reaches that caller or rolls back
its table. Notification/API/table/physical-state integration tests remain due.

## Executed finite evidence

All builds, lint and execution ran on llumik. Remote artifact root:
`/home/u/gpda-phase3-20261030/`; local retained logs/manifests/patch:
outer workspace `artifacts/cpufreq-phase3-owned-transactions-20261030/`.

* W=1 UML and ARM64 Image incremental builds passed with gpda-kbuild:trixie.
  Final build logs have no compiler warning/error matches. Existing output
  trees were reused; this is not a clean or reproducible build claim.
* UML full run: 31 cases passed, no failures/skips, normal halt, exit zero.
  The changed CCF test verifies PRE veto produces zero mux writes and an
  unchanged result, followed by success; provider rejection produces an
  uncertain result. Prior helper up/down/intermediate/PLL/reconnect cases pass.
* Seven existing Group4 QEMU cases passed (0:0, 1:0, 0:1 through 0:5), with
  gpda-combined:trixie and the retained DTBs. No runner-matched warning, BUG,
  failed KTAP or lockdep diagnostics. These exercise ownership/probe boundaries,
  not operational targets.
* Remote checkpatch: zero errors, zero warnings for the six-code-file diff.
* All six modified code files match the remote source SHA256 manifest.
  Binary/config hashes are in owned-binary.sha256. UML linux:
  bc924a9a034fe6c7ea6bb00286af4c558582c65311b57cbc8a020f61d3dd7ce2.
  ARM64 Image:
  561e04713bf291b892c257b2f143f1cdb3c2013afdc60b71c2fc65c0390a19ad.

Selected manifests plus build logs tie the modified sources to the retained
outputs; they do not revalidate the full inherited source tree.

## Required continuation, not covered by these passes

The requested actual production CPU0/CPU4/CCI callback matrix was NOT run.
Non-Gemini CCI callback rejection/retry is NOT demonstrated by the helper test.
Owned old/intermediate/target requirements, persistent operational guard and
consumer requests, restored regulator-core requests/live floors, fixed-SRAM
operational transitions, serialized OPP refresh, transaction-wide generation
exclusion, synchronous-notifier concurrency, and bounded verified recovery
remain outstanding. Group4 exposes acquire/release/retained-owner recovery;
its setter re-reserves snapshots after PRE delivery. Extending that mechanism
requires a transaction contract rather than holding OPP table locks through
CCF callbacks or treating an aggregate rail reading as ownership.

Framework notifications/accounting remain incomplete: CPUFreq can publish
synthetic reversal and devfreq can publish old frequency after a changed error.
No new final-hardware-outcome interface or fake success was introduced. The
requested accounting-ready transaction result interface is still due.

Independent review of the completed operational slice, then truthful framework
accounting and thermal containment are required before admission. A72/buckB
firmware status, ownership and initial synchronization remain separate; no
boot frequency or physical acceptance is inferred. Uncertain mux errors still
cannot be resolved from CCF cached topology alone.
