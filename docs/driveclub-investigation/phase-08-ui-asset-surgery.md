## Phase 8 — UI-asset surgery (2026-04-21)

All the asset-side probes in this phase were scoped to Driveclub
`CUSA00003` v1.28 running through
`scripts/run_driveclub_overlay.sh`, with the patched `.rpk`s and UI
text panels dropped into
`/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003`. The
overlay dir is a symlink mirror of the installed game with only the
patched files as real content.

### Baseline on entry to this phase

From the prior asset-patch work:

- Track-pack `PostFXConfig_{interior,exterior,helmet}` inline scalars
  zeroed (TemporalFade, ManualExposureLog2, ManualAutoMix, Master-
  Brightness, WeatherOverrideMix, MotionBlurLevel).
- All prerace camera `Fade` values zeroed (old `preracecam_*`, new
  `new_preracecam_*`, `CutsceneCamera_*`, `WorldCamera_*`,
  `track_preview`).
- India night LUT references rewritten to India day LUTs.
- Prerace animationlib `Fade` track keyframes zeroed.
- `globaldata.rpk` kept at vanilla (prior patches there were
  unstable).

Observed effect of that baseline: the vanilla "fade to near-black" at
race start is **replaced by a semi-transparent static image of the
selected car** over the live race, with the race faintly visible
underneath and HUD intact on top. Same variable ~5-30 s duration
with occasional "never recovers".

### Iteration 2.1 — PostFXConfig bucket A (TXAA / NonBloomed)

`TXAAOverallWeight = 0`, `TXAAColourClamping = 0`,
`NonBloomedAttenuation = 0` on all three `PostFXConfig_*`.

Result: the overlay became **permanent instead of gradually lifting**.
This inverted the earlier framing. Those PostFX fields were not the
overlay's source; they were part of the decay that eventually washed
it out. Reverted. Kept `Iteration 2.0` attempted combined shot notes
and the learning in
`docs/driveclub-next-test-plan.md`.

### Iteration 2.0 — Transition/ManualExposureLog2 animlib sweep

`PatchTransitionTracks` and `PatchNamedTrackKeyframes` reused the
`Fade`-layout offsets (`0x2c / 0x4c / 0x6c`) blindly wherever the
ASCII field name appeared inside the animationlib resource. This
corrupted the keyframe stride of other curves and crashed the game's
`.rpk` parser during race load (null-ptr read in the eboot at
`0x80067894a`). Both helpers are still in `Program.cs` as dead code
behind guarded call sites for a future safer decoder.

### Iteration 3.x — newui panel sweep

The overlay persisted across every UI panel disable we tried. In
order:

1. `vehicle_select_background.txt` — set all gradient `A_RGBA` /
   `B_RGBA` alphas to `0.000`. No effect.
2. `loading_freeplay.txt::image_track.ACTIVE = FALSE`. No effect.
3. `backgrounds.txt::BG_RGBA` alpha `0`. No effect.
4. Broad nuke: every `loading_*.txt` plus `pause_background.txt` set
   to root-PANEL `ACTIVE FALSE`. No effect.
5. `get_in_car_animation.txt` set to root-PANEL `ACTIVE FALSE`. No
   effect.
6. `get_in_car_animation.txt` made `0×0` offscreen with `CLIP TRUE`
   and alpha `0`. No effect.
7. `freeplay.ctl` Page entry `getincar.freeplay` commented out.
   **Crash** — null-ptr read at `0x800285285` when the game tried to
   navigate to that page.

Reverted everything in 3.x once the file-open log confirmed the
emulator was reading the overlay files (774+ newui panel reads in
one session, every patched filename included).

### What that rules out definitively

- The overlay is not rendered through any `newui/panels/*.txt` file
  we have touched — and we covered every fullscreen panel that a
  search across the panel set identified (`WIDTH 192x` match, plus
  the pre-race / get-in-car / loading family).
- The `FreeplayGetInCar` controller (and its `TourGetInCar` /
  `ChallengeGetInCar` siblings) ignore the panel's `ACTIVE` flag,
  dimensions, RGBA alpha, and clip rect. Setting the panel to a
  `0×0` sprite offscreen with alpha 0 had no visible effect on the
  car render.
- Removing the Page registry entry for `getincar.freeplay` crashes.
  The page navigation graph is hard-required.
- The dim layer is not `backgrounds.txt`, not `pause_background.txt`,
  not a `loading_*` gradient, not a `vehicle_select_background`
  gradient.

### Where the overlay actually lives

Given the sum of the evidence above, the car-image overlay and its
accompanying dim layer are drawn by the `FreeplayGetInCar` controller
(compiled C++ code inside the eboot), not by the UI panel system.
The controller writes the 3D car render and the dim layer directly
into the main HDR / composite scene target addresses we identified
in earlier phases (`0x500cdd0000`, `0x5009688000`, `0x5008130000`,
`0x500fdd0000`). The UI `.txt` panels for those pages are carriers
for the page navigation graph, not the render surface.

This is consistent with an "engine-driven UI" architecture where the
declarative panel files define layout slots and the C++ controller
fills them by talking directly to the renderer.

### Dead ends attempted before escalation

We already ruled out `sceVideoOutAdjustColor` / host gamma as the
overlay carrier during Phase 4:

> stubbing out the gamma write and forcing presenter gamma to stay at
> `1.0` did not move the blackout

and forcing `Finish()` around the race-start window also failed. So
the "engine-side clear on gamma-pulse" hook that might look obvious
from the gamma=0.5 race-start signal has already been tried in a
different form and was a clean miss. There is no sceVideoOut-side
lever left to pull from the emulator side.

### Conclusion of the asset track

The asset-side surface we can reach from `.rpk` and `newui/*.txt`
patches is exhausted as far as this overlay is concerned. The
controller that renders it is not reachable through any of those
declarative files. The remaining escalation path is:

- binary-patch the eboot to neutralize the `FreeplayGetInCar` render
  path

See the next section for the plan.
