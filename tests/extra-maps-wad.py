#!/usr/bin/env python3
"""extra-maps-wad.py IN.wad OUT.wad NAME... : copy a Doom II IWAD and append maps.

Each NAME (MAP33, MAP34...) is a copy of MAP01 (the marker lump and the map data lumps that
follow it) under that name, added at the end of the directory. Used by tests/map-limit.sh to get
a WAD with maps past MAP32 (the BFG/Unity Doom II has a MAP33) without shipping a WAD file."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import wadtool  # noqa: E402

if len(sys.argv) < 4:
    sys.exit("usage: extra-maps-wad.py IN.wad OUT.wad NAME...")
ident, lumps, _ = wadtool.read_wad(sys.argv[1])
names = [l.name for l in lumps]
if b"MAP01" not in names:
    sys.exit("extra-maps-wad: %s has no MAP01" % sys.argv[1])
first = names.index(b"MAP01")
n = 1
while first + n < len(lumps) and lumps[first + n].name in wadtool.MAP_LUMPS:
    n += 1
src = lumps[first:first + n]
for new in sys.argv[3:]:
    new = new.upper().encode("ascii")
    if new in names:
        sys.exit("extra-maps-wad: %s already has %s" % (sys.argv[1], new.decode()))
    for i, l in enumerate(src):
        lumps.append(wadtool.Lump(new if i == 0 else l.name, l.data, 0))
wadtool.write_wad(sys.argv[2], ident, lumps)
