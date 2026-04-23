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
