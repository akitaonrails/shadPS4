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
# Investigation-probe env vars are passed through as an explicit prefix so
# the user can toggle them per-launch without editing the script:
#   SHADPS4_DC_DRAWLOG=1 scripts/run_driveclub_overlay.sh
#   SHADPS4_DC_TORTURE=1 scripts/run_driveclub_overlay.sh
PROBE_ENV=""
for var in SHADPS4_DC_DRAWLOG SHADPS4_DC_UBOLOG SHADPS4_DC_TORTURE \
           SHADPS4_DC_LUM_CLAMP SHADPS4_DC_EXPO_RESTORE SHADPS4_DC_UBO_NUKE \
           SHADPS4_DC_TEX_DUMP \
           SHADPS4_DC_TEX_NUKE_ADDRS SHADPS4_DC_TEX_NUKE_PIPES SHADPS4_DC_TEX_NUKE_ALL \
           SHADPS4_DC_NUKE_AFTER_ARM SHADPS4_DC_NUKE_SKIP_PIPES SHADPS4_DC_NUKE_KIND \
           SHADPS4_DC_NUKE_MIN_DIM SHADPS4_DC_NUKE_MAX_DIM \
           SHADPS4_DC_NUKE_MAX_ASPECT SHADPS4_DC_NUKE_FORMAT \
           SHADPS4_DC_NUKE_INCLUDE_DATA SHADPS4_DC_NUKE_INCLUDE_GPU_MOD \
           SHADPS4_DC_UBO_SMASH SHADPS4_DC_UBO_SMASH_CB \
           SHADPS4_DC_UBO_SMASH_MIN_SIZE SHADPS4_DC_UBO_SMASH_MAX_SIZE \
           SHADPS4_DC_UBO_SMASH_ONLY_READ \
           SHADPS4_DC_PC_SMASH SHADPS4_DC_PC_RANGE SHADPS4_DC_PC_VALUE \
           SHADPS4_DC_DISPATCHLOG SHADPS4_DC_DISPATCH_SKIP \
           SHADPS4_DC_DRAW_SKIP \
           SHADPS4_DC_EXPOSURE_PIN SHADPS4_DC_EXPOSURE_PIN_VALUE \
           SHADPS4_DC_TONEMAP_SSBODUMP \
           SHADPS4_DC_FRAMEORDER \
           SHADPS4_DC_UBOSNAPSHOT \
           SHADPS4_DC_LIGHT_PIN \
           SHADPS4_DC_LIGHT_PIN_FILE \
           SHADPS4_DC_LIGHT_PIN_FILE_224 \
           SHADPS4_DC_LIGHT_PIN_WINDOW \
           SHADPS4_DC_LIGHT_PIN_BOOST; do
  if [[ -n "${!var:-}" ]]; then
    PROBE_ENV+="${var}=${!var} "
  fi
done

exec distrobox-enter gaming -- sh -lc '
  pidfile="$1"
  bin="$2"
  eboot="$3"
  probes="$4"
  shift 4
  export SDL_JOYSTICK_HIDAPI=1
  export SDL_JOYSTICK_HIDAPI_PS4=1
  export SDL_JOYSTICK_HIDAPI_PS5=1
  export SDL_JOYSTICK_HIDAPI_XBOX=1
  export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
  if [[ -n "$probes" ]]; then
    export $probes
  fi
  # SDL backend auto-picked: wayland first, x11 fallback. We tried
  # hard-forcing x11 for RenderDoc; the distrobox has no XAUTHORITY and
  # the emulator died silently. With rdocEnable off in the per-game
  # config, F12 falls back to the game-only screenshot path which works
  # under Wayland.
  echo $$ > "$pidfile"
  exec "$bin" "$eboot" "$@"
' sh "$PIDFILE" "$BIN" "$EBOOT" "$PROBE_ENV" "$@"
