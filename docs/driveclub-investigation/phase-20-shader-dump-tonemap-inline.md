## Phase 20 — shader dump analysis + the tonemap-is-inline discovery

### Setup

- `SHADPS4_DC_DISPATCHLOG` extended to log the compute shader's
  `info.pgm_hash` alongside the pipeline hash, so each dispatch line
  cleanly maps to a dumped `cs_0x<pgm_hash>_<perm>.spv` file.
- Pipeline cache for `CUSA00003` snapshot-moved aside so shadPS4
  recompiles every shader on the next launch and dumps all of them.
  `patch_shaders` in the game config was already `true`, so any
  `.spv` dropped into `shader/patch/` is picked up automatically.
- `sceVideoOutAdjustColor` log extended to print the full 16-byte
  `SceVideoOutColorSettings` payload (gamma + 3 reserved dwords) to
  see if the game sneaks an animated value into the reserved slots
  that shadPS4 otherwise ignores.

### First real find: the (1,1,1) auto-exposure shader

After a full recompile pass, 112 unique compute shaders dumped. The
`(1,1,1)` scalar-writer `cs_0x00000000eea18ba7_0.spv` disassembles
cleanly as a classic histogram-based auto-exposure reduction:

- `ssbo_1` is a 256-entry luminance histogram (input).
- `ssbo_2` is a parameter block: adapt rate, min/max clamps, target
  mid-grey, previous-frame exposure — the usual eye-adaptation
  controls.
- `ssbo_3` is the output, one `vec2<f32>` written as two u32 stores
  at `buf2_dword_off + 0` and `+1`. The first u32 is the adapted
  exposure (after `Fma + Log2 + clamp`), the second is the last
  max-luminance sample.

The bit-field unpacking of `push.buf_offsets0` into three 8-bit
slots, the `FMA(0.0352941, index, -4.0) → FMul 3.32192802 → Exp2`
sequence (log10-to-log2 conversion on the histogram index), and
the `FDiv mid_grey / avg_lum → Log2 → Fma(adapt_rate, ..., prev) →
Exp2` chain are unambiguous auto-exposure. Nothing hidden in the
math.

### First test: patch the exposure writer

Wrote a minimal GLSL replacement that keeps the same descriptor
layout (SSBO 0/1/2) and the same push-constant packing, but writes
`floatBitsToUint(1.0f)` to both output slots instead of the adapted
value. Compiled with `glslangValidator -V --target-env vulkan1.3`,
dropped at
`~/.local/share/shadPS4/shader/patch/cs_0x00000000eea18ba7_0.spv`,
pipeline cache wiped again to force the patch-aware
`CompileModule()` path (`GetShaderPatch` is only consulted on fresh
compile — the disk cache bypasses it).

Result: blackout 17:47:34 → 17:48:00, 26 s. **Same behaviour as
baseline.** The patched shader fires every frame writing `1.0` to
its output, yet the dim continues on its normal schedule. `ssbo_3`
is therefore not the path the tonemap reads — or there's a
redundant writer.

### Second test: shotgun idea vs reality

The user proposed patching all 8 dim-only compute shaders to
bypass their math as a confirmation. Short-circuited it: Phase 19c
already ran a *stronger* version via `SHADPS4_DC_DISPATCH_SKIP=`
listing every one of those 8. Skipping means no writes at all;
patching to constants is a subset (writes *something*, but not the
computed value). Neither shifted the dim. Patching the other 7
wouldn't change the outcome and each would need bespoke GLSL to
match its SSBO / image / sampler layout.

### Third test: VideoOutAdjustColor reserved fields

3992 calls across a full race with blackout + lift. Every payload:

    gamma=0.5 reserved=[0x3f800000, 0xe484a0, 0x20]

Zero variation. `reserved[0]` is a constant `1.0f`, `reserved[1]`
is a fixed guest-memory pointer, `reserved[2]` is constant 32. No
animated second-gamma / second-brightness hidden in there. The
videoout API is not the dim path.

### The load-bearing conclusion

Driveclub has **no separate tonemap pass** in the graphics
pipeline. Scene geometry pipelines write directly to the visible
composite `0x500cdd0000` as part of the MRT G-buffer draw — the
three writes-to-composite-alone pipelines we'd previously suspected
for tonemap (`0x2bd7..ae2`, `0xadd2..da9`, `0xe6e4..d99`) are the
HUD/UI compositors (already confirmed by texnuke breaking the HUD).

So every material shader has the exposure / fade multiplier baked
in inline, reading the dim scalar from a shared UBO or SSBO that's
bound to most scene draws. The 101 "dim-only" graphics pipelines
from Phase 19b are various cars / props / environmental meshes
that all see that shared value.

### Reframing the attack surface

Every shader-reachable surface has now been cleared except a
**single UBO offset inside a shared scene-constants buffer**. That
offset is read by hundreds of scene material shaders and baked
into their fragment output. Any broad probe of that UBO crashes
because it co-hosts matrices and descriptor pointers. Every
previous null result (texture nukes, UBO smashes, push-constant
rewrites, dispatch skips, draw skips, exposure-shader patches)
falls out of this single constraint: we've been probing *around*
that offset, not at it.

### Planned Phase 21 — fragment-shader inline exposure sweep

Approach:

1. Pick the top three MRT-writing pipelines (by draw count) from
   the clean baseline log.
2. Walk each `[dc-pipemap]` entry to resolve its fragment-shader
   pgm_hash, and pull the matching `fs_0x<pgm_hash>_*.spv` from
   the dumps.
3. Disassemble each; find the final `OpStore` to the colour output
   `OpVariable` tagged `RenderTargetIndex 0` (that's the write to
   `0x500cdd0000`). Trace backwards to the multiplicand chain and
   identify every UBO field that multiplies into it.
4. Patch one fs shader: replace each candidate UBO-read with a
   constant (`1.0f` in the exposure slot, identity for everything
   else). Compile and drop in `shader/patch/`. Run the race.
5. Observe: for the meshes drawn by that specific pipeline, does
   the dim disappear? If yes — we've found the field and its byte
   offset in the UBO. Walk back to where that UBO address is
   written (game-side) and we're out.
6. "Shotgun" form of the same: apply the patch to all top-N most-
   used material fs shaders at once. If dim disappears on all
   patched pipelines simultaneously, it is definitively a shared-
   UBO inline multiplier; then remove patches one-by-one to find
   the minimal set.

This keeps iteration fast (shader patch files, no emulator
rebuild) once the first mapping is done.
