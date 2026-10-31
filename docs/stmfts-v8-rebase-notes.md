# STM FTS5 (stmfts) v8 rebase notes

Branch `touch/stmfts-v8` = `obs/sm7150` with David Heidelberg's FTS5 series
**v8** (lore `20260925-stmfts5-v8`) imported in place of the ~v4 FTS5 commit
`obs/sm7150` carried, and our local follow-ups reworked on top.

Final series (`git log --oneline obs/sm7150..touch/stmfts-v8`):

    d3097ba  dt-bindings: input: touchscreen: st,stmfts: Import v8 of the FTS5 series
    10bb501  Input: stmfts - import v8 of the FTS5 series
    82eb2d0  Input: stmfts - process the whole FTS5 event batch
    31a705a  Input: stmfts - enable runtime PM before registering the input device
    dd3666e  Input: stmfts - don't power off a controller that is already off
    91f0ce8  Input: stmfts - register as DRM panel follower for in-cell touch
    1aae348  Input: stmfts - re-run the wake sequence on panel prepare
    53d3fb5  Input: stmfts - track panel-follower state for system sleep
    (+ this docs: commit)

## Disposition of the obs/sm7150 follow-ups

Mapping from each local stmfts commit on `obs/sm7150` to its state on v8.

|obs commit|disposition on v8|
|-|-|
|`74b045c` process the whole FTS5 event batch|**Ported** as `82eb2d0`. Reworked onto v8's parser: `STATUS_UPDATE`/`USER_REPORT` now `continue` (keep draining the 32-event batch) instead of `goto sync`; `DEBUG` still syncs.|
|`ac7948` guard against NULL match data|**Dropped, redundant.** Both OF entries and both `i2c_device_id` entries carry an ops table and v8 already resolves via `i2c_get_match_data()` (OF table then id table), so every path that can bind the driver yields non-NULL `sdata->ops`. The guard would be dead code.|
|`eec03f` balance regulators in panel-follower mode|**Dropped, folded in.** Its two effects already exist on v8: the `powered` idempotency flag is `dd3666e`, and the `devm_add_action_or_reset(stmfts_power_off)` is registered unconditionally (both modes) in `91f0ce8`, before the follower/power-on branch.|
|`be938e` use i2c_get_match_data()|**Dropped, redundant.** v8's import already uses `i2c_get_match_data()`; there is no `of_device.h` include left to drop.|
|`bcce52` fix panel-follower PM tracking and probe ordering|**Split.** Probe-ordering half (enable runtime PM via `devm_pm_runtime_enable()` before `input_register_device()`, drop `.remove`) is `31a705a`. Runtime-PM gating on `sdata->powered` is in `dd3666e`/`91f0ce8`. The remaining PM-tracking half — latch `sdata->is_follower`, fall back to self-power on `-ENODEV`, gate the system-sleep ops on `is_follower` instead of `drm_is_panel_follower()` — is **ported** as `53d3fb5`.|
|`d7ea0d` drop the stmfts5 fallback|**Dropped, redundant.** v8's binding already has a plain `compatible` enum (`st,stmfts` / `st,stmfts5`) with no `"st,stmfts5","st,stmfts"` fallback.|
|`23bfafd` allow the stmfts5 fallback|**Dropped, superseded.** There is no fallback on v8 to allow; this was the predecessor of `d7ea0d`.|
|`e988109` document the panel property|**Dropped, redundant** (confirmed by `dtbs_check`, rc=0). v8's binding inherits `panel` from `touchscreen.yaml` via `allOf: $ref` with `unevaluatedProperties: false`, so the sunfish `panel = <&panel>` validates without an explicit `panel: true`.|
|`30b8525` Kconfig: depend on DRM||!DRM|**Dropped, folded in.** The `depends on DRM || !DRM` line is added together with the DRM-follower code in `91f0ce8` (keeps bisectability).|

Not a follow-up but noted: `91f0ce8` is the rework of the old obs "register as
DRM panel follower" commit onto v8; FTS5 event-ID masking is already part of
v8's import.

## Differences from the old (v4-ish) FTS5 that matter

- **v8 1/5 "wait for controller ready after reset" (kept, load-bearing).**
  `stmfts_power_on()` now enables the IRQ right after releasing reset and
  waits (up to 300 ms) for the controller-ready event instead of a fixed
  50 ms sleep. On sunfish both rails are cut on panel power-down, so the
  controller boots cold on unblank; the fixed sleep was too short and touch
  died after the first display-off. Every power-on path (probe, system
  resume, and the panel-follower `.panel_prepared`) goes through this, so the
  fix applies in follower mode too.
- **Match data:** v8 uses `i2c_get_match_data()` (OF + i2c id table).
- **Binding:** `st,stmfts5` is a standalone compatible, no fallback;
  `mode-switch-gpios` is optional for `st,stmfts5` and rejected otherwise;
  `panel` is inherited from `touchscreen.yaml`, not re-listed.
- **Our panel-follower additions on top of v8:** power the in-cell controller
  from the DRM panel's prepare/unprepare; make `stmfts_power_off()` idempotent
  (`powered`); gate runtime-PM ops on `powered`; re-run the wake + scan-start
  sequence on `.panel_prepared` so touch resumes with the evdev held open
  across a blank; enable runtime PM before registering the input device;
  track `is_follower` so a device that failed the follower add (`-ENODEV`,
  disabled panel node) still sleeps/resumes normally.

## Hardware test plan (Pixel 4a / sunfish)

Driver is `CONFIG_TOUCHSCREEN_STMFTS=m` (`stmfts.ko`). Watch `dmesg -w`.

1. **Cold boot touch.** Boot, then move a finger on the screen.
   - Expect: the evdev `stmfts` appears (`/proc/bus/input/devices`,
     `libinput list-devices`), and touch moves the pointer.
   - Expect no `failed to read...`, `failed to resume device`, or
     `FTS... timeout`/`-ETIMEDOUT` in dmesg from the reset wait.
2. **Driver bind sanity.** `dmesg | grep -i stmfts` right after boot.
   - Expect a clean probe (no `-EPROBE_DEFER` left pending, no
     `no chip match data`), and the panel-follower registered (controller
     powers on once the panel is prepared).
3. **Screen off/on resume, x10.** Blank and unblank the display ten times
   (power button, or `wlopm`/DPMS), waiting ~2 s each, with a touch check
   after every unblank.
   - Expect: touch works after *every* unblank, not just the first.
   - Expect no regulator imbalance warnings (`unbalanced disables for
     vdd/avdd`) and no IRQ-depth warnings on blank.
   - This is the exact regression the v8 reset-wait + `.panel_prepared`
     wake-sequence fix target.
4. **Suspend/resume.** `systemctl suspend` (or `rtcwake -m mem -s 20`),
   resume, touch check.
   - Expect: touch works after resume. In follower mode the panel drives the
     power cycle, so `stmfts_suspend/resume` are no-ops; no power-off/on
     warnings from the system-sleep path.
5. **Open/close across blank.** Keep a reader open (`evtest /dev/input/eventN`),
   blank and unblank the screen.
   - Expect: no `failed to disable touchscreen` / `failed to enable` spam
     while the panel is off (the set-scan calls are skipped when `!powered`),
     and events resume after unblank.
6. **Unbind / rebind.** With the screen **on**:
   `echo <i2c-addr> > /sys/bus/i2c/drivers/stmfts/unbind` then `...-/bind`
   (addr e.g. `3-0049`).
   - Expect: unbind powers the controller off exactly once (no
     double-disable regulator warning — the `powered` guard), input device
     disappears; rebind brings it back and touch works again.
   - Then repeat with the screen **off**: unbind must not warn about an
     already-off controller, rebind must defer power-on to the next
     `.panel_prepared`.
7. **No use-after-free on unbind.** Run unbind/rebind a few times with
   `CONFIG_DEBUG_KOBJECT_RELEASE`/KASAN if available.
   - Expect: no KASAN use-after-free around `panel_follower` or the IRQ
     thread; the follower is removed (devm) before the input device and the
     power-off action.

### dmesg lines to expect / not expect

- Expect (debug): `initializing ST-Microelectronics FTS...` once per probe.
- Do **not** expect: `failed to resume device`, `failed to read status`,
  `unbalanced disables`/`Underflow of regulator enable count`,
  `no chip match data`, or `pm_runtime_resume_and_get ... -EACCES` from an
  evdev open racing probe.
