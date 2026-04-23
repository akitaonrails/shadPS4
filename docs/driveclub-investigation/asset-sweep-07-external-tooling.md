## External tooling / level inspection

There is no evidence yet of a turnkey "open the whole track in Blender
and watch the blackout" path.

What does exist locally:

- `DriveClubFS` can unpack `.ndx + .dat` and extract binary resources,
  XML, and textures from `.rpk`
- the local tool explicitly supports Driveclub `1.28`
- resource types include `RTUID_SCENE`, `RTUID_CAMERA`,
  `RTUID_MATERIAL`, `RTUID_SHADER`, `RTUID_LEVEL_DATA`, and
  `RTUID_GUI_ANIM`
- a local `data/nexus/viewer/config.xml` exists, which suggests
  Evolution had some kind of Nexus viewer workflow

Practical interpretation:

- yes, we can extract track packs and inspect textures / XML / binary
  resources externally
- no, there is not yet a proven generic DCC path in this investigation
  for loading the whole assembled level with live postfx behavior
- the most realistic external next step is to use `DriveClubFS` against
  one track pack and inspect the extracted `PostFXConfig_*`, camera, and
  related XML/bin resources directly
