## Phase 24 — The dim is upstream of tonemap

Phases 18–23 all attacked downstream surfaces: tonemap math,
exposure UBO scalars, histogram adaptation, tile-lighting
dispatches. Every one of those probes is gated and confirmed in
logs; none of them eliminated the blackout.

### Confirmed negatives (state of the art)

| Intervention | Confirmation | Visual result |
|---|---|---|
| Identity-tonemap (SPIR-V patch: three final `Fma` → `OpCopyObject` in `cs_0x000000002c918c06`) | patch installed at `~/.local/share/shadPS4/shader/patch/`, pipeline cache wiped, recompile observed in log | **No visible change** — dim still present on Munnar |
| Pin cb4[16] and cb4[8] to daylight values (`SHADPS4_DC_EXPOSURE_PIN=1 PIN_VALUE=3.0`) | `[dc-exppin]` log confirms writes hit the mapped ring-buffer SSBOs | Canada over-exposes to white, India tail-lights / mirror / sky **still dim** |
| Skip all 8 dim-only compute pipelines (`ShouldSkipDriveclubDispatch` auto-kill when `SHADPS4_DC_EXPOSURE_PIN=1`) | `[dc-dispatchskip] no-op dispatch` lines for each pipeline hash, first-seen submits logged | No blackout fix |
| Skip tile-lighting compute `0x000002da4ae7f686` (120×68×1, shader `0x000000002d9c66a5`) alone | `SHADPS4_DC_DISPATCH_SKIP=0x000002da4ae7f686 SHADPS4_DC_NUKE_AFTER_ARM=3` run 2026-04-22: log line `vk_rasterizer.cpp:1698 ShouldSkipDriveclubDispatch: [dc-dispatchskip] no-op dispatch pipeline=0x000002da4ae7f686 first-seen submit=1179` — skip is definitively active from submit 1179 onward | **No visible change** — dim unchanged, scene does not go unlit either. This pass is not load-bearing for the visible image and is not the dim source. |

### What this proves

The HDR render target is already dim **at the moment the tonemap
compute reads it**. Every probe that sits at tonemap or later
can only re-scale what is still in the buffer — it cannot
recover the information that was never written (or was written
as black) earlier in the frame.

Specifically:

1. The identity-tonemap patch proved the tonemap's own math
   is not reducing values. If it were, `Fma → OpCopyObject`
   would have brightened the output. It did not.
2. The cb4 pins only move the *ceiling* (over-exposure on
   bright tracks) — they cannot lift the *floor* (the dim
   values already sitting in HDR).
3. The 46656 scene-material draws per race produce HDR; some
   pass between those draws and tonemap is either attenuating
   HDR globally (atmospheric / fog) or leaving a load-bearing
   buffer (envmap / IBL probe) black.

### Symptom-to-mechanism mapping

| Symptom | What that implies |
|---|---|
| Mirror goes full black during blackout | A reflection source (planar reflection RT or env-probe cubemap) is black. Screen-space reflections can't recover information that isn't in HDR either. |
| Tail lights go from red to near-black | Emissive pixels are being attenuated post-emission. Emissive is normally additive — only a multiplicative extinction (fog/atmosphere) can dim it after it was written. |
| Sky goes dark | Sky is a cubemap sample or a distant skybox — both see the full atmospheric extinction path first. |
| Worst on Munnar 19:30 (sunset/dusk) | Sunset = longest atmospheric path + densest extinction in physically-based fog models. |
| Progressive darkening over minutes on other tracks | Time-of-day driver feeding the atmosphere/fog params is advancing — density or extinction coefficients accumulate or animate wrong. |
| Not a uniform scalar (some pixels mid-gray, some fully black) | Rules out "single exposure multiply". Rules in *distance-weighted* or *material-weighted* attenuation. |

### New probable path

Three upstream suspects, ordered by fit to symptoms:

1. **Atmospheric scattering / volumetric fog extinction pass.**
   Best fit — only hypothesis that explains *all* symptoms
   simultaneously (mirror black, tail lights dim, sky dark,
   time-of-day sensitivity, non-uniform dimming). Likely a
   compute that samples depth + per-pixel extinction and
   multiplies HDR in-place. Could also be a fullscreen draw.
2. **Reflection-probe / envmap update compute.** Explains
   mirror-black and diffuse-IBL ambient going dark, but does
   not on its own explain tail-light dimming unless emissive
   is separately IBL-modulated (uncommon).
3. **Pre-tonemap apply-exposure-into-HDR pass.** If the
   engine pre-multiplies HDR by the eye-adapt scalar before
   tonemap (Destiny-style pipeline), the dim is baked in at
   *that* compute, not at tonemap. This would also explain
   why pinning the scalar the tonemap reads does nothing —
   we'd be pinning a read that happens after the damage.

### Next probe: frame-order dump (`SHADPS4_DC_FRAMEORDER`)

Add a strict-submit-order log that records every graphics
draw and every compute dispatch for the first N submits after
race-window arm, with pipeline hash, shader hash, compute dim
or draw prim count, render target addr / primary SSBO
addresses, and a brief descriptor binding summary.

Purpose: walk backwards from the tonemap compute
(`0x000002c995517e7f`) to the last scene-material draw in a
single frame and enumerate every pipeline between them. The
true dim producer is somewhere in that list.

Usage:

    SHADPS4_DC_FRAMEORDER=2 SHADPS4_DC_NUKE_AFTER_ARM=3 \
      scripts/run_driveclub_overlay.sh

(2 submits ≈ 2 frames worth of ordered pipeline chain;
race-window gate ensures we skip the menu frames.)
