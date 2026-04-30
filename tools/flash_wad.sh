#!/usr/bin/env bash
# Flash DOOM1.WAD (or any DOOM-compatible WAD) into the 'wad' raw flash
# partition. Run this once, after the firmware is flashed.
#
# Usage:
#   ./tools/flash_wad.sh /path/to/DOOM1.WAD             # auto-detects port
#   ./tools/flash_wad.sh /path/to/DOOM1.WAD /dev/cu.X   # explicit port
#
# Requires the ESP-IDF environment to be active (idf.py / esptool.py on
# the PATH). Reads the WAD partition's offset from partitions.csv so it
# stays in sync if you ever resize the layout.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <DOOM1.WAD> [serial-port]" >&2
    exit 1
fi
WAD=$1
PORT=${2:-}

if [[ ! -f "$WAD" ]]; then
    echo "error: WAD file '$WAD' not found" >&2
    exit 1
fi

# Project root = parent of this script's dir.
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Pull the wad partition's offset from partitions.csv.
OFFSET=$(python3 - <<EOF
import sys, csv
with open("$ROOT/partitions.csv") as f:
    for row in csv.reader(f):
        cleaned = [c.strip() for c in row if c.strip() and not c.strip().startswith('#')]
        if len(cleaned) < 5:
            continue
        if cleaned[0] == "wad":
            print(cleaned[3])
            sys.exit(0)
sys.exit("no 'wad' line found in partitions.csv")
EOF
)

echo "Flashing '$WAD' to wad partition at offset $OFFSET"
PORT_ARG=""
[[ -n "$PORT" ]] && PORT_ARG="-p $PORT"
# shellcheck disable=SC2086
python -m esptool --chip esp32c6 $PORT_ARG \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
    "$OFFSET" "$WAD"
echo "Done. Reset the badge to start DOOM."
