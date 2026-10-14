#!/bin/sh
# Fast monsters after loading a save (make test-skill-load; also part of
# test-unit and test-asan).
#
#   skill-load.sh -bin <cgdoom-host> [-out DIR]
#
# G_DoLoadGame must not set gameskill to the save's skill before G_InitNew,
# which switches the fast monsters by comparing the old and the new skill:
# a Nightmare save loaded from the title of a fresh session (each launch on
# the calculator is one) would then have normal-speed monsters, and a
# normal save loaded during a Nightmare game would keep fast ones. The host's
# -gamelog reports the skill (4 = Nightmare) and the tics of a demon's run
# state (sargtics: 2 normal, 1 fast). Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/skillload
WAD=$ROOT/test-wads/fd/freedoom-0.13.0/freedoom1.wad

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT" || exit 2

fail() { echo "FAIL skill-load: $*"; exit 1; }

# In a game: Doom menu, Save Game, slot <n> (pre-filled name), save
save_script() {
    printf '20\n1 menu\n3\n1 back\n3\n1 back\n3\n1 back\n3\n1 enter\n3\n'
    i=0; while [ $i -lt "$1" ]; do printf '1 back\n3\n'; i=$((i + 1)); done
    printf '1 enter\n3\n1 enter\n100000\n'
}
# Doom menu, Load Game, slot <n> (from the title or in a game)
load_script() {
    printf '10\n1 menu\n3\n1 back\n3\n1 back\n3\n1 enter\n3\n'
    i=0; while [ $i -lt "$1" ]; do printf '1 back\n3\n'; i=$((i + 1)); done
    printf '1 enter\n100000\n'
}
# run <name> <host options...>: GAMELOG end line in $OUT/<name>.log
run() {
    name=$1; shift
    "$BIN" -iwad "$WAD" -data "$OUT" -gamelog "$@" > "$OUT/$name.log" 2>&1 ||
        fail "$name: run failed (see $OUT/$name.log)"
    grep '^GAMELOG end ' "$OUT/$name.log"
}
check() {   # check <name> <line> <skill> <sargtics>
    case "$2" in
    *" map=E1M1 state=0 "*" skill=$3 sargtics=$4 "*) ;;
    *) fail "$1: expected E1M1, skill $3, sargtics $4: $2";;
    esac
}

save_script 0 > "$OUT/save0.txt"
save_script 1 > "$OUT/save1.txt"
load_script 0 > "$OUT/load0.txt"
load_script 1 > "$OUT/load1.txt"

# Slot 0: a Nightmare save; slot 1: a Hurt Me Plenty save
check nm-save "$(run nm-save -warp 1 1 -skill 5 -tics 120 -input "$OUT/save0.txt")" 4 1
check hmp-save "$(run hmp-save -warp 1 1 -skill 3 -tics 120 -input "$OUT/save1.txt")" 2 2
ls "$OUT"/*_0.DSG > /dev/null 2>&1 && ls "$OUT"/*_1.DSG > /dev/null 2>&1 ||
    fail "the saves were not written (see $OUT/*-save.log)"

# A fresh session loads the Nightmare save from the title: fast monsters
check title-nm "$(run title-nm -tics 120 -input "$OUT/load0.txt")" 4 1
# A Nightmare game loads the normal save: back to normal speed
check nm-hmp "$(run nm-hmp -warp 1 1 -skill 5 -tics 120 -input "$OUT/load1.txt")" 2 2
# A normal game loads the Nightmare save
check hmp-nm "$(run hmp-nm -warp 1 1 -skill 3 -tics 120 -input "$OUT/load0.txt")" 4 1
echo "PASS skill-load (fast monsters follow the loaded save's skill)"
