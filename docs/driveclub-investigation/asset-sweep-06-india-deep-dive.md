## India first-track deep dive

The first India reverse track (`india_road_circuit_01_r.rpk`) is now the
best concrete asset-side target because it matches the user's most
reliable reproduction: long blackout at `19:30`, clear weather.

### Resource split

Using a local probe tool against
`data/leveldata/india_road_circuit_01_r.rpk`:

- `india_posteffects | RTUID_LEVEL_DATA`
  - depends on:
    - `EVO+LEVEL+ACTORPostFXConfig_interior`
    - `EVO+LEVEL+ACTORPostFXConfig_exterior`
    - `EVO+LEVEL+ACTORPostFXConfig_helmet`
    - `animations | RTUID_ANIMATIONLIB`
- `india_road_circuit_01_n_prerace_cams | RTUID_LEVEL_DATA`
  - depends on:
    - older `EVO+LEVEL+ACTORpreracecam_*`
    - newer `EVO+LEVEL+ACTORnew_preracecam_*`
    - `animations | RTUID_ANIMATIONLIB`
    - `sequences | RTUID_ANIMATIONLIB`

This gives a clean family split:

- **prerace camera system**
- **postfx/exposure system**
- **animation libraries that can override both**

### Older and newer camera actors

The older `preracecam_*` actors identify themselves as `worldcam`.
They expose inline scalar fields such as:

- `VFoV`
- `Fade`
- `LookAt`
- `LookAtOffset_X`
- `LookAtOffset_Y`
- `LookAtRoll`
- `RelativeTo`
- `RelativeToOffset`
- `FocusDistance`
- `DepthOfField`
- `ShutterSpeed`
- `ShakeEnabled`
- `ShakeIntensity`

The newer `new_preracecam_*` actors identify themselves as `cutsccam`.
They expose:

- `VFoV`
- `Fade`
- `LookAt`
- `LookAtOffset_X`
- `LookAtOffset_Y`
- `LookAtRoll`
- `Pivot`
- `CameraInPivotSpace`
- `FocusDistance2`
- `ApertureFNumber`
- `EnableNewDOF`
- `AutoFocus`
- `ShutterSpeed`
- `ShakeEnabled`
- `ShakeIntensity`

The same inline `Fade` slot also exists on the India
`CutsceneCamera_*` actor.

### PostFXConfig actor fields are inline and patchable

The `PostFXConfig_interior`, `PostFXConfig_exterior`, and
`PostFXConfig_helmet` actor resources all share the same inline scalar
layout. The relevant fields are not opaque references; their values sit
directly in the actor blob.

Shared defaults observed in all three actor resources:

- `ManualExposureLog2 = -16`
- `AutoTargetLuminance = 0.3612`
- `ManualAutoMix = 1`
- `AutoMaxLuminance = 300`
- `AutoSpeed = 1`
- `WeatherType = 0`
- `WeatherOverrideMix = 1`
- `MasterBrightness = 0.003`
- `TemporalFade = 1`
- `NonBloomedAttenuation = 1`
- `Enabled = 1`
- `TXAAOverallWeight = 0.7`
- `TXAAColourClamping = 1`
- `MotionBlurLevel = 0.2` exterior/helmet, `0.3` interior

The only obvious string-level difference between the three postfx
actors is the remap cube they point to:

- exterior: `colourcuberemap_india_linear.dds`
- helmet: `colourcuberemap_india_linear.dds`
- interior: `colourcuberemap_india_interior_linear.dds`

This is important because it means `TemporalFade` and the basic exposure
controls can be brute-force patched directly without first solving a
complex parser.

### Animation libraries are likely overriding some darkness state

The India postfx level depends on an `RTUID_ANIMATIONLIB` resource whose
string table contains:

- `india_dc_grade_interior`
- `india_dc_grade_exterior`

and the following animated property names:

- `SunElevation`
- `AutoTargetLuminance`
- `ManualExposureLog2`
- `ManualAutoMix`
- `AutoMaxLuminance`
- `AutoSpeed`
- `MasterBrightness`
- `MotionBlurLevel`
- `ColourRemapVolumeName`

This is a strong hint that the grade/exposure system is driven by a
time-of-day or sun-elevation curve, not only by the inline defaults.

Crucial negative result:

- the postfx animation library does **not** show `TemporalFade`

That creates a useful split:

- likely **blackout transition** controls:
  - prerace/cutscene camera `Fade`
  - postfx `TemporalFade`
- likely **scene remains too dark after fade lifts** controls:
  - `india_dc_grade_exterior`
  - `india_dc_grade_interior`
  - exposure/brightness/remap curves

### Camera animation library also contains Fade tracks

The prerace animation library contains named tracks for both the older
and newer camera families. For the older `prerace_*` tracks, it clearly
contains:

- `Time`
- `Transform`
- `Transition`
- `VFoV`
- `Fade`

The newer `new_preracecam_*` tracks expose rotation/translation/VFoV and
camera-focus fields. The string dump does not clearly show a `Fade`
track for every new camera, but the actor still contains the inline
`Fade` field, so the prerace handoff can still depend on it.

### Practical interpretation

The current best model is now a two-layer one:

1. a prerace/postfx transition gate is temporarily darkening or gating
   visibility (`Fade`, `TemporalFade`)
2. when that gate relaxes, the underlying India exterior grade can still
   make the scene much too dark at the tested evening time

This matches the user's repeated observation that the race can begin
with a true fade-to-black phase and then reveal a scene that is still
wrongly dark underneath.

### First asset-side brute-force patch set

A dedicated local tool (`tools/driveclub_asset_patch`) was added to
generate patched copies of the India track pack without touching the
installed game file.

The first brute-force patch set does all of this at once:

- zero every prerace/cutscene camera `Fade`
- zero every `PostFXConfig_*` `TemporalFade`
- neutralize several inline postfx scalars:
  - `MasterBrightness = 1`
  - `ManualExposureLog2 = 0`
  - `ManualAutoMix = 0`
  - `WeatherOverrideMix = 0`
  - `MotionBlurLevel = 0`
- force the postfx animation library's night LUT references back to the
  day LUTs:
  - `colourcuberemap_india_interior_night_linear.dds` ->
    `colourcuberemap_india_interior_linear.dds`
  - `colourcuberemap_india_night_linear.dds` ->
    `colourcuberemap_india_linear.dds`

This is the first non-renderer brute-force that actually targets
concrete prerace/postfx content associated with the India blackout.
