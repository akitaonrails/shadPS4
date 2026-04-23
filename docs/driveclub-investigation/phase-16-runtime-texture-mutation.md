## Phase 16 — runtime texture mutation, rotated

The Phase 15 texdump exposed ~1100 unique textures per race session
but the dark-car-static-image only showed up intermittently, and PNG
previews of tiled dumps were too scrambled to triage by eye. Rather
than port the detile math, we switched to mutating textures in place
at the emulator and watching the scene react.

### Scaffolding

`MaybeNukeDriveclubTexture(const VideoCore::Image& image)` was added
next to the dump hook in `Rasterizer::BindTextures`. On first bind of
each unique guest address, it overwrites the full `info.guest_size`
with a per-texture deterministic tint (splitmix64 hash of the guest
address → RGB565 for BC1/BC3, RGBA for uncompressed) and then calls
`texture_cache.InvalidateMemory(addr, size)` so the next
`FindImage` re-uploads the clobbered bytes to the Vulkan image.
Without that invalidation the cache keeps serving the original
upload and the writes are invisible — confirmed the first time we
tried, where 1093 nuke events fired with no visual effect.

Gates:

- `SHADPS4_DC_TEX_NUKE_ALL=1` — match every bound texture
- `SHADPS4_DC_TEX_NUKE_ADDRS=0xAAA,0xBBB,…` — explicit addresses
- `SHADPS4_DC_TEX_NUKE_PIPES=0xHHH,0xIII,…` — every texture bound by
  these pipelines
- `SHADPS4_DC_NUKE_SKIP_PIPES=…` — exclude list
- `SHADPS4_DC_NUKE_KIND=scene|postfx|ui|other|all` (default `scene`)
  — classifier based on RT pattern
- `SHADPS4_DC_NUKE_AFTER_ARM=N` — skip the first N race-gate arms
  (menus/panorama). Default 0.
- `SHADPS4_DC_NUKE_MIN_DIM` / `_MAX_DIM` — size bracket
- `SHADPS4_DC_NUKE_MAX_ASPECT` — reject long strips above this ratio
- `SHADPS4_DC_NUKE_FORMAT=<substr>` — format name substring filter
- `SHADPS4_DC_NUKE_INCLUDE_DATA=1` — re-include float/int data formats
- `SHADPS4_DC_NUKE_INCLUDE_GPU_MOD=1` — re-include render targets
  being re-sampled

A draw-kind classifier (`ClassifyDriveclubDraw(regs)`) sets a
per-draw atomic at `Draw()`/`DrawIndirect()` entry so BindTextures
can know whether it's inside a scene / post-fx / UI draw without
inspecting regs itself.

All of this is env-gated — with no env var set the probe is a zero-
cost no-op and the emulator behaves exactly as baseline.

### The 32×32 LUT finding

Early runs with every-texture nuke produced a uniformly cyan (then
yellow, then again cyan on re-hash) fullscreen scene: HUD still
visible, 3D scene completely flat. Interpretation: one nuked
texture was a fullscreen-collapser — a LUT that the tonemap reads
and which maps every input to a single colour.

Bracketed binary search on size:

1. `MIN_DIM=1 MAX_DIM=31` → scene intact, blackout unchanged. Nothing
   dim-related in that bracket.
2. `MIN_DIM=32 MAX_DIM=63` → scene collapses to a flat hue. Hit.
3. `MIN_DIM=32 MAX_DIM=47` → still collapsed. Keep bisecting.
4. `MIN_DIM=32 MAX_DIM=39` → collapsed.
5. `MIN_DIM=32 MAX_DIM=32` → still collapsed. Size locked at 32×32.

Log of the 32×32 nuke showed four candidates:

    0x5007b53400  R8G8B8A8Unorm       pipe 0xefc48aef4d3fc629
    0x5003896400  R8G8B8A8Unorm       pipe 0x1817f4403d552f1f
    0x50ba119c00  Bc1RgbaSrgbBlock    pipe 0x6dd8a5ae9841fc54
    0x50b884d000  Bc1RgbaSrgbBlock    pipe 0x6dd8a5ae9841fc54

Testing the two R8G8B8A8 candidates together still collapsed.
Testing `0x5007b53400` alone — collapsed. **That's the colour-grading
LUT** bound to pipeline `0xefc48aef4d3fc629` and sampled by the
tonemap path. 32×32 R8G8B8A8 is exactly the unrolled shape of a
32×32×32 colour cube or a 2D LUT slice.

Killing this LUT does not affect the blackout — during the flat-cyan
session the mirror visibly goes black and returns just like baseline.
The LUT is downstream of the dim. It is documented here so we can
*skip* it in future texture nukes (it produces a false-positive
scene collapse that hides whatever we're actually trying to see).

### The batch rotation — confirming no texture is the dim

With the LUT identified we rotated every remaining skipped category
through `SHADPS4_DC_NUKE_*` knobs. One env-var change per round,
one game session per round, no rebuild:

| round | filter                                  | observed visual           | dim? |
|-------|-----------------------------------------|----------------------------|------|
| 1     | `MIN_DIM=1 MAX_DIM=31`                  | coloured scene             | yes  |
| 2-5   | 32..63 → 32..47 → 32..39 → 32..32       | flat colour scene (LUT)    | yes  |
| 6     | LUT alone `0x5007b53400`                | flat colour scene          | yes  |
| 7     | `MIN_DIM=64 MAX_DIM=127`                | coloured scene             | yes  |
| 8     | strips only (`MAX_ASPECT=0`)            | coloured scene             | yes  |
| 9     | `INCLUDE_DATA=1 FORMAT=Sfloat`          | normal-looking scene       | yes  |
| 10    | `INCLUDE_GPU_MOD=1`                     | coloured scene             | yes  |
| 11    | `FORMAT=Bc4`                            | normal-looking scene       | yes  |
| 12    | `FORMAT=Bc5` (normal maps)              | normal-looking scene       | yes  |
| 13    | `KIND=postfx`                           | HUD glitched, scene clean  | yes  |
| 14    | `KIND=ui`                               | baseline                   | yes  |

In every single round the blackout continued to happen and the
mirror continued to go black for the usual 8–30 s window. The
user-reported blackout is therefore **not texture-content driven**.
No asset texture, LUT, normal map, HDR feed, UI atlas, post-fx
buffer, or GPU-modified render target is the dim.

### What this rules out

Durable conclusions that hold regardless of how the remaining
investigation turns out:

1. The blackout is not a fullscreen overlay *image* (no texture to
   nuke matches the dim window).
2. The blackout is not a mask-on-HDR composited through a BC1 / BC3
   alpha layer.
3. The blackout is not an LUT mis-swap — the colour-grading LUT is
   `0x5007b53400` and nuking it decouples from blackout timing.
4. The blackout is not in any sampled texture on any scene-material
   pipeline, including HDR feeds and GPU-modified intermediates.

So the dim must live in one of:

- a **UBO field** (shader uniform) that multiplies scene output —
  the Phase 13 probe only covered ~12 hand-picked pipelines; the
  full race-window scene pipeline set is >60 and was never diffed
  exhaustively
- a **push constant** — we have not instrumented these at all
- a **compute dispatch** output (auto-exposure / bloom / luma /
  histogram) that writes a value the graphics pipelines then read —
  drawlog does not see compute and the texnuke covers only sampled
  images, not storage buffers that compute writes to
- a **fixed-function pipeline state** — constant blend colour, blend
  factor, colour-write mask

### Path forward (next sessions)

Ranked by payoff vs effort:

1. **Compute dispatch probe** — add `SHADPS4_DC_DISPATCHLOG=1` that
   logs every `Rasterizer::Dispatch` during the race window with
   its buffer bindings, and `SHADPS4_DC_UBO_NUKE_ALL=1` that runs
   the same hash-tint scheme on every compute-bound storage/uniform
   buffer. Biggest blind spot in the diagnostic stack and the most
   likely single home for the dim given that Driveclub's bloom /
   exposure run on compute.
2. **Exhaustive UBO nuke** — port the same bracketing logic we used
   for textures to the UBO bind path: for each draw, rewrite each
   bound UBO's guest memory with a per-draw random fill, invalidate
   the buffer_cache range, let the game's next per-frame UBO write
   repopulate. If the dim disappears during a specific
   size/pipeline bracket we've found the uniform that drives it.
3. **Push-constant sweep** — lower-priority; only warranted if both
   above come back empty. Push constants are 128 bytes at most on
   AMD/GCN; a sweep would rewrite them per-draw just before
   `cmdbuf.pushConstants()`.

### Commit state at end of Phase 16

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` now carries:

- `ClassifyDriveclubDraw` (scene / postfx / ui / other)
- `g_driveclub_current_pipeline_hash`, `g_driveclub_current_draw_kind`,
  `g_driveclub_arm_count` atomics
- `MaybeNukeDriveclubTexture` with all the knobs above
- `MaybeDumpDriveclubTexture` (Phase 15)
- `MaybeClampDriveclubLuminanceUbo` / `MaybeRestoreDriveclubExposureUbo` /
  `MaybeNukeDriveclubExposureUbo` (Phase 13)
- race-gate now increments `g_driveclub_arm_count` and logs `armed#N`

Nothing changes emulator behaviour unless a `SHADPS4_DC_*` env var
is set. Safe to leave on the branch while the investigation moves
to compute.
