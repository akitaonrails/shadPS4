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
