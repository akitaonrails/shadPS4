## Phase 13 — the UBO dump breaks it open

### Setup

`SHADPS4_DC_UBOLOG=1` enabled a 128-byte hex dump of every non-special
uniform buffer for six hand-picked race-window pipelines, guarded by
the existing race-window arm. Single Munnar 19:30 baseline session
captured 1905 frames / ~35 s. Log: `.../shadPS4/log/shad_log.txt`,
32 MB, 150k `[dc-drawlog]` lines + 9k `[dc-ubolog]` lines.

### The VideoOut-gamma red herring

Every frame of the session, the game calls
`sceVideoOutColorSettingsSetGamma(gamma=0.5)` and the follow-up
`sceVideoOutAdjustColor` — 2181 back-to-back calls over 35 s, which
then flow into `presenter.pp_settings.gamma`. The value never
changes: no call ever uses anything but `0.5`. Since menus and
panorama look fine while this is happening, the presenter gamma path
is constant and cannot be the blackout on/off. `NoteDriveclubVideoOutGamma`
only fires on values inside `[0.49..0.51]` so it stays in bounds but
is decoupled from the actual dim.

Side observation: `post_process.frag`'s `pow(rgb, 1.0/(2.4+1.0-pp.gamma))`
curve does do something non-trivial at `gamma=0.5` (exponent 1/2.9
instead of 1/2.4) — but since it is constant across the whole session
it is not what flips the scene from bright to dim. Keep it filed as a
"possibly wrong sRGB interpretation" follow-up, not a blackout lead.

### The UBO offset that moves

Pipeline `0xf6e5670be11b0009` dumps a 48-byte UBO at `stage=0 cb0`
and the identical buffer at `stage=2 cb0` (same guest address — VS
and FS both read it). Across 307 samples inside the race window
(submits 1452..1758) only one float in that UBO ever changes: **offset 3**.

Layout is invariant:

    [0]=1.0    [1]=1.0    [2]=0.3     [3]=LUM      <-- only one that moves
    [4]=1.0    [5]=0.25   [6]=1.0     [7]=0.25
    [8]=0.25   [9]=1.0    [10]=0.25   [11]=1.0

`[0..3]` looks like a `vec4(scale_r, scale_g, scale_b, luminance)` and
`[4..11]` are two `vec4` weight masks `(1,¼,1,¼)` and `(¼,1,¼,1)` —
textbook eye-adaptation / auto-exposure signature.

Offset 3 is monotonically rising over the captured ~2 s:

    submit=1452   lum=5.35
    submit=1502   lum=7.50
    submit=1602   lum=10.30
    submit=1702   lum=12.65
    submit=1758   lum=13.81

307 samples, every single one larger than the one before. No
oscillation, no convergence — a clean climb with constant slope. The
session ended inside the climb, never reaching equilibrium.

### Why this matches the symptom exactly

The user-reported blackout:

- kicks in a second after the race loads (adaptation starts from a
  neutral value, then climbs)
- deepens over 5–30 s (value keeps climbing as the system hunts for
  a scene brightness it never stabilises on)
- sometimes never comes back (the loop has no upper clamp — if the
  target is numerically unreachable, it just stays dim forever)

The HUD/minimap overlay on top of the blackout is untouched because
overlays are composited *after* the tonemapper — this UBO only
reaches the scene rendering, not the 2D UI passes. That also matches.

### What's probably wrong

This looks like an eye-adaptation feedback loop whose denominator or
upper clamp is off. The rising value is almost certainly the
"current adapted luminance" the tonemap divides by — so as it climbs,
the scene gets darker. The loop either:

- reads its histogram from a stale/over-bright HDR source (texture
  cache serving the wrong resident copy, or a page-fault miss
  returning zeros that get interpreted as "fully saturated"), or
- never runs the shrinking half of the adapt loop (missed compute
  dispatch, or a scheduling race between the histogram pass and the
  tonemap's UBO write)

Either way the downstream symptom is the same: offset 3 climbs
without bound, and the tonemap responds by dimming the scene.

### Next step — quick validation via clamp

Before chasing the root cause in the histogram/compute side, cheap
test:

- at UBO bind time for pipeline `0xf6e5670be11b0009`, recognise the
  invariant layout `(1.0, 1.0, 0.3, x, 1.0, 0.25, ...)` and overwrite
  offset 3 with a fixed value (2.0, say — low end of the observed
  range, near the "bright" plateau).
- env-gate it: `SHADPS4_DC_LUM_CLAMP=2.0`.
- if the blackout disappears or dramatically shortens, the UBO is
  confirmed as the sole dim driver and the real fix is upstream in
  whichever pass writes this value.
- if the blackout is unchanged, this UBO is a downstream reader of
  something else and we need a different intervention.

If the clamp works, the follow-up is to find the shader that writes
this UBO (compute pass that computes adapted luminance) and fix *its*
feedback rather than patching the consumer. That compute shader is
probably one of the other 5 `kDriveclubLaterRaceGatePipelines` that
Codex already singled out.

### Action items

- extend `vk_rasterizer.cpp` with an `SHADPS4_DC_LUM_CLAMP` env knob
  that rewrites offset 3 of the matching UBO shape at bind time
- keep the drawlog + ubolog instrumentation in place — it's now the
  primary diagnostic for this class of bug
- longer session recording (5+ minutes) is still useful: need to see
  whether the value *ever* plateaus on its own or keeps climbing
  forever, and whether a brief recovery phase corresponds to a value
  dip
