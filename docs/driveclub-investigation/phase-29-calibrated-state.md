## Phase 29 — The calibrated-state snapshot

### Methodology

Built a consolidated recording harness `SHADPS4_DC_RECORD=1` that:

- Creates a persistent session dir at
  `~/Pictures/driveclub-runs/run-YYYYMMDD_HHMMSS/` (distrobox HOME).
- Every ~120 submits, dumps the three known lighting UBOs (1936,
  1008, 224 bytes) to `snap_HH:MM:SS.mmm_subNNNNNN_<size>.bin`.
- Every ~60 submits logs wall-clock-timestamped lines with key
  slot values.
- Cross-correlates against user-noted wall-clock timestamps of
  visible state transitions (race-start / pitch-black / recalibrated).

### Canada race, timelapse +30 — user-noted state transitions

| wall-clock | submit | visible state |
|---|---|---|
| 11:20:08 | ~1097 | race start — scene visibly dim |
| 11:21:29 | ~4770 | pitch black — unplayable |
| 11:21:48 | ~5610 | auto-recalibrated — correct colors |

Screenshots taken at each moment in
`~/Pictures/Screenshots/screenshot-*.png`.

### What the 1936-byte scene-lighting UBO contains over time

All values are what the game / emulator has written into the UBO
that scene-material shaders read.

**Ambient slots `[24..27]`** (earlier labelled "sun" — they drive
mountain-top / sun-facing surfaces):

    race-start (11:20:08):   89.3 / 71.1 / 33.9 / 13.6  (big, natural daytime)
    pitch-black (11:21:29):  0    / 0    / 0    / 0     (fully decayed with TOD)
    recalibrated (11:21:48): 0    / 0    / 0    / 0     (still zero)

These don't come back at recalibration — the "correct" look after
1:30 is NOT driven by these slots being high.

**Slots `[38] [48] [50]`** (I was calling these "fade"):

    race-start:             8-40   / 7-37  / 3-16      (oscillating, small)
    pitch-black:           466-509 / 429-468 / 182-199 (climbing)
    recalibrated:         606-638 / 557-587 / 237-249  (continuing to climb)

These are **monotonic positive**, not a fade-down-fade-up curve.
They reach ~75× their race-start value by the recalibration
moment. These look like **auto-exposure / HDR scale** terms that
need to be *high* for the scene to look correct.

**Slots `[144..295]` — uninitialized garbage at race start**

This is the smoking gun. Diff of the 1936 UBO between pitch-black
(submit 4770) and recalibrated (submit 5610) reveals dozens of
slots that were denormal / near-zero / uninitialized up until the
recalibration event, at which point the game writes real values:

| slot | pitch-black | recalibrated | likely role |
|---|---|---|---|
| [112] | 1.4e-45 | **7.03e+05** | scale factor / exposure |
| [144] | 4e-38 | **1.0** | flag |
| [145] | 1.1e-44 | **83** | scalar intensity |
| [147] | 0.42 | **3e+05** | auto-exposure scalar |
| [176] | 1.4e-45 | **557** | ambient R? |
| [178] | 1.8e-26 | **237** | ambient B? |
| [240] | 7e+05 | 851 | decreases |
| [241] | 3e-41 | **170** | scalar |
| [242] | 1e-35 | **2098** | HDR sun intensity? |
| [245] | 9.8e-45 | **170** | scalar |
| [246] | 8.5e-39 | **2108** | HDR intensity |
| [248-250] | 2.775 / 2.775 / 2.775 | **837 / 170 / 2113** | RGB sun HDR |
| [284-287] | 0 / 0 / 0 / 0 | **1.5 / 1.5 / 1.5 / 1.5** | constant 4-vector |
| [288-290] | 0.61 / 0.33 / 0.16 | **1.0 / 1.0 / 1.0** | saturated unit |
| [293-295] | 6 / 492 / ~0 | **0.66 / 0.66 / 0.66** | triplet |
| [312] | 0 | **7.03e+05** | exposure again |

### Interpretation

The 1936-byte scene-lighting UBO has **two classes of slots**:

1. **Continuously animated** (ambient [24..27] decaying with TOD,
   fade [38][48][50] monotonically climbing). These update every
   frame with real values from the start.
2. **One-shot initialised** (the ~30 slots above in [144..295]).
   On real PS4 these are populated very early in the frame
   schedule, before the first visible frame. On shadPS4 the
   population is **delayed by ~90 seconds** (varying with
   timelapse setting), during which the game renders with
   partially-uninitialised data — producing the dim-then-pitch-
   black-then-recover arc.

Until those slots hit real values, scene-material FS shaders
reading them get zero / NaN / denormal contributions to their
lighting math, producing the visibly-dim scene.

### Why this is an emulator bug

The game runs correctly on PS4 hardware. On PS4 the slots are
populated before the first rendered frame. shadPS4 is delaying
some compute pass / CPU-side write / memory sync that initialises
them.

Root-cause fix: find the upstream initialisation step (compute,
CPU write, or page-fault sync) that populates the one-shot slots
and make sure shadPS4 runs it promptly at race load.

### Pragmatic bandage — "calibrate at arm" pin

Short-term: we have a byte-perfect snapshot of the 1936 UBO in
its recalibrated state (`tools/driveclub_pin_snapshots/canada_
calibrated_1936.bin`, 1936 bytes). On the very first race-arm-
qualified `BindBuffers` call for a 1936-byte scene UBO, memcpy
this snapshot over the game's partially-initialised bytes. The
game then takes over naturally — the continuously-animated slots
update every frame (no conflict), and the one-shot slots were
populated by us so the scene renders correctly from the start.

Fires **once per race-arm bump** (so re-entering a race resets the
state and re-applies).

Env:

    SHADPS4_DC_CALIBRATE_AT_ARM=1
    SHADPS4_DC_CALIBRATE_FILE=tools/driveclub_pin_snapshots/canada_calibrated_1936.bin

Next steps:

- Verify the calibrate-at-arm pin produces a correct-looking scene
  from race start on Canada.
- Capture calibrated snapshots for other tracks (Munnar, Japan,
  Norway) and select by track (via a user-set per-track env or
  auto-detect from an in-memory marker).
- In parallel: investigate the actual upstream shadPS4 gap that
  delays the one-shot slot initialisation. Candidates include:
  CPU-side writes that aren't triggering page-fault sync, or
  a compute shader that does the one-shot write and is being
  mis-identified / delayed.
