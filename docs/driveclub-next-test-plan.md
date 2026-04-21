<!--
SPDX-FileCopyrightText: 2026 Fabio Akita
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Driveclub race-start blackout — test iteration plan

Short-lived working doc. Lives on `gamma-debug`. Kept so we don't forget
any item between builds. Each iteration is one patcher run + one launch;
each hypothesis bucket is a discrete bullet so ruling it in or out is
unambiguous.

## Vanilla vs. current-patched baseline

Vanilla (unpatched track pack, just the MSAA fix + dither on the
emulator side):

- race-start transition is a **semi-transparent crossfade toward
  almost-black**, never a solid 100 % blocker
- timing is highly variable (under 10 s to "never recovers")
- HUD stays alive throughout
- mirror goes dark with the main view

Current patched state (Iteration 1, already tested, saved in overlay):

- `PostFXConfig_*` inline scalars zeroed: `TemporalFade=0`,
  `MasterBrightness=1`, `ManualExposureLog2=0`, `ManualAutoMix=0`,
  `WeatherOverrideMix=0`, `MotionBlurLevel=0`
- `preracecam_*`/`new_preracecam_*`/`CutsceneCamera_*`/`WorldCamera_*`
  inline `Fade=0`
- prerace animationlib `Fade` tracks zeroed at key offsets
  `0x2c / 0x4c / 0x6c`
- India `posteffects` animationlib night-LUT references rewritten to
  day-LUT references

Observed result: the **semi-transparent crossfade is gone**, but now the
prerace window parks the "car selected / press-X-to-start" UI backdrop
at full alpha. HUD still alive. We broke the dimming curve; the overlay
composite mechanism itself is still running.

That tells us the composite carrier is not the `Fade` scalar we already
zeroed. It is something else higher up the chain.

## Candidates not yet ruled out

| # | Bucket | Why it's plausible | Where to cut |
|---|---|---|---|
| A | TXAA / temporal-history blending | `TXAAOverallWeight=0.7` default means 70 % of the frame is weighted toward history. The latched menu frame could be exactly that history buffer. | `PostFXConfig_*`: `TXAAOverallWeight`, `TXAAColourClamping`, `NonBloomedAttenuation` |
| B | Prerace camera `Transition` curve | Older `prerace_01..08` sequences have `Time → Transform → Transition → VFoV → Fade`. `Transition` is scalar-animated, untouched by current patcher. | prerace animationlib: `Transition` at the same offsets as `Fade` |
| C | PostFXConfig animated scalar overrides | `india_posteffects.lvl` animationlib keys `ManualExposureLog2`, `MasterBrightness`, `AutoTargetLuminance` — these **override** our inline actor zeros every frame. | `india_posteffects.lvl` animlib: zero keyframes for those curves |
| D | `Enabled = 1` gate on PostFXConfig | Nuclear. If we flip one actor to `Enabled = 0`, we see whether postfx gates the composite at all. | `PostFXConfig_exterior.Enabled = 0` (exterior view only, as control) |
| E | `globaldata.rpk` shared fade sequences | Previously unstable when patched; may not be the right target or may need inverted writes (1.0 instead of 0.0). Parked for later. | `globaldata.rpk` — do not touch this iteration |
| F | UI / menu overlay actor | The latched image is a menu UI — somewhere in `newui/` or `gui/` a panel is held over the prerace frame. | separate sweep; do not combine with A–C |

## Iteration 2 — A+B+C attempted, split after crash

### 2.0 first attempt (A + B + C combined) — **CRASHED**

Combined A/B/C build crashed Munnar at race-loading:

```
open /app0/data/leveldata/india_road_point_02_n.rpk
Critical: Unreachable code! Unhandled access violation at code address
0x80067894a: Read from address 0x0
```

Cause narrowed (by code inspection) to the two new animationlib-sweep
helpers, `PatchTransitionTracks` and `PatchNamedTrackKeyframes`. Both
reused the `Fade`-layout offsets (`0x2c / 0x4c / 0x6c`) blindly wherever
the ASCII field name appeared, which is unsafe for curves with a
different keyframe stride. The eboot's `.rpk` parser walked into
corrupted struct bytes and null-derefed.

Both helpers were removed from the active call sites. They remain in
the file as dead code so a future iteration with a proper keyframe-row
decoder can re-enable them.

### 2.1 split — bucket A only (current overlay state)

One patcher run, one launch, bucket A exclusively.

Target tracks:

- `india_road_circuit_01_r.rpk` (India reverse circuit — primary 19:30
  night repro)
- `india_road_point_02_n.rpk` (Munnar — secondary night repro)

Patcher additions (2.1 scope):

- [x] PostFXConfig_{interior,exterior,helmet}: **new** writes
  - `TXAAOverallWeight = 0.0`
  - `TXAAColourClamping = 0.0`
  - `NonBloomedAttenuation = 0.0`
- [ ] ~~Prerace animationlib Transition tracks~~ — deferred, crashed the
      game parser on 2.0; needs a proper per-sequence decoder
- [ ] ~~`india_posteffects.lvl` animationlib scalar curves~~ — deferred
      for the same reason

Do **not** touch:

- `globaldata.rpk` (fragile, parked)
- `PostFXConfig_*.Enabled` (reserved for Iteration 3)
- inverted writes (reserved for Iteration 3 fallback)

Expected outcomes and interpretations:

| What you see | What we learned |
|---|---|
| Race starts immediately visible, no overlay, no dim | A or B or C is the composite carrier. Next iter drops one at a time to isolate. |
| Overlay parks on a *differently-shaded* image (greyer / brighter / tinted) | Animationlib curves (C) were driving exposure on top of the overlay; TXAA/Transition still composit the overlay itself. |
| Overlay parks as before, same latched car image | None of A/B/C is the carrier. Move to D (Enabled=0 nuclear) in Iteration 3. |
| Boot crash | One of the three patches is corrupting structure. Narrow by dropping them one at a time. |

## Iteration 3 — UI backdrop transparency

Iteration 2.1 outcome: turning off PostFX decay (`TXAAOverallWeight`,
`NonBloomedAttenuation`) made the latched UI overlay **permanent
instead of gradually lifting**. That inverts the earlier framing — those
PostFX fields were not the overlay's source; they were part of the
decay that washed it out. The overlay source must be a UI layer that
stays `ACTIVE` during prerace. Bucket A patches reverted; overlay is
back at Iter 1 baseline.

Targeted file for Iter 3:
`newui/panels/vehicle_select_background.txt`. This is a 1922×1082
full-screen panel, loaded as the background for every vehicle-select
page (`vehicleselect.ctl`). If the page system does not clear its
`ACTIVE` state when handing off to race, its gradients keep rendering
under whatever the race draws.

Patch shape: drop a text-identical replacement in the overlay with
every `GRADIENT`'s `A_RGBA` and `B_RGBA` alpha channel set to `0.000`.
The panel stays structurally intact (ACTIVE, dimensions, transform all
preserved) so the vehicle-select page's layout logic still works — it
just composites nothing visible. If this is the overlay source, the
race start should go visible immediately without the static-car
backdrop.

Overlay tree work: `newui/` can no longer be a single symlink into the
installed game. Replaced it with a real dir; every subdir except
`panels` is symlinked back to the install. `panels/` is itself a real
dir; every `.txt` except `vehicle_select_background.txt` is symlinked
back. Only the one file is a real patched copy.

Expected outcomes:

| What you see | What we learned |
|---|---|
| Race visible immediately, no backdrop | `vehicle_select_background` was the overlay source. Narrow further to identify which gradient(s) were visible. |
| Overlay still there, looks identical | Wrong backdrop — try `x_vehicleselect.txt` full panel transparency next, or look at `loading_freeplay.txt` / `pause_background.txt` / `backgrounds.txt`. |
| Overlay slightly dimmer / different shape | Partial match — we killed some gradients but there's another layer on top. |
| Car-select menu itself looks broken | We nuked too much. Only a problem if we hit the menu; it's intended to still work normally. |

## Iteration 3.1 — surgical image_track + backgrounds.txt (MISSED)

- Set `loading_freeplay.txt::image_track.ACTIVE = FALSE`
- Set `backgrounds.txt::BG_RGBA` alpha to `0.0`

Result: **identical behavior to Iter 1**. The static car image is still
shown, semi-transparent, with the race rendering underneath but very
dark. Takes ~30 s (or more) to lift.

Conclusion: neither of those two panels carries the overlay or the dim
layer. The car image comes from somewhere else.

## Iteration 3.2 — broad loading + pause-background disable

All `loading_*.txt` top-level PANELs and `pause_background.txt`
disabled (root PANEL `ACTIVE FALSE`) via overlay text files:

- `loading_challenge.txt`, `loading_fge.txt`, `loading_freeplay.txt`,
  `loading_frontend.txt`, `loading_gamescomdemo.txt`,
  `loading_kiosk_demo.txt`, `loading_privatelobby.txt`,
  `loading_replay.txt`, `loading_tour.txt`, `pause_background.txt`
- `backgrounds.txt` kept at `BG_RGBA` alpha `0` from 3.1

Rationale: if *any* of these was the carrier, 3.1 missed it because we
only patched one specific file or one specific sub-element. 3.2 takes
the hammer to every plausible fullscreen UI backdrop at once. Expected
cost: loading screens render blank for a few seconds during actual
loading. Acceptable for this iteration.

Expected outcomes:

| What you see | What we learned |
|---|---|
| Race immediately visible, no car backdrop, no dim | The carrier is one of those panels. Narrow by enabling them one at a time. |
| Car image and dim both gone, loading screen blank | Same, but note that loading also went blank — fine. |
| Overlay/dim unchanged | Carrier is not in `newui/panels/` at all. Next vector: `gui/` resources, or the game is rendering directly (3D model from vehicleselect, or engine-level frame capture). |
| Boot or race crash | One of the disabled panels is load-bearing somewhere else. Narrow by re-enabling one at a time. |

## Iteration 4 — fallback branches (only if Iteration 3.2 inconclusive)

- [ ] **D**: `PostFXConfig_exterior.Enabled = 0` only. Interior/helmet
  stay normal — acts as a within-frame A/B. If exterior clears and the
  other views still show the overlay, the postfx chain is the gate.
- [ ] Inverted fade write: where we currently write `0.0`, write `1.0`
  instead. This tests whether 0.0 is a "game-invalid" value that
  bypasses the fade path via a null-check rather than actually disabling
  it.

## Iteration 4+ — different composite source

If D and inverted writes don't move the needle, the composite carrier
isn't in the track pack. Pivot targets:

- [ ] UI rpks (`newui/`, `gui/`) — find the prerace "press X" backdrop
  actor, zero its alpha / delete its render.
- [ ] `globaldata.rpk` revisited with surgical single-sequence writes
  and 1.0 inversion, one sequence at a time, instead of the broad
  multi-sequence shotgun that was unstable.

## Launcher

- Run via `scripts/run_driveclub_overlay.sh` (added this iteration) so
  the emulator points at
  `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin`.
  The overlay dir is a symlink tree of the real install with our
  patched `.rpk`s dropped into `data/leveldata/` — launching from there
  makes path resolution pick up the patches without touching the real
  install tree.
- The existing `scripts/run_driveclub_live.sh` continues to run against
  the real install for vanilla A/B comparison.
- `SHADPS4_DC_TORTURE` is **not** set for these runs. The emulator-side
  probe hook is compiled in but dormant; we're exclusively testing
  asset-side changes now.

## What we promised ourselves

- Never accept "fade is unavoidable" as an outcome. Each run that comes
  back inconclusive produces the next iteration's hypothesis.
- Variable fade duration is not correlatable on its own. Run each
  iteration with 3–4 race starts at 19:30 clear weather to see whether
  the visual result is stable across retries.
- Keep this doc updated in place. When an iteration closes, mark its
  bullet checked and record the observed result in one line under its
  table row.
