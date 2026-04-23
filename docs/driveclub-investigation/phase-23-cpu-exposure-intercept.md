## Phase 23 — CPU-side exposure intercept, cross-track

### The intercept

`MaybePinDriveclubExposure(base, size, cb_idx)` fires in
`Rasterizer::BindBuffers` right before `buffer_cache.ObtainBuffer`,
gated on `SHADPS4_DC_EXPOSURE_PIN=1`. When the current draw is the
tonemap compute (`0x000002c995517e7f`, `DrawKind::Compute`) and
the buffer being bound is `ssbo_5` (cb index 4), we overwrite 4
bytes at `vsharp.base_address + 64` (the `[buf4_dword_off + 16]`
offset the shader reads) with `SHADPS4_DC_EXPOSURE_PIN_VALUE` and
invalidate the buffer_cache range so Vulkan actually uploads the
dirtied bytes.

Initial semantics: hard pin — overwrite always. Final semantics:
clamp-min — only overwrite when the game's natural value is
*below* the threshold, to avoid over-brightening scenes that
don't need it.

### What the `[dc-exppin]` log shows

First-frame capture of the natural value across ~30 samples on
Munnar India 19:30 (dim track):

    previous=0.008974 → 0.0072926 → 0.005926 → 0.004815 → 0.003913
              → 0.003180 → 0.002584 → 0.002100 → 0.001706 → 0.001386

Clear exponential decay. The game writes the animation curve into
a ring of ssbo buffers; addresses alternate (`0x4000ba0450`,
`0x4000d40450`, `0x4000e00450`, …) because the engine rotates
through fresh slots. `previous=1.0` entries are the fresh slots
that haven't been animated yet; non-1.0 entries are the slots
whose frame has already written the fade value.

This is the scripted race-start fade captured in the act. It is
not an auto-exposure warm-up curve — it's the engine's own
animation dragging exposure down.

### Cross-track problem

- **India / Munnar 19:30**: natural value dives to 10⁻³;
  clamp-min with threshold 3.0 gives a playable bright race.
- **Canada & Japan 19:30**: scene is already bright but the
  natural exposure values there are also sub-1.0. Clamp-min
  with threshold 3.0 blows those tracks out.
- **Threshold 0.5**: India gets only marginally brighter (the
  dim dip goes well below 0.5 so it's still too dim), Canada /
  Japan are still over-brightened.

The natural-exposure numeric range *overlaps* between a dim
track's "normal" and a bright track's "normal". A single scalar
threshold can't distinguish them.

### Decision

For now the exposure pin is a per-track knob: user flips it on
for known-dim tracks, leaves it off for naturally-bright tracks.

### Vanilla state confirmed

- No shader patches in `~/.local/share/shadPS4/shader/patch/`.
- `GPU.dump_shaders = false` in CUSA00003.json.
- `GPU.patch_shaders = true` is the shadPS4 default — harmless
  when the patch dir is empty.
- Pipeline cache restored (snapshots stashed under
  `cache/CUSA00003.snapshot-phase*`).
- All `SHADPS4_DC_*` probes remain env-var-gated in the committed
  binary — zero emulator behaviour change with no env vars set.

### Usage cheatsheet

    # Dim-biased tracks (India Munnar 19:30 etc.)
    SHADPS4_DC_EXPOSURE_PIN=1 SHADPS4_DC_EXPOSURE_PIN_VALUE=3.0 \
      SHADPS4_DC_NUKE_AFTER_ARM=3 scripts/run_driveclub_overlay.sh

    # Naturally-bright tracks: no env vars, baseline shadPS4

Tune `SHADPS4_DC_EXPOSURE_PIN_VALUE` per track (0.5..5.0 range).
