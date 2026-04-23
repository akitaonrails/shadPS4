## Munnar track-pack findings

Primary repro track:

- India `Munnar`, pack `india_road_point_02_n`
- user repro settings: `19:30`, clear weather

The Munnar pack is the first asset-side target that produced a stable,
repeatable change in blackout behavior without breaking boot.

### What is inside `india_road_point_02_n.rpk`

The Munnar probe found these relevant resource families:

- prerace camera level data:
  - `india_road_point_02_n_prerace_cams`
  - `preracecam_*`
  - `new_preracecam_*`
  - `track_preview`
  - `WorldCamera_*`
  - `CutsceneCamera_*`
- postfx level data:
  - `india_posteffects`
  - `PostFXConfig_exterior`
  - `PostFXConfig_interior`
  - `PostFXConfig_helmet`
- animation libraries:
  - prerace camera animationlib from `india_road_point_02_n_prerace_cams.lvl`
  - postfx animationlib from `india_common/india_posteffects.lvl`

Most important new finding:

- the prerace animationlib contains explicit sequences
  `prerace_01` through `prerace_08`
- those sequences carry animated `Fade` tracks with keyed values
  ramping to `1.0`
- this is the first concrete track-local blackout-style animation found
  in the game assets

Additional narrowing from the probe:

- `india_posteffects` depends only on:
  - `PostFXConfig_exterior`
  - `PostFXConfig_interior`
  - `PostFXConfig_helmet`
  - one local animationlib (`0x0046EAB21E3CF9E7`)
- the local postfx animationlib contains grading/exposure tracks such
  as:
  - `india_dc_grade_exterior`
  - `india_dc_grade_interior`
  - `SunElevation`
  - `AutoTargetLuminance`
  - `ManualExposureLog2`
  - `ManualAutoMix`
  - `AutoMaxLuminance`
  - `AutoSpeed`
  - `MasterBrightness`
  - `MotionBlurLevel`
  - `ColourRemapVolumeName`
- this means the Munnar postfx branch is largely self-contained inside
  the track pack; it does **not** obviously bind to `globaldata`
  `FadeIn/FadeOut` sequences through the `india_posteffects` resource
  itself

### What was patched in the stable Munnar tests

The track-local patch set that boots reliably included:

- all actor `Fade` values zeroed for:
  - `preracecam_*`
  - `new_preracecam_*`
  - `track_preview`
  - `WorldCamera_*`
  - `CutsceneCamera_*`
- all `PostFXConfig_*` inline `TemporalFade = 0`
- several other inline postfx scalar neutralizations
- India night LUT references swapped to day LUTs
- the prerace animationlib `Fade` tracks zeroed directly

### What that changed at runtime

This did **not** remove the blackout itself.

What it changed reliably:

- the image *under* the blackout stopped looking like the normal race
  frame and instead latched the prior car / `Press X to start event`
  screen
- HUD stayed alive
- game audio continued
- mirror still went black during the blackout interval

Current stable conclusion for the Munnar pack:

- `india_road_point_02_n.rpk` controls what image gets latched or
  persists under the blackout
- it does **not** appear to own the blackout master switch itself
- zeroing track-local camera fades and even the explicit prerace
  animation `Fade` tracks is not enough to disable the blackout
- the local postfx animationlib looks responsible for track-specific
  exposure / grading, but not for the blackout master switch

This is an important split:

- track-local prerace assets affect the visible content under blackout
- the blackout timing/switch likely lives in a shared layer above or
  alongside the track pack
