#!/bin/sh
# Maps past MAP32 (make test-map-limit; also part of test-unit and test-asan).
#
#   map-limit.sh -bin <cgdoom-host> [-out DIR]
#
# The engine is linuxdoom-1.10's and has per-map data for the 32 Doom II maps only (HU titles,
# par times, intermission level names). A Doom II WAD with a MAP33 (the BFG/Unity doom2.wad) read
# past mapnames2[] in HU_Start when the launcher started it there (it offered maps up to 99), and
# leaving the level would have read past cpars[] and lnames[]. Every case below must exit
# normally; under ASan (make test-asan) that also means no report. The WAD is a copy of
# freedoom2.wad with MAP01 added again as MAP33 and MAP34 (tests/extra-maps-wad.py; made here,
# never stored). tests/input/map-limit.txt exits the level at tic 40 and presses USE twice in the
# intermission.
#   1. -warp 33: G_InitNew limits a Doom II map to MAP32 (as it limits a Doom map to 9): the game
#      starts MAP32 and leaves it for MAP16, never MAP33.
#   2. -forcemap 33 (host test option: gamemap is set after the level started, past G_InitNew):
#      HU_Start, the par time of G_DoCompleted and the intermission see MAP33 (no table entry),
#      then the next level MAP34.
#   3. The same at gamemap -3 (below the tables).
#   4. The same in Doom (freedoom1.wad) at map 50, past mapnames[], pars[] and lnames[].
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/maplimit
WADS=$ROOT/test-wads/fd/freedoom-0.13.0
INPUT=$ROOT/tests/input/map-limit.txt

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
command -v python3 > /dev/null || { echo "map-limit: needs python3" >&2; exit 2; }

WAD2=$OUT/freedoom2-maps.wad
python3 -I "$ROOT/tests/extra-maps-wad.py" "$WADS/freedoom2.wad" "$WAD2" MAP33 MAP34 || exit 2

fail=0
# run NAME ARGS...: runs cgdoom-host, log in $OUT/NAME.log; sets rc
run() {
    name=$1; shift
    "$BIN" "$@" -data "$OUT" -gamelog > "$OUT/$name.log" 2>&1
    rc=$?
}
# expect NAME REGEX...: every regex must match a line of the log
expect() {
    name=$1; shift
    for re in "$@"; do
        if ! grep -q "$re" "$OUT/$name.log"; then
            echo "FAIL map-limit $name: no log line matching '$re' (log: $OUT/$name.log)"
            fail=$((fail + 1)); return 1
        fi
    done
    return 0
}
# check NAME DESCRIPTION REGEX...: exit status 0 and the expected log lines
check() {
    name=$1; text=$2; shift 2
    if [ "$rc" != 0 ]; then
        echo "FAIL map-limit $name: exit status $rc (log: $OUT/$name.log)"
        grep -m 2 -E 'ERROR|runtime error|[Ee]rror' "$OUT/$name.log"
        fail=$((fail + 1))
    elif expect "$name" "$@"; then
        echo "PASS map-limit $name ($text)"
    fi
}

# 1. -warp 33 starts MAP32 (episode 1 for Doom II) and goes on to MAP16
run warp33 -iwad "$WAD2" -warp 33 -exitat 40 -input "$INPUT" -tics 150
check warp33 "MAP33 requested: MAP32, then MAP16" \
    'GAMELOG level .*map=E1M32 ' 'GAMELOG completed' 'GAMELOG level .*map=E1M16 '
grep -q 'map=E1M33 ' "$OUT/warp33.log" && {
    echo "FAIL map-limit warp33: the game ran MAP33"; fail=$((fail + 1)); }

# 2. gamemap 33 past the tables, then MAP34
run force33 -iwad "$WAD2" -warp 1 -forcemap 33 -exitat 40 -input "$INPUT" -tics 150
check force33 "HU title, par and level names at MAP33, then MAP34" \
    'GAMELOG level .*map=E1M33 ' 'GAMELOG completed' 'GAMELOG level .*map=E1M34 '

# 3. gamemap below the tables (no USE: the next level would not exist)
run forceneg -iwad "$WAD2" -warp 1 -forcemap -3 -exitat 40 -tics 100
check forceneg "gamemap -3: title, par and level names" \
    'GAMELOG level .*map=E1M-3 ' 'GAMELOG completed'

# 4. Doom: past mapnames[45], pars[] and the nine level names and map nodes
run doom50 -iwad "$WADS/freedoom1.wad" -warp 1 1 -forcemap 50 -exitat 40 \
    -input "$INPUT" -tics 100
check doom50 "Doom map 50: title, par, level names, map nodes" \
    'GAMELOG level .*map=E1M50 ' 'GAMELOG completed'

[ "$fail" = 0 ] && exit 0
exit 1
