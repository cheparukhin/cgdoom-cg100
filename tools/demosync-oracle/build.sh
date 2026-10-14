#!/bin/sh
# Build the linuxdoom-1.10 demo-sync oracle: the reference that
# tests/demosync/*.ckpt are checked against before tests/demo-sync.sh -update.
#
#   tools/demosync-oracle/build.sh [linuxdoom-1.10.src.tgz] [OUTDIR]
#
# Unpacks id's linuxdoom-1.10 source (default: the pristine GPL release kept in
# ref/extra/EDiasAlberto_DoomCasioPrizm/doomsrc/linuxdoom-1.10.src.tgz; ref/ is
# only read), strips the CRs, drops i_main/i_net/i_sound/i_system/i_video,
# applies oracle.patch and builds OUTDIR/oracle (default build-host/oracle)
# with the host cc. oracle.patch only makes the game run headless on a 64-bit
# host, and adds the trace; the game logic is id's:
#   - g_game.c: accept demo version 109 (v1.9) as Chocolate Doom does; call
#     ORACLE_TraceTic() at the end of G_Ticker;
#   - 64-bit: pointer arrays allocated with 8-byte entries (p_setup.c
#     P_GroupLines, r_data.c), maptexture_t's obsolete on-disk pointer is an
#     int, intptr_t casts (r_data.c colormaps, r_draw.c translation tables),
#     8-byte zone alignment (z_zone.c), no pointer-in-int defaults (m_misc.c
#     chat macros, sndserver);
#   - info.c: sprnames[] is NULL-terminated (R_InitSpriteDefs relies on it).
#
# Run (HOME: an empty directory, so no ~/.doomrc is read; DOOMWADDIR: a
# directory with only doom1.wad, or doom.wad/doomu.wad/doom2.wad for other
# IWADs):
#   HOME=/tmp/empty DOOMWADDIR=/path/to/wads ORACLE_TRACE=demo1.txt \
#       build-host/oracle/oracle -playdemo demo1
# A demo file x.lmp in the current directory plays with "-playdemo x".
# Compare with: cgdoom-host -iwad doom1.wad -playdemo DEMO1 -demotrace t.txt

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TGZ=${1:-$ROOT/ref/extra/EDiasAlberto_DoomCasioPrizm/doomsrc/linuxdoom-1.10.src.tgz}
OUT=${2:-$ROOT/build-host/oracle}
CC=${CC:-cc}

[ -f "$TGZ" ] || { echo "no $TGZ" >&2; exit 1; }
rm -rf "$OUT/src" "$OUT/tgz"
mkdir -p "$OUT/src" "$OUT/tgz" || exit 1
tar xzf "$TGZ" -C "$OUT/tgz" || exit 1
for f in "$OUT"/tgz/linuxdoom-1.10/*.[ch]; do
    tr -d '\r' < "$f" > "$OUT/src/$(basename "$f")"
done
rm -rf "$OUT/tgz"
cd "$OUT/src" || exit 1
rm -f i_main.c i_net.c i_sound.c i_system.c i_video.c
patch -p1 -s < "$HERE/oracle.patch" || { echo "oracle.patch does not apply" >&2; exit 1; }
cp "$HERE/oracle_sys.c" .
$CC -std=gnu89 -O1 -g -w -DNORMALUNIX -fno-strict-aliasing -fwrapv \
    -I"$HERE/compat" -include stdint.h ./*.c -o "$OUT/oracle" || exit 1
echo "built $OUT/oracle"
