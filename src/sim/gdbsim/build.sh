#!/bin/bash
# Build "sh-elf-run64": the GDB SH simulator from toolchain-build/ with src/sim/gdbsim/sim64.patch
# (64-bit statistics counters, sim_counters()/sim_mark() services, SIM_PCPROF per-PC profile).
# Only interp.c and sh-sim.h change; everything else is linked from the existing toolchain-build
# objects, so this takes a few seconds.  Output: build-sim/tools/sh-elf-run64
#
# usage: src/sim/gdbsim/build.sh [-f]      (-f: rebuild even if up to date)
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
TB="$ROOT/toolchain-build"
SRC="$TB/gdb-15.2/sim"
BLD="$TB/build-sim"
OUT="${SIM_BUILD:-$ROOT/build-sim}/tools"
WORK="${SIM_BUILD:-$ROOT/build-sim}/gdbsim"
BIN="$OUT/sh-elf-run64"

if [ ! -f "$BLD/sim/sh/libsim.a" ] || [ ! -f "$SRC/sh/interp.c" ]; then
	echo "build.sh: toolchain-build/ (gdb-15.2 sim sources and objects) not found under $TB" >&2
	exit 1
fi
if [ "${1:-}" != "-f" ] && [ -x "$BIN" ] && [ "$BIN" -nt "$HERE/sim64.patch" ] && [ "$BIN" -nt "$HERE/build.sh" ]; then
	echo "$BIN is up to date"
	exit 0
fi

rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
cp "$SRC/sh/interp.c" "$SRC/sh/sh-sim.h" "$WORK/"
(cd "$WORK" && patch -p1 -s < "$HERE/sim64.patch")

cd "$WORK"
cc -g -O2 -DHAVE_CONFIG_H -DSIM_TOPDIR_BUILD -DWITH_HW=0 -DDEFAULT_INLINE=0 \
	-I. -I"$BLD/sim/sh" -I"$SRC/sh" -I"$BLD/sim" -I"$SRC" -I"$SRC/.." -I"$SRC/../include" \
	-I"$BLD/bfd" -I"$SRC/common" -I"$BLD/sim/common" -I"$BLD" -I"$BLD/gnulib/import" \
	-I"$TB/gdb-15.2/gnulib/import" -I"$BLD/gnulib" \
	-w -c interp.c -o interp.o

ZSTD_LIB="${ZSTD_LIB:--L/opt/homebrew/opt/zstd/lib}"
cc -o "$BIN" "$BLD/sim/sh/nrun.o" interp.o "$BLD/sim/sh/libsim.a" "$BLD/opcodes/.libs/libopcodes.a" \
	"$BLD/bfd/.libs/libbfd.a" "$BLD/libsframe/.libs/libsframe.a" "$BLD/libiberty/libiberty.a" \
	"$BLD/gnulib/import/libgnu.a" $ZSTD_LIB -lzstd -lz -ltermcap
echo "built $BIN"
