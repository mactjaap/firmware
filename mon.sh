#!/usr/bin/env bash
set -eu
PORT="${PORT:-/dev/ttyUSB0}"
set +u
 . ~/esp-idf/export.sh
set -u
idf.py -p "$PORT" monitor
