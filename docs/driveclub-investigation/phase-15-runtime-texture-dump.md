## Phase 15 — runtime texture dump

`SHADPS4_DC_TEX_DUMP=1` turns on per-texture guest-memory dumping
inside `Rasterizer::BindTextures`. One dump per unique
`info.guest_address`, only while the race-window gate is armed.
Output path:

    $HOME/.local/share/shadPS4/texdump/s<submit>_a<addr>_<w>x<h>_<fmt>_tm<tile>.bin

plus a `[dc-texdump]` log line with the same metadata so the log
acts as a searchable index. Data is raw guest bytes — still tiled
where the game stored them tiled — so de-swizzle happens offline
with whatever follows shadPS4's `tile_manager` logic.

The hit criteria: find a dump whose first-seen submit lands after
a gate re-arm (= race start), whose dimensions are roughly 1920×1080
or a power-of-two close to it, and whose pixel format matches a
photo asset (8-bit RGBA / BC3 / BC7 rather than HDR float). That's
the "dark car static image" candidate. Cross-check: does the same
texture address appear during a bright recovery phase? If it only
fires during dim, we've probably got the overlay.

No mutation yet — this round is observation only.

### Round 15a — first texdump session

Clean Munnar 19:30 run with `SHADPS4_DC_DRAWLOG=1
SHADPS4_DC_TEX_DUMP=1`. Gate arms at submits 383, 1094, 1309, 1453
(race starts around submit 1309). Session ran 2967 submits / ~60 s.
User-observed outcome this run: normal blackout (no "dark car
static image" variant), scene recovered at ≈25 s.

Output:

    /mnt/data/distrobox/gaming/.local/share/shadPS4/texdump/
        1079 .bin files, ~1.7 GB total

All 1079 dumps indexed in the log as `[dc-texdump]` lines with
metadata. The dumps are raw guest bytes — still in GCN tile order
where the game stored the texture tiled. Tile modes seen: tm=0
(DisplayLinearGeneral-ish depth buffers), tm=13 (Thin1DThin),
tm=14 (Thin2DThin). Most asset textures are tm=13.

### Round 15b — viewer pipeline

Two converter scripts added to `tools/`:

- `tools/texdump_to_dds.py` — wraps each dump in a DDS/DX10 header
  so the file is nominally viewable by anything that reads DDS.
  ImageMagick's `identify` recognises the shape but does not
  decode BC7 (only DXT1/DXT5 via the legacy FourCC path).
- `tools/texdump_to_png.py` — decodes BC1/BC3/BC4/BC5/BC7 and
  passes through R8G8B8A8, emits a PNG per dump. Requires
  `texture2ddecoder + Pillow`. A venv at `/tmp/texdec` is set up
  for this on the current host.

Output:

    /mnt/data/distrobox/gaming/.local/share/shadPS4/texdump_png/
        969 .png files

The PNGs are spatially scrambled (the BC block order is the
tiled-memory order, not row-major), so silhouettes are broken.
Dominant colour patches survive, which is enough to triage a
large batch but not to recognise fine detail. Full de-swizzle for
GCN tm=13/tm=14 is a follow-up — shadPS4's own
`video_core/amdgpu/tiling.cpp` plus `tile_manager.*` compute
shader is the reference implementation to port to Python.

### Round 15c — narrowing the visual hypothesis

User feedback after eyeballing the PNGs: the blackout does **not**
read as a full-screen photo. The visual signature is closer to a
heavy camera vignette — darker towards the frame edges, with the
centre slightly less dim — i.e. a *gradient / mask* compositing
over the scene, not a full photo overlay. That rules out the
fullscreen 1920×1080 BC7Srgb candidates I flagged first (those are
likely lobby / menu backgrounds, kept around as resident assets).

Candidates to check for gradient / vignette shape — prioritised by
user:

    s000383_a509b821a00_256x256_Bc1RgbaSrgbBlock_tm13.png
    s000383_a5003831e00_128x128_Bc1RgbaSrgbBlock_tm13.png
    s000490_a50b95d6c00_256x256_Bc1RgbaSrgbBlock_tm13.png
    s001094_a5029e8f100_1024x1024_Bc1RgbaSrgbBlock_tm13.png
    s001453_a50a4b42900_256x256_Bc1RgbaSrgbBlock_tm13.png

Why these five, in the user's reading:

- All are **Bc1 with sRGB + alpha** — the canonical format for UI /
  overlay sprites that need a soft alpha (vignette masks ship this
  way in most engines).
- 256×256 and 128×128 are typical fullscreen-blend mask
  resolutions; 1024×1024 is the higher-quality variant.
- First-seen submits 383 / 490 / 1094 / 1453 straddle the gate
  arms — one pre-race, one mid-load, one at race start — so if
  the blackout mask is resident throughout it should appear here.
- `0x50a4b42900` at submit 1453 is particularly interesting — it
  first binds at the *second* gate arm of the race window, which
  is the same boundary where the Phase 14 UBO state flips. Could
  be the per-race vignette instance.

These are all tiled and will look scrambled under the current
`texdump_to_png.py`; a gradient is actually the *easiest* pattern
to recognise under a tile-scramble because the per-tile means are
preserved. So the PNGs should already be usable for triage —
look for concentric light-to-dark patches.

### Handoff notes

State in-tree at time of handoff (branch `gamma-debug`):

- `src/video_core/renderer_vulkan/vk_rasterizer.cpp` carries, in
  order of addition:
  - `NoteDriveclubDrawlog`                     — `SHADPS4_DC_DRAWLOG=1`
  - `NoteDriveclubUboLog` + `kDcUboLogPipelines` — `SHADPS4_DC_UBOLOG=1`
  - `MaybeClampDriveclubLuminanceUbo`          — `SHADPS4_DC_LUM_CLAMP=<float>`
  - `MaybeRestoreDriveclubExposureUbo`         — `SHADPS4_DC_EXPO_RESTORE=1`
  - `MaybeNukeDriveclubExposureUbo`            — `SHADPS4_DC_UBO_NUKE=1` (currently writes `floats[16]=0.001f`)
  - `MaybeDumpDriveclubTexture`                — `SHADPS4_DC_TEX_DUMP=1`
  - Pre-existing torture path                  — `SHADPS4_DC_TORTURE=1`
- `scripts/run_driveclub_overlay.sh` passes all of these through
  from environment.
- `kDcUboLogPipelines` was extended in Phase 14 Round 5 to cover
  the three single-RT composition pipelines. Safe to keep; they
  are gated by the race-window guard so they cost nothing outside
  the race window.
- `tools/texdump_to_dds.py` and `tools/texdump_to_png.py` are
  standalone — no emulator coupling, safe to leave on tree.

Nothing in this list **fixes** the blackout. The emulator behaves
exactly as baseline when none of the env knobs are set. Upstream
has no dependency on any of these — if the fix ends up living in
a completely different file, all of the above can be reverted in
a single commit.

Current quota status is running low, so the next working session
will be in Codex. Concrete next steps for that session:

1. **Triage the five user-picked Bc1 candidates above.** Open
   their PNGs in a viewer, look for concentric brightness
   gradients. A bigger batch contact-sheet for all `Bc1Rgba*`
   textures under 1024×1024 is also useful —
   ```
   magick montage \
     $(ls /mnt/data/distrobox/gaming/.local/share/shadPS4/texdump_png/*_Bc1RgbaSrgbBlock_*.png \
        | head -60) \
     -geometry 256x256+3+3 -tile 6x /tmp/bc1_batch.png
   ```
   If one of them looks like a centre-bright/edge-dark vignette,
   that's almost certainly the mask.

2. **If a mask candidate emerges**, map its guest address back to
   the pipeline that binds it. The log already has a
   `[dc-texdump] ... addr=0x...` line per dump; cross-reference
   that address in the `[dc-drawlog]` entries of the same submit
   to find which pipeline was drawing when that texture bound.
   Then add that pipeline to `kDcUboLogPipelines` to get its UBOs
   and see if an alpha-scale field is there.

3. **If triage fails**, implement GCN detile in
   `tools/texdump_to_png.py` — port the 2D tile math from
   `src/video_core/amdgpu/tiling.cpp`. A detiled dump will make
   silhouettes legible and should resolve the triage one way or
   the other. That's the one meaningful piece of dev work left
   on this branch if the quick triage does not converge.

4. **Parallel lead: compute-shader output.** All UBO probing so
   far has targeted graphics draws. shadPS4's auto-exposure /
   bloom / luma-integration passes are compute dispatches which
   the drawlog doesn't see. A `SHADPS4_DC_DISPATCHLOG=1` analogue
   hooked into `Rasterizer::Dispatch` would surface any compute
   pass that uses the luma/histogram textures from the torture
   probe (`kTortureSourceAddrs`) as a binding, and let us do the
   same per-submit UBO diff there. This is the cleanest way to
   test whether the dim originates in a compute pass that writes
   an exposure value into a buffer that graphics pipelines then
   consume.

5. **Do not retire any of the three open paths from Phase 14
   above.** Overlay hunt is path 1 and is what we're actively in.
   If overlay hunt fails, path 2 (remaining `depth=no` single-RT
   post-fx UBOs) is the next diagnostic; path 3 (runtime
   camera-fade hook) is the fallback when the diagnostic has gone
   cold on both other paths.
