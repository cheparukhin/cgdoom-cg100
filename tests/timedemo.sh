#!/bin/sh
# Timedemo (make test-timedemo; also part of test-unit and test-asan).
#
#   timedemo.sh -bin <cgdoom-host> [-out DIR]
#
# -timedemo plays the WAD's DEMO1..DEMO4 back to back, every tic rendered
# (CGD_TimeDemo, cgdoom.h). freedoom1.wad has the four demos: each must be
# reported with as many frames as it has tics, which -playdemo gives as the
# number of tics before its end. Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/timedemo
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

LOG=$OUT/timedemo.log
"$BIN" -iwad "$WAD" -timedemo > "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL timedemo: exit status $rc (log: $LOG)"
    exit 1
fi
for n in 1 2 3 4; do
    frames=$(sed -n "s/^timedemo DEMO$n: \([0-9]*\) frames.*/\1/p" "$LOG")
    if [ -z "$frames" ]; then
        echo "FAIL timedemo: no DEMO$n result (log: $LOG)"
        exit 1
    fi
    # Tics of the demo: the gametic at the end of a -playdemo run
    "$BIN" -iwad "$WAD" -playdemo DEMO$n -gamelog > "$OUT/demo$n.log" 2>&1
    tics=$(sed -n 's/^GAMELOG end .*gametic=\([0-9]*\).*/\1/p' "$OUT/demo$n.log")
    if [ "$frames" != "$tics" ]; then
        echo "FAIL timedemo: DEMO$n $frames frames, $tics tics (log: $LOG)"
        exit 1
    fi
done
echo "PASS timedemo (DEMO1-4, one frame per tic)"
