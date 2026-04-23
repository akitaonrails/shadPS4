## Phase 7 — aggressive torture probe (post-codex)

Picking up after the late-2026-04-20 codex handoff. All of codex's
`src/` changes were stashed (`git stash list` will show them labelled
`codex probe scaffolding 2026-04-20`) so the tree is clean except
for the doc itself. The torture hook below was built from scratch on
`vk_rasterizer.cpp::BindTextures`.

### Mechanism

Env var `SHADPS4_DC_TORTURE=1`. When any draw writes to one of the
four critical HDR/visible surfaces —

```
0x500cdd0000 1920x1080 R8G8B8A8Srgb          visible composite
0x5009688000 1920x1080 B10G11R11UfloatPack32 HDR main
0x5008130000 1920x1080 B10G11R11UfloatPack32 HDR alt primary
0x500fdd0000 1920x1080 B10G11R11UfloatPack32 HDR alt composite
```

— the second pass of `BindTextures` null-substitutes specific sampled
inputs before the descriptor write. Per-substitution logging is
rate-limited to one line per unique (dst, src-addr, dims, format)
tuple, tagged `[dc-torture]`. No freeze, no sabotage, no shader
replacement — one-vector experiment only.

### Run 1 — substitute every sampled input < 1024×1024 on matching draws

Result: **main world pitch black, HUD intact, world visible in the
car's mirror**. 370 unique substitutions, dominated by BC1/BC3/BC5/
BC7 material textures (74 BC5, 71 BC1, 65 BC7, …).

Takeaway: I nulled every material texture for every geometry pass
that writes to the critical targets. Cars, track, sky had no diffuse
/ normal / AO — of course they drew black. Misframed as a "composite"
bug; these targets actually receive direct material-shaded geometry
passes. The mirror surviving was not a real signal — see below.

### Run 2 — same, but skip BC-compressed formats

Result: **main world pitch black, HUD intact, mirror also black.**
58 unique substitutions, all non-BC. Format histogram:

```
 12 R32Uint                8x4  tile lighting index buffers  (many)
 10 R16G16B16A16Sfloat     various
  8 R8Unorm                32x32
  6 B10G11R11UfloatPack32  various
  4 R32Sfloat              at 0x500c4d0000 at 3 different sizes
  4 R8G8B8A8Srgb           256x256 textures
  2 R32G32B32A32Sfloat     80x48 and 364x276 — tile luma grids?
  1 D32Sfloat               960x540 — half-res depth
```

Also killed: ten half-res 960×540 buffers (SSR/bloom/temporal
pyramid), mirror RT candidate `0x505fe57600 512x256 R8G8B8A8Unorm`,
and the `0x500c4d0000 R32Sfloat` cascaded shadow/hierarchical depth
at three mip sizes.

Mirror visible in Run 1 was therefore a coincidence of BC-null
rendering: when cockpit BC materials were nulled they shaded flat
black, but shader discards keyed on `texture(bc).a` with null
textures returning 1.0 meant alpha tests didn't clip, and the
mirror-mesh region let whatever was drawn before show through.
Not a real rendering path. Dismiss.

Takeaway: the R32Uint `8×4` buffers are almost certainly forward+
per-tile light lists — zeroing them means "no light affects any
tile" which alone would black out most of the scene. The four
`32×32 R16G16B16A16Sfloat` buffers at `0x5008130000` inputs look
like ambient / irradiance probe grids. And the half-res pyramid is
clearly needed for post-process.

### Run 3 — substitute only `0x501d630000 80×48 R32G32B32A32Sfloat`

The 80×48 R32G32B32A32 buffer has the exact shape of a per-tile
luma grid feeding HDR eye-adaptation. Isolated this as the only
torture target.

Result: **scene renders normally, race-start fade still present and
visually identical to baseline**. One substitution fired and was
logged. Everything else on screen behaved as usual.

Takeaway: this buffer — the strongest-shaped candidate for the
adaptation input — doesn't drive the fade. Not conclusive for all
small textures, but reduces the space of plausible single-texture
culprits.

### Run 4 — substitute eight small non-BC candidates

Tortured all of:

```
0x501d630000 80x48   R32G32B32A32Sfloat
0x501df20000 364x276 R32G32B32A32Sfloat
0x50ba598400 128x2   R16G16B16A16Sfloat   — odd-shape histogram?
0x5003896400 32x32   R8G8B8A8Unorm
0x5065f2dc00 32x32   R16G16B16A16Sfloat   — probe
0x5065f2bc00 32x32   R16G16B16A16Sfloat   — probe
0x5065f31c00 32x32   R16G16B16A16Sfloat   — probe
0x50239c0000 32x32   R16G16B16A16Sfloat   — probe
```

Result: **scene pitch black, HUD only, mirror black**.

7 of the 8 substitutions fired (`0x501df20000` didn't rebind during
this run). The black scene identifies the four 32×32 R16G16B16A16
buffers as the ambient-probe grids responsible for ambient
illumination — kill them and the scene has zero non-direct lighting
and draws as black.

Takeaway: these probes are essential for any visible scene.
Confirmed as ambient/irradiance grids, not adaptation feedback.

### Run-family conclusion for the texture-sampling hypothesis

Across 4 torture runs the only configuration that produced a
visible scene showed **the race-start fade unchanged**. I can't
prove the fade is feedback-driven by a small sampled texture,
because I only tested one candidate in isolation (80×48). But the
stronger result is negative: the class of plausibly-shaped inputs
(tile-averaged HDR float grids, 32×32 probe-shaped, small HDR
histograms) either doesn't affect the fade or kills the scene
entirely.

The fade persisting regardless of sampled-texture torture means the
driving signal is either:

1. a push-constant / SGPR scalar pushed per-draw by the game
2. a uniform buffer whose contents ramp over time
3. a shader-intrinsic time-based computation reading game time

None of those are reachable from texture-binding substitution.

### Variability signal — the part I was wrong about

Initially I claimed the fade duration looked "shorter on this tree
than during codex's runs" and offered that as evidence the codex
changes might have worsened it. That was wrong.

**Fade duration is highly variable and non-correlatable**: under
10 seconds on some race starts, ~20 seconds on others, 30+ seconds
sometimes, and occasionally never recovers. Same build, same track,
same time of day. No observed correlation with anything reproducible.

The "never came back" case is the load-bearing signal. Scripted
cinematic fades have fixed durations and always complete; a loop
that sometimes fails to converge is inconsistent with "scripted
animation" and more consistent with:

- a feedback integrator whose input can get stuck at zero
- a ping-pong buffer pair with cache aliasing (codex's own line
  of investigation on `0x5009688000` create→ExpandImage→CopyImage
  churn)
- an async streaming / cache-warmup race that sometimes lands OK
  and sometimes traps

This re-opens the feedback-loop hypothesis but via a mechanism
that texture-null torture cannot falsify — the emulator cache state
itself is what's varying.

### What to try next (no more "accept and move on")

1. **Solid-white substitute** on the same four 32×32
   R16G16B16A16Sfloat probe-candidates instead of null. If the
   scene becomes blown-out-bright (as expected when ambient probes
   are set to white) but the fade changes shape or disappears,
   something in the adaptation chain is reading one of them.
2. **Per-frame binding log** for draws into `0x500cdd0000` during
   the first 15 seconds of a race. Look for a binding whose address
   flips between two values each frame (ping-pong) vs. one that
   stays put — mismatch would show the cache-alias bug codex saw.
3. **Push-constant / UBO torture** — intercept the uniform-buffer
   write path for draws to the critical targets, stomp the first
   N floats to 1.0, observe whether the fade disappears. This is
   the natural next vector once texture-null is exhausted.
4. **Codex's `ExpandImage` / `FindImage` same-address preservation**
   (currently stashed) — re-apply and re-test under the variability
   lens. If the handoff was already addressing the aliasing but the
   tests weren't repeated enough to see the variability, the change
   may be a real fix masked by the non-determinism.

The texture-sampling torture track is closed. The next probe vector
must reach either game uniform data or the cache-aliasing path.
