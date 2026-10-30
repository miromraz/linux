# MT6797 protected regulator ownership implementation — 2026-10-30

Implemented real regulator-core protection and generation-tagged consumer lifetimes. Operational Phase3 admission remains closed. This is a software implementation for NEW independent review, not hardware approval or proof of the complete OF/NVMEM discovery integration. Hardware HOLD remains; recovery/accounting/thermal/A72 operational work is not admitted.

## Source and commit boundary

Started at kernel `24bdf4ff9baac0803fced3186cc9a6797cb87c00`, whose rail baseline was `d50f92317fb8`. Read the lifecycle-completion record and independent consumer-handover review completely, plus the handover implementation report and applicable instructions. Inspected regulator-core request rollback, registration and request aggregation, the reservation/replace engine, CPU initialization/release and group deduplication, CCI initialization/remove and supplier notifications. Relevant history remains d50f92317fb8 / 9c50e50b34db / 4c404604a1fc; no model provenance is inferred.

At the user's subsequent explicit coordination request, committed the independently approved, unchanged fifteen audio paths separately as `56f8572d4f132b39390472f213087b1c5a821eb2`. Verified every live and staged sound/test hash against the approved manifest and verified that the index contained only those paths. The approved audio patch hash is `d369935ee8204ccea2ac0eebfe9a48ed9f4b2674d8e24859192594d218162fe9`. Audio approval and its five-suite independent execution belong to `docs/audio-kernel-v6-final-independent-review-2026-10-30.md`; this rail work does not claim an audio rerun or physical audio verification. Rail's commit parent includes that separate audio commit. Input work was neither changed nor staged.

The focused rail paths are `drivers/regulator/mt6797-provider.c`, its header and tests, `include/linux/regulator/mt6797.h`, `drivers/cpufreq/mediatek-cpufreq.c`, and `drivers/devfreq/mtk-cci-devfreq.c`. This report is mirrored under kernel `docs/` so it is included in the same focused rail commit.

## Real core requests and durable ownership

Three private logical regulator devices represent the separate CPU0, CPU4 and CCI requirements on the shared proc rail. Each has a real regulator-core guard handle; adopted users have a second real core handle and voltage request. The raw adapters remain the only physical writers, reached through the existing shared-rail engine. These logical supplies are not competing independent hardware regulators and are not exposed through the normal operational CPU/CCI supply paths.

All three boot guard requests are installed at the selected physical proc voltage before adoption can lower any guard. Initial rail conversion only raises rails; it does not lower boot protection before those requests exist. A fixed-SRAM logical regulator and permanent core guard request own the 1.20 V requirement after conversion. Registration does not normalize the physical rails through an independent writer.

Acquisition first installs the new consumer request, explicitly validates the selected rails and current framework/provider generations even if regulator core skipped an unchanged setter, and only then lowers the old proc guard request. A failed first core vote is put only after core rollback; the boot guard survives. If a later acquisition step fails, any installed consumer handle remains in registry storage. No caller-local or devm storage owns these handles.

Return installs a guard covering both the fresh live/intermediate floor and the departing consumer's previous request, verifies it while the old consumer request still exists, and only then puts the consumer handle. Return deliberately retains the old request's voltage instead of performing a discretionary lowering. This avoids relying on metadata or on rollback after an uncertain physical write. The last driver lease withdraws its consumer handle, not the guard for still-running hardware. Failed return retains the consumer handle, device reference and registry clock/OPP/configuration ownership. An already-consumed local cookie cannot release that retained ownership again.

Every acquisition allocates an independent monotonic cookie. Duplicate CPU-group users share the underlying regulator request and device reference but receive different cookies; only the last valid release transfers ownership. Stale and duplicate release are rejected before accounting changes. CPU callers use canonical devices 0/4 and validate the OPP sharing mask against the selected four-CPU group.

The registry's root device and requests are non-devm. A module reference is deliberately retained once protected supplies are created: boot clocks and their guards outlast driver bind attempts, so ordinary coordinator module unload is prevented while this protection exists. Raw provider unbind still drains and removes raw callbacks under the existing owner/engine locks; logical descriptors contain no pointer to raw-provider devres. Provider loss invalidates transactions without discarding core requests. Rebind does not automatically clear faults or retained-error ownership; that requires later explicit recovery, not a readiness flip.

## Entry points, admission and lock ordering

`mtk_cpu_dvfs_info_init()` now establishes the protected lease and group mask without duplicating the registry's OPP/configuration ownership. The probe loop deduplicates siblings normally. `mtk_cpu_dvfs_info_release()` is the shared secure-return boundary used by successful protected initialization followed by later initialization failure or the closed publication gate. The protected loop does not enter the unadmitted A72/B initialization path. The non-Gemini branches and provider B code are unchanged.

`mtk_ccifreq_probe()` acquires a protected lease in local state and uses `mtk_ccifreq_release_protection()` on its closed-admission unwind. `mtk_ccifreq_remove()` uses the same helper for protected state. Registry ownership survives destruction of that local devm allocation. No devfreq/cpufreq callbacks are published for the protected Gemini path. Normal CCI consumer-driver unbind no longer permanently revokes the clock/NVMEM suppliers; actual device deletion and supplier loss still revoke protection. An absent CCI driver needs no ownership: its guard persists. Missing CCI device/discovery still defers preflight safely.

`transaction_lock` serializes full ownership operations and shared-user accounting. Regulator notifications run without OPP snapshot locks or CCF reservations. The regulator setter takes a fresh reservation of all six clocks and all three OPP tables *after* PRE notification, then checks preflight/provider generations under `owner_lock` before engine/raw access. Reservations are released before POST/ABORT notification. Explicit post-call reconciliation repeats that validation for unchanged core requests and notification-induced generation changes. No regulator-core API runs under owner/engine/raw locks. The active setter context is restricted to the transaction's task, so unrelated callers cannot use a temporarily valid transaction context to program a logical supply.

## Executed validation

All builds, tests and lint ran on llumik. No dev builds/tests/lint, subagents, shell worktrees, hardware/HP access, hardware locks, flashing or invented DCO signoff.

Snapshot: `/home/u/gpda-ownership-20261030`, a reflink copy of the preceding handover snapshot, updated with the rail source and approved MFD commit files. Audio V6-final and input work are not included in this rail build snapshot. These results are not a combined release kernel validation.

Container: `gpda-kbuild:trixie`, image `sha256:c14e8d674cd56085c3bf14aca316cac84480c07e418b9a28e085af87e8bf9594`.

Evidence: `artifacts/cpufreq-v11-regulator-ownership-20261030/`. `validate.sh` preserves exact commands. The final run explicitly removed the six ARM64 target objects and the relevant UML registry/engine/adapter/test objects before rebuilding. Output directories otherwise retain prior configuration/build products; these are not clean full-tree builds.

* ARM64 W=1 rebuilt registry, coordinator, DA9211, SRAM, CPU and CCI objects successfully. This is object compilation, not a full ARM64 Image/modules link or boot.
* UML W=1 linked and booted successfully: 24 passing cases in five suites, normal halt. The provider suite has 13 cases, including four production-ownership variants. Existing real adapter, regulator-core and engine suites also pass.
* Production ownership tests use the public acquire/release API, real regulator-core handles and requests, three distinct dynamic OPP tables and CCF reservations. They verify unequal simultaneous requirements, duplicate group users, independent cookies, stale releases, last-owner return, absent-driver protection, late CCI acquisition/reacquisition, provider detach/rebind with retained ownership, first-vote failure, commit-then-error return, and competing acquisition threads.
* The notifier-generation test changes a real OPP voltage during regulator PRE notification. It completes without a snapshot-lock deadlock, rejects the stale transaction and performs no physical write. Core request rollback preserves the existing guard.
* Assertions inspect actual regulator-core per-handle requests, not just engine floors. Device-reference counts increase exactly once per owned group, return to baseline on normal return, and remain elevated on retained error. Every normal synthetic physical write checks all three live floors and the proc/SRAM envelope. The commit-error case separately checks the selected physical rail after failure.
* Final code checkpatch: zero errors and zero warnings, with `--no-signoff`.

Exact selected identities:

```text
UML linux/vmlinux: a04531a8b703936689b748105aeddaf1469942444877a0536d8042eb7fd3f700
ARM64 registry object: aee6a487aaf2eea2302ed0b0f9f7b411d0bcb68f80394213020b4e6ad277f15f
ARM64 CPU object: 385799a9555fbead437f0c71101654e6a4b0834b0a7fcb88ca326e31460705f7
ARM64 CCI object: c9897dda3e9681b2a79d224c102cea1e0575b338a0d84044da5bedf5d8ade009
UML config: 1d48910da26c62c568152802c1b5af64f3cd898a7b38def69f32ac9abaf95911
ARM64 config: daca82084cd96be480a9c5449aeec89105410e24346ecfbfc6efc2734acbacc3
```

`source.sha256` identifies 21 relevant source files; every entry matches the remote snapshot. Both configurations, full build/run logs, binary hashes and patch are retained. This selected-source verification is not a whole-tree provenance assertion.

## Independent-review boundaries

The ownership fixtures inject discovery and raw transport, retain the existing single fixed test clock, and invoke the shared production lifecycle API. They do not execute real OF CPU enumeration, the static CPU/CCI probe/remove callbacks, real devres release, all NVMEM arrival orders, or both actual raw adapters together in one Gemini-shaped fixture. Those integration coverage gaps remain explicit; separately passing adapter suites do not close them. Concurrent acquisition and synchronous notification mutation are exercised, but exhaustive paused-clock/provider/OPP mutation interleavings, lockdep/KCSAN, and allocation failure at every setup boundary are not claimed.

The implementation must receive a NEW independent rail review. Permanent guard/module retention and conservative no-lowering return are intentional protected-lifecycle policies. Operational recovery, thermal/accounting contracts, A72 admission, electrical settling and firmware exclusion remain outside this increment. No hardware readiness or functional audio claim follows from these tests.
