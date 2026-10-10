#!/usr/bin/env python3
"""pipesim - static in-order dual-issue model of an SH-4A (SH4AL-DSP) loop body.

    pipesim.py prog.elf START_SYM END_SYM [--iters N] [--model pess|opt|gcc] [--pairing aligned|manual] [-v]
    pipesim.py prog.elf - - --path A:B C:D ...   (one call through taken branches, e.g. a drawer's
                                                  1-pixel path; symbols may be written SYM+OFFSET)

Takes the instructions in [START_SYM, END_SYM) of the ELF (as disassembled by sh-elf-objdump),
treats them as the body of a zero-overhead (DSP repeat) loop, issues N iterations through a model
of the SH-4A pipeline and prints the steady-state cycles per iteration (and, with -v, the issue
cycle of every instruction of the last iteration with the reason for any stall).

The model (not cycle-accurate; documented guesses, see README.md "Pipeline model"):
  * groups, as in the Renesas SH4AL-DSP Software Manual (Rev. 1.50) table 4.2:
      LS  memory accesses, pref, movca, ocb*, mova, ldrs/ldre, lds/sts Rn<->MACH/MACL/PR, ldc Rm,CR1
      EX  ALU, shifts, dt, cmp/*, tst, clrt/sett, xtrct, swap, ext*, mul.l/muls.w/mulu.w/dmul*.l
      MT  mov Rm,Rn, mov #imm,Rn, nop
      BR  branches
      CO  everything else (ldrc/setrc: issue rate 2): issues alone
  * two instructions issue in the same cycle iff they are consecutive, they are not both LS /
    both EX / both BR, neither is CO, the second does not read or write a register written by
    the first, and the pairing rule holds:
      --pairing aligned (default): the first is at a 4-byte aligned address. NOT in the manual
        (which only requires addr and addr+2 in the same 1 KiB page); an empirical assumption
        that explains Azur's "omitting this nop slows the loop to 7 cycles/i".
      --pairing manual: the manual's rule only (same 1 KiB page). --any-pairs: no rule at all.
  * latencies (producer -> consumer):            pess   opt   gcc
        EX/MT result -> EX/MT/store-data          1      1     1
        EX/MT result -> address operand           2      1     1
        load result  -> EX/MT                     2      2     3
        load result  -> address operand           3      2     3
        load result  -> store data                2      1     3
        @Rn+/@-Rn base update -> next address     1      1     1
        mul.l -> sts macl                         3      3     3
        sts macl/mach/pr -> use                   3      1     3
    "gcc" is GCC's sh4a.md (every load 3 cycles, sts from MACL 3): the pessimistic bound.
"""
import argparse, os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", ".."))
BIN = os.path.join(ROOT, "toolchain", "bin")

LAT = {
    "pess": dict(ex_data=1, ex_addr=2, ld_ex=2, ld_addr=3, ld_store=2, auto=1, sts=3),
    "opt":  dict(ex_data=1, ex_addr=1, ld_ex=2, ld_addr=2, ld_store=1, auto=1, sts=1),
    "gcc":  dict(ex_data=1, ex_addr=1, ld_ex=3, ld_addr=3, ld_store=3, auto=1, sts=3),
}

REG = re.compile(r"^r([0-9]|1[0-5])$")

def disasm(elf):
    # -m sh4al-dsp: the repeat-loop set-up is emitted as raw .word opcodes (sh4_asm.h), so the
    # ELF is not necessarily tagged sh4al-dsp; force the DSP opcode table so they decode.
    out = subprocess.run([os.path.join(BIN, "sh-elf-objdump"), "-d", "-z", "-m", "sh4al-dsp", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = {}
    insns = []
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if m:
            syms[m.group(2)] = int(m.group(1), 16)
            continue
        m = re.match(r"^\s+([0-9a-f]+):\s+([0-9a-f]{2} [0-9a-f]{2})\s+(\S+)\s*([^!]*)", line)
        if m:
            insns.append((int(m.group(1), 16), m.group(3), m.group(4).strip()))
    return syms, insns

def regs_in(s):
    return set("r" + x for x in re.findall(r"\br([0-9]|1[0-5])\b", s))

class Insn:
    def __init__(self, addr, op, args):
        self.addr, self.op, self.args = addr, op, args
        self.text = (op + " " + args).strip()
        self.reads_data, self.reads_addr, self.writes, self.writes_auto = set(), set(), set(), set()
        self.is_load = False
        self.is_mul = False
        self.is_sts = False
        self.rate = 1                 # issue rate: cycles until the next instruction may issue
        self.group = "CO"
        self.classify()

    def classify(self):
        op, a = self.op, self.args
        parts = [p.strip() for p in re.split(r",(?![^(]*\))", a)] if a else []
        mem = [p for p in parts if p.startswith("@")]
        base = op.split(".")[0]
        if op in ("nop",):
            self.group = "MT"; return
        if base in ("bf", "bt", "bra", "bsr", "braf", "bsrf", "jmp", "jsr", "rts"):
            self.group = "BR"
            if base in ("bf", "bt"): self.reads_data.add("T")
            if base in ("jmp", "jsr", "braf", "bsrf"): self.reads_data |= regs_in(a)
            return
        if base in ("mov", "movca", "pref", "ocbi", "ocbp", "ocbwb", "movua", "movs") and mem or base == "pref":
            self.group = "LS"
            if len(parts) == 1:              # pref @Rn
                self.reads_addr |= regs_in(parts[0]); return
            src, dst = parts
            if src.startswith("@"):          # load
                self.is_load = True
                self.reads_addr |= regs_in(src)
                self.writes |= regs_in(dst)
                if src.endswith("+"): self.writes_auto |= regs_in(src)
            else:                            # store
                self.reads_data |= regs_in(src)
                self.reads_addr |= regs_in(dst)
                if dst.startswith("@-"): self.writes_auto |= regs_in(dst)
            return
        if base == "mov" and len(parts) == 2 and not mem and not (REG.match(parts[0]) or parts[0].startswith("#")):
            self.group = "LS"; self.is_load = True; self.writes |= regs_in(parts[1]); return   # pc-relative literal
        if base == "mov" and len(parts) == 2 and not mem:
            self.group = "MT"
            self.reads_data |= regs_in(parts[0]) if REG.match(parts[0]) else set()
            self.writes |= regs_in(parts[1])
            return
        if base == "mov" and len(parts) == 2:   # pc-relative literal "mov.l 0x..,rN"
            self.group = "LS"; self.is_load = True; self.writes |= regs_in(parts[1]); return
        if base == "mova":
            self.group = "LS"; self.writes.add("r0"); return
        ex2 = ("add", "addc", "addv", "sub", "subc", "subv", "and", "or", "xor", "shld", "shad",
               "xtrct", "mul", "mulu", "muls", "dmuls", "dmulu", "div1", "div0s", "tst", "cmp")
        ex1 = ("extu", "exts", "swap", "neg", "negc", "not", "movt")
        exs = ("shll", "shlr", "shll2", "shlr2", "shll8", "shlr8", "shll16", "shlr16", "shal", "shar",
               "rotl", "rotr", "rotcl", "rotcr", "dt")
        if base in ("mul", "mulu", "muls", "dmuls", "dmulu"):
            self.group = "EX"; self.is_mul = True
            self.reads_data |= regs_in(a); self.writes.add("MACL")
            if base.startswith("dmul"): self.writes.add("MACH")
            return
        if base in ex2 or base.startswith("cmp"):
            self.group = "EX"
            if len(parts) == 1:              # cmp/pz, cmp/pl Rn
                self.reads_data |= regs_in(parts[0]); self.writes.add("T"); return
            src, dst = parts
            if src.startswith("#"):
                self.reads_data |= regs_in(dst)
            else:
                self.reads_data |= regs_in(src) | regs_in(dst)
            if base in ("tst",) or base.startswith("cmp") or base.startswith("mul") or base.startswith("dmul"):
                self.writes.add("T") if not base.startswith(("mul", "dmul")) else None
            else:
                self.writes |= regs_in(dst)
            if base in ("addc", "subc", "div1", "addv", "subv", "div0s"): self.writes.add("T")
            if base in ("addc", "subc", "div1"): self.reads_data.add("T")
            return
        if base in ex1:
            self.group = "EX"
            if base == "movt":
                self.reads_data.add("T"); self.writes |= regs_in(a); return
            src, dst = parts
            self.reads_data |= regs_in(src); self.writes |= regs_in(dst)
            if base == "negc": self.reads_data.add("T"); self.writes.add("T")
            return
        if base in exs:
            self.group = "EX"
            self.reads_data |= regs_in(a); self.writes |= regs_in(a)
            if base in ("dt", "shll", "shlr", "shal", "shar", "rotl", "rotr", "rotcl", "rotcr"): self.writes.add("T")
            if base in ("rotcl", "rotcr"): self.reads_data.add("T")
            return
        if base in ("clrt", "sett"):
            self.group = "EX"; self.writes.add("T"); return
        if base == "sts" and len(parts) == 2 and parts[0] in ("mach", "macl", "pr"):   # sts SR1, Rn
            self.group = "LS"; self.is_sts = True
            self.reads_data.add(parts[0].upper()); self.writes |= regs_in(parts[1]); return
        if base == "lds" and len(parts) == 2 and parts[1] in ("mach", "macl", "pr"):   # lds Rm, SR1
            self.group = "LS"; self.reads_data |= regs_in(parts[0]); self.writes.add(parts[1].upper()); return
        if base in ("ldrs", "ldre"):
            self.group = "LS"; return
        if base in ("ldrc", "setrc"):
            self.group = "CO"; self.rate = 2; self.reads_data |= regs_in(a); return
        self.group = "CO"

def can_pair_addr(p, ins, pairing):
    if ins.addr != p.addr + 2:
        return pairing == "any"
    if pairing == "aligned":
        return p.addr % 4 == 0
    return pairing == "any" or (p.addr >> 10) == (ins.addr >> 10)

def simulate(body, iters, lat, pairing, taken=2):
    if pairing is True: pairing = "any"
    elif pairing is False: pairing = "aligned"
    ready = {}            # reg -> (cycle when written, kind) ; kind: 'ex' | 'ld' | 'auto'
    def avail(reg, use):
        if reg not in ready: return 0
        c, kind = ready[reg]
        if kind == "auto": return c + lat["auto"]
        if kind == "mul": return c + 3
        if kind == "sts": return c + lat["sts"]
        if kind == "ld":
            return c + (lat["ld_addr"] if use == "addr" else lat["ld_store"] if use == "store" else lat["ld_ex"])
        return c + (lat["ex_addr"] if use == "addr" else lat["ex_data"])
    cycle = 0
    sched = []
    prev = None           # (insn, cycle, slot) of the previously issued instruction
    for it in range(iters):
        if it > 0 and prev is not None and body_branch(body):
            # loop-back through a taken branch: the target cannot issue before branch + taken
            bc = [c for (i2, ins2, c, sl, w) in sched if i2 == it - 1 and ins2.group == "BR"][-1]
            prev = (prev[0], max(prev[1], bc + taken - 1), 1)
        for ins in body:
            need = 0
            why = ""
            # --path: a jump between ranges goes through a taken branch (the last BR among the
            # previous two instructions, i.e. the branch or the branch + its delay slot)
            if prev is not None and ins.addr != prev[0].addr + 2 and it == 0:
                brs = [c for (i2, ins2, c, sl, w) in sched[-2:] if ins2.group == "BR"]
                if brs and brs[-1] + taken > need:
                    need, why = brs[-1] + taken, "taken branch"
            for r in ins.reads_addr:
                t = avail(r, "addr")
                if t > need: need, why = t, "%s(addr)" % r
            for r in ins.reads_data:
                use = "store" if (ins.group == "LS" and not ins.is_load) else "data"
                t = avail(r, use)
                if t > need: need, why = t, "%s(%s)" % (r, use)
            # pairing with previous instruction
            can_pair = False
            if prev is not None and prev[2] == 0:
                p = prev[0]
                aligned = can_pair_addr(p, ins, pairing)
                grp_ok = not (p.group == ins.group and p.group in ("LS", "EX", "BR")) and "CO" not in (p.group, ins.group)
                dep_ok = not ((p.writes | p.writes_auto) & (ins.reads_data | ins.reads_addr | ins.writes | ins.writes_auto))
                can_pair = aligned and grp_ok and dep_ok
            if can_pair and need <= prev[1]:
                c, slot, why = prev[1], 1, ""
            else:
                base = prev[1] + prev[0].rate if prev is not None else 0
                c, slot = max(base, need), 0
                if need <= base:
                    why = ""
            for r in ins.writes:
                ready[r] = (c, "ld" if ins.is_load else "mul" if ins.is_mul else "sts" if ins.is_sts else "ex")
            for r in ins.writes_auto:
                ready[r] = (c, "auto")
            prev = (ins, c, slot)
            sched.append((it, ins, c, slot, why))
    return sched

def body_branch(body):
    return any(i.group == "BR" for i in body[-2:])

def resolve(syms, spec):
    if "+" in spec:
        a, b = spec.split("+", 1)
        return syms[a] + int(b, 0)
    return syms[spec] if spec in syms else int(spec, 0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf"); ap.add_argument("start"); ap.add_argument("end")
    ap.add_argument("--iters", type=int, default=12)
    ap.add_argument("--model", default="pess", choices=sorted(LAT))
    ap.add_argument("--any-pairs", action="store_true", help="same as --pairing any")
    ap.add_argument("--pairing", default="aligned", choices=("aligned", "manual", "any"))
    ap.add_argument("--per", type=float, default=1.0, help="pixels per iteration (for the c/px column)")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("--once", action="store_true", help="straight-line code: simulate the range once (branches not taken)")
    ap.add_argument("--taken", type=int, default=2, help="cycles from a taken loop branch to the target's issue")
    ap.add_argument("--call", nargs=3, metavar=("RS", "END", "RET"), help="per-call model: start=function, loop [RS,END), epilogue [END,RET)")
    ap.add_argument("-k", type=int, nargs="+", default=[0, 1, 4, 44], help="loop iterations for --call")
    ap.add_argument("--path", nargs="+", metavar="A:B", help="per-call model of a path through taken "
                    "branches: the ranges [A,B) are issued once, in order (START/END are ignored); "
                    "each jump between ranges costs a taken branch (--taken)")
    a = ap.parse_args()
    if a.any_pairs: a.pairing = "any"
    syms, insns = disasm(a.elf)
    if a.path:
        seq = []
        for r in a.path:
            x, y = (resolve(syms, t) for t in r.split(":"))
            seq += [Insn(ad, op, args) for (ad, op, args) in insns if x <= ad < y]
        sched = simulate(seq, 1, LAT[a.model], a.pairing, a.taken)
        print("path %s model=%s insns=%d cycles=%d" % (" ".join(a.path), a.model, len(seq),
              sched[-1][2] + 1 + 2))   # +2: rts latency, as --call
        if a.v:
            for (it, ins, c, slot, why) in sched:
                print("  %3d %s %-4s %-30s %s" % (c, "|" if slot else " ", ins.group, ins.text,
                                                 ("stall:" + why) if why else ""))
        return
    s, e = resolve(syms, a.start), resolve(syms, a.end)
    body = [Insn(ad, op, args) for (ad, op, args) in insns if s <= ad < e]
    if not body:
        sys.exit("empty body")
    if a.call:
        # per-call model: [start, rs) once, [rs, end) K times, [end, ret) once (branches not taken)
        rs, re_, ret = (resolve(syms, x) for x in a.call)
        pre = [Insn(ad, op, args) for (ad, op, args) in insns if s <= ad < rs]
        loop = [Insn(ad, op, args) for (ad, op, args) in insns if rs <= ad < re_]
        post = [Insn(ad, op, args) for (ad, op, args) in insns if re_ <= ad < ret]
        for k in a.k:
            seq = pre + loop * k + post
            sched = simulate(seq, 1, LAT[a.model], a.pairing, a.taken)
            print("call k=%d model=%s cycles=%d" % (k, a.model, sched[-1][2] + 1 + 2))   # +2: rts latency
        return
    if a.once:
        sched = simulate(body, 1, LAT[a.model], a.pairing, a.taken)
        total = sched[-1][2] + 1
        print("%-28s model=%s insns=%d cycles=%d (straight line)" % (a.start, a.model, len(body), total))
        if a.v:
            for (it, ins, c, slot, why) in sched:
                print("  %3d %s %-4s %-30s %s" % (c, "|" if slot else " ", ins.group, ins.text,
                                                 ("stall:" + why) if why else ""))
        return
    sched = simulate(body, a.iters, LAT[a.model], a.pairing, a.taken)
    n = len(body)
    first = [c for (it, ins, c, slot, why) in sched if it == a.iters - 2][0]
    last = [c for (it, ins, c, slot, why) in sched if it == a.iters - 1][0]
    per_iter = last - first
    print("%-28s model=%s insns=%d cycles/iter=%d cycles/px=%.2f" %
          (a.start, a.model, n, per_iter, per_iter / a.per))
    if a.v:
        base = last
        for (it, ins, c, slot, why) in sched:
            if it != a.iters - 1: continue
            print("  %3d %s %-4s %-30s %s" % (c - base, "|" if slot else " ", ins.group, ins.text,
                                             ("stall:" + why) if why else ""))

if __name__ == "__main__":
    main()
