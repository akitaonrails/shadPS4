## Phase 10 — 2026-04-22 rethink: what the last day actually proved

This section exists to prevent another loop through adjacent
`CameraFade` / UI / late-render branches.

### Clean misses and what they mean

1. **Late rendered/image tree is not the blackout master switch.**
   The full chain around:
   - late black carrier `0x1c2223026ec47d56`
   - `0x5000108000`, `0x5000900000`
   - `0x5008130000`, `0x5009688000`, `0x50105c8000`
   - the parallel `0x500fdd / 0x5015ae / 0x501e8f` branch
   - the tiny `32x32` side input branch
   
   was stressed heavily. We changed:
   - what image appears under blackout
   - overall darkness
   - whether the main view was black while HUD survived
   
   But **mirror blackout/recovery still behaved normally**. Therefore
   this tree is a carrier/composite path for the main view, not the
   shared blackout timer/gate.

2. **Track-local and local prerace packs are adjacent only.**
   `india_road_point_02_n.rpk`, `india_landscape_gui.rpk`, local
   prerace camera fades, local postfx scalars, local animlib edits:
   all of these changed the latched image or the darkness underneath
   blackout, but never removed the blackout itself.

3. **`globaldata.rpk` is probably relevant, but direct patching is not
   a usable path.**
   Even narrow `globaldata` edits (fade-only, RTT/grading-only, shared
   postfx actor edits, single named fade-ish edits) repeatedly caused
   black boot / hang / crash before race load. So:
   - `globaldata` is still a plausible shared layer
   - but brute-force pack patching there is too unstable to teach us
     anything clean

4. **Prerace UI/page families are not the blackout carrier.**
   Replacing or rerouting:
   - `FreeplayGetInCar`
   - `PreraceWaiting`
   - `PreraceCountdown`
   - `get_in_car_animation`
   - `vehicle_select_background`
   - `loading_freeplay`
   - `loading_india.rpk`
   - `vehicle_previews.rpk`
   
   either did nothing, broke handoff flow, or only changed decorative
   layers. The actual blackout persisted.

5. **Safe `newui` fade-control assets are a clean miss.**
   We widened `newui/control/transitions.ctl` from a narrow alpha-track
   flatten to the whole safe fade family:
   - `FadeUp`, `FadeOut`
   - `ZoomInAndFade`, `ZoomOutAndFade`
   - `FaceOffFadeIn`, `FaceOffFadeOut`
   - `OverdriveTextFadeIn`, `OverdriveTextFadeOut`
   - `RG_ScrollReady*`, `RG_ScrollGo*`, `RG_FadeBG*`
   - `LiveColumnFade`
   - `ResultIn`, `ResultOut`
   
   and rerouted the live race-path panels:
   - `loading_freeplay.txt`
   - `x_freeplay.txt`
   - `x_live_lobby_pre_race.txt`
   
   to a no-op `NoFade` transition. Result: **baseline blackout**.
   Therefore the safe declarative `newui` fade layer is not the switch.

6. **String-adjacent camera/fade event names are not the switch.**
   Broad and narrow experiments around:
   - `CameraFade`
   - `SetCamera`
   - `ScreenFadeOut`
   - `StartGame`
   - `AutoPilot`
   - `GarageEnable`
   
   produced only:
   - much darker starts
   - broken race launch handoff
   - missing UI strings
   - boot breakage
   
   but never disabled the blackout. The important lesson is not just
   that these names missed; it is that **name-based/static string
   patching is too indirect**.

7. **A real code-side `ScreenFadeOut` store was identified and was a
   clean miss.**
   The direct handler/store at the inner-ELF function around
   `0x146640`, with the matched overlay raw-store at `0x14a847`, was
   NOPed by signature. Result: **baseline blackout**. So the obvious
   explicit `ScreenFadeOut` config/store is not the controlling branch.

### What we were fundamentally assuming wrong

For most of the day, the working model was still:

- "There is a specific fade effect, fade asset, or fade-named handler."
- "If we corrupt enough things called fade/camera/postfx, we should hit it."

That model is now too weak.

The evidence fits a different model better:

- the blackout is probably **not a named fade effect at all**
- it is more likely a **stateful shared visibility gate or scene
  ownership transition**
- the gate acts **before main view and mirror split**
- the explicit fade/camera/UI layers we can see are only consumers or
  decorations around that state

That explains why:
- HUD survives
- mirror is the best blackout oracle
- the main view can be made black/dark/broken without touching the real
  blackout timing
- every declarative fade family keeps missing

### What is actually missing from the investigation

We have traced:
- assets by name
- UI control files by name
- strings in the binary by name
- late render dependencies by image address

What we have **not** traced is the thing that matters most:

- the **actual runtime state transition** that decides whether gameplay
  cameras are allowed to present the world yet

Concretely, the missing classes of probe are:

1. **Live dispatch/call tracing, not string matching.**
   We need the real callsites/handlers executing during:
   - car select confirm
   - race load begin
   - blackout start
   - blackout end / mirror recovery
   
   Names like `CameraFade` and `ScreenFadeOut` were not enough.

2. **Shared pre-split camera/view ownership state.**
   Anything that governs:
   - prerace camera active vs gameplay camera active
   - visibility ownership / handoff / "ready" state
   - gating of scene presentation to mirror and main view together

3. **Dynamic dispatch targets or message IDs, not the static strings
   that happen to sit nearby in `.rodata`.**
   The day repeatedly showed that patching visible strings only hits
   adjacent registration/config plumbing.

### Best next direction from this floor

No more blind asset shotguns and no more string-token corruption.

The next step should be a **runtime trace** of the actual prerace →
gameplay handoff dispatch:
- capture the live functions/message IDs/callbacks that fire between
  car select and blackout start
- capture the matching ones that fire when blackout lifts / mirror
  recovers
- patch only those exact live dispatches once identified

If we cannot trace at that level, then we are still guessing around the
edges, no matter how broad the shotgun is.

### New working hypothesis (2026-04-22)

The blackout is probably **not a fade effect** in the ordinary sense.

It is more likely a **shared state-machine gate** that controls when the
gameplay world is allowed to present after the prerace flow hands off to
the real race. The visible fade-like behavior is only how that gate
manifests on screen.

That model fits all the clean misses:
- UI fades do nothing
- explicit `ScreenFadeOut` handling does nothing
- local track/postfx changes only alter the image under blackout
- late render/composite trees only alter the carrier image
- mirror remains the best oracle for the real gate

The likely structure is:
1. menu / vehicle-select controller state
2. prerace panorama / confirmation state
3. race-load state
4. gameplay world loaded but presentation still gated
5. shared gate opens
6. main view and mirror both recover

### Exact user-observed runtime sequence to trace

This is the sequence that must be traced in code, not inferred from
asset names:

1. **Car select**
2. **Paint selection**
3. **Confirm**
4. **Brief world panorama with "start event" confirmation**
5. **Race loads**
6. **Cars visible on starting grid**
7. **Blackout fades in**
8. **After a delay, blackout fades out**
9. **Mirror comes back with the world**

Important consequence:
- the blackout occurs **after** the race world is already present
- it is therefore not just "loading screen still visible"
- and not just "wrong prerace image"

### Canonical baseline timeline (2026-04-22, user verbatim)

Exact step-by-step of what the **unpatched** game does at race start,
as observed in repeatable repro at India tracks, 19:30 clear:

1. Car selection menu.
2. Paint selection.
3. Brief world panorama animates and presents a "start event"
   confirmation.
4. User presses X.
5. Race loads and prepares.
6. **For ~1 second the track is fully visible** — cars on grid
   waiting for the green light, mirror working, scene in colour.
7. **Blackout fades in**, darkens the whole scene to almost black.
   Cars are **still faintly visible underneath**, especially the red
   tail lights moving. Mirror goes **pure black** (no structure).
8. Wait 10–30 s (variable, sometimes never recovers).
9. Blackout fades out, mirror re-populates with the world, scene
   lifts back to colour.
10. Gameplay is responsive.

Three details in step 7 are load-bearing:
- main view is **dim but not zero** — HDR content like tail lights
  survives the darkening, so the main scene render pass is still
  executing and writing real values
- mirror is **actually zero** — not dim, not faint, no shape visible
  at all
- HUD and minimap are **always fully intact** at their normal
  brightness and colour, painted on top of whatever the scene layer
  is doing. They are not gated by the blackout at all.

That is not "exposure is low" alone. A single low-exposure multiplier
applied to both the main HDR target and the mirror target would leave
**both** with faint structure, not one dim and one black. The mirror
being cliff-edge black while the main view is dim-but-visible means
**two different mechanisms are in play simultaneously**:

- main view: a multiplicative darkening, either by exposure value or
  by a near-opaque black composite that still lets bright HDR pixels
  burn through
- mirror: the mirror *render pass itself* is suppressed, or its
  output is overwritten with zero, or its source camera is disabled

And the HUD/minimap being unaffected in the same frames means a third
fact: **the gate does not gate the UI/HUD composite path at all**.
The final frame is clearly a composite of:

- a 3D world layer that is currently darkened
- a mirror reflection layer that is currently zeroed
- a HUD/minimap layer that is currently full-normal

All three happen in the same frame. Only the first two are gated.

That three-way split — main dim, mirror zero, HUD fine — is the
signature of a **shared state-machine gate** governing the 3D world
presentation subsystem (codex's 2026-04-22 hypothesis), not of any
single named fade effect and not of the emulator's final present.

### Comparison of the two working models

Codex's gate model and the earlier init-delay hypothesis can both be
held against this timeline:

| Model | Predicts step 6 (track visible) | Predicts step 7 (main dim + mirror black) | Predicts step 9 (shared recovery) | Verdict |
|---|---|---|---|---|
| **Codex state-machine gate** | World is renderable, not yet gate-allowed to be *fully* presented | Gate closes: suppresses mirror pass entirely, multiplicatively darkens main | Gate opens, both subsystems re-enable together | Fits cleanly |
| **Init-delay / unconverged scene** | World not yet rendered; no track visible on frame 1 | N/A — would predict dim *before* cars appear, not *after* | N/A — init has no "close and re-open" motion | Rejected by step 6 |

The init model is now out. The cars-visible-then-darken sequence is
not what an init delay looks like. The gate model is what remains.

Refinement on the gate model from this observation:

- the gate is not a single visual effect; it's **one signal driving
  at least two subsystems simultaneously** (main-view darken +
  mirror suppress).
- whatever the game code checks to decide "gate open" has to be the
  same flag read by both the mirror pipeline and the main-view
  tonemap / composite path.
- that shared flag is the real target. Binary-patching it to always
  read "open" is what would actually fix the blackout, independent
  of any visual-layer patches.

### Why 3 days of patches keep missing

Everything we patched so far lived **past** the gate:

- UI panels → consumer of the gate's presentation permission
- PostFXConfig scalars → tuning of the already-permitted pipeline
- LUT swaps → colour mapping inside the already-permitted pipeline
- prerace camera Fade tracks → animation driven at the gate-closed end
- TXAA / NonBloomed → decay shape of the overlay, not its source
- ScreenFadeOut NOP, CameraFade string corruption → decorations

None of these can reach a flag that is evaluated before the main
render subsystems even decide to run. We were dialling parameters on
systems the gate was actively holding shut.

Every "we changed how something looks" success reinforces the gate
model: whatever the gate gates, we're allowed to tune *after* it
decides to open. Before it decides, we can change nothing visible,
because the gate won't let us.

### Concrete shape of the next probe

Codex already has this in the later sections: runtime dispatch trace
instead of more declarative shotguns. This observation sharpens what
to look for:

- **one shared flag-read** that fires both at step 7 (gate close) and
  at step 8 (gate open), checked simultaneously by
  - the mirror render pass (enabled / skipped)
  - the main-view scene darkening (exposure driven low or full-screen
    composite darkening applied)
- the HUD/minimap path does not read that flag, which is how we'll
  identify the right code region: any trace where the HUD-composite
  path moves in lockstep with the darkening is reading the wrong flag
- the shared site is almost certainly a class accessor like
  `GameWorld::IsPresentationReady()` / `Scene::IsRenderableToUser()`
  or a specific handshake flag on the race-state controller that
  gates the 3D-world subsystem only
- once identified, a one-byte patch forcing it to return true unlocks
  both the main-view darkening and the mirror simultaneously while
  leaving HUD unchanged — that triple signature is the verification
  that we've patched the right thing

Worth adding to the resume point: any runtime trace must capture
mirror-pass enable/disable toggles and main-view exposure or final
composite branch around steps 6 → 7 → 8 **with HUD as a null control
channel**. If a candidate flag also changes HUD rendering, it's not
the gate — the gate leaves HUD untouched.

### Merged assessment after reviewing both hypotheses

Claude's addition contributes one important refinement that should be
treated as load-bearing:

- the world is already visible on the starting grid **before** the
  blackout closes

That observation decisively rejects any remaining "scene not ready yet"
or "late initialization" model. The blackout is not the absence of a
finished frame. It is a later **close/re-open gate** applied after the
gameplay world is already renderable.

So the merged model is now:

- **not** a fade asset problem
- **not** a late composite problem
- **not** an initialization delay
- **not** a HUD/final-present problem
- **yes** a shared 3D-world presentation gate that:
  - darkens the main view
  - suppresses the mirror completely
  - leaves HUD/minimap untouched

### Better next path from the merged model

Do not trace "fade" names anymore.

Trace the **shared gate reader** instead:

1. Find where the mirror pass is conditionally suppressed during the
   blackout window.
2. Find what main-view darkening branch toggles in the same window.
3. Intersect those two control paths.
4. Reject any candidate that also affects HUD/minimap.

The most valuable runtime markers for the next probe are therefore:
- mirror enable/disable
- main-view darken branch enable/disable
- race-state handoff around panorama -> grid
- no HUD coupling

The best fix target is no longer "a fade effect". It is the first
shared boolean/state read that explains:
- main dim
- mirror zero
- HUD untouched
- recovery of both main+mirror later

### What the next trace must answer

The next runtime probe must identify:
- the controller/page/state transition from step 4 to step 5
- the state transition that activates blackout at step 7
- the state transition that re-enables world presentation at step 8/9
- whether main view and mirror re-enable off the same gate or sibling
  gates

The next step should therefore be a **sequence tracer**, not another
fade shotgun:
- log the live handoff dispatches/callbacks/messages around this exact
  sequence
- then patch the exact live branch once identified

2. Load the carved ELF:

   ```
   /mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.et_exec.elf
   ```

   Import as x86-64, no section headers. Ghidra's autoanalysis will
   walk function bodies via the program headers.

3. Jump to the four anchor points already located this session:

   | Anchor | VA | What it is |
   |---|---|---|
   | `FreeplayGetInCar` class name string | `0x12c485d` | |
   | `FreeplayGetInCarPage` page name string | `0x12c4848` | |
   | RTTI name-getter returning class name | `0xf89b60` | |
   | Class constructor (sets vtable pointer) | `0xf89b00` | |
   | Class vtable (unresolved in static file) | `0x15dc3e0` | |

4. In Ghidra, cross-reference from the string VAs. The
   constructor at `0xf89b00` writes
   `*(this) = &vtable[0] (VA 0x15dc3f0)`. Once Ghidra applies its
   runtime relocation analysis, the vtable's function pointers
   (`vfn[0..N]`) will display actual addresses instead of the
   `0x000000070000001X` relocation placeholders we see in raw
   bytes.

5. Identify the render / draw slot. Expected names in the UI
   controller base class: `Render`, `OnRender`, `Draw`, `OnDraw`,
   `UpdateAndRender`. The dispatch almost certainly calls into
   GNM (sceGnm*) draw-state setup. Confirm by checking the render
   method calls into known graphics-driver import stubs.

6. Apply the patch:

   ```
   # Break the overlay symlink and replace with a patched copy.
   cp /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
      /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin.vanilla
   rm /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   cp /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
      /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ```

   Byte-patch the overlay eboot (not the `.vanilla` backup). First
   patch to try: replace the first byte of the identified render
   method with `0xC3` (`ret`). Remember the eboot has an OELF
   wrapper: the 288-byte header means file offset of the patch
   inside `eboot.bin` is `inner_ELF_file_offset + 288`.

7. Test:

   ```
   /mnt/data/Projects/shadPS4/scripts/stop_driveclub_live.sh
   /mnt/data/Projects/shadPS4/scripts/run_driveclub_overlay.sh
   ```

   Load Munnar at 19:30 clear, observe whether the static-car
   overlay disappears while HUD / race flow stay intact.

8. Rollback if anything goes wrong:

   ```
   rm /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ln -s /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin \
         /mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin
   ```

### What not to waste time on

These have all been tried and produced clean misses. Do not retest
without a new hypothesis.

- Host gamma / `sceVideoOutAdjustColor` hook (Phase 4).
- Forced `Finish()` at race-start window (Phase 4).
- Any texture-null torture on sampled inputs during race draws
  (Phase 7).
- Any `newui/panels/*.txt` single-panel surgery: loading screens,
  backgrounds, pause, vehicle_select_background, get_in_car_animation
  (Phase 8).
- Removing the `getincar.freeplay` Page entry from `freeplay.ctl`
  (Phase 8 — crashes).
- `SHADPS4_DC_TORTURE` env var enabled — sampled-input hypothesis
  closed.

### Exit memory

Remember the durable rule (see global memory):
`feedback_never_accept_as_is` — there is no "accept it" option. The
race-start overlay is a bug we are working to kill, not a feature
to live with. Binary-patching the eboot is the next hypothesis; if
it fails, the next one will be found from whatever that failure
teaches us.
