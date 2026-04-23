## Probe direction after the asset sweep

Do **not** default back to another generic renderer-side shotgun.

The best next brute-force direction is now the track-side prerace/postfx
family:

1. Extract one race track `.rpk` and inspect the `preracecam` /
   `PostFXConfig_*` resources directly.
2. Add a broad runtime probe aimed at the prerace/postfx handoff rather
   than the final color chain.
3. If a runtime hook is feasible, neutralize the likely scalar family at
   once:
   - `TemporalFade`
   - `MasterBrightness`
   - `ManualExposureLog2`
   - `AutoTargetLuminance`
   - `AutoSpeed`
   - colour-remap / grading overrides
