#!/usr/bin/env bash
# Full clean build, optional flash erase (ERASE=1), flash and monitor.
set -euo pipefail
cd "$(dirname "$0")"

PORT="${PORT:-/dev/cu.wchusbserial210}"

set +u
. ~/esp-idf/export.sh
set -u

idf.py fullclean build

if [ "${ERASE:-0}" = "1" ]; then
    idf.py -p "$PORT" erase_flash
fi

idf.py -p "$PORT" flash monitor
