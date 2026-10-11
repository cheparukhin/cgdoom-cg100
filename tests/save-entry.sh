#!/bin/sh
# Save-name entry on the host (make test-saveentry).
#
#   save-entry.sh -bin <cgdoom-host> [-out DIR]
#
# Drives the save menu with tests/input/save-entry.txt (raw matrix keys, as
# PL_ScanKeyboard reports them) and checks the descriptions written to the
# .DSG files (the first 24 bytes):
#   - an empty slot is pre-filled with the map name and EXE saves it
#     ("E1M1" for freedoom1, "MAP01" for freedoom2); Use and Weapon 1,
#     pressed in the same tic as that EXE, are not typed into it;
#   - typing works: letters, ALPHA digits, space, DEL ("A1 B0"); releasing
#     ALPHA over a held key types nothing;
#   - MENU (escape) cancels: no file for slot 2.
# Then loads slot 1 through the Load Game menu, checks the save is valid,
# pauses, and checks CGD_IsPaused() through the host's -gamelog.
# Then saves with DelaySaves=0 (-nodelaysaves) into a read-only and a
# writable data directory: a failed write is reported, not "game saved."
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/saveentry
WADS=$ROOT/test-wads/fd/freedoom-0.13.0

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <cgdoom-host> [-out DIR]" >&2; exit 2; }

fail() { echo "FAIL saveentry: $*"; exit 1; }

# Description of a save file: bytes up to the first NUL of the first 24
desc() { head -c 24 "$1" | tr '\000' '\n' | head -n 1; }

run_wad() {
    wad=$1; dir=$OUT/$wad
    rm -rf "$dir"; mkdir -p "$dir" || exit 2
    "$BIN" -iwad "$WADS/$wad.wad" -warp 1 1 -skill 1 -tics 400 \
        -input "$ROOT/tests/input/save-entry.txt" -data "$dir" \
        > "$dir/save.log" 2>&1 || fail "$wad: run failed (see $dir/save.log)"
}

run_wad freedoom1
run_wad freedoom2

for w in "freedoom1 E1M1" "freedoom2 MAP01"; do
    set -- $w
    dir=$OUT/$1
    s0=$(ls "$dir"/*_0.DSG 2>/dev/null); s1=$(ls "$dir"/*_1.DSG 2>/dev/null)
    [ -n "$s0" ] || fail "$1: no slot 0 save in $dir"
    [ -n "$s1" ] || fail "$1: no slot 1 save in $dir"
    ls "$dir"/*_2.DSG > /dev/null 2>&1 && fail "$1: slot 2 was saved (escape)"
    d0=$(desc "$s0"); d1=$(desc "$s1")
    [ "$d0" = "$2" ] || fail "$1: slot 0 description '$d0', expected '$2'"
    [ "$d1" = "A1 B0" ] || fail "$1: slot 1 description '$d1', expected 'A1 B0'"
done

# Load slot 1 of freedoom1 from the title screen menu: Load Game, slot 1
dir=$OUT/freedoom1
cat > "$dir/load.txt" <<'EOS'
10
1 menu
3
1 back
3
1 back
3
1 enter
3
1 back
3
1 enter
20
1 pause
10
1 pause
100000
EOS
"$BIN" -iwad "$WADS/freedoom1.wad" -tics 160 -input "$dir/load.txt" \
    -data "$dir" -gamelog > "$dir/load.log" 2>&1 ||
    fail "load run failed (see $dir/load.log)"
grep -q "GAMELOG end .*map=E1M1 state=0" "$dir/load.log" ||
    fail "the loaded game is not in E1M1 (see $dir/load.log)"

# CGD_IsPaused() along the way (GAMELOG idle=): title screen (NOLEVEL 4),
# menu over it (6), the loaded level (0), Pause (1), playing again (0)
seq=$(sed -n 's/^GAMELOG idle .* idle=\([0-9]*\)$/\1/p' "$dir/load.log" |
    tr '\n' ' ')
[ "$seq" = "4 6 0 1 0 " ] ||
    fail "CGD_IsPaused sequence '$seq', expected '4 6 0 1 0 ' (see $dir/load.log)"

# Final review: with DelaySaves=0 (-nodelaysaves) a write the platform
# refuses must not be reported as "game saved." (G_DoSaveGame checks the
# PL_FileWrite result and logs the lost file). A read-only data directory
# makes the host's writes fail; a writable one must still save at once.
nd=$OUT/nodelay
rm -rf "$nd"; mkdir -p "$nd/ro" "$nd/rw" || exit 2
chmod 555 "$nd/ro"
"$BIN" -iwad "$WADS/freedoom1.wad" -warp 1 1 -skill 1 -tics 400 -nodelaysaves \
    -input "$ROOT/tests/input/save-entry.txt" -data "$nd/ro" \
    > "$nd/ro.log" 2>&1
rc=$?
chmod 755 "$nd/ro"
[ $rc -eq 0 ] || fail "nodelaysaves read-only run failed (see $nd/ro.log)"
grep -q "G_DoSaveGame: .*_0.DSG not written" "$nd/ro.log" ||
    fail "a failed immediate save is not reported (see $nd/ro.log)"
ls "$nd/ro"/*.DSG > /dev/null 2>&1 && fail "read-only dir got a save"
"$BIN" -iwad "$WADS/freedoom1.wad" -warp 1 1 -skill 1 -tics 400 -nodelaysaves \
    -input "$ROOT/tests/input/save-entry.txt" -data "$nd/rw" \
    > "$nd/rw.log" 2>&1 || fail "nodelaysaves run failed (see $nd/rw.log)"
grep -q "not written" "$nd/rw.log" && fail "a good save reported as failed (see $nd/rw.log)"
ls "$nd/rw"/*_0.DSG > /dev/null 2>&1 || fail "no immediate save in $nd/rw"

# External review (2026-10-11): fire held while the save menu opens and
# released during the name entry must not stay pressed in the game after
# the save. The run is recorded (-record 0): the menu tics are not in the
# demo, so its last ticcmds are the ones after the menu closed, and none
# may carry BT_ATTACK (1).
st=$OUT/sticky
rm -rf "$st"; mkdir -p "$st" || exit 2
cat > "$st/input.txt" <<'EOS'
20
3 fire
1 fire menu
3 fire
1 fire back
3 fire
1 fire back
3 fire
1 fire back
3 fire
1 fire enter
3 fire
1 fire enter
3 fire
10
1 enter
100000
EOS
"$BIN" -iwad "$WADS/freedoom1.wad" -warp 1 1 -skill 1 -tics 160 -record 0 \
    -input "$st/input.txt" -data "$st" > "$st/run.log" 2>&1 ||
    fail "sticky-key run failed (see $st/run.log)"
lmp=$(ls "$st"/*D00.LMP 2>/dev/null)
[ -n "$lmp" ] || fail "sticky-key run: no demo in $st"
ls "$st"/*_0.DSG > /dev/null 2>&1 || fail "sticky-key run: no save in $st"
sticky=$(python3 -I - "$lmp" <<'EOS'
import sys
d = open(sys.argv[1], 'rb').read()[13:]
cmds = [d[i:i+4] for i in range(0, len(d) - 3, 4)]
fired = [c[3] & 1 for c in cmds]
# fire held at first, none in the last 60 ticcmds (after the menu)
print('ok' if any(fired) and len(fired) > 60 and not any(fired[-60:]) else
      'tics %d, fire in the last 60: %d' % (len(fired), sum(fired[-60:])))
EOS
)
[ "$sticky" = ok ] || fail "fire stays pressed after the save ($sticky, see $st)"

echo "PASS saveentry (pre-filled E1M1/MAP01, typed 'A1 B0', escape, load," \
    "CGD_IsPaused, DelaySaves=0 failure, keys released while typing)"
exit 0
