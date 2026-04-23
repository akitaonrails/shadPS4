## Phase 19b — dim-vs-lift dispatch diff (the first real hit)

After three failed attempts to random-fill compute UBOs, pivoted to
a dispatch-frequency diff: clean baseline run with DRAWLOG +
DISPATCHLOG, user reports wall-clock at blackout start and end, we
map those onto the `[dc-timeline]` heartbeat and split the session
into pre-race / dim / bright submit windows.

### The run

- emulator start (pidfile mtime): `17:15:52.48`
- blackout starts (user):         `17:16:20` → ~28 s into session → t≈28000 ms
- blackout lifts (user):           `17:16:50` → ~58 s into session → t≈58000 ms
- session ends (log mtime):        `17:17:17` → ~85 s wall-clock, t=76500 ms in the log

Dim window: 30 s. Bright window (post-lift): 18 s. Pre-race: 28 s.

Cross-referenced against the timeline heartbeat:

- dim starts at **submit ≈ 1418** (timeline t=28083 ms, frame=1575)
- dim ends at   **submit ≈ 2536** (timeline t=57966 ms, frame=3255)
- session ends  submit ≈ 3261 (t=76500 ms, frame=4365)

### Per-pipeline dispatch counts across the three windows

Parsed 158758 `[dc-dispatchlog]` entries, bucketed each into pre /
dim / bright by submit. Looked for pipelines that fire inside dim
and **do not fire at all inside bright**:

| pipeline                | shape        | pre | dim | bright |
|-------------------------|--------------|-----|-----|--------|
| `0x0000089fc5730348`    | (16, 1, 1)   | 420 | 961 | **0**  |
| `0x000008323ad915ad`    | (2, 2, 1)    | 420 | 961 | **0**  |
| `0x00000eea2b7d89d4`    | **(1, 1, 1)**| 420 | 961 | **0**  |
| `0x000002da4ae7f686`    | (120, 68, 1) | 43  | 164 | **0**  |
| `0x000006b667870fd4`    | (256, 256, 1)| 2   | 5   | **0**  |
| `0x00000bba6f0b7ff2`    | (128, 1, 1)  | 4   | 3   | **0**  |
| `0x00000495f24bd6da`    | (256, 128, 1)| 4   | 3   | **0**  |
| `0x00000f56cce1d724`    | **(1, 1, 1)**| 54  | 72  | **0**  |

### Why this is the first genuine hit

Every previous probe either changed **nothing** (texture content,
push constants) or changed **everything** (broad UBO smash, broad
compute smash). This diff finds eight specific compute pipelines
that *naturally* stop running the instant the blackout lifts. The
shapes alone tell the story:

- (120, 68, 1) = 1920/16 × 1080/16 — screen-space downsample at
  16× mip, classic bloom / luma first stage
- (256, 128, 1) = fullscreen tile grid
- (256, 256, 1) = 256² histogram / atlas fill
- (128, 1, 1), (16, 1, 1) = reductions along the linear axis
- (2, 2, 1) = 4-thread aggregate
- (1, 1, 1) **×2** = single-workgroup single-invocation writes —
  each of these is producing exactly one scalar, which is the
  textbook signature of an auto-exposure integrator

That chain — downsample → histogram → reduce → single-scalar
write — is **exactly** the HDR eye-adaptation pipeline every modern
engine runs. It stops when the adaptation converges and the
tonemap locks to a final exposure, which is what the user sees
when the blackout "lifts".

### What this means for every preceding phase

- Phase 13's luminance clamp on `0xf6e5670be11b0009` offset 3
  could not land because it was targeting a graphics pipeline that
  *consumed* the compute-written exposure, not the one that *wrote*
  it. Offset 3 was the live adapted-luminance value the fragment
  shader reads after compute has already set it; clamping the
  consumer doesn't stop the compute from re-writing the next frame.
- Phase 16's texture nukes never shifted the dim because no
  texture drives it — the live exposure value is written to a
  storage buffer by compute, not encoded in any sampled asset.
- Phase 17's graphics UBO smash mostly rolled through without
  hitting it because the dim scalar lives in a compute-written SSBO
  that the graphics pipelines read as a descriptor, and our
  filters / size brackets kept missing the exact buffer.
- Phase 19's compute UBO random-fill kept crashing because those
  same UBOs carry descriptor pointers; nuking them SIGTRAPs before
  the single-scalar output buffer ever gets a chance to be written.

The exhaustion chain wasn't wasted — it forced us to *prove*
everything else was clean, which is what made this diff readable
in the first place.

### Planned Phase 20 — dispatch skip

Next step is a surgical no-op: add a `SHADPS4_DC_DISPATCH_SKIP=
0xHH,0xII,...` env var that intercepts `Rasterizer::DispatchDirect`
and `DispatchIndirect`, and if the current compute pipeline hash is
in the skip list, returns before `cmdbuf.dispatch(...)` runs. The
dispatch becomes a pure no-op.

Test sequence (no rebuilds needed between runs):

1. Skip both (1,1,1) writers:
   `SHADPS4_DC_DISPATCH_SKIP=0x00000eea2b7d89d4,0x00000f56cce1d724`
   — expected: adaptation stops updating, scene stays at whatever
   the previous-written exposure value was. If blackout never
   develops on race start, those scalars are the dim driver.
2. Skip the three top-firing ones (16,1,1) + (2,2,1) + (1,1,1):
   `SHADPS4_DC_DISPATCH_SKIP=0x0000089fc5730348,0x000008323ad915ad,0x00000eea2b7d89d4`
   — expected: breaks the reduction chain earlier. If skipping
   just the scalar writer isn't enough but the chain works, this
   confirms multi-stage.
3. Skip all 8. Sanity check — what's the maximum surface area we
   can take out without crashing / breaking something unrelated.

The difference between blackout-still-happens-then-stops vs
blackout-never-starts vs scene-stays-frozen-at-a-fixed-exposure
will tell us precisely which role each dispatch plays in the
adaptation loop.
