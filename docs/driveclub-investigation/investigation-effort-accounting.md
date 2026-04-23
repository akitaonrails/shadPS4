## Investigation effort accounting

A catalog of everything produced during the DriveClub v1.28
gamma/blackout investigation on shadPS4, from first fork commit
(2026-04-21) to resolution (2026-04-23). Captured before cleanup so
the scale of what it took to arrive at one config flag
(`readbacks_mode: 2`) is not lost to housekeeping.

### Calendar

- **Duration:** 3 days of active work (2026-04-21 through 2026-04-23).
- **Resolution:** one integer in `custom_configs/CUSA00003.json`.
- **Phases documented:** 33 numbered phases (`phase-01` through
  `phase-31`) plus side threads (asset-sweep 01-09, race-start
  blackout, dump-driven late-branch analysis, code-side camera
  family follow-up, interlude pipeline-cache corruption, handoff
  notes, preamble, priority note, resume point, file-path
  cheatsheet, upstream candidates).

### Git activity (branch `gamma-debug` vs `main`)

- **Fork commits:** 44 (all authored as AkitaOnRails /
  boss@akitaonrails.com).
- **Per-day:** 2 on Apr-20 (branch baseline), 22 on Apr-21,
  18 on Apr-22, 4 on Apr-23 (wrap-up).
- **Net diff vs `main`:** 91 files changed, +15,668 / −1,204
  lines.
- **Raw insertions across all 44 commits:** ~23,470 lines (some
  code was repeatedly rewritten through the investigation).

### Documentation

- **`docs/driveclub-investigation/`:** 54 files, 412 KB, 6,867
  lines of prose.
- **Phase files:** 33 numbered `phase-*.md` plus ~21 supporting
  files (asset sweeps, handoff notes, upstream candidates,
  preamble, README index, resume-point, file-path cheatsheet).
- **Screenshots cross-referenced in prose:** 2 wall-clock-anchored
  state-transition shots (race-start / pitch-black / recalibrated).

### Source-code instrumentation

Heaviest-touched file: `src/video_core/renderer_vulkan/vk_rasterizer.cpp`.

- **Before instrumentation (main):** 1,364 lines.
- **After instrumentation (branch tip):** 4,680 lines.
- **Growth:** +3,316 lines (~3.4× the upstream file), all
  probes / skip-logs / pin lifecycles / recording harness /
  calibrate-at-arm machinery.

Other heavily modified files: `buffer_cache.cpp` (stream-copy
probe), `texture_cache.cpp` (runtime texture dump), `blit_helper.*`
(+191 / +12 lines of helper code), `liverpool.cpp`, `linker.cpp`,
`file_system.cpp`, `libc_internal_io.cpp`, `vk_compute_pipeline.h`,
`vk_pipeline_cache.cpp`, `vk_swapchain.cpp`.

### Runtime probe surface

32 environment variables defined in `scripts/run_driveclub_overlay.sh`
(47 unique `SHADPS4_DC_*` tokens across the tree). Representative
set:

    SHADPS4_DC_DRAWLOG                SHADPS4_DC_UBOLOG
    SHADPS4_DC_TORTURE                SHADPS4_DC_LUM_CLAMP
    SHADPS4_DC_EXPO_RESTORE           SHADPS4_DC_UBO_NUKE
    SHADPS4_DC_TEX_DUMP               SHADPS4_DC_TEX_NUKE_*
    SHADPS4_DC_NUKE_AFTER_ARM         SHADPS4_DC_NUKE_KIND
    SHADPS4_DC_NUKE_MIN_DIM           SHADPS4_DC_NUKE_FORMAT
    SHADPS4_DC_UBO_SMASH              SHADPS4_DC_PC_SMASH
    SHADPS4_DC_DISPATCHLOG            SHADPS4_DC_DRAW_SKIP
    SHADPS4_DC_EXPOSURE_PIN           SHADPS4_DC_TONEMAP_SSBODUMP
    SHADPS4_DC_FRAMEORDER             SHADPS4_DC_UBOSNAPSHOT
    SHADPS4_DC_LIGHT_PIN              SHADPS4_DC_LIGHT_PIN_FILE_224
    SHADPS4_DC_LIGHT_PIN_WINDOW       SHADPS4_DC_LIGHT_PIN_BOOST
    SHADPS4_DC_LOG_SKIPS              SHADPS4_DC_LOG_RO_WRITES
    SHADPS4_DC_PROBE_TONEMAP          SHADPS4_DC_RECORD
    SHADPS4_DC_CALIBRATE_AT_ARM       SHADPS4_DC_CALIBRATE_FILE
    SHADPS4_DC_LOG_STREAMCOPY

### Scripts

Five DriveClub-specific shell scripts in `scripts/`:

- `run_driveclub_live.sh` — direct launch
- `run_driveclub_overlay.sh` — launch with overlay dir + probes
- `run_driveclub.sh` — legacy wrapper
- `start_driveclub_live.sh` / `stop_driveclub_live.sh` — pid-file
  managed live session

### Tooling

Under `tools/`:

- `driveclub_asset_patch/` — C# asset-replacement patcher,
  1.3 MB on disk (including `bin/`/`obj/` build output), 497
  lines of C# source.
- `driveclub_asset_probe/` — C# asset-inspector, 692 KB, 203
  lines of C# source.
- `driveclub_pin_snapshots/` — 92 KB reference captures:
  - `light_ubo_1936.bin` (Munnar calibrated)
  - `sun_ubo_224.bin` (Munnar sun/sky)
  - `canada_calibrated_1936.bin` (Canada calibrated, post-1:30)
  - `tonemap_lum_gamma_1.8.dis.txt` — disassembled tonemap
    with gamma-1.8 luminance-preserve patch
  - `README.md` — usage notes
- `texdump_to_dds.py`, `texdump_to_png.py` — decode helpers for
  runtime texture dumps.

### Runtime artifacts (in-box, `/mnt/data/distrobox/gaming/`)

- **`shad_log.txt`:** 61 MB (single log file, accumulated over
  many runs).
- **`shader/dumps/`:** 57 MB, 5,803 files (shader triples: `.bin`
  SPIR-V + `.spv` + `.asl.txt` + `.irprogram.txt` +
  `.srtprogram.txt` per shader).
- **`Pictures/driveclub-runs/`:** 51 screenshots, 204 KB total,
  the recording-harness output for one Canada calibrated run.
- **`.local/share/shadPS4QtLauncher/versions/`:** 3 custom
  AppImages retained alongside the nightly (fontlib 19 MB,
  gamma-debug 8 KB symlink, main-2026-04-19 22 MB,
  Pre-release/current 22 MB).

### Runtime artifacts (project tree, `/mnt/data/Projects/shadPS4/tmp/`)

Overlay/asset-sweep staging, 314 MB total:

- `driveclub_overlay/` — symlink mirror of the installed game
  with patched `.rpk`s dropped in, 10 MB.
- `driveclub_overlay_patched_backup/` — alternate overlay.
- Asset-sweep test RPKs: `globaldata.blackout-test.rpk` (11 MB),
  `globaldata.fadeonly-test.rpk` (11 MB),
  `globaldata.rtt-only.rpk` (11 MB),
  `india_landscape_gui.blackout-test.rpk` (5.4 MB),
  `india_road_point_02_n.blackout-test.rpk` (4.2 MB).
- `driveclub_bisect/` — bisect workspace.
- `globaldata_probe/`, `global_probe/`, `munnar_probe/` (332 KB),
  `india_landscape_gui_probe/` — probe directories.
- `blackout_video_analysis/`, `_run2/`, `_vanilla/` — captured-video
  analysis workspaces.
- `dc-logs/`, `dc-patches/`, `eboot_extract/` — working areas.
- Standalone dumps: `278a20.bin`, `2792f0.bin`, `27a280.bin`,
  `293090.bin`, `e.bin`.
- `driveclub-live.pid` — live-session pidfile.

### Build output

`/mnt/data/Projects/shadPS4/build/`: 186 MB. Normal for a clang
release build of the emulator core.

### Conversation transcript

`~/.claude/projects/-mnt-data-Projects-shadPS4/8fa4b2f0-*.jsonl`:
36 MB, 13,515 JSONL records (one per Claude / tool / user turn).
This is the full verbatim trace of the agent sessions that drove
the work.

### Headline ratios

- Lines of investigation prose per line of eventual fix: **6,867
  markdown lines : 1 integer**.
- Renderer-file growth vs upstream: **3.4×** (`vk_rasterizer.cpp`).
- Probe env vars defined vs probes actually required to find the
  bug: **32 defined : 1 required** (`SHADPS4_DC_LOG_STREAMCOPY`,
  added in phase 30 to rule out the ObtainBuffer first-copy race;
  even then, the fix came from toggling a setting, not from the
  probe data).
- Git commits vs eventual behavioural change: **44 commits : 1
  config integer**.
- Disk footprint of investigation artifacts (excluding build/ and
  jsonl transcript): ~**427 MB** (57 MB shader dumps + 61 MB logs
  + 314 MB overlay/probe tmp + 92 KB snapshots + ~200 KB
  screenshots + 5.4 KB custom-build metadata not counted as MBs).

### What the effort bought that the one-line fix didn't

Several things are worth keeping even though the fix itself didn't
need them:

1. **Phase-28 `IsComputeImageCopy` format-compat fix** (commit
   `2b9525ab`) — a real upstream-worthy bug in the renderer,
   surfaced only because we were reading every silent-skip path.
2. **Libc-internal I/O fixes** — several sceKernel / libc_internal
   edge cases that affected v1.28 content access (phase-03).
3. **Runtime texture dump + shader dump infrastructure** — reusable
   diagnostic tooling that survives this investigation.
4. **Consolidated recording harness** (`SHADPS4_DC_RECORD`) —
   cross-correlates UBO state with wall-clock screenshots;
   general-purpose.
5. **Full PS4→shadPS4 DriveClub investigation narrative** — 33
   phases of prose that document blind alleys (the 25-phase chase
   down a non-existent shader bug) as much as findings, and that
   should make the next similar investigation shorter.

### What we spent that we shouldn't have

1. **25 phases chasing a shader / UBO write-side bug** that never
   existed. The symptom ("progressive darkening") looked like a
   broken tonemap and dominated our priors for a week. Lesson
   filed in `feedback_assume_emulation_bug.md` and in phase-31's
   "why we missed this" section.
2. **Overlay/asset sweep (~12 RPK substitutions)** before pivoting
   to the renderer. Phase-12 closed this line decisively but only
   after the fact.
3. **Custom build rebuilds** stored as separate AppImage entries
   (`fontlib`, `gamma-debug`, `main-2026-04-19`) in the QtLauncher
   `versions/` dir. None of these produced the fix; the stock
   nightly is sufficient.
4. **SPIR-V tonemap patches** (`tonemap_lum_gamma_1.8.dis.txt`
   and earlier variants) — worked as a band-aid but treated the
   wrong layer. Retained only as a reference for the shape of the
   tonemap compute.

### Cleanup scope

With the fix in hand, most of the above can be reclaimed:

- Box-side logs (`shad_log.txt` 61 MB) — can be truncated or
  rotated; only the last run's stream-copy data informed phase 31.
- Shader dumps (57 MB, 5803 files) — no longer needed; disable
  `dumpShaders` in the live config, delete the directory.
- Custom AppImage builds under `versions/` except nightly — 41 MB.
- `tmp/` asset-sweep workspace — 314 MB, all exploratory.
- `driveclub_asset_patch/{bin,obj}/` and
  `driveclub_asset_probe/{bin,obj}/` — C# build output,
  regenerable.

Keep:
- `docs/driveclub-investigation/` (all 54 files, 412 KB).
- `tools/driveclub_pin_snapshots/` (92 KB, reference captures).
- `tools/driveclub_asset_patch/` and `_asset_probe/` C# sources
  (the `Program.cs` files only).
- `tools/texdump_to_*.py` helpers.
- `scripts/run_driveclub_*.sh` + `start/stop_driveclub_live.sh`.
- The 44 git commits on `gamma-debug` — they are the audit trail.
- Investigation screenshots in `Pictures/driveclub-runs/` — 204 KB,
  cheap, anchor the phase-29 timing claims.
- The `.jsonl` conversation transcript (36 MB) — immutable history.

Rough reclaimable footprint: **~470 MB** (shader dumps 57 MB +
truncatable log 61 MB + custom AppImages 41 MB + tmp workspace
314 MB). Remaining investigation footprint after cleanup: ~500 KB
of prose + ~1 MB of C# source + 92 KB of snapshots.
