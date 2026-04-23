## Phase 26 — UBO fade pin: memory diff finds the blackout state

Phase 25's frame-order trace exhausted the "guess-a-pass and skip it"
approach — every single compute / draw skip along the pre-tonemap
chain came up null, and every inline-FS patch either broke unrelated
rendering or left the blackout intact. The dim wasn't produced by any
one pass. It was already sitting in memory, written there by the CPU
or an upstream compute, before any visible pass ran.

### Approach — periodic UBO snapshot + three-way diff

`SHADPS4_DC_UBOSNAPSHOT=<period>` dumps every bound scene-material
UBO to disk every `<period>` submits (gated to the race-window arm).
A full race produces ~60 numbered snapshot dirs, each with 300–800
`.bin` files named `pipe_HHHH_cbN_addrXXXX_szYYY.bin`.

Walking the log timestamps back, three snapshots line up with visible
state transitions on Munnar India 19:30:

- **Bright** (21:53:54, arm=9, pre-race) — snap 33
- **Dim** (21:54:01, arm=10, blackout just started) — snap 34
- **Recovered** (21:54:10, arm=10, scene back to color) — snap 39

Diffing byte-by-byte across the three collapses the search space from
"anywhere in GPU state" to a finite list: buffers whose content
actually differs between the three visual states.

### Finding 1 — the 1936-byte lighting UBO

A single 1936-byte UBO is bound at many `(pipeline, cb_idx)` combos
(cb0..cb9 across 50+ scene pipelines). Four adjacent floats in it
scale by the **exact same 0.094 ratio** between bright and dim on
Munnar:

    off=0x98  [ 38]  bright=80.108      dim=7.505     recov=59.94
    off=0xc0  [ 48]  bright=-27.399     dim=-2.567    recov=-20.50
    off=0xc8  [ 50]  bright=75.277      dim=7.052     recov=56.32
    off=0xf4  [ 61]  bright=0.001335    dim=0.000125  recov=0.000999

These are light-intensity slots. The CPU writes
`base_intensity × fade_factor` every frame; during the scripted
race-start fade `fade_factor ≈ 0.094`.

### Finding 2 — overwrite proves the UBO is live

`SHADPS4_DC_LIGHT_PIN=1` + `SHADPS4_DC_LIGHT_PIN_FILE=<path>` loads a
1936-byte snapshot file and, on every `BindBuffers` call for a
1936-byte UBO that passes a content sanity check, `memcpy`s the
recovered-state bytes in place (and invalidates buffer_cache so the
rewrite propagates).

With the pin active, Munnar's behavior inverts:

- Without pin: race starts dim, lifts to bright ~10s later, stays bright.
- With pin: race starts bright, dims at T+30s, stays dim.

That 30-second shift was the key: my pin was holding the 1936 UBO
at "recovered" values, but something else was animating the scene
into dim independently.

### Finding 3 — the second fade UBO (224 bytes)

Re-running the snapshot probe *with the pin active* captured a new
bright/transition/dim triple. Diffing non-1936 buffers across these
three snaps surfaced `pipe_bd3d5e506384461c_cb1`, a 224-byte sun /
key-light transform UBO. The fade signature is at offsets
`[48..51]`:

    off=0xc0  [ 48]  bright=0.7483    trans=0.7483    dim=0.3341
    off=0xc4  [ 49]  bright=-0.7483   trans=-0.7483   dim=-0.3341
    off=0xc8  [ 50]  bright=0.28996   trans=0.28996   dim=0.01003
    off=0xcc  [ 51]  bright=0.06450   trans=0.06450   dim=0.05050

Slot `[50]` drops 97% (0.29 → 0.01). The first-stage pin held the
1936-byte UBO bright but did nothing for this one, which runs its
own ~30-second fade timer that kicks in after the race-start
transient.

`SHADPS4_DC_LIGHT_PIN_FILE_224` mirrors the first pin but targets
224-byte buffers with a distinctive content signature
(floats [0]=5.79, [4]=3, [8]=5.79e-7).

### Result

With both pins active on Munnar India 19:30:

- Race starts at the game-intended sunset lighting (not at the
  scripted-blackout dim).
- The scene gets progressively darker in a natural time-of-day way.
- Car headlights turn on when it gets dark enough, exactly as the
  game was designed to do.
- Mirror reflections come back when headlights illuminate the scene
  behind.
- Sky brightens slightly as the night-side stars / moon become
  visible.

No color shift, no over-exposure, no blown whites. The harsh
blackout-and-lift cycle is gone. The scene behaves like a normal
sunset-to-night transition.

### What the "blackout bug" actually was

The game runs a scripted race-start fade on top of its normal
time-of-day animation. The scripted fade multiplies light
intensities in two specific UBOs (the 1936-byte scene-light buffer
and the 224-byte sun transform) by a very small scalar (~0.094 in
the first UBO, as low as ~0.034 in the second). During the first
~10 seconds of the race, that multiplier animates back up to ~1.0.
The combined effect of the scripted fade and the time-of-day
animation is what produces the "harsh blackout that eventually
lifts to color" signature.

On hardware this is probably intentional — a crossfade from a
darker establishing shot to the race lighting. In emulation it
reads as a bug because the emulator's rendering pipeline exposes
the fade as broken-looking over-attenuation of every light source.

---

## Phase 26b — Pin lifecycle: content-aware auto-expire

First iteration of the pin had a fixed `g_driveclub_race_window`
gate (~32 submits). That caused two failures at long run times:

1. The pin would release briefly between gate arms and slam back
   on the next frame, causing visible flashes mid-race.
2. Once the game reached natural *night* TOD, the scripted fade
   was already over but the pin kept forcing the UBOs to
   recovered-*sunset* values. Scene looked unnaturally bright.

Tried a fixed 20s timeout via `SHADPS4_DC_LIGHT_PIN_WINDOW`. That
worked on 60× timelapse but was too short on 1× (where the scripted
blackout window itself can stretch 30–60s because fewer game-side
animation ticks happen per second of wallclock).

Final design: **three-state lifecycle driven by the game's own
animation state**, not wallclock.

```
state = idle  (0)            — haven't seen blackout yet.
                               Pin does not fire, natural state passes through.
state = engaged (1)          — saw game write v98 < 15  (or vc8 < 0.05)
                               Pin is active; overwrites the UBO.
state = expired (2)          — saw game write v98 >= 40 (or vc8 >= 0.20)
                               after being engaged. Permanent release for
                               the rest of the session. Natural TOD takes over.
```

Transitions are one-way: `idle → engaged → expired`. Never re-engages
in the same session. This makes timelapse irrelevant — the pin follows
the game's fade curve, releasing precisely when the game itself has
finished the scripted fade, whether that takes 5s or 60s.

`SHADPS4_DC_LIGHT_PIN_WINDOW` is now just a safety upper-bound (default
6000 submits ≈ 100s) in case the game never writes a recovered value
for some reason.

---

## Phase 26c — Stable tonemap output shaping

Once the UBO pin removed the hard blackout bug, the remaining visible
issue was that the rendered image felt **dim and slightly
desaturated** compared to the menu thumbnails. The game's natural SDR
tonemap output was technically correct but perceptually dark.

Tried several SPIR-V patches to the tonemap compute shader
`cs_0x000000002c918c06` (the final shader that writes to the
swapchain image). All patches are surgical edits to the RGB store
path: the original three FMA ops (`%1162 %1163 %1164`) are wrapped
with additional operations before being composed into the output
vec4.

### Attempts that did not work

| Attempt | Problem |
|---|---|
| Fixed exposure multiply (1.3x → 2.0x) | Bright enough but shadows still crushed and highlights blew out |
| Exposure + per-channel gamma (pow(x, 1/1.5)) | Midtones crushed to black, highlights washed |
| Above + flat brightness offset (+0.15) | Entire image lifted into light gray milk |
| Pure sRGB gamma encode (pow(x, 1/2.2)) | Midtones lift OK but atmospheric depth flattens — distant mountains and sky look as lit as foreground |

Per-channel operations on RGB are the common failure mode. They all
push each channel independently toward 1.0, which collapses
saturation at the bright end and lifts desaturated (pale) colors
faster than saturated ones, destroying the image's color balance.

### The fix: luminance-preserve gamma

```
lum = 0.2989*r + 0.5870*g + 0.1141*b   (BT.601 perceptual luminance)
lum_new = pow(max(lum, 1e-5), 1/gamma)
scale = lum_new / max(lum, 1e-5)
out.rgb = rgb * scale                   (uniform per-pixel scale)
```

Key property: the per-channel scale factor is **identical across R,
G, B** in a given pixel. So hue and saturation are preserved exactly,
only perceived brightness changes. Colors stay vivid; atmospheric
perspective (hazy distance) stays intact; shadows lift without going
milky.

This is the same conceptual operation that film LUTs, ACES tone
mapping, and most photography exposure-compensation tools use
internally.

### Tuning

Tuned `gamma` interactively with the user:

- 1.3 — too gentle, scene still feels too dark
- 1.5 — closer but still a touch under
- 1.8 — close to right
- 2.0 — too much, starts looking unnaturally bright

Current value: **gamma 1.8** (`1/gamma = 0.5556`).

Final shader patch in `shader/patch/cs_0x000000002c918c06_0.spv`.
The patch also includes a `max(x, 0)` clamp on each channel before
the pow to protect against negative HDR values (would produce NaN
otherwise).

---

## Usage

Pin snapshots live at `tools/driveclub_pin_snapshots/`:

- `light_ubo_1936.bin` — recovered-state 1936-byte lighting UBO.
- `sun_ubo_224.bin` — recovered-state 224-byte sun transform UBO.

Combined with the tonemap shader patch that ships in
`shader/patch/`:

    SHADPS4_DC_LIGHT_PIN=1 \
    SHADPS4_DC_LIGHT_PIN_FILE=tools/driveclub_pin_snapshots/light_ubo_1936.bin \
    SHADPS4_DC_LIGHT_PIN_FILE_224=tools/driveclub_pin_snapshots/sun_ubo_224.bin \
    SHADPS4_DC_NUKE_AFTER_ARM=3 \
      scripts/run_driveclub_overlay.sh

Env vars in detail:

| Var | Default | Effect |
|---|---|---|
| `SHADPS4_DC_LIGHT_PIN=1` | off | master enable for UBO pin probes |
| `SHADPS4_DC_LIGHT_PIN_FILE=<path>` | unset | snapshot to memcpy into the 1936-byte UBO while pin is engaged |
| `SHADPS4_DC_LIGHT_PIN_FILE_224=<path>` | unset | snapshot for the 224-byte sun UBO |
| `SHADPS4_DC_LIGHT_PIN_WINDOW=<N>` | 6000 | safety upper-bound in submits after first engagement |
| `SHADPS4_DC_NUKE_AFTER_ARM=<N>` | 3 | number of gate arms to skip before any pin activates (avoids menu/lobby) |

The shader patch in `shader/patch/cs_0x000000002c918c06_0.spv` is
always active if present — no env var required. Delete the file to
revert tonemap to the game's original output.

---

## What still needs proving

- Canada, Japan, Norway, and other non-Munnar tracks. The snapshot
  values are Munnar-19:30-specific; the content-aware auto-expire
  should handle tracks where the fade never drops below the
  engagement threshold (stays idle forever). Needs verification that
  it doesn't false-engage on tracks with already-low light levels.
- The mirror-goes-black symptom during the *first* second of the
  race (before the pin fires) — may need to tighten
  `SHADPS4_DC_NUKE_AFTER_ARM` or widen the first-engagement threshold.
- Cross-timelapse behavior on 1× with very slow TOD — visually
  suspected to work based on content-aware design but not yet
  explicitly tested end-to-end.
