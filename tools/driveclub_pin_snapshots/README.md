# Driveclub fade-UBO pin snapshots

These two `.bin` files are captured post-recovery "bright" states of
two uniform buffers Driveclub uses to carry its race-start fade
animation. They were extracted from Munnar India 19:30 the first time
the scene lifted out of the scripted blackout fade.

- `light_ubo_1936.bin` — 1936-byte lighting buffer bound at many
  `(pipeline, cb_slot)` combos across ~50 scene material pipelines.
  Carries a bundle of light intensities the game multiplies by a
  blackout fade scalar. Offsets `[38]`, `[48]`, `[50]`, `[61]` are
  the primary fade slots; the full buffer is pinned by
  `SHADPS4_DC_LIGHT_PIN_FILE`.
- `sun_ubo_224.bin` — 224-byte sun/key-light transform. Carries a
  4-vector at `[48..51]` that drops 97% further at the second-stage
  fade (the one that kicks in ~30s into a pinned race). Pinned by
  `SHADPS4_DC_LIGHT_PIN_FILE_224`.

Usage:

    SHADPS4_DC_LIGHT_PIN=1 \
    SHADPS4_DC_LIGHT_PIN_FILE=tools/driveclub_pin_snapshots/light_ubo_1936.bin \
    SHADPS4_DC_LIGHT_PIN_FILE_224=tools/driveclub_pin_snapshots/sun_ubo_224.bin \
    SHADPS4_DC_NUKE_AFTER_ARM=3 \
      scripts/run_driveclub_overlay.sh

Each pin is gated to the race-window arm and only fires when the
buffer's content signature matches — so a non-Driveclub title or a
non-matching buffer won't be touched.

Content is extracted-at-runtime float values, not game assets. Values
are Munnar-19:30-specific. Cross-track validation in progress.
