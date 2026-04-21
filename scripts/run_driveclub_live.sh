#!/usr/bin/env bash
set -euo pipefail

ROOT="/mnt/data/Projects/shadPS4"
BIN="/mnt/data/Projects/shadPS4/build/shadps4"
EBOOT="/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin"
STOP_SCRIPT="${ROOT}/scripts/stop_driveclub_live.sh"

if [[ ! -x "$BIN" ]]; then
  printf 'ERROR: binary missing: %s\n' "$BIN" >&2
  exit 1
fi
if [[ ! -f "$EBOOT" ]]; then
  printf 'ERROR: Driveclub eboot not found: %s\n' "$EBOOT" >&2
  exit 1
fi

# Enforce a single live emulator session before every launch.
"$STOP_SCRIPT" >/dev/null 2>&1 || true

exec distrobox-enter gaming -- env \
  SDL_JOYSTICK_HIDAPI=1 \
  SDL_JOYSTICK_HIDAPI_PS4=1 \
  SDL_JOYSTICK_HIDAPI_PS5=1 \
  SDL_JOYSTICK_HIDAPI_XBOX=1 \
  SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 \
  "$BIN" "$EBOOT" "$@"
