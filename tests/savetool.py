#!/usr/bin/env python3
"""savetool.py - test data for tests/save-demo-robustness.sh (dependency-free, Python 3).

Usage:
  savetool.py bigmap   <in.wad> <out.wad> <MAP> <count>
  savetool.py dropthings <in.wad> <out.wad> <MAP> <type>
  savetool.py setlump  <in.wad> <out.wad> <LUMP> <data-file>
  savetool.py info     <save> <wad> <MAP> <layout>
  savetool.py patch    <save> <out> <wad> <MAP> <layout> <field>=<value> ...

Commands
  bigmap   Copy a WAD with <count> extra things in map <MAP> (THINGS lump): teleport
           destinations (type 14: invisible, outside the blockmap and the sectors' thing
           lists, inert), spread over the map's bounding box. Every one is a mobj record
           in a savegame, so the map's savegame outgrows the engine's savegame buffer.
  dropthings
           Copy a WAD without the things of type <type> in map <MAP> (e.g. 2: the
           player 2 start).
  setlump  Copy a WAD with the data of lump <LUMP> replaced by the bytes of <data-file>
           (e.g. a malformed DEMO1).
  info     Parse a savegame and print the number of records of each kind.
  patch    Write a copy of a savegame with fields changed. Fields:
             header.skill header.episode header.map header.ingameN     (bytes)
             playerN.health playerN.readyweapon playerN.pendingweapon
             playerN.fixedcolormap playerN.pspriteK.state               (player records)
             mobjN.<sprite|frame|type|state|flags|movedir|player|lastlook>
             specialN.sector                                            (special records)
             sectorN.<floorpic|ceilingpic>  sideN.<toptexture|bottomtexture|midtexture>
           playerN is the N-th player record (in-game players in order), mobjN the N-th
           mobj record, specialN the N-th special record; sideN counts the sides in the
           order the world block stores them. Values are integers.

<layout> is the output of `cgdoom-host -savelayout` (one line of key=value pairs): the
records are memory images of the engine's structs, so their sizes and the offsets of the
fields depend on the build. The savegame buffer is assumed to start on an `align`
boundary (it is a zone block), as PADSAVEP pads absolute addresses.
"""
import struct
import sys


def die(msg):
    sys.exit("savetool: " + msg)


# ---- WAD files ----

def read_wad(path):
    with open(path, "rb") as f:
        buf = f.read()
    ident, n, diroff = struct.unpack_from("<4sii", buf, 0)
    if ident not in (b"IWAD", b"PWAD"):
        die(path + ": not a WAD")
    lumps = []
    for i in range(n):
        pos, size, name = struct.unpack_from("<ii8s", buf, diroff + 16 * i)
        lumps.append([name.rstrip(b"\0").upper(), buf[pos:pos + size]])
    return ident, lumps


def write_wad(path, ident, lumps):
    data = bytearray(12)
    entries = []
    for name, content in lumps:
        while len(data) % 4:
            data.append(0)
        entries.append((len(data), len(content), name))
        data += content
    while len(data) % 4:
        data.append(0)
    diroff = len(data)
    for pos, size, name in entries:
        data += struct.pack("<ii8s", pos, size, name)
    struct.pack_into("<4sii", data, 0, ident, len(entries), diroff)
    with open(path, "wb") as f:
        f.write(data)


def find_lump(lumps, name, start=0):
    name = name.upper().encode()
    for i in range(start, len(lumps)):
        if lumps[i][0] == name:
            return i
    die("lump %s not found" % name.decode())


def map_lump(lumps, mapname, lumpname):
    """Index of the lump <lumpname> of map <mapname> (the lumps after the marker)."""
    m = find_lump(lumps, mapname)
    for i in range(m + 1, min(m + 12, len(lumps))):
        if lumps[i][0] == lumpname.encode():
            return i
    die("map %s has no %s" % (mapname, lumpname))


def cmd_bigmap(inp, out, mapname, count):
    ident, lumps = read_wad(inp)
    count = int(count)
    vx = lumps[map_lump(lumps, mapname, "VERTEXES")][1]
    xs = [struct.unpack_from("<hh", vx, i)[0] for i in range(0, len(vx), 4)]
    ys = [struct.unpack_from("<hh", vx, i)[1] for i in range(0, len(vx), 4)]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    t = map_lump(lumps, mapname, "THINGS")
    things = bytearray(lumps[t][1])
    side = 1
    while side * side < count:
        side += 1
    for i in range(count):
        x = x0 + (x1 - x0) * (i % side) // side
        y = y0 + (y1 - y0) * (i // side) // side
        # x, y, angle, type 14 (teleport destination), all skills
        things += struct.pack("<hhhhh", x, y, 0, 14, 7)
    lumps[t][1] = bytes(things)
    write_wad(out, ident, lumps)


def cmd_dropthings(inp, out, mapname, thingtype):
    ident, lumps = read_wad(inp)
    t = map_lump(lumps, mapname, "THINGS")
    things = lumps[t][1]
    lumps[t][1] = b"".join(things[i:i + 10] for i in range(0, len(things) - 9, 10)
                           if struct.unpack_from("<h", things, i + 6)[0] != int(thingtype))
    write_wad(out, ident, lumps)


def cmd_setlump(inp, out, lumpname, datafile):
    ident, lumps = read_wad(inp)
    with open(datafile, "rb") as f:
        lumps[find_lump(lumps, lumpname)][1] = f.read()
    write_wad(out, ident, lumps)


# ---- Savegames ----

TC_END, TC_MOBJ = 0, 1
TC_ENDSPECIALS = 7
SPECIALS = ["ceiling", "door", "floor", "plat", "flash", "strobe", "glow"]


def parse_layout(text):
    words = text.split()
    if not words or words[0] != "SAVELAYOUT":
        die("bad layout (expected the output of cgdoom-host -savelayout)")
    return {k: int(v) if v.lstrip("-").isdigit() else v
            for k, v in (w.split("=", 1) for w in words[1:])}


def world_size(lumps, mapname):
    """Bytes of the world block (P_ArchiveWorld): 7 shorts per sector, 3 per line
    and 5 per side of each line."""
    nsectors = len(lumps[map_lump(lumps, mapname, "SECTORS")][1]) // 26
    ld = lumps[map_lump(lumps, mapname, "LINEDEFS")][1]
    shorts = nsectors * 7
    linesides = []
    for i in range(0, len(ld) - 13, 14):
        s0, s1 = struct.unpack_from("<hh", ld, i + 10)
        n = (s0 != -1) + (s1 != -1)
        shorts += 3 + 5 * n
        linesides.append(n)
    return nsectors, linesides, shorts * 2


def parse_save(buf, lumps, mapname, lay):
    align = lay["align"]

    def pad(o):
        return (o + align - 1) // align * align

    rec = {"players": [], "mobjs": [], "specials": []}
    o = lay["header"]
    if len(buf) < o:
        die("savegame shorter than its header")
    ingame = buf[24 + 16 + 3: 24 + 16 + 3 + lay["maxplayers"]]
    for i in range(lay["maxplayers"]):
        if ingame[i]:
            o = pad(o)
            rec["players"].append(o)
            o += lay["player"]
    nsectors, linesides, wsize = world_size(lumps, mapname)
    rec["world"] = o
    rec["nsectors"], rec["linesides"] = nsectors, linesides
    o += wsize
    while True:
        cls = buf[o]
        o += 1
        if cls == TC_END:
            break
        if cls != TC_MOBJ:
            die("unknown thinker class %d at %d" % (cls, o - 1))
        o = pad(o)
        rec["mobjs"].append(o)
        o += lay["mobj"]
    while True:
        cls = buf[o]
        o += 1
        if cls == TC_ENDSPECIALS:
            break
        if cls >= len(SPECIALS):
            die("unknown special class %d at %d" % (cls, o - 1))
        o = pad(o)
        rec["specials"].append((cls, o))
        o += lay["special%d" % cls]
    if buf[o] != 0x1D or o + 1 != len(buf):
        die("no consistency marker at the end (parsed up to %d of %d)" % (o, len(buf)))
    return rec


def load(save, wad, mapname, layout):
    with open(save, "rb") as f:
        buf = bytearray(f.read())
    with open(layout) as f:
        lay = parse_layout(f.read())
    _, lumps = read_wad(wad)
    return buf, lay, parse_save(buf, lumps, mapname.upper(), lay)


def cmd_info(save, wad, mapname, layout):
    buf, lay, rec = load(save, wad, mapname, layout)
    print("size=%d players=%d world=%d sectors=%d sides=%d mobjs=%d specials=%d" % (
        len(buf), len(rec["players"]), rec["world"], rec["nsectors"],
        sum(rec["linesides"]), len(rec["mobjs"]), len(rec["specials"])))
    print("specials: " + " ".join(SPECIALS[c] for c, _ in rec["specials"]))


def put_int(buf, off, size, value, lay):
    end = "<" if lay["endian"] == "little" else ">"
    fmt = {1: "b", 2: "h", 4: "i", 8: "q"}[size]
    if size == 1 and value > 127:
        fmt = "B"
    struct.pack_into(end + fmt, buf, off, value)


def field_offset(rec, lay, field):
    """(offset, size) of a field name of the patch command"""
    obj, name = field.split(".", 1)
    ptr = lay["ptr"]

    def index(prefix):
        return int(obj[len(prefix):])

    if obj == "header":
        if name.startswith("ingame"):
            return 24 + 16 + 3 + int(name[6:]), 1
        return 24 + 16 + ["skill", "episode", "map"].index(name), 1
    if obj.startswith("player"):
        base = rec["players"][index("player")]
        if name.startswith("psprite"):
            k = int(name[7:name.index(".")])
            return base + lay["player.psprites"] + k * lay["pspdef"], ptr
        return base + lay["player." + name], 4
    if obj.startswith("mobj"):
        base = rec["mobjs"][index("mobj")]
        return base + lay["mobj." + name], ptr if name in ("state", "player") else 4
    if obj.startswith("special"):
        cls, base = rec["specials"][index("special")]
        return base + lay["special%d.sector" % cls], ptr
    if obj.startswith("sector"):
        n = index("sector")
        return rec["world"] + 2 * (7 * n + ["floorpic", "ceilingpic"].index(name) + 2), 2
    if obj.startswith("side"):
        # the n-th side in world order: lines are 3 shorts, then 5 per side
        n = index("side")
        o = rec["world"] + 2 * 7 * rec["nsectors"]
        for sides in rec["linesides"]:
            o += 2 * 3
            if n < sides:
                return o + 2 * (5 * n + 2 + ["toptexture", "bottomtexture",
                                             "midtexture"].index(name)), 2
            n -= sides
            o += 2 * 5 * sides
        die("no side %d" % index("side"))
    die("unknown field " + field)


def cmd_patch(save, out, wad, mapname, layout, *assignments):
    buf, lay, rec = load(save, wad, mapname, layout)
    for a in assignments:
        field, value = a.split("=", 1)
        off, size = field_offset(rec, lay, field)
        put_int(buf, off, size, int(value, 0), lay)
    with open(out, "wb") as f:
        f.write(buf)


def main(argv):
    cmds = {"bigmap": (cmd_bigmap, 4), "dropthings": (cmd_dropthings, 4),
            "setlump": (cmd_setlump, 4), "info": (cmd_info, 4), "patch": (cmd_patch, 6)}
    if len(argv) < 2 or argv[1] not in cmds or len(argv) - 2 < cmds[argv[1]][1]:
        sys.exit(__doc__)
    fn, n = cmds[argv[1]]
    if argv[1] != "patch" and len(argv) - 2 != n:
        sys.exit(__doc__)
    fn(*argv[2:])


if __name__ == "__main__":
    main(sys.argv)
