#!/usr/bin/env bash
set -euo pipefail

ROOT="/mnt/data/Projects/shadPS4"
BIN="${ROOT}/build/shadps4"
OVERLAY_DIR="${ROOT}/tmp/driveclub_overlay/CUSA00003"
EBOOT="${OVERLAY_DIR}/eboot.bin"
PIDFILE="${DRIVECLUB_PIDFILE:-${ROOT}/tmp/driveclub-live.pid}"
STOP_SCRIPT="${ROOT}/scripts/stop_driveclub_live.sh"

if [[ ! -x "$BIN" ]]; then
  printf 'ERROR: binary missing: %s\n' "$BIN" >&2
  exit 1
fi
if [[ ! -e "$EBOOT" ]]; then
  printf 'ERROR: overlay eboot missing: %s\n' "$EBOOT" >&2
  printf 'The overlay dir is a symlink mirror of the installed game with\n' >&2
  printf 'patched .rpks dropped into data/leveldata/. See\n' >&2
  printf 'docs/driveclub-next-test-plan.md for the layout.\n' >&2
  exit 1
fi

mkdir -p "$(dirname "$PIDFILE")"
rm -f "$PIDFILE"

# Enforce a single live emulator session before every launch so the
# overlay run does not collide with a vanilla run, and so the previous
# overlay session's pidfile is cleaned up before we write the new one.
"$STOP_SCRIPT" >/dev/null 2>&1 || true

# Match start_driveclub_live.sh: launch inside the distrobox as a sh
# so we can capture the emulator's pid into the pidfile before exec.
# The stop script depends on that pidfile to cleanly terminate the
# session regardless of which eboot path was used.
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
