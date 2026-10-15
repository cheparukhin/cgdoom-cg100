#!/bin/sh
# True colour against 8-bit (make test-tc-exact).
#
#   tc-exact.sh -bin8 <8-bit cgdoom-host> -bintc <true-colour cgdoom-host>
#               [-out DIR] [-only NAME]...
#
# Runs every scenario of tests/scenarios.txt with both builds of the same
# screen size: the 8-bit one with -palredraw (its status bar redrawn at each
# palette change, as the true-colour build's must be), the true-colour one
# with -tcexact (light tables from COLORMAP) and -features none. Their
# frames as RGB565 (-crc565) must be equal, except the frames that differ on
# purpose (flagged F by -crc565: spectre fuzz, whose true-colour darkening
# is r_draw.c's FUZZDARK; the frames after a gamma change and the wipes
# across a palette change, where the 8-bit palette also recolours pixels
# drawn earlier).
# Exit status 0 = PASS.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN8=
BINTC=
OUT=$ROOT/build-host-hd/tests/tcexact
ONLY=
WADS=$ROOT/test-wads/fd/freedoom-0.13.0

while [ $# -gt 0 ]; do
    case "$1" in
    -bin8) BIN8=$2; shift 2;;
    -bintc) BINTC=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    -only) ONLY="$ONLY $2"; shift 2;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN8" ] && [ -n "$BINTC" ] || {
    echo "usage: $0 -bin8 <8-bit host> -bintc <true-colour host>" >&2; exit 2; }
mkdir -p "$OUT" || exit 2

grep -v '^[[:space:]]*#' "$ROOT/tests/scenarios.txt" | grep -v '^[[:space:]]*$' \
    > "$OUT/.scenarios"
pass=0
fail=0
while read -r name rest; do
    if [ -n "$ONLY" ]; then
        case " $ONLY " in *" $name "*) ;; *) continue;; esac
    fi
    case "$rest" in golden=*) rest=${rest#* };; esac
    args=$(echo "$rest" | sed "s|\\\$WADS|$WADS|g")
    # shellcheck disable=SC2086
    "$BIN8" $args -palredraw -crc565 "$OUT/$name.8.c565" \
        > "$OUT/$name.8.log" 2>&1 < /dev/null
    rc8=$?
    # shellcheck disable=SC2086
    "$BINTC" $args -tcexact -features none -crc565 "$OUT/$name.tc.c565" \
        > "$OUT/$name.tc.log" 2>&1 < /dev/null
    rctc=$?
    if [ $rc8 != 0 ] || [ $rctc != 0 ]; then
        echo "FAIL $name: exit status $rc8 / $rctc"
        fail=$((fail + 1))
        continue
    fi
    if res=$(sh "$ROOT/tests/crc565-compare.sh" "$OUT/$name.8.c565" \
            "$OUT/$name.tc.c565"); then
        echo "PASS $name ($(echo "$res" | tail -1))"
        pass=$((pass + 1))
    else
        echo "FAIL $name"
        echo "$res" | sed 's/^/    /'
        fail=$((fail + 1))
    fi
done < "$OUT/.scenarios"
echo "---- tc-exact: $pass passed, $fail failed"
[ $fail = 0 ]
