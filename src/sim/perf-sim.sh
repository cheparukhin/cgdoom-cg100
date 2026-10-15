#!/bin/sh
# Per-frame cost baseline of cgdoom-sim.elf in the SH simulator (needs sh-elf-run64: make sim-tool).
#
#   perf-sim.sh -bin build-sim/cgdoom-sim.elf [-from F] [-only NAME]... [-prof NAME]... [-golden DIR]
#               [-out DIR]
#
# For each scenario of tests/scenarios.txt (default: the gameplay ones) runs the full scenario with
# "-simstats -simwin F 0" (F = 42 by default: frames 0..41 are the level-start screen wipe, and the
# level is loaded before frame 0) and prints a markdown table of the per-frame averages. The CRCs are
# checked against the goldens on the way (-golden, default tests/golden). Scenarios given with -prof
# (default e1m1 map01 demo1) also record a PC profile of the window and print the top functions
# (src/sim/simprof). Everything is written to the -out directory (default build-sim/perf), with the
# simulator of ${SIM_BUILD:-build-sim}/tools. Runs are deterministic: same numbers every time.

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=
FROM=42
ONLY=
PROF=
OUT=$ROOT/build-sim/perf
SCEN=$ROOT/tests/scenarios.txt
GOLDEN=$ROOT/tests/golden
WADS=$ROOT/test-wads/fd/freedoom-0.13.0
RUNNER=$ROOT/src/sim/simrun
SIMPROF=$ROOT/src/sim/simprof
JOBS=${JOBS:-4}

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -from) FROM=$2; shift 2;;
    -only) ONLY="$ONLY $2"; shift 2;;
    -prof) PROF="$PROF $2"; shift 2;;
    -out) OUT=$2; shift 2;;
    -golden) GOLDEN=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <elf> [-from F] [-only NAME]... [-prof NAME]..." >&2; exit 2; }
RUN64=${SIM_BUILD:-$ROOT/build-sim}/tools/sh-elf-run64
[ -x "$RUN64" ] || { echo "perf-sim: needs $RUN64 (make sim-tool)" >&2; exit 2; }
[ -n "$ONLY" ] || ONLY="e1m1 e1m2 e1m3 e2m2 e3m4 demo1 map01 map07 demo2-1"
[ -n "$PROF" ] || PROF="e1m1 map01 demo1"
mkdir -p "$OUT" || exit 2

run_one() {
    name=$1
    line=$(grep -v '^[[:space:]]*#' "$SCEN" | awk -v n="$name" '$1 == n')
    [ -n "$line" ] || { echo "perf-sim: no scenario $name" > "$OUT/$name.log"; return; }
    args=$(echo "${line#"$name"}" | sed -e 's/golden=[^ ]*//' -e "s|\\\$WADS|$WADS|g")
    pp=
    case " $PROF " in *" $name "*) pp="--pcprof $OUT/$name.prof";; esac
    # shellcheck disable=SC2086
    "$RUNNER" --stats $pp "$BIN" $args -crc "$OUT/$name.crc" -simstats -simwin "$FROM" 0 \
        > "$OUT/$name.log" 2>&1 < /dev/null
    echo "rc=$?" >> "$OUT/$name.log"
}

t0=$(date +%s)
n=0
for name in $ONLY; do
    run_one "$name" &
    n=$((n + 1))
    [ $((n % JOBS)) -eq 0 ] && wait
done
wait

kv() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

echo "| scenario | frames | insts/frame | cycles/frame | median | p90 | max | render | game | other | CRC |"
echo "|---|---|---|---|---|---|---|---|---|---|---|"
for name in $ONLY; do
    log=$OUT/$name.log
    s=$(grep '^SIMFRAMES' "$log")
    crc=FAIL
    nf=$(wc -l < "$OUT/$name.crc" 2>/dev/null | tr -d ' ')
    if [ -n "$nf" ] && cmp -s "$GOLDEN/$name.crc" "$OUT/$name.crc"; then crc=ok; fi
    grep -q '^rc=0' "$log" || crc="exit $(sed -n 's/^rc=//p' "$log")"
    [ -n "$s" ] || { echo "| $name | - | - | - | - | - | - | - | - | - | $crc |"; continue; }
    c=$(kv cycles "$s"); r=$(kv render_cycles "$s"); g=$(kv game_cycles "$s"); o=$(kv other_cycles "$s")
    pct() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.1f%%", 100 * a / b }'; }
    ld=$(kv load_frames "$s"); ld=${ld:-0}
    fr=$(kv frames "$s"); [ "$ld" -gt 0 ] && fr="$fr (+$ld load)"
    echo "| $name | $fr | $(kv insts "$s") | $c | $(kv median_cycles "$s") | $(kv p90_cycles "$s") | $(kv max_cycles "$s") | $(pct "$r" "$c") | $(pct "$g" "$c") | $(pct "$o" "$c") | $crc |"
done
echo
echo "(per-frame values over frames $FROM..end without level-load frames; simulator model cycles; the test platform's CRC/present work is excluded)"

for name in $PROF; do
    [ -f "$OUT/$name.prof" ] || continue
    echo
    echo "### $name: top functions (frames $FROM..end)"
    echo '```'
    "$SIMPROF" -n 25 "$BIN" "$OUT/$name.prof" | tee "$OUT/$name.top"
    echo '```'
done
echo
echo "perf-sim: $(( $(date +%s) - t0 ))s, logs in $OUT"
