## Post-asset-sweep conclusions

The later Munnar / prerace experiments narrowed several branches to
clean misses.

### `FreeplayGetInCar` / `PreraceWaiting` / `PreraceCountdown` UI path

The prerace UI pages and panel files are **not** the blackout carrier.

What was tried:

- `freeplay.ctl`
  - `getincar.freeplay -> GenericTiled`
  - `getincar.freeplay -> PreraceWaiting`
- `prerace.ctl`
  - `PreraceWaiting -> GenericTiled`
  - `PreraceCountdown -> GenericTiled`
- panel file swaps / visual probes
  - `get_in_car_animation.txt` changed to a loud full-screen magenta
    fill
  - `x_preracewaiting.txt` changed to a loud full-screen magenta fill
  - `x_preracecountdown.txt` changed to a loud full-screen cyan fill

Observed behavior:

- replacing or rerouting `getincar.freeplay` blocked forward progress
  from car select
- replacing the `FreeplayGetInCar` page asset caused a hang
- the magenta `get_in_car_animation` probe only produced a thin line at
  the bottom of the screen
- the cyan `PreraceCountdown` probe briefly appeared as a thin line,
  then vanished
- the actual blackout still happened normally

Conclusion:

- these pages/controllers participate in the transition
- they are **not** the blackout image or blackout switch
- `FreeplayGetInCar` owns an important forward-handoff contract, but
  not the blackout itself

### `india_landscape_gui.rpk`

`india_landscape_gui.rpk` was the last strong local rendered-preview
candidate on the asset side. It turned into a clean miss.

Probe summary:

- local cutscene camera:
  - `EVO+LEVEL+ACTORCutsceneCamera_2386587591`
- local GUI postfx:
  - `EVO+LEVEL+ACTORPostFXConfig_exterior`
  - `EVO+LEVEL+ACTORPostFXConfig_interior`
- shared/global postfx actors embedded in the pack:
  - `EVO+LEVEL+ACTORPostFX_bloom`
  - `EVO+LEVEL+ACTORPostFX_BloomOverride`
  - `EVO+LEVEL+ACTORPostFX_TXAAOverride`

Patch set that was tested together:

- cutscene camera `Fade = 0`
- `PostFXConfig_*` neutralized
- `PostFX_bloom` neutralized
- `PostFX_BloomOverride` neutralized
- `PostFX_TXAAOverride` neutralized

Observed behavior:

- Munnar still behaved like baseline
- blackout timing and recovery were unchanged

Conclusion:

- `india_landscape_gui.rpk` is not the blackout switch
- the rendered preview that persists under blackout is not owned by this
  local GUI pack alone

### Shared `globaldata` animationlib RTT/grading-only shot

After broad and fade-only `globaldata.rpk` edits were already known to
be unstable, a narrower animationlib-only shot was attempted:

- `RTT_BLUR_ON`
- `RTT_BLUR_OFF`
- `RTT_GRADING_ON`
- `RTT_GRADING_OFF`

with:

- `TemporalFade = 0`
- `OverrideBlend = 0`

and **without** touching:

- `FadeIn`
- `FadeOut`
- `FadeIn_Fast`
- `FadeOut_Fast`
- shared actor records

Observed behavior:

- still black-booted / hung before reaching race start

Conclusion:

- even narrow `globaldata` animationlib edits are too unstable to use
  as a practical runtime patch surface
- `globaldata` remains analysis-only unless a much safer patch or
  runtime override method exists

### Shared prerace state-string rewrite

A code-side state rewrite was attempted on overlay `eboot.bin`:

- `kViewVehicle -> kStart`
- `kGetInVehicle -> kStart`

Observed behavior:

- race still loaded normally
- blackout still happened normally
- the game then hung in-race and could not be exited cleanly

Conclusion:

- this did hit a shared state path
- it did **not** hit the blackout switch
- blind rewriting of prerace state literals is not a productive next
  path

### Current load-bearing conclusions

What is now effectively exhausted:

- host-present / gamma / final output theory
- prerace UI page/panel family
- local Munnar prerace/postfx brute-force as the blackout master switch
- `india_landscape_gui.rpk`
- direct `globaldata.rpk` mutation as a safe runtime test path
- blind prerace state-string rewrites

What still seems plausible:

- a code-side handoff/state branch around the actual car select ->
  race-load -> blackout transition
- but it needs targeted tracing first, not another guessed family cut

### Best next code-side trace points

The most useful code-side sites found so far are:

- `0x2936a0`
  - shared freeplay/prerace string/state user
- `0x2c1cc0`
  - real `kGetInVehicle` user in the freeplay/prerace path
- `0x2c5200`
  - dispatcher that maps one branch directly to `kGetInVehicle`

Recommended next step:

- instrument these specific paths to log which state/page/camera branch
  is actually selected at:
  - car select confirm
  - race load handoff
  - blackout start
- then patch the chosen branch once, instead of doing another broad
  asset or controller shotgun
