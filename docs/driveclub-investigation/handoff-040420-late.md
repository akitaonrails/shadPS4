## Handoff status (2026-04-20, late)

This section is the current state after several more hours of probing on
`gamma-debug`. It supersedes the earlier optimism around one specific
surface fix.

### The hard conclusions

- The race-start blackout is **not**:
  - final present / swapchain gamma
  - `sceVideoOutAdjustColor`
  - QtLauncher / host display output
  - degamma / min-max blend / predication
- It is inside the game's **internal scene HDR / composite path**.
- HUD staying correct while the world blacks out is a stable discriminator:
  the UI path is fine; the world-composite branch is not.

### The key full-res surfaces

These are the only targets that kept mattering in every useful run:

- `0x500cdd0000` — `1920x1080` `R8G8B8A8Srgb`
  - visible world composite target
  - this is the thing that visibly goes black
- `0x5009688000` — `1920x1080` `B10G11R11UfloatPack32`
  - main HDR-like scene branch
- `0x5008130000` — `1920x1080` `B10G11R11UfloatPack32`
  - alternate HDR-like scene branch
- `0x500fdd0000` — `1920x1080` `B10G11R11UfloatPack32`
  - another HDR-like temporal/composite branch
- `0x500ddc0000` — `1920x1080` `R8Srgb`
  - auxiliary visible/mask branch that often stays alive
- `0x50105c8000` — `1920x1080` `R16G16Sfloat`
  - another stable scene-side intermediate

### What the stats now say

Across repeated clean baseline runs:

- `0x500cdd0000` repeatedly drops to zero during the blackout.
- `0x5009688000` often drops with it, but not always at the exact same
  moment.
- `0x5008130000` sometimes stays alive while `0x500cdd0000` is black,
  and sometimes later also dies.
- `0x500fdd0000` frequently stays healthy and can even become brighter
  while the visible target is black.
- `0x500ddc0000` and `0x50105c8000` often stay alive through the
  blackout.

That means the bug no longer looks like "one buffer dies and takes the
rest with it". It looks more like a broken handoff or dependency between
multiple HDR branches before the final visible composite.

### What the aggressive probes accomplished

The aggressive phase was still useful, even though it did not produce a
fix:

- Freezing `0x500cdd0000` changed the symptom immediately.
  - That proved it is on the critical visible path.
  - But freezing it mostly produced a stale full-screen world plate with
    HUD still updating on top.
- Freezing HDR branches (`0x5009688000`, `0x5008130000`, `0x500fdd0000`)
  also produced static-world / stale-plate symptoms.
  - That proved those surfaces are temporal/history/composite feeders.
  - But it stopped being diagnostic after a point, because "static
    postcard" only means "we pinned a temporal HDR branch", not which
    pass is actually wrong in the real unfrozen run.
- Broad compute sabotage proved the correct subsystem:
  destroying the compute-heavy scene branch killed the world while the
  HUD survived.
  But single-hash and small-group sabotage did not isolate a culprit
  reliably.

### What we tried and should stop retrying blindly

- Surface freeze as the primary diagnostic.
  - It has reached diminishing returns.
  - It keeps proving "temporal HDR branch" without telling us which
    *live* pass is the real source of the blackout.
- Early hot-path skip experiments on draw/dispatch writers.
  - Multiple attempts caused boot-time GPU crashes before any useful
    race data was collected.
- Small sampled-input substitution on the `0x500cdd0000` composite path.
  - The substitutions failed technically as an isolator and never
    produced a clean directional result.

### Current baseline on `gamma-debug`

As of this handoff, the branch is back to a **plain non-freeze,
non-sabotage, booting baseline** with the useful passive stats still in
place:

- `SceneFreezeMode` is effectively `none`
- `scene-sabotage` is `none`
- the build boots and reproduces the real behaviour again
- the world blacks out for 20+ seconds and then returns, matching
  vanilla baseline

### The fastest reasonable next step

Do **not** go back to more surface-freeze variants first.

The next probe should be:

1. **Passive**
   - no skipped draws
   - no frozen surfaces
   - no writer sabotage
2. **Late-start**
   - arm only after the visible world has been healthy for a short
     stable window
3. **Restricted to the full-res HDR/visible branch**
   - `0x500cdd0000`
   - `0x5009688000`
   - `0x5008130000`
   - `0x500fdd0000`
   - `0x500ddc0000`
   - `0x50105c8000`
4. **Sequence-oriented**
   - capture the exact pass order and shader hashes during the
     transition from healthy frame -> blackout frame
   - ideally with a slightly longer logging window than the previous
   - ideally with a slightly longer logging window than the previous
     late-start trace attempt

The current best hypothesis is:

- not "one bad output gamma path"
- not "one dead sampled texture"
- not "one buffer always dies first"
- but a broken transition inside the full-res HDR composite/exposure/
  history chain before `0x500cdd0000`

That is where the next round should spend its budget.
