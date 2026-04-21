#!/usr/bin/env bash
set -euo pipefail

BIN="/mnt/data/Projects/shadPS4/build/shadps4"
EBOOT="/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin"
PIDFILE="${DRIVECLUB_PIDFILE:-/mnt/data/Projects/shadPS4/tmp/driveclub-live.pid}"

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
  shift 3
  export SDL_JOYSTICK_HIDAPI=1
  export SDL_JOYSTICK_HIDAPI_PS4=1
  export SDL_JOYSTICK_HIDAPI_PS5=1
  export SDL_JOYSTICK_HIDAPI_XBOX=1
  export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
  echo $$ > "$pidfile"
  exec "$bin" "$eboot" "$@"
' sh "$PIDFILE" "$BIN" "$EBOOT" "$@"
