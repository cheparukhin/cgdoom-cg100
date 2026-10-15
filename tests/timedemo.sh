#!/bin/sh
# Timedemo (make test-timedemo; also part of test-unit and test-asan).
#
#   timedemo.sh -bin <cgdoom-host> [-out DIR]
#
# -timedemo plays the WAD's DEMO1..DEMO4 back to back, every tic rendered
# (CGD_TimeDemo, cgdoom.h). freedoom1.wad has the four demos: each must be
# reported with as many frames as it has tics, which -playdemo gives as the
# number of tics before its end. Copies of the WAD with damaged demos check
# that rejected demos are dropped. Exit status 0 = PASS.

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
# Rejected demos (too short) are dropped: with DEMO2-4 rejected, DEMO1
# alone is reported (as above); with all four rejected, none is, and the
# run ends cleanly before any level was loaded
TOOL="python3 $ROOT/tests/savetool.py"
printf 'xyz' > "$OUT/short.lmp"
cp "$WAD" "$OUT/a.wad"
for n in 2 3 4; do
    $TOOL setlump "$OUT/a.wad" "$OUT/a2.wad" DEMO$n "$OUT/short.lmp" &&
        mv "$OUT/a2.wad" "$OUT/a.wad" || { echo "FAIL timedemo: setlump"; exit 1; }
done
$TOOL setlump "$OUT/a.wad" "$OUT/b.wad" DEMO1 "$OUT/short.lmp" ||
    { echo "FAIL timedemo: setlump"; exit 1; }
"$BIN" -iwad "$OUT/a.wad" -timedemo > "$OUT/a.log" 2>&1 ||
    { echo "FAIL timedemo: DEMO2-4 rejected: exit status $? (log: $OUT/a.log)"; exit 1; }
frames=$(sed -n 's/^timedemo DEMO1: \([0-9]*\) frames.*/\1/p' "$OUT/a.log")
tics=$(sed -n 's/^GAMELOG end .*gametic=\([0-9]*\).*/\1/p' "$OUT/demo1.log")
if [ "$frames" != "$tics" ] || [ "$(grep -c '^timedemo' "$OUT/a.log")" != 1 ]; then
    echo "FAIL timedemo: DEMO2-4 rejected: not DEMO1 alone (log: $OUT/a.log)"
    exit 1
fi
"$BIN" -iwad "$OUT/b.wad" -timedemo > "$OUT/b.log" 2>&1 ||
    { echo "FAIL timedemo: all rejected: exit status $? (log: $OUT/b.log)"; exit 1; }
grep -q '^timedemo: no demo in the WAD' "$OUT/b.log" ||
    { echo "FAIL timedemo: all rejected: no 'no demo' report (log: $OUT/b.log)"; exit 1; }
echo "PASS timedemo (DEMO1-4, one frame per tic; rejected demos dropped)"
