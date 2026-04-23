## Phase 19c — dispatch and draw skip tests (null across the board)

After Phase 19b identified 8 compute pipelines and 101 graphics
pipelines that fire during dim and go silent at lift, we built
`ShouldSkipDriveclubDispatch` / `ShouldSkipDriveclubDraw` to turn
matching pipelines into no-ops via two new env knobs:

- `SHADPS4_DC_DISPATCH_SKIP=0xHH,0xII,…`
- `SHADPS4_DC_DRAW_SKIP=0xHH,0xII,…`

Both gated by the race window + `NUKE_AFTER_ARM` so pre-race draws
that share a pipeline hash stay intact.

Test matrix, all under the `NUKE_AFTER_ARM=3` gate so only
post-race-load dispatches/draws get skipped:

| # | skipped                                          | observed                      |
|---|--------------------------------------------------|-------------------------------|
| 1 | 2 (1,1,1) compute scalar writers                  | baseline, blackout 7 s lift   |
| 2 | all 8 dim-only compute pipelines                  | baseline + HUD timer glitch, blackout 7 s |
| 3 | 4 HDR-writing dim-only graphics draws             | baseline, blackout 10 s       |
| 4 | highest-rate dim-only draw (0x4a8b..e10b, 513×)   | baseline, blackout 25 s       |
| 5 | **all 101 dim-only graphics draws**                | baseline, blackout 24 s lift  |

Result across the matrix: **the dim runs on its own schedule
regardless**. Not one of the pipelines that visibly stops at lift
is the thing that causes the lift.

### Durable conclusion

The 8 dim-only compute + 101 dim-only graphics pipelines are
*correlated* with the dim phase — they happen while adaptation is
in progress and stop when it converges. But they're downstream
effects, not the driver. Taking every one of them out doesn't
change when or how the dim ends.

Combined with Phases 16–18 ruling out texture content, UBO random
fill, and push constants, the honest frame is:

- The dim is somewhere in shader-reachable state, **but it's
  co-located with load-bearing state** (matrices, descriptor
  pointers) that crashes when the probe touches it. Random-fill
  is too coarse to separate the dim field from the crash fields.
- **Or** the dim lives in state that is not part of the per-draw
  Vulkan surface at all — pipeline-creation-time state, blend
  constants pulled from registers, scanout metadata, compute
  output buffers that flow through non-descriptor paths, etc.

Every probe we can build with env-var knobs has been tried.
Meaningful new runtime probes would require:

- per-offset UBO surgery (one 4-byte flip per run; dozens of
  runs)
- a fixed-function state diff harness (dump blend state per draw
  across dim / bright submits)
- a compute-output replay probe (overwrite SSBO after dispatch
  rather than before)

All three are real work, none are env-var toggles.

### Commit state at end of Phase 19c

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` carries:

- `DriveclubDrawKind::Compute` + its mask bit
- `IsDcDispatchLogEnabled` / `NoteDriveclubDispatchlog`
- `GetDcDispatchSkipPipes` / `ShouldSkipDriveclubDispatch` +
  call-site at both Direct and Indirect dispatches
- `GetDcDrawSkipPipes` / `ShouldSkipDriveclubDraw` + call-site at
  both Draw and DrawIndirect

`scripts/run_driveclub_overlay.sh` passes through
`SHADPS4_DC_DISPATCHLOG`, `SHADPS4_DC_DISPATCH_SKIP`, and
`SHADPS4_DC_DRAW_SKIP`.

Zero emulator behaviour change when none of the env vars are set.

A full baseline run log (pre-race + dim + lift + bright, 100 MB)
is archived at
`tmp/dc-logs/baseline-dim-lift_17-24-06_start_17-24-22_end.txt`
so the diff analysis can be re-run or extended without needing the
user to capture another wall-clock-timed session.
