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

### Why the rabbit hole went on so long

The wrong initial premise drove the whole chain. "The image looks dim"
looked like a tonemap problem, so I reached for tonemap tools. But
Driveclub's race-start dim pocket is a **game-side fade-in VFX** (the
cinematic reveal lasts about 5-10 seconds before the game itself
transitions to full-brightness output), and everything I was watching
during it — dim menus becoming bright, dim scene becoming colourful —
looked indistinguishable from an auto-exposure adapting. It wasn't
auto-exposure; it was the game's own scripted fade.

Once I had the `SHADPS4_PP_BYPASS=1` diagnostic path, two frames of
observation showed raw game output was fine everywhere *except* the
fade, and the fade was unreachable from post-process math (you can't
multiply zero into visibility). The whole tonemap toolkit had been
solving the wrong problem.

The post-mortem lesson: **when "the image looks wrong" is the bug, the
first diagnostic should be a bypass path that shows raw game output.**
If bypass looks right, the emulator post-process is actively mangling
the signal; keep your hands off the tonemap and look elsewhere. If
bypass also looks wrong, then you're compensating for something real.
Driveclub fell into the first category. We added bypass near the end
of the session, after re-deriving the same answer with a dozen
different tonemap tweaks — it should have been the first test.

### Final configuration after cleanup

The gamma-debug wrapper is now down to just SDL HIDAPI env vars. The
shader does **standard sRGB encode plus Bayer dither** — no exposure
multiplier, no ACES, no auto-exposure, no bypass flag (there's nothing
to bypass since the only thing left is a standard sRGB encode). The
auto-exposure compute pass, ACES shaders, and exposure/tonemap push
constants were all removed in a cleanup commit. The Bayer dither
survived because it's a universal win on 8-bit swapchains, costs
nothing, and is independent of any tonemap decision.

What remains from this investigation, useful and kept:

  - `src/video_core/host_shaders/post_process.frag` — standard sRGB
    encode + 4x4 Bayer dither.
  - `src/video_core/host_shaders/ms_depth_to_color.frag` — MSAA depth
    resolve (Phase 4 fix).
  - Diagnostic LOG_INFO in `vk_swapchain::Create` and `GetFrameViewFormat`
    — cheap one-shot logs that surface useful facts about the render
    path without needing a GPU debugger.
  - Shape-logging warning in `ResolveDepthOverlap::else` — catches
    future unhandled combinations without flooding the log.
  - Pipeline cache config enable.

## Phase 5 — brightness follow-up after the cleanup (dead ends)

After the cleanup above landed, a later session revisited the "daytime
scenes look dim" issue with fresh eyes. Tried several approaches that
were each rolled back; documenting them here so we don't re-tread
them next time.

### Confirmed: Driveclub uses `sceVideoOutAdjustColor`

Added a `LOG_INFO` in the HLE implementations of
`sceVideoOutColorSettingsSetGamma` and `sceVideoOutAdjustColor` to see
whether the game's own brightness slider reaches the emulator. It does
— moving the in-game brightness slider fires calls with the expected
gamma value, and `pp_settings.gamma` updates in real time. The game's
slider is properly wired; the shader responds. The slider just tops out
before the image gets as bright as one might expect.

Those two log lines stay in the code as small diagnostic aids.

### Re-tried scene-aware auto-exposure (peak+mean) — removed again

Revisited the auto-exposure idea with a simpler design: sample peak
and arithmetic mean of a centre-cropped region, pick
`min(TARGET_MEAN / mean, SAFE_PEAK / peak)` as the boost multiplier,
clamp to `[1.0, 3.0]`. The peak cap was supposed to prevent blow-out
by construction.

Two problems killed it:

  1. It **normalises the output toward a target**, which *fights the
     game's brightness slider*. The user raises the slider → game
     outputs brighter frames → auto-exposure sees a higher mean →
     auto-exposure reduces the boost → net change on screen: zero. The
     slider stops doing anything visible.
  2. Even with the peak cap, the boost was applied globally to every
     pixel including HUD, so a dim scene with low measured mean pushed
     exposure up and blew out HUD text that was already near-white.

Removed the whole auto-exposure pass again. The lesson is sharper this
time: **do not build a feedback loop on top of a signal the user can
already control directly.** If the game has a brightness slider and we
sample what the game outputs, any auto-correction is going to cancel
the slider's effect by construction.

### Static linear boost — tried 1.5x, 2.0x, rolled back

Tried `color_linear * SDR_BRIGHTNESS` with `SDR_BRIGHTNESS` at 1.5,
then 2.0. Visibly different from 1.0 only on scenes that had bright
reference pixels (night with street lights). Daytime scenery stayed
dim because the source values are so low (~0.1-0.2 linear) that even
2x leaves them at 0.2-0.4 linear, still dim after sRGB encode. And
any pixel above 0.5 linear blows out hard. Rolled back.

The mathematical ceiling: a linear multiplier cannot brighten dark
midtones without also clipping anything bright. For this game's
daytime output distribution, there's no linear constant that wins
across the frame.

### Static gamma lift (pow curve) — tried 0.7, 0.85, rolled back

The one remaining lever that can lift darks without clipping whites:
a pre-encode gamma curve, `pow(color_linear, exponent)` with
exponent < 1.0. Tried exponent 0.7, then 0.85 after the 0.7 looked
washed-out/flat. 0.85 was less aggressive but introduced visible
colour banding in gradients (because the curve amplifies the 8-bit
source's quantisation steps on its way into a still-8-bit swapchain;
the Bayer dither helps but can't hide the bigger steps a stronger
curve produces).

User called this one dead: washed-out image and amplified banding,
not worth the modest midtone lift. Rolled back. Shader stays at
standard sRGB encode + Bayer dither.

### Conclusion: no free lunch on SDR

Driveclub was designed for the calibrated display pipeline a real
PS4 provided (the system-level RGB-range / display-calibration
settings Sony added through the 4.0 and 7.0 firmware updates). Our
Vulkan swapchain on a Linux desktop sits downstream of none of that,
so what the game writes to the framebuffer is shown faithfully, but
without the OS-side nudging a real PS4 applies before HDMI-out.

The remaining options to brighten Driveclub daytime scenes, all out
of scope for the current branch:

  1. **Implement proper HDR output.** shadPS4's existing HDR path
     activates when the game declares HDR + monitor supports BT2020
     PQ + `allowHDR` is true in config. Driveclub predates PS4 HDR
     (game released 2014, HDR arrived with firmware 4.0 in 2016), so
     it doesn't declare HDR. Retrofitting Driveclub to emit the HDR
     swapchain format would mean patching the game's rendering
     pipeline, not the emulator.
  2. **Patch Driveclub's shaders / GI constants** to push the
     daytime sun / ambient-light contributions. Multi-day reverse-
     engineering of the game's rendering code. Not rewarding in
     hours-per-shader-constant terms.
  3. **Accept the dim daytime look.** Moody and atmospheric is a
     Driveclub-specific design choice; the result we ship is what the
     game actually outputs. The in-game brightness slider works and
     gives the user some control.

Shipped configuration for this branch: option 3. The shader stays
minimal. Future sessions can pick up the HDR-output path or the
shader-patching path if either becomes important.

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

## Handoff status (2026-04-20, late)

This section is the current state after several more hours of probing on
`gamma-debug`. It supersedes the earlier optimism around one specific
surface fix.

### The hard conclusions

- The race-start blackout is **not**:
  - final present / swapchain gamma
  - `sceVideoOutAdjustColor`
  - QtLauncher / host display output
  - degamma / min-max blend / predication
- It is inside the game's **internal scene HDR / composite path**.
- HUD staying correct while the world blacks out is a stable discriminator:
  the UI path is fine; the world-composite branch is not.

### The key full-res surfaces

These are the only targets that kept mattering in every useful run:

- `0x500cdd0000` — `1920x1080` `R8G8B8A8Srgb`
  - visible world composite target
  - this is the thing that visibly goes black
- `0x5009688000` — `1920x1080` `B10G11R11UfloatPack32`
  - main HDR-like scene branch
- `0x5008130000` — `1920x1080` `B10G11R11UfloatPack32`
  - alternate HDR-like scene branch
- `0x500fdd0000` — `1920x1080` `B10G11R11UfloatPack32`
  - another HDR-like temporal/composite branch
- `0x500ddc0000` — `1920x1080` `R8Srgb`
  - auxiliary visible/mask branch that often stays alive
- `0x50105c8000` — `1920x1080` `R16G16Sfloat`
  - another stable scene-side intermediate

### What the stats now say

Across repeated clean baseline runs:

- `0x500cdd0000` repeatedly drops to zero during the blackout.
- `0x5009688000` often drops with it, but not always at the exact same
  moment.
- `0x5008130000` sometimes stays alive while `0x500cdd0000` is black,
  and sometimes later also dies.
- `0x500fdd0000` frequently stays healthy and can even become brighter
  while the visible target is black.
- `0x500ddc0000` and `0x50105c8000` often stay alive through the
  blackout.

That means the bug no longer looks like "one buffer dies and takes the
rest with it". It looks more like a broken handoff or dependency between
multiple HDR branches before the final visible composite.

### What the aggressive probes accomplished

The aggressive phase was still useful, even though it did not produce a
fix:

- Freezing `0x500cdd0000` changed the symptom immediately.
  - That proved it is on the critical visible path.
  - But freezing it mostly produced a stale full-screen world plate with
    HUD still updating on top.
- Freezing HDR branches (`0x5009688000`, `0x5008130000`, `0x500fdd0000`)
  also produced static-world / stale-plate symptoms.
  - That proved those surfaces are temporal/history/composite feeders.
  - But it stopped being diagnostic after a point, because "static
    postcard" only means "we pinned a temporal HDR branch", not which
    pass is actually wrong in the real unfrozen run.
- Broad compute sabotage proved the correct subsystem:
  destroying the compute-heavy scene branch killed the world while the
  HUD survived.
  But single-hash and small-group sabotage did not isolate a culprit
  reliably.

### What we tried and should stop retrying blindly

- Surface freeze as the primary diagnostic.
  - It has reached diminishing returns.
  - It keeps proving "temporal HDR branch" without telling us which
    *live* pass is the real source of the blackout.
- Early hot-path skip experiments on draw/dispatch writers.
  - Multiple attempts caused boot-time GPU crashes before any useful
    race data was collected.
- Small sampled-input substitution on the `0x500cdd0000` composite path.
  - The substitutions failed technically as an isolator and never
    produced a clean directional result.

### Current baseline on `gamma-debug`

As of this handoff, the branch is back to a **plain non-freeze,
non-sabotage, booting baseline** with the useful passive stats still in
place:

- `SceneFreezeMode` is effectively `none`
- `scene-sabotage` is `none`
- the build boots and reproduces the real behaviour again
- the world blacks out for 20+ seconds and then returns, matching
  vanilla baseline

### The fastest reasonable next step

Do **not** go back to more surface-freeze variants first.

The next probe should be:

1. **Passive**
   - no skipped draws
   - no frozen surfaces
   - no writer sabotage
2. **Late-start**
   - arm only after the visible world has been healthy for a short
     stable window
3. **Restricted to the full-res HDR/visible branch**
   - `0x500cdd0000`
   - `0x5009688000`
   - `0x5008130000`
   - `0x500fdd0000`
   - `0x500ddc0000`
   - `0x50105c8000`
4. **Sequence-oriented**
   - capture the exact pass order and shader hashes during the
     transition from healthy frame -> blackout frame
   - ideally with a slightly longer logging window than the previous
     late-start trace attempt

The current best hypothesis is:

- not "one bad output gamma path"
- not "one dead sampled texture"
- not "one buffer always dies first"
- but a broken transition inside the full-res HDR composite/exposure/
  history chain before `0x500cdd0000`

That is where the next round should spend its budget.

## Phase 7 — aggressive torture probe (post-codex)

Picking up after the late-2026-04-20 codex handoff. All of codex's
`src/` changes were stashed (`git stash list` will show them labelled
`codex probe scaffolding 2026-04-20`) so the tree is clean except
for the doc itself. The torture hook below was built from scratch on
`vk_rasterizer.cpp::BindTextures`.

### Mechanism

Env var `SHADPS4_DC_TORTURE=1`. When any draw writes to one of the
four critical HDR/visible surfaces —

```
0x500cdd0000 1920x1080 R8G8B8A8Srgb          visible composite
0x5009688000 1920x1080 B10G11R11UfloatPack32 HDR main
0x5008130000 1920x1080 B10G11R11UfloatPack32 HDR alt primary
0x500fdd0000 1920x1080 B10G11R11UfloatPack32 HDR alt composite
```

— the second pass of `BindTextures` null-substitutes specific sampled
inputs before the descriptor write. Per-substitution logging is
rate-limited to one line per unique (dst, src-addr, dims, format)
tuple, tagged `[dc-torture]`. No freeze, no sabotage, no shader
replacement — one-vector experiment only.

### Run 1 — substitute every sampled input < 1024×1024 on matching draws

Result: **main world pitch black, HUD intact, world visible in the
car's mirror**. 370 unique substitutions, dominated by BC1/BC3/BC5/
BC7 material textures (74 BC5, 71 BC1, 65 BC7, …).

Takeaway: I nulled every material texture for every geometry pass
that writes to the critical targets. Cars, track, sky had no diffuse
/ normal / AO — of course they drew black. Misframed as a "composite"
bug; these targets actually receive direct material-shaded geometry
passes. The mirror surviving was not a real signal — see below.

### Run 2 — same, but skip BC-compressed formats

Result: **main world pitch black, HUD intact, mirror also black.**
58 unique substitutions, all non-BC. Format histogram:

```
 12 R32Uint                8x4  tile lighting index buffers  (many)
 10 R16G16B16A16Sfloat     various
  8 R8Unorm                32x32
  6 B10G11R11UfloatPack32  various
  4 R32Sfloat              at 0x500c4d0000 at 3 different sizes
  4 R8G8B8A8Srgb           256x256 textures
  2 R32G32B32A32Sfloat     80x48 and 364x276 — tile luma grids?
  1 D32Sfloat               960x540 — half-res depth
```

Also killed: ten half-res 960×540 buffers (SSR/bloom/temporal
pyramid), mirror RT candidate `0x505fe57600 512x256 R8G8B8A8Unorm`,
and the `0x500c4d0000 R32Sfloat` cascaded shadow/hierarchical depth
at three mip sizes.

Mirror visible in Run 1 was therefore a coincidence of BC-null
rendering: when cockpit BC materials were nulled they shaded flat
black, but shader discards keyed on `texture(bc).a` with null
textures returning 1.0 meant alpha tests didn't clip, and the
mirror-mesh region let whatever was drawn before show through.
Not a real rendering path. Dismiss.

Takeaway: the R32Uint `8×4` buffers are almost certainly forward+
per-tile light lists — zeroing them means "no light affects any
tile" which alone would black out most of the scene. The four
`32×32 R16G16B16A16Sfloat` buffers at `0x5008130000` inputs look
like ambient / irradiance probe grids. And the half-res pyramid is
clearly needed for post-process.

### Run 3 — substitute only `0x501d630000 80×48 R32G32B32A32Sfloat`

The 80×48 R32G32B32A32 buffer has the exact shape of a per-tile
luma grid feeding HDR eye-adaptation. Isolated this as the only
torture target.

Result: **scene renders normally, race-start fade still present and
visually identical to baseline**. One substitution fired and was
logged. Everything else on screen behaved as usual.

Takeaway: this buffer — the strongest-shaped candidate for the
adaptation input — doesn't drive the fade. Not conclusive for all
small textures, but reduces the space of plausible single-texture
culprits.

### Run 4 — substitute eight small non-BC candidates

Tortured all of:

```
0x501d630000 80x48   R32G32B32A32Sfloat
0x501df20000 364x276 R32G32B32A32Sfloat
0x50ba598400 128x2   R16G16B16A16Sfloat   — odd-shape histogram?
0x5003896400 32x32   R8G8B8A8Unorm
0x5065f2dc00 32x32   R16G16B16A16Sfloat   — probe
0x5065f2bc00 32x32   R16G16B16A16Sfloat   — probe
0x5065f31c00 32x32   R16G16B16A16Sfloat   — probe
0x50239c0000 32x32   R16G16B16A16Sfloat   — probe
```

Result: **scene pitch black, HUD only, mirror black**.

7 of the 8 substitutions fired (`0x501df20000` didn't rebind during
this run). The black scene identifies the four 32×32 R16G16B16A16
buffers as the ambient-probe grids responsible for ambient
illumination — kill them and the scene has zero non-direct lighting
and draws as black.

Takeaway: these probes are essential for any visible scene.
Confirmed as ambient/irradiance grids, not adaptation feedback.

### Run-family conclusion for the texture-sampling hypothesis

Across 4 torture runs the only configuration that produced a
visible scene showed **the race-start fade unchanged**. I can't
prove the fade is feedback-driven by a small sampled texture,
because I only tested one candidate in isolation (80×48). But the
stronger result is negative: the class of plausibly-shaped inputs
(tile-averaged HDR float grids, 32×32 probe-shaped, small HDR
histograms) either doesn't affect the fade or kills the scene
entirely.

The fade persisting regardless of sampled-texture torture means the
driving signal is either:

1. a push-constant / SGPR scalar pushed per-draw by the game
2. a uniform buffer whose contents ramp over time
3. a shader-intrinsic time-based computation reading game time

None of those are reachable from texture-binding substitution.

### Variability signal — the part I was wrong about

Initially I claimed the fade duration looked "shorter on this tree
than during codex's runs" and offered that as evidence the codex
changes might have worsened it. That was wrong.

**Fade duration is highly variable and non-correlatable**: under
10 seconds on some race starts, ~20 seconds on others, 30+ seconds
sometimes, and occasionally never recovers. Same build, same track,
same time of day. No observed correlation with anything reproducible.

The "never came back" case is the load-bearing signal. Scripted
cinematic fades have fixed durations and always complete; a loop
that sometimes fails to converge is inconsistent with "scripted
animation" and more consistent with:

- a feedback integrator whose input can get stuck at zero
- a ping-pong buffer pair with cache aliasing (codex's own line
  of investigation on `0x5009688000` create→ExpandImage→CopyImage
  churn)
- an async streaming / cache-warmup race that sometimes lands OK
  and sometimes traps

This re-opens the feedback-loop hypothesis but via a mechanism
that texture-null torture cannot falsify — the emulator cache state
itself is what's varying.

### What to try next (no more "accept and move on")

1. **Solid-white substitute** on the same four 32×32
   R16G16B16A16Sfloat probe-candidates instead of null. If the
   scene becomes blown-out-bright (as expected when ambient probes
   are set to white) but the fade changes shape or disappears,
   something in the adaptation chain is reading one of them.
2. **Per-frame binding log** for draws into `0x500cdd0000` during
   the first 15 seconds of a race. Look for a binding whose address
   flips between two values each frame (ping-pong) vs. one that
   stays put — mismatch would show the cache-alias bug codex saw.
3. **Push-constant / UBO torture** — intercept the uniform-buffer
   write path for draws to the critical targets, stomp the first
   N floats to 1.0, observe whether the fade disappears. This is
   the natural next vector once texture-null is exhausted.
4. **Codex's `ExpandImage` / `FindImage` same-address preservation**
   (currently stashed) — re-apply and re-test under the variability
   lens. If the handoff was already addressing the aliasing but the
   tests weren't repeated enough to see the variability, the change
   may be a real fix masked by the non-determinism.

The texture-sampling torture track is closed. The next probe vector
must reach either game uniform data or the cache-aliasing path.

## Post-reset clean branch log (2026-04-21)

This section supersedes the contaminated "stacked torture" phase that
followed the note above.

I explicitly restored these files to `HEAD` before resuming:

- `src/video_core/renderer_vulkan/vk_compute_pipeline.h`
- `src/video_core/renderer_vulkan/vk_rasterizer.cpp`
- `src/video_core/renderer_vulkan/vk_rasterizer.h`
- `src/video_core/texture_cache/texture_cache.cpp`

Only the experiments below should be treated as clean evidence. The
older stacked torture runs changed too many overlapping branches and
should **not** be used to rule out families anymore.

### Clean baseline reset

- The renderer debug stack was removed and rebuilt from a clean
  baseline.
- `vk_rasterizer.cpp` kept only one persistent convenience change:
  Driveclub `CUSA00003` auto-enables the experiment path when launched
  from Manager.

### Branch 1 — late fixed-function family on tracked race-target draws

Shot on:

- viewport/scissor
- depth/stencil/bounds/bias
- cull/front-face/discard
- blend constants
- color write masks
- attachment feedback loop

Gate:

- graphics draws whose render targets matched the known Driveclub race
  chain (`0x500cdd0000`, `0x5009688000`, `0x5008130000`, `0x500fdd0000`)

Observed result:

- HUD stayed intact
- scene started with a hint of color, then faded to black
- normal blackout timing remained

Conclusion:

- This was a **clean miss**.
- Broad late graphics fixed-function state on the known race-target
  draws is not enough to hit the blackout switch.

### Branch 2 — broad race-scene draw shot on tracked targets

Added on top of the clean baseline:

- null sampled textures on race-target draws
- forced color clears/loadops in `BeginRendering` for the tracked race
  targets
- depth/stencil clear suppression on those draws

Observed result:

- screen went orange / yellow underneath intact HUD
- mirror still started black, then recovered later in a tinted state
- blackout state clearly still existed

Conclusion:

- This hit the visible race-scene branch hard
- but **did not** hit the blackout controller itself
- useful only as evidence that "recoloring the main race image" is not
  the same as killing the blackout

### Branch 3 — transfer / movement family

The draw-side repainting above was removed. The next clean shot moved
to transfer-style operations:

- `EliminateFastClear`
- `Resolve`
- compute image copy
- compute image clear

Arm logic:

- arm on the known race path, then apply the shot for a short submit
  window to scene-sized color images

Observed result:

- all black scene
- HUD intact
- blackout still there
- after recovery, the image came back heavily corrupted

Conclusion:

- This was another **clean miss** for the blackout switch.
- Transfer-style image movement / clear operations alone do not explain
  the fade-to-black behavior.

### Branch 4 — remaining draw-state family, combined with transfer shot

Kept Branch 3 alive, then added the untouched graphics draw-state
bucket on race-target graphics passes:

- zero `push_data`
- null `Flatbuf` user-data buffers
- null all non-special read-only descriptor buffers

Observed result:

- all black scene
- HUD intact
- mirror black
- blackout unchanged

Conclusion:

- This was also a **clean miss**.
- Per-draw user-data / flat user-data / read-only buffer descriptors on
  the known race-target graphics path are not sufficient to hit the
  blackout switch.

### What these clean misses actually rule out

After the clean reset, the following families have now been probed and
failed to disable or materially destabilize the blackout itself:

1. Late fixed-function graphics state on tracked race-target draws
2. Broad sampled-texture / forced-clear corruption of the tracked main
   race-scene targets
3. Transfer-style image movement and clear family (`Resolve`,
   `EliminateFastClear`, compute image copy/clear)
4. Per-draw graphics-side user-data / flatbuf / read-only descriptor
   buffer family on tracked race-target passes

### What this does **not** justify anymore

Do **not** reuse the contaminated post-doc assumptions that:

- "the blackout branch was already hit" because the scene turned green,
  blue, orange, or purple
- "the mirror path was nearly isolated" by those tint runs
- "sampled-input poisoning proved a participating blackout branch" in
  the stacked builds

Those statements came from overlapping experiments before the clean
reset and are not reliable as eliminations.

### Current clean takeaway

After the reset, every broad shot tied to the **known race-scene draw
path** or to its immediate transfer family has still left the blackout
intact.

That means the current clean evidence points away from:

- the visible tracked race-target draw path itself
- the immediate transfer/resolve/compute-copy/compute-clear family for
  those scene-sized images
- the obvious graphics per-draw control-state family on those same
  passes

The next branch must therefore be chosen as a genuinely different
family, not another narrower variation of the same race-target draw
path.

### Probe rule going forward

Do **not** spend build-and-test time on very small follow-up changes
whose most likely outcome is only to reconfirm what is already known.

Use this rule instead:

- reset to clean baseline before each new shot
- one hypothesis family per build
- prefer broad class-level probes over tiny surgical deltas
- only narrow after a family has clearly hit or disturbed the blackout
  itself
- if a task is interrupted to answer another request, resume the
  interrupted task immediately after answering; do not wait for an
  extra "continue" prompt

In other words: no more "small confirmation" shots unless a previous
broader experiment has already proven that the branch is blackout-bearing.

### Race gate rule

Do **not** arm broad blackout shots from "full-res target exists" alone.
That repeatedly hit boot/menu paths and blacked the UI before the race
ever started.

The clean passive discriminator established a safer coarse gate:

- only treat the **later** race-like graphics pipeline hashes as valid
  arm signals
- explicitly exclude the early submit-97 hashes that also appeared
  before the real race-start path settled

Later race-like hashes seen in the passive `[dc-gate]` run:

- `0x967922c49cee2dd1`
- `0xe72e555cdb85af86`
- `0xb98e78a7a28007ce`
- `0x3ff0fc8f05bc302d`
- `0xd14226a181106f7e`
- `0x1bb555896c9e247e`
- `0xff9e11acb5a72dff`
- `0x5148c06fb63b96e0`
- `0x660a29eb5e92b0ad`
- `0xfe66b45b84a7dd16`
- `0x02c197f768d8d430`
- `0xf6e5670be11b0009`

Early hashes that must **not** arm shots:

- `0x1cdd747ee89204c0`
- `0x6bde71906ac1af18`

Operational rule:

- if a future broad family needs a race-start gate, it should arm from
  this later-pipeline whitelist first
- do not go back to target-only arming unless a later result proves this
  gate insufficient

Failure note:

- the later-pipeline whitelist **alone** was still not safe enough for
  active PM4 / orchestration shots
- one such broad shot still armed during menu load and crashed with
  `Attempted to access invalid address 0x200`

Persistent baseline guard:

- keep a baseline-safe race window helper in the renderer
- it should stay passive by default and only arm when all of these are
  true in the same submit:
  - at least `3` distinct hashes from the later whitelist
  - depth valid
  - visible target `0x500cdd0000`
  - at least one HDR target among `0x5009688000`, `0x5008130000`,
    `0x500fdd0000`
- future broad shots should key off that latched race window rather than
  the later whitelist alone

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
