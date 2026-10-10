#!/usr/bin/env python3
"""wadtool.py - WAD preparation tool for the CGDoom fx-CG100 port (dependency-free, Python 3).

Usage:
  wadtool.py info   <wad>
  wadtool.py strip  <in> <out> [--keep-demos | --drop-demos] [--drop-ends]
  wadtool.py align  <in> <out> [--align N]            (default N=4)
  wadtool.py pack   <in> <out> [--drop-demos] [--drop-ends] [--align N]
  wadtool.py verify <a> <b>
  wadtool.py fetch-wad [<out>] [--drop-demos] [--save-original <path>]

Commands
  info    Type, lump count, size, per-category breakdown (count and bytes) and the
          number of lumps whose data offset is not 4-byte aligned.
  strip   Remove what CGDoom never uses (no sound, no music):
            - sound effects DS*/DP* (outside sprite/flat/patch namespaces and not
              music-headed), identified by DMX header (format 3 = DS, 0 = DP)
            - music: any lump named D_* whose data starts with 'MUS\\x1a' or 'MThd'
              (D_START/D_END style markers and other D_ lumps are kept)
            - GENMIDI, DMXGUS, DMXGUSC
          --drop-demos  also removes DEMO1..DEMOn (default: keep)
          --drop-ends   also removes ENDOOM (text screen, unused by the engine)
          Everything else (map lumps, markers S_START/F_START/P_START..., textures,
          PLAYPAL, COLORMAP, ...) is kept in original order.
  align   Rewrite the WAD so every non-empty lump's data offset is a multiple of N
          (zero padding), directory at the end (also aligned). Order, names and
          contents are preserved. Zero-size lumps get the current (aligned) offset.
  pack    strip + align; the recommended command for the calculator. Prints before and
          after sizes and whether the result fits the budget
          (4,500,000 - 450,000 = 4,050,000 bytes).
  verify  Check that b is a (name, content)-identical ordered subset of a; report
          lumps of b that differ or do not exist in a, and the lumps dropped from a.
          Exit status 0 on success, 1 on any difference.
  fetch-wad
          Download the shareware doom1.wad v1.9 (from Debian's doom-wad-shareware
          package, an unmodified copy), check its SHA-256, and pack it like
          `pack --drop-ends` into <out> (default DOOM1.WAD), ready to copy to the
          calculator. --save-original also keeps the unmodified doom1.wad.

Note: lumps crossing a fragmented flash block boundary cannot be zero-copy mapped; the
device-side PL_WadMap falls back to a RAM copy in that case, nothing to do here.
"""
import hashlib
import io
import os
import shutil
import ssl
import struct
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

STORAGE_BUDGET = 4_500_000
ADDIN_BUDGET = 450_000
WAD_BUDGET = STORAGE_BUDGET - ADDIN_BUDGET

MAP_LUMPS = {b"THINGS", b"LINEDEFS", b"SIDEDEFS", b"VERTEXES", b"SEGS", b"SSECTORS",
             b"NODES", b"SECTORS", b"REJECT", b"BLOCKMAP"}


def die(msg):
    sys.exit("wadtool: " + msg)


class Lump:
    __slots__ = ("name", "data", "offset", "ns")

    def __init__(self, name, data, offset):
        self.name = name      # bytes, NUL-stripped, upper-case as stored
        self.data = data      # bytes
        self.offset = offset  # original file offset
        self.ns = None        # namespace: 'S','F','P' (inside X_START..X_END) or None

    @property
    def sname(self):
        return self.name.decode("latin-1")


def read_wad(path):
    with open(path, "rb") as f:
        buf = f.read()
    return parse_wad(buf, path)


def parse_wad(buf, path):
    if len(buf) < 12:
        die("%s: too short" % path)
    ident, n, diroff = struct.unpack_from("<4sii", buf, 0)
    if ident not in (b"IWAD", b"PWAD"):
        die("%s: not a WAD (id %r)" % (path, ident))
    if n < 0 or diroff < 0 or diroff + 16 * n > len(buf):
        die("%s: bad directory" % path)
    lumps = []
    for i in range(n):
        off, size, nm = struct.unpack_from("<ii8s", buf, diroff + 16 * i)
        nm = nm.split(b"\0", 1)[0]
        if size and (off < 0 or off + size > len(buf)):
            die("%s: lump %d (%s) out of range" % (path, i, nm.decode("latin-1")))
        lumps.append(Lump(nm, buf[off:off + size] if size else b"", off))
    mark_namespaces(lumps)
    return ident, lumps, len(buf)


def mark_namespaces(lumps):
    """Tag lumps between S_START/SS_START, F_START/FF_START, P_START/PP_START and ends."""
    starts = {b"S_START": "S", b"SS_START": "S", b"F_START": "F", b"FF_START": "F",
              b"P_START": "P", b"PP_START": "P"}
    ends = {b"S_END", b"SS_END", b"F_END", b"FF_END", b"P_END", b"PP_END"}
    cur = None
    for l in lumps:
        if l.name in starts:
            cur = starts[l.name]
        elif l.name in ends:
            cur = None
        else:
            l.ns = cur


def is_music(l):
    return l.data[:4] == b"MUS\x1a" or l.data[:4] == b"MThd"


def is_sound(l):
    """DMX sound: DS* (format 3) or DP* (PC speaker, format 0), outside sprite/flat/patch ranges."""
    if l.ns is not None or l.name[:2] not in (b"DS", b"DP"):
        return False
    if is_music(l):
        return False
    if len(l.data) >= 4:
        fmt = struct.unpack_from("<H", l.data, 0)[0]
        return fmt == (3 if l.name[:2] == b"DS" else 0)
    return False


def is_demo(l):
    n = l.name
    return l.ns is None and n.startswith(b"DEMO") and n[4:].isdigit()


def classify(lumps):
    """Return list of category names parallel to lumps."""
    cats = [None] * len(lumps)
    i = 0
    n = len(lumps)
    while i < n:
        l = lumps[i]
        nm = l.name
        # map: marker (ExMy / MAPnn) followed by the standard map lumps
        if not l.data and l.ns is None and (
                (len(nm) == 4 and nm[0:1] == b"E" and nm[2:3] == b"M" and nm[1:2].isdigit() and nm[3:4].isdigit())
                or (len(nm) == 5 and nm.startswith(b"MAP") and nm[3:].isdigit())):
            cats[i] = "maps"
            j = i + 1
            while j < n and lumps[j].name in MAP_LUMPS:
                cats[j] = "maps"
                j += 1
            i = j
            continue
        i += 1
    for i, l in enumerate(lumps):
        if cats[i]:
            continue
        nm = l.name
        if nm in (b"S_START", b"S_END", b"SS_START", b"SS_END") or l.ns == "S":
            cats[i] = "sprites"
        elif nm in (b"F_START", b"F_END", b"FF_START", b"FF_END") or l.ns == "F":
            cats[i] = "flats"
        elif nm in (b"P_START", b"P_END", b"PP_START", b"PP_END") or l.ns == "P" \
                or nm in (b"PNAMES", b"TEXTURE1", b"TEXTURE2"):
            cats[i] = "patches/textures"
        elif is_sound(l):
            cats[i] = "sounds"
        elif nm.startswith(b"D_") and is_music(l):
            cats[i] = "music"
        elif is_music(l) and l.ns is None:
            cats[i] = "music"
        elif nm in (b"GENMIDI", b"DMXGUS", b"DMXGUSC") or nm.startswith(b"DMX"):
            cats[i] = "genmidi/dmx"
        elif is_demo(l):
            cats[i] = "demos"
        else:
            cats[i] = "other"
    return cats


CAT_ORDER = ["maps", "sprites", "flats", "patches/textures", "sounds", "music",
             "genmidi/dmx", "demos", "other"]


def cmd_info(path):
    ident, lumps, size = read_wad(path)
    cats = classify(lumps)
    print("%s: %s, %d lumps, %d bytes" % (path, ident.decode(), len(lumps), size))
    tot = {c: [0, 0] for c in CAT_ORDER}
    for l, c in zip(lumps, cats):
        tot[c][0] += 1
        tot[c][1] += len(l.data)
    print("  %-18s %7s %12s" % ("category", "lumps", "bytes"))
    for c in CAT_ORDER:
        print("  %-18s %7d %12d" % (c, tot[c][0], tot[c][1]))
    mis = sum(1 for l in lumps if l.data and l.offset % 4)
    print("lumps with offset %% 4 != 0: %d (of %d non-empty)" % (mis, sum(1 for l in lumps if l.data)))
    removable = sum(len(l.data) for l, c in zip(lumps, cats) if c in ("sounds", "music", "genmidi/dmx"))
    print("strippable (sound+music+genmidi): %d bytes" % removable)


def strip_lumps(lumps, drop_demos=False, drop_ends=False):
    cats = classify(lumps)
    out = []
    for l, c in zip(lumps, cats):
        if c in ("sounds", "music", "genmidi/dmx"):
            continue
        if drop_demos and c == "demos":
            continue
        if drop_ends and l.name == b"ENDOOM":
            continue
        out.append(l)
    return out


def write_wad(path, ident, lumps, align=4):
    if align < 1 or align & (align - 1):
        die("--align must be a power of two")

    def pad(buf):
        buf.extend(b"\0" * (-len(buf) % align))

    buf = bytearray(b"\0" * 12)
    pad(buf)
    entries = []
    for l in lumps:
        if l.data:
            pad(buf)
            off = len(buf)
            buf += l.data
        else:
            off = len(buf)
        entries.append((off, len(l.data), l.name))
    pad(buf)
    diroff = len(buf)
    for off, sz, nm in entries:
        buf += struct.pack("<ii8s", off, sz, nm)
    struct.pack_into("<4sii", buf, 0, ident, len(lumps), diroff)
    with open(path, "wb") as f:
        f.write(buf)
    return len(buf)


def parse_opts(args, flags, values=()):
    pos, opt = [], {}
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--align":
            i += 1
            if i >= len(args):
                die("--align needs a value")
            opt["align"] = int(args[i], 0)
        elif a in values:
            i += 1
            if i >= len(args):
                die(a + " needs a value")
            opt[a] = args[i]
        elif a in flags:
            opt[a] = True
        elif a.startswith("--"):
            die("unknown option " + a)
        else:
            pos.append(a)
        i += 1
    return pos, opt


def cmd_strip(args):
    pos, o = parse_opts(args, ("--keep-demos", "--drop-demos", "--drop-ends"))
    if len(pos) != 2:
        die("strip <in> <out> [--keep-demos|--drop-demos] [--drop-ends]")
    ident, lumps, size = read_wad(pos[0])
    kept = strip_lumps(lumps, "--drop-demos" in o, "--drop-ends" in o)
    # plain strip keeps the original layout style but compacts; use align=1 (no padding)
    n = write_wad(pos[1], ident, kept, align=1)
    print("strip: %d -> %d lumps, %d -> %d bytes" % (len(lumps), len(kept), size, n))


def cmd_align(args):
    pos, o = parse_opts(args, ())
    if len(pos) != 2:
        die("align <in> <out> [--align N]")
    ident, lumps, size = read_wad(pos[0])
    n = write_wad(pos[1], ident, lumps, o.get("align", 4))
    print("align: %d lumps, %d -> %d bytes" % (len(lumps), size, n))


def cmd_pack(args):
    pos, o = parse_opts(args, ("--keep-demos", "--drop-demos", "--drop-ends"))
    if len(pos) != 2:
        die("pack <in> <out> [--drop-demos] [--drop-ends] [--align N]")
    ident, lumps, size = read_wad(pos[0])
    kept = strip_lumps(lumps, "--drop-demos" in o, "--drop-ends" in o)
    n = write_wad(pos[1], ident, kept, o.get("align", 4))
    report_pack(len(lumps), len(kept), size, n)
    return 0


def report_pack(nlumps, nkept, size, n):
    print("pack: %d -> %d lumps" % (nlumps, nkept))
    print("size: before %d, after %d (saved %d, %.1f%%)" % (size, n, size - n, 100.0 * (size - n) / size))
    fits = n <= WAD_BUDGET
    print("budget: %d - %d = %d bytes: %s" % (STORAGE_BUDGET, ADDIN_BUDGET, WAD_BUDGET,
          "FITS (%d spare)" % (WAD_BUDGET - n) if fits else "DOES NOT FIT (%d over)" % (n - WAD_BUDGET)))


# Shareware doom1.wad v1.9: 4,196,020 bytes, MD5 f0cefca49926d00903cf57551d901abe.
SHAREWARE_SIZE = 4_196_020
SHAREWARE_SHA256 = "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771"
# Debian's doom-wad-shareware 1.9.fixed-5 ships it unmodified "other than decompressing it".
# The snapshot.debian.org URL is addressed by the .deb's SHA-1 and does not expire.
SHAREWARE_DEB_URLS = [
    "https://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed-5_all.deb",
    "https://snapshot.debian.org/file/732562ea15f0a6623611bdb739a03af72d8dbaf1",
    "https://mirrors.kernel.org/debian/pool/non-free/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed-5_all.deb",
]


def download(url):
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            return r.read()
    except urllib.error.URLError as e:
        # python.org builds for macOS have no CA certificates until "Install Certificates.command"
        # is run; curl uses the system store.
        if isinstance(e.reason, ssl.SSLError) and shutil.which("curl"):
            return subprocess.run(["curl", "-fsSL", "--max-time", "120", url],
                                  check=True, capture_output=True).stdout
        raise


def deb_member(deb, prefix):
    """Data of the first member of a .deb (an ar archive) whose name starts with prefix."""
    if deb[:8] != b"!<arch>\n":
        raise ValueError("not a .deb archive")
    pos = 8
    while pos + 60 <= len(deb):
        name = deb[pos:pos + 16].decode("ascii", "replace").strip().rstrip("/")
        size = int(deb[pos + 48:pos + 58])
        if name.startswith(prefix):
            return deb[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    raise ValueError("no %s member in the .deb" % prefix)


def wad_from_deb(deb):
    with tarfile.open(fileobj=io.BytesIO(deb_member(deb, "data.tar")), mode="r:*") as tar:
        for m in tar.getmembers():
            if m.isfile() and m.name.rsplit("/", 1)[-1].lower() == "doom1.wad":
                return tar.extractfile(m).read()
    raise ValueError("doom1.wad is not in the package")


def cmd_fetch_wad(args):
    pos, o = parse_opts(args, ("--drop-demos",), ("--save-original",))
    if len(pos) > 1:
        die("fetch-wad [<out>] [--drop-demos] [--save-original <path>]")
    out = pos[0] if pos else "DOOM1.WAD"
    wad = None
    for url in SHAREWARE_DEB_URLS:
        print("fetch: %s" % url)
        try:
            wad = wad_from_deb(download(url))
        except Exception as e:  # any failure: try the next source
            print("  failed: %s" % e)
            continue
        digest = hashlib.sha256(wad).hexdigest()
        if len(wad) == SHAREWARE_SIZE and digest == SHAREWARE_SHA256:
            break
        print("  wrong file: %d bytes, SHA-256 %s" % (len(wad), digest))
        wad = None
    if wad is None:
        die("could not download doom1.wad; get the v1.9 shareware WAD yourself and use `pack`")
    print("fetch: doom1.wad v1.9 shareware, SHA-256 OK")
    if "--save-original" in o:
        with open(o["--save-original"], "wb") as f:
            f.write(wad)
    ident, lumps, size = parse_wad(wad, "doom1.wad")
    kept = strip_lumps(lumps, "--drop-demos" in o, drop_ends=True)
    if os.path.dirname(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
    n = write_wad(out, ident, kept)
    report_pack(len(lumps), len(kept), size, n)
    print("wrote %s: copy it with CGDoom.g3a to the root of the calculator's storage memory" % out)
    return 0


def cmd_verify(args):
    if len(args) != 2:
        die("verify <a> <b>")
    _, la, _ = read_wad(args[0])
    _, lb, _ = read_wad(args[1])
    bad = 0
    ia = 0
    dropped = []
    for l in lb:
        # advance in a to the next lump with the same name
        j = ia
        while j < len(la) and la[j].name != l.name:
            j += 1
        if j == len(la):
            print("MISSING/ORDER: %s not found in a after a index %d" % (l.sname, ia))
            bad += 1
            continue
        dropped.extend(la[ia:j])
        if la[j].data != l.data:
            print("DIFFERENT: %s (a index %d)" % (l.sname, j))
            bad += 1
        ia = j + 1
    dropped.extend(la[ia:])
    cats = classify(la)
    catmap = {id(x): c for x, c in zip(la, cats)}
    summary = {}
    for d in dropped:
        c = catmap[id(d)]
        s = summary.setdefault(c, [0, 0])
        s[0] += 1
        s[1] += len(d.data)
    print("a: %d lumps, b: %d lumps, dropped from a: %d" % (len(la), len(lb), len(dropped)))
    for c in CAT_ORDER:
        if c in summary:
            print("  dropped %-18s %5d lumps %10d bytes" % (c, summary[c][0], summary[c][1]))
    unexpected = [d.sname for d in dropped if catmap[id(d)] not in ("sounds", "music", "genmidi/dmx", "demos")
                  and d.name != b"ENDOOM"]
    if unexpected:
        print("note: dropped lumps outside sound/music/demo/ENDOOM: %s" % ", ".join(unexpected[:20]))
    print("verify: %s" % ("OK" if not bad else "%d problem(s)" % bad))
    return 1 if bad else 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    c, a = argv[1], argv[2:]
    if c == "info" and len(a) == 1:
        cmd_info(a[0])
    elif c == "strip":
        cmd_strip(a)
    elif c == "align":
        cmd_align(a)
    elif c == "pack":
        return cmd_pack(a)
    elif c == "verify":
        return cmd_verify(a)
    elif c == "fetch-wad":
        return cmd_fetch_wad(a)
    else:
        die("bad command; see --help")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
