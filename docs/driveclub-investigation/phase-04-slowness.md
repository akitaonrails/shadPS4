## Phase 4 — "slowness" (resolved, two causes)

The subjective "game is running like molasses" had two separate roots.
Pinned them one at a time.

### Evidence collected from a real v1.28 session

Log at `~/.local/share/shadPS4/log/shad_log.txt`:

- **Pipeline cache enabled** ✓. First v1.28 session compiled 864 shaders
  + 590 pipelines (vs ~881 total for v1.00). The extra ~330 shader
  modules are v1.28-specific content — compiled fresh because the cache
  was empty on first v1.28 launch. `Cache dumped` on shutdown confirms
  persistence, so this cost is one-shot. Subsequent launches replay
  instantly.
- **1325 occurrences** of
  `texture_cache.cpp ResolveDepthOverlap: Unimplemented depth overlap copy`
  clustered in the in-game portion. Smoking gun, but needed to know
  which shape combinations were hitting.

### Root cause #1 — 60 fps patch causes slow-motion smooth playback

The user described the feel as "smooth but in slow motion" — frames
arrive at a steady rate, cars accelerate slowly, lap time in 1 minute
shows what should be half a lap, AI cars overtake normally at 25 km/h
while the player's gauge reads 25 km/h.

This is the classic shape of "render rate and logic timestep out of
sync". The `Driveclub.xml` 60 fps patch I re-enabled when we swapped
to v1.28 was designed for PS4 Pro's 60 fps render pipeline — and from
everything I've read these community eboot patches rewrite the render
rate without touching the internal fixed-timestep game-logic rate.
Real PS4 Pro's engine handles the mismatch internally; shadPS4 doesn't.

**Fix:** disable the patch:

```sh
mv ~/.local/share/shadPS4/patches/Driveclub.xml \
   ~/.local/share/shadPS4/patches/Driveclub.xml.disabled-for-v1.0
```

Game reverts to 30 fps native cap; everything moves at the correct wall-
clock speed. Confirmed by the user with a clean run. Tradeoff is 30 fps
instead of 60, which matches stock PS4 behavior.

Long-term fix options (deferred):
- Find/write a Driveclub patch that *also* scales the internal timestep.
- Implement frame interpolation host-side (much larger scope, out of
  scope for this branch).

### Root cause #2 — ResolveDepthOverlap gap breaks night scenes

Separate problem. After disabling the 60 fps patch the game runs at
correct speed, but on a **night track the scene is near-pure black** —
only HUD visible, no headlight cones, no track surface, no cars. Day
scenes looked dim (Phase 2 handled that) but were mostly visible.

Instrumenting the `else` branch in
`TextureCache::ResolveDepthOverlap` (caching each unique shape
combination and logging once per run) revealed a **single offender**
hit over a thousand times per session:

```
cache(fmt=D32Sfloat     depth=true  stencil=false samples=4)
  -> new(fmt=R32G32B32A32Sfloat depth=false stencil=false samples=1)
  binding=1 (Texture)
```

Driveclub's forward+ / screen-space lighting renders geometry into a
**4x MSAA depth target**, then binds that depth aspect as a **1-sample
R32G32B32A32Sfloat sampler2D** for the lighting accumulation pass,
SSAO, soft particle edges, etc. shadPS4's resolver didn't have a path
for this combination, so the `else` branch just `FreeImage`-d the
cached MSAA depth and returned an uninitialised 1-sample color image.
Every depth-based effect downstream was reading garbage.

**Why day was "only dim" and night was "pure black":** day scenes have
strong ambient + sun lighting baked into the material pass; the broken
depth-sampled passes just subtly wash out details we papered over
with exposure. Night scenes rely entirely on screen-space volumetric
headlights keyed off the depth buffer — no depth → no lights → no
scene.

### Fix shape in this branch

Mirror of the existing `BlitHelper::ReinterpretColorAsMsDepth` path
(which already handles the opposite direction, 1x color → MSAA depth).

- **New fragment shader**
  `src/video_core/host_shaders/ms_depth_to_color.frag` — fullscreen
  triangle, binds source as `texture2DMS`, `texelFetch`es sample 0 per
  pixel, writes `vec4(depth, 0, 0, 1)` to the color attachment. Sample
  0 is used verbatim rather than averaging — depth sampling downstream
  wants a specific visibility decision, not an intermediate.
- **New helper**
  `BlitHelper::ReinterpretMsDepthAsColor(width, height, num_samples,
  src_fmt, dst_fmt, source, dest)` — creates a depth-aspect sampled
  view on the source, a color-aspect attachment view on the dest,
  binds a cached pipeline keyed by `(num_samples, dst_format)`, draws
  the fullscreen triangle through the new fragment shader. Pipeline's
  `rasterizationSamples = e1` because the destination is 1x; the
  shader itself does the per-sample fetch.
- **Wiring**: new `else if` branch in
  `TextureCache::ResolveDepthOverlap` that matches the shape
  `cache.is_depth && cache.samples > 1 && !new.is_depth &&
  new.samples == 1` and dispatches `ReinterpretMsDepthAsColor`. The
  pre-existing `else` is kept with the shape-logging instrumentation
  so any *other* unhandled combination is still caught for future
  debugging.

### Verified behaviour

Confirmed on the user's RTX 5090 + 7950X3D box after redeploying the
binary through the gamma-debug QtLauncher entry:

- `[depth-dbg] Unimplemented depth overlap copy` warnings disappear for
  the D32Sfloat → R32G32B32A32Sfloat case (Driveclub's only offender).
- Night tracks: car headlights illuminate the road, track boundaries
  visible, AI car lights visible — screen-space lighting stack back
  online.
- Day tracks: subtle tightening of depth-dependent effects; no
  regressions observed.

### Status

**Resolved.** Both mechanisms addressed. Candidates for upstream:

1. `BlitHelper::ReinterpretMsDepthAsColor` + its shader + the
   `TextureCache::ResolveDepthOverlap` wiring are a clean, bounded
   fix. Good upstream PR shape — symmetric with the existing
   `ReinterpretColorAsMsDepth` path. Drop the `[depth-dbg]` tag
   before submitting; the shape-logging instrumentation is a
   reasonable addition on its own but can be a separate PR or left
   out of the fix PR.
2. The 60 fps patch issue is user configuration rather than an
   emulator bug, so nothing to upstream from that root cause —
   documenting it here for any future Driveclub guide on the
   compatibility repo.
