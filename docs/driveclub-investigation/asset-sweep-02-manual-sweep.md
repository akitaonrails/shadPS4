## Manual asset sweep: strongest new lead

A broad string sweep across the installed v1.28 game files found
something much more plausible than the previous renderer-side guesses:

- the actual race track packs contain explicit prerace camera actors
- those same packs contain explicit postfx config actors and exposure
  fields
- global data contains generic fade/postfx override tracks

This is the most important cluster found in the asset files so far.

### Evidence in track packs

The actual track `.rpk` files under `data/leveldata/` contain names such
as:

- `new_preracecam_*`
- `preracecam_*`
- `cutsccam`
- `worldcam`
- `pre_race_*`
- `track_preview`
- `PostFXConfig_interior`
- `PostFXConfig_exterior`
- `PostFXConfig_helmet`

They also contain explicit camera/postfx scalar names embedded directly
in the binary data:

- `ManualExposureLog2`
- `AutoTargetLuminance`
- `ManualAutoMix`
- `AutoMaxLuminance`
- `AutoSpeed`
- `MasterBrightness`
- `TemporalFade`
- `ColourRemapVolumeName`
- `FocusDistance2`
- `ApertureFNumber`
- `ShutterSpeed`
- `DepthOfField`
- `ShowDOFLerp`

This is not generic UI text. These strings live inside the race-track
resource packs themselves, next to the prerace camera actor data.

### Evidence in global asset packs and UI

`data/leveldata/globaldata.rpk` contains generic fade/postfx names such
as:

- `FadeIn`
- `FadeOut`
- `FadeIn_Fast`
- `FadeOut_Fast`
- `RTT_BLUR_ON`
- `RTT_BLUR_OFF`
- `RTT_GRADING_ON`
- `RTT_GRADING_OFF`
- `PostFX_ColourGradingOverride`
- `PostFX_BloomOverride_*`

The UI tree also contains ordinary page transitions like `BootFade` and
`FadeOut`, but those are almost certainly just menu/UI transitions and
should not be treated as evidence of the race blackout by themselves.

### Why this matters

This asset-side evidence fits the observed behavior better than the
previous renderer-side families:

- the blackout begins at race start rather than at final present
- mirror and main camera both seem to obey the same visibility timing
- the duration is variable, which fits prerace-camera / postfx / track
  state better than a simple hardcoded final overlay
- the track packs explicitly define prerace cameras and postfx controls
  in the same resource neighborhood

Current best hypothesis:

- the blackout is more likely tied to a **prerace camera / cutscene /
  PostFXConfig system** than to the final presenter or the visible
  race-scene color chain itself
- likely suspects inside that family are `TemporalFade`,
  `MasterBrightness`, luminance/exposure controls, colour-remap
  volumes, or a prerace-camera handoff that temporarily drives those
