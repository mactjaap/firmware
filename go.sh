#!/usr/bin/env bash
set -eu
PORT="${PORT:-/dev/cu.wchusbserial210}"
set +u
 . ~/esp-idf/export.sh
set -u
#idf.py fullclean
#idf.py build
#idf.py -p /dev/cu.wchusbserial210 flash
idf.py -p "$PORT" monitor
