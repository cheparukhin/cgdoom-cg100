#!/bin/sh
# Savegame and demo robustness (make test-robust; also part of test-unit and test-asan).
#
#   save-demo-robustness.sh -bin <cgdoom-host or cgdoom-sim.elf> [-runner <simrun>] [-out DIR]
#
# Damaged, foreign or oversized savegames and damaged demos must fail cleanly: no read or
# write outside a buffer (run it with the ASan build: test-asan does), and the right outcome.
#   Loads (G_DoLoadGame), from the menu, of a valid freedoom1 E1M1 savegame changed by
#   tests/savetool.py (it knows this build's record layout from cgdoom-host -savelayout):
#   - a header that G_InitNew would change or cannot load (map 33 or 0, skill 5, episode 0,
#     player flags), or a file shorter than the header (0, 41 bytes): refused before anything
#     changes; loaded during a level, that level goes on;
#   - truncated after the header (50, 100 bytes, half, all but the last byte), an index out
#     of range (states, thing type, player, sector, flat, texture, weapon), a savegame of
#     another level layout (freedoom2 MAP01 loaded as freedoom1 E1M1): "Bad savegame",
#     exit status 2;
#   - the valid savegame: loads (from the title and during a level).
#   Saves (G_DoSaveGame) in a freedoom2 copy whose MAP02 has 3000 extra things (generated
#   here, never committed): refused with "save failed: level too large!" (logged as "not
#   written"); the slot keeps its delayed copy of an earlier MAP01 save, or its file.
#   Demos (G_DoPlayDemo, G_ReadDemoTiccmd) of a recorded valid freedoom1 demo:
#   - -playdemofile: 1 or 12 bytes, consoleplayer 4, consoleplayer not in the game, no
#     player, player 0 missing, a netgame demo on a map without player 2's start: fatal
#     "Demo is ..." (as a demo of another version), exit status 2; 0 bytes: the M_ReadFile
#     error; without its 0x80 marker: plays exactly the valid demo's frames; a ticcmd that
#     saves into slot 6 or 7: ignored (no file, no overflow);
#   - the same as DEMO1 lumps (-playdemo), and in the title loop (-demos): a damaged DEMO1
#     is skipped exactly like a DEMO1 of another version (identical frames).
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
RUNNER=
OUT=$ROOT/build-host/tests/robust
WADS=$ROOT/test-wads/fd/freedoom-0.13.0

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -runner) RUNNER=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <binary> [-runner R] [-out DIR]" >&2; exit 2; }

TOOL="python3 $ROOT/tests/savetool.py"
W1=$WADS/freedoom1.wad
W2=$WADS/freedoom2.wad
rm -rf "$OUT"; mkdir -p "$OUT" || exit 2
$RUNNER "$BIN" -savelayout > "$OUT/layout.txt" || { echo "FAIL robust: -savelayout"; exit 1; }

fails=0
passes=0
fail() { echo "FAIL robust: $*"; fails=$((fails + 1)); }
pass() { passes=$((passes + 1)); }

# A sanitizer report in a log (I_Error exits with status 2, a crash with another one)
crashed() { grep -q -E 'AddressSanitizer|runtime error|LeakSanitizer' "$1"; }

# The binary with a watchdog: a run that hangs (e.g. monsters looking for a player forever
# in a demo without one) ends with SIGALRM, exit status 142
WATCHDOG=120
[ -n "$RUNNER" ] && WATCHDOG=1800
run_bin() {
    perl -e 'alarm shift @ARGV; exec @ARGV or die "exec: $!\n"' $WATCHDOG $RUNNER "$BIN" "$@"
}

# Menu scripts (host keymap: MENU = Doom menu, D-pad down = "back", EXE = enter)
cat > "$OUT/save0.txt" <<'EOF'
20
1 menu
3
1 back
3
1 back
3
1 back
3
1 enter
3
1 enter
3
1 enter
100000
EOF
# Load slot 0 at tic 10, then play on
cat > "$OUT/load0.txt" <<'EOF'
10
1 menu
3
1 back
3
1 back
3
1 enter
3
1 enter
100000
EOF
# MAP01: save slot 0; the level is exited (-exitat), "use" through the
# intermission; MAP02: save slot 0 again (the menu remembers Save Game)
cat > "$OUT/bigflow.txt" <<'EOF'
20
1 menu
3
1 back
3
1 back
3
1 back
3
1 enter
3
1 enter
3
1 enter
80
1 use
10
1 use
10
1 use
10
1 use
10
1 use
40
1 menu
3
1 enter
3
1 enter
3
1 enter
100000
EOF

#---
# Loads
#---

base=$OUT/base
mkdir -p "$base"
run_bin -iwad "$W1" -warp 1 1 -skill 3 -tics 120 -input "$OUT/save0.txt" -data "$base" \
    > "$base/log" 2>&1 || { echo "FAIL robust: saving run failed (see $base/log)"; exit 1; }
SAVE=$(ls "$base"/*_0.DSG 2>/dev/null)
[ -n "$SAVE" ] || { echo "FAIL robust: no savegame in $base"; exit 1; }
SNAME=$(basename "$SAVE")
len=$(wc -c < "$SAVE" | tr -d ' ')
$TOOL info "$SAVE" "$W1" E1M1 "$OUT/layout.txt" > "$base/info" ||
    { echo "FAIL robust: savetool cannot parse the savegame"; exit 1; }
grep -q 'specials=[1-9]' "$base/info" ||
    { echo "FAIL robust: the savegame has no special to damage"; exit 1; }

# load NAME EXPECT [ARGS...]: loads $OUT/case.dsg (as slot 0) with the menu at tic 10.
# EXPECT: "refused" (the title or level $want_map goes on), "bad" ("Bad savegame$want_msg",
# status 2), "ok" (loaded)
load() {
    name=$1; expect=$2; shift 2
    d=$OUT/load-$name
    mkdir -p "$d"
    cp "$OUT/case.dsg" "$d/$SNAME"
    run_bin -iwad "$W1" -tics 150 -input "$OUT/load0.txt" -data "$d" -gamelog "$@" \
        > "$d/log" 2>&1
    rc=$?
    endmap=$(sed -n 's/^GAMELOG end .* map=\([^ ]*\) .*/\1/p' "$d/log")
    if crashed "$d/log"; then
        fail "load $name: sanitizer report (see $d/log)"; return
    fi
    case $expect in
    refused)
        # the level or title that was running goes on: $want_map
        if [ $rc != 0 ] || ! grep -q 'G_DoLoadGame: slot 0' "$d/log" ||
            [ "$endmap" != "$want_map" ]; then
            fail "load $name: status $rc, end map '$endmap' (expected a refusal, $want_map; see $d/log)"
            return
        fi;;
    bad)
        if [ $rc != 2 ] || ! grep -q "FATAL: Bad savegame$want_msg" "$d/log"; then
            fail "load $name: status $rc (expected 'Bad savegame$want_msg', status 2; see $d/log)"
            return
        fi;;
    ok)
        if [ $rc != 0 ] || [ "$endmap" != E1M1 ] || grep -q 'G_DoLoadGame: slot' "$d/log"; then
            fail "load $name: status $rc, end map '$endmap' (expected E1M1; see $d/log)"
            return
        fi;;
    esac
    pass
}

patch() { $TOOL patch "$SAVE" "$OUT/case.dsg" "$W1" E1M1 "$OUT/layout.txt" "$@" ||
    { echo "FAIL robust: savetool patch $*"; exit 1; }; }

cp "$SAVE" "$OUT/case.dsg"
load valid ok
load valid-inlevel ok -warp 2 2 -skill 2

# Refused before G_InitNew: from the title (E0M0) and during a level (E2M2)
want_map=E0M0
: > "$OUT/case.dsg"; load trunc0 refused
head -c 41 "$SAVE" > "$OUT/case.dsg"; load trunc41 refused
want_map=E2M2
for f in header.map=33 header.map=0 header.skill=5 header.episode=0 header.episode=5 \
         header.ingame0=0 header.ingame1=2; do
    patch "$f"
    load "$f" refused -warp 2 2 -skill 2
done
head -c 49 "$SAVE" > "$OUT/case.dsg"; load trunc49-inlevel refused -warp 2 2 -skill 2

# After G_InitNew: Bad savegame (with the check that must catch it)
want_msg=": truncated"
for n in 50 100 $((len / 2)); do
    head -c $n "$SAVE" > "$OUT/case.dsg"
    load trunc$n bad
done
want_msg=
head -c $((len - 1)) "$SAVE" > "$OUT/case.dsg"; load trunc$((len - 1)) bad
for c in "mobj0.state=-1 state -1" "mobj0.state=100000 state 100000" \
         "mobj0.type=1000 thing type" "mobj0.player=5 player 5" "mobj0.player=2 player 2" \
         "mobj0.sprite=10000 sprite 10000" "mobj0.lastlook=4 lastlook" "mobj0.movedir=9 movedir" \
         "special0.sector=-1 sector -1" "special0.sector=100000 sector 100000" \
         "player0.psprite0.state=5000 state 5000" "player0.readyweapon=9 weapon 9" \
         "player0.pendingweapon=12 weapon 12" "player0.health=-1 player health" \
         "player0.fixedcolormap=33 colormap" "sector0.floorpic=10000 flat 10000" \
         "sector3.ceilingpic=-2 flat -2" "side0.toptexture=10000 texture 10000" \
         "side5.midtexture=-1 texture -1" "header.ingame1=1"; do
    set -- $c
    f=$1; shift
    want_msg=${1:+": $*"}
    patch "$f"
    load "$f" bad
done
want_msg=

# Another level layout: a freedoom2 MAP01 savegame (same save name) as freedoom1 E1M1
d=$OUT/foreign; mkdir -p "$d"
run_bin -iwad "$W2" -wadname "$(echo "$SNAME" | sed 's/_0.DSG//')" -warp 1 -skill 3 -tics 120 \
    -input "$OUT/save0.txt" -data "$d" > "$d/log" 2>&1
if [ -f "$d/$SNAME" ]; then
    cp "$d/$SNAME" "$OUT/case.dsg"
    load foreign bad
else
    fail "no freedoom2 savegame named $SNAME in $d (see $d/log)"
fi

#---
# Saves too large for the savegame buffer
#---

big=$OUT/big.wad
$TOOL bigmap "$W2" "$big" MAP02 3000 || { echo "FAIL robust: savetool bigmap"; exit 1; }
BNAME=BIG_0.DSG

# Delayed saves: MAP01 saved (delayed copy), MAP02 refused, MAP01's copy written at exit
d=$OUT/big-delayed; mkdir -p "$d"
run_bin -iwad "$big" -warp 1 -skill 3 -tics 400 -exitat 60 -input "$OUT/bigflow.txt" \
    -data "$d" -gamelog > "$d/log" 2>&1
rc=$?
map=$(head -c 43 "$d/$BNAME" 2>/dev/null | tail -c 1 | od -An -tu1 | tr -d ' ')
if crashed "$d/log" || [ $rc != 0 ]; then
    fail "big delayed: status $rc (see $d/log)"
elif ! grep -q 'GAMELOG level .* map=E1M2' "$d/log"; then
    fail "big delayed: MAP02 never started (see $d/log)"
elif ! grep -q "G_DoSaveGame: $BNAME not written (savegame larger than" "$d/log"; then
    fail "big delayed: the MAP02 save is not refused (see $d/log)"
elif [ "$map" != 1 ]; then
    fail "big delayed: slot 0 has map '$map', expected MAP01's save (1)"
else
    pass
fi

# Immediate saves: the slot's file is left alone
d=$OUT/big-immediate; mkdir -p "$d"
echo "previous save of slot 0" > "$d/$BNAME"
run_bin -iwad "$big" -warp 2 -skill 3 -tics 120 -nodelaysaves -input "$OUT/save0.txt" \
    -data "$d" > "$d/log" 2>&1
rc=$?
if crashed "$d/log" || [ $rc != 0 ]; then
    fail "big immediate: status $rc (see $d/log)"
elif ! grep -q "G_DoSaveGame: $BNAME not written (savegame larger than" "$d/log"; then
    fail "big immediate: the save is not refused (see $d/log)"
elif [ "$(cat "$d/$BNAME")" != "previous save of slot 0" ]; then
    fail "big immediate: the slot's previous file was overwritten"
else
    pass
fi

#---
# Demos
#---

dd=$OUT/demo; mkdir -p "$dd"
run_bin -iwad "$W1" -warp 1 1 -skill 3 -tics 300 -input builtin:walk -record 5 -data "$dd" \
    > "$dd/rec.log" 2>&1 || { echo "FAIL robust: recording run failed"; exit 1; }
DEMO=$dd/valid.lmp
mv "$dd/FRE0PD05.LMP" "$DEMO" || { echo "FAIL robust: no recorded demo"; exit 1; }
[ "$(tail -c 1 "$DEMO" | od -An -tx1 | tr -d ' ')" = 80 ] ||
    { echo "FAIL robust: the recorded demo does not end with 0x80"; exit 1; }

# mkdemo NAME PYTHON-EXPR: $dd/NAME.lmp from an expression of d (the valid demo's bytes)
mkdemo() {
    python3 -c "d = open('$DEMO', 'rb').read(); open('$dd/$1.lmp', 'wb').write(bytes($2))" ||
        { echo "FAIL robust: mkdemo $1"; exit 1; }
}
mkdemo empty "b''"
mkdemo one "d[:1]"
mkdemo twelve "d[:12]"
mkdemo console4 "d[:8] + bytes([4]) + d[9:]"
mkdemo console1 "d[:8] + bytes([1]) + d[9:]"
mkdemo noplayer "d[:9] + bytes([0, 0, 0, 0]) + d[13:]"
mkdemo player1only "d[:8] + bytes([1, 0, 1, 0, 0]) + d[13:]"
mkdemo netgame "d[:10] + bytes([1]) + d[11:]"
mkdemo nomarker "d[:-1]"
mkdemo version200 "bytes([200]) + d[1:]"
# ticcmd 10's buttons: BT_SPECIAL | BTS_SAVEGAME | slot << BTS_SAVESHIFT
mkdemo slot6 "d[:56] + bytes([0x80 | 2 | 6 << 2]) + d[57:]"
mkdemo slot7 "d[:56] + bytes([0x80 | 2 | 7 << 2]) + d[57:]"

# play MODE NAME EXPECT [WAD]: MODE file (-playdemofile) or lump (-playdemo DEMO1 of a
# copy of freedoom1 with NAME as DEMO1); EXPECT: "damaged" (fatal "Demo is"), "readfile"
# (the M_ReadFile error), "valid" (the valid demo's frames), "plays" (status 0, no save)
play() {
    mode=$1; name=$2; expect=$3; wad=${4:-$W1}
    d=$OUT/play-$mode-$name
    mkdir -p "$d"
    if [ $mode = file ]; then
        cp "$dd/$name.lmp" "$d/PLAY.LMP"
        set -- -iwad "$wad" -playdemofile PLAY.LMP
    else
        $TOOL setlump "$W1" "$OUT/demo.wad" DEMO1 "$dd/$name.lmp" ||
            { echo "FAIL robust: savetool setlump"; exit 1; }
        set -- -iwad "$OUT/demo.wad" -playdemo DEMO1
    fi
    run_bin "$@" -tics 3000 -data "$d" -crc "$d/crc" > "$d/log" 2>&1
    rc=$?
    if crashed "$d/log"; then
        fail "$mode demo $name: sanitizer report (see $d/log)"; return
    fi
    case $expect in
    damaged)
        if [ $rc != 2 ] || ! grep -q 'FATAL: Demo is' "$d/log"; then
            fail "$mode demo $name: status $rc (expected a fatal 'Demo is', see $d/log)"
            return
        fi;;
    readfile)
        if [ $rc != 2 ] || ! grep -q 'FATAL: M_ReadFile: cannot open' "$d/log"; then
            fail "$mode demo $name: status $rc (expected the M_ReadFile error, see $d/log)"
            return
        fi;;
    valid)
        if [ $rc != 0 ] || ! cmp -s "$d/crc" "$OUT/play-$mode-valid/crc"; then
            fail "$mode demo $name: status $rc, frames differ from the valid demo's (see $d/log)"
            return
        fi;;
    plays)
        if [ $rc != 0 ] || [ ! -s "$d/crc" ] || ls "$d" | grep -q -i '\.dsg$'; then
            fail "$mode demo $name: status $rc or a savegame was written (see $d/log)"
            return
        fi;;
    esac
    pass
}

nostart=$OUT/nostart2.wad
$TOOL dropthings "$W1" "$nostart" E1M1 2 || { echo "FAIL robust: savetool dropthings"; exit 1; }

for mode in file lump; do
    play $mode valid plays
    play $mode nomarker valid
    for n in one twelve console4 console1 noplayer player1only; do
        play $mode $n damaged
    done
done
play file empty readfile
play lump empty damaged
play file version200 damaged
play file netgame plays
play file netgame damaged "$nostart"
play file slot6 plays
play file slot7 plays

# Title loop: a damaged DEMO1 is skipped like one of another version
title() {
    name=$1
    d=$OUT/title-$name; mkdir -p "$d"
    $TOOL setlump "$W1" "$OUT/demo.wad" DEMO1 "$dd/$name.lmp" ||
        { echo "FAIL robust: savetool setlump"; exit 1; }
    run_bin -iwad "$OUT/demo.wad" -demos -tics 900 -data "$d" -crc "$d/crc" > "$d/log" 2>&1
    rc=$?
    if crashed "$d/log" || [ $rc != 0 ]; then
        fail "title $name: status $rc (see $d/log)"; return 1
    fi
    return 0
}
if title version200; then
    pass
    for n in empty twelve console4 noplayer player1only; do
        if title $n; then
            if cmp -s "$OUT/title-$n/crc" "$OUT/title-version200/crc"; then
                pass
            else
                fail "title $n: frames differ from those with a DEMO1 of another version"
            fi
        fi
    done
fi

if [ $fails = 0 ]; then
    echo "PASS robust ($passes cases: damaged, foreign and oversized savegames; damaged demos)"
    exit 0
fi
echo "FAIL robust: $fails of $((passes + fails)) cases"
exit 1
