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
- require a recent `sceVideoOutAdjustColor(... gamma=0.5)` pulse before the
  later race-like draw cluster is allowed to arm the race window

That extra gamma hint matters because the pipeline-only guard still caught
menu-loading lookalikes. The baseline-safe guard now requires both:

- a recent `gamma=0.5` video-out hint
- the later race-like draw signature with visible target + HDR target + depth

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

## Phase 8 — UI-asset surgery (2026-04-21)

All the asset-side probes in this phase were scoped to Driveclub
`CUSA00003` v1.28 running through
`scripts/run_driveclub_overlay.sh`, with the patched `.rpk`s and UI
text panels dropped into
`/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003`. The
overlay dir is a symlink mirror of the installed game with only the
patched files as real content.

### Baseline on entry to this phase

From the prior asset-patch work:

- Track-pack `PostFXConfig_{interior,exterior,helmet}` inline scalars
  zeroed (TemporalFade, ManualExposureLog2, ManualAutoMix, Master-
  Brightness, WeatherOverrideMix, MotionBlurLevel).
- All prerace camera `Fade` values zeroed (old `preracecam_*`, new
  `new_preracecam_*`, `CutsceneCamera_*`, `WorldCamera_*`,
  `track_preview`).
- India night LUT references rewritten to India day LUTs.
- Prerace animationlib `Fade` track keyframes zeroed.
- `globaldata.rpk` kept at vanilla (prior patches there were
  unstable).

Observed effect of that baseline: the vanilla "fade to near-black" at
race start is **replaced by a semi-transparent static image of the
selected car** over the live race, with the race faintly visible
underneath and HUD intact on top. Same variable ~5-30 s duration
with occasional "never recovers".

### Iteration 2.1 — PostFXConfig bucket A (TXAA / NonBloomed)

`TXAAOverallWeight = 0`, `TXAAColourClamping = 0`,
`NonBloomedAttenuation = 0` on all three `PostFXConfig_*`.

Result: the overlay became **permanent instead of gradually lifting**.
This inverted the earlier framing. Those PostFX fields were not the
overlay's source; they were part of the decay that eventually washed
it out. Reverted. Kept `Iteration 2.0` attempted combined shot notes
and the learning in
`docs/driveclub-next-test-plan.md`.

### Iteration 2.0 — Transition/ManualExposureLog2 animlib sweep

`PatchTransitionTracks` and `PatchNamedTrackKeyframes` reused the
`Fade`-layout offsets (`0x2c / 0x4c / 0x6c`) blindly wherever the
ASCII field name appeared inside the animationlib resource. This
corrupted the keyframe stride of other curves and crashed the game's
`.rpk` parser during race load (null-ptr read in the eboot at
`0x80067894a`). Both helpers are still in `Program.cs` as dead code
behind guarded call sites for a future safer decoder.

### Iteration 3.x — newui panel sweep

The overlay persisted across every UI panel disable we tried. In
order:

1. `vehicle_select_background.txt` — set all gradient `A_RGBA` /
   `B_RGBA` alphas to `0.000`. No effect.
2. `loading_freeplay.txt::image_track.ACTIVE = FALSE`. No effect.
3. `backgrounds.txt::BG_RGBA` alpha `0`. No effect.
4. Broad nuke: every `loading_*.txt` plus `pause_background.txt` set
   to root-PANEL `ACTIVE FALSE`. No effect.
5. `get_in_car_animation.txt` set to root-PANEL `ACTIVE FALSE`. No
   effect.
6. `get_in_car_animation.txt` made `0×0` offscreen with `CLIP TRUE`
   and alpha `0`. No effect.
7. `freeplay.ctl` Page entry `getincar.freeplay` commented out.
   **Crash** — null-ptr read at `0x800285285` when the game tried to
   navigate to that page.

Reverted everything in 3.x once the file-open log confirmed the
emulator was reading the overlay files (774+ newui panel reads in
one session, every patched filename included).

### What that rules out definitively

- The overlay is not rendered through any `newui/panels/*.txt` file
  we have touched — and we covered every fullscreen panel that a
  search across the panel set identified (`WIDTH 192x` match, plus
  the pre-race / get-in-car / loading family).
- The `FreeplayGetInCar` controller (and its `TourGetInCar` /
  `ChallengeGetInCar` siblings) ignore the panel's `ACTIVE` flag,
  dimensions, RGBA alpha, and clip rect. Setting the panel to a
  `0×0` sprite offscreen with alpha 0 had no visible effect on the
  car render.
- Removing the Page registry entry for `getincar.freeplay` crashes.
  The page navigation graph is hard-required.
- The dim layer is not `backgrounds.txt`, not `pause_background.txt`,
  not a `loading_*` gradient, not a `vehicle_select_background`
  gradient.

### Where the overlay actually lives

Given the sum of the evidence above, the car-image overlay and its
accompanying dim layer are drawn by the `FreeplayGetInCar` controller
(compiled C++ code inside the eboot), not by the UI panel system.
The controller writes the 3D car render and the dim layer directly
into the main HDR / composite scene target addresses we identified
in earlier phases (`0x500cdd0000`, `0x5009688000`, `0x5008130000`,
`0x500fdd0000`). The UI `.txt` panels for those pages are carriers
for the page navigation graph, not the render surface.

This is consistent with an "engine-driven UI" architecture where the
declarative panel files define layout slots and the C++ controller
fills them by talking directly to the renderer.

### Dead ends attempted before escalation

We already ruled out `sceVideoOutAdjustColor` / host gamma as the
overlay carrier during Phase 4:

> stubbing out the gamma write and forcing presenter gamma to stay at
> `1.0` did not move the blackout

and forcing `Finish()` around the race-start window also failed. So
the "engine-side clear on gamma-pulse" hook that might look obvious
from the gamma=0.5 race-start signal has already been tried in a
different form and was a clean miss. There is no sceVideoOut-side
lever left to pull from the emulator side.

### Conclusion of the asset track

The asset-side surface we can reach from `.rpk` and `newui/*.txt`
patches is exhausted as far as this overlay is concerned. The
controller that renders it is not reachable through any of those
declarative files. The remaining escalation path is:

- binary-patch the eboot to neutralize the `FreeplayGetInCar` render
  path

See the next section for the plan.

## Phase 9 — eboot binary-patch plan

### Target

The `FreeplayGetInCar` controller class inside
`/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin`.
Specifically: neutralize its render/update function so the page still
navigates and transitions (page dictionary stays complete) but no 3D
car and no dim layer are drawn while the page is active.

Known facts from early recon (2026-04-21):

- `eboot.bin` is 25 MB; `file` reports `data` (stripped / unrecognised
  ELF variant — PS4 OELF, probably post-sceAuthEtcFromSelf decryption).
- `FreeplayGetInCar` ASCII string is present twice in the binary.
  Siblings `TourGetInCar`, `ChallengeGetInCar`, `GetInCar` also
  present.
- `readelf` / `objdump` / `nm` are available. Ghidra and radare2 are
  not currently installed on this host; we will add one before doing
  real disassembly work.

### Approach

1. **Install a disassembler** with PS4 OELF support. Shortlist in
   preference order:
   - `ghidra` (Arch: `pacman -S ghidra`) — de facto standard for
     console RE; ships with a built-in OELF loader via community
     plugins.
   - `radare2` / `rizin` — lighter, script-friendly; OELF partial
     support via `iaito` / plugins.
   - As a last resort: parse the OELF segments by hand in Python,
     feed the `.text` range to `capstone` (`pip install capstone`).

2. **Find the controller's class/vtable** in two passes:
   a. Locate the two ASCII occurrences of `FreeplayGetInCar` via
      `grep -abo` on the raw file. Record file offsets.
   b. In the disassembler, search for data-ref / LEA references to
      those offsets. They will fall inside the controller registry /
      class constructor. Follow to the class's virtual-method table.

3. **Identify the render dispatch**. The controller vtable has a
   handful of virtual methods; the one we want is the per-frame
   render call that issues the 3D car draws + dim composite. Common
   naming patterns: `Render`, `OnRender`, `Draw`, `OnDraw`,
   `UpdateAndRender`. The method will dispatch into the engine's 3D
   renderer (many sceGnm / graphics-module calls downstream).

4. **Patch strategy**. Two variants to try in order of lowest risk:
   a. Prologue RET — replace the first byte of the method with `0xC3`
      (`ret`). The page is still active, timer still counts, but the
      method is a no-op. If the method has a non-void return, we may
      need to stub a return value (zero the return register before
      the ret).
   b. NOP the specific draw call — if prologue-RET breaks the flow
      (hang / soft-lock), find the GNM draw dispatch inside the
      method and replace its `call` instruction with NOPs of equal
      length.

5. **Apply safely**:
   - Copy real `eboot.bin` into
     `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin`
     (replacing the current symlink).
   - Keep the unpatched copy as `eboot.bin.vanilla` next to it.
   - Patch the overlay copy byte-for-byte. Document every patched
     offset in this doc and in a companion `eboot_patches.md`.

6. **Verify**: launch via `scripts/run_driveclub_overlay.sh`, run a
   Munnar 19:30 start, observe whether the overlay disappears while
   the rest of the flow (page navigation, HUD, race start) is intact.

### Rollback

The overlay eboot lives only at
`tmp/driveclub_overlay/CUSA00003/eboot.bin`. Deleting it and
re-symlinking to the real install restores vanilla behaviour in one
command. No destructive operation touches the installed game tree.

### Risks

- The controller may have more than one callsite that renders the
  car (e.g. separate `Update` + `Render` method, or the render is
  done by a child actor). First patch might be incomplete.
- Driveclub's v1.28 eboot may have integrity checks. If a plain byte
  patch causes the OELF to fail its internal signature check, we'll
  see it as a launch-time refusal rather than a visual change.
- Patching may break `TourGetInCar` / `ChallengeGetInCar` if those
  share the same controller class via inheritance. Minor — we aren't
  using tour / challenge modes in the repro.

### Recon already completed (2026-04-21 end-of-session)

Concrete data gathered, stored at
`/mnt/data/Projects/shadPS4/tmp/eboot_extract/`:

- `eboot.elf` — inner ELF carved out of the OELF at file offset
  `0x120` (`OELF header = 32 B + 8 × 32 B segment descriptors`,
  rounded to 16 B → 0x120).
- `eboot.et_exec.elf` — same ELF with `e_type` flipped from
  `ET_SCE_EXEC (0xFE10)` to `ET_EXEC (0x0002)` and OSABI from
  `FreeBSD (0x09)` to `SYSV (0x00)` so GNU binutils and LLVM tools
  accept it. Standard `objdump` still refuses (no section headers);
  `llvm-objdump -d` works.

ELF layout:

| Segment | File offset | VA | File size | Flags |
|---|---|---|---|---|
| LOAD #0 (text + rodata) | `0x4000` | `0x0` | `0x1551ab8` | R E |
| LOAD #1 (data) | `0x1558000` | `0x1554000` | `0x11be20` | RW |

→ `file_off (text) = VA + 0x4000`.
→ `file_off (data) = VA - 0x1554000 + 0x1558000 = VA + 0x4000`.

String VAs (all in text segment):

| String | VA | File offset (inner ELF) |
|---|---|---|
| `TourGetInCar` (×2) | `0x12c4041` / `0x12c4052` | `0x12c8041` / `0x12c8052` |
| `FreeplayGetInCar` (×2) | `0x12c4848` / `0x12c485d` | `0x12c8848` / `0x12c885d` |
| `ChallengeGetInCar` (×2) | `0x12c4c57` / `0x12c4c6d` | `0x12c8c57` / `0x12c8c6d` |

The two `FreeplayGetInCar` strings are a 20-char
`FreeplayGetInCarPage` followed by a 16-char `FreeplayGetInCar`
(the page's class name and the controller's class name).

Instructions that reference `FreeplayGetInCar` string VAs:

| VA | Bytes | Instruction | Targets |
|---|---|---|---|
| `0xf89b60` | `48 8d 05 f6 ac 33 00` | `lea rax, [rip+0x33acf6]` | `0x12c485d` (controller name) |
| `0xf89b70` | `48 8d 05 d1 ac 33 00` | `lea rax, [rip+0x33acd1]` | `0x12c4848` (page name) |

Each is followed immediately by `c3` (`ret`) and padding NOPs. These
are two virtual-method name-getter functions — standard Itanium
RTTI accessors that return a pointer to the class's ASCII name.

The constructor that writes the vtable pointer for this class is
directly above them, at `VA 0xf89b00-0xf89b50`:

```
f89b39: lea rax, [rip+0x6528a0]   # = 0x15dc3e0
f89b40: add rax, 0x10             # points to first vfn slot
f89b44: mov [rbx], rax            # *this = vtable
```

Vtable lives at VA `0x15dc3e0` (file offset `0x15e03e0` inside the
inner ELF, inside LOAD #1 data segment). Its function-pointer slots
are zero or carry **SCE dynamic relocation encoded placeholders**:

```
vtable at VA 0x15dc3e0 / file 0x15e03e0:
  +000  0000000000000000  (offset-to-top)
  +008  0000000700000016  (RTTI ptr reloc)
  +010  0000000000000000  vfn[0]
  +018  0000000700000017  vfn[1]  (reloc, not an addr)
  ... rest zero
```

The nonzero 8-byte values (`0x0000000700000016` etc.) are SCE
relocation-record indices, not resolved function addresses. The
static file does not carry the real virtual method addresses for
`Render` / `OnDraw` / etc. — those are filled in by the dynamic
linker at runtime.

### What that means for the patch plan

We cannot pre-compute the `FreeplayGetInCar::Render` file offset by
reading the static ELF alone. Two viable paths from here:

1. **Parse the SCE dynamic section** (DYNAMIC segment starts at file
   offset `0x186e568`) to extract the relocation table and symbol
   table, resolve the vtable slots to their intended target
   addresses, and patch the render slot.
   - Needs either a tool with SCE OELF support (Ghidra + PS4 plugin,
     rizin + rz-pssl, or custom Python) or the shadPS4 loader code
     itself extended to dump resolved relocations.

2. **Patch the class constructor instead of the vtable**. The
   constructor at `VA 0xf89b00` stores `0x15dc3e0 + 0x10` as the
   vtable pointer in the object. If we change the source address to
   a vtable that contains a NO-OP/RET stub for the render slot, any
   virtual dispatch becomes a no-op. Requires either:
   - a scratch region in the file to hold a fake vtable, or
   - editing the existing vtable after dynamic linking, which is
     equivalent to path 1.

3. **Patch the RTTI name getter**. The name-getter at `VA 0xf89b60`
   returns a pointer to the `FreeplayGetInCar` class name. If we
   change the return string to point at a different class name that
   the factory does not recognise, the registry lookup for the
   `getincar.freeplay` page fails at controller-registration time,
   and the page becomes unrenderable in the same way the
   `comment-out-the-Page-entry` experiment did — which crashed the
   game. Not viable.

4. **Patch a downstream SCE-GNM draw dispatcher**. If we can find a
   specific draw-state set or texture binding that only the
   GetInCar controller path hits, we can NOP it. Requires full code
   analysis. Same tooling requirement as path 1.

### Recommended order of operations for next session

1. **Install a real RE tool with PS4 OELF support.** Fastest option
   likely `ghidra` from the Arch repos, plus the community
   `ghidra-ps4-loader` plugin. Alternative: rizin with SCE plugins.
   Either gives us proper relocation resolution and xref tracing.

2. **Load `eboot.et_exec.elf` (or the original OELF if the plugin
   can handle it) into the chosen tool.** Use the string VAs above
   as anchor points. Find the class vtable at VA `0x15dc3e0` once
   relocations are resolved. Identify the render method slot.

3. **Patch the overlay eboot**, not the real install. Overlay path:
   `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin`.
   Current overlay is a symlink — break it and drop a patched copy.
   Keep `eboot.bin.vanilla` beside it for one-step rollback.

4. **Test via `scripts/run_driveclub_overlay.sh`.** Repro track:
   Munnar 19:30 clear.

### File-path cheatsheet for the binary-patch work

```
# Original OELF (read-only install)
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin   (25 MB OELF)

# Carved inner ELF (writable, for analysis)
/mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.elf              (inner ELF, ET_SCE_EXEC, FreeBSD)
/mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.et_exec.elf      (same, retyped to ET_EXEC + SYSV so
                                                                      GNU/LLVM tools parse it)

# Overlay eboot (write path for the patched binary)
/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin (currently symlinked to real install)
```

Everything above this line is asset-side; everything below will be
byte-level patching of the eboot. No emulator source changes planned
for this phase.

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

## Phase 10 — 2026-04-22 rethink: what the last day actually proved

This section exists to prevent another loop through adjacent
`CameraFade` / UI / late-render branches.

### Clean misses and what they mean

1. **Late rendered/image tree is not the blackout master switch.**
   The full chain around:
   - late black carrier `0x1c2223026ec47d56`
   - `0x5000108000`, `0x5000900000`
   - `0x5008130000`, `0x5009688000`, `0x50105c8000`
   - the parallel `0x500fdd / 0x5015ae / 0x501e8f` branch
   - the tiny `32x32` side input branch
   
   was stressed heavily. We changed:
   - what image appears under blackout
   - overall darkness
   - whether the main view was black while HUD survived
   
   But **mirror blackout/recovery still behaved normally**. Therefore
   this tree is a carrier/composite path for the main view, not the
   shared blackout timer/gate.

2. **Track-local and local prerace packs are adjacent only.**
   `india_road_point_02_n.rpk`, `india_landscape_gui.rpk`, local
   prerace camera fades, local postfx scalars, local animlib edits:
   all of these changed the latched image or the darkness underneath
   blackout, but never removed the blackout itself.

3. **`globaldata.rpk` is probably relevant, but direct patching is not
   a usable path.**
   Even narrow `globaldata` edits (fade-only, RTT/grading-only, shared
   postfx actor edits, single named fade-ish edits) repeatedly caused
   black boot / hang / crash before race load. So:
   - `globaldata` is still a plausible shared layer
   - but brute-force pack patching there is too unstable to teach us
     anything clean

4. **Prerace UI/page families are not the blackout carrier.**
   Replacing or rerouting:
   - `FreeplayGetInCar`
   - `PreraceWaiting`
   - `PreraceCountdown`
   - `get_in_car_animation`
   - `vehicle_select_background`
   - `loading_freeplay`
   - `loading_india.rpk`
   - `vehicle_previews.rpk`
   
   either did nothing, broke handoff flow, or only changed decorative
   layers. The actual blackout persisted.

5. **Safe `newui` fade-control assets are a clean miss.**
   We widened `newui/control/transitions.ctl` from a narrow alpha-track
   flatten to the whole safe fade family:
   - `FadeUp`, `FadeOut`
   - `ZoomInAndFade`, `ZoomOutAndFade`
   - `FaceOffFadeIn`, `FaceOffFadeOut`
   - `OverdriveTextFadeIn`, `OverdriveTextFadeOut`
   - `RG_ScrollReady*`, `RG_ScrollGo*`, `RG_FadeBG*`
   - `LiveColumnFade`
   - `ResultIn`, `ResultOut`
   
   and rerouted the live race-path panels:
   - `loading_freeplay.txt`
   - `x_freeplay.txt`
   - `x_live_lobby_pre_race.txt`
   
   to a no-op `NoFade` transition. Result: **baseline blackout**.
   Therefore the safe declarative `newui` fade layer is not the switch.

6. **String-adjacent camera/fade event names are not the switch.**
   Broad and narrow experiments around:
   - `CameraFade`
   - `SetCamera`
   - `ScreenFadeOut`
   - `StartGame`
   - `AutoPilot`
   - `GarageEnable`
   
   produced only:
   - much darker starts
   - broken race launch handoff
   - missing UI strings
   - boot breakage
   
   but never disabled the blackout. The important lesson is not just
   that these names missed; it is that **name-based/static string
   patching is too indirect**.

7. **A real code-side `ScreenFadeOut` store was identified and was a
   clean miss.**
   The direct handler/store at the inner-ELF function around
   `0x146640`, with the matched overlay raw-store at `0x14a847`, was
   NOPed by signature. Result: **baseline blackout**. So the obvious
   explicit `ScreenFadeOut` config/store is not the controlling branch.

### What we were fundamentally assuming wrong

For most of the day, the working model was still:

- "There is a specific fade effect, fade asset, or fade-named handler."
- "If we corrupt enough things called fade/camera/postfx, we should hit it."

That model is now too weak.

The evidence fits a different model better:

- the blackout is probably **not a named fade effect at all**
- it is more likely a **stateful shared visibility gate or scene
  ownership transition**
- the gate acts **before main view and mirror split**
- the explicit fade/camera/UI layers we can see are only consumers or
  decorations around that state

That explains why:
- HUD survives
- mirror is the best blackout oracle
- the main view can be made black/dark/broken without touching the real
  blackout timing
- every declarative fade family keeps missing

### What is actually missing from the investigation

We have traced:
- assets by name
- UI control files by name
- strings in the binary by name
- late render dependencies by image address

What we have **not** traced is the thing that matters most:

- the **actual runtime state transition** that decides whether gameplay
  cameras are allowed to present the world yet

Concretely, the missing classes of probe are:

1. **Live dispatch/call tracing, not string matching.**
   We need the real callsites/handlers executing during:
   - car select confirm
   - race load begin
   - blackout start
   - blackout end / mirror recovery
   
   Names like `CameraFade` and `ScreenFadeOut` were not enough.

2. **Shared pre-split camera/view ownership state.**
   Anything that governs:
   - prerace camera active vs gameplay camera active
   - visibility ownership / handoff / "ready" state
   - gating of scene presentation to mirror and main view together

3. **Dynamic dispatch targets or message IDs, not the static strings
   that happen to sit nearby in `.rodata`.**
   The day repeatedly showed that patching visible strings only hits
   adjacent registration/config plumbing.

### Best next direction from this floor

No more blind asset shotguns and no more string-token corruption.

The next step should be a **runtime trace** of the actual prerace →
gameplay handoff dispatch:
- capture the live functions/message IDs/callbacks that fire between
  car select and blackout start
- capture the matching ones that fire when blackout lifts / mirror
  recovers
- patch only those exact live dispatches once identified

If we cannot trace at that level, then we are still guessing around the
edges, no matter how broad the shotgun is.

### New working hypothesis (2026-04-22)

The blackout is probably **not a fade effect** in the ordinary sense.

It is more likely a **shared state-machine gate** that controls when the
gameplay world is allowed to present after the prerace flow hands off to
the real race. The visible fade-like behavior is only how that gate
manifests on screen.

That model fits all the clean misses:
- UI fades do nothing
- explicit `ScreenFadeOut` handling does nothing
- local track/postfx changes only alter the image under blackout
- late render/composite trees only alter the carrier image
- mirror remains the best oracle for the real gate

The likely structure is:
1. menu / vehicle-select controller state
2. prerace panorama / confirmation state
3. race-load state
4. gameplay world loaded but presentation still gated
5. shared gate opens
6. main view and mirror both recover

### Exact user-observed runtime sequence to trace

This is the sequence that must be traced in code, not inferred from
asset names:

1. **Car select**
2. **Paint selection**
3. **Confirm**
4. **Brief world panorama with "start event" confirmation**
5. **Race loads**
6. **Cars visible on starting grid**
7. **Blackout fades in**
8. **After a delay, blackout fades out**
9. **Mirror comes back with the world**

Important consequence:
- the blackout occurs **after** the race world is already present
- it is therefore not just "loading screen still visible"
- and not just "wrong prerace image"

### Canonical baseline timeline (2026-04-22, user verbatim)

Exact step-by-step of what the **unpatched** game does at race start,
as observed in repeatable repro at India tracks, 19:30 clear:

1. Car selection menu.
2. Paint selection.
3. Brief world panorama animates and presents a "start event"
   confirmation.
4. User presses X.
5. Race loads and prepares.
6. **For ~1 second the track is fully visible** — cars on grid
   waiting for the green light, mirror working, scene in colour.
7. **Blackout fades in**, darkens the whole scene to almost black.
   Cars are **still faintly visible underneath**, especially the red
   tail lights moving. Mirror goes **pure black** (no structure).
8. Wait 10–30 s (variable, sometimes never recovers).
9. Blackout fades out, mirror re-populates with the world, scene
   lifts back to colour.
10. Gameplay is responsive.

Three details in step 7 are load-bearing:
- main view is **dim but not zero** — HDR content like tail lights
  survives the darkening, so the main scene render pass is still
  executing and writing real values
- mirror is **actually zero** — not dim, not faint, no shape visible
  at all
- HUD and minimap are **always fully intact** at their normal
  brightness and colour, painted on top of whatever the scene layer
  is doing. They are not gated by the blackout at all.

That is not "exposure is low" alone. A single low-exposure multiplier
applied to both the main HDR target and the mirror target would leave
**both** with faint structure, not one dim and one black. The mirror
being cliff-edge black while the main view is dim-but-visible means
**two different mechanisms are in play simultaneously**:

- main view: a multiplicative darkening, either by exposure value or
  by a near-opaque black composite that still lets bright HDR pixels
  burn through
- mirror: the mirror *render pass itself* is suppressed, or its
  output is overwritten with zero, or its source camera is disabled

And the HUD/minimap being unaffected in the same frames means a third
fact: **the gate does not gate the UI/HUD composite path at all**.
The final frame is clearly a composite of:

- a 3D world layer that is currently darkened
- a mirror reflection layer that is currently zeroed
- a HUD/minimap layer that is currently full-normal

All three happen in the same frame. Only the first two are gated.

That three-way split — main dim, mirror zero, HUD fine — is the
signature of a **shared state-machine gate** governing the 3D world
presentation subsystem (codex's 2026-04-22 hypothesis), not of any
single named fade effect and not of the emulator's final present.

### Comparison of the two working models

Codex's gate model and the earlier init-delay hypothesis can both be
held against this timeline:

| Model | Predicts step 6 (track visible) | Predicts step 7 (main dim + mirror black) | Predicts step 9 (shared recovery) | Verdict |
|---|---|---|---|---|
| **Codex state-machine gate** | World is renderable, not yet gate-allowed to be *fully* presented | Gate closes: suppresses mirror pass entirely, multiplicatively darkens main | Gate opens, both subsystems re-enable together | Fits cleanly |
| **Init-delay / unconverged scene** | World not yet rendered; no track visible on frame 1 | N/A — would predict dim *before* cars appear, not *after* | N/A — init has no "close and re-open" motion | Rejected by step 6 |

The init model is now out. The cars-visible-then-darken sequence is
not what an init delay looks like. The gate model is what remains.

Refinement on the gate model from this observation:

- the gate is not a single visual effect; it's **one signal driving
  at least two subsystems simultaneously** (main-view darken +
  mirror suppress).
- whatever the game code checks to decide "gate open" has to be the
  same flag read by both the mirror pipeline and the main-view
  tonemap / composite path.
- that shared flag is the real target. Binary-patching it to always
  read "open" is what would actually fix the blackout, independent
  of any visual-layer patches.

### Why 3 days of patches keep missing

Everything we patched so far lived **past** the gate:

- UI panels → consumer of the gate's presentation permission
- PostFXConfig scalars → tuning of the already-permitted pipeline
- LUT swaps → colour mapping inside the already-permitted pipeline
- prerace camera Fade tracks → animation driven at the gate-closed end
- TXAA / NonBloomed → decay shape of the overlay, not its source
- ScreenFadeOut NOP, CameraFade string corruption → decorations

None of these can reach a flag that is evaluated before the main
render subsystems even decide to run. We were dialling parameters on
systems the gate was actively holding shut.

Every "we changed how something looks" success reinforces the gate
model: whatever the gate gates, we're allowed to tune *after* it
decides to open. Before it decides, we can change nothing visible,
because the gate won't let us.

### Concrete shape of the next probe

Codex already has this in the later sections: runtime dispatch trace
instead of more declarative shotguns. This observation sharpens what
to look for:

- **one shared flag-read** that fires both at step 7 (gate close) and
  at step 8 (gate open), checked simultaneously by
  - the mirror render pass (enabled / skipped)
  - the main-view scene darkening (exposure driven low or full-screen
    composite darkening applied)
- the HUD/minimap path does not read that flag, which is how we'll
  identify the right code region: any trace where the HUD-composite
  path moves in lockstep with the darkening is reading the wrong flag
- the shared site is almost certainly a class accessor like
  `GameWorld::IsPresentationReady()` / `Scene::IsRenderableToUser()`
  or a specific handshake flag on the race-state controller that
  gates the 3D-world subsystem only
- once identified, a one-byte patch forcing it to return true unlocks
  both the main-view darkening and the mirror simultaneously while
  leaving HUD unchanged — that triple signature is the verification
  that we've patched the right thing

Worth adding to the resume point: any runtime trace must capture
mirror-pass enable/disable toggles and main-view exposure or final
composite branch around steps 6 → 7 → 8 **with HUD as a null control
channel**. If a candidate flag also changes HUD rendering, it's not
the gate — the gate leaves HUD untouched.

### Merged assessment after reviewing both hypotheses

Claude's addition contributes one important refinement that should be
treated as load-bearing:

- the world is already visible on the starting grid **before** the
  blackout closes

That observation decisively rejects any remaining "scene not ready yet"
or "late initialization" model. The blackout is not the absence of a
finished frame. It is a later **close/re-open gate** applied after the
gameplay world is already renderable.

So the merged model is now:

- **not** a fade asset problem
- **not** a late composite problem
- **not** an initialization delay
- **not** a HUD/final-present problem
- **yes** a shared 3D-world presentation gate that:
  - darkens the main view
  - suppresses the mirror completely
  - leaves HUD/minimap untouched

### Better next path from the merged model

Do not trace "fade" names anymore.

Trace the **shared gate reader** instead:

1. Find where the mirror pass is conditionally suppressed during the
   blackout window.
2. Find what main-view darkening branch toggles in the same window.
3. Intersect those two control paths.
4. Reject any candidate that also affects HUD/minimap.

The most valuable runtime markers for the next probe are therefore:
- mirror enable/disable
- main-view darken branch enable/disable
- race-state handoff around panorama -> grid
- no HUD coupling

The best fix target is no longer "a fade effect". It is the first
shared boolean/state read that explains:
- main dim
- mirror zero
- HUD untouched
- recovery of both main+mirror later

### What the next trace must answer

The next runtime probe must identify:
- the controller/page/state transition from step 4 to step 5
- the state transition that activates blackout at step 7
- the state transition that re-enables world presentation at step 8/9
- whether main view and mirror re-enable off the same gate or sibling
  gates

The next step should therefore be a **sequence tracer**, not another
fade shotgun:
- log the live handoff dispatches/callbacks/messages around this exact
  sequence
- then patch the exact live branch once identified

2. Load the carved ELF:

   ```
   /mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.et_exec.elf
   ```

   Import as x86-64, no section headers. Ghidra's autoanalysis will
   walk function bodies via the program headers.

3. Jump to the four anchor points already located this session:

   | Anchor | VA | What it is |
   |---|---|---|
   | `FreeplayGetInCar` class name string | `0x12c485d` | |
   | `FreeplayGetInCarPage` page name string | `0x12c4848` | |
   | RTTI name-getter returning class name | `0xf89b60` | |
   | Class constructor (sets vtable pointer) | `0xf89b00` | |
   | Class vtable (unresolved in static file) | `0x15dc3e0` | |

4. In Ghidra, cross-reference from the string VAs. The
   constructor at `0xf89b00` writes
   `*(this) = &vtable[0] (VA 0x15dc3f0)`. Once Ghidra applies its
   runtime relocation analysis, the vtable's function pointers
   (`vfn[0..N]`) will display actual addresses instead of the
   `0x000000070000001X` relocation placeholders we see in raw
   bytes.

5. Identify the render / draw slot. Expected names in the UI
   controller base class: `Render`, `OnRender`, `Draw`, `OnDraw`,
   `UpdateAndRender`. The dispatch almost certainly calls into
   GNM (sceGnm*) draw-state setup. Confirm by checking the render
   method calls into known graphics-driver import stubs.

6. Apply the patch:

   ```
   # Break the overlay symlink and replace with a patched copy.
   cp /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
      /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin.vanilla
   rm /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   cp /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
      /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ```

   Byte-patch the overlay eboot (not the `.vanilla` backup). First
   patch to try: replace the first byte of the identified render
   method with `0xC3` (`ret`). Remember the eboot has an OELF
   wrapper: the 288-byte header means file offset of the patch
   inside `eboot.bin` is `inner_ELF_file_offset + 288`.

7. Test:

   ```
   /mnt/data/Projects/shadPS4/scripts/stop_driveclub_live.sh
   /mnt/data/Projects/shadPS4/scripts/run_driveclub_overlay.sh
   ```

   Load Munnar at 19:30 clear, observe whether the static-car
   overlay disappears while HUD / race flow stay intact.

8. Rollback if anything goes wrong:

   ```
   rm /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ln -s /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
         /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ```

### What not to waste time on

These have all been tried and produced clean misses. Do not retest
without a new hypothesis.

- Host gamma / `sceVideoOutAdjustColor` hook (Phase 4).
- Forced `Finish()` at race-start window (Phase 4).
- Any texture-null torture on sampled inputs during race draws
  (Phase 7).
- Any `newui/panels/*.txt` single-panel surgery: loading screens,
  backgrounds, pause, vehicle_select_background, get_in_car_animation
  (Phase 8).
- Removing the `getincar.freeplay` Page entry from `freeplay.ctl`
  (Phase 8 — crashes).
- `SHADPS4_DC_TORTURE` env var enabled — sampled-input hypothesis
  closed.

### Exit memory

Remember the durable rule (see global memory):
`feedback_never_accept_as_is` — there is no "accept it" option. The
race-start overlay is a bug we are working to kill, not a feature
to live with. Binary-patching the eboot is the next hypothesis; if
it fails, the next one will be found from whatever that failure
teaches us.

## Phase 10 — video diagnostic pins the blackout to an animlib curve

### The video and what it measured

OBS recording `/home/akitaonrails/Videos/OBS/2026-04-22 11-48-45.mkv`,
1920×1080, h264, 12 seconds, covering the Munnar 19:30 race-load
window. User confirmed no controller input and no R1 press during
recording — every visual change is the game's own scripted behaviour.

Sampled at 10 Hz for frame-level luminance analysis, the blackout
sequence is four **discrete brightness plateaus** with one-frame
transitions between them, on a camera that stays dead-fixed through
the whole window:

| moment | centre luminance | interpretation |
|---|---|---|
| t=0 race-loaded, no HUD | 41 | clean scene visible |
| t=3.5 s blackout enters | 1.6 | near-pitch black |
| t=3.8 s automatic lift | 42 | HUD active, scene barely visible |
| t=5.5 s dark again | 1.2 | ~0.5 % of full |
| t=6.3 s shallow lift | 9.0 | intermediate |
| t=8.9 s last dark frame | 8.9 | steady state |
| t=9.0 s gate opens | 95 | full scene, camera also changed |

Camera structural diff (brightness-normalised): every dark/lift frame
from t=2.5 s to t=8.9 s is structurally identical (pixel-diff 0.44–6.18
out of 255). The t=0 clean scene and the t=9.0 s recovered scene are
both structurally *different* from the blackout window (diff ≈ 65 and
≈ 95 respectively), confirming three distinct camera positions:

1. Position A — t=0, race loaded, wide track view, HUD not active
2. Position B — t=2.5–8.9 s, prerace cinematic fixed camera, HUD active
3. Position C — t=9.0 s, cockpit camera, race starts

The earlier "R1 briefly lifts the blackout" observation from previous
sessions was correct as an independent signal, but this recording
happened without R1 and still shows the 1.6 → 42 → 1.2 → 9 → 95
staircase. Therefore the staircase is **the game's own scripted fade
curve**, not camera-reactive per-frame logic.

### Keyframe-dump findings (`india_posteffects.lvl` animlib)

Custom float-scanner walks each named track in the animlib and lists
meaningful floats in the bytes following the ASCII name. Abridged
output, both variants (`india_dc_grade_interior` and
`india_dc_grade_exterior`) share the same layout:

```
SunElevation             (all zeros — no keyed animation)
AutoTargetLuminance      +22=1   +30=1
ManualExposureLog2       +21=1   +29=1
ManualAutoMix            +13=85   +21=0.9903  +25=0.1386  +29=0.9903  +33=0.1386
AutoMaxLuminance         +18=300  +22=1
AutoSpeed                +14=180  +18=0.09   +22=1
MasterBrightness         +17=0.003  +21=1  +25=0.0007  +29=1  +32=1.528e-05
MotionBlurLevel          +18=0.3 (interior) or 0.2 (exterior)
ColourRemapVolumeName    → night LUT → day LUT fallback (already patched)
```

Smoking gun: `MasterBrightness` has two dim-side keyframe values —
`0.003` and `0.0007` — and two bright-side values at `1.0`. The
product `scene_luminance × 0.0007` puts a normal scene at 0.07 %
brightness, which is exactly the 1.6 / 255 = 0.63 % centre luminance
we measured at the deepest blackout. `0.003` corresponds to the
shallower dim (centre ≈ 8.9 / 255 = 3.5 %). Both values match the
plateaus the video recorded.

The 1.0 values are the recovered-brightness keyframes. The
1.528e-05 value is small enough that it's almost certainly a slope /
tangent, not a brightness value; it's left alone.

### Inline defaults vs animlib — where the override comes from

`PostFXConfig_{interior,exterior,helmet}` actors carry an inline
`MasterBrightness = 0.003` baseline. Our Phase 8 patcher set those to
1.0. That had no visible effect **because the animationlib's
keyframe curve runs every frame and rewrites the live exposure value
from the curve back to 0.003 → 0.0007 → 1.0**. The inline value only
matters if nothing is driving the curve, and the curve is always
driven during prerace.

This retroactively explains every iteration 2.x and 3.x clean miss:
we were dialling downstream PostFX and UI panels while the scripted
animlib curve kept reasserting the dim values from above.

### Safer patcher — `PatchAnimlibFloatValues`

New helper in `tools/driveclub_asset_patch/Program.cs`. For each named
track, scans up to N bytes following the name; for each `(oldValue,
newValue)` replacement pair it finds the first 4-byte window that
reads `oldValue` (within 0.5 % tolerance, floored at 1e-7), and
overwrites it with `newValue`. Every other byte — header fields,
timing bytes, slopes, adjacent tracks — stays untouched. This avoids
the April-20 `PatchNamedTrackKeyframes` failure mode where blind
offset sweeps corrupted record strides and crashed the game's parser.

First live call on `india_posteffects.lvl`:

```
animations.MasterBrightness.curve[1] MasterBrightness+29: 0.003  -> 1
animations.MasterBrightness.curve[1] MasterBrightness+37: 0.0007 -> 1
animations.MasterBrightness.curve[2] MasterBrightness+29: 0.003  -> 1
animations.MasterBrightness.curve[2] MasterBrightness+37: 0.0007 -> 1
```

Two occurrences (interior + exterior grade), two dim keyframe values
each, four writes total per track pack. Applied to both Munnar
(`india_road_point_02_n.rpk`) and India reverse circuit
(`india_road_circuit_01_r.rpk`).

### What this is expected to change

If `MasterBrightness` is the only animated scalar driving the dim,
the blackout staircase should flatten — either disappear entirely or
persist only through a second, smaller-amplitude scalar (e.g. the
`ManualAutoMix` 0.1386 / 0.9903 oscillation, which we deliberately
did NOT touch because auto-vs-manual mixing has weirder failure
modes). A measurable outcome:

- success case: centre luminance during prerace hovers around 95/255
  (the recovered-scene level), from t=2.5 s onward
- partial case: centre luminance climbs from 1.6 to maybe 40–60 but
  still dips during some prerace frames
- null case: centre still dips to 1.6 — means a different scalar is
  the driver; `ManualAutoMix` next, then `AutoMaxLuminance`,
  `AutoTargetLuminance`

### Probe direction if this run still shows a dip

The scan output lists every candidate scalar worth touching next, in
priority order:

1. `ManualAutoMix` — values 0.9903 and 0.1386 are the oscillation.
   Try rewriting both to a constant (either 0 for full auto or 1 for
   full manual) to see which breaks the blackout. Risk: changing the
   mix mid-curve may interact badly with AutoMaxLuminance.
2. `ManualExposureLog2` — currently reads 1 / 0 / 1 in the scan.
   Worth re-scanning with lower threshold to see if there's a
   negative-log dim value like -4 hidden in it.
3. `AutoTargetLuminance` — scan showed only 0 and 1 but a finer
   threshold may reveal a mid-range target.

Every new candidate goes through the same value-matched safe patcher
— never the blind-offset sweep.

## Phase 11 — Iteration 11 combined multi-scalar patch, and the vanilla decider

### Iteration 11 — patch

Extended the `PatchAnimlibFloatValues` call list on the
`india_posteffects.lvl` animlib to cover three brightness-related
scalars at once instead of just `MasterBrightness`:

```
MasterBrightness     0.003  -> 1.0
MasterBrightness     0.0007 -> 1.0
ManualAutoMix        0.99034 -> 1.0  (force all-manual exposure)
ManualAutoMix        0.13865 -> 1.0
AutoTargetLuminance  0.38   -> 1.0  (target full brightness)
```

Ten writes per track-pack (2 grade variants × the replacements
above). Applied to both Munnar (`india_road_point_02_n.rpk`) and
India reverse (`india_road_circuit_01_r.rpk`). `ManualExposureLog2`
was deliberately left alone — its positive-log values (`5.8`, `4.5`)
are brightening keyframes, not dim values.

### User-visual report

"didn't see any difference from baseline. blackout is still there."

No recording of Iteration 11 was captured on that run, so only
subjective data for this patch combination — but we returned to the
question "are we on the right track?" and ran a decider instead of
iterating further.

### Decider — pure-vanilla comparison

Moved all 60 patched files out of
`tmp/driveclub_overlay/CUSA00003/` into a sibling backup tree and
symlinked every slot back to the pristine install. Launched with
**zero** Driveclub-local patches, only the emulator-side changes
that are compiled into the binary (MSAA depth resolve, Bayer dither,
the `[dc-timeline]` heartbeat, `SHADPS4_DC_TORTURE` which stays
dormant). User recorded Munnar 19:30, 22-second clip,
`2026-04-22 12-31-53.mkv`.

Same frame-extraction protocol as Run 2 (right-half crop, 2 Hz
sampling). Measurements:

| moment | vanilla centre | vanilla mirror | Run 2 centre | Run 2 mirror |
|---|---|---|---|---|
| t=0.0 s (clean start) | 27 | **38** | 88 | 3.5 |
| t=3.0 s | 12 | 16 | 42 | 3.5 |
| t=5.5 s | **1.23** | **1.36** | 6.6 | 3.5 |
| t=6.0–9.0 s (deep blackout plateau) | **1.22–1.23** stable | **1.23** stable | 6.5 | 3.5 stuck |
| t=9.5 s (recovery) | 76 | **72** | 83 | 3.5 stuck |

### What the numbers definitively say

**1. The animlib-scalar strategy is real but bounded.**

Vanilla deep-blackout centre: 1.22/255 = 0.48 %. Run 2 deep-blackout
centre: 6.5/255 = 2.5 %. That is a real 5× numeric improvement from
the MasterBrightness patch — **but perceptually 0.5 % and 2.5 % both
read as "black"**. The patch does what the math predicts; the math
just doesn't predict enough amplitude.

**2. MasterBrightness×everything-else ≠ observed values.**

If the 0.0007 MasterBrightness keyframe were the only multiplier
acting on the scene, our patch lifting it to 1.0 should have
produced ~1428× brighter output. We see 5×. The remaining ~285×
attenuation lives downstream of the animlib scalars we've been
patching — in the tonemap, in HDR→SDR compression, in a clamped
exposure stage, or in a scene-render decision that the scalars only
modulate, not gate.

**3. Vanilla mirror works normally — it is not "suppressed".**

In pure vanilla, mirror luminance tracks the scene: 38 at the start,
1.23 at the blackout floor (same value as the scene), 72 at
recovery. The mirror isn't gated off; it's just reflecting a dark
scene and so it reads dark. The "mirror pass is gated" hypothesis
from Phase 10 collapses — the mirror is fine, the scene is dark.

That simplifies the whole model: **there is one blackout driver, not
two**. Whatever multiplier attenuates the main view also attenuates
the mirror because the mirror samples the same scene.

**4. Our 60 accumulated patches broke mirror recovery.**

Run 2 mirror is stuck at 3.5 for the entire 10-second clip — it
never recovers to the ~72 that vanilla shows. Somewhere in our 60
patched files, one patch is gating the mirror's reflection
rendering. It's not the animlib keyframes we know about (they were
tested in isolation in Run 2 and 3); it's most likely in the UI
panel disables or in one of the other animlib resources we touched.

### What stays and what changes in the working model

Stays:
- There is a blackout state with measurable discrete dim plateaus.
- It is game-scripted, happens on a fixed prerace camera, ends on
  camera handoff to cockpit.
- The `MasterBrightness` / `ManualAutoMix` / `AutoTargetLuminance`
  animlib keyframes *are* real inputs to scene darkness.

Changes:
- The real blackout amplitude is set downstream of these scalars, not
  by them. Further scalar-hunting will produce tiny numerical wins
  (maybe 6.5 → 12 → 20) but no visible change until we find the
  downstream multiplier.
- The "mirror pass is gated separately from main view" model is out.
  One darkening path, both subsystems affected identically.
- One of our 60 track-pack or UI-panel patches is actively gating
  the mirror pipeline in the patched state. That patch is an
  unintended lever on the blackout subsystem and is our best
  forward-handle.

### Strong new lead — bisect the 60 patches

A bisect of the 60 accumulated patches to find which one gates the
mirror is the cheapest next diagnostic:

- Split the 60 into 30 + 30.
- Run with only the first half patched; check mirror behaviour in
  vanilla baseline video (just eyeball at recovery — is mirror ≈ 72
  or ≈ 3?).
- If mirror is broken → culprit in first half. If working → in
  second half.
- Four binary bisections → 60 patches narrowed to 1 specific patch.

Whichever patch gates the mirror is almost certainly also adjacent
to the real blackout-driving code. That gives us a pointer into the
render-state subsystem we've been unable to reach from the scalar
side.

### Recommended next session

1. **Bisect to find the mirror-gating patch.** Maximum four
   launches. Output: one specific patched file.
2. **Inspect that patch's content** to identify which render
   subsystem or actor it disables. The mirror depends on a
   reflection camera + a scene render pass; the guilty patch is
   interacting with one of those.
3. **Revert that one patch** to see if mirror recovers. That gives
   us a clean "mirror works" baseline to test future scalar patches
   against.
4. **From the guilty patch's identity, map to the real gate.** If
   it's e.g. a fullscreen UI panel whose disable also kills mirror
   reflections, the mirror reflection pass must consume from that
   panel's texture. That's an unexpected render-dependency chain
   worth following.

Continuing to layer more animlib scalar patches without resolving
the mirror regression is expected to produce only imperceptible
numeric changes. The bisect is the next load-bearing diagnostic.

## Operational — pipeline cache corruption after many stop/start cycles

Symptom seen at 2026-04-22 ~12:44 onward, during the Phase 11 bisect
setup:

- Emulator launches but deterministically crashes at Vulkan swapchain
  init before any game code runs. Log ends with:
  ```
  [Render.Vulkan] <Info> vk_swapchain.cpp:81 Create: Swapchain created: ...
  [Debug] <Critical> signals.cpp:96 SignalHandler: Unreachable code!
  Unhandled access violation at code address 0x...: Read from address 0x10
  ```
- Reproduces on both scripted launches and on the user's own terminal.
- Reproduces with a **100 % symlink overlay** (zero patched files).
  That rules out our overlay state as the cause.
- The same binary (built 11:25 today) had been running fine through
  the earlier Munnar recordings at 12:15 and 12:31 on the same
  session.

Root cause: **corrupted persistent pipeline cache**. shadPS4 caches
compiled Vulkan pipelines at
`$XDG_DATA_HOME/shadPS4/cache/CUSA00003/`. Over many launches a cache
entry can end up inconsistent with the live pipeline state, and on
next boot dereferencing it produces the null-deref at swapchain init.

**Fix:**

```sh
CACHE=/mnt/data/distrobox/gaming/.local/share/shadPS4/cache
mv "$CACHE" "${CACHE}.backup-$(date +%s)"
mkdir -p "$CACHE"
```

Next cold launch takes several minutes of shader compilation but
boots cleanly. Confirmed recovered 2026-04-22 12:48 — user launched,
reached main menu, quit cleanly.

This is operational guidance, not investigation evidence. Anyone
else resuming work and seeing a deterministic swapchain-init crash
should wipe the pipeline cache first before debugging anything else.

## Phase 12 — bisect closes the asset-side track

### Bisect result

The "our 60 patches broke the mirror" hypothesis from Phase 11 was
the working lead. A binary bisect of the 60 overlay files narrowed
it to a single culprit in 5 rounds:

| round | patches active | mirror at recovery |
|---|---|---|
| 0 (pure vanilla) | 0 of 60 | **recovers** |
| 1 | 2 `.rpk`s only | **recovers** — UI side is the culprit set |
| 2 | + Group A (7 UI panels) | **recovers** — culprit is in Group B (7 left) |
| 3 | + Group B1 (4 loading panels + pause) | **recovers** — culprit is in B2 (3 left) |
| 4 | + `transitions.ctl` | **recovers** — 2 left |
| 5 | + `x_live_lobby_pre_race.txt` | **STUCK** — this is the one |

The single file that flips the blackout presentation from "dim
vanilla scene" to "latched car image" is
`newui/panels/x_live_lobby_pre_race.txt`. Diff against install is
exactly two lines on the `loading_icon` spinner element:

```
-  ATTRIBUTE("IN_TRANS"  "ZoomInAndFade")
-  ATTRIBUTE("OUT_TRANS" "ZoomOutAndFade")
+  ATTRIBUTE("IN_TRANS"  "NoFade")
+  ATTRIBUTE("OUT_TRANS" "NoFade")
```

Renaming the loading-spinner's transitions from `ZoomInAndFade` to
`NoFade` short-circuits whatever transition driver normally produces
the gradual dim during prerace. The rest of the UI stack freezes
on the last rendered menu frame, which is why we kept seeing the
car-select image during the blackout instead of a dim scene. The
patch was retired (reverted to install symlink, real file moved to
`tmp/driveclub_bisect/retired_patches/`).

### Retracted: the "mirror regression" hypothesis from Phase 11

Phase 11 claimed our patches broke the mirror because Run 2's
mirror stayed at ~3.5 luminance for the full 10.5 s clip. That read
was wrong. Run 2's recording simply wasn't long enough to reach the
mirror recovery point — the blackout plateaus past our clip length
on that run. With a longer observation in today's bisect rounds,
the mirror recovers normally in every patched configuration. The
single-darkening-driver model from Phase 11 stands, but the "one
of our patches gates the mirror separately" subhypothesis doesn't.

### What the bisect proved about the blackout itself

Definitively:

1. The blackout is **entirely game-scripted**. It happens
   identically in the pure-vanilla overlay (0 patched files) —
   confirmed with the 2026-04-22 12:31:53 recording, mirrored by
   the final state after the bisect.
2. None of our 60 accumulated patches lengthen, shorten, or
   eliminate the blackout window. At best one patch changes what's
   *visible* during the window; the window itself is untouchable
   from the asset side.
3. The animlib-scalar work from Phase 10 / 11 produces a real 5×
   numeric lift at the deepest plateau (1.2 → 6.5 / 255) but stays
   sub-perceptual, consistent with ~280× of further attenuation
   happening downstream of anything we can reach from the animlib.

### What's retired after Phase 12

Out of the 60 overlay files the bisect walked through:

- **Retired (actively harmful):** `x_live_lobby_pre_race.txt` —
  visual regression, reverted to install.
- **Kept (benign, no visible effect but harmless):** the 2
  patched `.rpk`s + 13 remaining UI panel patches + `transitions.ctl` +
  `x_freeplay.txt`. None of these do anything perceptible; they
  also don't actively break anything. Kept because removing them
  risks knocking another file into a broken state we don't
  understand yet.

### Conclusion of the asset-side era

The entire asset-side investigation (Phases 8–12) is now closed as
a productive direction for reducing the blackout. The driver is in
compiled code or in shader / post-process state set from compiled
code. Three viable paths remain:

1. **Per-draw render trace** — env-var gated log in
   `vk_rasterizer.cpp` that dumps every draw's
   `(frame, render_target, pipeline_hash)` for a windowed time
   around race-start. Compare dim-plateau frames vs
   recovered-frame draws to identify which specific pipeline /
   shader is responsible for the dim multiplier. Cheapest
   implementation; leverages the existing `[dc-timeline]`
   heartbeat infra.
2. **Shader uniform / push-constant dump** — similar instrumentation
   but captures the actual float values pushed to the GPU each
   frame. Would directly show the dim scalar changing over time,
   without guessing.
3. **Eboot binary patching** — Phase 9 plan is still documented
   with anchor VAs and OELF-carving mechanics ready. Needs Ghidra
   on-hand and multi-hour reverse-engineering.

Path 1 is the right next move. It's incremental from the Phase 10
heartbeat work and produces objective data about what the GPU is
actually doing during the blackout window, which no amount of
additional asset-side surgery could give us.

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

## Clean misses after the fixed race gate

With the hardened race-window guard in place, the following additional
families were probed and still did **not** materially disturb the
blackout itself:

1. Scheduler / timeline slowdown family
   - forcing `Finish()` around the race-start window made the game slow
     but left the blackout unchanged
   - this rules out the coarse "GPU is simply finishing too late"
     timing theory
2. `sceVideoOutAdjustColor` / host gamma family
   - stubbing out the gamma write and forcing presenter gamma to stay
     at `1.0` did not move the blackout
3. Host final-output family
   - bypassing host FSR and host post-process and presenting the raw
     guest video-out image still left the blackout intact
   - this is a clean miss against the "final overlay / final postfx"
     theory
4. Video-out label / flip-wait family
   - forcing VO labels ready and bypassing the VO wait path during the
     race window fell back to baseline behavior with the blackout still
     intact

Current clean takeaway:

- the blackout is upstream of host present
- it is not explained by `sceVideoOutAdjustColor`
- it is not explained by coarse GPU slowdown / forced finish
- it is not explained by the VO label / flip wait path

That means the remaining lead should move away from final output and
toward a game-side camera / postfx / prerace system or another shared
upstream branch.

## Manual asset sweep: strongest new lead

A broad string sweep across the installed v1.28 game files found
something much more plausible than the previous renderer-side guesses:

- the actual race track packs contain explicit prerace camera actors
- those same packs contain explicit postfx config actors and exposure
  fields
- global data contains generic fade/postfx override tracks

This is the most important cluster found in the asset files so far.

### Evidence in track packs

The actual track `.rpk` files under `data/leveldata/` contain names such
as:

- `new_preracecam_*`
- `preracecam_*`
- `cutsccam`
- `worldcam`
- `pre_race_*`
- `track_preview`
- `PostFXConfig_interior`
- `PostFXConfig_exterior`
- `PostFXConfig_helmet`

They also contain explicit camera/postfx scalar names embedded directly
in the binary data:

- `ManualExposureLog2`
- `AutoTargetLuminance`
- `ManualAutoMix`
- `AutoMaxLuminance`
- `AutoSpeed`
- `MasterBrightness`
- `TemporalFade`
- `ColourRemapVolumeName`
- `FocusDistance2`
- `ApertureFNumber`
- `ShutterSpeed`
- `DepthOfField`
- `ShowDOFLerp`

This is not generic UI text. These strings live inside the race-track
resource packs themselves, next to the prerace camera actor data.

### Evidence in global asset packs and UI

`data/leveldata/globaldata.rpk` contains generic fade/postfx names such
as:

- `FadeIn`
- `FadeOut`
- `FadeIn_Fast`
- `FadeOut_Fast`
- `RTT_BLUR_ON`
- `RTT_BLUR_OFF`
- `RTT_GRADING_ON`
- `RTT_GRADING_OFF`
- `PostFX_ColourGradingOverride`
- `PostFX_BloomOverride_*`

The UI tree also contains ordinary page transitions like `BootFade` and
`FadeOut`, but those are almost certainly just menu/UI transitions and
should not be treated as evidence of the race blackout by themselves.

### Why this matters

This asset-side evidence fits the observed behavior better than the
previous renderer-side families:

- the blackout begins at race start rather than at final present
- mirror and main camera both seem to obey the same visibility timing
- the duration is variable, which fits prerace-camera / postfx / track
  state better than a simple hardcoded final overlay
- the track packs explicitly define prerace cameras and postfx controls
  in the same resource neighborhood

Current best hypothesis:

- the blackout is more likely tied to a **prerace camera / cutscene /
  PostFXConfig system** than to the final presenter or the visible
  race-scene color chain itself
- likely suspects inside that family are `TemporalFade`,
  `MasterBrightness`, luminance/exposure controls, colour-remap
  volumes, or a prerace-camera handoff that temporarily drives those

## Munnar track-pack findings

Primary repro track:

- India `Munnar`, pack `india_road_point_02_n`
- user repro settings: `19:30`, clear weather

The Munnar pack is the first asset-side target that produced a stable,
repeatable change in blackout behavior without breaking boot.

### What is inside `india_road_point_02_n.rpk`

The Munnar probe found these relevant resource families:

- prerace camera level data:
  - `india_road_point_02_n_prerace_cams`
  - `preracecam_*`
  - `new_preracecam_*`
  - `track_preview`
  - `WorldCamera_*`
  - `CutsceneCamera_*`
- postfx level data:
  - `india_posteffects`
  - `PostFXConfig_exterior`
  - `PostFXConfig_interior`
  - `PostFXConfig_helmet`
- animation libraries:
  - prerace camera animationlib from `india_road_point_02_n_prerace_cams.lvl`
  - postfx animationlib from `india_common/india_posteffects.lvl`

Most important new finding:

- the prerace animationlib contains explicit sequences
  `prerace_01` through `prerace_08`
- those sequences carry animated `Fade` tracks with keyed values
  ramping to `1.0`
- this is the first concrete track-local blackout-style animation found
  in the game assets

Additional narrowing from the probe:

- `india_posteffects` depends only on:
  - `PostFXConfig_exterior`
  - `PostFXConfig_interior`
  - `PostFXConfig_helmet`
  - one local animationlib (`0x0046EAB21E3CF9E7`)
- the local postfx animationlib contains grading/exposure tracks such
  as:
  - `india_dc_grade_exterior`
  - `india_dc_grade_interior`
  - `SunElevation`
  - `AutoTargetLuminance`
  - `ManualExposureLog2`
  - `ManualAutoMix`
  - `AutoMaxLuminance`
  - `AutoSpeed`
  - `MasterBrightness`
  - `MotionBlurLevel`
  - `ColourRemapVolumeName`
- this means the Munnar postfx branch is largely self-contained inside
  the track pack; it does **not** obviously bind to `globaldata`
  `FadeIn/FadeOut` sequences through the `india_posteffects` resource
  itself

### What was patched in the stable Munnar tests

The track-local patch set that boots reliably included:

- all actor `Fade` values zeroed for:
  - `preracecam_*`
  - `new_preracecam_*`
  - `track_preview`
  - `WorldCamera_*`
  - `CutsceneCamera_*`
- all `PostFXConfig_*` inline `TemporalFade = 0`
- several other inline postfx scalar neutralizations
- India night LUT references swapped to day LUTs
- the prerace animationlib `Fade` tracks zeroed directly

### What that changed at runtime

This did **not** remove the blackout itself.

What it changed reliably:

- the image *under* the blackout stopped looking like the normal race
  frame and instead latched the prior car / `Press X to start event`
  screen
- HUD stayed alive
- game audio continued
- mirror still went black during the blackout interval

Current stable conclusion for the Munnar pack:

- `india_road_point_02_n.rpk` controls what image gets latched or
  persists under the blackout
- it does **not** appear to own the blackout master switch itself
- zeroing track-local camera fades and even the explicit prerace
  animation `Fade` tracks is not enough to disable the blackout
- the local postfx animationlib looks responsible for track-specific
  exposure / grading, but not for the blackout master switch

This is an important split:

- track-local prerace assets affect the visible content under blackout
- the blackout timing/switch likely lives in a shared layer above or
  alongside the track pack

## `globaldata.rpk` findings

`globaldata.rpk` is the strongest shared asset candidate found so far,
but it is too fragile to brute-patch broadly.

### What is inside `globaldata.rpk`

Direct string and probe inspection found:

- shared transition tracks:
  - `FadeIn`
  - `FadeOut`
  - `FadeIn_Fast`
  - `FadeOut_Fast`
- shared postfx transition tracks:
  - `RTT_BLUR_ON`
  - `RTT_BLUR_OFF`
  - `RTT_GRADING_ON`
  - `RTT_GRADING_OFF`
- shared override actors:
  - `PostFX_BloomOverride_*`
  - `PostFX_ColourGradingOverride`
- shared cutscene cameras

This is exactly the kind of shared layer that could sit above both the
main race view and the mirror/prerace handoff.

Important limit from the track probe:

- the `globaldata` string found inside `india_road_point_02_n.rpk`
  appears in generic asset-path/provenance text alongside other pack
  references such as `worlds/india_common.rpk`
- it is **not** evidence that Munnar's `india_posteffects` directly
  references `globaldata` fade sequences at runtime

### What was tried

Two classes of `globaldata` patches were attempted:

1. broad shared patch
   - touched fade sequences
   - touched RTT blur/grading override tracks
   - touched shared override actors
2. fade-only patch
   - touched only:
     - `FadeIn`
     - `FadeOut`
     - `FadeIn_Fast`
     - `FadeOut_Fast`

### What happened

Both classes were unstable at boot.

Important correction discovered during testing:

- one boot crash was caused by the overlay accidentally missing
  `globaldata.rpk` entirely
- after restoring the original file, the broad and fade-only
  `globaldata` patches were still unstable enough that they are not
  trustworthy runtime shotguns

Current conclusion for `globaldata.rpk`:

- it remains a plausible shared blackout layer
- but it is **not** a safe brute-force patch target
- broad or direct sequence edits there should be treated as fragile and
  analysis-only until a much more surgical patching method exists

## Asset-side takeaways so far

What is now load-bearing:

- track-local prerace/postfx assets are real and relevant
- Munnar-local patches change the image latched under blackout
- shared `globaldata` fade/transition content is a plausible master
  layer

What is now ruled out or deprioritized:

- the idea that the blackout is purely in host present, host gamma, or
  final output
- the idea that track-local camera `Fade` alone owns the blackout
- brute-force editing of `globaldata.rpk` as a safe runtime test path

Recommended direction from this point:

- keep `globaldata` at baseline while testing
- use track-local packs like Munnar to understand what content is
  carried under blackout
- treat `globaldata` as a shared transition/analysis target, not as a
  broad shotgun target
  values

## India first-track deep dive

The first India reverse track (`india_road_circuit_01_r.rpk`) is now the
best concrete asset-side target because it matches the user's most
reliable reproduction: long blackout at `19:30`, clear weather.

### Resource split

Using a local probe tool against
`data/leveldata/india_road_circuit_01_r.rpk`:

- `india_posteffects | RTUID_LEVEL_DATA`
  - depends on:
    - `EVO+LEVEL+ACTORPostFXConfig_interior`
    - `EVO+LEVEL+ACTORPostFXConfig_exterior`
    - `EVO+LEVEL+ACTORPostFXConfig_helmet`
    - `animations | RTUID_ANIMATIONLIB`
- `india_road_circuit_01_n_prerace_cams | RTUID_LEVEL_DATA`
  - depends on:
    - older `EVO+LEVEL+ACTORpreracecam_*`
    - newer `EVO+LEVEL+ACTORnew_preracecam_*`
    - `animations | RTUID_ANIMATIONLIB`
    - `sequences | RTUID_ANIMATIONLIB`

This gives a clean family split:

- **prerace camera system**
- **postfx/exposure system**
- **animation libraries that can override both**

### Older and newer camera actors

The older `preracecam_*` actors identify themselves as `worldcam`.
They expose inline scalar fields such as:

- `VFoV`
- `Fade`
- `LookAt`
- `LookAtOffset_X`
- `LookAtOffset_Y`
- `LookAtRoll`
- `RelativeTo`
- `RelativeToOffset`
- `FocusDistance`
- `DepthOfField`
- `ShutterSpeed`
- `ShakeEnabled`
- `ShakeIntensity`

The newer `new_preracecam_*` actors identify themselves as `cutsccam`.
They expose:

- `VFoV`
- `Fade`
- `LookAt`
- `LookAtOffset_X`
- `LookAtOffset_Y`
- `LookAtRoll`
- `Pivot`
- `CameraInPivotSpace`
- `FocusDistance2`
- `ApertureFNumber`
- `EnableNewDOF`
- `AutoFocus`
- `ShutterSpeed`
- `ShakeEnabled`
- `ShakeIntensity`

The same inline `Fade` slot also exists on the India
`CutsceneCamera_*` actor.

### PostFXConfig actor fields are inline and patchable

The `PostFXConfig_interior`, `PostFXConfig_exterior`, and
`PostFXConfig_helmet` actor resources all share the same inline scalar
layout. The relevant fields are not opaque references; their values sit
directly in the actor blob.

Shared defaults observed in all three actor resources:

- `ManualExposureLog2 = -16`
- `AutoTargetLuminance = 0.3612`
- `ManualAutoMix = 1`
- `AutoMaxLuminance = 300`
- `AutoSpeed = 1`
- `WeatherType = 0`
- `WeatherOverrideMix = 1`
- `MasterBrightness = 0.003`
- `TemporalFade = 1`
- `NonBloomedAttenuation = 1`
- `Enabled = 1`
- `TXAAOverallWeight = 0.7`
- `TXAAColourClamping = 1`
- `MotionBlurLevel = 0.2` exterior/helmet, `0.3` interior

The only obvious string-level difference between the three postfx
actors is the remap cube they point to:

- exterior: `colourcuberemap_india_linear.dds`
- helmet: `colourcuberemap_india_linear.dds`
- interior: `colourcuberemap_india_interior_linear.dds`

This is important because it means `TemporalFade` and the basic exposure
controls can be brute-force patched directly without first solving a
complex parser.

### Animation libraries are likely overriding some darkness state

The India postfx level depends on an `RTUID_ANIMATIONLIB` resource whose
string table contains:

- `india_dc_grade_interior`
- `india_dc_grade_exterior`

and the following animated property names:

- `SunElevation`
- `AutoTargetLuminance`
- `ManualExposureLog2`
- `ManualAutoMix`
- `AutoMaxLuminance`
- `AutoSpeed`
- `MasterBrightness`
- `MotionBlurLevel`
- `ColourRemapVolumeName`

This is a strong hint that the grade/exposure system is driven by a
time-of-day or sun-elevation curve, not only by the inline defaults.

Crucial negative result:

- the postfx animation library does **not** show `TemporalFade`

That creates a useful split:

- likely **blackout transition** controls:
  - prerace/cutscene camera `Fade`
  - postfx `TemporalFade`
- likely **scene remains too dark after fade lifts** controls:
  - `india_dc_grade_exterior`
  - `india_dc_grade_interior`
  - exposure/brightness/remap curves

### Camera animation library also contains Fade tracks

The prerace animation library contains named tracks for both the older
and newer camera families. For the older `prerace_*` tracks, it clearly
contains:

- `Time`
- `Transform`
- `Transition`
- `VFoV`
- `Fade`

The newer `new_preracecam_*` tracks expose rotation/translation/VFoV and
camera-focus fields. The string dump does not clearly show a `Fade`
track for every new camera, but the actor still contains the inline
`Fade` field, so the prerace handoff can still depend on it.

### Practical interpretation

The current best model is now a two-layer one:

1. a prerace/postfx transition gate is temporarily darkening or gating
   visibility (`Fade`, `TemporalFade`)
2. when that gate relaxes, the underlying India exterior grade can still
   make the scene much too dark at the tested evening time

This matches the user's repeated observation that the race can begin
with a true fade-to-black phase and then reveal a scene that is still
wrongly dark underneath.

### First asset-side brute-force patch set

A dedicated local tool (`tools/driveclub_asset_patch`) was added to
generate patched copies of the India track pack without touching the
installed game file.

The first brute-force patch set does all of this at once:

- zero every prerace/cutscene camera `Fade`
- zero every `PostFXConfig_*` `TemporalFade`
- neutralize several inline postfx scalars:
  - `MasterBrightness = 1`
  - `ManualExposureLog2 = 0`
  - `ManualAutoMix = 0`
  - `WeatherOverrideMix = 0`
  - `MotionBlurLevel = 0`
- force the postfx animation library's night LUT references back to the
  day LUTs:
  - `colourcuberemap_india_interior_night_linear.dds` ->
    `colourcuberemap_india_interior_linear.dds`
  - `colourcuberemap_india_night_linear.dds` ->
    `colourcuberemap_india_linear.dds`

This is the first non-renderer brute-force that actually targets
concrete prerace/postfx content associated with the India blackout.

## External tooling / level inspection

There is no evidence yet of a turnkey "open the whole track in Blender
and watch the blackout" path.

What does exist locally:

- `DriveClubFS` can unpack `.ndx + .dat` and extract binary resources,
  XML, and textures from `.rpk`
- the local tool explicitly supports Driveclub `1.28`
- resource types include `RTUID_SCENE`, `RTUID_CAMERA`,
  `RTUID_MATERIAL`, `RTUID_SHADER`, `RTUID_LEVEL_DATA`, and
  `RTUID_GUI_ANIM`
- a local `data/nexus/viewer/config.xml` exists, which suggests
  Evolution had some kind of Nexus viewer workflow

Practical interpretation:

- yes, we can extract track packs and inspect textures / XML / binary
  resources externally
- no, there is not yet a proven generic DCC path in this investigation
  for loading the whole assembled level with live postfx behavior
- the most realistic external next step is to use `DriveClubFS` against
  one track pack and inspect the extracted `PostFXConfig_*`, camera, and
  related XML/bin resources directly

## Probe direction after the asset sweep

Do **not** default back to another generic renderer-side shotgun.

The best next brute-force direction is now the track-side prerace/postfx
family:

1. Extract one race track `.rpk` and inspect the `preracecam` /
   `PostFXConfig_*` resources directly.
2. Add a broad runtime probe aimed at the prerace/postfx handoff rather
   than the final color chain.
3. If a runtime hook is feasible, neutralize the likely scalar family at
   once:
   - `TemporalFade`
   - `MasterBrightness`
   - `ManualExposureLog2`
   - `AutoTargetLuminance`
   - `AutoSpeed`
   - colour-remap / grading overrides

## Post-asset-sweep conclusions

The later Munnar / prerace experiments narrowed several branches to
clean misses.

### `FreeplayGetInCar` / `PreraceWaiting` / `PreraceCountdown` UI path

The prerace UI pages and panel files are **not** the blackout carrier.

What was tried:

- `freeplay.ctl`
  - `getincar.freeplay -> GenericTiled`
  - `getincar.freeplay -> PreraceWaiting`
- `prerace.ctl`
  - `PreraceWaiting -> GenericTiled`
  - `PreraceCountdown -> GenericTiled`
- panel file swaps / visual probes
  - `get_in_car_animation.txt` changed to a loud full-screen magenta
    fill
  - `x_preracewaiting.txt` changed to a loud full-screen magenta fill
  - `x_preracecountdown.txt` changed to a loud full-screen cyan fill

Observed behavior:

- replacing or rerouting `getincar.freeplay` blocked forward progress
  from car select
- replacing the `FreeplayGetInCar` page asset caused a hang
- the magenta `get_in_car_animation` probe only produced a thin line at
  the bottom of the screen
- the cyan `PreraceCountdown` probe briefly appeared as a thin line,
  then vanished
- the actual blackout still happened normally

Conclusion:

- these pages/controllers participate in the transition
- they are **not** the blackout image or blackout switch
- `FreeplayGetInCar` owns an important forward-handoff contract, but
  not the blackout itself

### `india_landscape_gui.rpk`

`india_landscape_gui.rpk` was the last strong local rendered-preview
candidate on the asset side. It turned into a clean miss.

Probe summary:

- local cutscene camera:
  - `EVO+LEVEL+ACTORCutsceneCamera_2386587591`
- local GUI postfx:
  - `EVO+LEVEL+ACTORPostFXConfig_exterior`
  - `EVO+LEVEL+ACTORPostFXConfig_interior`
- shared/global postfx actors embedded in the pack:
  - `EVO+LEVEL+ACTORPostFX_bloom`
  - `EVO+LEVEL+ACTORPostFX_BloomOverride`
  - `EVO+LEVEL+ACTORPostFX_TXAAOverride`

Patch set that was tested together:

- cutscene camera `Fade = 0`
- `PostFXConfig_*` neutralized
- `PostFX_bloom` neutralized
- `PostFX_BloomOverride` neutralized
- `PostFX_TXAAOverride` neutralized

Observed behavior:

- Munnar still behaved like baseline
- blackout timing and recovery were unchanged

Conclusion:

- `india_landscape_gui.rpk` is not the blackout switch
- the rendered preview that persists under blackout is not owned by this
  local GUI pack alone

### Shared `globaldata` animationlib RTT/grading-only shot

After broad and fade-only `globaldata.rpk` edits were already known to
be unstable, a narrower animationlib-only shot was attempted:

- `RTT_BLUR_ON`
- `RTT_BLUR_OFF`
- `RTT_GRADING_ON`
- `RTT_GRADING_OFF`

with:

- `TemporalFade = 0`
- `OverrideBlend = 0`

and **without** touching:

- `FadeIn`
- `FadeOut`
- `FadeIn_Fast`
- `FadeOut_Fast`
- shared actor records

Observed behavior:

- still black-booted / hung before reaching race start

Conclusion:

- even narrow `globaldata` animationlib edits are too unstable to use
  as a practical runtime patch surface
- `globaldata` remains analysis-only unless a much safer patch or
  runtime override method exists

### Shared prerace state-string rewrite

A code-side state rewrite was attempted on overlay `eboot.bin`:

- `kViewVehicle -> kStart`
- `kGetInVehicle -> kStart`

Observed behavior:

- race still loaded normally
- blackout still happened normally
- the game then hung in-race and could not be exited cleanly

Conclusion:

- this did hit a shared state path
- it did **not** hit the blackout switch
- blind rewriting of prerace state literals is not a productive next
  path

### Current load-bearing conclusions

What is now effectively exhausted:

- host-present / gamma / final output theory
- prerace UI page/panel family
- local Munnar prerace/postfx brute-force as the blackout master switch
- `india_landscape_gui.rpk`
- direct `globaldata.rpk` mutation as a safe runtime test path
- blind prerace state-string rewrites

What still seems plausible:

- a code-side handoff/state branch around the actual car select ->
  race-load -> blackout transition
- but it needs targeted tracing first, not another guessed family cut

### Best next code-side trace points

The most useful code-side sites found so far are:

- `0x2936a0`
  - shared freeplay/prerace string/state user
- `0x2c1cc0`
  - real `kGetInVehicle` user in the freeplay/prerace path
- `0x2c5200`
  - dispatcher that maps one branch directly to `kGetInVehicle`

Recommended next step:

- instrument these specific paths to log which state/page/camera branch
  is actually selected at:
  - car select confirm
  - race load handoff
  - blackout start
- then patch the chosen branch once, instead of doing another broad
  asset or controller shotgun

## Dump-driven late-branch analysis

After the asset/UI/controller branches stalled, the investigation moved
to race-start image/pipeline dumping around the actual blackout window.

### First concrete late carrier

The first useful dump split showed:

- `0x500cdd0000` contains a healthy race image before blackout fully
  settles
- a later graphics pipeline writes two full-res black outputs:
  - `0x5000108000`
  - `0x5000900000`
- that pipeline hash is:
  - `0x1c2223026ec47d56`

Its observed sampled inputs included:

- `0x5016040000` full-res `R8G8B8A8Snorm`
- `0x5015a20000` `512x384`
- `0x501e910000` / later `0x501e8f0000` `32x32`

This was the first strong late-stage carrier candidate.

### Late carrier is not the blackout switch

Several direct torture shots were run against that late family.

#### Skip late black-output pipeline

Skipping `0x1c2223026ec47d56` caused:

- main scene all black
- HUD/map still intact
- mirror still black at first, then recovering normally

Conclusion:

- `0x1c2223026ec47d56` is a main-view carrier/composite pass
- it is **not** the blackout master switch

#### Tiny `32x32` side branch

The tiny compute producer:

- `0x000001007af82364`

writes:

- `0x501e8f0000` / `0x501e910000`

Observed input characteristics:

- simple gradient-like `32x32` image input
- one small state/control buffer

Results:

- skipping both compute producers feeding the late branch changed the
  scene materially
- skipping only the full-res `Snorm` compute did **not** change the
  blackout meaningfully
- skipping only the tiny compute made the scene go black with HUD
  intact, but the blackout timing still survived
- nulling the tiny compute buffer briefly exposed a full-screen grey
  overlay under HUD, then the usual blackout still happened

Conclusion:

- the tiny branch modulates a late overlay/composite input
- it still does **not** appear to be the blackout master switch

### Newer upstream full-res feedback family

The wider passive graph dump exposed a newer upstream loop around these
full-res surfaces:

- `0x50105c8000`
- `0x5009688000`
- `0x5008130000`
- compute auxiliaries:
  - `0x501459f800`
  - `0x5014d88800`
  - `0x5015571800`
  - `0x5016830000`

Key producers/readers in that family:

- `0x7e5d7e5e5abfc092` -> writes `0x50105c8000`
- `0x7271230fb8d57731` -> reads `0x50105c8000 + 0x5008130000`, writes
  `0x5009688000`
- `0x88a292ce37169b74` -> reads `0x5011b20000 + 0x5009688000`, writes
  `0x5008130000`
- `0x00000209c18a474f` -> writes:
  - `0x501459f800`
  - `0x5014d88800`
  - `0x5015571800`
- `0x00000404da9734f2` -> reads those and writes `0x5016830000`
- `0x729626e3082b941f` -> reads `0x5016830000`, writes `0x5008130000`

Torture result:

- skipping this family changed the scene strongly
- user impression was "lots of motion blur or similar"
- blackout still remained

Conclusion:

- this is another feeder/carrier family
- still not the blackout switch

### Parallel `500fdd / 5015ae / 501e8f` branch

Another branch feeding the same late carrier was identified:

- `0x500fdd0000`
- `0x5015ae0000`
- `0x501e8f0000`

Key producers:

- `0x8d3903b3d86ec69e`
- `0xd95182db58ebc733`
- `0x6df834395a1984ff`
- `0x2a5e157a75d687d2`
- `0x000001b96128ff3e`
- `0x4a8b302d5b6ce10b`
- `0x000001007af82364`

Torture result:

- scene all black again
- HUD/map intact
- mirror still black first, then recovering

Conclusion:

- this parallel branch is also only a visible-scene feeder/carrier
- blackout timing survives above it

### Broad combined late-feeder shot

A broader "all known late feeders into the black carrier" shot was
attempted by combining:

- the `500813 / 500968 / 50105c` side
- the `500fdd / 5015ae / 501e8f` side
- tiny `32x32` sidecars

Observed behavior:

- unstable / crash on race load

Binary split result:

- Half A (`500813 / 500968 / 50105c` side) remained crash-prone
- Half B (`500fdd / 5015ae / 501e8f` side) produced all-black scene
  with HUD intact, but blackout still survived

Conclusion:

- the whole mapped late rendered/image tree is now best classified as:
  - visible-scene carrier/composite machinery
  - **not** the blackout master switch

This is a load-bearing conclusion. It explains why so many earlier
renderer-side shots changed tint, motion-blur feel, or the frozen image
under blackout without actually disabling blackout timing.

### Shared-state shot on the late carrier family

To verify that the switch was not hiding in local state on the same
pipelines, two state-only shots were run on the known carrier family and
its immediate feeders:

1. zero small read-only buffers / `Flatbuf`
2. zero only `push_data`

Observed behavior:

- buffer/state zeroing darkened the scene under blackout
- zeroing only `push_data` returned to baseline-like behavior
- blackout timing still survived in both cases

Conclusion:

- even the local state on the late carrier family is not the blackout
  master switch
- the real gate is still above or outside this rendered/image branch

### New code-side lead: gameplay camera dispatcher

Disassembly of the best remaining code-side handoff sites produced the
first concrete non-renderer branch with semantic meaning.

At `0x2c5200`, the code is a dispatcher that selects values for:

- `gameplay_camera_view`

Observed candidate values in the string table:

- `fly`
- `simple`
- `inputorbit`
- `fixedoffset`
- `driverhead`
- `vehicle_attached`
- `vehicle_chase`
- `orbit`
- `iview_obtainer`

This is much more specific than the earlier page/controller guesses.
It directly matches the repeated symptom that the blackout often carries
a prerace/static camera-like image underneath it.

Additional nearby code-side observations:

- `0x2936a0` is still a shared freeplay/prerace state user
- `0x2c1cc0` is still a real `kGetInVehicle`-path function
- but `0x2c5200` now looks like the strongest concrete branch because it
  selects actual gameplay camera view modes rather than only page/state
  names

### Updated load-bearing conclusion

What is now effectively ruled out:

- the late black-output pipeline as the blackout switch
- the late `32x32` side branch as the blackout switch
- the newer `500813 / 500968 / 50105c` feedback family as the blackout
  switch
- the parallel `500fdd / 5015ae / 501e8f` family as the blackout switch
- local buffer / `Flatbuf` / `push_data` state on that late carrier
  family

What still looks promising:

- a code-side race-handoff branch that selects or latches the wrong
  gameplay camera / view mode
- a shared state branch above the rendered late carrier tree

Best next step after this doc update:

- reset renderer-side torture to baseline
- test a camera-mode branch on the `gameplay_camera_view` dispatcher
  instead of another renderer-side family

## Code-side camera-family follow-up

The next code-side branch after the renderer/image tree was the shared
camera / handoff family around:

- `0x2c5200`
- `0x2c1cc0`
- `0x2936a0`
- adjacent setup at `0x293840`

### `0x2c5200` gameplay camera dispatcher

Disassembly showed `0x2c5200` is not a page router. It is a dispatcher
for:

- `gameplay_camera_view`

Observed string arms:

- `fly`
- `simple`
- `inputorbit`
- `fixedoffset`
- `driverhead`
- `vehicle_attached`
- `vehicle_chase`
- `orbit`
- `iview_obtainer`
- `world`
- `photomode`
- `blender`
- `customisation`

#### Broad camera-mode shot

Test:

- all dispatcher arms forced to `vehicle_chase`

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- blackout is not explained by a simple wrong `gameplay_camera_view`
  choice in this dispatcher

### `0x2c5200` shared camera-state apply block

The handler does not only set `gameplay_camera_view`. It also applies a
shared camera-state family:

- `aperture_f_value`
- `exposure_compensation`
- `focal_distance_metres`
- `shutter_speed`
- `screen_filter_index`
- `screen_filter_name`

#### Narrow camera-parameter shot

Test:

- disable only the camera-parameter setter calls

Observed behavior:

- baseline behavior
- blackout unchanged

#### Broad camera-state shot

Test:

- disable the whole shared camera-state apply block, including
  `gameplay_camera_view` and the parameter setters above

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- the full `0x2c5200` shared camera-state handler is a clean miss

### `0x2c1cc0` `kGetInVehicle` helper family

`0x2c1cc0` is a real `kGetInVehicle` path function. A later internal
helper call inside it (`0x2c5710`) looked like the strongest remaining
local handoff candidate.

Test:

- disable the `0x2c1cc0 -> 0x2c5710` helper call

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- this `kGetInVehicle` helper family is also a clean miss

### `0x2936a0` broad shared transition/state function

Earlier analysis had already flagged `0x2936a0` as a strong shared
freeplay/prerace state user.

Additional disassembly showed it mostly talks to strings such as:

- `photomode`
- `PhotoModeTutorial_JPSKU`
- `PhotoModeTutorial`
- `socialhub`
- `text_description`

Test:

- function cut at entry (`ret`)

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- `0x2936a0` is not the blackout switch either
- it is likely tutorial / shared UI-state logic, not the blackout gate

### `0x293840` adjacent object-setup path

Because `0x293840` seeds the shared `1b0` handoff object used by the
other functions above, it was tested as the next broad cut in this
family.

Test:

- function cut at entry (`ret`)

Observed behavior:

- boot crash / black boot

Conclusion:

- `0x293840` is too early / boot-sensitive to use as a practical
  runtime torture target
- it does not give usable blackout evidence

### Updated family conclusion

What is now effectively exhausted:

- the `0x2c5200` camera-mode dispatcher
- the `0x2c5200` shared camera-state apply family
- the `0x2c1cc0` `kGetInVehicle` helper family
- the broad `0x2936a0` shared transition/state function
- the adjacent `0x293840` object-setup path as a safe torture target

This means the whole current code-side camera/prerace caller family has
gone cold in the same way the late rendered/image carrier tree did.

What remains plausible after these misses:

- the shared callee/object side those functions talk to
- not the camera/prerace callers themselves
- not the late image carriers they eventually feed

- Static `CameraFade` / `SetCamera` string corruption is an adjacency path only: it changes race-start darkness and handoff, but still has not hit the blackout itself. Stop this line and pivot to runtime dispatch tracing instead of more string splits.

## Phase 13 — the UBO dump breaks it open

### Setup

`SHADPS4_DC_UBOLOG=1` enabled a 128-byte hex dump of every non-special
uniform buffer for six hand-picked race-window pipelines, guarded by
the existing race-window arm. Single Munnar 19:30 baseline session
captured 1905 frames / ~35 s. Log: `.../shadPS4/log/shad_log.txt`,
32 MB, 150k `[dc-drawlog]` lines + 9k `[dc-ubolog]` lines.

### The VideoOut-gamma red herring

Every frame of the session, the game calls
`sceVideoOutColorSettingsSetGamma(gamma=0.5)` and the follow-up
`sceVideoOutAdjustColor` — 2181 back-to-back calls over 35 s, which
then flow into `presenter.pp_settings.gamma`. The value never
changes: no call ever uses anything but `0.5`. Since menus and
panorama look fine while this is happening, the presenter gamma path
is constant and cannot be the blackout on/off. `NoteDriveclubVideoOutGamma`
only fires on values inside `[0.49..0.51]` so it stays in bounds but
is decoupled from the actual dim.

Side observation: `post_process.frag`'s `pow(rgb, 1.0/(2.4+1.0-pp.gamma))`
curve does do something non-trivial at `gamma=0.5` (exponent 1/2.9
instead of 1/2.4) — but since it is constant across the whole session
it is not what flips the scene from bright to dim. Keep it filed as a
"possibly wrong sRGB interpretation" follow-up, not a blackout lead.

### The UBO offset that moves

Pipeline `0xf6e5670be11b0009` dumps a 48-byte UBO at `stage=0 cb0`
and the identical buffer at `stage=2 cb0` (same guest address — VS
and FS both read it). Across 307 samples inside the race window
(submits 1452..1758) only one float in that UBO ever changes: **offset 3**.

Layout is invariant:

    [0]=1.0    [1]=1.0    [2]=0.3     [3]=LUM      <-- only one that moves
    [4]=1.0    [5]=0.25   [6]=1.0     [7]=0.25
    [8]=0.25   [9]=1.0    [10]=0.25   [11]=1.0

`[0..3]` looks like a `vec4(scale_r, scale_g, scale_b, luminance)` and
`[4..11]` are two `vec4` weight masks `(1,¼,1,¼)` and `(¼,1,¼,1)` —
textbook eye-adaptation / auto-exposure signature.

Offset 3 is monotonically rising over the captured ~2 s:

    submit=1452   lum=5.35
    submit=1502   lum=7.50
    submit=1602   lum=10.30
    submit=1702   lum=12.65
    submit=1758   lum=13.81

307 samples, every single one larger than the one before. No
oscillation, no convergence — a clean climb with constant slope. The
session ended inside the climb, never reaching equilibrium.

### Why this matches the symptom exactly

The user-reported blackout:

- kicks in a second after the race loads (adaptation starts from a
  neutral value, then climbs)
- deepens over 5–30 s (value keeps climbing as the system hunts for
  a scene brightness it never stabilises on)
- sometimes never comes back (the loop has no upper clamp — if the
  target is numerically unreachable, it just stays dim forever)

The HUD/minimap overlay on top of the blackout is untouched because
overlays are composited *after* the tonemapper — this UBO only
reaches the scene rendering, not the 2D UI passes. That also matches.

### What's probably wrong

This looks like an eye-adaptation feedback loop whose denominator or
upper clamp is off. The rising value is almost certainly the
"current adapted luminance" the tonemap divides by — so as it climbs,
the scene gets darker. The loop either:

- reads its histogram from a stale/over-bright HDR source (texture
  cache serving the wrong resident copy, or a page-fault miss
  returning zeros that get interpreted as "fully saturated"), or
- never runs the shrinking half of the adapt loop (missed compute
  dispatch, or a scheduling race between the histogram pass and the
  tonemap's UBO write)

Either way the downstream symptom is the same: offset 3 climbs
without bound, and the tonemap responds by dimming the scene.

### Next step — quick validation via clamp

Before chasing the root cause in the histogram/compute side, cheap
test:

- at UBO bind time for pipeline `0xf6e5670be11b0009`, recognise the
  invariant layout `(1.0, 1.0, 0.3, x, 1.0, 0.25, ...)` and overwrite
  offset 3 with a fixed value (2.0, say — low end of the observed
  range, near the "bright" plateau).
- env-gate it: `SHADPS4_DC_LUM_CLAMP=2.0`.
- if the blackout disappears or dramatically shortens, the UBO is
  confirmed as the sole dim driver and the real fix is upstream in
  whichever pass writes this value.
- if the blackout is unchanged, this UBO is a downstream reader of
  something else and we need a different intervention.

If the clamp works, the follow-up is to find the shader that writes
this UBO (compute pass that computes adapted luminance) and fix *its*
feedback rather than patching the consumer. That compute shader is
probably one of the other 5 `kDriveclubLaterRaceGatePipelines` that
Codex already singled out.

### Action items

- extend `vk_rasterizer.cpp` with an `SHADPS4_DC_LUM_CLAMP` env knob
  that rewrites offset 3 of the matching UBO shape at bind time
- keep the drawlog + ubolog instrumentation in place — it's now the
  primary diagnostic for this class of bug
- longer session recording (5+ minutes) is still useful: need to see
  whether the value *ever* plateaus on its own or keeps climbing
  forever, and whether a brief recovery phase corresponds to a value
  dip

## Phase 14 — the scene-pipeline UBOs are the wrong layer

Phase 13 built `SHADPS4_DC_LUM_CLAMP` to force offset 3 of pipeline
`0xf6e5670be11b0009` down to 2.0 during the race window. The probe
fired 1093 times across the session, signature matched, the write
was observed — and the blackout was unchanged. Phase 13's "runaway
eye-adaptation" story did not survive first contact.

### Round 2 — phase-transition UBO in cb4

Went back to the raw data and ranked all six logged pipelines by
per-offset dynamic range. Pipelines `0x6bde71906ac1af18` /
`0x1cdd747ee89204c0` stage=1 cb=4 stood out: a 32-float UBO whose
tail block (offsets 24..31) flipped from `(292.6, 288.3, 271.3,
13.6, 0.31, 0.31, 0.30, 0.015)` pre-race to **all zero** at
submit 1466 — the exact gate-re-arm that coincides with race start.
Also at the transition:

- off[0]:  0.4 → 1.0
- off[12]: 0   → 1    (clean binary flag)
- off[16]: 0.001 → 0.59
- off[20..22]: sign/magnitude shift

Restored offsets 24..31 to the pre-race values during the Z phase
via `SHADPS4_DC_EXPO_RESTORE=1`. Blackout unchanged again.

### Round 3 — the "nuke" diagnostic

Built `SHADPS4_DC_UBO_NUKE=1` — writes a loud ±1e30 pattern into
the UBO at Draw-time to verify that in-place writes to guest
memory from that hook actually propagate to the shader. Full-UBO
nuke: scene went fully white during race and stayed white.
**Writes do propagate.** This eliminated the "buffer_cache cached
the upload too early" theory.

### Round 4 — binary-searching the whitening

Then narrowed which offsets, when blown to ±1e30, cause the
whiteout:

- nuke 0..31  → white
- nuke 24..31 → white only in menu animation, race stays at baseline blackout
- nuke 0..23  → white during race
- nuke 0..11  → baseline
- nuke 12..23 → white
- nuke 12..17 → white
- nuke 12..14 → baseline
- nuke 15..17 → (implied) white
- nuke 16     → white

So off[16] is one of the values the fragment shader consumes into
its final color math. Tried the obvious follow-up: force off[16]
to 0.001 (the pre-race value) during the race window. **Baseline
blackout, no brightness change.**

### What this proves, and what it doesn't

The Round-3/4 whiteout is a tonemap-overflow side effect: any
float in the fragment-shader path, when pushed to ±1e30, overflows
the tonemap and the visible composite clamps to white. That's
orthogonal to the actual race-start dim mechanism. Setting the same
offset to its *normal range* (0.001..6) did not shift brightness
at all — so off[16] is not a scene-brightness multiplier.

The cleanest reading is that the UBO in `0x6bde71906ac1af18` cb=4
encodes a state transition (camera-mode / scene-context / lighting
preset flip) that happens to coincide with the blackout but is not
the thing drawing the dim pixels. Restoring the pre-race state
doesn't repaint the scene bright because that scene's dim is
produced somewhere else — most likely in a fullscreen
tonemap/post-fx pass that runs after the scene draws and before
the HUD composite.

That matches the user-observed layering exactly: the HUD is drawn
on top of the blackout and stays intact, so whatever dims the
scene dims the HDR→SDR output of the tonemap, not the scene shader
output directly.

### Pivot — broaden the UBO probe to post-fx pipelines

`kDcUboLogPipelines` currently lists six scene-draw pipelines
Codex picked during Phase 12. We need to extend it (or replace it)
with the pipelines that target `0x500cdd0000` alone with no depth
— those are the fullscreen composition / tonemap / present passes
where the dim logically has to live.

Plan for the next round:

- launch a clean drawlog-only session (no UBO interference), play
  through Munnar 19:30 to recover the pipeline landscape
- from `[dc-drawlog]` lines, extract every pipeline whose `rts` is
  exactly `0x500cdd0000` with `depth=no` (or similar single-RT
  fullscreen patterns) — those are tonemap candidates
- add the top ~6 of those to `kDcUboLogPipelines` and capture
  their UBOs across the race window
- diff bright-phase vs dim-phase samples the same way, looking
  for a clean multiplier that moves monotonically with the fade

If a clean candidate shows up there, re-run the clamp/restore test
on that offset — this time with confidence it's in the actual dim
pipeline, not a coincident state UBO.

### Status

Binary `build/shadps4` currently has three experimental knobs live:

- `SHADPS4_DC_LUM_CLAMP=<float>` — clamps off[3] of `0xf6e5..0009`
- `SHADPS4_DC_EXPO_RESTORE=1`    — restores off[24..31] of the cb4 UBO
- `SHADPS4_DC_UBO_NUKE=1`        — writes `off[16] = 0.001f` on cb4

None of them changes the blackout. They're kept in tree as the
scaffolding for the next round — all three become no-ops when the
env var is unset.

### Round 5 — extending the UBO probe to composition pipelines

Added the three pipelines that write to the visible composite
`0x500cdd0000` as their sole color target (depth=yes) — `0x2bd7..ae2`,
`0xadd2..da9`, `0xe6e4..d99` — into `kDcUboLogPipelines`. One clean
race session with UBOLOG on, 2064–1974 samples per pipeline.

Findings:

- `0xe6e4..d99` stage=0 cb=1 encodes two resolution modes: **80×48**
  pre-race and **364×276** from submit 1300 onwards. `off[4..9]`
  form `(width, height, 1/w, 1/h, 0.5/w, 0.5/h)` — classic luma grid
  metadata. 80×48 is codex's luma-per-tile grid from the torture
  probe. The mode switch lines up exactly with the 3rd gate arm,
  i.e. race start.
- `0x2bd7..ae2` cb=0/1 `off[12..14]` collapse from large per-frame
  values (mean 39 / −23 / 7) to 2–3 static values (mean −0.5 / −0.4
  / −0.7) at the same submit-1300 boundary.
- `0xadd2..da9` has no clean phase-split; every offset is
  per-frame-variable.

None of these UBO shifts is the dim driver: the user-reported
blackout during this run was brief (≈26 s, ending in a recovery)
and showed the same "dark car static image" visual we flagged in
Phase 12. The "mode switch at race start" pattern just confirms the
game re-parameterises a lot of post-fx passes at the boundary; it
doesn't tell us which pass paints the dim pixels. Still circling
the same layer.

### The three open paths

Not giving up on any of them — they stay in-tree as parallel leads:

1. **Hunt the overlay draw (in-flight, Phase 15 scaffolding below).**
   The blackout visually reads as a specific *texture* — the "dark
   car static image" — drawn as a fullscreen overlay over the scene,
   not as a gradient. Dump every bound guest-side texture during
   the race window, compare against UI assets in
   `newui/panels/*.txt`, and find the draw that renders the overlay.
   Once identified, either disable that draw, or force its alpha to
   zero. Clearest win path if it works.

2. **Probe the remaining single-RT `depth=no` post-fx pipelines.**
   `0x5000900000`, `0x5000108000`, `0x500fdd0000`, `0x509a400000`,
   `0x5015770000`, `0x501abf8000`, and friends all write 1920×1080
   surfaces with depth disabled — classic fullscreen blit/post-fx
   targets. Extend `kDcUboLogPipelines` to cover pipelines that hit
   those RTs and repeat the bright-vs-dim diff. Slower, but it's
   the same diagnostic that already found the cb4 mode flip — just
   applied at the right layer this time.

3. **Revisit codex's camera-fade line.** Phase 12 flagged
   `CameraFade` / `SetCamera` adjacency as a partial hit: string
   corruption "changes race-start darkness and handoff, but still
   has not hit the blackout itself." That's not a dead end — it's
   an underpowered lever. A targeted runtime hook that forces the
   camera-fade output value to 1.0 at a known call site, rather
   than patching strings, could finally either confirm or fully
   rule out this axis.

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


## Phase 16 — runtime texture mutation, rotated

The Phase 15 texdump exposed ~1100 unique textures per race session
but the dark-car-static-image only showed up intermittently, and PNG
previews of tiled dumps were too scrambled to triage by eye. Rather
than port the detile math, we switched to mutating textures in place
at the emulator and watching the scene react.

### Scaffolding

`MaybeNukeDriveclubTexture(const VideoCore::Image& image)` was added
next to the dump hook in `Rasterizer::BindTextures`. On first bind of
each unique guest address, it overwrites the full `info.guest_size`
with a per-texture deterministic tint (splitmix64 hash of the guest
address → RGB565 for BC1/BC3, RGBA for uncompressed) and then calls
`texture_cache.InvalidateMemory(addr, size)` so the next
`FindImage` re-uploads the clobbered bytes to the Vulkan image.
Without that invalidation the cache keeps serving the original
upload and the writes are invisible — confirmed the first time we
tried, where 1093 nuke events fired with no visual effect.

Gates:

- `SHADPS4_DC_TEX_NUKE_ALL=1` — match every bound texture
- `SHADPS4_DC_TEX_NUKE_ADDRS=0xAAA,0xBBB,…` — explicit addresses
- `SHADPS4_DC_TEX_NUKE_PIPES=0xHHH,0xIII,…` — every texture bound by
  these pipelines
- `SHADPS4_DC_NUKE_SKIP_PIPES=…` — exclude list
- `SHADPS4_DC_NUKE_KIND=scene|postfx|ui|other|all` (default `scene`)
  — classifier based on RT pattern
- `SHADPS4_DC_NUKE_AFTER_ARM=N` — skip the first N race-gate arms
  (menus/panorama). Default 0.
- `SHADPS4_DC_NUKE_MIN_DIM` / `_MAX_DIM` — size bracket
- `SHADPS4_DC_NUKE_MAX_ASPECT` — reject long strips above this ratio
- `SHADPS4_DC_NUKE_FORMAT=<substr>` — format name substring filter
- `SHADPS4_DC_NUKE_INCLUDE_DATA=1` — re-include float/int data formats
- `SHADPS4_DC_NUKE_INCLUDE_GPU_MOD=1` — re-include render targets
  being re-sampled

A draw-kind classifier (`ClassifyDriveclubDraw(regs)`) sets a
per-draw atomic at `Draw()`/`DrawIndirect()` entry so BindTextures
can know whether it's inside a scene / post-fx / UI draw without
inspecting regs itself.

All of this is env-gated — with no env var set the probe is a zero-
cost no-op and the emulator behaves exactly as baseline.

### The 32×32 LUT finding

Early runs with every-texture nuke produced a uniformly cyan (then
yellow, then again cyan on re-hash) fullscreen scene: HUD still
visible, 3D scene completely flat. Interpretation: one nuked
texture was a fullscreen-collapser — a LUT that the tonemap reads
and which maps every input to a single colour.

Bracketed binary search on size:

1. `MIN_DIM=1 MAX_DIM=31` → scene intact, blackout unchanged. Nothing
   dim-related in that bracket.
2. `MIN_DIM=32 MAX_DIM=63` → scene collapses to a flat hue. Hit.
3. `MIN_DIM=32 MAX_DIM=47` → still collapsed. Keep bisecting.
4. `MIN_DIM=32 MAX_DIM=39` → collapsed.
5. `MIN_DIM=32 MAX_DIM=32` → still collapsed. Size locked at 32×32.

Log of the 32×32 nuke showed four candidates:

    0x5007b53400  R8G8B8A8Unorm       pipe 0xefc48aef4d3fc629
    0x5003896400  R8G8B8A8Unorm       pipe 0x1817f4403d552f1f
    0x50ba119c00  Bc1RgbaSrgbBlock    pipe 0x6dd8a5ae9841fc54
    0x50b884d000  Bc1RgbaSrgbBlock    pipe 0x6dd8a5ae9841fc54

Testing the two R8G8B8A8 candidates together still collapsed.
Testing `0x5007b53400` alone — collapsed. **That's the colour-grading
LUT** bound to pipeline `0xefc48aef4d3fc629` and sampled by the
tonemap path. 32×32 R8G8B8A8 is exactly the unrolled shape of a
32×32×32 colour cube or a 2D LUT slice.

Killing this LUT does not affect the blackout — during the flat-cyan
session the mirror visibly goes black and returns just like baseline.
The LUT is downstream of the dim. It is documented here so we can
*skip* it in future texture nukes (it produces a false-positive
scene collapse that hides whatever we're actually trying to see).

### The batch rotation — confirming no texture is the dim

With the LUT identified we rotated every remaining skipped category
through `SHADPS4_DC_NUKE_*` knobs. One env-var change per round,
one game session per round, no rebuild:

| round | filter                                  | observed visual           | dim? |
|-------|-----------------------------------------|----------------------------|------|
| 1     | `MIN_DIM=1 MAX_DIM=31`                  | coloured scene             | yes  |
| 2-5   | 32..63 → 32..47 → 32..39 → 32..32       | flat colour scene (LUT)    | yes  |
| 6     | LUT alone `0x5007b53400`                | flat colour scene          | yes  |
| 7     | `MIN_DIM=64 MAX_DIM=127`                | coloured scene             | yes  |
| 8     | strips only (`MAX_ASPECT=0`)            | coloured scene             | yes  |
| 9     | `INCLUDE_DATA=1 FORMAT=Sfloat`          | normal-looking scene       | yes  |
| 10    | `INCLUDE_GPU_MOD=1`                     | coloured scene             | yes  |
| 11    | `FORMAT=Bc4`                            | normal-looking scene       | yes  |
| 12    | `FORMAT=Bc5` (normal maps)              | normal-looking scene       | yes  |
| 13    | `KIND=postfx`                           | HUD glitched, scene clean  | yes  |
| 14    | `KIND=ui`                               | baseline                   | yes  |

In every single round the blackout continued to happen and the
mirror continued to go black for the usual 8–30 s window. The
user-reported blackout is therefore **not texture-content driven**.
No asset texture, LUT, normal map, HDR feed, UI atlas, post-fx
buffer, or GPU-modified render target is the dim.

### What this rules out

Durable conclusions that hold regardless of how the remaining
investigation turns out:

1. The blackout is not a fullscreen overlay *image* (no texture to
   nuke matches the dim window).
2. The blackout is not a mask-on-HDR composited through a BC1 / BC3
   alpha layer.
3. The blackout is not an LUT mis-swap — the colour-grading LUT is
   `0x5007b53400` and nuking it decouples from blackout timing.
4. The blackout is not in any sampled texture on any scene-material
   pipeline, including HDR feeds and GPU-modified intermediates.

So the dim must live in one of:

- a **UBO field** (shader uniform) that multiplies scene output —
  the Phase 13 probe only covered ~12 hand-picked pipelines; the
  full race-window scene pipeline set is >60 and was never diffed
  exhaustively
- a **push constant** — we have not instrumented these at all
- a **compute dispatch** output (auto-exposure / bloom / luma /
  histogram) that writes a value the graphics pipelines then read —
  drawlog does not see compute and the texnuke covers only sampled
  images, not storage buffers that compute writes to
- a **fixed-function pipeline state** — constant blend colour, blend
  factor, colour-write mask

### Path forward (next sessions)

Ranked by payoff vs effort:

1. **Compute dispatch probe** — add `SHADPS4_DC_DISPATCHLOG=1` that
   logs every `Rasterizer::Dispatch` during the race window with
   its buffer bindings, and `SHADPS4_DC_UBO_NUKE_ALL=1` that runs
   the same hash-tint scheme on every compute-bound storage/uniform
   buffer. Biggest blind spot in the diagnostic stack and the most
   likely single home for the dim given that Driveclub's bloom /
   exposure run on compute.
2. **Exhaustive UBO nuke** — port the same bracketing logic we used
   for textures to the UBO bind path: for each draw, rewrite each
   bound UBO's guest memory with a per-draw random fill, invalidate
   the buffer_cache range, let the game's next per-frame UBO write
   repopulate. If the dim disappears during a specific
   size/pipeline bracket we've found the uniform that drives it.
3. **Push-constant sweep** — lower-priority; only warranted if both
   above come back empty. Push constants are 128 bytes at most on
   AMD/GCN; a sweep would rewrite them per-draw just before
   `cmdbuf.pushConstants()`.

### Commit state at end of Phase 16

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` now carries:

- `ClassifyDriveclubDraw` (scene / postfx / ui / other)
- `g_driveclub_current_pipeline_hash`, `g_driveclub_current_draw_kind`,
  `g_driveclub_arm_count` atomics
- `MaybeNukeDriveclubTexture` with all the knobs above
- `MaybeDumpDriveclubTexture` (Phase 15)
- `MaybeClampDriveclubLuminanceUbo` / `MaybeRestoreDriveclubExposureUbo` /
  `MaybeNukeDriveclubExposureUbo` (Phase 13)
- race-gate now increments `g_driveclub_arm_count` and logs `armed#N`

Nothing changes emulator behaviour unless a `SHADPS4_DC_*` env var
is set. Safe to leave on the branch while the investigation moves
to compute.
