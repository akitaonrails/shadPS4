#!/usr/bin/env bash
set -euo pipefail

WRAPPER="/mnt/data/distrobox/gaming/bin/shadps4-driveclub-gamma-debug"

if [[ ! -x "$WRAPPER" ]]; then
  printf 'ERROR: wrapper missing: %s\n' "$WRAPPER" >&2
  exit 1
fi

exec distrobox-enter gaming -- "$WRAPPER" "$@"
