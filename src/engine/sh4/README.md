# src/engine/sh4 — SH4AL-DSP assembly for CGDoom (fx-CG100)

Hand-scheduled column/span drawers, palette expansion for the presenter, a cache-line fill and a
vanilla-exact `FixedDiv` helper. Every routine is **bit-exact** with its C reference and writes no
memory the C reference does not write. The randomized test (`make -C src/engine/sh4/test run`) runs
63,966 cases in the main build (plus 6,507 and 600 in the `.text` and forced-P1 builds) with 0
mismatches, 0 stray writes to static memory, 0 stack-canary hits and 0 callee-saved-register
violations, after checking that the `.S` files also assemble without `-Wa,--dsp` (raw-opcode form, §8).

| file | contents |
|---|---|
| `cgd_sh4.h` | Public prototypes, `cgd_sh4_set_colormaps()`, inline `cgd_pref()`, `cgd_udiv64_32()` and `cgd_fixed_div()` |
| `r_draw_sh4.S` | The 8 drawers. The hot 4 go to `.ilram`, the rare 4 (fuzz, translated) to `.text` |
| `r_draw_sh4_c.c` | Rotated-colormap registration, the C fallback paths the asm tail-calls, and `cgd_expand_build_pal32()` |
| `expand_sh4.S` | `cgd_expand_p8_rgb565[_p32]`, `cgd_fill32`. **Generated** by `test/gen_expand.py`: edit the generator, not this file |
| `sh4_asm.h` | Section macros (`.ilram` vs `.text`), `ALIGN4`, `FUNC`, and `LDRS`/`LDRE`/`LDRC` (repeat-loop set-up as raw opcodes) |
| `test/` | Randomized bit-exactness test (`test_sh4.c`, `ref_draw.c`, `abi_call.S`), benchmarks, and the design tools: `pipesim.py` (pipeline model), `listsched.py` (set-up scheduler), `modsched.py` (exact modulo-schedule search), `gen_expand.py` |

Build flags: the target flags, `sh-elf-gcc -m4-nofpu -mb -c file.S`, with or without `-Wa,--dsp`.
The `.S` files need the C preprocessor, so assemble them with gcc, not raw `as`. **Never build these
for the host.** The host keeps the C drawers.
- gas rejects the `ldrs`/`ldre`/`ldrc` mnemonics unless the DSP ISA is enabled ("opcode not valid for
  this cpu variant"). Both game builds assemble the readable mnemonics with the DSP ISA:
  - CMake / fxSDK (`fxsdk build-cg`): the toolchain file passes `-m4-nofpu -mb -Wa,--dsp`, and
    `CMakeLists.txt` defines `CGD_SH4_DSP_MNEMONICS` for the `.S` files;
  - the sim game (`src/sim/cgdoom.mk`, `make sim` / `test-sim` / `test-sim-full` / `perf-sim`):
    `$(CGSIM_CFLAGS) -Wa,--dsp -DCGD_SH4_DSP_MNEMONICS -MMD -MP`, plus `-DCGD_SH4_NO_ILRAM` unless
    `CGSIM_ILRAM=1`. `-MMD -MP` makes a change to `sh4_asm.h` rebuild the sim objects.
- Without `CGD_SH4_DSP_MNEMONICS`, `sh4_asm.h` emits the three instructions as raw opcodes
  (`LDRS(label)`, `LDRE(label)`, `LDRC(regno)`), so the files also assemble without
  `-Wa,--dsp` (`test/Makefile`'s drawer builds use this raw-opcode form).
- `make -C src/engine/sh4/test asmcheck` (part of `run`) assembles both files in the raw-opcode form
  without `-Wa,--dsp` and checks that the section bytes and relocations equal those of the mnemonic
  form (`-DCGD_SH4_DSP_MNEMONICS -Wa,--dsp`, the game builds), in both section layouts. This also
  range-checks the displacements, which gas checks only for the mnemonics.
- Objects built without DSP instructions are not tagged `sh4al-dsp`. `test/pipesim.py`
  disassembles with `-m sh4al-dsp`; do the same when reading the objects with `objdump`.
- `CGD_SH4` (engine C code: use the `*_SH4` drawers and register the rotated colormaps) is defined by
  `src/engine/r_draw.h` itself under `__sh__` unless `-DCGD_NO_SH4` is given (`CGSIM_SH4=0`, the C-only
  sim reference). No build passes it on the command line.

---

## 1. Functions

All drawers are `void f(void)`. They read the vanilla globals exactly like `r_draw.c` does:
`dc_colormap dc_x dc_yl dc_yh dc_iscale dc_texturemid dc_source dc_translation ds_y ds_x1 ds_x2
ds_colormap ds_xfrac ds_yfrac ds_xstep ds_ystep ds_source ylookup[] columnofs[] centery viewheight
colormaps fuzzpos`. The types match CGDoom: `byte *ylookup[]`, `int columnofs[]`, `fixed_t == int`,
`lighttable_t == byte`.

| asm | replaces | semantics / side effects |
|---|---|---|
| `R_DrawColumn_SH4` | `R_DrawColumn` | `dc_colormap[dc_source[(frac>>16)&127]]`, frac = `texturemid + (yl-centery)*iscale` (32-bit wrap) |
| `R_DrawFuzzColumn_SH4` | `R_DrawFuzzColumn` | Writes `dc_yl=1` / `dc_yh=viewheight-2` like C. `colormaps[6*256+dest[fuzzoffset[fuzzpos]]]`, `fuzzpos` advanced (mod 50) exactly like C |
| `R_DrawTranslatedColumn_SH4` | `R_DrawTranslatedColumn` | `dc_colormap[dc_translation[dc_source[frac>>16]]]`. Arithmetic shift, **no** `&127`, as in vanilla |
| `R_DrawSpan_SH4` | `R_DrawSpan` | `spot = ((yfrac>>10)&4032) + ((xfrac>>16)&63)`, the exact vanilla math (not Chocolate's packed position). `ds_x2 < ds_x1` returns; the C loop would run 2^32 times |
| `R_DrawColumnLow_SH4` | `R_DrawColumnLow` | low detail, see below |
| `R_DrawSpanLow_SH4` | `R_DrawSpanLow` | low detail, see below |
| `R_DrawFuzzColumnLow_SH4` | `R_DrawFuzzColumnLow` (Chocolate) | low detail, see below |
| `R_DrawTranslatedColumnLow_SH4` | `R_DrawTranslatedColumnLow` (Chocolate) | low detail, see below |

**Low detail (`detailshift = 1`).** These follow Chocolate Doom, which matches the DOS executable:
- **Columns.** `x = dc_x << 1` is a local, so `dc_x` is **not** modified. Every texel goes to `columnofs[x]` and `columnofs[x+1]`.
  - linuxdoom-1.10's C `R_DrawColumnLow` does `dc_x <<= 1` on the global. That breaks sprites with several posts, because `R_DrawVisSprite` uses `dc_x` as its loop variable.
- **Spans.** `count = ds_x2 - ds_x1` is computed **before** doubling. `ds_x1 <<= 1; ds_x2 <<= 1` are applied to the globals, as both vanilla and Chocolate do. The span draws 2·(count+1) pixels from `columnofs[ds_x1]`.
  - linuxdoom computes the count after doubling and overdraws by 2·(x2−x1)+… pixels.
- The C versions in `test/ref_draw.c` define these semantics. `r_draw.c` has the same code, so the host build matches (§7.5).

Palette expansion and helpers:

```c
void cgd_expand_p8_rgb565(uint16_t *dst, uint8_t const *src, uint16_t const *pal, int n);
void cgd_expand_build_pal32(uint32_t *pal32, uint16_t const *pal);           /* C, 1 KiB table */
void cgd_expand_p8_rgb565_p32(uint16_t *dst, uint8_t const *src, uint32_t const *pal32, int n);
void cgd_fill32(void *dst, uint32_t value, size_t size);
static inline uint32_t cgd_udiv64_32(uint32_t hi, uint32_t lo, uint32_t d);  /* needs hi < d */
static inline int32_t  cgd_fixed_div(int32_t a, int32_t b);                  /* vanilla FixedDiv */
static inline void     cgd_pref(const void *p);
```

**`cgd_expand_p8_rgb565(dst, src, pal, n)` computes `dst[i] = pal[src[i]]`.** It follows the contract in `src/gint/cgd_gint.h`:
- `n % 16 == 0`.
- `dst` is 32-byte aligned and is one of:
  - **P1 cached SDRAM** (`dst>>29 == 4`). Each 32-byte line is allocated with `movca.l` (no line fill), written whole, then purged with `ocbp`. The data is in RAM when the function returns, so the caller only needs a `synco` before it starts a DMA (`present_dma()` in `src/gint/video.c` does exactly that, with no `ocbwb` loop).
  - **anything else**, e.g. on-chip X/Y RAM at `0xE5007000`/`0xE5017000`. Only plain stores are used: no `movca.l`/`ocb*`. The presenter does not use such destinations; the routines support them.
- `src` may have any alignment. It is read exactly once and never beyond `src+n`.
- It uses no repeat loops, no DSP registers and no MACH/MACL, so it is **safe in interrupt context**.
- The plain entry builds the 1 KiB table on its own stack when `n >= 256`. That costs about **870 modelled cycles** (≈940 under the "gcc" latencies; 64 iterations of a 13-cycle loop, two palette entries of each half per iteration) and **1052 bytes of stack** at its peak. For `n < 256` it uses a simple loop (8–10 c/px; the table pays off from about 120–170 px, so the 256 threshold is conservative).
- The `_p32` entry uses a caller-maintained table. It costs 12 bytes of stack and has no per-call table cost: use it (§7.6).
  - Whole line loop (16 px, `ocbp`/`movca`/`pref`/`dt`/`bf` included): 45 cycles = **2.81 c/px** (pess and opt; 53 = 3.3 c/px under "gcc").
  - Plain entry, same loop plus the table: ≈2.83 c/px at n = 64,000 (the presenter's one call per frame) and ≈3.09 at n = 3,200. The table build only matters for calls of a few thousand pixels or fewer; at one 64,000-px call per frame `_p32` saves just the ≈870 cycles of one table build and the 1 KiB of stack.
- `_p32` returns without writing if `n` is not a positive multiple of 16.

**`cgd_fill32(dst, v, size)`.** `dst` must be 32-byte aligned and `size % 32 == 0`. P1 destinations use `movca.l` and the lines stay cached. Other destinations use plain stores.

**`cgd_fixed_div` matches vanilla `FixedDiv`, guard included.** `b == INT_MIN` saturates as vanilla does. The one difference: for `a == INT_MIN` with `|b| < 65536` or `b == -65536`, vanilla calls `I_Error`, while this returns INT_MIN/INT_MAX by the sign of the true quotient. `b == 65536` returns INT_MIN, which is the exact result (vanilla returns it too).
- A second guard, `(|a| >> 15) >= |b|`, does this. It can only fire for `a == INT_MIN` (for any other `a` the vanilla guard already implies `(|a| >> 15) < |b|`), and it keeps the unsigned quotient below 2^31. A `>> 16` guard would let `32768 < |b| < 65536` through with a wrong-signed result; the test's boundary divisors (§8) catch that.
- `cgd_udiv64_32` is `div0u` followed by 32×(`rotcl`, `div1`), about 66 EX cycles.
- The steps are spelled out rather than written with `.rept`. GCC sizes inline asm by counting lines, and an underestimate produces "pcrel too far" errors.

## 2. Key techniques

- **Rotated colormap** (sign-extension trick, exact).
  - `mov.b` sign-extends. So the drawers index `rot + (map - colormaps) + 128` with the signed texel, where `rot[k*256+j] = colormaps[k*256 + (j^0x80)]`.
  - This saves the `extu.b` and, more importantly, keeps the colormap lookup off a second `r0`-indexed load chain (see §3).
  - One 8,704-byte copy of the whole COLORMAP lump is built once by `cgd_sh4_set_colormaps()`. There is no per-call or per-light-level copy.
  - Per call, the drawer checks `swap.b(cm - lump) < nmaps`. That is equivalent to "a 256-byte map inside the registered lump" and costs 3 LS + 4 EX. If the check fails (unregistered table, foreign or misaligned colormap), the drawer tail-calls its C fallback, so output is still exact.
  - 1-pixel columns (below) skip the check: for one texel the `extu.b` is cheaper than the check, so they index `dc_colormap` directly and need no registered lump.
- **COLEXTRABITS.** `frac9 = frac<<9` and `step9 = iscale<<9`, so `(frac>>16)&127 == frac9>>>25` in one `shld` with no mask. This is exact modulo 2^32.
- **Exact span index in 3 EX + 2 MT.**
  - `X = xfrac<<10`, `Y = yfrac<<10`.
  - `rm = Y>>>26`, `rn = X`, `xtrct rm,rn` gives `(ytexel<<16)|(X>>>16)`.
  - `rn >>>= 10` gives `ytexel*64 + xtexel`, because the fraction garbage falls out.
  - Vanilla C needs 5 EX for this, plus 2 adds.
- **Right-to-left spans with pre-decrement stores.** The span starts at `x + count*xstep` and `y + count*ystep`, found by `mul.l` (exact modulo 2^32), and steps backwards. Each store is a `mov.b rC,@-rD`, so there is no pointer increment.
- **Low-detail columns.** Two `mov.b rC,@-rD` stores from `dest+2`, then `rD += 322`. This adds no EX op per pixel compared with full detail. It needs `columnofs[x+1] == columnofs[x]+1`, which `R_InitBuffer` always gives; the drawer checks it and falls back to C otherwise.
- **DSP zero-overhead repeat loops** (`ldrs`/`ldre`/`ldrc`). There is no `dt`/`bf` in the hot loops. The loop count is limited to RC (12 bits): counts above 7936 go to C, and they never occur at 320×200.
- **2-stage software pipelining.** Each iteration does two pixels: stage 0 of pair *k*, stage 1 of pair *k−1*.
  - A prologue runs stage 0 of pair 0, and an epilogue runs stage 1 of the last pair.
  - For an odd pixel count, the last pair's second pixel is computed but not stored.
    - Its texel read stays inside `dc_source[0..127]` (columns) or the 4 KiB flat (spans).
    - Nothing is ever written outside the C version's pixels. The test checks this for all static memory and the expansion's stack (§8).
  - Columns of 2 pixels (low detail: 2 texels) run prologue plus epilogue only, with one taken branch from the set-up to the epilogue (a delayed conditional branch, `bt/s` or `bf/s`).
- **1-pixel columns.** `R_DrawColumn_SH4` and `R_DrawColumnLow_SH4` load `dc_yl`/`dc_yh` (and `dc_iscale`, `centery`) first and branch on `dc_yl == dc_yh` before saving anything. The 1-pixel path is straight-line code in r0–r7 (nothing saved, no colormap check, no repeat loop): `frac = dc_texturemid + (dc_yl − centery)·dc_iscale`, texel, `extu.b`, colour from `dc_colormap`, one store (low detail: two stores, to `columnofs[x]` and `columnofs[x+1]`, so no contiguity check either). In sprite-heavy scenes half of all column calls are 1 pixel (§3).
- **Translated drawers share one body** that stores every texel through r4 and r8, both stepped by 320. In low detail r8 is the second column. In full detail r8 = r4, so the second store rewrites the same byte with the same value, and no other memory is touched.
- **Set-up code** is ordered by `test/listsched.py`, a list scheduler using the same pipeline model.
  - The frac→`mul.l`→prologue chain goes first.
  - The checks branch to C (after popping saved registers) before any store.
  - The column and low-column set-up (with the 1-pixel dispatch) is ordered under the 4-byte pairing rule (`--pairing aligned`, the default), with the dispatch branch marked `/* barrier: r1 r2 ... */`: such a branch is scheduled as early as possible, keeps everything before it, and lets nothing that writes the listed registers move above it. The 1-pixel blocks are hand-scheduled.
  - The span set-up blocks were ordered with `--pairing any` and a group model that treats `mul.l`, `sts` and `ldrs`/`ldre` as CO, so they are not optimal under the model above; re-ordering them may save up to about 4 cycles per call.

## 3. Pipeline model and loop schedules

The model (`test/pipesim.py`) is a documented guess. No drawer has been timed on hardware yet, and the GDB
simulator has no SH-4A timing.
- **Groups**, from the Renesas *SH4AL-DSP Software Manual* (Rev. 1.50) table 4.2:
  - LS: memory, `pref`, `movca`, `ocb*`, `mova`, `ldrs`/`ldre`, `lds`/`sts` between Rn and MACH/MACL/PR.
  - EX: ALU, shifts, `dt`, `cmp/*`, `tst`, `clrt`/`sett`, `xtrct`, `swap`, `ext*`, `mul.l` (2 execution cycles).
  - MT: `mov Rm,Rn`, `mov #imm,Rn`, `nop`.
  - BR: branches.
  - CO: `ldrc`/`setrc` (issue rate 2) and the other system instructions. These issue alone.
  - GCC's `sh.md` puts `cmp`/`tst` in MT and `mov #imm` in EX, but that table is for the SH-4, not the SH-4A; the SH4AL-DSP manual is followed here.
- **Pairing:** two consecutive instructions can issue together if they are not both LS/EX/BR, neither is CO, there is no RAW/WAW dependency between them, and the pairing rule holds.
  - The manual's only address rule is that `addr` and `addr+2` are in the same 1 KiB page (`--pairing manual`).
  - The default model also requires the first instruction to be 4-byte aligned. **This rule is not in the manual.** It is an empirical assumption that explains Azur's "omitting this nop slows the loop to 7 cycles/i". The loops are written for it, and every loop below has the same speed under both rules (fuzz is 1 cycle faster under `manual`, per-call set-up 3–8 cycles faster).
- **Latencies:**

| producer → consumer | pess | opt | gcc |
|---|---|---|---|
| EX/MT → EX/MT/store data | 1 | 1 | 1 |
| EX/MT → address operand | 2 | 1 | 1 |
| load → EX | 2 | 2 | 3 |
| load → address | 3 | 2 | 3 |
| load → store data | 2 | 1 | 3 |
| `mul.l` → `sts macl` | 3 | 3 | 3 |
| `sts macl` → use | 3 | 1 | 3 |

"pess" matches the stalls Lephe documented in gint's `image_p4_normal.S` and the 2-cycle load-use delay noted in CGDoom's `cgdoom-asm.s` (his `.line` loop uses a load two cycles later with no lost cycle). "opt" is faster than both. "gcc" is GCC's `sh4a.md`, which gives every load a latency of 3. It is **slower than both** and is the pessimistic bound for the hardware A/B in §9. All loops below are scheduled to run at the same speed under "pess" and "opt". Every loop has a 4-byte-aligned head, an even body length, and one LS+EX or LS/EX+MT pair per cycle.

Under "gcc" the column loops lose their slack. The hand-tuned schedules assume a 2-cycle load-use delay:

| loop | pess / opt | gcc |
|---|---|---|
| R_DrawColumn | 8 cycles per 2 px (4.0 c/px) | 10 (5.0 c/px) |
| R_DrawColumnLow | 9 per 4 px (2.25) | 10 (2.5) |
| R_DrawSpan | 11 per 2 px (5.5) | 11 (5.5) |
| R_DrawSpanLow | 12 per 4 px (3.0) | 12 (3.0) |
| expansion core (16 px, no line overhead) | 40 (2.5) | 48 (3.0) |
| expansion line incl. `ocbp`/`movca`/`pref`/`dt`/`bf` | 45 (2.81) | 53 (3.31) |

`modsched.py` (an exact search, §8) bounds what a re-schedule could gain there. The spans already run at their floor under all three models. Low-detail columns: under "gcc" (and "max", the elementwise maximum of the three tables) the minimum is II=10, proved, so the shipped loop (9 / 9 / 10) is optimal under each model separately *for its register allocation* (one colour register, r10). (With a second colour register, i.e. one more saved register and an adapted epilogue, the model gives II=9 under "gcc" with the shipped two-stage split, and II=9 under "max" with a three-stage pipeline; worth it only if hardware shows 3-cycle loads, §9.) Full-detail columns: `modsched.py col max` finds II=8 (4.0 c/px under all three models), but only with a three-stage pipeline (a new prologue and epilogue); with the shipped two-stage split the best under "max" is 9 (`--stage1 Add,Ac,Bl,Ast,Bdd,Bc,Bst`). If the hardware turns out to behave like "gcc", re-derive the column loop from `col max`. `modsched.py KIND LAT --shipped` checks the shipped loops against the model: the column loops violate it under "gcc" exactly at the colour-load → store edges (2 cycles where "gcc" wants 3), which are the stalls `pipesim.py` reports.

**Why not the 7-instruction column loop of an earlier prototype?** Both indexed loads go through `r0`: `mov rI,r0` → texel `@(r0,src)` → colour `@(r0,cm)`. That chain occupies `r0` for 5 cycles per pixel under "pess". The model gives it **8 cycles/px** as written, and at best 5 even when software-pipelined. The texel address is therefore computed with an `add` into a separate register, so `r0` only carries the texel (3 cycles per pixel).

### R_DrawColumn — 16 instructions per 2 pixels, **8 cycles (4.0 c/px)**

Registers in the loop:

| register | holds |
|---|---|
| r0 | texel |
| r1 | dest pointer (+320 before each store) |
| r2 | step9 |
| r3 | −25 |
| r4 | frac9 |
| r5 | `dc_source` |
| r6 | rotated colormap base |
| r7 | 320 |
| r8 / r9 | texel addresses of pixels a / b |
| r10 | colour |

Callee-saved r8–r10 are pushed and popped.

```
c0: mov r4,r8        | add r7,r1          a(k) frac copy      | dest -> a(k-1)
c1: mov.b @(r0,r6),r10 | shld r3,r8        colour a(k-1)       | a(k) index
c2: mov.b @r9,r0     | add r5,r8          texel b(k-1)        | a(k) address
c3: mov.b r10,@r1    | add r2,r4          store a(k-1)        | frac += step
c4: mov r4,r9        | add r7,r1          b(k) frac copy      | dest -> b(k-1)
c5: mov.b @(r0,r6),r10 | shld r3,r9        colour b(k-1)       | b(k) index
c6: mov.b @r8,r0     | add r5,r9          texel a(k)          | b(k) address
c7: mov.b r10,@r1    | add r2,r4          store b(k-1)        | frac += step
```

Per pixel this is 3 LS, 4 EX and 1 MT, which is EX-bound at 4 cycles. Each load-to-address distance is ≥3 and each load-to-store distance is 2.

### R_DrawColumnLow — 18 instructions per 2 texels (4 pixels), **9 cycles (2.25 c/px)**

The texels have separate frac registers, r4 and r11, each stepped by 2·step9. Each texel does 2×`mov.b rC,@-rD` and then `rD += 322`. The schedule was found with `test/modsched.py`.

### R_DrawSpan — 22 instructions per 2 pixels, **11 cycles (5.5 c/px)**

Ops per pixel: `a` mov Y→rm, `b` shld −26, `c` mov X→rn, `d` xtrct, `e` shld −10, `f` mov rn→r0, `g` texel, `h` colour, `i` store, `j` X−=XS, `k` Y−=YS. That is 5 EX, 3 MT and 3 LS. The `'` marks stage 1, which belongs to the previous pair.

```
n0: a_A  | e_B'     n1: h_A' | f_B'    n2: c_A | b_A    n3: g_B' | k_A    n4: i_A' | j_A
n5: d_A  | a_B      n6: h_B' | e_A     n7: f_A | b_B    n8: c_B  | k_B    n9: g_A  | j_B
n10: i_B' | d_B
```

`r0` is busy 5 cycles per pixel (f → h). Two write-after-read pairs share a cycle (n1, n5), with the reader first.

11 instructions per pixel in 2 issue slots per cycle make 5.5 c/px the floor for this instruction count, and the loop reaches it under "pess", "opt" and "gcc" (`pipesim.py`, and `modsched.py span LAT --shipped`).
- **Same-cycle write-after-read pairs.** II = 11 depends on them. The SH-4A can issue an op that overwrites a register in the same cycle as an op that reads it, if the reader goes first: the pairing rule only forbids the second instruction reading or writing a register the *first writes*. The loop does that twice per pass, at n1 (`h_A'` reads `r0`, `f_B'` overwrites it) and n5 (`d_A` reads `r1`, `a_B` overwrites it). `modsched.py` gives such anti-dependences latency 0, so the `r0` cycle f → g → h → f′ costs 5 cycles per pixel, RecMII = 10, and the model finds II = 11 under every latency table. With latency 1 (`--war1`, a stricter model that forbids these pairs) the cycle costs 2 + 3 + 1 = 6 ("pess") and RecMII = 12; `modsched.py span --shipped --war1` lists exactly those two pairs as its violations.
- A better register allocation cannot go below 11 cycles: the bound is the 22 instructions, not a dependence. Fewer instructions would need a 3-operand shift or an `r0`-free indexed load, which the SH-4 does not have (the texel load must be `@(r0,Rn)` and the colour load needs the texel in `r0`, so the `mov rn,r0` stays).

### R_DrawSpanLow — 24 instructions per 2 texels, **12 cycles (3.0 c/px)**

8 LS + 10 EX + 6 MT fill all 24 slots, so 12 is the resource floor. The schedule was found with `modsched.py spanlow max --stage1 Be,Bf,Bg,Ah,Bh,Ai,Ai2,Bi,Bi2`: under the worst latency of each edge across "pess", "opt" and "gcc", with R_DrawSpan's stage split, which is the split the SpanLow prologue and epilogue implement. `pipesim.py` gives 12 cycles under all three models and both pairing rules.

```
c0: a_A  | e_B'     c1: h_A' | f_B'    c2: b_A | c_A     c3: d_A | g_B'    c4: e_A | i_A'    c5: k_A | i2_A'
c6: h_B' | f_A      c7: j_A  | a_B     c8: g_A | b_B     c9: k_B | i_B'    c10: c_B | j_B    c11: d_B | i2_B'
```

Every colour load is 3 cycles ahead of its first store (the "gcc" store-data latency) and every texel load 3 or more ahead of its colour load. c1 and c6 read `r0` and overwrite it in the same cycle. Modelled per call: 591 cycles for 85 texels.

### Expansion `_p32` — 80 instructions per 16 pixels, **40 cycles (2.5 c/px)**, LS-bound

The schedule is in `gen_expand.py`: 5 LS + 3 EX + 2 MT per 2 pixels, and `xtrct` merges two entries of the duplicated table. Per 16-pixel line the overhead is `ocbp`/`movca`/`pref`/`add`/`dt`/`bf`, and the modelled whole line loop is 45 cycles, **2.81 c/px** (53 cycles, 3.31 c/px under "gcc"). The plain entry adds the stack table build, about 870 cycles per call (§1).

### Rare drawers

| loop | pess | opt | gcc |
|---|---|---|---|
| translated | 16 c/px | 12 c/px | 16 c/px |
| fuzz | 10 c/px | 7 c/px | 11 c/px |

These are simple loops in `.text`. Fuzz rows must stay strictly ordered, because `dest[-320]` must see the row just written.

### Per-call model

These figures come from `pipesim.py --call` (set-up + K iterations + epilogue + `rts`), or `--path` for the calls that skip the loop through taken branches, on the test ELF, in the "pess" model, with "opt" / "gcc" where they differ. The C figures are the same model applied to the GCC -O2 build of `test/ref_draw.c`.

| routine | cycles per call (pess) | opt / gcc |
|---|---|---|
| R_DrawColumn | **32** (n=1), 57 (n=2), 68 (3–4), 84 (7–8), 412 (n≈89), 724 (n≈168); 52 + 4n for even n ≥ 4 | 30, 53, 48 + 4n / 34, 61, 53 + 5n |
| C R_DrawColumn (GCC -O2) | 47 + **14n** (61 at n=1) | 40 + 11n / 50 + 15n |
| R_DrawColumnLow | **33** (1 texel), 63 (2), 76 (3–4), 463 (89 texels = 178 px) | 31, 62, 75, 462 / 37, 70, 84, 514 |
| C R_DrawColumnLow | 45 + **17** per texel | 39 + 14 / 49 + 18 |
| R_DrawSpan | 75 (n=1–2), 153 (n≈16), 1011 (n≈171), 1825 (n=320) | 72, 1007 / 79, 1015 |
| C R_DrawSpan | 39 + **22n** | 34 + 19n / 40 + 23n |
| R_DrawSpanLow | 87 (1–2 texels), 591 (85 texels = 170 px) | 83, 587 / 93, 597 |
| C R_DrawSpanLow | 38 + **37** per texel | 35 + 31 / 40 + 42 |
| expand `_p32` / plain (64,000 px, one frame) | 2.81 / ≈2.83 c/px | 2.81 / 3.31, ≈3.33 (gcc) |
| asm `n < 256` byte loop (expand) | 10 c/px | 8 / 11 |

The paths that skip the loop (1-pixel columns, n = 2) go through taken branches, which `--call` does not model (it assumes fall-through), so their figures come from `pipesim.py --path`, which charges each taken branch. The 1-pixel dispatch (`cmp/eq` + not-taken `bt`) is absorbed by the set-up schedule: it adds nothing to the multi-pixel paths in the model. Through the general path a 1-pixel column would cost about as much as n = 2 (57 cycles under "pess" instead of 32).

**The speed-up depends on the column-length mix.** Columns cost about 52 cycles of set-up per call (32 in all for a 1-pixel column), so short columns are dominated by it. The table below weights the per-call model by column-length histograms of the host build's `R_DrawColumn` calls (full detail, `freedoom1.wad`, the `tests/scenarios.txt` scenarios):

| scene | calls | mean px | calls ≤ 8 px | asm c/px (pess) | C c/px (pess / opt) | speed-up pess / opt / gcc |
|---|---|---|---|---|---|---|
| e1m1 | 580k | 86.7 | 19% | 4.60 | 14.5 / 11.5 | **3.2× / 2.5× / 2.8×** |
| e1m3 | 459k | 104.2 | 10% | 4.51 | 14.5 / 11.4 | 3.2× / 2.5× / 2.8× |
| e3m4 | 416k | 78.7 | 17% | 4.66 | 14.6 / 11.5 | 3.1× / 2.5× / 2.8× |

- Wall-heavy scenes run at about 4.5–4.7 c/px, 2.5–3.2× faster than C.
- Sprite-heavy scenes are dominated by set-up: most calls are 1–2 px posts from `R_DrawMaskedColumn`, where the
  1-pixel path saves about 25 cycles per call against the general path. The register-argument entry of §9 targets
  the remaining set-up. The per-frame call mix and modelled cycles of each scenario, from the current simulator
  profiles, are in [`tests/perf-baseline.md`](../../../tests/perf-baseline.md) ("Modelled hardware cycles of the
  drawers").
- The sprite statistics come from Freedoom sprites; the shareware `doom1.wad` may have longer posts.

Spans and the other drawers on realistic lengths:

| routine | asm c/px | C c/px (pess / opt) | speed-up |
|---|---|---|---|
| spans (len 20..320, mean ≈170) | 5.9 | 22.2 / 19.2 | **3.3–3.8×** |
| low-detail columns (89 texels) | 2.6 | 8.8 / 7.2 | 2.7–3.3× |
| low-detail spans (85 texels) | 3.5 | 18.7 / 15.7 | **≈4.5–5.4×** |

With the measured span-length histograms of the same scenes (means 50–125 px) the asm runs at 6.0–6.8 c/px, 3.3–3.7× faster than C under "pess" and 2.9–3.2× under "opt".

The asm is faster than C at every column length: a 1-pixel column costs 32 cycles against 61 under "pess", 30 against 51 under "opt" and 34 against 65 under "gcc". A 2-pixel column costs 57 against 75. Cache misses are not modelled. Texture and flat fetches and framebuffer line fills add the same cost to C and asm.

## 4. Measured (GDB SH simulator, sim64 counters)

Measured with `make -C src/engine/sh4/test bench`. These are **simulated instructions per output pixel, call overhead included**. The simulator's "cycles" are SH-1-style (1 per instruction + 2 per taken branch). Neither number captures dual issue: use §3 for timing.

| kernel | C -O2 insts/px | asm insts/px | ratio |
|---|---|---|---|
| R_DrawColumn, len 10..168 | 14.37 | **8.82** | 1.63× |
| R_DrawColumn, len 1 / 2 / 3 / 8 / 32 / 168 | 46.0 / 30.0 / 24.7 / 18.0 / 15.0 / 14.2 | **42.0** / 40.0 / 33.0 / 16.5 / 10.1 / 8.4 | |
| R_DrawColumnLow, len 1 / 2 / 3 / 8 / 32 / 168 (per output pixel) | 27.0 / 17.5 / 14.3 / 10.4 / 8.6 / 8.1 | 23.5 / 23.3 / 18.7 / 9.4 / 5.7 / 4.7 | |
| R_DrawColumnLow, len 10..168 | 8.21 | **4.94** | 1.66× |
| R_DrawTranslatedColumn | 16.39 | 11.75 | 1.39× |
| R_DrawFuzzColumn | 21.38 | 6.01 | 3.56× |
| R_DrawSpan, len 20..320 | 22.20 | **11.48** | 1.93× |
| R_DrawSpan, len 1 / 4 / 16 / 64 / 320 | 57.0 / 30.8 / 24.2 / 22.6 / 22.1 | 96.0 / 30.5 / 15.9 / 12.2 / 11.2 | |
| R_DrawSpanLow, len 20..320 | 15.77 | **6.56** | 2.40× |
| expand, 64,000 px (plain / p32) | 8.00 (byte loop) | 5.34 / 5.31 | |

The asm executes more instructions per call: set-up is about 70–90 instructions (≈42 in all for a 1-pixel column), against 32–40 for C. It executes far fewer per pixel, and those dual-issue. The instruction ratio therefore understates the real gain: compare 8 asm instructions in 4 cycles per pixel with 14 C instructions in 11–14 cycles.

## 5. Sizes and sections

| section | bytes | contents |
|---|---|---|
| `.ilram` | **1352** | R_DrawColumn 328, R_DrawColumnLow 364, R_DrawSpan 316, R_DrawSpanLow 344 (literal pools included) |
| `.text` | 504 + 1016 | translated + fuzz (both variants); `expand_sh4.o` (`_p32` 768, plain entry 164, fill32 82) |
| `.rodata` | 500 | the 5×50 fuzz offset table |
| `.data` / `.bss` | 0 | none |

- gint's linker script collects exactly `*(.ilram)` into the 4 KiB IL memory at `0xE5200000`.
  - gint itself uses about 180 B, so the drawers use about 33% of it.
  - gint copies it at start-up and again after every world switch. That is fine for code.
- `-DCGD_SH4_NO_ILRAM` puts the hot drawers in `.text` instead, for A/B timing on hardware.
- `-DCGD_SH4_EXPAND_ILRAM` also moves the expansion into ILRAM, for a total of 2.3 KiB. This is not recommended unless measurements show I-cache misses in the presenter.
- The sim harness `src/sim/sim.ld` maps `.ilram` into plain RAM, so sim builds work either way.

## 6. Constraints

1. **Repeat loops: foreground only.**
   - The 8 drawers use `ldrs`/`ldre`/`ldrc`. gint saves SR, which holds RC, on interrupts, but **not RS/RE**.
   - A drawer is safe as long as **no code that can run in interrupt context executes `ldrs`/`ldre`**. Known offenders:
     - gint's USB FIFO read path (`usb_pipe_read4`, `src/usb/read4.S`), reachable from the BRDY interrupt while an asynchronous USB **read** is pending. Writes and logging are fine.
     - fxlibc `memchr()` and anything built on it.
     - gint/Azur image rendering (`dimage`, …).
   - Do not call any of these from DMA or timer callbacks. The expansion routines contain no repeat loops.
   - Also required: `SR.DSP = 1`, which gint sets at start-up.
2. **Alignment.**
   - All accesses are natural-size and aligned: byte texture, colormap and framebuffer accesses; 4-byte globals, `ylookup`/`columnofs`, literal pools and stack. Colormaps, textures, flats and the source frame may have any alignment.
   - The expansion needs a 4-byte-aligned `pal32` (or a 2-byte-aligned `pal` for the plain entry) and a 32-byte-aligned `dst`.
3. **Preconditions** are the same as for the C code.
   - `dc_source` has 128 readable bytes. `ds_source` has 4096 bytes. The translated drawer reads `dc_source[frac>>16]` unmasked, exactly like C.
   - For an odd pixel count, the drawers also read the texel of the phantom last pixel, which lies within those bounds.
   - Counts above 7936, or above 199 for fuzz, go to the C path.
   - `fuzzpos` must be in [0,50); otherwise the C path is used.
4. **Stack.**
   - Drawers: up to 20 bytes (r8–r12). The 1-pixel column paths use none (r0–r7 only).
   - `_p32`: 12 bytes.
   - The plain expansion entry: 1052 bytes at its peak (1 KiB table, `pr`, 6 saved registers) when `n >= 256`.
5. **`cgd_sh4_set_colormaps()` must be called.** Without it every drawer takes its C fallback, which is slightly slower than plain C because of the extra check. (1-pixel columns are the exception: they never need the registered lump.) Call it again if the COLORMAP data moves or changes; vanilla never changes it.

## 7. Integration (r_draw.c / r_main.c / r_data.c / gint layer)

How the routines are wired into the engine and the gint layer.

1. **Build.**
   - `r_draw_sh4.S`, `expand_sh4.S` and `r_draw_sh4_c.c` are built for the **calculator** (CMake globs
     them) and the **sim** (`src/sim/cgdoom.mk`) only, never for the host; flags at the top of this file.
   - `CGD_SH4` is defined by `src/engine/r_draw.h` under `__sh__` (and not `CGD_NO_SH4`); the engine files
     that need the prototypes `#include "sh4/cgd_sh4.h"` under it.
2. **`r_data.c`, `R_InitColormaps`**, after `colormaps = …`, registers one 32-byte-aligned `PU_STATIC`
   rotated copy of the whole lump:
   ```c
   #ifdef CGD_SH4
     { int lump = W_GetNumForName("COLORMAP"), n = W_LumpLength(lump) / 256;   /* 34 */
       uint8_t *rot = Z_Malloc(n * 256 + 31, PU_STATIC, 0);
       rot = (uint8_t *)(((uintptr_t)rot + 31) & ~31);
       cgd_sh4_set_colormaps(colormaps, n, rot); }
   #endif
   ```
3. **`r_main.c`.** Vanilla's `transcolfunc` and `spanfunc` pointers are back next to `colfunc`, and
   `R_ExecuteSetViewSize` is the one place that picks the drawers:
   ```c
   #ifdef CGD_SH4
     if (!detailshift) { colfunc = basecolfunc = R_DrawColumn_SH4;   fuzzcolfunc = R_DrawFuzzColumn_SH4;
                         transcolfunc = R_DrawTranslatedColumn_SH4;    spanfunc = R_DrawSpan_SH4; }
     else              { colfunc = basecolfunc = R_DrawColumnLow_SH4; fuzzcolfunc = R_DrawFuzzColumnLow_SH4;
                         transcolfunc = R_DrawTranslatedColumnLow_SH4; spanfunc = R_DrawSpanLow_SH4; }
   #else   /* same with the C names (R_DrawColumn, …, R_DrawColumnLow, …) */
   #endif
   ```
4. **Call sites** use the vanilla pointers, not CGDoom's direct calls:
   - `r_segs.c`: the 3 wall-tier calls are `colfunc()`;
   - `r_plane.c`: `spanfunc()` in `R_MapPlane`, `colfunc()` for the sky;
   - `r_things.c`: `colfunc = transcolfunc` for translated sprites.
   - An indirect `jsr @Rn` costs the same as CGDoom's direct call, which also loads the address from a literal pool.
   - **No call site passes `count < 0`** (`dc_yh < dc_yl`): `R_DrawMaskedColumn` and the sky are
     guarded, and `R_RenderSegLoop` skips empty mid-texture columns (vanilla calls `R_GetColumn` +
     `colfunc` for them, 26% of the column calls in e1m1). The drawers still handle it exactly: the
     count check sends it to the C fallback, which returns at once (about 35 cycles, set-up included).
     There is no early return for it, which would lengthen the set-up of every real call (sim profiles
     of e1m1, map01, demo1, e1m1-low and demo1-low: 0 fallback calls).
5. **Host C low-detail drawers.** `r_draw.c` has `ref_R_DrawColumnLow`, `ref_R_DrawSpanLow`,
   `ref_R_DrawFuzzColumnLow` and `ref_R_DrawTranslatedColumnLow` of `test/ref_draw.c` without the `ref_`
   prefix, so host and sim CRCs agree in low detail. The full-detail C drawers are the reference.
6. **Presenter (gint).**
   - `src/gint/video.c` keeps `static uint32_t g_pal32[256]`, rebuilt with `cgd_expand_build_pal32()` in
     `PL_SetPalette`. `present_dma()` makes one `cgd_expand_p8_rgb565_p32(g_fb16, fb, g_pal32, 64000)`
     call per frame into the 32-byte-aligned RGB565 frame in P1 SDRAM, then `synco()` and the DMA to the
     LCD. The strong versions (`cgd_expand_p8_rgb565_p32` in `expand_sh4.o`, `cgd_expand_build_pal32` in
     `r_draw_sh4_c.c`) replace the weak C ones in `video.c`.
   - The CPU fallback `present_cpu()` (no frame buffer, or an unaligned source frame) looks up `g_pal32`
     per pixel and writes the LCD data register directly; it does not call the expansion routines.
   - The persistent table keeps the plain entry's per-call table build (about 870 cycles and 1 KiB of
     stack) out of the frame.
   - For P1 destinations the routine purges each line with `ocbp`, so the data is in RAM when it returns. `present_dma()` therefore calls only `synco()` after the expansion, to order the write-backs before the DMA starts; an `ocbwb` loop over the frame (4000 lines that are no longer cached) would cost roughly 12–20k cycles, 0.1–0.17 ms per frame, for nothing. A replacement expander must purge (or write back) every line itself.
7. **Helpers not used by the engine or the gint layer.** All but `cgd_pref` are covered by the test (§8):
   - `cgd_fixed_div`, a vanilla-exact `FixedDiv` (with `cgd_udiv64_32`). The engine's `FixedDiv` in
     `m_fixed.c` has its own inline-asm division, checked by the root `make test-fixed-sim`.
   - `cgd_pref(p)`, e.g. to prefetch the next column's texture in `R_RenderSegLoop` while the current one draws.
   - `cgd_fill32`, for P1 clears such as a full-frame background.
   - The plain `cgd_expand_p8_rgb565`, which needs no `pal32` table.

## 8. Tests and tools

```sh
make -C src/engine/sh4/test run     # asmcheck, then randomized bit-exactness (N=3000/drawer): ILRAM build,
                                    # .text build, forced-P1 build of the expansion (movca.l/ocbp paths);
                                    # exit 0 = PASS
make -C src/engine/sh4/test asmcheck  # the raw-opcode .S build (no -Wa,--dsp) matches the --dsp
                                    # mnemonic build of the game builds byte for byte
make -C src/engine/sh4/test bench   # simulated instruction counts C vs asm (builds sim64 if needed)
make -C src/engine/sh4/test pipe    # pipeline-model cycles of every inner loop (pess, opt, gcc);
                                    # PIPE_FLAGS="--pairing manual" for the manual's pairing rule
make -C src/engine/sh4/test sched   # modsched.py: the shipped loops meet the model (pess, opt; spans also
                                    # gcc), and the minimum II of each kernel under "max" (python only)
make -C src/engine/sh4/test sizes   # section/symbol sizes
N=20000 make -C src/engine/sh4/test run
```

**Cases covered:**
- Views of 320×200, 320×168, 224×112 and 96×48 with window offsets.
- Every x position and row.
- Counts from −5 to the full height, with short counts (0–4) weighted.
- **1-pixel passes:** `dc_yl == dc_yh` only, for `R_DrawColumn` and `R_DrawColumnLow`, with every colormap kind below, with non-contiguous `columnofs` (low detail) and with nothing registered; stray-write check after every call.
- Steps: realistic, tiny, negative and arbitrary 32-bit.
- `centery` offsets.
- Colormap pointers:
  - registered maps;
  - unregistered tables;
  - misaligned pointers inside the lump;
  - just before or after the lump;
  - nothing registered.
- Non-contiguous `columnofs`, which exercises the low-detail C path.
- `fuzzpos` 0..49, with `dc_yl = 0` and `dc_yh = viewheight-1` edges.
- Global side effects, compared after every case.
- Framebuffer guards of 4 KiB on both sides.
- Expansion sizes 16..6400 at 8 source alignments. The first cases use fixed sizes: 3,200, 6,400 (the test buffer's maximum), and 16, 240, 256, 272 around the plain entry's table threshold, for both entries.
- `fill32` sizes 0..6400.
- 30,000 `FixedDiv` operands, plus `a == INT_MIN` against fixed boundary divisors (±1, ±32767..±32769, ±40000, ±65535..±65537, ±131072, INT_MAX, INT_MIN) and 300 random `|b| <= 65600`.
- A **long pass**: full-height columns on 320×200 (`dc_yl = 0`, `dc_yh = 199`) and full-width spans for every drawer, `N/100 + 8` cases each.

**Checks:**
- After every case, the touched area plus a margin is compared.
- Every 16 cases, and at the end, the whole framebuffer is compared, guards included.
- **Stray writes:** every 128th case, every case of the long pass, every 1-pixel case and every 8th expansion case snapshot all static memory, i.e. `[0x1000, __bss_end)`: `.text`, `.rodata`, `.data` and `.bss`, ≈830 KB. They then check after the asm call that only the asm's own output buffer and the globals the drawer may write (`dc_yl`, `dc_yh`, `fuzzpos`, `ds_x1`, `ds_x2`) changed. This catches writes the framebuffer guards cannot see: a translated drawer whose second store pointer walks 320 bytes per pixel through `.bss` fails from the first sampled translated case.
- **Expansion stack canary:** 32 words in the caller's frame, directly above the plain entry's 1 KiB stack table, are checked after every expansion call. A 4-byte overrun past the table makes every plain-entry case fail with "stack canary".
- **Calling convention:** every asm drawer call goes through `test/abi_call.S`, which loads r8–r14 with values derived from a random word and checks them, and r15, after the call. This matters for the 1-pixel paths, which save no registers: `mov #0,r9` in the 1-pixel path makes every 1-pixel case fail with "callee-saved register clobbered".
- **Assembler flags** (`asmcheck`): see the top of this file. Restoring the plain `ldrs`/`ldre`/`ldrc` mnemonics makes it fail with gas's "opcode not valid for this cpu variant"; a wrong displacement in the `LDRE` macro makes it fail with a byte diff.
- **Other injected bugs the comparison catches:** a wrong mask or a missing `extu.b` in the 1-pixel column path; a dropped second store in the 1-pixel low-detail path; a swapped write-after-read pair in the R_DrawSpanLow loop (fails from case 2 at N=300); a `>> 16` instead of `>> 15` second guard in `cgd_fixed_div` (7 of the fixed `INT_MIN` divisors and about half of the random ones fail).

Design tools in `test/`:
- **`pipesim.py`:** loop, straight-line and per-call models over the linked ELF. `--path A:B C:D …` models one call through taken branches, e.g. `pipesim.py ELF - - --path _R_DrawColumn_SH4:cgd_col_disp cgd_col_one:cgd_col_one_end` for the 1-pixel column, or `_R_DrawColumn_SH4:cgd_col_n2 cgd_col_end:cgd_col_ret` for n = 2.
- **`listsched.py`:** reorders set-up blocks under the model. It applies the 4-byte pairing rule by default and supports dispatch branches marked `/* barrier: regs */` (§2).
- **`modsched.py`:** exact modulo-schedule search. See its docstring for the model. In short:
  - `modsched.py KIND [II] [pess|opt|gcc|max] [--px P]`, KIND = `col collow span spanlow`. Without II it starts at max(ResMII, RecMII), proves every smaller II infeasible (a positive dependence cycle, or exhaustive search) and prints the first feasible schedule (rows, stages). `max` is the elementwise maximum of the three latency tables, so its schedules run stall-free under all three.
  - Anti-dependences (write-after-read) have latency 0 and may share a cycle with the reader first, as the pairing rule allows (`--war1` gives them latency 1, which forbids such pairs and puts R_DrawSpan at II = 12, §3).
  - `--px P` unrolls the model to P pixels (texels) per pass; `--stage1 OPS` pins a two-stage split so that an existing prologue/epilogue can stay (every alignment of the stage boundary is tried, including splits with a stage-1 op ahead of op 0, so "infeasible" is a proof for the split); `--shipped` checks the loops of `r_draw_sh4.S` (table `SHIPPED`, hand-transcribed issue times, not derived from the code: update it whenever a loop changes) instead of searching.
  - The printed body starts at the earliest op (or the `--stage1` window), so the listed order and the stage numbers agree, and the listing can be typed in as written.
  - Search: longest-path closure for exact time windows, most-constrained-first assignment with window propagation, resource-aware bounds tightening, a slot-matching bound, and an unrolling shortcut for feasibility. Each 2-pixel kernel solves in milliseconds under every table. Minimum II under "max" (c/px) for P = 1 / 2 / 3 / 4 pixels or texels per pass: span 6 / 11 / 17 / 22 (6.0 / 5.5 / 5.67 / 5.5); spanlow 6 / 12 / 18 / 24 (3.0 throughout); col 5 / 8 / 12 / 16 (5.0 / 4.0 / 4.0 / 4.0); collow 5 / 10 / 15 / 20 (2.5 throughout). No unrolling beats the shipped P = 2 under any single table either (under "pess" collow is 9 / 14 / 18 for P = 2 / 3 / 4, i.e. 2.25 / 2.33 / 2.25 c/px). All of these, P = 1–4 under every table, take about 16 s in total; the slowest proof is collow under "gcc" at P = 4 (II 18 and 19 infeasible, ≈9 s).
- **`gen_expand.py`:** generates `expand_sh4.S`.

## 9. Further work (not done)

- **Register-argument entry points. Recommended next step.**
  - The globals cost about 27 loads and stores per call, about 45–50 cycles of set-up. In sprite-heavy scenes most calls are tiny (in demo1, 58% of the column calls are 1 or 2 pixels), so set-up weighs heavily there.
  - A `R_DrawColumn_SH4_args(dest, src, cmrot, count, frac9, step9)` called from `R_RenderSegLoop` and `R_DrawMaskedColumn`, which already hold these values in registers, would cut the set-up to about 12 cycles.
  - That saves about 40 cycles per multi-pixel call and about 20 per 1-pixel call: with the current call mix, about 25k cycles per frame in e1m1 and 83k in demo1 ([`tests/perf-baseline.md`](../../../tests/perf-baseline.md), "Remaining levers").
  - The SH ABI passes only 4 arguments in registers (r4–r7). Six would spill two to the stack, so pack count/frac/step or pass a pointer to a small struct.
- **Owning the `dc_*`/`ds_*` globals in one asm block.** The engine would declare them `extern`. One base literal plus `@(disp,Rn)` loads would replace 10 literal loads per call.
- **R_DrawColumn under "gcc" latencies:** 10 → 8 cycles per 2 px is possible with a three-stage pipeline (`modsched.py col max`, §3). Only worth it if hardware timing shows 3-cycle loads.
- **R_DrawColumnLow under "gcc" latencies:** 10 → 9 cycles per 4 px with a second colour register (r12: one more push/pop, B's stage-1 ops and the epilogue use it), keeping the two-stage split (§3). Same condition.
- Not worth doing: `mov.w` stores of doubled colours for low detail. A 16-bit doubled-colormap lookup needs the texel scaled by 2 in `r0`, which costs one more EX op per texel. The low-detail loops are already EX-bound (column: 8 EX + 8 LS in 9 cycles). Saving one LS per texel while adding one EX gives no better than 9–10 cycles per 4 px for the column, and II ≥ 12 for SpanLow (12 EX), the same as the byte version. It only helps with pre-doubled texture bytes. So with these loops a doubled colormap does not let one set of operations cover two pixels.
- **Two-line iterations of the expansion**, which halve the 4–5 cycle per-line overhead.
- **Hardware validation of the latency model.**
  - Use gint `prof_*` around 10^4 calls of each drawer, both with `CGD_SH4_NO_ILRAM` and without.
  - Expected bracket: "pess"/"opt" vs "gcc" (§3). Column 4.0 vs 5.0 c/px; expansion line 2.81 vs 3.31 c/px.
  - Test the 4-byte-alignment pairing rule directly, for example by measuring one loop with and without a leading `nop`.
  - Use the SH-4A performance counters if the SH7305 has them (not confirmed; layout from Linux `arch/sh/kernel/cpu/sh4a/perf_event.c`):
    - registers: 2 counters, `CCBR(i)` at `0xFF200800 + 4i`, `PMCTR(i)` at `0xFC100000 + 4i`, `PMCAT` at `0xFC100080`;
    - enable: set `CLR` in `PMCAT`, write `CCBR = (event << 6) | CMDS | PPCE`, then set `DUC`;
    - events: `0x0000` cycles, `0x0202` instructions, `0x0203` instructions issued in parallel, `0x002a` I-cache miss, `0x0032`/`0x003a` O-cache read/write miss, `0x0033`/`0x003b` wait cycles from O-cache misses, `0x02b4` operand-read wait cycles, `0x0236`/`0x023e` X/Y-memory reads/writes, `0x0220`/`0x0222`/`0x02a0` TLB misses.
    - If they exist they give exact IPC and miss counts per drawer, which settles the latency model directly.
