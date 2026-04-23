## Phase 21 — mapping fragment shaders to the real flip buffer

### Setup

Full pipeline cache wipe + shader-dumps wipe + empty patch dir so
every shader is recompiled fresh. Run with `SHADPS4_DC_DRAWLOG=1
SHADPS4_DC_DISPATCHLOG=1` only, no mutation. Fresh recompile fires
the `[dc-pipemap]` logger for every graphics pipeline, giving us a
complete map from pipeline hash to per-stage shader pgm_hashes.

Run stats: 440 pipemap entries, 1510 fs shaders + 510 cs shaders
dumped, timeline covers 64 s. Blackout 18:07:30..18:07:40 (10 s),
user-reported.

### The LogicalStage number gotcha

`[dc-pipemap]` prints `stages=N=0x...` using LogicalStage indexing,
not the Stage enum used in dump filenames. Correct mapping:

    LogicalStage::Fragment            = 0   ← dump prefix "fs_"
    LogicalStage::TessellationControl = 1
    LogicalStage::TessellationEval    = 2
    LogicalStage::Vertex              = 3   ← dump prefix "vs_"
    LogicalStage::Geometry            = 4
    LogicalStage::Compute             = 5

My first pass took `stages[3]` as fragment and ended up with
vertex-shader hashes. Correct stage-0 lookup yields real fragment
shaders.

### Pipelines writing to the actual flip buffers

`sceVideoOutRegisterBuffers` says the presented frame comes from
`0x5000900000` and `0x5000108000`. `0x500cdd0000` that earlier
phases kept returning to is actually a G-buffer MRT target, not
what the user sees.

Drawlog filter `rts == 0x5000900000 OR 0x5000108000, depth=no`
yields 14 unique pipelines. Crossed against `[dc-pipemap]` for
their fragment pgm_hash:

**Always-on (fires pre + dim + bright — candidate tonemap / final composite):**

    pipeline            fs hash        pre  dim  bright  notes
    0x27666d94b4423505  0xf93edfcf      517  338  760    2 ssbo + 1 img, 9 samples
    0x652a18722d6b6326  0x78662cab      517  338  760    same structure, variant
    0x20df10c7ad503b04  0x914c1f74      496  338  760    1920×1080 Bc7Srgb photo compositor
    0x1c2223026ec47d56  0xcf007151      155  338  760    2 ssbo + 5 img, 10 samples — biggest

**In-race-only (pre=0 — kick in at race start, candidate dim applier):**

    pipeline            fs hash        pre  dim  bright  notes
    0x0d3e2852f666c342  0xcc3e3973      0    324  760    2 samples, 8 FMul
    0x679ef877c9ddbadd  0x0aff1952      0    324  760    1 sample, 4 FMul (also used by 0x64d30a63381f04b6)
    0xa58dd401e0038afd  0xae595320      0    324  760    1 sample, 3 FMul — simplest
    0x71ffc1def01f4747  0x1ddb9986      0    324  760    2 samples, 7 FMul

**None of the 4 in-race-only fs shaders contain Exp2 or Log2.**
So the dim is not applied through classic `pow(x, gamma)` tonemap
math in these compositors. It's either:

- pre-multiplied from a `ssbo_1` scalar bound across these
  pipelines, or
- applied upstream (before reaching the flip-buffer composite) and
  the in-race-only shaders just do simple alpha-blend of HUD /
  overlays on top of an already-dimmed scene.

Same story for the four always-on compositors — zero Exp2/Log2.
The tonemap either is done in **compute** and the fragment shaders
just sample its output, or the exposure multiplier is in a
**LUT-sample** path (no math ops visible, just texture lookup).

### Where we stand

Completed mapping — we now know the exact fragment-shader pgm_hashes
of every pipeline that writes the frame the user sees. The path
the scene pixels take is:

- G-buffer MRT writes at `0x500cdd0000 / ABE / D5C` (hundreds of material shaders)
- Deferred-lighting / tonemap → HDR composite (TBD — probably compute)
- Final composite into flip buffer (`0x5000900000 / 108000`) via
  the 8 fs shaders named above
- Presenter blits flip buffer to swapchain

The dim happens somewhere in the HDR→SDR step, and it is NOT in
the 8 flip-buffer fragment shaders (no exp/log tonemap math, only
alpha-blend compositing of pre-baked input).

### Saved artefacts for next session

- Full baseline log (100 MB, 440 pipemap + 3465-frame timeline):
  still at the usual `shad_log.txt` path (mtime 18:07:59).
- All 1510 fragment shader SPIR-V dumps + 510 compute:
  `~/.local/share/shadPS4/shader/dumps/`
- Fragment-shader hash shortlist (in-race-only and always-on flip
  writers) documented in the tables above.
- Pipeline cache *snapshot* backup at
  `cache/CUSA00003.snapshot-phase21-<ts>` in case cache state
  matters for debugging reproducibility.

### Planned Phase 22 — hunting the tonemap among compute

Next concrete move: search cs_*.spv for shaders that

- Write into a storage buffer whose address matches `fs_img0` of any
  of the 8 flip-writer fragment shaders (they read an HDR
  intermediate — that intermediate is the compute output)
- Use Exp2/Log2 + multiplies on read values (classic tonemap math)

The existing `[dc-dispatchlog]` already carries `pipeline=... shader=...`
for every compute dispatch, so we can cross-reference with the
draw order inside a submit: the compute whose output is read by the
immediately-following graphics draw that writes the flip buffer is
the tonemap.

If that lands us at a cs_*.spv with exposure math, we patch *that*
shader to hold exposure at 1.0 and confirm the dim stops.

### Commit state at end of Phase 21

- `vk_rasterizer.cpp` gains `NoteDriveclubDispatchlog(cs_hash, shader_hash, dim_x, dim_y, dim_z)` signature (shader hash added).
- `video_out.cpp` `sceVideoOutAdjustColor` log now prints the full 16-byte
  color-settings payload.
- No behaviour change when no `SHADPS4_DC_*` env var is set.
