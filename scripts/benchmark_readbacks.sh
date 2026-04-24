#!/usr/bin/env bash
# Run a single readbacks-perf benchmark cell: pick a mode, launch
# the emulator against a fixed DriveClub workload, collect the CSV
# produced by the measurement harness (Phase A).
#
# Usage:
#   scripts/benchmark_readbacks.sh <mode> <label> [seconds]
#
# where:
#   <mode>    one of: disabled, relaxed, precise, batched  (name, not int)
#   <label>   free-form string that names the cell (e.g. canada-60s, menu-30s)
#   [seconds] optional wall-clock to keep the emulator alive (default 60)
#
# Environment overrides:
#   BIN          emulator binary to run (default: build-perf/shadps4)
#   EBOOT        path to eboot.bin (default: DriveClub v1.28 install)
#   CFG_DIR      shadPS4 per-game configs (default: gaming distrobox home)
#   BENCH_DIR    output root (default: $ROOT/bench)
#   RUN_ID       subdir under BENCH_DIR (default: timestamp)
#   NO_LAUNCH    if set, just print the env that would be exported
#
# The script:
#   1. Rewrites the per-game CUSA00003.json to set readbacks_mode.
#   2. Runs the emulator with SHADPS4_READBACKS_METRICS pointing at
#      a mode-labelled CSV under BENCH_DIR/RUN_ID/.
#   3. Sends SIGTERM after <seconds>, gives 5s grace, SIGKILL if
#      still alive. The harness's signal handler dumps the CSV
#      before re-raising.
#   4. Restores the original CUSA00003.json on exit.
#
# Does NOT open a window for the user to drive manually — each cell
# is expected to be deterministic based on fixed-duration wall clock.
# (Correctness A/B at phase E uses screenshot-timestamped runs.)

set -euo pipefail

MODE_NAME=${1:?"usage: $0 <disabled|relaxed|precise|batched> <label> [seconds]"}
LABEL=${2:?"usage: $0 <mode> <label> [seconds]"}
SECONDS_ARG=${3:-60}

case "$MODE_NAME" in
  disabled) MODE_INT=0 ;;
  relaxed)  MODE_INT=1 ;;
  precise)  MODE_INT=2 ;;
  batched)  MODE_INT=3 ;;  # reserved for phase D
  *) echo "invalid mode: $MODE_NAME" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN=${BIN:-${ROOT}/build-perf/shadps4}
# SERIAL defaults to DriveClub; override to e.g. CUSA00207 for Bloodborne.
SERIAL=${SERIAL:-CUSA00003}
EBOOT=${EBOOT:-/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/${SERIAL}/eboot.bin}
CFG_DIR=${CFG_DIR:-/mnt/data/distrobox/gaming/.local/share/shadPS4/custom_configs}
BENCH_DIR=${BENCH_DIR:-${ROOT}/bench}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}

OUT_DIR="${BENCH_DIR}/${RUN_ID}"
CSV_PATH="${OUT_DIR}/${SERIAL}-${LABEL}-${MODE_NAME}.csv"
LOG_PATH="${OUT_DIR}/${SERIAL}-${LABEL}-${MODE_NAME}.log"
CFG_PATH="${CFG_DIR}/${SERIAL}.json"
CFG_BACKUP="${CFG_DIR}/${SERIAL}.json.bench-backup"

[[ -x "$BIN" ]] || { echo "binary not executable: $BIN" >&2; exit 2; }
[[ -f "$EBOOT" ]] || { echo "eboot missing: $EBOOT" >&2; exit 2; }
[[ -f "$CFG_PATH" ]] || { echo "config missing: $CFG_PATH" >&2; exit 2; }

mkdir -p "$OUT_DIR"

# Swap readbacks_mode in the per-game config and restore on exit.
cp "$CFG_PATH" "$CFG_BACKUP"
restore_cfg() { mv -f "$CFG_BACKUP" "$CFG_PATH" 2>/dev/null || true; }
trap restore_cfg EXIT

python3 - "$CFG_PATH" "$MODE_INT" <<'PY'
import json, sys
path, mode = sys.argv[1], int(sys.argv[2])
with open(path) as f:
    cfg = json.load(f)
cfg.setdefault("GPU", {})["readbacks_mode"] = mode
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY

echo "== ${LABEL} / ${MODE_NAME} (mode=${MODE_INT})"
echo "   csv:   ${CSV_PATH}"
echo "   log:   ${LOG_PATH}"
echo "   run:   ${SECONDS_ARG}s"

if [[ -n "${NO_LAUNCH:-}" ]]; then
  exit 0
fi

export SHADPS4_READBACKS_METRICS="$CSV_PATH"
export SDL_JOYSTICK_HIDAPI=1
export SDL_JOYSTICK_HIDAPI_PS4=1
export SDL_JOYSTICK_HIDAPI_PS5=1
export SDL_JOYSTICK_HIDAPI_XBOX=1
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1

# Abort if a previous emulator is still live inside the box. The
# benchmark has to own the only running instance or the CSV gets
# scribbled by an unrelated process.
if distrobox-enter gaming -- pgrep -x shadps4 >/dev/null 2>&1; then
  echo "   ERROR: shadps4 already running inside the container. Aborting." >&2
  distrobox-enter gaming -- pgrep -x shadps4 >&2
  exit 3
fi

# Launch. distrobox-enter wraps docker exec, which runs in the host
# but launches the binary inside the container's PID namespace. We
# cannot reliably signal the child binary from the host side, so
# all kill operations below go through `distrobox-enter pkill` so
# the signal is delivered inside the container namespace.
distrobox-enter gaming -- "$BIN" "$EBOOT" > "$LOG_PATH" 2>&1 &
EMULATOR_PID=$!
echo "   pid:   ${EMULATOR_PID} (wrapper)"

sleep "$SECONDS_ARG"

# Stop: SIGTERM from inside the container (triggers the harness's
# signal handler → CSV dump → re-raise default). Then poll until
# the process is actually gone before returning; this is what the
# previous host-side pgrep flow was missing.
echo "   stopping via in-container pkill -TERM shadps4"
distrobox-enter gaming -- pkill -TERM -x shadps4 2>/dev/null || true

# Wait up to GRACE seconds for the emulator to exit cleanly so the
# CSV write in the signal handler completes.
GRACE=8
for ((i=0; i<GRACE; i++)); do
  if ! distrobox-enter gaming -- pgrep -x shadps4 >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

# If still alive after grace, SIGKILL — but we will have lost the
# CSV for that cell. The grace window (8s) is generous enough that
# this should almost never hit on a healthy run; if it does, note
# it loudly.
if distrobox-enter gaming -- pgrep -x shadps4 >/dev/null 2>&1; then
  echo "   kill -9 (harness dump path did not complete in ${GRACE}s)" >&2
  distrobox-enter gaming -- pkill -KILL -x shadps4 2>/dev/null || true
fi

# Clean up the wrapper pid so the shell doesn't accumulate orphans.
wait "$EMULATOR_PID" 2>/dev/null || true

# Extra safety: if for any reason a stray shadps4 still exists,
# surface it rather than silently leaking into the next cell.
if distrobox-enter gaming -- pgrep -x shadps4 >/dev/null 2>&1; then
  echo "   ERROR: shadps4 still running after cleanup, refusing to continue" >&2
  distrobox-enter gaming -- pgrep -af shadps4 >&2
  exit 4
fi

if [[ -f "$CSV_PATH" ]]; then
  echo "   csv size: $(stat -c %s "$CSV_PATH") bytes"
else
  echo "   WARN: no CSV produced at ${CSV_PATH}"
fi
