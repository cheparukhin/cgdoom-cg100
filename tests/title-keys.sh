#!/bin/sh
# Special keys on the title screen (make test-title-keys; also part of
# test-unit and test-asan).
#
#   title-keys.sh -bin <cgdoom-host> [-out DIR]
#
# Before the first level starts there is no player (the status bar's plyr
# is NULL), and a special key that shows a message (gamma, cheat, noclip)
# must not write through it: that would crash the calculator from the
# title screen. tests/input/title-keys.txt presses each special key
# once on the title screen, then the title loop goes on.
# Second run: with title demos on (-demos, as on the calculator), a key
# pressed during a demo opens the Doom menu, as in vanilla
# (tests/input/title-demo-key.txt). Third run: a WAD without demo lumps
# (tools/wadtool.py pack --drop-demos) cycles its title pages instead of
# stopping with "W_GetNumForName: demo1 not found!". Exit status 0 = PASS. The NULL write
# is caught by the ASan/UBSan run (test-asan); a plain build may not fault.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
OUT=$ROOT/build-host/tests/titlekeys
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

LOG=$OUT/title.log
"$BIN" -iwad "$WAD" -tics 120 -input "$ROOT/tests/input/title-keys.txt" \
    -data "$OUT" -gamelog > "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL title-keys: exit status $rc (log: $LOG)"
    exit 1
fi
if grep -q 'GAMELOG level .*map=E[1-9]M' "$LOG"; then
    echo "FAIL title-keys: a level started, the keys were not tested on the title (log: $LOG)"
    exit 1
fi

LOG=$OUT/titledemo.log
"$BIN" -iwad "$WAD" -demos -tics 400 \
    -input "$ROOT/tests/input/title-demo-key.txt" -data "$OUT" -gamelog \
    > "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL title-keys: title demo run, exit status $rc (log: $LOG)"
    exit 1
fi
# CGD_IsPaused() = CGD_IDLE_DEMO | CGD_IDLE_MENU while the demo plays
if ! grep -q 'GAMELOG idle .*demoplayback=1 .*idle=10$' "$LOG"; then
    echo "FAIL title-keys: a key during a title demo did not open the menu (log: $LOG)"
    exit 1
fi

NODEMOS=$OUT/nodemos.wad
LOG=$OUT/nodemos.log
if ! python3 -I "$ROOT/tools/wadtool.py" pack "$WAD" "$NODEMOS" --drop-demos \
    > "$LOG" 2>&1; then
    echo "FAIL title-keys: wadtool.py pack --drop-demos failed (log: $LOG)"
    exit 1
fi
"$BIN" -iwad "$NODEMOS" -demos -tics 1500 -data "$OUT" -gamelog >> "$LOG" 2>&1
rc=$?
if [ "$rc" != 0 ]; then
    echo "FAIL title-keys: title loop of a WAD without demos, exit status $rc (log: $LOG)"
    exit 1
fi
if grep -q 'GAMELOG level .*demoplayback=1' "$LOG"; then
    echo "FAIL title-keys: a demo played from a WAD without demos (log: $LOG)"
    exit 1
fi
rm -f "$NODEMOS"
echo "PASS title-keys (special keys before the first level, menu key during a title demo, title loop without demos)"
