## Race-start blackout (2026-04-20 follow-up)

### Symptom

Separate from the earlier SDR/brightness chase: at the start of a race,
the world layer would sometimes fade almost to black while the HUD
remained fully visible. Day tracks sometimes recovered after 5-10
seconds; dusk could stay black for 30+ seconds or never come back.

### What the probes ruled out

- Not final present / post-process: timed "game-only" screenshots showed
  the raw guest image already black.
- Not `sceVideoOutAdjustColor` gamma: forcing a neutral
  `SHADPS4_VIDEOOUT_GAMMA_OVERRIDE=1.0` did not change the behaviour.
- Not `ResolveDepthOverlap`, degamma, min/max blend, or predication:
  dedicated logging for those paths stayed quiet during the blackout.

### What the probes showed

The useful signal came from instrumenting the `1920x1080` non-video-out
scene targets and then the texture cache itself:

- `0x5009688000` (`B10G11R11UfloatPack32`) is the main suspect target.
- During blackouts, that target repeatedly dropped to near-zero while
  sibling float targets such as `0x5008130000` / `0x500fdd0000` stayed
  populated.
- `TextureCache::FindImage()` / `ExpandImage()` showed repeated same-address
  churn on `0x5009688000`:
  create new image -> `ExpandImage()` -> `CopyImage()` from
  `0x5009688000` to `0x5009688000` -> reselect via `used_overlap=true`.

That pattern points to a generic texture-cache alias/recreation bug, not
a Driveclub-specific post-process quirk. Driveclub is just a very visible
repro because its scene pipeline is sensitive to losing the HDR-like world
buffer for even a few frames.

### Experimental fix in `gamma-debug`

`TextureCache::ExpandImage()` previously did this unconditionally:

1. create the replacement image
2. `RefreshImage(new_image)` from CPU memory
3. `CopyImage(src_image)` from the old GPU image

For same-address GPU render targets, step 2 is likely wrong: the CPU-side
backing for that guest address is stale or zero, while the live contents
exist only in the GPU render target. The branch now skips the CPU refresh
when all of these are true:

- same guest address
- source image is/was a render target
- source image `SafeToDownload()` (GPU contents authoritative)

In that path the expanded image now preserves GPU-authored contents,
inherits the old usage bits, and carries `GpuModified` forward.

### Expected effect

If the diagnosis is right, the race-start blackout should become stable or
disappear entirely, especially on dusk tracks. The expected win is not
"brighter output"; it is keeping the world render target from being
recreated out from under the game.

### Follow-up: equal-address variants were still diverging

The first `ExpandImage()` preservation fix was not enough on its own.
Later probes showed two more details:

- `ResolveOverlap()` was still freeing expanded same-address variants on
  the `equal-address incompatible block` path, even though the guest
  address / format / tile mode matched and Driveclub was just rebinding
  the same target with different resource shapes (`1/1`, `9/1`, `10/1`,
  `11/1`).
- After preserving those variants, `FindImage()` could still hand back
  the smaller exact-size image, while a larger compatible same-address
  image already existed in the cache.

That second point matters because these are separate Vulkan images. If a
smaller `1/1` image is selected for one pass and the larger `11/1`
image is selected for a later pass, their contents diverge even though
they represent the same guest memory. That gives exactly the kind of
intermittent "world goes black, HUD survives" behaviour seen in
Driveclub.

Current `gamma-debug` branch status:

- same-address mip/resource-shape variants are preserved instead of
  immediately freed when the format / type / tile mode stay compatible
- `FindImage()` now prefers the largest compatible same-address image as
  the canonical backing image for subsequent lookups

This keeps the cache from bouncing between multiple independent Vulkan
images for `0x5009688000` and related scene targets.

### Follow-up: later composite stage and bundled shader-input probe

Subsequent traces weakened the "main HDR target cache churn" theory as
the sole cause. The stronger pattern became:

- the later `1920x1080` `RGBA8 sRGB` scene target at `0x500cdd0000`
  goes black during the race-start blackout
- larger upstream scene buffers can stay populated at the same time
- the recurring in-race composite shader for that target is
  `fs_hash=0x1b0d793e`
- that shader repeatedly samples a few small inputs, most notably:
  - `0x505fdf4800` `BC6HUfloat` `128x128`
  - `0x505ff45200` `BC6HUfloat` `256x1024`
  - `0x505fe57600` `R8G8B8A8Unorm` `512x256`

Importantly, those smaller sampled inputs did **not** show the same
free/recreate/overlap churn that `0x5009688000` showed earlier. They
were created once and then reused normally. That pushed suspicion away
from a second texture-cache lifetime bug and toward content correctness
inside the composite stage itself.

To accelerate the next round of testing, the `gamma-debug` branch now
contains a bundled visual probe in `vk_rasterizer.cpp`:

- only when drawing to `0x500cdd0000`
- only for fragment shader `0x1b0d793e`
- sampled inputs at the three addresses above are substituted with
  1x1 solid debug textures
  - `0x505fdf4800` → bright red
  - `0x505ff45200` → bright green
  - `0x505fe57600` → bright blue

The point of this probe is not to "fix" Driveclub. It is to make the
composite stage fail loudly and directionally, so one run can answer
multiple questions at once:

- does the blackout path actually depend on those small sampled inputs?
- which substituted input dominates the visible result?
- does replacing them change the blackout shape, duration, or recovery?
