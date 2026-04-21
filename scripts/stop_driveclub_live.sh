#!/usr/bin/env bash
set -euo pipefail

BIN="/mnt/data/Projects/shadPS4/build/shadps4"
EBOOT="/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin"
PIDFILE="${DRIVECLUB_PIDFILE:-/mnt/data/Projects/shadPS4/tmp/driveclub-live.pid}"

kill_pid() {
  local sig="$1"
  local pid="$2"
  distrobox-enter gaming -- kill "-$sig" "$pid" 2>/dev/null || true
}

pid=""
if [[ -f "$PIDFILE" ]]; then
  pid="$(<"$PIDFILE")"
fi

if [[ -n "${pid}" ]]; then
  kill_pid TERM "$pid"
  for _ in $(seq 1 30); do
    if ! distrobox-enter gaming -- sh -lc "kill -0 '$pid' 2>/dev/null"; then
      rm -f "$PIDFILE"
      exit 0
    fi
    sleep 0.2
  done
  kill_pid KILL "$pid"
  for _ in $(seq 1 20); do
    if ! distrobox-enter gaming -- sh -lc "kill -0 '$pid' 2>/dev/null"; then
      rm -f "$PIDFILE"
      exit 0
    fi
    sleep 0.1
  done
fi

# Fallback if the pidfile is stale or missing.
distrobox-enter gaming -- sh -lc '
  bin="$1"
  eboot="$2"
  pids="$(pgrep -f "${bin} ${eboot}" || true)"
  if [ -n "$pids" ]; then
    kill -TERM $pids 2>/dev/null || true
    sleep 1
    pids="$(pgrep -f "${bin} ${eboot}" || true)"
    if [ -n "$pids" ]; then
      kill -KILL $pids 2>/dev/null || true
    fi
  fi
' sh "$BIN" "$EBOOT"

rm -f "$PIDFILE"
