## Phase 3 — v1.28 content access

### Myth busted

The distrobox docs (`docs/driveclub-shadps4.md`) said
`DriveClubFS 1.1.0 crashes at file 12/8018 with EndOfStreamException` on a
v1.28-merged tree. That claim does **not** reproduce against current data.

What I actually found:

1. **Upstream DriveClubFS has no 1.28 fix and none in flight.**
   Master is 4 commits ahead of tag `1.1.0`, all README-only. Zero PRs ever
   opened. One closed issue (`#1` "DriveClub VR", Nov 2025) describes the
   same `EndOfStreamException` shape but was closed without any engagement.
2. **Only other fork** (`illusionyy/DriveClubFS`, 2023) is 0 ahead / 7 behind
   upstream — a stale snapshot, no divergent code.
3. **Running DriveClubFS unpack-all on a properly-merged v1.28 tree succeeded
   cleanly** — 8018/8018 files extracted, 47 GB, exit 0, zero errors or
   skips. Either the earlier attempt was run against a badly-staged tree, or
   the crash was transient and has since been resolved silently.

No fork patch needed for 1.28. The fork at `~/Projects/DriveClubFS` still
exists for future stewardship but there's nothing to apply today.

### Working v1.28 install recipe

Paths below are on the gaming distrobox host mount. Commands prefixed
`distrobox enter gaming -- ...` where the NAS mount differs from the sandbox.

```sh
# Starting point: CUSA00003/ is a working v1.00 install (DriveClubFS-unpacked)
# Goal: swap in v1.28 content, preserve v1.00 as backup.

cd /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4

# 1. Extract v1.28 PKG to a staging dir. ~19 GB PKG, ~20 GB output.
#    Contains eboot.bin (v1.28), game.ndx (v1.28, 419K), ~41 high-index .dat
#    files, sce_module (libc.prx + libSceFios2.prx), sce_companion_httpd,
#    .prx/.plt resources, and a partial sce_sys/about/.
/mnt/data/distrobox/gaming/tools/ShadPKG/build-cli/shadpkg extract \
    -i /mnt/terachad/.../Driveclub.v1.28.PATCH.REPACK.PS4-GCMR.pkg \
    -o /mnt/terachad/.../CUSA00003-v128-test

# 2. Symlink the v1.00 .dat files into the v1.28 tree so DriveClubFS sees
#    the full set (the v1.28 index references both low- and high-numbered
#    .dat files).
cd CUSA00003-v128-test
for f in ../CUSA00003/game*.dat; do ln -s "$f" "$(basename "$f")"; done

# 3. Run DriveClubFS. v1.28 index reports 8018 entries; output ~47 GB loose.
dotnet ~/Projects/DriveClubFS/DriveClubFS/bin/Release/net10.0/DriveClubFS.dll \
    unpack-all \
    -i /mnt/terachad/.../CUSA00003-v128-test \
    -o /mnt/terachad/.../CUSA00003-v128-test-out \
    --skip-verifying-checksum

# 4. Swap in place, backup preserved.
cd /mnt/terachad/Emulators/EmuDeck/roms_rare/ps4
mv CUSA00003 CUSA00003.v100-working-backup   # keep as rollback
mkdir CUSA00003
mv CUSA00003-v128-test-out/* CUSA00003/       # 8018 loose files (47 GB)
rmdir CUSA00003-v128-test-out

# 5. Copy v1.28 metadata on top (not the .dat files — loose files win).
cd CUSA00003-v128-test
for f in eboot.bin game.ndx *.prx *.plt; do
  [ -e "$f" ] && cp -a "$f" ../CUSA00003/
done
for d in sce_sys sce_module sce_companion_httpd; do
  [ -d "$d" ] && cp -a "$d" ../CUSA00003/
done

# 6. CRITICAL: restore base sce_sys files from v1.00 backup (see below).
cd ../CUSA00003/sce_sys
for f in param.sfo disc_info.dat keystone; do
  [ ! -f "$f" ] && cp -a "../../CUSA00003.v100-working-backup/sce_sys/$f" .
done

# 7. Re-enable the 60 fps patch (its offsets target v1.28 eboot).
mv ~/.local/share/shadPS4/patches/Driveclub.xml.disabled-for-v1.0 \
   ~/.local/share/shadPS4/patches/Driveclub.xml
```

Keep `CUSA00003-v128-test/` around — it's the re-runnable DriveClubFS
input if the loose-file tree ever needs regeneration. Leave
`CUSA00003.v100-working-backup/` in place for one-command rollback.

### The `sce_sys/param.sfo` gotcha

v1.28 is a **CUMULATIVE_PATCH** PKG. ShadPKG's extraction of that PKG gives
you `sce_sys/about/right.sprx` and not much else — base metadata (param.sfo,
disc_info.dat, keystone) is NOT re-shipped by cumulative patches because a
real PS4 already has them from the base install.

On our side the only copy of those files lives in the v1.00 backup tree. If
you naively `cp -a` the v1.28 `sce_sys/` over the new install, you end up
with a folder that has `about/right.sprx` but no `param.sfo` at all. shadPS4
loads the eboot, applies the Driveclub.xml patch, boots the game, but the
game's internal "am I running v1.28? what content is unlocked?" checks silently
fall back to "not yet released → download required" because there's no APP_VER
to compare against. Hence "all content locked, asking to download", even
though the v1.28 eboot is the one actually running.

The v1.00 backup's `param.sfo` is fine to restore as-is: it already has both
`APP_VER = 01.28` and `VERSION = 01.00` (the base VERSION stays at 01.00
forever, APP_VER reflects whatever patch is installed on top).

`npbind.dat` is absent in both our v1.00 backup and the v1.28 patch output.
That's only needed for premium DLC entitlement verification; base game and
free-update content don't require it.

### After the fix

With the restored `param.sfo` the user confirmed in-game that:
- Previously greyed-out / "download required" content is now accessible.
- New v1.28 content is visible in menus.

Remaining concerns carried into next phase:

- **Slowness during gameplay** — see Phase 4.
- **Dim image** — unchanged by the v1.28 swap, still owned by Phase 2.
