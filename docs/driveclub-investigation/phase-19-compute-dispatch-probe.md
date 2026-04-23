## Phase 19 — compute dispatch probe (crashes, not dim)

### Scaffolding

- New `DriveclubDrawKind::Compute = 4`.
- `Rasterizer::DispatchDirect` / `DispatchIndirect` set the current
  pipeline hash (via `ComputePipeline::GetComputeKey()`) and the
  current draw-kind to `Compute` before `BindResources`, so the
  existing UBO smasher and texture nuker can target compute
  pipelines using `SHADPS4_DC_NUKE_KIND=compute`.
- `NoteDriveclubDispatchlog(hash, dim_x, dim_y, dim_z)` logs one
  `[dc-dispatchlog]` line per (submit, pipeline) tuple inside the
  race window. Enable with `SHADPS4_DC_DISPATCHLOG=1`.

### Dispatch landscape

A single clean race run with `DISPATCHLOG=1` captured 19066
dispatch events over ~30 s inside the race window. The workgroup
shape distribution cleanly identifies the classic HDR-exposure /
bloom compute chain:

| dim              | count | plausible role                               |
|------------------|-------|----------------------------------------------|
| (1, 1, 1)        | 4560  | scalar reduction / final aggregate write      |
| (256, 1, 1)      | 2918  | histogram accumulation (256 luminance bins)   |
| (256, 128, 1)    | 2275  | full-screen tiled dispatch                    |
| (240, 135, 1)    | 1004  | 1920/8 × 1080/8 bloom / luma downsample       |
| (120, 68, 1)     | 282   | 1920/16 × 1080/16 mip                         |
| (64, 64, 6)      | 276   | cube-map post-process (6 faces)              |
| (16, 16, 1)      | 351   | 32× downsample tile-grid                      |
| (8, 8, 1)        | 252   | finer compute tile                            |

`(1,1,1)` alone has **21 unique pipeline hashes**. These are the
prime candidates for "writes a single scalar to an exposure /
gamma buffer that graphics then reads".

### Three-batch mutation sweep, all three crashed

The UBO smasher was re-targeted at compute by setting
`NUKE_KIND=compute`, keeping its existing size/cb filters.

1. **Smash all read-only compute UBOs**
   → crash on race load (SIGTRAP / exit 133).
2. **Smash ≤256 B read-only compute UBOs**
   → crash on race load.
3. **Smash only `(1,1,1)` dispatches' ≤256 B UBOs**
   (pipeline filter applied only to textures, UBO smash stayed
   broad because `MaybeSmashDriveclubUbo` has no pipeline-hash
   filter)
   → crash on race load.

Observation: compute-bound UBOs in shadPS4 very often carry **buffer
addresses / descriptor pointers**, not plain scalars. A random-fill
pattern therefore nulls a pointer that the compute kernel
dereferences next, and the dispatch faults before any useful output
gets written. Every broad batch hits this failure mode.

### Full diagnostic exhaustion table

| phase | surface                     | budget used | outcome             |
|-------|-----------------------------|-------------|---------------------|
| 13    | UBO offset targeted clamp   | per-pipeline| no dim              |
| 15    | texture dump                | —           | observability only  |
| 16    | texture content random-fill | 10 batches  | no dim              |
| 17    | graphics UBO random-fill    | 6 batches   | chaos; no dim       |
| 18    | graphics push constants     | 1 batch     | not consumed        |
| 19    | compute UBO random-fill     | 3 batches   | 3/3 crashes         |

The random-fill approach is now a dead end across the four probe-
reachable layers — any bracket wide enough to include the dim is
wide enough to either mask it (chaos) or kill the frame before the
blackout transition can be observed.

### Three remaining attack paths

1. **Dim-phase dispatch bisect.** Re-run with `DISPATCHLOG=1`, note
   the submit numbers at "mirror goes black" and "mirror animates
   again" (the user already uses those as reliable phase markers).
   Grep the log for pipelines that fire **only** during the dim
   submit range. Phase 12's asset-side bisect applied to compute.
   Low cost, no new code.
2. **Compute-output replay.** Intercept *after* the dispatch —
   right after `cmdbuf.dispatch(...)` — and overwrite the writable
   SSBO(s) the kernel just produced with a known identity value
   (`1.0f`, or a captured-bright-phase snapshot). Zero crash risk
   because the kernel has already finished; we are only
   corrupting downstream reads. Requires a new probe next to
   `NoteDriveclubDispatchlog`.
3. **Eboot disassembly, informed by diagnostic exhaustion.** Codex
   went through 12 phases of eboot RE with cold leads. Repeating
   that work now with "the dim is not in texture / UBO / push-
   constant / compute-input" as a constraint narrows the search
   considerably: the write must either happen in a shader we
   haven't instrumented, in presenter-side post-fx, or in the
   compute output path covered by option 2.

### Commit state at end of Phase 19

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` gains:

- `DriveclubDrawKind::Compute` + its mask entry
- `IsDcDispatchLogEnabled` + `NoteDriveclubDispatchlog`
- dispatch-site instrumentation for pipeline hash + draw kind at
  both Direct and Indirect entry points

`scripts/run_driveclub_overlay.sh` passes through
`SHADPS4_DC_DISPATCHLOG`.

Zero-cost when no `SHADPS4_DC_*` env var is set.
