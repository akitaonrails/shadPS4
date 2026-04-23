## Phase 10 — video diagnostic pins the blackout to an animlib curve

### The video and what it measured

OBS recording `/home/akitaonrails/Videos/OBS/2026-04-22 11-48-45.mkv`,
1920×1080, h264, 12 seconds, covering the Munnar 19:30 race-load
window. User confirmed no controller input and no R1 press during
recording — every visual change is the game's own scripted behaviour.

Sampled at 10 Hz for frame-level luminance analysis, the blackout
sequence is four **discrete brightness plateaus** with one-frame
transitions between them, on a camera that stays dead-fixed through
the whole window:

| moment | centre luminance | interpretation |
|---|---|---|
| t=0 race-loaded, no HUD | 41 | clean scene visible |
| t=3.5 s blackout enters | 1.6 | near-pitch black |
| t=3.8 s automatic lift | 42 | HUD active, scene barely visible |
| t=5.5 s dark again | 1.2 | ~0.5 % of full |
| t=6.3 s shallow lift | 9.0 | intermediate |
| t=8.9 s last dark frame | 8.9 | steady state |
| t=9.0 s gate opens | 95 | full scene, camera also changed |

Camera structural diff (brightness-normalised): every dark/lift frame
from t=2.5 s to t=8.9 s is structurally identical (pixel-diff 0.44–6.18
out of 255). The t=0 clean scene and the t=9.0 s recovered scene are
both structurally *different* from the blackout window (diff ≈ 65 and
≈ 95 respectively), confirming three distinct camera positions:

1. Position A — t=0, race loaded, wide track view, HUD not active
2. Position B — t=2.5–8.9 s, prerace cinematic fixed camera, HUD active
3. Position C — t=9.0 s, cockpit camera, race starts

The earlier "R1 briefly lifts the blackout" observation from previous
sessions was correct as an independent signal, but this recording
happened without R1 and still shows the 1.6 → 42 → 1.2 → 9 → 95
staircase. Therefore the staircase is **the game's own scripted fade
curve**, not camera-reactive per-frame logic.

### Keyframe-dump findings (`india_posteffects.lvl` animlib)

Custom float-scanner walks each named track in the animlib and lists
meaningful floats in the bytes following the ASCII name. Abridged
output, both variants (`india_dc_grade_interior` and
`india_dc_grade_exterior`) share the same layout:

```
SunElevation             (all zeros — no keyed animation)
AutoTargetLuminance      +22=1   +30=1
ManualExposureLog2       +21=1   +29=1
ManualAutoMix            +13=85   +21=0.9903  +25=0.1386  +29=0.9903  +33=0.1386
AutoMaxLuminance         +18=300  +22=1
AutoSpeed                +14=180  +18=0.09   +22=1
MasterBrightness         +17=0.003  +21=1  +25=0.0007  +29=1  +32=1.528e-05
MotionBlurLevel          +18=0.3 (interior) or 0.2 (exterior)
ColourRemapVolumeName    → night LUT → day LUT fallback (already patched)
```

Smoking gun: `MasterBrightness` has two dim-side keyframe values —
`0.003` and `0.0007` — and two bright-side values at `1.0`. The
product `scene_luminance × 0.0007` puts a normal scene at 0.07 %
brightness, which is exactly the 1.6 / 255 = 0.63 % centre luminance
we measured at the deepest blackout. `0.003` corresponds to the
shallower dim (centre ≈ 8.9 / 255 = 3.5 %). Both values match the
plateaus the video recorded.

The 1.0 values are the recovered-brightness keyframes. The
1.528e-05 value is small enough that it's almost certainly a slope /
tangent, not a brightness value; it's left alone.

### Inline defaults vs animlib — where the override comes from

`PostFXConfig_{interior,exterior,helmet}` actors carry an inline
`MasterBrightness = 0.003` baseline. Our Phase 8 patcher set those to
1.0. That had no visible effect **because the animationlib's
keyframe curve runs every frame and rewrites the live exposure value
from the curve back to 0.003 → 0.0007 → 1.0**. The inline value only
matters if nothing is driving the curve, and the curve is always
driven during prerace.

This retroactively explains every iteration 2.x and 3.x clean miss:
we were dialling downstream PostFX and UI panels while the scripted
animlib curve kept reasserting the dim values from above.

### Safer patcher — `PatchAnimlibFloatValues`

New helper in `tools/driveclub_asset_patch/Program.cs`. For each named
track, scans up to N bytes following the name; for each `(oldValue,
newValue)` replacement pair it finds the first 4-byte window that
reads `oldValue` (within 0.5 % tolerance, floored at 1e-7), and
overwrites it with `newValue`. Every other byte — header fields,
timing bytes, slopes, adjacent tracks — stays untouched. This avoids
the April-20 `PatchNamedTrackKeyframes` failure mode where blind
offset sweeps corrupted record strides and crashed the game's parser.

First live call on `india_posteffects.lvl`:

```
animations.MasterBrightness.curve[1] MasterBrightness+29: 0.003  -> 1
animations.MasterBrightness.curve[1] MasterBrightness+37: 0.0007 -> 1
animations.MasterBrightness.curve[2] MasterBrightness+29: 0.003  -> 1
animations.MasterBrightness.curve[2] MasterBrightness+37: 0.0007 -> 1
```

Two occurrences (interior + exterior grade), two dim keyframe values
each, four writes total per track pack. Applied to both Munnar
(`india_road_point_02_n.rpk`) and India reverse circuit
(`india_road_circuit_01_r.rpk`).

### What this is expected to change

If `MasterBrightness` is the only animated scalar driving the dim,
the blackout staircase should flatten — either disappear entirely or
persist only through a second, smaller-amplitude scalar (e.g. the
`ManualAutoMix` 0.1386 / 0.9903 oscillation, which we deliberately
did NOT touch because auto-vs-manual mixing has weirder failure
modes). A measurable outcome:

- success case: centre luminance during prerace hovers around 95/255
  (the recovered-scene level), from t=2.5 s onward
- partial case: centre luminance climbs from 1.6 to maybe 40–60 but
  still dips during some prerace frames
- null case: centre still dips to 1.6 — means a different scalar is
  the driver; `ManualAutoMix` next, then `AutoMaxLuminance`,
  `AutoTargetLuminance`

### Probe direction if this run still shows a dip

The scan output lists every candidate scalar worth touching next, in
priority order:

1. `ManualAutoMix` — values 0.9903 and 0.1386 are the oscillation.
   Try rewriting both to a constant (either 0 for full auto or 1 for
   full manual) to see which breaks the blackout. Risk: changing the
   mix mid-curve may interact badly with AutoMaxLuminance.
2. `ManualExposureLog2` — currently reads 1 / 0 / 1 in the scan.
   Worth re-scanning with lower threshold to see if there's a
   negative-log dim value like -4 hidden in it.
3. `AutoTargetLuminance` — scan showed only 0 and 1 but a finer
   threshold may reveal a mid-range target.

Every new candidate goes through the same value-matched safe patcher
— never the blind-offset sweep.
