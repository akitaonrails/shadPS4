## Phase 25 — Frame-order trace (the pre-tonemap chain is mapped)

The `SHADPS4_DC_FRAMEORDER` dump added in Phase 24 captured 2009
ordered events over two gated submits on Munnar India 19:30
(blackout phase). Walking the trace gives us, for the first
time, the exact chain of passes sitting between the last
G-buffer draw and the tonemap compute.

Submit 1023: 279 events. A short submit — likely the tail of the
previous game frame's post-FX.

Submit 1024: 1726 events. The full race frame. Scene-material
draws occupy seq ~0..1455; the tail (seq 1456..1473) is the
pre-tonemap chain we care about.

### The pre-tonemap chain (submit 1024, seq 1455 → 1473)

| seq  | kind | pipeline                | shader           | shape / RTs                                  | hypothesis |
|------|------|-------------------------|------------------|----------------------------------------------|---|
| 1446–1455 | draw | many (scene materials) | —                | 4-RT MRT into `0x500cdd..0x500abe..0x500d5c8..0x5009688`, depth=yes | G-buffer fill (4 attachments + depth) |
| 1456 | draw | `0xe6e42747689b5d99`    | —                | 1 RT (`0x500cdd0000`), idx=4                 | G-buffer finalize / degenerate |
| **1457** | **disp** | **`0x00000f95971a22dd`** | **`0x00000000f94c089f`** | **(60,34,1) — half-res tile grid**          | **Suspect A: half-res compute (SSAO / SSR / tile-light cluster)** |
| 1458 | draw | `0xcbd9e605ba011568`    | —                | rts=`<none>`, depth=yes, idx=4               | resource sync / barrier draw |
| 1459 | draw | `0xfc5d9a7665a22012`    | —                | rts=`<none>`, depth=yes, idx=3               | resource sync / barrier draw |
| **1460** | **disp** | **`0x00000663dc6eb328`** | **`0x0000000066315c56`** | **(256,1,1)**                               | **Suspect B, stage 1 — progressive reduction** |
| **1461** | **disp** | **`0x00000663dc6eb328`** | (same)           | **(2560,1,1)**                              | **Suspect B, stage 2 — 10× expansion** |
| **1462** | **disp** | **`0x00000663dc6eb328`** | (same)           | **(10240,1,1)**                             | **Suspect B, stage 3 — 40× expansion. Canonical volumetric froxel integrate or light-list reduction.** |
| 1463 | draw | `0xfc5d9a7665a22012`    | —                | rts=`<none>`, idx=3                          | sync draw |
| 1464 | draw | `0xc00021913c1e4aaf`    | —                | `0x5012810000`, depth=no, idx=3              | post-FX fullscreen → small RT |
| 1465 | draw | `0x8a469a0b828e81bc`    | —                | `0x5012a50000`, depth=yes, idx=3             | post-FX fullscreen |
| 1466 | draw | `0x1b0c2d3e9836d8b2`    | —                | `0x5013b70000`, depth=no, idx=3              | post-FX fullscreen |
| 1467 | draw | `0x0855e31e96524a96`    | —                | `0x5012b90000`, depth=no, idx=3              | post-FX fullscreen |
| 1468 | draw | `0x540554bec2838c67`    | —                | `0x5012a50000`, depth=yes, idx=3             | post-FX fullscreen |
| 1469 | draw | `0xadd2ec4587065da9`    | —                | `0x500cdd0000`, depth=yes, idx=3             | post-FX back into G-buffer RT0 |
| 1470 | disp | `0x000005bd4236206f`    | `0x000000005bc75fd9` | (1,1,1) single thread                      | scalar reduction commit |
| **1471** | **draw** | **`0x7e5d7e5e5abfc092`** | —        | **2 RTs (`0x50105c8000,0x5011b20000`)**, depth=no, idx=3 | **Suspect C: dual-output fullscreen apply — "apply fog/bloom into HDR"** |
| 1472 | disp | `0x00000bfd30824fac`    | `0x00000000bfce517c` | (15,9,1) coarse tile grid                  | per-tile exposure / adaptive curve |
| **1473** | **disp** | **`0x000002c995517e7f`** | **`0x000000002c918c06`** | (120,68,1) — full-screen tile              | **TONEMAP** |
| 1474+ | draw | HUD / UI                | —                | `0x5008130000`                               | post-tonemap overlay |

None of these five suspects (A/B/C, plus the single-thread 1470 and
the coarse-tile 1472) are in the Phase 23 dim-only auto-kill set
`{0x0000089fc5730348, 0x000008323ad915ad, 0x00000eea2b7d89d4,
0x000002da4ae7f686, 0x000002f76d33c858, 0x000006b667870fd4,
0x000000a556b3c66b, 0x00000f56cce1d724}`. That is why the exposure
pin leaves the dim intact: the auto-kill only covers histogram
adaptation, not the atmospheric / post-FX / apply chain.

### Suspect ranking

1. **Suspect B — `0x00000663dc6eb328`, seq 1460–1462, progressive
   (256,1,1) → (2560,1,1) → (10240,1,1).** Strongest signal.
   Progressive 10× expansion of a single shader is the canonical
   Frostbite-style froxel pipeline (inject lights → scatter →
   integrate along depth slices). If this writes the extinction
   / inscatter volume wrong, every surface that samples the
   volume later is darkened. Matches all symptoms:
   - Worst on sunset track (thickest atmosphere).
   - Progressive worsening (volume accumulates state frame over
     frame).
   - Mirror black (reflection path samples the same fog volume).
   - Sky dark (sky fullscreen samples fog extinction).
   - Tail lights dim (emissive is multiplied by transmittance
     along the camera ray).

2. **Suspect C — `0x7e5d7e5e5abfc092`, seq 1471, dual-RT
   fullscreen apply.** Sits one step before tonemap. If this is
   where the volume integration result is multiplied into HDR
   (classic "apply fog" fullscreen pass), killing it stops the
   damage at the last possible moment before tonemap.

3. **Suspect A — `0x00000f95971a22dd`, seq 1457, (60,34,1)
   half-res tile compute.** Could be SSAO; if miscalibrated SSAO
   overshoots, it attenuates everything. Weaker fit for the
   non-uniform pattern (SSAO doesn't specifically black out
   mirrors).

4. `0x00000bfd30824fac` — seq 1472 (15,9,1) coarse tile. Adaptive
   tone-curve candidate; would affect tonemap output but not HDR
   contents directly.

5. `0x000005bd4236206f` — seq 1470 (1,1,1) single-thread
   reduction. Probably an exposure scalar commit; Phase 23
   already ruled out the read-side of that scalar.

### Next probe: skip Suspect B (volumetric chain) in isolation

    SHADPS4_DC_DISPATCH_SKIP=0x00000663dc6eb328 \
      SHADPS4_DC_NUKE_AFTER_ARM=3 \
      scripts/run_driveclub_overlay.sh

Expected outcomes:
- **If fog volume is the dim producer:** blackout lifts on India,
  the sky / mirror / tail-lights come back to color. Possibly
  visible atmospheric haze is gone (fine — we want to eliminate
  the broken mechanism, not replace it).
- **If it is not:** no visual change; move to Suspect C (draw
  skip `0x7e5d7e5e5abfc092`) next.

If Suspect B lifts the blackout, the follow-up is to verify on
Canada / Japan (which should stay bright without over-exposure,
since we are not compensating downstream). If the fix holds
cross-track, we have elimination.
