## Phase 22 — the tonemap compute shader is identified

Scanned all 510 dumped compute shaders for the classic tonemap
signature (≥2 Exp2 + ≥2 Log2 per channel). Five hits.

Cross-referenced each against the dispatchlog to find their
pipeline hash and firing pattern across pre / dim / bright:

| shader              | local | shape        | pre | dim | bright | pipe |
|---------------------|-------|--------------|-----|-----|--------|------|
| `0x2c918c06`        | (16,16,1)| (16,16,1) | 511 | 338 | 759    | `0x000002c995517e7f` |
| `0x798afc37`        | (64,1,1) | (48,1,1)  | 480 | 338 | 759    | `0x000007992c19654b` |
| `0x87a4a313`        | (8,8,1)  | (128,64,1)|   7 |   0 |   0    | boot-time only       |
| `0xeea18ba7`        | (1,1,1)  | (1,1,1)   | 483 | 258 |   0    | histogram auto-exposure (known from P19) |
| `0x67bc2fc3`        | (64,1,1) | (256,1,1) | 158 | 338 | 759    | `0x0000067c00134376` |

`cs_0x2c918c06` is the prime tonemap candidate:

- 8 Exp2 + 8 Log2 — per-channel `pow()` applied to RGB on two
  image fetches (input HDR + history buffer? bloom + HDR?)
- 5 SSBOs: `ssbo_1..ssbo_5` — scene params, TAA history uniforms,
  bloom params, camera motion vectors, exposure state
- 11 sampled images: `cs_img24/32/40/48/56/64/72/80/88/116/124` —
  HDR main, HDR history, bloom mips, motion vectors, depth, etc.
- 3 samplers
- 2 `OpImageWrite` sites — writes its result to the flip buffer
  pair (`0x5000900000` + `0x5000108000` alternating)
- Dispatch shape (16,16,1) = 256 threads × hundreds of tiles
  across the full 1920×1080 surface

### The skip test

`SHADPS4_DC_DISPATCH_SKIP=0x000002c995517e7f` during the race
window. Result (screenshot `2026-04-22_18-16-49.png`):

- Scene is visibly rendering (track, trees, car body all present)
- Colour palette is shifted strongly towards cyan / teal — the
  colour grading this pass applies is gone
- Heavy temporal ghosting and motion blur — TAA history isn't
  being combined with current frame
- **Blackout does not play as usual** — user-reported exact words
- Faint ghost of the "dark car static image" is still visible
  on top, partially transparent — that overlay lives in a
  *separate* pass (still writing to the flip buffer after the
  tonemap compute) and isn't affected by the skip

This is the end of the runtime-probe hunt. `cs_0x2c918c06` is:

1. The full scene→flip-buffer path (skipping it strips every
   per-frame composite operation at once).
2. The tonemap applier (its absence removes the colour-grading
   look).
3. The exposure applier (the blackout's dim visibly stops
   cycling).
4. The TAA resolve (temporal ghosting appears the moment we
   skip it).

### Where the dim input enters this shader

5 SSBO and 11 image inputs; the exposure scalar must be in one of
them. Strongest prior: `ssbo_5` (last binding → typically the
"misc / exposure" grouped parameter buffer) or `cs_img116` (index
116 → single-channel exposure lookup). The histogram auto-exposure
compute `0xeea18ba7` writes a `vec2<f32>` to *its* own `ssbo_3`
which is the emulator-assigned binding 2; at the tonemap's dispatch,
the *same guest buffer* may be bound as a different `ssbo_N` index
via the game's root-signature-equivalent. Without runtime binding
inspection we can't be sure which of `2c918c06`'s SSBOs is the
exposure scalar.

### Planned Phase 23 — patch the tonemap compute

Three options, increasing cost / decreasing risk:

1. **Shotgun constant** — write a minimal GLSL compute with the
   same 19-binding descriptor set layout; body samples the HDR
   input image and writes it directly to the output image without
   tonemap / exposure / TAA. Drops into `shader/patch/`, scene
   will look like raw HDR (probably over-bright) but the **dim
   cycle should stop entirely**. This is the confirmation patch.

2. **Exposure-pin patch** — same structure, but read the
   exposure SSBO and clamp it to a fixed float before the pow()
   chain. Preserves TAA and colour grading, just kills the
   dim-time-varying exposure. This is the "make it playable"
   patch.

3. **SPIR-V surgical edit** — disassemble, find the exact load
   from the exposure SSBO/image, replace it with a constant, re-
   assemble. Preserves everything else. Highest fidelity but
   demands careful SPIR-V bookkeeping across the full 1561-line
   shader.

All three are substantial work. (1) is the fastest to validate the
theory, (2) gives a playable result, (3) gives the cleanest
production-grade patch. Each needs one fresh pipeline-cache wipe
per iteration so `CompileModule` consults the patch dir.

### Commit state at end of Phase 22

No new emulator code in this phase — the existing
`SHADPS4_DC_DISPATCH_SKIP` knob was sufficient to prove the
tonemap compute hypothesis. The skip test is a one-line env var
and leaves no residue when unset.
