#!/bin/sh
# Host unit tests for the gint platform layer (src/gint), with stubbed gint
# headers (stub/). Nothing here runs on the calculator: these tests cover
# the pure logic of the platform (file layer, WAD access, UTLB planning,
# heap discovery, input waits, tic/chord/idle timing, keymap/name helpers,
# pixel expansion, fatal-error hints).
#
#   sh tests/gint-host/run.sh            build into $OUT and run every test
#   CGD_TEST_WADS="a.wad b.wad" sh ...   also run the WAD tests on these
#   SANITIZE=0 sh ...                    build without ASan/UBSan
#
# By default the freedoom WADs of test-wads/ are used when present.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/cgd-gint-host}
CC=${CC:-cc}
mkdir -p "$OUT"

CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -DCGD_HOST_TEST \
 -I$HERE/stub -I$ROOT/src/gint -I$ROOT/src/platform -I$ROOT/src/engine"
if [ "${SANITIZE:-1}" != 0 ]; then
	CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all \
 -fno-omit-frame-pointer"
	export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
fi
G=$ROOT/src/gint
E=$ROOT/src/engine

if [ -z "${CGD_TEST_WADS+x}" ]; then
	CGD_TEST_WADS=$(find "$ROOT/test-wads" -name 'freedoom*.wad' 2>/dev/null | sort)
fi

pass=0; fail=0; failed=""
run() {
	name=$1; shift
	printf '== %s\n' "$name"
	if "$@"; then pass=$((pass + 1)); else fail=$((fail + 1)); failed="$failed $name"; fi
}
build() {
	exe=$1; shift
	# shellcheck disable=SC2086
	$CC $CFLAGS -o "$OUT/$exe" "$@" && return 0
	echo "BUILD FAILED: $exe"
	fail=$((fail + 1)); failed="$failed $exe"
	return 1
}

# test_files.c #includes files.c (malloc interposed)
build test_files   "$HERE/test_files.c" "$G/wad.c" \
	&& run files "$OUT/test_files" $CGD_TEST_WADS
build test_wadmap "$HERE/test_wadmap.c" "$G/files.c" \
	&& run wadmap "$OUT/test_wadmap"
build test_tlb     "$HERE/test_tlb.c" "$G/tlbplan.c" \
	&& run tlb "$OUT/test_tlb"
build test_memory  "$HERE/test_memory.c" "$G/memory.c" "$G/no_osheap.c" \
	&& run memory "$OUT/test_memory"
build test_input   "$HERE/test_input.c" "$G/input.c" \
	&& run input "$OUT/test_input"
# test_time.c #includes time.c (gint timers/profiler/sleep stubbed)
build test_time    "$HERE/test_time.c" "$G/input.c" \
	&& run time "$OUT/test_time"
build test_keymaps  "$HERE/test_keymaps.c" "$G/keymaps.c" "$E/cgdoom-kbd.c" \
	&& run keymaps "$OUT/test_keymaps"
build test_names   "$HERE/test_names.c" "$G/wadnames.c" "$E/cgdoom-names.c" \
	&& run names "$OUT/test_names"
build test_expand  "$HERE/test_expand.c" "$G/video.c" \
	&& run expand "$OUT/test_expand"
build test_hints   "$HERE/test_hints.c" "$G/hints.c" \
	&& run hints "$OUT/test_hints"

echo "gint-host: $pass passed, $fail failed${failed:+ (}${failed# }${failed:+)}"
[ "$fail" -eq 0 ]
