#!/bin/sh
# Demo record/playback round trip on the host (make test-roundtrip).
#
#   demo-roundtrip.sh -bin <cgdoom-host> [-out DIR]
#
# Records demo slot 5 of a cheat-free scripted run (E1M3, builtin:walk, the
# player dies and respawns) with -record into DIR, checks that the file got
# its 8.3 name (FRE0PD05.LMP for freedoom1, src/engine/cgdoom-names.c), plays
# it back with -playdemofile and requires the playback to render exactly the
# recorded frames. Then (external review, 2026-10-11) a recording that opens
# the Doom menu for a while: the menu pauses the level but not playback, so
# the paused tics must not be in the demo. The per-tic game states
# (-demotrace, without gametic; the paused tics collapse into one) of the
# recording and the playback must be the same. Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/roundtrip
WADS=$ROOT/test-wads/fd/freedoom-0.13.0
TICS=900

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }

mkdir -p "$OUT" || exit 2
rm -f "$OUT/FRE0PD05.LMP" "$OUT/rec.crc" "$OUT/play.crc"

"$BIN" -iwad "$WADS/freedoom1.wad" -warp 1 3 -skill 3 -tics $TICS \
    -input builtin:walk -record 5 -data "$OUT" -crc "$OUT/rec.crc" \
    > "$OUT/rec.log" 2>&1 || { echo "FAIL roundtrip: recording run failed"; exit 1; }
[ -f "$OUT/FRE0PD05.LMP" ] || {
    echo "FAIL roundtrip: no FRE0PD05.LMP in $OUT:"; ls "$OUT"; exit 1; }

"$BIN" -iwad "$WADS/freedoom1.wad" -playdemofile FRE0PD05.LMP -tics 100000 \
    -data "$OUT" -crc "$OUT/play.crc" > "$OUT/play.log" 2>&1 ||
    { echo "FAIL roundtrip: playback run failed"; exit 1; }

n=$(wc -l < "$OUT/rec.crc")
if ! head -n "$n" "$OUT/play.crc" | cmp -s - "$OUT/rec.crc"; then
    echo "FAIL roundtrip: playback differs from the recording"
    exit 1
fi

rm -f "$OUT/FRE0PD06.LMP"
cat > "$OUT/menu.txt" <<'EOS'
20
15 fwd
1 menu
40
1 menu
20 fwd
1 fwd fire
30 right
100000
EOS
"$BIN" -iwad "$WADS/freedoom1.wad" -warp 1 1 -skill 3 -tics 200 \
    -input "$OUT/menu.txt" -record 6 -data "$OUT" -demotrace "$OUT/menu-rec.trace" \
    > "$OUT/menu-rec.log" 2>&1 || { echo "FAIL roundtrip: menu recording failed"; exit 1; }
"$BIN" -iwad "$WADS/freedoom1.wad" -playdemofile FRE0PD06.LMP -tics 100000 \
    -data "$OUT" -demotrace "$OUT/menu-play.trace" > "$OUT/menu-play.log" 2>&1 ||
    { echo "FAIL roundtrip: menu playback failed"; exit 1; }
states() { grep -v '^END' "$1" | cut -d' ' -f2- | uniq; }
states "$OUT/menu-rec.trace" > "$OUT/menu-rec.states"
states "$OUT/menu-play.trace" > "$OUT/menu-play.states"
m=$(wc -l < "$OUT/menu-rec.states")
if [ "$m" -lt 100 ] || ! cmp -s "$OUT/menu-rec.states" "$OUT/menu-play.states"; then
    echo "FAIL roundtrip: a recording with the menu opened plays back differently" \
        "(diff $OUT/menu-rec.states $OUT/menu-play.states)"
    exit 1
fi
echo "PASS roundtrip ($n frames recorded and played back identically; with the" \
    "menu opened: $m game states)"
exit 0
