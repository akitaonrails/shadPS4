## Dump-driven late-branch analysis

After the asset/UI/controller branches stalled, the investigation moved
to race-start image/pipeline dumping around the actual blackout window.

### First concrete late carrier

The first useful dump split showed:

- `0x500cdd0000` contains a healthy race image before blackout fully
  settles
- a later graphics pipeline writes two full-res black outputs:
  - `0x5000108000`
  - `0x5000900000`
- that pipeline hash is:
  - `0x1c2223026ec47d56`

Its observed sampled inputs included:

- `0x5016040000` full-res `R8G8B8A8Snorm`
- `0x5015a20000` `512x384`
- `0x501e910000` / later `0x501e8f0000` `32x32`

This was the first strong late-stage carrier candidate.

### Late carrier is not the blackout switch

Several direct torture shots were run against that late family.

#### Skip late black-output pipeline

Skipping `0x1c2223026ec47d56` caused:

- main scene all black
- HUD/map still intact
- mirror still black at first, then recovering normally

Conclusion:

- `0x1c2223026ec47d56` is a main-view carrier/composite pass
- it is **not** the blackout master switch

#### Tiny `32x32` side branch

The tiny compute producer:

- `0x000001007af82364`

writes:

- `0x501e8f0000` / `0x501e910000`

Observed input characteristics:

- simple gradient-like `32x32` image input
- one small state/control buffer

Results:

- skipping both compute producers feeding the late branch changed the
  scene materially
- skipping only the full-res `Snorm` compute did **not** change the
  blackout meaningfully
- skipping only the tiny compute made the scene go black with HUD
  intact, but the blackout timing still survived
- nulling the tiny compute buffer briefly exposed a full-screen grey
  overlay under HUD, then the usual blackout still happened

Conclusion:

- the tiny branch modulates a late overlay/composite input
- it still does **not** appear to be the blackout master switch

### Newer upstream full-res feedback family

The wider passive graph dump exposed a newer upstream loop around these
full-res surfaces:

- `0x50105c8000`
- `0x5009688000`
- `0x5008130000`
- compute auxiliaries:
  - `0x501459f800`
  - `0x5014d88800`
  - `0x5015571800`
  - `0x5016830000`

Key producers/readers in that family:

- `0x7e5d7e5e5abfc092` -> writes `0x50105c8000`
- `0x7271230fb8d57731` -> reads `0x50105c8000 + 0x5008130000`, writes
  `0x5009688000`
- `0x88a292ce37169b74` -> reads `0x5011b20000 + 0x5009688000`, writes
  `0x5008130000`
- `0x00000209c18a474f` -> writes:
  - `0x501459f800`
  - `0x5014d88800`
  - `0x5015571800`
- `0x00000404da9734f2` -> reads those and writes `0x5016830000`
- `0x729626e3082b941f` -> reads `0x5016830000`, writes `0x5008130000`

Torture result:

- skipping this family changed the scene strongly
- user impression was "lots of motion blur or similar"
- blackout still remained

Conclusion:

- this is another feeder/carrier family
- still not the blackout switch

### Parallel `500fdd / 5015ae / 501e8f` branch

Another branch feeding the same late carrier was identified:

- `0x500fdd0000`
- `0x5015ae0000`
- `0x501e8f0000`

Key producers:

- `0x8d3903b3d86ec69e`
- `0xd95182db58ebc733`
- `0x6df834395a1984ff`
- `0x2a5e157a75d687d2`
- `0x000001b96128ff3e`
- `0x4a8b302d5b6ce10b`
- `0x000001007af82364`

Torture result:

- scene all black again
- HUD/map intact
- mirror still black first, then recovering

Conclusion:

- this parallel branch is also only a visible-scene feeder/carrier
- blackout timing survives above it

### Broad combined late-feeder shot

A broader "all known late feeders into the black carrier" shot was
attempted by combining:

- the `500813 / 500968 / 50105c` side
- the `500fdd / 5015ae / 501e8f` side
- tiny `32x32` sidecars

Observed behavior:

- unstable / crash on race load

Binary split result:

- Half A (`500813 / 500968 / 50105c` side) remained crash-prone
- Half B (`500fdd / 5015ae / 501e8f` side) produced all-black scene
  with HUD intact, but blackout still survived

Conclusion:

- the whole mapped late rendered/image tree is now best classified as:
  - visible-scene carrier/composite machinery
  - **not** the blackout master switch

This is a load-bearing conclusion. It explains why so many earlier
renderer-side shots changed tint, motion-blur feel, or the frozen image
under blackout without actually disabling blackout timing.

### Shared-state shot on the late carrier family

To verify that the switch was not hiding in local state on the same
pipelines, two state-only shots were run on the known carrier family and
its immediate feeders:

1. zero small read-only buffers / `Flatbuf`
2. zero only `push_data`

Observed behavior:

- buffer/state zeroing darkened the scene under blackout
- zeroing only `push_data` returned to baseline-like behavior
- blackout timing still survived in both cases

Conclusion:

- even the local state on the late carrier family is not the blackout
  master switch
- the real gate is still above or outside this rendered/image branch

### New code-side lead: gameplay camera dispatcher

Disassembly of the best remaining code-side handoff sites produced the
first concrete non-renderer branch with semantic meaning.

At `0x2c5200`, the code is a dispatcher that selects values for:

- `gameplay_camera_view`

Observed candidate values in the string table:

- `fly`
- `simple`
- `inputorbit`
- `fixedoffset`
- `driverhead`
- `vehicle_attached`
- `vehicle_chase`
- `orbit`
- `iview_obtainer`

This is much more specific than the earlier page/controller guesses.
It directly matches the repeated symptom that the blackout often carries
a prerace/static camera-like image underneath it.

Additional nearby code-side observations:

- `0x2936a0` is still a shared freeplay/prerace state user
- `0x2c1cc0` is still a real `kGetInVehicle`-path function
- but `0x2c5200` now looks like the strongest concrete branch because it
  selects actual gameplay camera view modes rather than only page/state
  names

### Updated load-bearing conclusion

What is now effectively ruled out:

- the late black-output pipeline as the blackout switch
- the late `32x32` side branch as the blackout switch
- the newer `500813 / 500968 / 50105c` feedback family as the blackout
  switch
- the parallel `500fdd / 5015ae / 501e8f` family as the blackout switch
- local buffer / `Flatbuf` / `push_data` state on that late carrier
  family

What still looks promising:

- a code-side race-handoff branch that selects or latches the wrong
  gameplay camera / view mode
- a shared state branch above the rendered late carrier tree

Best next step after this doc update:

- reset renderer-side torture to baseline
- test a camera-mode branch on the `gameplay_camera_view` dispatcher
  instead of another renderer-side family
