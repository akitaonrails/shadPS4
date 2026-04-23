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
