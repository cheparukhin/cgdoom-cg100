#!/bin/sh
# Quicksave and quickload (make VARIANT=hd test-quicksave; CGD_EXTRAS builds).
#
#   quicksave.sh -bin <cgdoom-host> [-out DIR]
#
# The two keymap actions are bound in a CGDOOM.CFG (TAN quicksaves, COS
# quickloads). In E1M1: the first quicksave opens the Save menu to pick a
# slot (slot 0, its pre-filled name saved with EXE); after 100 tics a second
# quicksave asks before overwriting it (EXE confirms); 100 tics later a
# quickload asks too and goes back to the second save, so the level ends
# 100 tics short of its unloaded run (GAMELOG end leveltime=). A quickload
# answered with the menu key (escape) changes nothing.
#
# Then 40 quicksave/quickload rounds over two level exits in an 1850 KiB
# zone, which the same play without saves also needs: every save has its
# 176 KiB archive buffer, and the delayed copies of earlier saves stay in
# the zone (PU_STATIC) across the loads. With the buffer in the zone, the
# zone soon had no 176 KiB left in one piece and Z_Malloc ended the game
# (it took over 1900 KiB); it is malloc'd now (g_game.c G_DoSaveGame).
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host-hd/tests/quicksave
WAD=$ROOT/test-wads/fd/freedoom-0.13.0/freedoom1.wad

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }

fail() { echo "FAIL quicksave: $*"; exit 1; }

run() {   # run <name> <answer to the quickload prompt>
    dir=$OUT/$1
    rm -rf "$dir"; mkdir -p "$dir" || exit 2
    printf 'Version=1\nKeymap.QuickSave=TAN\nKeymap.QuickLoad=COS\n' \
        > "$dir/CGDOOM.CFG"
    cat > "$dir/input.txt" <<EOS
20
1 k:TAN
3
1 enter
3
1 enter
100
1 k:TAN
3
1 enter
100
1 k:COS
3
1 $2
100
EOS
    "$BIN" -iwad "$WAD" -warp 1 1 -skill 1 -tics 300 -input "$dir/input.txt" \
        -data "$dir" -gamelog > "$dir/run.log" 2>&1 ||
        fail "$1: run failed (see $dir/run.log)"
    sed -n 's/^GAMELOG end .*leveltime=\([0-9]*\).*/\1/p' "$dir/run.log"
}

kept=$(run kept menu)
loaded=$(run loaded enter)
[ -n "$kept" ] && [ -n "$loaded" ] || fail "no GAMELOG end line (see $OUT)"
# The cancelled prompt leaves the game running (idle 0 at the end)
grep -q "^GAMELOG end .*idle=0$" "$OUT/kept/run.log" ||
    fail "the cancelled quickload left the menu open (see $OUT/kept/run.log)"
ls "$OUT/loaded"/*_0.DSG > /dev/null 2>&1 || fail "no slot 0 save"
d=$(head -c 24 "$OUT"/loaded/*_0.DSG | tr '\000' '\n' | head -n 1)
[ "$d" = "E1M1" ] || fail "slot 0 description '$d', expected 'E1M1'"
# The quickload goes back by the 100 tics between the second save and it
# (plus the few tics of the prompt)
diff=$((kept - loaded))
[ $diff -ge 100 ] && [ $diff -le 110 ] ||
    fail "leveltime $kept without the load, $loaded with it (expected ~100 less)"

dir=$OUT/cycles
rm -rf "$dir"; mkdir -p "$dir" || exit 2
printf 'Version=1\nKeymap.QuickSave=TAN\nKeymap.QuickLoad=COS\n' \
    > "$dir/CGDOOM.CFG"
{
    printf '20\n1 fwd\n5\n1 k:TAN\n3\n1 enter\n3\n1 enter\n'
    i=0
    while [ $i -lt 40 ]; do
        printf '25 fwd run fire\n5 left\n1 k:TAN\n3\n1 enter\n'
        printf '20 fwd fire\n1 k:COS\n3\n1 enter\n'
        i=$((i + 1))
    done
    printf '5000\n'
} > "$dir/input.txt"
"$BIN" -iwad "$WAD" -warp 1 1 -skill 2 -tics 2700 -input "$dir/input.txt" \
    -data "$dir" -zone 1850 -exitat 700,1500 -gamelog > "$dir/run.log" 2>&1 ||
    fail "cycles: run failed in an 1850 KiB zone (see $dir/run.log)"
# Each round opens two prompts (GAMELOG idle=2: menu active)
prompts=$(grep -c '^GAMELOG idle .*idle=2$' "$dir/run.log")
[ "$prompts" -ge 60 ] || fail "cycles: only $prompts prompts (see $dir/run.log)"

echo "PASS quicksave (slot picked, overwrite and load confirmed, escape cancels, 40 save/load rounds in a small zone)"
