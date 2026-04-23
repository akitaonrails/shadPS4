## File / path cheatsheet

```
# Fork source (this branch)
~/Projects/shadPS4                 # akitaonrails/shadPS4, origin=fork, upstream=canonical
~/Projects/DriveClubFS             # akitaonrails/DriveClubFS, origin=fork, upstream=Nenkai

# Local build artifact
/mnt/data/Projects/shadPS4/build/shadps4

# Custom launcher (isolated, preserves working Manager setup)
/mnt/data/distrobox/gaming/bin/shadps4-driveclub-gamma-debug

# Production Manager path (unchanged)
/mnt/data/distrobox/gaming/.local/share/shadPS4QtLauncher/versions/main-2026-04-19/Shadps4-sdl.AppImage

# Driveclub install tree
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003                    # live (v1.28 now)
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003.v100-working-backup # rollback
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003-v128-test          # DriveClubFS input staging
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/Driveclub.v1.28.PATCH.REPACK.PS4-GCMR.pkg

# shadPS4 data dirs
~/.local/share/shadPS4/config.json                              # Vulkan.pipeline_cache_enabled=true
~/.local/share/shadPS4/custom_configs/CUSA00003.json            # per-game (unchanged)
~/.local/share/shadPS4/patches/Driveclub.xml                    # 60fps patch, re-enabled
~/.local/share/shadPS4/log/shad_log.txt
~/.local/share/shadPS4-gamma-dbg/                               # isolated wrapper's tree
```
