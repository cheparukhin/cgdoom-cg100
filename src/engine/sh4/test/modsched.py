#!/usr/bin/env python3
"""modsched - exact modulo-schedule search for the SH-4A drawer loops (design tool, not in the build).

    modsched.py KIND [II] [LAT] [--px P] [--max-ii N] [--node-limit N] [--stage1 OPS] [--war1] [-q]
    modsched.py KIND [LAT] --shipped [--war1] [-q]

      KIND   span | spanlow | col | collow
      II     try this initiation interval only (default: search upwards from max(ResMII, RecMII)
             and report the minimum feasible II, with a proof that every smaller II is infeasible)
      LAT    pess | opt | gcc | max: the latency tables of pipesim.py; "max" is the elementwise
             maximum of the three, so a schedule found under it runs without a stall under all three
             (default: pess)
      --px P pixels (span, col) or texels (spanlow, collow) per loop pass (default 2, the shipped
             loops). Pixel p feeds pixel p+1 at distance 0 and the last pixel feeds the first of the
             next pass at distance 1 (collow: each texel of the pass has its own frac, as shipped).
      --stage1 OPS   pin a two-stage split (OPS in stage 1, the rest in stage 0), e.g. the shipped
             one, so that only the loop body changes and the prologue/epilogue can stay. Every
             alignment of the stage boundary is tried (stage s = [s*II + off, (s+1)*II + off) for
             each off that keeps op 0 in its stage), so "infeasible" is a proof for the split
      --shipped      check the loop of ../r_draw_sh4.S (table SHIPPED) against the model instead
      --war1         give anti-dependences latency 1 (a stricter model that forbids a WAR pair in one cycle)

Model. Every op of one loop pass gets an issue time t; the loop body is the ops sorted by t mod II
("row"), the stage is t div II. A constraint (src, dst, lat, dist) means
    t[dst] + II*dist >= t[src] + lat.
lat >= 1 is a true dependence (RAW, or the @-Rn base update between two stores). lat == 0 is an
anti-dependence (WAR): dst overwrites a register that src reads. Both may then issue in the same cycle
provided src is the FIRST of the pair (pipesim.py: the second instruction of a pair must not read or
write a register written by the first, nothing forbids writing one the first reads). The hand-written
R_DrawSpan loop relies on this twice per pass (h_A'|f_B' and d_A|a_B); with --war1 (latency 1) its
r0 cycle f -> g -> h -> f' costs 2+3+1 per pixel and the model wrongly says II >= 12.
Resources per row: at most 2 instructions, at most 1 EX and at most 1 LS (MT pairs with anything).
A row with one instruction is padded with a nop, so every pair starts 4-byte aligned (pipesim.py's
default pairing rule) when the loop head is aligned.

Search (exact). The constraints form a simple temporal network: the longest-path closure D (Floyd-
Warshall over lat - II*dist) gives, relative to op 0 (fixed at t = 0, w.l.o.g.), the exact window
[D[0][j], -D[j][0]] of every op; a positive cycle means II < RecMII. The search assigns ops in
most-constrained-first order (fewest resource-feasible issue times in the window). After every
assignment it
  * narrows all windows through D (path consistency: a partial assignment that respects the narrowed
    windows extends to a full solution of the temporal constraints);
  * moves window ends to resource-feasible times and propagates that through D, to a fixpoint;
  * checks a per-class slot count and a bipartite matching of the unassigned ops to the free issue
    slots of their window rows (a necessary condition for the modulo reservation table);
  * checks the same-cycle order of anti-dependent pairs.
The enumeration is complete, so "infeasible" is a proof for the model, provided the constraint graph
is strongly connected (every window finite; all four kernels are, for P = 1..6; the script says so
otherwise). Feasibility is shortcut by unrolling: m passes of a P/m-pixel schedule at II/m are a
P-pixel schedule at II (kept only if it meets the P-pixel model). Every schedule printed is
re-checked by verify(). The 2-pixel kernels solve in milliseconds; all four kernels for P = 1..4
under every table take about 16 s in total, the slowest being collow gcc --px 4 (II 18 and 19
infeasible, ~9 s). --node-limit caps a search.

What the model does not see (check the result with pipesim.py on the real code): register
allocation beyond the listed dependences, T-bit/literal-pool effects, and the set-up/epilogue.
"""
import argparse, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pipesim import LAT

LAT = dict(LAT)
LAT["max"] = {k: max(LAT[m][k] for m in ("pess", "opt", "gcc")) for k in LAT["pess"]}
NEG = -10 ** 9


# ------------------------------------------------------------------------------------------ models
def span_model(P, L, low=False):
    """R_DrawSpan(Low), one pass = P pixels (texels). Ops of a pixel (registers of the shipped loop):
       a mov Y,rm (r8->r1)   b shld -26,rm   c mov X,rn (r7->r2)   d xtrct rm,rn   e shld -10,rn
       f mov rn,r0   g texel mov.b @(r0,src),r0   h colour mov.b @(r0,cm),rC   i store mov.b rC,@-rD
       (i2 second store, low detail)   j X -= XS   k Y -= YS"""
    ops, cons = [], []
    px = [chr(65 + i) for i in range(P)]
    for p in px:
        ops += [(p+"a", "MT"), (p+"b", "EX"), (p+"c", "MT"), (p+"d", "EX"), (p+"e", "EX"), (p+"f", "MT"),
                (p+"g", "LS"), (p+"h", "LS"), (p+"i", "LS"), (p+"j", "EX"), (p+"k", "EX")]
        if low: ops.append((p+"i2", "LS"))
        cons += [(p+"a", p+"b", L["ex_data"], 0), (p+"b", p+"d", L["ex_data"], 0), (p+"c", p+"d", L["ex_data"], 0),
                 (p+"d", p+"e", L["ex_data"], 0), (p+"e", p+"f", L["ex_data"], 0),
                 (p+"f", p+"g", L["ex_addr"], 0), (p+"g", p+"h", L["ld_addr"], 0), (p+"h", p+"i", L["ld_store"], 0),
                 (p+"a", p+"k", 0, 0),          # WAR Y: k overwrites what a reads
                 (p+"c", p+"j", 0, 0)]          # WAR X
        if low: cons += [(p+"h", p+"i2", L["ld_store"], 0), (p+"i", p+"i2", L["auto"], 0)]
    last = "i2" if low else "i"
    for n in range(P):
        x, y, dist = px[n], px[(n + 1) % P], (1 if n == P - 1 else 0)
        cons += [(x+"k", y+"a", L["ex_data"], dist),   # RAW Y
                 (x+"j", y+"c", L["ex_data"], dist),   # RAW X
                 (x+"d", y+"a", 0, dist),              # WAR rm
                 (x+"f", y+"c", 0, dist),              # WAR rn
                 (x+"h", y+"f", 0, dist),              # WAR r0
                 (x+last, y+"h", 0, dist),             # WAR colour register
                 (x+last, y+"i", L["auto"], dist)]     # @-rD base update
    return ops, cons


def col_model(P, L, low=False):
    """R_DrawColumn(Low), one pass = P pixels (texels). Ops of a pixel:
       m mov frac,rI   s shld -25,rI   a add src,rI   l texel mov.b @rI,r0   c colour mov.b @(r0,cm),rC
       st store   (st2 second store, low detail)   dd dest += 320 (322)   ff frac += step
       Full detail: one frac stepped per pixel, stores mov.b rC,@rD. Low detail: one frac per texel of
       the pass (stepped by P*step), stores mov.b rC,@-rD (rD = dest+2) then dd."""
    ops, cons = [], []
    px = [chr(65 + i) for i in range(P)]
    for p in px:
        ops += [(p+"m", "MT"), (p+"s", "EX"), (p+"a", "EX"), (p+"l", "LS"), (p+"c", "LS"), (p+"st", "LS"),
                (p+"dd", "EX"), (p+"ff", "EX")]
        if low: ops.append((p+"st2", "LS"))
        cons += [(p+"m", p+"s", L["ex_data"], 0), (p+"s", p+"a", L["ex_data"], 0), (p+"a", p+"l", L["ex_addr"], 0),
                 (p+"l", p+"c", L["ld_addr"], 0), (p+"c", p+"st", L["ld_store"], 0), (p+"dd", p+"st", L["ex_addr"], 0),
                 (p+"m", p+"ff", 0, 0),          # WAR frac
                 (p+"l", p+"m", 0, 1)]           # WAR rI: each pixel keeps its own index register
        if low: cons += [(p+"c", p+"st2", L["ld_store"], 0), (p+"st", p+"st2", L["auto"], 0),
                         (p+"ff", p+"m", L["ex_data"], 1)]   # low: every texel has its own frac
    last = "st2" if low else "st"
    for n in range(P):
        x, y, dist = px[n], px[(n + 1) % P], (1 if n == P - 1 else 0)
        if not low: cons += [(x+"ff", y+"m", L["ex_data"], dist)]   # RAW frac (shared, stepped per pixel)
        cons += [(x+last, y+"dd", L["auto"] if low else 0, dist),  # dest: @-rD update (low) / WAR (full)
                 (x+"c", y+"l", 0, dist),              # WAR r0
                 (x+last, y+"c", 0, dist)]             # WAR colour register
    return ops, cons


# The loops in ../r_draw_sh4.S as issue times (pixel A's first op at 0; stage-1 ops at row + II).
SHIPPED = {
    "col": (8, dict(Am=0, As=1, Aa=2, Aff=3, Bm=4, Bs=5, Al=6, Ba=6, Bff=7, Add=8, Ac=9, Bl=10, Ast=11,
                    Bdd=12, Bc=13, Bst=15)),
    "collow": (9, dict(Am=0, As=1, Aa=2, Bm=3, Al=4, Bs=4, Ba=5, Bff=6, Ac=7, Add=7, Bl=8, Aff=8, Ast=9,
                       Ast2=10, Bc=11, Bdd=12, Bst=14, Bst2=15)),
    "span": (11, dict(Aa=0, Ab=2, Ac=2, Ak=3, Aj=4, Ad=5, Ba=5, Ae=6, Af=7, Bb=7, Bc=8, Bk=8, Ag=9, Bj=9,
                      Bd=10, Be=11, Ah=12, Bf=12, Bg=14, Ai=15, Bh=17, Bi=21)),
    "spanlow": (12, dict(Aa=0, Ab=2, Ac=2, Ad=3, Ae=4, Ak=5, Af=6, Aj=7, Ba=7, Ag=8, Bb=8, Bk=9, Bc=10,
                         Bj=10, Bd=11, Be=12, Ah=13, Bf=13, Bg=15, Ai=16, Ai2=17, Bh=18, Bi=21, Bi2=23)),
}

KINDS = {"span": (span_model, False), "spanlow": (span_model, True),
         "col": (col_model, False), "collow": (col_model, True)}


# ------------------------------------------------------------------------------------------ search
def closure(N, idx, cons, II):
    """Longest-path closure of the difference constraints; None if there is a positive cycle."""
    D = [[NEG] * N for _ in range(N)]
    for i in range(N): D[i][i] = 0
    for s, d, lat, dist in cons:
        w = lat - II * dist
        if w > D[idx[s]][idx[d]]: D[idx[s]][idx[d]] = w
    for k in range(N):
        Dk = D[k]
        for i in range(N):
            dik = D[i][k]
            if dik == NEG: continue
            Di = D[i]
            for j in range(N):
                v = Dk[j]
                if v != NEG and dik + v > Di[j]: Di[j] = dik + v
    if any(D[i][i] > 0 for i in range(N)): return None
    return D


def res_mii(ops):
    n = len(ops)
    return max(-(-n // 2), sum(g == "EX" for _, g in ops), sum(g == "LS" for _, g in ops))


def rec_mii(ops, cons, hi=200):
    idx = {o[0]: i for i, o in enumerate(ops)}
    return next(II for II in range(1, hi) if closure(len(ops), idx, cons, II) is not None)


class Result:
    def __init__(self, status, sol=None, nodes=0, exact=True):
        self.status, self.sol, self.nodes, self.exact = status, sol, nodes, exact


def solve(ops, cons, II, node_limit=None, stage=None, off=0):
    names = [o[0] for o in ops]; grp = [o[1] for o in ops]
    N = len(names); idx = {n: i for i, n in enumerate(names)}
    D = closure(N, idx, cons, II)
    if D is None: return Result("recmii")
    exact = all(D[0][j] != NEG and D[j][0] != NEG for j in range(N))
    tmax = sum(abs(l) + II * abs(d) for _, _, l, d in cons) + II
    lo = [D[0][j] if D[0][j] != NEG else -tmax for j in range(N)]
    hi = [-D[j][0] if D[j][0] != NEG else tmax for j in range(N)]
    if stage:                                   # pinned stages: t in [s*II+off, (s+1)*II+off) (op 0 at t = 0)
        for n, st in stage.items():
            j = idx[n]
            lo[j], hi[j] = max(lo[j], st * II + off), min(hi[j], st * II + II - 1 + off)
        if not lo[0] <= 0 <= hi[0]: return Result("infeasible", exact=exact)
    zero = {}                                   # (i, j) -> dists of lat-0 edges i -> j
    for s, d, lat, dist in cons:
        if lat == 0: zero.setdefault((idx[s], idx[d]), []).append(dist)
    rows = [[] for _ in range(II)]
    t = [None] * N
    nodes = [0]

    def must_first(i, j):                       # i must be slot 0 if i, j share the actual cycle
        return any(t[j] + II * k == t[i] for k in zero.get((i, j), ()))

    def fits(n, tt):
        r = rows[tt % II]
        if len(r) >= 2: return False
        if r:
            m = r[0]
            if grp[n] != "MT" and grp[m] == grp[n]: return False
            t[n] = tt
            bad = must_first(n, m) and must_first(m, n)
            t[n] = None
            if bad: return False
        return True

    def row_ok(n, tt):                          # resource-only test (no order), for forward checking
        r = rows[tt % II]
        return len(r) < 2 and not (r and grp[n] != "MT" and grp[r[0]] == grp[n])

    def count_ok(unassigned):
        free = sum(2 - len(r) for r in rows)
        if len(unassigned) > free: return False
        for g in ("EX", "LS"):
            need = sum(1 for j in unassigned if grp[j] == g)
            if need and need > sum(1 for r in rows if len(r) < 2 and all(grp[m] != g for m in r)):
                return False
        return True

    def domain(j, lo, hi):
        if hi[j] - lo[j] + 1 >= II:
            return [c for c in range(II) if row_ok(j, c)]   # rows; any is reachable
        return [tt for tt in range(lo[j], hi[j] + 1) if row_ok(j, tt)]

    def tighten(lo, hi, left):
        """Bounds consistency: move every window end to a resource-feasible issue time (rows only
        fill up as the search goes deeper) and push the change through D, to a fixpoint."""
        todo = list(left)
        while todo:
            j = todo.pop()
            a, b = lo[j], hi[j]
            while a <= b and not row_ok(j, a): a += 1
            if a > b: return False
            while not row_ok(j, b): b -= 1
            if a == lo[j] and b == hi[j]: continue
            lo[j], hi[j] = a, b
            Dj = D[j]
            for k in left:
                ch = False
                if Dj[k] != NEG and a + Dj[k] > lo[k]: lo[k] = a + Dj[k]; ch = True
                if D[k][j] != NEG and b - D[k][j] < hi[k]: hi[k] = b - D[k][j]; ch = True
                if ch:
                    if lo[k] > hi[k]: return False
                    if k not in todo: todo.append(k)
        return True

    ACCEPT = {"E": ("EX", "MT"), "L": ("LS", "MT"), "*": ("EX", "LS", "MT")}

    def match_ok(lo, hi, left):
        """Necessary condition: the unassigned ops can be matched to the free issue slots of their
        window rows (rows are filled independently of time; exact for the reservation table).
        A free row offers an E slot (EX/MT) and an L slot (LS/MT); a row holding one op offers an L
        slot after an EX, an E slot after an LS, and a slot for anything after an MT."""
        slots = []
        for c in range(II):
            r = rows[c]
            if not r: slots.append((c, "E")); slots.append((c, "L"))
            elif len(r) == 1: slots.append((c, {"EX": "L", "LS": "E", "MT": "*"}[grp[r[0]]]))
        if len(slots) < len(left): return False
        bycol = {}
        for k, (c, ty) in enumerate(slots): bycol.setdefault(c, []).append(k)
        adj = []
        for j in left:
            cs = range(II) if hi[j] - lo[j] + 1 >= II else {tt % II for tt in range(lo[j], hi[j] + 1)}
            adj.append([k for c in cs for k in bycol.get(c, ()) if grp[j] in ACCEPT[slots[k][1]]])
        owner = [-1] * len(slots)
        def aug(u, seen):
            for k in adj[u]:
                if k in seen: continue
                seen.add(k)
                if owner[k] < 0 or aug(owner[k], seen):
                    owner[k] = u; return True
            return False
        order = sorted(range(len(left)), key=lambda u: len(adj[u]))
        return all(aug(u, set()) for u in order)

    def rec(lo, hi, left):
        if not left:
            return {names[i]: t[i] for i in range(N)}
        nodes[0] += 1
        if node_limit and nodes[0] > node_limit: raise TimeoutError
        best, bsize = None, None
        for j in left:
            sz = len(domain(j, lo, hi))
            if sz == 0: return None
            if best is None or (sz, hi[j] - lo[j]) < bsize: best, bsize = j, (sz, hi[j] - lo[j])
        n = best
        rest = [j for j in left if j != n]
        for tt in range(lo[n], hi[n] + 1):
            if not fits(n, tt): continue
            nlo, nhi = lo[:], hi[:]
            nlo[n] = nhi[n] = tt
            Dn = D[n]; ok = True
            for j in rest:
                if Dn[j] != NEG and tt + Dn[j] > nlo[j]: nlo[j] = tt + Dn[j]
                if D[j][n] != NEG and tt - D[j][n] < nhi[j]: nhi[j] = tt - D[j][n]
                if nlo[j] > nhi[j]: ok = False; break
            if not ok: continue
            t[n] = tt; rows[tt % II].append(n)
            if count_ok(rest) and tighten(nlo, nhi, rest) and match_ok(nlo, nhi, rest):
                r = rec(nlo, nhi, rest)
                if r: return r
            rows[tt % II].remove(n); t[n] = None
        return None

    t[0] = 0; rows[0].append(0)
    lo[0] = hi[0] = 0
    for j in range(1, N):
        if lo[j] > hi[j]: return Result("infeasible", nodes=0, exact=exact)
    try:
        sol = rec(lo, hi, list(range(1, N)))
    except TimeoutError:
        return Result("limit", nodes=nodes[0], exact=exact)
    return Result("found" if sol else "infeasible", sol, nodes[0], exact)


def solve_staged(ops, cons, II, node_limit, stage):
    """solve() with a pinned stage split, at every alignment of the stage boundary that keeps op 0
    (t = 0) inside its stage. (Anchoring the boundary at op 0 alone would miss schedules with a
    stage-1 op before row 0, e.g. spanlow max --stage1 <shipped>,Bk.)"""
    s0, nodes, exact = stage[ops[0][0]], 0, True
    for k in range(II):
        off = -s0 * II - k
        r = solve(ops, cons, II, node_limit, stage, off)
        nodes += r.nodes; exact = exact and r.exact
        if r.status != "infeasible":
            r.nodes, r.off = nodes, off
            return r
    return Result("infeasible", nodes=nodes, exact=exact)


def verify(sol, ops, cons, II):
    """Independent check of a schedule against the model; returns a list of violations."""
    grp = dict(ops); err = []
    for s, d, lat, dist in cons:
        if sol[d] + II * dist < sol[s] + lat:
            err.append("%s -> %s: t=%d + %d*%d < t=%d + %d" % (s, d, sol[d], II, dist, sol[s], lat))
    rows = {}
    for n, tt in sol.items(): rows.setdefault(tt % II, []).append(n)
    for c, r in rows.items():
        if len(r) > 2: err.append("row %d: %d ops" % (c, len(r)))
        for g in ("EX", "LS"):
            if sum(grp[n] == g for n in r) > 1: err.append("row %d: two %s" % (c, g))
        if len(r) == 2:
            a, b = r
            first = [x for x, y in ((a, b), (b, a)) for s, d, lat, dist in cons
                     if (s, d) == (x, y) and lat == 0 and sol[d] + II * dist == sol[s]]
            if len(set(first)) > 1: err.append("row %d: %s and %s must both issue first" % (c, a, b))
    return err


# ------------------------------------------------------------------------------------------ output
def body(sol, ops, cons, II):
    """Rows in issue order: [(row, [(name, group, t), ...])] with anti-dependent pairs ordered."""
    grp = dict(ops)
    rows = {}
    for n, tt in sol.items(): rows.setdefault(tt % II, []).append(n)
    out = []
    for c in range(II):
        r = sorted(rows.get(c, []), key=lambda n: sol[n])
        if len(r) == 2:
            a, b = r
            if any(s == b and d == a and lat == 0 and sol[a] + II * dist == sol[b] for s, d, lat, dist in cons):
                r = [b, a]
        out.append((c, [(n, grp[n], sol[n]) for n in r]))
    return out


def show(sol, ops, cons, II, base=None):
    """The loop body in issue order: rows from base mod II (base = start of stage 0: the earliest op,
    or the --stage1 window), so that the listed order and the stage numbers agree."""
    if base is None: base = min(sol.values())
    rows = dict(body(sol, ops, cons, II))
    for i in range(II):
        r = rows[(base + i) % II]
        cells = ["%-4s %-2s st%d" % (n, g, (tt - base) // II) for n, g, tt in r]
        cells += ["nop  MT"] * (2 - len(cells))
        print("  c%-2d  %-18s | %s" % (i, cells[0], cells[1]))


def main():
    ap = argparse.ArgumentParser(description="exact modulo scheduler for the SH-4A drawer loops")
    ap.add_argument("kind", choices=sorted(KINDS))
    ap.add_argument("rest", nargs="*", metavar="II|LAT", help="II (an integer) and/or a latency model %s"
                    % "|".join(sorted(LAT)))
    ap.add_argument("--px", type=int, default=2, help="pixels (texels for the low kinds) per loop pass")
    ap.add_argument("--max-ii", type=int, default=None, help="give up above this II (default: ResMII + 8)")
    ap.add_argument("--node-limit", type=int, default=None, help="search nodes per II before giving up")
    ap.add_argument("--war1", action="store_true", help="give anti-dependences latency 1 (a stricter model "
                    "that forbids a WAR pair in one cycle: it puts R_DrawSpan at II=12)")
    ap.add_argument("--stage1", metavar="OPS", help="two-stage loop with these ops (comma-separated, e.g. "
                    "Be,Bf,Bg) in stage 1 and all others in stage 0 (every alignment of the stage "
                    "boundary is tried): keeps the shipped prologue/epilogue split")
    ap.add_argument("--shipped", action="store_true", help="check the loop of ../r_draw_sh4.S (SHIPPED, "
                    "2 pixels/texels per pass) against the model instead of searching")
    ap.add_argument("-q", action="store_true", help="do not print the schedule")
    a = ap.parse_intermixed_args()
    a.II, a.lat = None, "pess"
    for x in a.rest:
        if x.isdigit(): a.II = int(x)
        elif x in LAT: a.lat = x
        else: ap.error("not an II or a latency model: %s" % x)
    builder, low = KINDS[a.kind]
    L = LAT[a.lat]
    ops, cons = builder(a.px, L, low)
    stage = None
    if a.stage1:
        s1 = set(a.stage1.split(","))
        bad = s1 - {o[0] for o in ops}
        if bad: ap.error("unknown ops: %s" % " ".join(sorted(bad)))
        stage = {o[0]: (1 if o[0] in s1 else 0) for o in ops}
    if a.war1: cons = [(s, d, max(lat, 1), dist) for s, d, lat, dist in cons]
    pix = a.px * (2 if low else 1)
    if a.shipped:
        if a.px != 2: ap.error("--shipped: the shipped loops have 2 pixels/texels per pass")
        II, sol = SHIPPED[a.kind]
        bad = verify(sol, ops, cons, II)
        print("%s shipped loop, II=%d (%.3f cycles/px), latencies %s%s: %s" % (a.kind, II, II / pix, a.lat,
              " --war1" if a.war1 else "", "meets the model" if not bad else "VIOLATES the model:"))
        for b in bad: print("  " + b)
        if not a.q: show(sol, ops, cons, II)
        sys.exit(1 if bad else 0)
    rm, cm = res_mii(ops), rec_mii(ops, cons)
    ng = {g: sum(x == g for _, x in ops) for g in ("EX", "LS", "MT")}
    print("%s, %d %s/pass (%d px), latencies %s: %d ops (%d EX, %d LS, %d MT), ResMII %d, RecMII %d"
          % (a.kind, a.px, "texels" if low else "pixels", pix, a.lat, len(ops), ng["EX"], ng["LS"], ng["MT"], rm, cm))
    tries = [a.II] if a.II else range(1, (a.max_ii or rm + 8) + 1)
    exact = True
    for II in tries:
        if II < rm and not a.II:
            continue
        if II < cm:
            print("  II=%-3d infeasible: positive dependence cycle (RecMII %d)" % (II, cm))
            if a.II: sys.exit(1)
            continue
        if II < rm:
            print("  II=%-3d infeasible: resources (ResMII %d)" % (II, rm)); sys.exit(1)
        t0 = time.time()
        r, how = None, ""
        if not stage and not a.war1:
            # Unrolling: m passes of a P0-pixel schedule at II0 give a P0*m-pixel schedule at m*II0
            # with the same rows; it is valid if it meets the P-pixel model's constraints (the same
            # global constraints, or weaker ones as for collow's per-texel fracs), so try divisors first.
            for P0 in range(1, a.px):
                m = a.px // P0
                if a.px % P0 or II % m: continue
                o0, c0 = builder(P0, L, low)
                r0 = solve(o0, c0, II // m, a.node_limit)
                if r0.status == "found":     # (kept only if it passes the P-pixel model: verify)
                    sol = {}
                    for n, tt in r0.sol.items():
                        for k in range(m):
                            sol[chr(ord(n[0]) + k * P0) + n[1:]] = tt + k * (II // m)
                    if verify(sol, ops, cons, II): continue
                    r, how = Result("found", sol, r0.nodes), ", %d x the %d-%s schedule at II=%d" % (
                        m, P0, "texel" if low else "pixel", II // m)
                    break
        if r is None:
            r = solve_staged(ops, cons, II, a.node_limit, stage) if stage else solve(ops, cons, II, a.node_limit)
        dt = time.time() - t0
        if r.status == "found":
            bad = verify(r.sol, ops, cons, II)
            if bad: sys.exit("internal error, schedule violates the model:\n  " + "\n  ".join(bad))
            if stage: bad = [n for n, st in stage.items() if (r.sol[n] - r.off) // II != st]
            if bad: sys.exit("internal error, ops outside their --stage1 stage: " + " ".join(bad))
            print("  II=%-3d schedule found (%d nodes, %.2fs%s): %.3f cycles/px" % (II, r.nodes, dt, how, II / pix))
            if not a.II and rm < II:
                print("  => minimum II = %d; II %d..%d proved infeasible%s%s"
                      % (II, rm, II - 1, " with this --stage1 split" if stage else "",
                         "" if exact else " (NOT exact: constraint graph not strongly connected)"))
            if not a.q: show(r.sol, ops, cons, II, getattr(r, "off", None))
            return
        if r.status == "limit":
            print("  II=%-3d undecided: node limit reached (%d nodes, %.2fs)" % (II, r.nodes, dt)); sys.exit(2)
        exact = exact and r.exact
        print("  II=%-3d infeasible: exhaustive search (%d nodes, %.2fs)%s"
              % (II, r.nodes, dt, "" if r.exact else " [NOT exact: windows bounded heuristically]"))
        if a.II: sys.exit(1)
    print("  no schedule up to II=%d" % tries[-1]); sys.exit(1)


if __name__ == "__main__":
    main()
