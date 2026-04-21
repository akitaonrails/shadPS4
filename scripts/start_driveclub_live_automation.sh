#!/usr/bin/env bash
set -euo pipefail

BIN="/mnt/data/Projects/shadPS4/build/shadps4"
EBOOT="/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin"
PIDFILE="${DRIVECLUB_PIDFILE:-/mnt/data/Projects/shadPS4/tmp/driveclub-live.pid}"

# Physical pad observed during validation:
#   VID=0x2dc8 PID=0x310b  (8BitDo Ultimate 2 Wireless Controller for PC)
# The replay pad uses a different PID in driveclub_pad_replay.py, so SDL can
# ignore the real device while still accepting the virtual one.
IGNORE_DEVICES="${SDL_GAMECONTROLLER_IGNORE_DEVICES:-0x2dc8/0x310b}"

if [[ ! -x "$BIN" ]]; then
  printf 'ERROR: binary missing: %s\n' "$BIN" >&2
  exit 1
fi
if [[ ! -f "$EBOOT" ]]; then
  printf 'ERROR: Driveclub eboot not found: %s\n' "$EBOOT" >&2
  exit 1
fi

mkdir -p "$(dirname "$PIDFILE")"
rm -f "$PIDFILE"

exec distrobox-enter gaming -- sh -lc '
  pidfile="$1"
  bin="$2"
  eboot="$3"
  ignore_devices="$4"
  shift 4
  export SDL_JOYSTICK_HIDAPI=1
  export SDL_JOYSTICK_HIDAPI_PS4=1
  export SDL_JOYSTICK_HIDAPI_PS5=1
  export SDL_JOYSTICK_HIDAPI_XBOX=1
  export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
  export SDL_GAMECONTROLLER_IGNORE_DEVICES="$ignore_devices"
  echo $$ > "$pidfile"
  exec "$bin" "$eboot" "$@"
' sh "$PIDFILE" "$BIN" "$EBOOT" "$IGNORE_DEVICES" "$@"
