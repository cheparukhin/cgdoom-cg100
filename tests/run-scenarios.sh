#!/bin/sh
# Run the scenarios of tests/scenarios.txt with a cgdoom binary.
#
#   run-scenarios.sh -mode golden|test|run -bin <binary> [options]
#
#   -mode golden   write tests/golden/<name>.crc (variants with golden= skipped)
#   -mode test     compare each run with its golden, print PASS/FAIL
#   -mode run      only check that every scenario exits successfully
#   -runner CMD    prefix command (e.g. "src/sim/simrun"), default none
#   -only NAME     run a single scenario (repeatable)
#   -scenarios F   scenario file (default tests/scenarios.txt)
#   -golden DIR    golden directory (default tests/golden)
#   -out DIR       output directory for CRCs and logs (default build-host/tests)
#   -extra ARGS    extra arguments appended to every run (e.g. "-noasm")
#
# Exit status: 0 if everything passed.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=test
BIN=
RUNNER=
ONLY=
SCEN=$ROOT/tests/scenarios.txt
GOLDEN=$ROOT/tests/golden
OUT=$ROOT/build-host/tests
EXTRA=
WADS=$ROOT/test-wads/fd/freedoom-0.13.0

while [ $# -gt 0 ]; do
    case "$1" in
    -mode) MODE=$2; shift 2;;
    -bin) BIN=$2; shift 2;;
    -runner) RUNNER=$2; shift 2;;
    -only) ONLY="$ONLY $2"; shift 2;;
    -scenarios) SCEN=$2; shift 2;;
    -golden) GOLDEN=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    -extra) EXTRA=$2; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done

if [ -z "$BIN" ]; then
    echo "usage: $0 -mode golden|test|run -bin <binary> [options]" >&2
    exit 2
fi
case "$MODE" in golden|test|run) ;; *) echo "bad mode $MODE" >&2; exit 2;; esac

mkdir -p "$OUT" || exit 2
[ "$MODE" = golden ] && { mkdir -p "$GOLDEN" || exit 2; }

pass=0
fail=0
total_start=$(date +%s)

# Read scenario lines (strip comments and blank lines)
grep -v '^[[:space:]]*#' "$SCEN" | grep -v '^[[:space:]]*$' > "$OUT/.scenarios"

while read -r name rest; do
    if [ -n "$ONLY" ]; then
        case " $ONLY " in *" $name "*) ;; *) continue;; esac
    fi

    ref=$name
    case "$rest" in
    golden=*)
        ref=${rest%% *}; ref=${ref#golden=}; rest=${rest#* }
        [ "$MODE" = golden ] && continue;;
    esac

    args=$(echo "$rest" | sed "s|\\\$WADS|$WADS|g")
    crc=$OUT/$name.crc
    log=$OUT/$name.log
    rm -f "$crc"

    start=$(date +%s)
    # shellcheck disable=SC2086
    $RUNNER $BIN $args $EXTRA -crc "$crc" -stats > "$log" 2>&1 < /dev/null
    rc=$?
    secs=$(( $(date +%s) - start ))

    frames=0
    [ -f "$crc" ] && frames=$(wc -l < "$crc" | tr -d ' ')
    zone=$(sed -n 's/^zone: .*peak_used=\([0-9]*\) KiB peak_static=\([0-9]*\) KiB.*/used \1K static \2K/p' "$log")
    ftime=$(sed -n 's/^frame time: avg=\([0-9.]*\) ms.*/\1 ms\/frame/p' "$log")
    info="$frames frames, ${secs}s, zone peak $zone${ftime:+, $ftime}"

    if [ $rc -ne 0 ]; then
        echo "FAIL $name: exit status $rc (see $log)"
        tail -5 "$log" | sed 's/^/     | /'
        fail=$((fail + 1))
        continue
    fi

    case "$MODE" in
    golden)
        cp "$crc" "$GOLDEN/$name.crc"
        echo "GOLDEN $name ($info)"
        pass=$((pass + 1));;
    run)
        echo "OK   $name ($info)"
        pass=$((pass + 1));;
    test)
        if [ ! -f "$GOLDEN/$ref.crc" ]; then
            echo "FAIL $name: no golden $GOLDEN/$ref.crc (run make golden)"
            fail=$((fail + 1))
        elif cmp -s "$crc" "$GOLDEN/$ref.crc"; then
            echo "PASS $name ($info)"
            pass=$((pass + 1))
        else
            first=$(diff "$crc" "$GOLDEN/$ref.crc" | sed -n '2p')
            echo "FAIL $name: differs from $ref.crc; first diff: $first"
            fail=$((fail + 1))
        fi;;
    esac
done < "$OUT/.scenarios"

echo "---- $MODE: $pass passed, $fail failed ($(( $(date +%s) - total_start ))s)"
[ $fail -eq 0 ]
