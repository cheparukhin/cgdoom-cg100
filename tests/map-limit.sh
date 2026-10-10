#!/bin/sh
# Maps past MAP32 (make test-map-limit; also part of test-unit and test-asan).
#
#   map-limit.sh -bin <cgdoom-host> [-out DIR]
#
# The engine is linuxdoom-1.10's and has HUD titles, par times and intermission
# level names for the 32 Doom II maps only. The launcher used to offer start
# maps up to 99, and a WAD with a MAP33 (the BFG/Unity doom2.wad) then read past
# mapnames2[] in HU_Start. The launcher now stops at 32, and G_InitNew limits a
# Doom II map to 32 (as it limits a Doom map to 9) for any start: launcher,
# -warp, save or demo. Here -warp 33 must start MAP32. Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/maplimit
WAD=$ROOT/test-wads/fd/freedoom-0.13.0/freedoom2.wad

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2

LOG=$OUT/warp33.log
"$BIN" -iwad "$WAD" -warp 33 -tics 70 -data "$OUT" -gamelog > "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL map-limit: exit status $rc (log: $LOG)"
    exit 1
fi
if ! grep -q 'GAMELOG level .*map=E1M32 ' "$LOG" || grep -q 'map=E1M33 ' "$LOG"; then
    echo "FAIL map-limit: -warp 33 did not start MAP32 (log: $LOG)"
    exit 1
fi
echo "PASS map-limit (-warp 33 starts MAP32)"
