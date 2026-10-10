#!/usr/bin/env bash
# Builds the SuperH toolchain + fxSDK + fxlibc + gint (dev branch, which has
# fx-CG100 / Graph Math+ support) into ./toolchain, without touching the system.
#
#   tools/build-toolchain.sh            # everything
#   tools/build-toolchain.sh gint       # rebuild just one step
#
# Requires: cmake, libpng, libusb, ncurses (Homebrew), python3 with Pillow.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/toolchain"
SRC="$ROOT/ref"
WORK="$ROOT/toolchain-build"
SYSROOT="$PREFIX/share/fxsdk/sysroot"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

BINUTILS_VERSION=2.42
GCC_VERSION=14.1.0

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="/opt/homebrew/opt/ncurses/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

mkdir -p "$PREFIX/bin" "$WORK"
log() { printf '\n=== [%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

fetch() { # url
  local f="$WORK/$(basename "$1")"
  [[ -f "$f" ]] || curl -fL --retry 3 "$1" -o "$f"
  echo "$f"
}

step_fxsdk() {
  log "fxSDK (host tools)"
  cd "$SRC/fxsdk"
  git checkout -q dev
  cmake -B "$WORK/fxsdk" -S . -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DFXLINK_DISABLE_UDISKS2=1 -DFXLINK_DISABLE_SDL2=1 \
    -DCMAKE_BUILD_TYPE=Release
  make -C "$WORK/fxsdk" -j"$JOBS"
  make -C "$WORK/fxsdk" install
  fxsdk path sysroot
}

step_binutils() {
  log "binutils $BINUTILS_VERSION"
  local a; a=$(fetch "https://ftpmirror.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz")
  cd "$WORK"
  [[ -d binutils-$BINUTILS_VERSION ]] || tar -xf "$a"
  touch "binutils-$BINUTILS_VERSION/intl/plural.c" 2>/dev/null || true
  rm -rf build-binutils && mkdir build-binutils && cd build-binutils
  "../binutils-$BINUTILS_VERSION/configure" --prefix="$SYSROOT" \
    --target=sh3eb-elf --with-multilib-list=m3,m4-nofpu \
    --program-prefix=sh-elf- --enable-libssp --enable-lto \
    --disable-nls --disable-werror --disable-gdb --disable-gprofng \
    --with-system-zlib
  make -j"$JOBS" MAKEINFO=true
  make install-strip MAKEINFO=true
  for t in "$SYSROOT"/bin/sh-elf-*; do ln -sf "$t" "$PREFIX/bin/"; done
}

step_gcc() {
  log "gcc $GCC_VERSION (C/C++ compiler + libgcc)"
  local a; a=$(fetch "https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz")
  cd "$WORK"
  if [[ ! -d gcc-$GCC_VERSION ]]; then
    tar -xf "$a"
    patch -u -N -p0 < "$SRC/sh-elf-gcc/patches/gcc-14.1.0-libgcc-use-soft-fp.patch"
    (cd gcc-$GCC_VERSION && ./contrib/download_prerequisites)
  fi
  mkdir -p "$SYSROOT/sh3eb-elf/bin"
  for t in as ld ar ranlib; do ln -sf "$SYSROOT/bin/sh-elf-$t" "$SYSROOT/sh3eb-elf/bin/$t"; done
  rm -rf build-gcc && mkdir build-gcc && cd build-gcc
  # Some Command Line Tools installs ship clang without a usable libc++ header
  # path; point the host C++ compiler at the SDK's copy explicitly.
  if [[ "$(uname)" == Darwin ]]; then
    export CXX="clang++ -isystem $(xcrun --show-sdk-path)/usr/include/c++/v1"
  fi
  "../gcc-$GCC_VERSION/configure" --prefix="$SYSROOT" --target=sh3eb-elf \
    --with-multilib-list=m3,m4-nofpu --enable-languages=c,c++ \
    --without-headers --program-prefix=sh-elf- --enable-libssp \
    --enable-lto --enable-clocale=generic --enable-libstdcxx-allocator \
    --disable-threads --disable-libstdcxx-verbose \
    --enable-cxx-flags=-fno-exceptions --disable-nls --with-system-zlib
  make -j"$JOBS" MAKEINFO=true all-gcc
  # libgcc's SH-specific unwinder variants race against libgcc_tm.h with -j;
  # retry serially if the parallel build trips over it.
  make -j"$JOBS" MAKEINFO=true all-target-libgcc || make -j1 MAKEINFO=true all-target-libgcc
  make MAKEINFO=true install-strip-gcc install-strip-target-libgcc
  for t in "$SYSROOT"/bin/sh-elf-*; do ln -sf "$t" "$PREFIX/bin/"; done
}

step_openlibm() {
  log "OpenLibm"
  cd "$SRC/OpenLibm"
  local flags=(USEGCC=1 TOOLPREFIX=sh-elf- CC=sh-elf-gcc AR=sh-elf-ar
               libdir="$(fxsdk path lib)" includedir="$(fxsdk path include)")
  make "${flags[@]}" clean >/dev/null 2>&1 || true
  make -j"$JOBS" "${flags[@]}"
  make "${flags[@]}" install-static-superh install-headers-superh
}

step_fxlibc() {
  log "fxlibc (dev)"
  cd "$SRC/fxlibc"
  git checkout -q dev
  rm -rf build-gint
  cmake -B build-gint -DFXLIBC_TARGET=gint -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-sh.cmake \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT/sh3eb-elf"
  make -C build-gint -j"$JOBS"
  make -C build-gint install
}

step_gint() {
  log "gint (dev, fx-CG100 support)"
  cd "$SRC/gint"
  git checkout -q dev
  rm -rf build-cg
  fxsdk build-cg -c
  fxsdk build-cg -j"$JOBS"
  fxsdk build-cg install
}

step_sim() {
  # GDB's SuperH instruction-set simulator (sh-elf-run). Used only for testing
  # and instruction/cycle counting of SH4 code without hardware.
  log "gdb sim (sh-elf-run)"
  local v=15.2
  local a; a=$(fetch "https://ftpmirror.gnu.org/gnu/gdb/gdb-$v.tar.xz")
  cd "$WORK"
  [[ -d gdb-$v ]] || tar -xf "$a"
  rm -rf build-sim && mkdir build-sim && cd build-sim
  "../gdb-$v/configure" --prefix="$PREFIX" --target=sh-elf --program-prefix=sh-elf- \
    --disable-gdb --disable-gdbserver --disable-gprofng --disable-binutils \
    --disable-ld --disable-gas --disable-nls --disable-werror --with-system-zlib \
    --enable-sim --disable-sim-hardware
  make -j"$JOBS" MAKEINFO=true all-sim
  install -m755 sim/sh/run "$PREFIX/bin/sh-elf-run"
}

STEPS=(fxsdk binutils gcc openlibm fxlibc gint)
[[ $# -gt 0 ]] && STEPS=("$@")
for s in "${STEPS[@]}"; do "step_$s"; done
log "done"
