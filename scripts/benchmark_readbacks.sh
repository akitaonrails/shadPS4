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
EBOOT=${EBOOT:-/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin}
CFG_DIR=${CFG_DIR:-/mnt/data/distrobox/gaming/.local/share/shadPS4/custom_configs}
BENCH_DIR=${BENCH_DIR:-${ROOT}/bench}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}

OUT_DIR="${BENCH_DIR}/${RUN_ID}"
CSV_PATH="${OUT_DIR}/${LABEL}-${MODE_NAME}.csv"
LOG_PATH="${OUT_DIR}/${LABEL}-${MODE_NAME}.log"
CFG_PATH="${CFG_DIR}/CUSA00003.json"
CFG_BACKUP="${CFG_DIR}/CUSA00003.json.bench-backup"

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

# Use exec+& so we keep control of the pid. distrobox-enter forwards
# env vars transparently.
distrobox-enter gaming -- "$BIN" "$EBOOT" > "$LOG_PATH" 2>&1 &
EMULATOR_PID=$!
echo "   pid:   ${EMULATOR_PID}"

# Sleep, then TERM + grace, then KILL. We look for the actual
# descendant shadps4 process under the distrobox-enter wrapper
# since the direct pid is the wrapper.
sleep "$SECONDS_ARG"

# pgrep for the real binary name so we catch the emulator child.
SHADPS4_PIDS=$(pgrep -f 'shadps4 .*eboot.bin' 2>/dev/null || true)
if [[ -n "$SHADPS4_PIDS" ]]; then
  echo "   stopping: $SHADPS4_PIDS"
  kill -TERM $SHADPS4_PIDS 2>/dev/null || true
fi
sleep 5
SHADPS4_PIDS=$(pgrep -f 'shadps4 .*eboot.bin' 2>/dev/null || true)
if [[ -n "$SHADPS4_PIDS" ]]; then
  echo "   kill -9: $SHADPS4_PIDS"
  kill -9 $SHADPS4_PIDS 2>/dev/null || true
fi

# Wait for the wrapper pid to wind up.
wait "$EMULATOR_PID" 2>/dev/null || true

if [[ -f "$CSV_PATH" ]]; then
  echo "   csv size: $(stat -c %s "$CSV_PATH") bytes"
else
  echo "   WARN: no CSV produced at ${CSV_PATH}"
fi
