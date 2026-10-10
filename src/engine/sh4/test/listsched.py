#!/usr/bin/env python3
"""listsched - reorder a straight-line block of SH4 assembly for the dual-issue model of pipesim.

    listsched.py block.s [--model pess|opt] [--pairing aligned|any] [--odd-start]
                                              ->  prints the reordered block and its cycle count

Design-time helper for the drawer set-up code. Input: one instruction per line (labels are not
allowed inside the block; comments kept). Dependencies: RAW (with the model's latencies), WAR,
WAW, T bit, memory (all stores and loads through r15 stay in order; other loads may move freely
with respect to each other but never across a store), branches (bt/bf/...) are barriers for
stores and for each other, and keep their position relative to the instructions that feed them.
Scheduling: greedy cycle-by-cycle list scheduling, priority = longest latency path to the end of
the block; at most one LS, one EX, one BR per cycle, 2 instructions per cycle; CO alone.
The output is emitted in issue order; pairs are emitted adjacently. With --pairing aligned (the
default since review round 2, matching pipesim's default rule) a pair may only start at an even
instruction position, so the emitted order keeps its pairs when the block starts 4-byte aligned
(or use --odd-start). The set-up blocks written before round 2 used --pairing any.
"""
import re, sys, argparse
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from pipesim import Insn, LAT

def parse(lines):
    out = []
    for ln in lines:
        code = ln.split("/*")[0].strip()
        if not code:
            continue
        m = re.match(r"(\S+)\s*(.*)", code)
        op, args = m.group(1), m.group(2).replace(" ", "")
        args = re.sub(r",", ", ", args)
        ins = Insn(0, op, args.replace(", ", ","))
        ins.src_line = ln.rstrip("\n")
        out.append(ins)
    return out

def barrier_regs(ins):
    """a branch marked '/* barrier: r1 r2 ... */' (a dispatch to another path): every earlier
    instruction stays before it, and no later instruction that writes one of the listed registers
    (the other path's inputs) may be hoisted above it. It is scheduled as early as possible."""
    m = re.search(r"/\*\s*barrier:([^*]*)\*/", getattr(ins, "src_line", ""))
    return set(m.group(1).split()) if m else None

def is_store(i):
    return i.group == "LS" and not i.is_load and i.op not in ("pref",)

def deps(block, lat):
    n = len(block)
    d = [[] for _ in range(n)]          # d[j] = list of (i, latency) : j must issue >= i + latency
    for j in range(n):
        J = block[j]
        for i in range(j):
            I = block[i]
            l = None
            # RAW
            wr = I.writes | I.writes_auto
            for r in wr:
                if r in J.reads_addr:
                    if r in I.writes_auto: l = max(l or 0, lat["auto"])
                    elif I.is_load: l = max(l or 0, lat["ld_addr"])
                    else: l = max(l or 0, lat["ex_addr"])
                if r in J.reads_data:
                    st = J.group == "LS" and not J.is_load
                    if I.is_mul: l = max(l or 0, 3)
                    elif I.is_sts: l = max(l or 0, lat["sts"])
                    elif I.is_load: l = max(l or 0, lat["ld_store"] if st else lat["ld_ex"])
                    else: l = max(l or 0, lat["ex_data"])
            # WAR / WAW: J writes something I reads or writes -> J not before I (same cycle ok
            # only if I is emitted first, which in-order emission guarantees: latency 0)
            jw = J.writes | J.writes_auto
            if jw & (I.reads_data | I.reads_addr):
                l = max(l or 0, 0)
            if jw & wr:
                l = max(l or 0, 1)
            # memory ordering: stack accesses (r15) are ordered among themselves; any other
            # store is ordered with every memory access
            def stack(x): return "r15" in (x.reads_addr | x.writes_auto)
            if is_store(I) or is_store(J):
                if (is_store(I) and not stack(I)) or (is_store(J) and not stack(J)):
                    if I.group == "LS" and J.group == "LS": l = max(l or 0, 1)
                elif stack(I) and stack(J):
                    l = max(l or 0, 1)
            # branches (to the slow path, where only r15 and the stack matter): stores never move
            # across a branch, branches keep their order; everything else may be hoisted above.
            if I.group == "BR" and (J.group == "BR" or is_store(J)):
                l = max(l or 0, 1)
            if J.group == "BR" and is_store(I):
                l = max(l or 0, 1)
            if barrier_regs(J) is not None:
                l = max(l or 0, 0)
            bi = barrier_regs(I)
            if bi is not None and (J.writes | J.writes_auto) & bi:
                l = max(l or 0, 0)
            if l is not None:
                d[j].append((i, l))
    return d

def schedule(block, lat, aligned=True, odd_start=False):
    n = len(block)
    d = deps(block, lat)
    succ = [[] for _ in range(n)]
    for j in range(n):
        for (i, l) in d[j]:
            succ[i].append((j, l))
    prio = [0] * n
    for i in reversed(range(n)):
        prio[i] = max([l + prio[j] for (j, l) in succ[i]] + [1])
    for i in range(n):              # a barrier branch (and what feeds it) goes first
        if barrier_regs(block[i]) is not None:
            boost = max(prio) + 100
            todo = [i]
            while todo:
                k = todo.pop()
                prio[k] += boost
                todo += [a for (a, l) in d[k] if prio[a] < boost]
    t = [None] * n
    done = 0
    cyc = 0
    order = []
    pos = 1 if odd_start else 0      # emitted instructions mod 2 (address bit 1)
    while done < n:
        slots = []
        cap = 1 if (aligned and pos) else 2   # pipesim's default pairing rule: the first of a pair is 4-byte aligned
        cand = [j for j in range(n) if t[j] is None and all(t[i] is not None and t[i] + l <= cyc for (i, l) in d[j])]
        cand.sort(key=lambda j: (-prio[j], j))
        for j in cand:
            if len(slots) == cap: break
            g = block[j].group
            if g == "CO" and slots: continue
            if slots and block[slots[0]].group == "CO": break
            if any(block[s].group == g and g in ("LS", "EX", "BR") for s in slots): continue
            # same-cycle constraint: latency-0 deps must be on an earlier slot in this cycle
            ok = all(t[i] is not None and (t[i] + l <= cyc) for (i, l) in d[j])
            if not ok: continue
            # an instruction cannot pair with one it depends on via RAW (latency >= 1 handles it)
            slots.append(j)
            t[j] = cyc
        for s in slots:
            order.append(s)
        done += len(slots)
        pos = (pos + len(slots)) & 1
        cyc += max([block[s].rate for s in slots] + [1])
    return [block[i] for i in order], cyc

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--model", default="pess")
    ap.add_argument("--pairing", default="aligned", choices=("aligned", "any"),
                    help="aligned (default, as pipesim): only an instruction at a 4-byte aligned "
                    "position can start a pair, so a single-issue cycle costs the next pair")
    ap.add_argument("--odd-start", action="store_true", help="the block starts at an address = 2 mod 4")
    a = ap.parse_args()
    block = parse(open(a.file).readlines())
    out, cyc = schedule(block, LAT[a.model], a.pairing == "aligned", a.odd_start)
    for ins in out:
        print(ins.src_line)
    print("/* listsched: %d instructions, %d cycles (model %s) */" % (len(out), cyc, a.model), file=sys.stderr)

if __name__ == "__main__":
    main()
