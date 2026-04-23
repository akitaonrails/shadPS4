## Phase 11 — Iteration 11 combined multi-scalar patch, and the vanilla decider

### Iteration 11 — patch

Extended the `PatchAnimlibFloatValues` call list on the
`india_posteffects.lvl` animlib to cover three brightness-related
scalars at once instead of just `MasterBrightness`:

```
MasterBrightness     0.003  -> 1.0
MasterBrightness     0.0007 -> 1.0
ManualAutoMix        0.99034 -> 1.0  (force all-manual exposure)
ManualAutoMix        0.13865 -> 1.0
AutoTargetLuminance  0.38   -> 1.0  (target full brightness)
```

Ten writes per track-pack (2 grade variants × the replacements
above). Applied to both Munnar (`india_road_point_02_n.rpk`) and
India reverse (`india_road_circuit_01_r.rpk`). `ManualExposureLog2`
was deliberately left alone — its positive-log values (`5.8`, `4.5`)
are brightening keyframes, not dim values.

### User-visual report

"didn't see any difference from baseline. blackout is still there."

No recording of Iteration 11 was captured on that run, so only
subjective data for this patch combination — but we returned to the
question "are we on the right track?" and ran a decider instead of
iterating further.

### Decider — pure-vanilla comparison

Moved all 60 patched files out of
`tmp/driveclub_overlay/CUSA00003/` into a sibling backup tree and
symlinked every slot back to the pristine install. Launched with
**zero** Driveclub-local patches, only the emulator-side changes
that are compiled into the binary (MSAA depth resolve, Bayer dither,
the `[dc-timeline]` heartbeat, `SHADPS4_DC_TORTURE` which stays
dormant). User recorded Munnar 19:30, 22-second clip,
`2026-04-22 12-31-53.mkv`.

Same frame-extraction protocol as Run 2 (right-half crop, 2 Hz
sampling). Measurements:

| moment | vanilla centre | vanilla mirror | Run 2 centre | Run 2 mirror |
|---|---|---|---|---|
| t=0.0 s (clean start) | 27 | **38** | 88 | 3.5 |
| t=3.0 s | 12 | 16 | 42 | 3.5 |
| t=5.5 s | **1.23** | **1.36** | 6.6 | 3.5 |
| t=6.0–9.0 s (deep blackout plateau) | **1.22–1.23** stable | **1.23** stable | 6.5 | 3.5 stuck |
| t=9.5 s (recovery) | 76 | **72** | 83 | 3.5 stuck |

### What the numbers definitively say

**1. The animlib-scalar strategy is real but bounded.**

Vanilla deep-blackout centre: 1.22/255 = 0.48 %. Run 2 deep-blackout
centre: 6.5/255 = 2.5 %. That is a real 5× numeric improvement from
the MasterBrightness patch — **but perceptually 0.5 % and 2.5 % both
read as "black"**. The patch does what the math predicts; the math
just doesn't predict enough amplitude.

**2. MasterBrightness×everything-else ≠ observed values.**

If the 0.0007 MasterBrightness keyframe were the only multiplier
acting on the scene, our patch lifting it to 1.0 should have
produced ~1428× brighter output. We see 5×. The remaining ~285×
attenuation lives downstream of the animlib scalars we've been
patching — in the tonemap, in HDR→SDR compression, in a clamped
exposure stage, or in a scene-render decision that the scalars only
modulate, not gate.

**3. Vanilla mirror works normally — it is not "suppressed".**

In pure vanilla, mirror luminance tracks the scene: 38 at the start,
1.23 at the blackout floor (same value as the scene), 72 at
recovery. The mirror isn't gated off; it's just reflecting a dark
scene and so it reads dark. The "mirror pass is gated" hypothesis
from Phase 10 collapses — the mirror is fine, the scene is dark.

That simplifies the whole model: **there is one blackout driver, not
two**. Whatever multiplier attenuates the main view also attenuates
the mirror because the mirror samples the same scene.

**4. Our 60 accumulated patches broke mirror recovery.**

Run 2 mirror is stuck at 3.5 for the entire 10-second clip — it
never recovers to the ~72 that vanilla shows. Somewhere in our 60
patched files, one patch is gating the mirror's reflection
rendering. It's not the animlib keyframes we know about (they were
tested in isolation in Run 2 and 3); it's most likely in the UI
panel disables or in one of the other animlib resources we touched.

### What stays and what changes in the working model

Stays:
- There is a blackout state with measurable discrete dim plateaus.
- It is game-scripted, happens on a fixed prerace camera, ends on
  camera handoff to cockpit.
- The `MasterBrightness` / `ManualAutoMix` / `AutoTargetLuminance`
  animlib keyframes *are* real inputs to scene darkness.

Changes:
- The real blackout amplitude is set downstream of these scalars, not
  by them. Further scalar-hunting will produce tiny numerical wins
  (maybe 6.5 → 12 → 20) but no visible change until we find the
  downstream multiplier.
- The "mirror pass is gated separately from main view" model is out.
  One darkening path, both subsystems affected identically.
- One of our 60 track-pack or UI-panel patches is actively gating
  the mirror pipeline in the patched state. That patch is an
  unintended lever on the blackout subsystem and is our best
  forward-handle.

### Strong new lead — bisect the 60 patches

A bisect of the 60 accumulated patches to find which one gates the
mirror is the cheapest next diagnostic:

- Split the 60 into 30 + 30.
- Run with only the first half patched; check mirror behaviour in
  vanilla baseline video (just eyeball at recovery — is mirror ≈ 72
  or ≈ 3?).
- If mirror is broken → culprit in first half. If working → in
  second half.
- Four binary bisections → 60 patches narrowed to 1 specific patch.

Whichever patch gates the mirror is almost certainly also adjacent
to the real blackout-driving code. That gives us a pointer into the
render-state subsystem we've been unable to reach from the scalar
side.

### Recommended next session

1. **Bisect to find the mirror-gating patch.** Maximum four
   launches. Output: one specific patched file.
2. **Inspect that patch's content** to identify which render
   subsystem or actor it disables. The mirror depends on a
   reflection camera + a scene render pass; the guilty patch is
   interacting with one of those.
3. **Revert that one patch** to see if mirror recovers. That gives
   us a clean "mirror works" baseline to test future scalar patches
   against.
4. **From the guilty patch's identity, map to the real gate.** If
   it's e.g. a fullscreen UI panel whose disable also kills mirror
   reflections, the mirror reflection pass must consume from that
   panel's texture. That's an unexpected render-dependency chain
   worth following.

Continuing to layer more animlib scalar patches without resolving
the mirror regression is expected to produce only imperceptible
numeric changes. The bisect is the next load-bearing diagnostic.
