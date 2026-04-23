## Phase 22+ — tonemap compute patches (real mitigation)

### The SPIR-V surgery that works

Two compute shaders hand-patched in
`~/.local/share/shadPS4/shader/patch/`. shadPS4's
`patch_shaders: true` flag already on in the game config, so
patches load automatically — the only catch is that
`GetShaderPatch` is consulted during fresh compiles only, so the
pipeline cache has to be wiped before each patch change.

**`cs_0x00000000eea18ba7_0.spv`** — the histogram auto-exposure.
Replaced with a no-op shader that references the same 3 SSBO
bindings (to keep the descriptor set layout valid) but never
writes an updated exposure value. Without this, the adaptation
loop ticks on and pulls exposure towards whatever the histogram
says the scene should be, which is the loop we don't want.

**`cs_0x000000002c918c06_0.spv`** — the tonemap compute, surgical
SPIR-V edit:

1. `%1158 = OpBitcast %f32_id %1124` (the ssbo_5 exposure scalar
   read) →
   ```
   %1999 = OpBitcast %f32_id %1124
   %1158 = OpExtInst %f32_id %360 FMax %1999 %f32_id_0_5
   ```
   Floors the compositor's post-fx weight at 0.5, so the blend
   term stays non-trivial even when the game's time-varying value
   collapses.

2. After `%1111..1113 = OpCompositeExtract %f32_id %1110 {0,1,2}`
   (the `cs_img48` fetch — the blend term's primary scene colour),
   inserted three new FMul lines:
   ```
   %2013 = OpFMul %f32_id %1111 %f32_id_10
   %2014 = OpFMul %f32_id %1112 %f32_id_10
   %2015 = OpFMul %f32_id %1113 %f32_id_10
   ```
   The downstream Fma chain into `%1147/%1152/%1156` then uses the
   boosted ids instead of the raw samples. The `×10` boost turned
   out to be playable; `×255` overexposed heavily, `×8` was too
   subtle.

`cs_img64` (the HDR add term) boost was tried and turned out to be
a reflection/auxiliary layer rather than the main scene — boosting
it only blew out the windshield reflection. `cs_img48` is the
actual scene / blend-base.

### What the patches produce visually

Both screenshots recorded at 18:41:
- during the would-be blackout (race start + a few seconds): scene
  dimmer than after, but clearly playable — road, barriers, HUD,
  other cars all legible.
- after the lift: normal dusk lighting, roads wet, distant
  mountains, clear sky.

No more pitch-black 5–30 s window. The whole race stays
navigable.

### The remaining gap — what's special about race start

User observation that reframes everything: the scene **starts
fully lit and correctly coloured for the first ~1 second of the
race**, then the blackout kicks in and drags everything dark for
5–30 s before lifting again.

This isn't an auto-exposure warm-up: a warm-up would go *from dim
to bright*. The actual path is **bright → dim → bright**. So
something happens at the 1-s mark that drags the exposure low
(or equivalent state). That's consistent with Codex's Phase 12
"camera fade" hypothesis: the game scripts a race-start fade
whose animation curve dips brightness then restores it. The dip
is what the user experiences as "blackout", and the restore is
what they call "colour coming back".

Codex's earlier string-corruption of `CameraFade` /  `SetCamera`
changed how the dip looked but didn't remove it. That's
consistent with the dip not being a single string-table toggle;
it's an animation curve driven by the engine.

### Where to hunt next — the material shader search

All ~hundreds of pipelines that write the MRT G-buffer
(`rts=0x500cdd0000,0x500abe0000,0x500d5c8000 depth=yes`) are
material shaders. If the fade animation multiplies a shared
uniform into each material's final colour write, every one of
those fs shaders reads that uniform at the same offset. Pin that
offset and the fade stops affecting materials — the tonemap patch
alone then keeps the visible pixels bright.

Plan for next session:

1. From the Phase 21 clean baseline log (100 MB, pipemap =
   440), enumerate every distinct MRT G-buffer pipeline and
   resolve its fragment pgm_hash via `[dc-pipemap]`.
2. Dump the SPIR-V of the top 3–5 highest-frequency material fs
   shaders.
3. Look for a common UBO load + FMul into the colour output that
   appears in all of them at the same SSBO offset. That's the
   shared fade uniform.
4. Patch a handful of those material shaders to replace that
   FMul with OpCopyObject of 1.0. If scene stays bright
   regardless of race timing, we've found the fade. Then either
   patch every MRT shader the same way, or (better) patch the
   upstream CPU-side code that writes the fade value.

### Commit state at end of Phase 22

- `shader/patch/cs_0x00000000eea18ba7_0.spv` (histogram no-op, compiled from
  `tmp/dc-patches/exposure_noop.comp`)
- `shader/patch/cs_0x000000002c918c06_0.spv` (SPIR-V surgical edit,
  sources at `tmp/dc-patches/tonemap_cs_x10.dis` and script in
  this doc)
- Snapshot backups of the pipeline cache under
  `cache/CUSA00003.snapshot-phase22*` in case any of them need to
  be re-inspected.

The race is playable with these patches. The dim is reduced to a
mild dimming in the first seconds of a race instead of a total
blackout. It's not the final fix (the fade curve still drags
exposure), but it's good enough to play.
