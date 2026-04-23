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
