## Post-reset clean branch log (2026-04-21)

This section supersedes the contaminated "stacked torture" phase that
followed the note above.

I explicitly restored these files to `HEAD` before resuming:

- `src/video_core/renderer_vulkan/vk_compute_pipeline.h`
- `src/video_core/renderer_vulkan/vk_rasterizer.cpp`
- `src/video_core/renderer_vulkan/vk_rasterizer.h`
- `src/video_core/texture_cache/texture_cache.cpp`

Only the experiments below should be treated as clean evidence. The
older stacked torture runs changed too many overlapping branches and
should **not** be used to rule out families anymore.

### Clean baseline reset

- The renderer debug stack was removed and rebuilt from a clean
  baseline.
- `vk_rasterizer.cpp` kept only one persistent convenience change:
  Driveclub `CUSA00003` auto-enables the experiment path when launched
  from Manager.

### Branch 1 — late fixed-function family on tracked race-target draws

Shot on:

- viewport/scissor
- depth/stencil/bounds/bias
- cull/front-face/discard
- blend constants
- color write masks
- attachment feedback loop

Gate:

- graphics draws whose render targets matched the known Driveclub race
  chain (`0x500cdd0000`, `0x5009688000`, `0x5008130000`, `0x500fdd0000`)

Observed result:

- HUD stayed intact
- scene started with a hint of color, then faded to black
- normal blackout timing remained

Conclusion:

- This was a **clean miss**.
- Broad late graphics fixed-function state on the known race-target
  draws is not enough to hit the blackout switch.

### Branch 2 — broad race-scene draw shot on tracked targets

Added on top of the clean baseline:

- null sampled textures on race-target draws
- forced color clears/loadops in `BeginRendering` for the tracked race
  targets
- depth/stencil clear suppression on those draws

Observed result:

- screen went orange / yellow underneath intact HUD
- mirror still started black, then recovered later in a tinted state
- blackout state clearly still existed

Conclusion:

- This hit the visible race-scene branch hard
- but **did not** hit the blackout controller itself
- useful only as evidence that "recoloring the main race image" is not
  the same as killing the blackout

### Branch 3 — transfer / movement family

The draw-side repainting above was removed. The next clean shot moved
to transfer-style operations:

- `EliminateFastClear`
- `Resolve`
- compute image copy
- compute image clear

Arm logic:

- arm on the known race path, then apply the shot for a short submit
  window to scene-sized color images

Observed result:

- all black scene
- HUD intact
- blackout still there
- after recovery, the image came back heavily corrupted

Conclusion:

- This was another **clean miss** for the blackout switch.
- Transfer-style image movement / clear operations alone do not explain
  the fade-to-black behavior.

### Branch 4 — remaining draw-state family, combined with transfer shot

Kept Branch 3 alive, then added the untouched graphics draw-state
bucket on race-target graphics passes:

- zero `push_data`
- null `Flatbuf` user-data buffers
- null all non-special read-only descriptor buffers

Observed result:

- all black scene
- HUD intact
- mirror black
- blackout unchanged

Conclusion:

- This was also a **clean miss**.
- Per-draw user-data / flat user-data / read-only buffer descriptors on
  the known race-target graphics path are not sufficient to hit the
  blackout switch.

### What these clean misses actually rule out

After the clean reset, the following families have now been probed and
failed to disable or materially destabilize the blackout itself:

1. Late fixed-function graphics state on tracked race-target draws
2. Broad sampled-texture / forced-clear corruption of the tracked main
   race-scene targets
3. Transfer-style image movement and clear family (`Resolve`,
   `EliminateFastClear`, compute image copy/clear)
4. Per-draw graphics-side user-data / flatbuf / read-only descriptor
   buffer family on tracked race-target passes

### What this does **not** justify anymore

Do **not** reuse the contaminated post-doc assumptions that:

- "the blackout branch was already hit" because the scene turned green,
  blue, orange, or purple
- "the mirror path was nearly isolated" by those tint runs
- "sampled-input poisoning proved a participating blackout branch" in
  the stacked builds

Those statements came from overlapping experiments before the clean
reset and are not reliable as eliminations.

### Current clean takeaway

After the reset, every broad shot tied to the **known race-scene draw
path** or to its immediate transfer family has still left the blackout
intact.

That means the current clean evidence points away from:

- the visible tracked race-target draw path itself
- the immediate transfer/resolve/compute-copy/compute-clear family for
  those scene-sized images
- the obvious graphics per-draw control-state family on those same
  passes

The next branch must therefore be chosen as a genuinely different
family, not another narrower variation of the same race-target draw
path.

### Probe rule going forward

Do **not** spend build-and-test time on very small follow-up changes
whose most likely outcome is only to reconfirm what is already known.

Use this rule instead:

- reset to clean baseline before each new shot
- one hypothesis family per build
- prefer broad class-level probes over tiny surgical deltas
- only narrow after a family has clearly hit or disturbed the blackout
  itself
- if a task is interrupted to answer another request, resume the
  interrupted task immediately after answering; do not wait for an
  extra "continue" prompt

In other words: no more "small confirmation" shots unless a previous
broader experiment has already proven that the branch is blackout-bearing.

### Race gate rule

Do **not** arm broad blackout shots from "full-res target exists" alone.
That repeatedly hit boot/menu paths and blacked the UI before the race
ever started.

The clean passive discriminator established a safer coarse gate:

- only treat the **later** race-like graphics pipeline hashes as valid
  arm signals
- explicitly exclude the early submit-97 hashes that also appeared
  before the real race-start path settled
- require a recent `sceVideoOutAdjustColor(... gamma=0.5)` pulse before the
  later race-like draw cluster is allowed to arm the race window

That extra gamma hint matters because the pipeline-only guard still caught
menu-loading lookalikes. The baseline-safe guard now requires both:

- a recent `gamma=0.5` video-out hint
- the later race-like draw signature with visible target + HDR target + depth

Later race-like hashes seen in the passive `[dc-gate]` run:

- `0x967922c49cee2dd1`
- `0xe72e555cdb85af86`
- `0xb98e78a7a28007ce`
- `0x3ff0fc8f05bc302d`
- `0xd14226a181106f7e`
- `0x1bb555896c9e247e`
- `0xff9e11acb5a72dff`
- `0x5148c06fb63b96e0`
- `0x660a29eb5e92b0ad`
- `0xfe66b45b84a7dd16`
- `0x02c197f768d8d430`
- `0xf6e5670be11b0009`

Early hashes that must **not** arm shots:

- `0x1cdd747ee89204c0`
- `0x6bde71906ac1af18`

Operational rule:

- if a future broad family needs a race-start gate, it should arm from
  this later-pipeline whitelist first
- do not go back to target-only arming unless a later result proves this
  gate insufficient

Failure note:

- the later-pipeline whitelist **alone** was still not safe enough for
  active PM4 / orchestration shots
- one such broad shot still armed during menu load and crashed with
  `Attempted to access invalid address 0x200`

Persistent baseline guard:

- keep a baseline-safe race window helper in the renderer
- it should stay passive by default and only arm when all of these are
  true in the same submit:
  - at least `3` distinct hashes from the later whitelist
  - depth valid
  - visible target `0x500cdd0000`
  - at least one HDR target among `0x5009688000`, `0x5008130000`,
    `0x500fdd0000`
- future broad shots should key off that latched race window rather than
  the later whitelist alone
