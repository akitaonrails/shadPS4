## Resume point for next session

Everything below this block is the smallest possible handoff. If
nothing in Phase 8 or Phase 9 gets read above this, these steps
still produce progress.

### State on disk at pause (2026-04-21)

- Track-pack patches applied to the overlay, all known safe:
  `india_road_circuit_01_r.rpk` and `india_road_point_02_n.rpk` in
  `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/data/leveldata/`
  carry the Iteration-1 set (PostFXConfig inline scalars, prerace Fade
  tracks, night→day LUT swap). Patcher source at
  `tools/driveclub_asset_patch/`; rebuild with
  `dotnet build -c Release` inside that directory.
- UI overlay sub-tree: `tmp/driveclub_overlay/CUSA00003/newui/` is a
  real directory with per-file symlinks into the install. All
  Phase-8 panel patches were reverted back to symlinks at the end of
  that phase; everything under `newui/` in the overlay currently
  matches vanilla. `newui/pages/freeplay.ctl` is symlinked back.
- Overlay `eboot.bin` is still a symlink to the real install; no
  binary patch has been written to disk yet.
- shadPS4 source: `SHADPS4_DC_TORTURE` sample-texture null probe is
  still compiled in at
  `src/video_core/renderer_vulkan/vk_rasterizer.cpp` but dormant
  (env var not exported in the launcher). Leave it.
- Investigation logs used during Phase 8 grepping:
  `/mnt/data/distrobox/gaming/.local/share/shadPS4/log/shad_log.txt`.

### One-liner sanity-check commands

```
# Overlay still wired correctly
ls -la /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/newui/panels \
    | head -5
# Should show: directory with symlinks back to real install.

# Patched track rpks present
ls -lh /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/data/leveldata/india_*.rpk

# Build still good
/mnt/data/Projects/shadPS4/build/shadps4 --help >/dev/null && echo OK
```

### What next session starts with

1. Install Ghidra on the Arch host:

   ```
   sudo pacman -S ghidra
   ```

   (Alternative: `rizin` / `rz-ghidra` for a lighter CLI path. Either
   works. Ghidra GUI is faster to navigate xrefs.)
