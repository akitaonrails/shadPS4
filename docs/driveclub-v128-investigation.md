<!--
SPDX-FileCopyrightText: 2026 Fabio Akita
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Driveclub on shadPS4: v1.28 + gamma investigation

Branch-local progress log. Lives on `gamma-debug`. Not destined for upstream as-is;
trimmed/split excerpts may eventually feed upstream PRs (see "Upstream candidates"
at the end).

Sibling deploy notes in `~/Projects/distrobox-gaming/docs/driveclub-shadps4.md`
are still the operational runbook for the gaming distrobox. This doc only captures
what changed while working on this branch.

## Target

- **Game**: DRIVECLUB™, CUSA00003, stock v1.28 disc image, no premium DLC.
- **Host**: Arch Linux, Clang 22.1.3, CMake 4.3, Vulkan 1.4.341, SDL3 3.4.4.
- **GPU**: NVIDIA RTX 5090, driver 595.58.3.0, Wayland/Hyprland.
- **CPU**: Ryzen 9 7950X3D.
- **Runtime**: gaming distrobox, `~/.local/share/shadPS4/` as the data root,
  QtLauncher as the shell. Upstream nightly `main-2026-04-19` was the baseline.

## Fork layout

- **`akitaonrails/shadPS4`** (this repo) — `origin`, fork of `shadps4-emu/shadPS4`.
  Remotes: `origin` = my fork (push), `upstream` = canonical (read-only).
  `main` tracks `upstream/main` so `git pull` on `main` is always a fast-forward.
  Branch-local experiments live on feature branches such as `gamma-debug`.
- **`akitaonrails/DriveClubFS`** (`~/Projects/DriveClubFS`) — fork of
  `Nenkai/DriveClubFS`. Upstream went dormant after tag `1.1.0` (June 2025) with
  only README churn since; my fork is the de-facto maintained copy. No code
  changes applied yet — see v1.28 section below for why the "1.28 crashes" claim
  from the distrobox docs did not reproduce.

## Verified build recipe

```sh
git submodule update --init --recursive --jobs 8
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel "$(nproc)"
```

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is mandatory on this host — CMake 4.3 dropped
compatibility with pre-3.5 `cmake_minimum_required`, and a few submodules
(miniz, etc.) still declare older minimums.

Output: `build/shadps4`, ~44 MB, PIE ELF, clang 22.

## Phase 1 — shader compile stalls

### Symptom

Driveclub menus animated at crawl speed on first entry, then snapped to fast
once past that moment. Pattern repeats on next cold launch.

### Root cause

shadPS4's persistent pipeline cache was not enabled by default. A single session
compiled ~527 shaders + ~354 pipelines from scratch; every new UI pipeline
stalled the GPU comm thread during its first use. Nothing cached to disk meant
the same cost every launch.

### Fix (config-only, no code)

Set `Vulkan.pipeline_cache_enabled = true` in
`~/.local/share/shadPS4/config.json`. Left `pipeline_cache_archived` at `false`
(zip-compressed cache) — we want fast load over small disk footprint.

First launch after enabling still has to compile everything. Second launch
onwards replays the cached pipelines and menus should be snappy.

Backup of the original config preserved as
`config.json.bak-before-pipeline-cache` next to the live file.

## Phase 2 — gamma / dim image (in flight)

Main complaint: Driveclub renders fine mechanically but the image is markedly
dim compared to any reference footage. HUD looks correct, scene looks
desaturated and crushed. The distrobox docs had already ruled out display-side
fixes (vkBasalt unstable on RTX 5090 + Vulkan 1.4, Hyprland `screen_shader`
has its own pitfalls, nvidia-settings Digital Vibrance is X11-only).

### Code landscape

The SDR present pipeline lives in `src/video_core/renderer_vulkan/`:

- `vk_swapchain.cpp` picks the swapchain format. Non-HDR path uses
  `R8G8B8A8Unorm` / `B8G8R8A8Unorm` in `SrgbNonlinear` colorspace — **driver
  does not auto-encode**, the shader is responsible for the sRGB OETF.
- `host_passes/pp_pass.*` runs `host_shaders/post_process.frag` on the
  video-out image before present. The fragment shader applies a piecewise
  sRGB-shaped encode via a `pp.gamma` push-constant and a `pp.hdr` flag.
- `vk_presenter.cpp::GetFrameViewFormat` (line ~652) maps guest pixel formats
  to Vulkan formats. Both `A2R10G10B10` and `A2R10G10B10Bt2020Pq` fall through
  to `eA2R10G10B10UnormPack32` — the PQ/HDR variant gets silently collapsed to
  linear Unorm, which I suspect is half the problem.
- `videoout::sceVideoOutAdjustColor` writes the game's requested gamma into
  `presenter.pp_settings.gamma`. Driveclub doesn't call this (it expects HDR
  via its PS4 tonemap instead), so the post-process is running at
  `gamma = 1.0` over whatever the game wrote.

### Upstream history reviewed

PRs worth knowing about:

- `#2381` (Feb 2025) — original HDR path: `A2B10G10R10Unorm` swapchain in
  `Hdr10St2084EXT` colorspace when `allowHDR` + game declares HDR.
- `#2485` / `#2619` — HDR settings UI plumbing.
- `#3690` (Oct 2025) — moved HDR swapchain reconfig onto the present thread
  (crash fix).
- `#1559` (Nov 2024) — "respect game brightness settings", eliminated an old
  sRGB hack. Suspected in `#1725` (Bloodborne too bright regression).
- `#1756` (Dec 2024) — reworked the gamma encode curve into the current
  piecewise function.
- `#1805` (Dec 2024, closed unmerged) — proposed a Graphics-tab gamma slider.
- `#3420` (Aug 2025) — per-shader degamma when sampler sets `force_degamma`.
- `#4087` (Feb 2026, open) — feature request for full/limited-range black
  level, exactly the symptom shape.

There is no open or merged fix specifically for Driveclub-style "SDR tonemap
makes scene dim". I'm currently treating this as a gap to fill.

### Branch changes (`gamma-debug`)

Minimal instrumentation, guarded by an env var so the binary is safe to ship
side-by-side with the nightly:

- `src/video_core/renderer_vulkan/vk_presenter.cpp`
  - `GetFrameViewFormat` logs each distinct guest `PixelFormat → vk::Format`
    mapping once per run (`[gamma-dbg]` tag).
  - Adds `ReadPpGammaOverride()` which reads `SHADPS4_PP_GAMMA_OVERRIDE`
    (valid range 0.1..2.0). Value is cached on `Presenter` as
    `pp_gamma_override`; when set, it's re-applied in `PrepareFrame` just
    before `pp_pass.Render(...)` so it wins over the game's own
    `sceVideoOutAdjustColor` and the devtools slider.
- `src/video_core/renderer_vulkan/vk_presenter.h`
  - New `pp_gamma_override` member on `Presenter` (default `-1.0f` = unset).
- `src/video_core/renderer_vulkan/vk_swapchain.cpp`
  - `Create()` now logs chosen format, colorspace, `supports_hdr`,
    `needs_hdr` at `Info` level.

### Deploy harness

`/mnt/data/distrobox/gaming/bin/shadps4-driveclub-gamma-debug` — isolated
wrapper. Sets `HOME=/mnt/data/distrobox/gaming/.local/share/shadPS4-gamma-dbg/`
so the binary writes into a completely separate data tree, seeds the
per-game JSON on first run, exports SDL HIDAPI env vars for controller
detection, and launches `CUSA00003/eboot.bin` directly. The production
QtLauncher path is untouched. Edit the `SHADPS4_PP_GAMMA_OVERRIDE` line at
the top to tune.

There is also now a **Manager-visible version entry** at
`~/.local/share/shadPS4QtLauncher/versions/gamma-debug-2026-04-20/` with a
`Shadps4-sdl.AppImage` shell wrapper that sets the same env vars (SDL
HIDAPI + `SHADPS4_PP_GAMMA_OVERRIDE=0.7` by default) and `exec`s the
instrumented binary (`Shadps4-sdl.real`). Registered in `versions.json` as
"Gamma Debug (local)". It runs against the **same HOME** as the nightly —
same v1.28 install, same per-game config, same shader cache — so switching
in the Manager's version dropdown is a true A/B. The standalone isolated
wrapper at `/mnt/data/distrobox/gaming/bin/shadps4-driveclub-gamma-debug`
still exists for fully-sandboxed testing if we ever need it.

### Findings from a live Driveclub run

Captured via the gamma-debug Manager entry on 2026-04-20:

```
[gamma-dbg] Swapchain created: format=B8G8R8A8Unorm colorSpace=SrgbNonlinear
            supports_hdr=false needs_hdr=false
[gamma-dbg] Video-out PixelFormat 0x80000000 -> vk::Format B8G8R8A8Srgb
```

So Driveclub on v1.28 uses plain 8-bit sRGB (`A8R8G8B8Srgb`), not the 10-bit
HDR-capable format I had been worrying about. The HDR-PQ fallthrough in
`GetFrameViewFormat` isn't relevant here (would matter for other titles —
keep that fix in our back pocket).

The dimness is therefore **not** a format-mismatch bug. The game is just
writing low-luminance values to an 8-bit sRGB target; hardware sRGB
decode on sample returns faithful linear values; the SDR re-encode is
mathematically correct; and the resulting output is simply dim because
the game's internal tonemap was tuned for HDR display brightness.

### Fix shape in this branch

Replaced the identity sRGB encode at the SDR present path with a
three-knob pipeline, all driven by push-constants (no per-frame CPU
cost). Source lives in `src/video_core/host_shaders/post_process.frag`
and plumbed through `pp_pass.h::Settings` / `vk_presenter.cpp`:

```
exposed    = linear_color * pp.exposure              // brighten
tonemapped = aces_per_channel(exposed)               // 0 = default, compat
           | aces_luma_preserving(exposed)           // 1 = preserve hue
final      = gamma_encode(tonemapped)                // existing sRGB curve
```

- **Exposure** (`pp.exposure`, default `1.0f`): pure linear multiplier.
  Rescues under-bright scenes. Values up to 10x are safe because ACES
  rolls off the top.
- **Tone-map mode** (`pp.tonemap_mode`, default `0`):
  - `0` = per-channel ACES (Narkowicz approximation). Simple, standard,
    slightly desaturating at the shoulder.
  - `1` = luma-preserving ACES. Tone-maps `dot(rgb, rec709_luma)` and
    scales chroma proportionally, so hue is preserved even under heavy
    exposure boost. Fixes the "bright but grey" look per-channel ACES
    can produce.
- **Gamma** (`pp.gamma`, default `1.0f`): existing sRGB-shape curve,
  clamp widened on my branch to `0.01..2.0` (spec is `0.1..2.0`).

Driver env vars on the debug wrapper:
`SHADPS4_PP_GAMMA_OVERRIDE`, `SHADPS4_PP_EXPOSURE`, `SHADPS4_PP_TONEMAP`
(`perchannel`/`0` or `luma`/`1`). Applied at `Presenter` construction and
re-asserted each frame before `pp_pass.Render` so they win over
`sceVideoOutAdjustColor` and the devtools slider.

### What worked for Driveclub

Final working combination confirmed in-game on 2026-04-20:

```
SHADPS4_PP_GAMMA_OVERRIDE=0.7
SHADPS4_PP_EXPOSURE=3.0
SHADPS4_PP_TONEMAP=luma
```

Scene content (track, cars, sky, trees) lifted from near-black to
correctly-readable brightness with preserved color. Highlights in the
game's pause menu saturate slightly but are acceptable for now — the
ACES shoulder keeps them from pure white, which the earlier hard-clamp
variant could not.

### Status

**Resolved for Driveclub** — by building a substantial tonemap toolkit
and then realising Driveclub doesn't need it.

The pipeline now supports gamma/exposure/ACES/auto-exposure/dither as
opt-in SDR post-process features, but the correct default for Driveclub
(and probably a lot of stock SDR titles) is **bypass**: the game writes
already-correct SDR, we just sRGB-encode and present.

### Lesson: when to use the tonemap chain, and when not to

A chunk of this investigation was spent fighting a symptom — "the scene
looks dim" — with progressively more aggressive corrections (manual
exposure, per-channel ACES, luma-preserving ACES, auto-exposure with
histogram analysis, shadow-lift curves, peak-aware clamping, mean-aware
headroom, hysteresis adaptation). Each fixed one case by breaking
another: menus would blow out when dark scenes got lifted, or dim
scenes would stay dim when menus were tamed.

The breakthrough came from adding a `SHADPS4_PP_BYPASS=1` env var that
skipped the entire tonemap/exposure chain and just sRGB-encoded the
raw video-out framebuffer. **With bypass on, Driveclub looked correct
in every scene**: menus bright and vivid, races properly-exposed,
night atmospheric, dawn moody. The only remaining dim pocket is a
5-second in-game fade at race start that is **the game's own rendering
effect** — a cinematic reveal where Driveclub itself writes low values
to its framebuffer for a few seconds before transitioning to full
brightness. Nothing we do in post-process affects that; it's
gameplay-side VFX.

So the right reading of the whole saga is:

  - shadPS4 without any SDR post-process produces **correct** output
    for games that write already-correct SDR values to their framebuffer.
    Driveclub is one of those.
  - The post-process machinery (ACES tonemap, exposure, gamma, auto)
    is only needed for games whose output is truly HDR-intent and
    needs to be compressed into SDR. Those exist — Bloodborne,
    God of War, etc. — but the treatment should be opt-in per game,
    not a default that mangles games like Driveclub.
  - "The scene looks dim" as a bug report has two very different root
    causes: (a) emulator post-process is crushing an already-correct
    image, or (b) the game legitimately writes dim values intending
    HDR amplification. A diagnostic bypass tells you which without
    having to guess.

### Final configuration in `gamma-debug`

Wrapper defaults applied by the QtLauncher version entry:

```
SHADPS4_PP_BYPASS=1          # shader does sRGB-encode only
SHADPS4_PP_AUTO_EXPOSURE=0   # compute dispatch skipped
SHADPS4_PP_EXPOSURE=1.0      # neutral (only takes effect if bypass=0)
SHADPS4_PP_TONEMAP=luma      # also gated by bypass
# SHADPS4_PP_GAMMA_OVERRIDE unset — shader uses standard sRGB
```

The auto-exposure compute pass, ACES shaders, dither pattern, and the
manual gamma/exposure/tonemap push-constants all stay in the codebase
as opt-in features. Default behaviour at the shader level remains
canonical SDR: `linear × 1.0 → ACES → sRGB → dither` if bypass is off;
with the wrapper having bypass on, that whole chain is skipped.

### Upstream candidates

Before anything can be upstreamed:

1. The `auto_exposure_pass` + its compute shader is a reasonable
   feature in isolation — could go upstream as opt-in behind a config
   flag, not a default. Needs A/B testing against multiple titles
   before becoming a default.
2. The ACES tonemap and luma-preserving variant are useful for
   HDR-intent games. Upstream would likely want them as a per-game
   JSON field (`Gpu.tonemap_mode`), not an env var. Drop the
   `[gamma-dbg]` log tags before PR.
3. The Bayer dither is a universal win on 8-bit swapchains and could
   go upstream unconditionally.
4. Bypass mode is either a keep-out (it's weird to ship an escape
   hatch as a feature) or it becomes the *default* and the tonemap
   chain becomes opt-in — either way it's an upstream conversation
   that can happen after the rest of the features stabilise.

## Phase 3 — v1.28 content access

### Myth busted

The distrobox docs (`docs/driveclub-shadps4.md`) said
`DriveClubFS 1.1.0 crashes at file 12/8018 with EndOfStreamException` on a
v1.28-merged tree. That claim does **not** reproduce against current data.

What I actually found:

1. **Upstream DriveClubFS has no 1.28 fix and none in flight.**
   Master is 4 commits ahead of tag `1.1.0`, all README-only. Zero PRs ever
   opened. One closed issue (`#1` "DriveClub VR", Nov 2025) describes the
   same `EndOfStreamException` shape but was closed without any engagement.
2. **Only other fork** (`illusionyy/DriveClubFS`, 2023) is 0 ahead / 7 behind
   upstream — a stale snapshot, no divergent code.
3. **Running DriveClubFS unpack-all on a properly-merged v1.28 tree succeeded
   cleanly** — 8018/8018 files extracted, 47 GB, exit 0, zero errors or
   skips. Either the earlier attempt was run against a badly-staged tree, or
   the crash was transient and has since been resolved silently.

No fork patch needed for 1.28. The fork at `~/Projects/DriveClubFS` still
exists for future stewardship but there's nothing to apply today.

### Working v1.28 install recipe

Paths below are on the gaming distrobox host mount. Commands prefixed
`distrobox enter gaming -- ...` where the NAS mount differs from the sandbox.

```sh
# Starting point: CUSA00003/ is a working v1.00 install (DriveClubFS-unpacked)
# Goal: swap in v1.28 content, preserve v1.00 as backup.

cd /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4

# 1. Extract v1.28 PKG to a staging dir. ~19 GB PKG, ~20 GB output.
#    Contains eboot.bin (v1.28), game.ndx (v1.28, 419K), ~41 high-index .dat
#    files, sce_module (libc.prx + libSceFios2.prx), sce_companion_httpd,
#    .prx/.plt resources, and a partial sce_sys/about/.
/mnt/data/distrobox/gaming/tools/ShadPKG/build-cli/shadpkg extract \
    -i /mnt/terachad/.../Driveclub.v1.28.PATCH.REPACK.PS4-GCMR.pkg \
    -o /mnt/terachad/.../CUSA00003-v128-test

# 2. Symlink the v1.00 .dat files into the v1.28 tree so DriveClubFS sees
#    the full set (the v1.28 index references both low- and high-numbered
#    .dat files).
cd CUSA00003-v128-test
for f in ../CUSA00003/game*.dat; do ln -s "$f" "$(basename "$f")"; done

# 3. Run DriveClubFS. v1.28 index reports 8018 entries; output ~47 GB loose.
dotnet ~/Projects/DriveClubFS/DriveClubFS/bin/Release/net10.0/DriveClubFS.dll \
    unpack-all \
    -i /mnt/terachad/.../CUSA00003-v128-test \
    -o /mnt/terachad/.../CUSA00003-v128-test-out \
    --skip-verifying-checksum

# 4. Swap in place, backup preserved.
cd /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4
mv CUSA00003 CUSA00003.v100-working-backup   # keep as rollback
mkdir CUSA00003
mv CUSA00003-v128-test-out/* CUSA00003/       # 8018 loose files (47 GB)
rmdir CUSA00003-v128-test-out

# 5. Copy v1.28 metadata on top (not the .dat files — loose files win).
cd CUSA00003-v128-test
for f in eboot.bin game.ndx *.prx *.plt; do
  [ -e "$f" ] && cp -a "$f" ../CUSA00003/
done
for d in sce_sys sce_module sce_companion_httpd; do
  [ -d "$d" ] && cp -a "$d" ../CUSA00003/
done

# 6. CRITICAL: restore base sce_sys files from v1.00 backup (see below).
cd ../CUSA00003/sce_sys
for f in param.sfo disc_info.dat keystone; do
  [ ! -f "$f" ] && cp -a "../../CUSA00003.v100-working-backup/sce_sys/$f" .
done

# 7. Re-enable the 60 fps patch (its offsets target v1.28 eboot).
mv ~/.local/share/shadPS4/patches/Driveclub.xml.disabled-for-v1.0 \
   ~/.local/share/shadPS4/patches/Driveclub.xml
```

Keep `CUSA00003-v128-test/` around — it's the re-runnable DriveClubFS
input if the loose-file tree ever needs regeneration. Leave
`CUSA00003.v100-working-backup/` in place for one-command rollback.

### The `sce_sys/param.sfo` gotcha

v1.28 is a **CUMULATIVE_PATCH** PKG. ShadPKG's extraction of that PKG gives
you `sce_sys/about/right.sprx` and not much else — base metadata (param.sfo,
disc_info.dat, keystone) is NOT re-shipped by cumulative patches because a
real PS4 already has them from the base install.

On our side the only copy of those files lives in the v1.00 backup tree. If
you naively `cp -a` the v1.28 `sce_sys/` over the new install, you end up
with a folder that has `about/right.sprx` but no `param.sfo` at all. shadPS4
loads the eboot, applies the Driveclub.xml patch, boots the game, but the
game's internal "am I running v1.28? what content is unlocked?" checks silently
fall back to "not yet released → download required" because there's no APP_VER
to compare against. Hence "all content locked, asking to download", even
though the v1.28 eboot is the one actually running.

The v1.00 backup's `param.sfo` is fine to restore as-is: it already has both
`APP_VER = 01.28` and `VERSION = 01.00` (the base VERSION stays at 01.00
forever, APP_VER reflects whatever patch is installed on top).

`npbind.dat` is absent in both our v1.00 backup and the v1.28 patch output.
That's only needed for premium DLC entitlement verification; base game and
free-update content don't require it.

### After the fix

With the restored `param.sfo` the user confirmed in-game that:
- Previously greyed-out / "download required" content is now accessible.
- New v1.28 content is visible in menus.

Remaining concerns carried into next phase:

- **Slowness during gameplay** — see Phase 4.
- **Dim image** — unchanged by the v1.28 swap, still owned by Phase 2.

## Priority note

**2026-04-20, later:** Phase 2 (gamma) is resolved with the ACES +
luma-preserving tonemap. Phase 4 (slowness + night-scene blackout) is
also resolved — it turned out to be two separate problems conflated
under "feels slow", see below.

## Phase 4 — "slowness" (resolved, two causes)

The subjective "game is running like molasses" had two separate roots.
Pinned them one at a time.

### Evidence collected from a real v1.28 session

Log at `~/.local/share/shadPS4/log/shad_log.txt`:

- **Pipeline cache enabled** ✓. First v1.28 session compiled 864 shaders
  + 590 pipelines (vs ~881 total for v1.00). The extra ~330 shader
  modules are v1.28-specific content — compiled fresh because the cache
  was empty on first v1.28 launch. `Cache dumped` on shutdown confirms
  persistence, so this cost is one-shot. Subsequent launches replay
  instantly.
- **1325 occurrences** of
  `texture_cache.cpp ResolveDepthOverlap: Unimplemented depth overlap copy`
  clustered in the in-game portion. Smoking gun, but needed to know
  which shape combinations were hitting.

### Root cause #1 — 60 fps patch causes slow-motion smooth playback

The user described the feel as "smooth but in slow motion" — frames
arrive at a steady rate, cars accelerate slowly, lap time in 1 minute
shows what should be half a lap, AI cars overtake normally at 25 km/h
while the player's gauge reads 25 km/h.

This is the classic shape of "render rate and logic timestep out of
sync". The `Driveclub.xml` 60 fps patch I re-enabled when we swapped
to v1.28 was designed for PS4 Pro's 60 fps render pipeline — and from
everything I've read these community eboot patches rewrite the render
rate without touching the internal fixed-timestep game-logic rate.
Real PS4 Pro's engine handles the mismatch internally; shadPS4 doesn't.

**Fix:** disable the patch:

```sh
mv ~/.local/share/shadPS4/patches/Driveclub.xml \
   ~/.local/share/shadPS4/patches/Driveclub.xml.disabled-for-v1.0
```

Game reverts to 30 fps native cap; everything moves at the correct wall-
clock speed. Confirmed by the user with a clean run. Tradeoff is 30 fps
instead of 60, which matches stock PS4 behavior.

Long-term fix options (deferred):
- Find/write a Driveclub patch that *also* scales the internal timestep.
- Implement frame interpolation host-side (much larger scope, out of
  scope for this branch).

### Root cause #2 — ResolveDepthOverlap gap breaks night scenes

Separate problem. After disabling the 60 fps patch the game runs at
correct speed, but on a **night track the scene is near-pure black** —
only HUD visible, no headlight cones, no track surface, no cars. Day
scenes looked dim (Phase 2 handled that) but were mostly visible.

Instrumenting the `else` branch in
`TextureCache::ResolveDepthOverlap` (caching each unique shape
combination and logging once per run) revealed a **single offender**
hit over a thousand times per session:

```
cache(fmt=D32Sfloat     depth=true  stencil=false samples=4)
  -> new(fmt=R32G32B32A32Sfloat depth=false stencil=false samples=1)
  binding=1 (Texture)
```

Driveclub's forward+ / screen-space lighting renders geometry into a
**4x MSAA depth target**, then binds that depth aspect as a **1-sample
R32G32B32A32Sfloat sampler2D** for the lighting accumulation pass,
SSAO, soft particle edges, etc. shadPS4's resolver didn't have a path
for this combination, so the `else` branch just `FreeImage`-d the
cached MSAA depth and returned an uninitialised 1-sample color image.
Every depth-based effect downstream was reading garbage.

**Why day was "only dim" and night was "pure black":** day scenes have
strong ambient + sun lighting baked into the material pass; the broken
depth-sampled passes just subtly wash out details we papered over
with exposure. Night scenes rely entirely on screen-space volumetric
headlights keyed off the depth buffer — no depth → no lights → no
scene.

### Fix shape in this branch

Mirror of the existing `BlitHelper::ReinterpretColorAsMsDepth` path
(which already handles the opposite direction, 1x color → MSAA depth).

- **New fragment shader**
  `src/video_core/host_shaders/ms_depth_to_color.frag` — fullscreen
  triangle, binds source as `texture2DMS`, `texelFetch`es sample 0 per
  pixel, writes `vec4(depth, 0, 0, 1)` to the color attachment. Sample
  0 is used verbatim rather than averaging — depth sampling downstream
  wants a specific visibility decision, not an intermediate.
- **New helper**
  `BlitHelper::ReinterpretMsDepthAsColor(width, height, num_samples,
  src_fmt, dst_fmt, source, dest)` — creates a depth-aspect sampled
  view on the source, a color-aspect attachment view on the dest,
  binds a cached pipeline keyed by `(num_samples, dst_format)`, draws
  the fullscreen triangle through the new fragment shader. Pipeline's
  `rasterizationSamples = e1` because the destination is 1x; the
  shader itself does the per-sample fetch.
- **Wiring**: new `else if` branch in
  `TextureCache::ResolveDepthOverlap` that matches the shape
  `cache.is_depth && cache.samples > 1 && !new.is_depth &&
  new.samples == 1` and dispatches `ReinterpretMsDepthAsColor`. The
  pre-existing `else` is kept with the shape-logging instrumentation
  so any *other* unhandled combination is still caught for future
  debugging.

### Verified behaviour

Confirmed on the user's RTX 5090 + 7950X3D box after redeploying the
binary through the gamma-debug QtLauncher entry:

- `[depth-dbg] Unimplemented depth overlap copy` warnings disappear for
  the D32Sfloat → R32G32B32A32Sfloat case (Driveclub's only offender).
- Night tracks: car headlights illuminate the road, track boundaries
  visible, AI car lights visible — screen-space lighting stack back
  online.
- Day tracks: subtle tightening of depth-dependent effects; no
  regressions observed.

### Status

**Resolved.** Both mechanisms addressed. Candidates for upstream:

1. `BlitHelper::ReinterpretMsDepthAsColor` + its shader + the
   `TextureCache::ResolveDepthOverlap` wiring are a clean, bounded
   fix. Good upstream PR shape — symmetric with the existing
   `ReinterpretColorAsMsDepth` path. Drop the `[depth-dbg]` tag
   before submitting; the shape-logging instrumentation is a
   reasonable addition on its own but can be a separate PR or left
   out of the fix PR.
2. The 60 fps patch issue is user configuration rather than an
   emulator bug, so nothing to upstream from that root cause —
   documenting it here for any future Driveclub guide on the
   compatibility repo.

## Upstream candidates

Code that is clean enough to feed back to `shadps4-emu/shadPS4` once the
experiment is validated:

1. The swapchain `LOG_INFO` at `vk_swapchain.cpp::Create()` and the
   `GetFrameViewFormat` per-format INFO log are broadly useful diagnostics
   and would fit as a standalone PR (drop the `[gamma-dbg]` tag).
2. Exposing the `pp.gamma` push-constant as a per-game JSON knob (my env-var
   path is a branch-local shim; the real fix is a config field plumbed through
   `emulator_settings.cpp`).
3. Possibly a fix for the `A2R10G10B10Bt2020Pq → Unorm` fallthrough, once we
   actually see a Driveclub session declare that format.

Everything else in this branch is scoped to the investigation and wouldn't
be upstreamed as-is.

## File / path cheatsheet

```
# Fork source (this branch)
~/Projects/shadPS4                 # akitaonrails/shadPS4, origin=fork, upstream=canonical
~/Projects/DriveClubFS             # akitaonrails/DriveClubFS, origin=fork, upstream=Nenkai

# Local build artifact
/mnt/data/Projects/shadPS4/build/shadps4

# Custom launcher (isolated, preserves working Manager setup)
/mnt/data/distrobox/gaming/bin/shadps4-driveclub-gamma-debug

# Production Manager path (unchanged)
/mnt/data/distrobox/gaming/.local/share/shadPS4QtLauncher/versions/main-2026-04-19/Shadps4-sdl.AppImage

# Driveclub install tree
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003                    # live (v1.28 now)
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003.v100-working-backup # rollback
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003-v128-test          # DriveClubFS input staging
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/Driveclub.v1.28.PATCH.REPACK.PS4-GCMR.pkg

# shadPS4 data dirs
~/.local/share/shadPS4/config.json                              # Vulkan.pipeline_cache_enabled=true
~/.local/share/shadPS4/custom_configs/CUSA00003.json            # per-game (unchanged)
~/.local/share/shadPS4/patches/Driveclub.xml                    # 60fps patch, re-enabled
~/.local/share/shadPS4/log/shad_log.txt
~/.local/share/shadPS4-gamma-dbg/                               # isolated wrapper's tree
```
