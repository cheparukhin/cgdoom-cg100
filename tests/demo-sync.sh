#!/bin/sh
# Demo sync regression test (make test-demosync / make test-demosync-sim).
#
#   demo-sync.sh -bin <cgdoom-host or cgdoom-sim.elf> [-runner <simrun>]
#                [-out DIR] [-update]
#
# Plays DEMO1..DEMO3 of the shareware DOOM v1.9 IWAD (doom1.wad, md5
# f0cefca49926d00903cf57551d901abe) given by $CGD_DOOM1_WAD with -demotrace,
# and compares each per-tic game-state trace with tests/demosync/demoN.ckpt:
# the SHA-256 and line count of the whole trace, plus every 128th line and the
# last two lines (to locate a divergence). The expected traces were checked
# tic by tic against id's linuxdoom-1.10 (the oracle built by
# tools/demosync-oracle/build.sh); the .ckpt files hold only data derived from
# our engine, no WAD data.
#
# The IWAD is not part of the repository: without $CGD_DOOM1_WAD (or with a
# different file) the test is skipped with a message and exit status 0.
# -update rewrites the .ckpt files from the current engine (only after the
# traces were checked against the oracle again).

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=
RUNNER=
OUT=$ROOT/build-host/tests/demosync
UPDATE=0
EXP=$ROOT/tests/demosync
MD5_WANT=f0cefca49926d00903cf57551d901abe
EVERY=128

while [ $# -gt 0 ]; do
    case "$1" in
    -bin) BIN=$2; shift 2;;
    -runner) RUNNER=$2; shift 2;;
    -out) OUT=$2; shift 2;;
    -update) UPDATE=1; shift;;
    *) echo "unknown option $1" >&2; exit 2;;
    esac
done
[ -n "$BIN" ] || { echo "usage: $0 -bin <binary> [-runner R] [-out DIR] [-update]" >&2; exit 2; }

WAD=${CGD_DOOM1_WAD:-}
if [ -z "$WAD" ] || [ ! -f "$WAD" ]; then
    echo "SKIP test-demosync: set CGD_DOOM1_WAD to the shareware DOOM v1.9 doom1.wad (md5 $MD5_WANT)"
    exit 0
fi
if command -v md5 >/dev/null 2>&1; then md5=$(md5 -q "$WAD")
else md5=$(md5sum "$WAD" | cut -d' ' -f1); fi
if [ "$md5" != "$MD5_WANT" ]; then
    echo "SKIP test-demosync: $WAD is not the shareware DOOM v1.9 doom1.wad (md5 $md5, want $MD5_WANT)"
    exit 0
fi

sha256() {
    if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else sha256sum "$1" | cut -d' ' -f1; fi
}

# checkpoint file of a trace: hash, line count, every $EVERY-th line, last 2 lines
ckpt() {
    echo "sha256 $(sha256 "$1") lines $(wc -l < "$1" | tr -d ' ')"
    awk -v e=$EVERY '(NR - 1) % e == 0' "$1"
    tail -n 2 "$1"
}

mkdir -p "$OUT" || exit 2
pass=0
fail=0
for n in 1 2 3; do
    trace=$OUT/demo$n.trace
    rm -f "$trace"
    start=$(date +%s)
    # -frameskip only skips rendering; the game state is the same
    # shellcheck disable=SC2086
    $RUNNER "$BIN" -iwad "$WAD" -playdemo DEMO$n -tics 1000000 -frameskip 7 \
        -demotrace "$trace" > "$OUT/demo$n.log" 2>&1 < /dev/null
    rc=$?
    secs=$(( $(date +%s) - start ))
    if [ $rc -ne 0 ] || [ ! -s "$trace" ]; then
        echo "FAIL demosync DEMO$n: exit status $rc, no trace (see $OUT/demo$n.log)"
        fail=$((fail + 1)); continue
    fi
    ckpt "$trace" > "$OUT/demo$n.ckpt"
    if [ $UPDATE = 1 ]; then
        cp "$OUT/demo$n.ckpt" "$EXP/demo$n.ckpt"
        echo "UPDATED tests/demosync/demo$n.ckpt ($(head -n 1 "$OUT/demo$n.ckpt"))"
        continue
    fi
    if cmp -s "$OUT/demo$n.ckpt" "$EXP/demo$n.ckpt"; then
        echo "PASS demosync DEMO$n ($(wc -l < "$trace" | tr -d ' ') trace lines, ${secs}s; $(tail -n 1 "$trace"))"
        pass=$((pass + 1))
    else
        first=$(diff "$EXP/demo$n.ckpt" "$OUT/demo$n.ckpt" | grep '^[<>]' | head -n 2 | tr '\n' ' ')
        echo "FAIL demosync DEMO$n: trace differs from tests/demosync/demo$n.ckpt"
        echo "     first difference (expected <, got >): $first"
        echo "     trace: $trace"
        fail=$((fail + 1))
    fi
done
[ $UPDATE = 1 ] && exit 0
echo "---- test-demosync: $pass passed, $fail failed"
[ $fail -eq 0 ]
