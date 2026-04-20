<!--
SPDX-FileCopyrightText: 2026 Fabio Akita
SPDX-License-Identifier: GPL-2.0-or-later
-->

# A day chasing Driveclub on shadPS4 (with Claude Code)

*2026-04-20 — `gamma-debug` session notes, written as a narrative rather than a
changelog.*

This isn't a "here's how I fixed it" post. It's a "here's what a full day with
an AI agent at the keyboard actually looked like" post. Some of it worked.
Plenty of it didn't. The game is in a better state at the end of the day than
it was at the start, but it isn't *fixed*, and the shape of the hours in
between is the actual interesting part.

The target: [DRIVECLUB](https://en.wikipedia.org/wiki/Driveclub) CUSA00003,
stock v1.28, on my Arch Linux desktop — Ryzen 9 7950X3D, RTX 5090, NVIDIA 595
driver, Hyprland/Wayland. The emulator is
[shadPS4](https://github.com/shadps4-emu/shadPS4), nightly `main-2026-04-19` as
the baseline, via a gaming distrobox. The assistant is Claude Code driving
Opus 4.7 with the 1M-context window turned on. My role was, effectively, QA
engineer, steering committee and release manager rolled into one.

I'm writing this mostly to push back against the "AI agents ship features
autonomously" narrative. That's not what happened here. What happened is a
human-in-the-loop investigation where the human did a lot of the loop.

## Starting point — forks and ground rules

Before any code changes, I set up the fork the way I wanted it:

- `origin` = `akitaonrails/shadPS4`, my fork, the push target.
- `upstream` = `shadps4-emu/shadPS4`, read-only.
- `main` tracks upstream exactly, so rebases stay trivial.
- Experimental work lives on branches. `gamma-debug` is today's.

I also dropped a `CLAUDE.md` at the repo root with project conventions: Clang
18+, the CMake preset names, code style rules copied from CONTRIBUTING.md, and
one authorship rule that turns out to matter more than you'd expect — **no
Claude/AI-attribution trailers anywhere**. Commit messages, PR descriptions,
comments, logs, scripts: all written in first person as me. The goal is to
avoid AI-attribution bias during upstream review. Claude Code's default
`Co-Authored-By: Claude …` trailer is overridden for this repo.

That paragraph is not fluff. It sets the frame for the whole day: the assistant
does the typing, I do the judgment calls, and the output has to read like I
wrote it. If I can't point at a line and say "yeah, that's my decision and I'd
defend it in review", it doesn't ship.

## Phase 1 — the easy win that set the tone

First symptom: **Driveclub's menus animate at crawl speed for the first minute
or so of a cold launch, then snap to full speed.** Same thing next cold launch.

The assistant went straight to "the pipeline cache probably isn't enabled".
One look at `~/.local/share/shadPS4/config.json` confirmed
`Vulkan.pipeline_cache_enabled = false`. Flip the bit, re-launch, eat the
one-shot first-session compile cost (527 shaders + 354 pipelines), and every
subsequent cold launch is snappy.

No code change. Config only. Five minutes, maybe.

This is the part that makes AI assistants feel magic. *All* of the other phases
felt less like this.

## Phase 2 — v1.28 content access (and the `param.sfo` rabbit hole)

Next was installing v1.28 properly. The distrobox notes from a previous session
claimed `DriveClubFS 1.1.0 crashes at file 12/8018 with EndOfStreamException`.
The assistant ran the unpack and got 8018/8018 cleanly. Old claim didn't
reproduce. No fork patch needed.

The actual gotcha was somewhere completely different. v1.28 is a
**CUMULATIVE_PATCH** PKG. When you extract one with `shadpkg`, you get
`sce_sys/about/right.sprx` and… not much else. The base metadata —
`param.sfo`, `disc_info.dat`, `keystone` — isn't re-shipped by cumulative
patches because a real PS4 already has them from the original install.

What that meant on my box: I'd naïvely `cp -a` the v1.28 `sce_sys/` over my
new install, end up with a `sce_sys/` that had `about/right.sprx` but no
`param.sfo` at all, boot the game, and it would silently decide "this content
isn't released yet, please download" for everything. The v1.28 eboot was
running. The patch was applied. The game just couldn't see its own version.

Fix: restore `param.sfo`, `disc_info.dat`, `keystone` from the v1.00 backup.
The v1.00 `param.sfo` already has `APP_VER = 01.28`, because that field gets
updated at patch-apply time on a real PS4 and the base `VERSION` field stays
at `01.00` forever.

This one took a chunk of time because "the game boots but content is locked"
doesn't scream "metadata file missing". You have to work backward from "what
would the game check to decide this content is locked" to "where does that
answer live on disk" to "does my install have that file".

Claude Code helped a lot here — it knew the shape of the PS4 param.sfo format,
it could read it, it could explain what each field did. What it *couldn't* do
was pre-emptively notice that the PKG extraction was lossy. I had to observe
the symptom, describe it, and then we'd go figure out why.

## Phase 3 — the "slowness" that was two different bugs

Back in-game, the feel was "smooth but in slow motion". Frames delivered at a
steady rate, cars accelerated gently, a lap-timer minute showed what should be
half a lap.

This is the classic shape of "render rate and logic timestep are out of sync".
I had re-enabled a community `Driveclub.xml` eboot patch that targets v1.28's
60 fps path, and that patch — like most of the community 60 fps patches for
PS4 games — rewrites the render rate without touching the internal
fixed-timestep game-logic rate. Real PS4 Pro hardware handles the mismatch
internally. shadPS4 doesn't.

Fix: rename `Driveclub.xml` to `Driveclub.xml.disabled-for-v1.0`. Game reverts
to 30 fps stock behaviour, everything moves at the correct wall-clock speed.

That was the first cause. The second one was hiding underneath it.

**Night tracks were almost pure black** — only HUD visible, no headlights, no
road surface, no cars. Day tracks looked "dim" but readable. That dim look is
the ghost that haunts the rest of this post, but on night tracks it wasn't
dim, it was *gone*.

The log had one giveaway repeated a thousand times per session:

```
texture_cache.cpp ResolveDepthOverlap: Unimplemented depth overlap copy
```

Instrumenting that branch to log each unique shape combination (and cache so
each shape only logs once, not a thousand times) gave me:

```
cache(fmt=D32Sfloat     depth=true  stencil=false samples=4)
  -> new(fmt=R32G32B32A32Sfloat depth=false stencil=false samples=1)
```

Driveclub renders geometry into a **4× MSAA depth target**, then binds that
depth aspect as a **1-sample R32G32B32A32Sfloat sampler2D** for its lighting
accumulation, SSAO, and soft-particle passes. shadPS4's resolver had no path
for this combination, so the catch-all `else` branch freed the cached MSAA
depth and returned an uninitialised 1-sample colour image. Every
depth-sampling pass downstream was reading garbage.

Day scenes survived because most of the lighting comes from ambient and sun
baked into the material pass. Night scenes *are* screen-space volumetric
headlights keyed off the depth buffer. No depth, no lights, no scene.

The fix was to mirror the existing `BlitHelper::ReinterpretColorAsMsDepth`
(1-sample colour → MSAA depth) in the opposite direction. A new fragment
shader `ms_depth_to_color.frag` that `texelFetch`es sample 0 of a
`texture2DMS`, a new helper `ReinterpretMsDepthAsColor`, and an `else if`
branch in `ResolveDepthOverlap` dispatching it. The catch-all `else` stayed
around with the shape-logging so any *other* unhandled combination is still
caught for future debugging.

After deploying: night tracks had headlights back. Road surface visible. AI
car brake lights lit. Screen-space lighting stack back online.

This is the one fix from today that I'd call a clean upstream candidate.
It's symmetric with code that already exists, bounded, testable, and has a
specific observed symptom it resolves. The rest of the day was… not that.

## Phase 4 — the dim-daylight hydra

The remaining complaint: **Driveclub's daytime scenes are markedly dim** —
HUD looks right, scene looks desaturated and crushed, and the game's own
in-menu brightness slider tops out before the image looks how reference
footage does.

This is where the day went sideways, and it's the part I actually want to
talk about.

### First reflex — "it must be a tonemap problem"

The assistant's first move was textbook. PS4 games that look dim in SDR are
usually writing HDR-intent values into an 8-bit sRGB framebuffer, expecting
the console's SDR tonemap to compress them. Implement a tonemap in the
emulator, apply it in the present shader, problem solved.

We built the toolkit:

- An exposure multiplier, `pp.exposure`, default `1.0`.
- ACES Narkowicz per-channel tonemap.
- A luma-preserving variant that tonemaps `dot(rgb, rec709_luma)` and scales
  chroma proportionally, so hue survives heavy exposure boost.
- Gamma curve clamp widened on the existing `pp.gamma` encode.
- All driven by push-constants, zero per-frame CPU cost.
- Env-var knobs (`SHADPS4_PP_EXPOSURE`, `SHADPS4_PP_TONEMAP`,
  `SHADPS4_PP_GAMMA_OVERRIDE`) so I could tune live without recompiling.

A specific combination made Driveclub look, in the assistant's view, "correct":

```
SHADPS4_PP_GAMMA_OVERRIDE=0.7
SHADPS4_PP_EXPOSURE=3.0
SHADPS4_PP_TONEMAP=luma
```

I tested it. Menus were blown out. Overcast daytime was still dim. Night
looked OK but different-OK than before. The assistant's "correct" and my
"correct" were different. Which is a pattern.

### The 5-second dim at race start

Then I noticed something the assistant hadn't: **at the start of every race,
the image dims for 5–10 seconds and then recovers**. During the dim, you
basically can't see. After it comes back, colours are fine.

I fed that observation back. The assistant's hypothesis: "that's game-side eye
adaptation, a scripted fade-in VFX, not something the emulator's tonemap is
doing". We added a diagnostic bypass — `SHADPS4_PP_BYPASS=1` — that skipped
the entire tonemap chain and just did a plain sRGB encode.

With bypass on, **Driveclub looked correct in every scene**. Menus bright and
vivid. Races properly exposed. Night atmospheric. The 5-second dim still
happened — confirming it's the game's own fade, unreachable from post-process
math because you can't multiply zero into visibility.

Which meant all the tonemap work was wrong from the premise up. The bug
wasn't "the emulator needs a tonemap"; it was "the game writes already-correct
SDR values and our post-process was mangling them".

The post-mortem lesson I committed to the investigation log reads:

> When "the image looks wrong" is the bug, the first diagnostic should be a
> bypass path that shows raw game output. If bypass looks right, the emulator
> post-process is actively mangling the signal; keep your hands off the
> tonemap and look elsewhere. If bypass also looks wrong, then you're
> compensating for something real. Driveclub fell into the first category. We
> added bypass near the end of the session, after re-deriving the same answer
> with a dozen different tonemap tweaks — it should have been the first test.

### The cleanup

With bypass proven right, I had the assistant strip the toolkit. ACES: gone.
Exposure multiplier: gone. Bypass flag: gone (nothing to bypass, the shader
is plain sRGB encode now). Auto-exposure compute pass that had been built
alongside this: gone.

One thing survived: a 4×4 Bayer dither (±1 LSB) that we'd added earlier to
break the visible banding the 8-bit swapchain produces in smooth gradients.
That's a universal SDR win, costs nothing, survived on its own merits.

Commit: "video_core: drop the SDR tonemap toolkit, keep dither + useful
logs". ~400 lines removed. That was the sane thing to ship.

At that point I thought we were done for the day. We weren't.

## Phase 5 — the follow-up that also didn't work

A few hours later I came back to the same build, cold. Menus looked OK. In-
game daylight was *still* dim. Specifically: when I moved the game's own
in-menu brightness slider, the screen clearly responded — going darker and
lighter — but "100%" still looked dim to me.

I asked the assistant to verify the game's slider was actually wired. We
added a `LOG_INFO` inside `sceVideoOutColorSettingsSetGamma` and
`sceVideoOutAdjustColor`. Moved the slider in-game. The log line fired with
the expected gamma values. `pp_settings.gamma` updated in real time. The
slider was wired correctly. It just tops out before the image gets as bright
as I wanted.

Those two log lines stayed in the code. Small diagnostic aids, no
performance cost, useful the next time someone chases this.

Then we tried three things, each of which I rolled back.

### Re-tried auto-exposure (peak + mean)

New design: sample peak and mean of a centre crop, compute
`min(TARGET_MEAN / mean, SAFE_PEAK / peak)` as the boost, clamp to
`[1.0, 3.0]`. The peak cap was supposed to prevent blow-out by construction.

Two problems killed it:

1. **It normalises output toward a target, which fights the game's
   brightness slider.** The user raises the slider → game outputs brighter
   frames → auto-exposure sees a higher mean → auto-exposure reduces the
   boost → net change on screen: zero. The slider stops doing anything.
2. Even with the peak cap, the boost applied globally to every pixel
   including HUD. Dim scenes with low mean pushed exposure up and blew out
   already-near-white HUD text.

This is the rule I came away with, sharper than before: **do not build a
feedback loop on top of a signal the user can already control directly.**
If the game has a brightness slider and you sample what the game outputs,
any auto-correction will cancel the slider's effect by construction. That's
not an implementation bug; it's geometry.

Removed.

### Static linear boost

Simplest possible thing: `color_linear * SDR_BRIGHTNESS` with
`SDR_BRIGHTNESS` at 1.5, then 2.0. I could see the difference on bright
reference pixels (night scenes with streetlights). Daytime scenery stayed
dim because the source values are so low (≈0.1–0.2 linear) that even a 2×
leaves them at 0.2–0.4, still dim after sRGB encode, and any pixel above
0.5 linear started blowing out hard.

The ceiling is mathematical: **a linear multiplier cannot brighten midtones
without clipping highlights.** For Driveclub's daylight distribution, there
is no linear constant that wins across the frame. Rolled back.

### Static gamma pre-encode (pow curve)

One remaining lever that *can* lift darks without clipping whites: a
pre-encode gamma curve, `pow(color_linear, exponent)` with `exponent < 1.0`.
Tried 0.7, looked washed-out and flat. Tried 0.85 after I corrected my
intuition about the direction (smaller exponent = more aggressive lift, not
less). 0.85 was gentler but introduced visible **colour banding** in
gradients — the curve amplifies the 8-bit source's quantisation steps on its
way into a still-8-bit swapchain. The Bayer dither helps but can't hide the
larger steps a stronger curve produces.

I called it: washed-out image plus amplified banding is not worth a modest
midtone lift. Rolled back. Shader stays at standard sRGB encode + Bayer
dither.

### Where that leaves Driveclub

Driveclub was designed for the calibrated display pipeline a real PS4
provides (the system-level RGB-range / display-calibration settings Sony
added through the 4.0 and 7.0 firmware updates). Our Vulkan swapchain on a
Linux desktop is downstream of none of that. What the game writes to the
framebuffer is shown faithfully, but without the OS-side nudging a real PS4
applies on the way to HDMI-out.

The remaining options to actually brighten Driveclub daytime, all out of
scope for today:

1. **Implement proper HDR output in shadPS4.** The existing HDR path
   activates when the game declares HDR + monitor supports BT2020 PQ +
   `allowHDR` is true. Driveclub predates PS4 HDR (game shipped 2014, HDR
   arrived with firmware 4.0 in 2016), so it doesn't declare HDR.
   Retrofitting it would mean patching the game's rendering pipeline, not
   the emulator.
2. **Patch Driveclub's shaders / GI constants** to push daytime sun and
   ambient-light contributions. Multi-day reverse-engineering with
   low hours-per-constant yield.
3. **Accept the dim daytime look** as a Driveclub design choice. The
   in-game slider works. Ship.

We shipped option 3.

## The process, honestly

A few observations I want to write down while they're fresh, because this is
the part that tends to get airbrushed out of AI-assisted-coding write-ups.

**The git history is the receipt.** Eleven commits on `gamma-debug`,
spanning roughly 12:55 to 16:02 in their commit timestamps, with active
testing and rebuild cycles between each one. Actual wall-clock session was
longer — that's just the window between the first commit and the last. Of
those eleven, the ones I'd be happy to defend in code review are:

- The pipeline-cache config change (not a commit, just config).
- The param.sfo restore (runbook, not code).
- The MSAA depth → 1-sample colour resolve — genuinely new code, real
  bugfix, clean shape.
- The swapchain and format-mapping diagnostic `LOG_INFO` lines.
- The `sceVideoOutAdjustColor` / `SetGamma` trace logs.
- The Bayer dither in the present shader.

That's it. The ACES toolkit came in on commit 3 and came out on commit 8.
The auto-exposure compute pass came in and came out twice. The static
brightness boost and the gamma-lift pow curve both came in and came out
inside this session without a commit landing between them — I had the
assistant revert before pushing. Each of those attempts cost me an
iteration: build, deploy, launch Driveclub, load a track, check against a
reference, feed back the symptom, redirect.

**Heavy QA was the bottleneck, not the typing.** Claude Code wrote shaders,
wrote compute passes, wired up descriptor sets, kept Vulkan validation happy,
plumbed push-constants, and handled the non-trivial bits of the
shader-recompiler descriptor layouts without me breathing over its shoulder.
What it couldn't do was know whether the image on *my* monitor looked right.
That required me to launch the game, drive a couple of laps, screenshot
it, compare against reference footage in my head, and come back with a
specific observation: "the menu is bright but the race dims and doesn't
recover" — not a vibe, a concrete shape the assistant could act on.

**The assistant's enthusiasm is a liability when the premise is wrong.**
Several times during the tonemap chase, the assistant said things amounting
to "final working combination confirmed" when what had actually happened is
we'd found a setting where one symptom looked better and another looked
worse. I had to push back with "trust me when I say it's way too dark" and
"you've been stacking filters without validating each one — consolidate your
knowledge of what we have been doing so far". After that correction the
assistant did reset, audit what had accumulated, and iterate more carefully.
But I had to ask for it.

**Rollbacks are part of the job.** Probably a third of the lines the
assistant wrote today got reverted. That's not failure — that's what
investigation looks like when you don't know the shape of the bug in
advance. The failure mode would have been shipping the accumulated pile.
The cleanup commit on this branch (`video_core: drop the SDR tonemap
toolkit`) is one of the more important commits in the history, and it
removes more code than it adds.

**Even hours in, we're not "done".** Daytime Driveclub still looks dim.
There's a dark-translucent overlay I noticed right at the end of the
session, flashing off for a single frame on camera changes — almost
certainly the game's own temporal eye-adaptation post-process, not
something we introduced, but I didn't get to confirm against the vanilla
nightly today. Tomorrow's problem.

The question "could an AI agent have shipped this autonomously" has a
clear answer for this session: no. The agent couldn't have decided when
to stop trying, when to revert, when to question its own premise. Those
are judgment calls that came from me sitting in front of the game,
squinting at a screenshot, and going "no, still dim, and now the HUD is
blown out". That loop is the whole thing.

## Net result

The custom `gamma-debug` build over the upstream `main-2026-04-19` nightly:

- Driveclub v1.28 content actually accessible (runbook fix, not code).
- Pipeline cache enabled so cold launches don't stall (config, not code).
- 60 fps patch documented as the cause of slow-motion playback; disabled
  by default for stock-speed gameplay.
- Night tracks have screen-space lighting back — headlights, road surface,
  brake lights. MSAA-depth → 1-sample colour resolve, real code fix,
  upstream-shaped.
- 4×4 Bayer dither in the present shader to break 8-bit swapchain
  banding. Universal SDR win.
- Swapchain and format-mapping diagnostic logs.
- `sceVideoOutAdjustColor` / `SetGamma` trace logs.

Not shipped:

- Tonemap / ACES / exposure / auto-exposure / gamma-lift toolkit. All
  tried, all rolled back, all outlined here so the next person (me,
  tomorrow, or whoever) doesn't go up the same hill expecting a different
  outcome.

Driveclub is more playable than it was at the start of the day. That's
the deliverable. It is not "solved" — daytime scenes still look dimmer
than reference footage, and the camera-change flash hasn't been traced.
The investigation log (`docs/driveclub-v128-investigation.md`) has the
full technical detail for each phase, including what to try next if any
of these threads becomes worth re-opening.

If you're coming to AI-assisted coding expecting "point the agent at the
problem and come back to a fixed bug", reset that expectation. What you
actually get is a very fast pair programmer who types well, remembers a
lot, and will cheerfully implement wrong solutions to subtly-misframed
problems unless you stay in the loop. The value is real. The autonomy
is not — at least not yet, and not on problems like this one.
