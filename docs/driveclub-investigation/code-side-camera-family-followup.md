## Code-side camera-family follow-up

The next code-side branch after the renderer/image tree was the shared
camera / handoff family around:

- `0x2c5200`
- `0x2c1cc0`
- `0x2936a0`
- adjacent setup at `0x293840`

### `0x2c5200` gameplay camera dispatcher

Disassembly showed `0x2c5200` is not a page router. It is a dispatcher
for:

- `gameplay_camera_view`

Observed string arms:

- `fly`
- `simple`
- `inputorbit`
- `fixedoffset`
- `driverhead`
- `vehicle_attached`
- `vehicle_chase`
- `orbit`
- `iview_obtainer`
- `world`
- `photomode`
- `blender`
- `customisation`

#### Broad camera-mode shot

Test:

- all dispatcher arms forced to `vehicle_chase`

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- blackout is not explained by a simple wrong `gameplay_camera_view`
  choice in this dispatcher

### `0x2c5200` shared camera-state apply block

The handler does not only set `gameplay_camera_view`. It also applies a
shared camera-state family:

- `aperture_f_value`
- `exposure_compensation`
- `focal_distance_metres`
- `shutter_speed`
- `screen_filter_index`
- `screen_filter_name`

#### Narrow camera-parameter shot

Test:

- disable only the camera-parameter setter calls

Observed behavior:

- baseline behavior
- blackout unchanged

#### Broad camera-state shot

Test:

- disable the whole shared camera-state apply block, including
  `gameplay_camera_view` and the parameter setters above

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- the full `0x2c5200` shared camera-state handler is a clean miss

### `0x2c1cc0` `kGetInVehicle` helper family

`0x2c1cc0` is a real `kGetInVehicle` path function. A later internal
helper call inside it (`0x2c5710`) looked like the strongest remaining
local handoff candidate.

Test:

- disable the `0x2c1cc0 -> 0x2c5710` helper call

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- this `kGetInVehicle` helper family is also a clean miss

### `0x2936a0` broad shared transition/state function

Earlier analysis had already flagged `0x2936a0` as a strong shared
freeplay/prerace state user.

Additional disassembly showed it mostly talks to strings such as:

- `photomode`
- `PhotoModeTutorial_JPSKU`
- `PhotoModeTutorial`
- `socialhub`
- `text_description`

Test:

- function cut at entry (`ret`)

Observed behavior:

- baseline behavior
- blackout unchanged

Conclusion:

- `0x2936a0` is not the blackout switch either
- it is likely tutorial / shared UI-state logic, not the blackout gate

### `0x293840` adjacent object-setup path

Because `0x293840` seeds the shared `1b0` handoff object used by the
other functions above, it was tested as the next broad cut in this
family.

Test:

- function cut at entry (`ret`)

Observed behavior:

- boot crash / black boot

Conclusion:

- `0x293840` is too early / boot-sensitive to use as a practical
  runtime torture target
- it does not give usable blackout evidence

### Updated family conclusion

What is now effectively exhausted:

- the `0x2c5200` camera-mode dispatcher
- the `0x2c5200` shared camera-state apply family
- the `0x2c1cc0` `kGetInVehicle` helper family
- the broad `0x2936a0` shared transition/state function
- the adjacent `0x293840` object-setup path as a safe torture target

This means the whole current code-side camera/prerace caller family has
gone cold in the same way the late rendered/image carrier tree did.

What remains plausible after these misses:

- the shared callee/object side those functions talk to
- not the camera/prerace callers themselves
- not the late image carriers they eventually feed

- Static `CameraFade` / `SetCamera` string corruption is an adjacency path only: it changes race-start darkness and handoff, but still has not hit the blackout itself. Stop this line and pivot to runtime dispatch tracing instead of more string splits.
