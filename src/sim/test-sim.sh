#!/bin/sh
# Short bit-exactness test of cgdoom-sim.elf against the host goldens.
#
#   test-sim.sh -bin build-sim/cgdoom-sim.elf [-tics N] [-only NAME]... [-golden DIR] [-out DIR]
#
# Runs the named scenarios of tests/scenarios.txt (default: e1m1 map01 demo1) in the simulator
# with "-tics N" appended (default 60; the last -tics wins), and checks that the per-frame CRC
# lines are exactly the first lines of DIR/<name>.crc (default tests/golden). Runs are
# deterministic, so a shortened run renders the same frames as the start of the full one.
# Exit status 0 if all passed.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=
TICS=60
ONLY=
OUT=$ROOT/build-sim/tests-short
SCEN=$ROOT/tests/scenarios.txt
GOLDEN=$ROOT/tests/golden
WADS=$ROOT/test-wads/fd/freedoom-0.13.0
RUNNER=$ROOT/src/sim/simrun

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -tics) TICS=$2; shift 2;;
    -only) ONLY="$ONLY $2"; shift 2;;
    -out) OUT=$2; shift 2;;
    -golden) GOLDEN=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <elf> [-tics N] [-only NAME]..." >&2; exit 2; }
[ -n "$ONLY" ] || ONLY="e1m1 map01 demo1"
mkdir -p "$OUT" || exit 2

pass=0
fail=0
t0=$(date +%s)
for name in $ONLY; do
    line=$(grep -v '^[[:space:]]*#' "$SCEN" | awk -v n="$name" '$1 == n')
    if [ -z "$line" ]; then
        echo "FAIL $name: not in $SCEN"; fail=$((fail + 1)); continue
    fi
    rest=${line#"$name"}
    ref=$name
    case "$rest" in
    *golden=*) ref=$(echo "$rest" | sed 's/.*golden=\([^ ]*\).*/\1/')
               rest=$(echo "$rest" | sed 's/golden=[^ ]*//');;
    esac
    args=$(echo "$rest" | sed "s|\\\$WADS|$WADS|g")
    crc=$OUT/$name.crc
    log=$OUT/$name.log
    rm -f "$crc"
    start=$(date +%s)
    # shellcheck disable=SC2086
    "$RUNNER" --stats "$BIN" $args -tics "$TICS" -crc "$crc" -stats -simstats > "$log" 2>&1 < /dev/null
    rc=$?
    secs=$(( $(date +%s) - start ))
    frames=0
    [ -f "$crc" ] && frames=$(wc -l < "$crc" | tr -d ' ')
    insts=$(sed -n 's/^SIMSTATS insts=\([0-9]*\).*/\1/p' "$log")
    info="$frames frames, ${secs}s, ${insts:-?} insts"
    if [ $rc -ne 0 ]; then
        echo "FAIL $name: exit status $rc (see $log)"
        tail -5 "$log" | sed 's/^/     | /'
        fail=$((fail + 1))
    elif [ ! -f "$GOLDEN/$ref.crc" ]; then
        echo "FAIL $name: no golden $GOLDEN/$ref.crc"; fail=$((fail + 1))
    elif [ "$frames" -lt "$TICS" ]; then
        echo "FAIL $name: only $frames frames ($info)"; fail=$((fail + 1))
    elif head -n "$frames" "$GOLDEN/$ref.crc" | cmp -s - "$crc"; then
        echo "PASS $name ($info, first $frames frames of $ref.crc)"
        pass=$((pass + 1))
    else
        first=$(head -n "$frames" "$GOLDEN/$ref.crc" | diff "$crc" - | sed -n '2p')
        echo "FAIL $name: differs from $ref.crc; first diff: $first"
        fail=$((fail + 1))
    fi
done
echo "---- test-sim: $pass passed, $fail failed ($(( $(date +%s) - t0 ))s, -tics $TICS)"
[ $fail -eq 0 ]
