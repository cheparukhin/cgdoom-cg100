#!/bin/sh
# Special keys on the title screen (make test-title-keys; also part of
# test-unit and test-asan).
#
#   title-keys.sh -bin <cgdoom-host> [-out DIR]
#
# External review: before the first level starts there is no player (the
# status bar's plyr is NULL), and frameskip, gamma, memory statistics,
# cheat, noclip and the profiler report wrote through it: a crash on the
# calculator from the title screen. tests/input/title-keys.txt presses each
# special key once on the title screen (the profiler reports 40 tics
# later), then the title loop goes on. Exit status 0 = PASS. The NULL write
# is caught by the ASan/UBSan run (test-asan); a plain build may not fault.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/titlekeys
WAD=$ROOT/test-wads/fd/freedoom-0.13.0/freedoom1.wad

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2

LOG=$OUT/title.log
"$BIN" -iwad "$WAD" -tics 120 -input "$ROOT/tests/input/title-keys.txt" \
    -data "$OUT" -gamelog > "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL title-keys: exit status $rc (log: $LOG)"
    exit 1
fi
if grep -q 'GAMELOG level .*map=E[1-9]M' "$LOG"; then
    echo "FAIL title-keys: a level started, the keys were not tested on the title (log: $LOG)"
    exit 1
fi
echo "PASS title-keys (special keys before the first level)"
