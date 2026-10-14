#!/bin/sh
# Demo recording that never began (make test-demo-unbegun; also part of test-unit and test-asan).
#
#   demo-unbegun.sh -bin <cgdoom-host> [-out DIR]
#
# G_RecordDemo arms a recording (demorecording = true); it only begins in G_BeginRecording, after
# the first level load. Without the "recording has begun" flag (g_game.c) the engine would write
# through the NULL demo_p when a game ends or fails in that window: G_Ticker on the title screen, the
# final G_CheckDemoStatus of D_DoomLoop, and I_Error during the first level load. Every case below
# must exit normally (no crash), save no demo and leave the slot's previous file untouched:
#   1. -record without a start map: the title screen, then -tics ends the game;
#   2. the same with the WAD's title demos playing (G_DoPlayDemo replaces demobuffer/demo_p);
#      the frames must also equal those of the same run without -record;
#   3. -warp with a zone too small for the level: I_Error in P_SetupLevel with the recording armed
#      (a clean fatal exit, status 2).
# And one recording that HAS begun: 4. End Game from a recorded game with title
#   demos on (tests/input/demo-endgame.txt). The first played demo ends the recording (G_DoPlayDemo):
#   the frames must equal those of the same run without -record (a recording that went on writing
#   ticcmds into the played demo lump would desync it), and the demo is saved, short
#   (game + title tics, not the whole run) and ends with the 0x80 marker.
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/unbegun
WADS=$ROOT/test-wads/fd/freedoom-0.13.0
WAD=$WADS/freedoom1.wad
LMP=FRE0PD05.LMP
OLD="previous demo of slot 5"

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }
mkdir -p "$OUT" || exit 2

fail=0
# run NAME EXPECTED_STATUS ARGS...: runs with -record 5 -data $OUT/NAME, a previous slot file in place
run() {
    name=$1; want=$2; shift 2
    dir=$OUT/$name
    mkdir -p "$dir" || exit 2
    printf '%s\n' "$OLD" > "$dir/$LMP"
    "$BIN" -iwad "$WAD" "$@" -record 5 -data "$dir" -crc "$dir/rec.crc" > "$dir/log" 2>&1
    rc=$?
    if [ "$rc" != "$want" ]; then
        echo "FAIL unbegun $name: exit status $rc, expected $want (log: $dir/log)"
        fail=$((fail + 1)); return 1
    fi
    if [ "$(cat "$dir/$LMP" 2>/dev/null)" != "$OLD" ]; then
        echo "FAIL unbegun $name: $LMP was overwritten or removed"
        fail=$((fail + 1)); return 1
    fi
    if [ "$(ls "$dir" | grep -c -i '\.lmp$')" != 1 ]; then
        echo "FAIL unbegun $name: unexpected demo files: $(ls "$dir")"
        fail=$((fail + 1)); return 1
    fi
    return 0
}

run title 0 -tics 60 && echo "PASS unbegun title (no demo saved, slot file kept)"

if run titledemo 0 -demos -tics 400; then
    "$BIN" -iwad "$WAD" -demos -tics 400 -crc "$OUT/titledemo/ref.crc" > "$OUT/titledemo/ref.log" 2>&1
    if cmp -s "$OUT/titledemo/ref.crc" "$OUT/titledemo/rec.crc"; then
        echo "PASS unbegun titledemo ($(wc -l < "$OUT/titledemo/rec.crc" | tr -d ' ') frames as without -record)"
    else
        echo "FAIL unbegun titledemo: frames differ from the run without -record"
        fail=$((fail + 1))
    fi
fi

if run ierror 2 -warp 1 1 -tics 60 -zone 1000; then
    if grep -q 'Z_Malloc' "$OUT/ierror/log"; then
        echo "PASS unbegun ierror (I_Error during the first level load, clean fatal exit)"
    else
        echo "FAIL unbegun ierror: no Z_Malloc failure in the log (zone size no longer fails the level load?)"
        fail=$((fail + 1))
    fi
fi

# 4. A begun recording ended by a title demo
dir=$OUT/endgame
rm -rf "$dir"; mkdir -p "$dir" || exit 2
EARGS="-iwad $WAD -warp 1 1 -skill 3 -demos -tics 1500 -input $ROOT/tests/input/demo-endgame.txt"
"$BIN" $EARGS -record 5 -data "$dir" -crc "$dir/rec.crc" > "$dir/log" 2>&1
rc=$?
"$BIN" $EARGS -crc "$dir/ref.crc" > "$dir/ref.log" 2>&1
rc2=$?
size=$(wc -c 2>/dev/null < "$dir/$LMP" | tr -d ' ')
last=$(tail -c 1 "$dir/$LMP" 2>/dev/null | od -An -tx1 | tr -d ' ')
if [ "$rc" != 0 ] || [ "$rc2" != 0 ]; then
    echo "FAIL unbegun endgame: exit status $rc / $rc2 (log: $dir/log)"
    fail=$((fail + 1))
elif ! cmp -s "$dir/ref.crc" "$dir/rec.crc"; then
    echo "FAIL unbegun endgame: frames differ from the run without -record (title demo written into)"
    fail=$((fail + 1))
elif [ -z "$size" ] || [ "$size" -lt 300 ] || [ "$size" -gt 2000 ] || [ "$last" != 80 ]; then
    echo "FAIL unbegun endgame: $LMP size '$size' (expected 300..2000), last byte '$last' (expected 80)"
    fail=$((fail + 1))
else
    echo "PASS unbegun endgame (recording ended by the title demo: $size bytes, frames as without -record)"
fi

[ "$fail" = 0 ] && exit 0
exit 1
