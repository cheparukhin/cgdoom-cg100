# SH4 assembly review log (src/engine/sh4)

## sh4 review round 1

Scope of fixes: `src/engine/sh4/` only. Test gate: `make -C src/engine/sh4/test run`, which runs three builds: 60,966 cases in the main build, 6,209 in the `.text` build and 600 in the forced-P1 build. All pass, with 0 failures.

### 1. HIGH: full-detail translated drawer wrote through a walking scratch pointer (fixed)

- **Finding.** `R_DrawTranslatedColumn_SH4` shares its loop with the low-detail variant and stores each texel through both r4 and r8, stepping both by 320.
  - In full detail, r8 pointed at a 4-byte `.bss` scratch object.
  - A column of n pixels therefore wrote to `scratch + 320*k` for k = 0..n-1, up to about 64 KB of `.bss` after the object.
  - The framebuffer-plus-guards test missed it.
- **Fix** (`r_draw_sh4.S`).
  - Full detail now sets `r8 = r4` (`mov r4, r8` replaces `mov.l .Lt_scratch, r8`). Both stores write the same byte with the same value, and nothing else is touched.
  - `.Lt_scratch` and `cgd_sh4_scratch` are deleted. The object now has no `.data` or `.bss`.
  - Loop timing is unchanged, at 16 / 12 c/px.
- **Test** (`test/test_sh4.c`).
  - New stray-write check. It snapshots all static memory, `[0x1000, __bss_end)`, which covers `.text`, `.rodata`, `.data` and `.bss`, about 830 KB.
  - After the asm call it verifies that only the asm's output buffer and the allowed side-effect globals changed. On a failure it undoes the change and then reports.
  - It runs every 128th random case, on every case of a new **long pass** (full-height columns and full-width spans for all 8 drawers), and on every 8th expansion case.
  - Against the pre-fix object, it fails with 46 failures (e.g. `R_DrawTranslatedColumn case 127: 118 stray word(s), first at 0x00011424`). With the fix it passes.

### 2. LOW: `cgd_fixed_div(INT_MIN, b)` for 32768 < |b| < 65536 returned a wrong-signed value (fixed)

- **Fix** (`cgd_sh4.h`).
  - The second guard is now `(ua >> 15) >= ub`. It can only fire for `a == INT_MIN`, and it saturates to INT_MIN or INT_MAX by sign for `|b| <= 65536`.
  - `b == 65536` returns INT_MIN, which is exact and is also what vanilla returns.
  - The result is unchanged for every input that vanilla accepts.
  - The header comment and README §1 now state the exact contract: vanilla I_Errors for `|b| < 65536` and for `b == -65536`.
- **Test.** `test_div` adds `a == INT_MIN` against fixed boundary divisors (±1, ±32767..±32769, ±40000, ±65535..±65537, ±131072, INT_MAX, INT_MIN) and 300 random `|b| <= 65600`. It also checks `cgd_fixed_div(INT_MIN, 65536) == INT_MIN`. The old header fails 7 of the fixed cases (±32769, ±40000, ±65535, -65536) and about half of the random ones.

### 3. HIGH: `make sim` cannot assemble `r_draw_sh4.S` (NOT fixed here: outside `src/engine/sh4/`)

- **Cause.** The `%.S.o` rule in `src/sim/cgdoom.mk` lacks `-Wa,--dsp`. Re-verified: plain `make sim` exits with rc=2 and 21 "opcode not valid for this cpu variant" errors. `make sim SIM_ARCH="-m4-nofpu -mb -Wa,--dsp"` links `cgdoom-sim.elf` with the fixed asm.
- **Required one-line fix for the owner of `src/sim/cgdoom.mk`** (line 42): `$(SIM_CC) $(CGSIM_CFLAGS) -Wa,--dsp -c $< -o $@`.
- README now documents the issue and the workaround at the top.

### 4. LOW: pipeline model groups and pairing did not match the SH4AL-DSP manual (fixed)

- **Fix** (`test/pipesim.py`, `test/listsched.py`).
  - Groups now follow manual table 4.2:
    - `mul.l` and `mul*.w`/`dmul*.l` are EX, and `mul.l`→`sts` is still 3 cycles.
    - `sts`/`lds` between Rn and MACH/MACL/PR are LS.
    - `ldrs`/`ldre` are LS.
    - `ldrc`/`setrc` are CO with issue rate 2.
    - `clrt`/`sett` are EX.
    - `cmp`/`tst` stay EX and `mov #imm` stays MT, as the SH4AL-DSP manual says. The GCC `sh.md` classes cited in finding 8 are for the SH-4.
  - New option `--pairing aligned|manual|any`. The 4-byte-alignment rule stays the default and is labelled empirical (Azur), not from the manual. `manual` applies the 1 KiB-page rule alone.
  - `listsched` honours the `sts` latency and the CO issue rate.
- **Effect.**
  - Loop timings are unchanged under both pairing rules, except fuzz, which is 1 cycle faster under `manual`.
  - Per-call set-up under "pess" changed: column 62→64, low column 72, span 74→75, low span 83→87.
  - README §3 has the corrected group list, the pairing text and the new per-call figures.
  - The set-up blocks were not re-ordered.

### 5. LOW: headline column c/px assumed wall-length columns (fixed, documentation)

- README §3 now has a call-weighted table built from the review's host histograms:
  - e1m1, e1m3 and e3m4: 4.6–4.7 c/px, 3.1–3.2× faster (pess), 2.5× (opt) and 2.7–2.8× (gcc).
  - demo1: 9.1 c/px, 2.0×, 1.7× and 1.9×. 77% of its calls are ≤ 8 px, 50% are 1 px, and set-up is about half of the asm column cycles.
  - The table includes the Freedoom and possible-desync caveats.
- §9 makes the register-argument entry the recommended next step. It notes the 4-register ABI limit and the cheaper 1-pixel interim path.

### 6. LOW: expansion table-build cost was inconsistent and understated (fixed, code and documentation)

- **Code.**
  - The plain entry's stack table loop now builds two entries of each half per iteration (generated by `gen_expand.py`). It is hand-ordered in aligned LS|EX pairs.
  - It takes 13 cycles per iteration × 64 (14 under gcc), about 830 for the loop and about 870 per call. The old loop was 10 per entry pair × 128, about 1300.
  - It saves r8–r10, so peak stack is 1052 bytes, previously 1040.
  - New labels `cgd_xt_loop`/`cgd_xt_end`, `cgd_xs_loop`/`cgd_xs_end` and `cgd_x_next`/`cgd_x32_next` let `make pipe` model the whole line loop.
- **Docs.** README, `cgd_sh4.h` and the `expand_sh4.S` header now agree:
  - `_p32`: 2.81 c/px for the whole line loop.
  - Plain entry: about 2.95 c/px at 6,400 px and about 2.83 at 64,000 px.
  - The `_p32` gain is about 0.13 c/px (about 0.15 ms per frame) in strip mode and negligible in DMA mode.
- **Test.**
  - Every expansion call now runs with a 32-word stack canary in the caller's frame. Injecting an overrun of 4 bytes past the table makes every plain case fail with "stack canary".
  - Every 8th expansion case also gets the static-memory stray check.
  - A store-offset slip made while writing the new loop was caught by the existing tests, as a bus error.

### 7. LOW: `mov.w` doubled-colour low-detail idea (fixed, documentation)

- Removed from §9 "Further work" and replaced by a note explaining why it cannot help: the EX-bound loops would gain one EX per texel.
- The SpanLow II=12 entry now says it is feasible by resource count but not found by the search.
- `notes/research-performance.md` §3.4 still claims "the same 8 operations cover two pixels". That file is outside this directory and was not edited.

### 8. LOW: README model text misstated GCC `sh4a.md`; no load-latency-3 bracket (fixed)

- **Model.** pipesim has a new `gcc` latency model: every load 3 cycles, EX→address 1, `sts macl` 3. `make pipe` runs pess, opt and gcc.
- **Results under gcc.**
  - Column: 10 cycles per 2 px (5.0 c/px).
  - Low column: 10 cycles per 4 px.
  - Span: 11.
  - Low span: 13.
  - Expansion core: 48 cycles. Line: 53 cycles (3.31 c/px).
- **README §3** now says that "gcc" is slower than both pess and opt, and lists the bracket.
- **modsched.** `modsched.py` takes a model argument. `col 9 gcc` and also `col 8 gcc` find schedules (the latter after a few minutes). This differs from the finding's "no II=8 within 200 s". modsched ignores the alignment rule and register allocation.

### 9. LOW: README called the presenter's `ocbwb` loop "cheap" (fixed, documentation only)

- README §7.6 now says that `cache_writeback()` after the purging expander is redundant (about 12–20k cycles, 0.1–0.17 ms per frame) and that only its `synco` is needed.
- It also describes the optional `video.c` change: call only `synco()` there when the asm expander is linked.
- `src/gint/video.c` is outside this directory and was not edited.

## sh4 review round 2

Three confirmed findings. All fixes are inside `src/engine/sh4/`. `make -C src/engine/sh4/test run` now runs `asmcheck` first, then the three builds: 63,966 + 6,507 + 600 cases, 0 failures. `N=20000` gives 425,726 cases, 0 failures.

### 1. MEDIUM: sim game build still assembled `src/engine/sh4/*.S` without `-Wa,--dsp` (round 1 finding 3; fixed inside this directory)

- **Finding.** `src/sim/cgdoom.mk` assembles the `.S` files with plain `$(CGSIM_CFLAGS)`. gas rejected every `ldrs`/`ldre`/`ldrc`, so `make sim`, `test-sim`, `test-sim-full` and `perf-sim` could not build.
- **Fix.** `cgdoom.mk` is outside the allowed directory, so the sources no longer need the flag.
  - `sh4_asm.h` gained `LDRS(label)`, `LDRE(label)` and `LDRC(regno)`. They emit the raw opcodes `0x8c00|disp`, `0x8e00|disp` (disp = (label − . − 4)/2, as in `sh-opc.h` and GDB `gencode.c`) and `0x4034|m<<8`.
  - With `-DCGD_SH4_DSP_MNEMONICS` they expand to the mnemonics instead. `r_draw_sh4.S` uses the macros.
  - The objects are no longer tagged `sh4al-dsp`, so `pipesim.py` disassembles with `-m sh4al-dsp`.
- **Verified.**
  - The new `r_draw_sh4.o` has the same `.ilram`/`.text`/`.rodata` bytes and relocations as the round-1 object.
  - `make sim`, with `cgdoom.mk` unchanged, now builds and links `build-sim/cgdoom-sim.elf`.
- **Test.** New `asmcheck` target (part of `run`).
  - It assembles both `.S` files with a copy of the sim game's flags (`$(SIM_CFLAGS) -Wall -I…`, no `--dsp`), and also with `-Wa,--dsp -DCGD_SH4_DSP_MNEMONICS`, in the default and `CGD_SH4_NO_ILRAM` layouts.
  - It then requires identical `objdump -s -r` output.
  - Restoring the plain mnemonics fails it with gas's "opcode not valid for this cpu variant". A deliberately wrong `LDRE` displacement fails it with a byte diff.
- **Still optional outside this directory.** Add `-MMD -MP` to the `%.S.o` rule in `cgdoom.mk` so that `sh4_asm.h` edits trigger rebuilds.

### 2. LOW (reported MEDIUM): 1-pixel columns cost a full set-up (fixed)

- **Finding.** The 1-pixel cost was really 62 cycles once the `bf/s` + `bra` double branch is counted (the table said 64). Half of demo1's column calls are 1 px. The finding measured a ~27-cycle saving with an unscheduled prototype.
- **Fix (`r_draw_sh4.S`).**
  - **Dispatch.** `R_DrawColumn_SH4` and `R_DrawColumnLow_SH4` load `dc_yl`, `dc_yh`, `dc_iscale` and `centery` first. They branch on `dc_yl == dc_yh` before any push.
  - **1-pixel paths `.Lc_one` / `.Ll_one`.** These are straight-line code using r0–r7 only. They skip the colormap registration check: the `extu.b` plus a direct `dc_colormap` lookup is cheaper for one texel. The low-detail path does two independent stores, to `columnofs[x]` and `columnofs[x+1]`, with no contiguity check. Both are hand-scheduled in aligned pairs.
  - **n = 2 (low: 2 texels)** now takes one branch, `bt/s .Lc_epi` with the first texel load in the delay slot, instead of `bf/s .Lc_noloop` followed by `bra .Lc_epi`. The loop-only `add` moved between `ldrc` and RS, so at least one instruction still separates them.
  - **Re-ordering.** The rest of the set-up was re-ordered with `listsched.py`, which gained two features:
    - It now applies the 4-byte pairing rule (`--pairing aligned`, default).
    - A `/* barrier: regs */` mark makes a dispatch branch keep everything before it, schedule as early as possible, and let nothing that writes the other path's inputs move above it.
  - **Size.** ILRAM grows from 1224 to 1356 bytes.
- **Measured.** pipesim's new `--path` mode charges each taken branch. pess / opt / gcc, before → after:

  | call | before | after |
  |---|---|---|
  | column, 1 px | 62 / 58 / 67 | 32 / 30 / 34 |
  | column, 2 px | 62 / 58 / 67 | 57 / 53 / 61 |
  | column, n ≥ 3 | 56 + 4n | 52 + 4n |
  | low column, 1 texel | 69 / 67 / 74 | 33 / 31 / 37 |
  | low column, 2 texels | 69 / 67 / 74 | 63 / 62 / 70 |
  | low column, 3–4 texels | 81 | 76 |

  - Weighted by the round-1 histograms, demo1 goes from 9.08 to 7.49 c/px, and the speed-up over C from 2.0× to 2.4× (pess). That saves ≈70k cycles, ≈0.6 ms per frame.
  - Wall scenes improve by about 1%.
  - A 1-pixel column is now faster than C (32 against 61 cycles), where before it was slower.
- **Test.**
  - New "1px" passes force `dc_yl == dc_yh` for both drawers. They cover every colormap kind, with odd `columnofs` (low detail) and with nothing registered, and run the stray-write check after every call.
  - New `test/abi_call.S` wraps every asm drawer call. It loads r8–r14 with values derived from a random word, then checks them and r15 after the call.
  - Mutations caught:
    - wrong mask in `.Lc_one`
    - missing `extu.b`
    - dropped second store in `.Ll_one`
    - `mov #0,r9` in `.Lc_one` ("callee-saved register clobbered: r9")
- **README.** Updated §2, §3 (per-call table, before/after table, scene table, break-even), §4, §5, §6, §8 and §9. The interim step in §9 is marked done.

### 3. LOW: strip-mode expansion figures used n = 6,400 px (fixed, documentation and test)

- **Finding.** `video.c` passes `STRIP_PIXELS` = 320 × 10 = 3,200 px per call. The 6,400 is the strip size in bytes.
- **Fix.** README §1, the per-call table and §7.6, plus `cgd_sh4.h`, now give these figures:
  - 3,200 px per call, 20 calls per frame.
  - Plain entry about 3.09 c/px.
  - `_p32` saves about 0.27 c/px; the per-frame figures (≈17k cycles, ≈0.15 ms) were right and are unchanged.
  - `gen_expand.py` and `expand_sh4.S` did not repeat the figures.
- **Test.** The first expansion cases use fixed sizes, for both entries:
  - 3,200 (the real strip size), never exercised before;
  - 6,400;
  - 16, 240, 256 and 272, around the plain entry's table threshold.

## sh4 round 3: loop schedules (integration-todo "Loop schedules")

- **`test/modsched.py` is now an exact search** (port of the scratchpad `msched/bounds_neg.py` + `unroll.py`, CLI kept:
  `modsched.py KIND [II] [LAT]`). Longest-path closure for exact windows (a positive cycle = II < RecMII),
  most-constrained-first assignment, window propagation, resource-aware bounds tightening, a slot-matching bound, an
  unrolling shortcut, and `verify()` on every result. Without II it reports the minimum feasible II and proves every
  smaller one infeasible. New: `--px P` (pixels/texels per pass), `max` latencies (elementwise max of pess/opt/gcc:
  stall-free under all three), `--stage1 OPS` (pin a two-stage split), `--shipped` (check the r_draw_sh4.S loops,
  table SHIPPED), `--war1` (old model). `make -C src/engine/sh4/test sched` runs the checks. All 2-pixel kernels solve
  in ms; P = 1..4 for all kernels and tables ≈16 s in total.
- **Model fixes.** (1) Anti-dependences (an op overwriting a register an earlier op reads) now have latency 0 and may
  share a cycle with the reader first; the pairing rule only forbids the second instruction touching a register the
  first *writes*. (2) collow: one frac per texel (as shipped) and the `@-rD` → `add 322` dependence (latency 1, was 0).
- **R_DrawSpan: the model was wrong, the code and README were right.** With latency-1 WARs the r0 cycle
  f → g → h → f′ is 2 + 3 + 1 = 6 c/px ("pess"), RecMII 12. The shipped loop pairs `h_A'|f_B'` and `d_A|a_B` in one
  cycle; with latency 0 RecMII = 10 and min II = 11 under every table. `modsched.py span --shipped --war1` lists exactly
  those two pairs. 11 cycles = 22 instructions / 2 is the issue-width floor; no register allocation goes lower.
- **R_DrawSpanLow: II 13 → 12 (3.25 → 3.0 c/px) under pess, opt and gcc**, both pairing rules (pipesim). Found with
  `modsched.py spanlow max --stage1 Be,Bf,Bg,Ah,Bh,Ai,Ai2,Bi,Bi2` (the span's stage split), so only the loop body
  changed: same registers, set-up, prologue, epilogue; the 2 padding nops are gone. Per call (pess / opt / gcc):
  85 texels 633 / 629 / 639 → 591 / 587 / 597; sim instructions 7.05 → 6.56 per px (len 20..320). `.ilram`
  1356 → 1352 bytes. Test: 63,966 + 6,507 + 600 cases, 0 failures; asmcheck green (mnemonic build with
  `-DCGD_SH4_DSP_MNEMONICS -Wa,--dsp` byte-identical to the raw-opcode build); swapping the `h_B'|f_A` pair is caught.
- **Columns (unchanged).** collow: min II under "gcc"/"max" is 10 (proved), so the shipped 9 / 9 / 10 is optimal per
  table. col: `col max` has II = 8 (4.0 c/px under all three tables) but needs a three-stage pipeline; the shipped
  two-stage split gives at best 9 under "max". Shipped col is 8 / 8 / 10 (pess / opt / gcc). Worth redoing only if
  hardware shows 3-cycle loads (README §9). No unrolling (P = 3, 4) beats P = 2 for any kernel under any table.

## SH4 loop schedules review (adversarial review of round 3)

- **Code: no defect found.** The R_DrawSpanLow body (II=12) was traced op by op: every RAW distance meets "max"
  (colour load → first store 3, texel load → colour load ≥ 3, MT → address 2), the two same-cycle WAR pairs (c1, c6 on
  r0; c10 on r7) have the reader first, every pair is LS/EX/MT-legal and 4-byte aligned, the loop leaves exactly the
  state the unchanged epilogue expects (r2 = d_B, r0 = texel A, T untouched: no T-writing op in the body), and a store
  as the repeat-end instruction is legal (Azur's `clear.S`/`r61524.s` end their repeat loops with stores). pipesim
  per call 591/587/597 (85 texels) and 87/83/93 (1–2 texels), sizes (.ilram 1352, SpanLow 344) and sim insts/px 6.56
  reproduced. Mutation: swapping the c10 WAR pair (`j_B` before `c_B`) fails R_DrawSpanLow from case 2 (N=300).
- **`SHIPPED` table** (hand-transcribed issue times behind `--shipped`): all four loops checked row by row against
  `r_draw_sh4.S`; they match. It is not derived from the code, so it must be updated with any loop change (README §8).
- **modsched.py defect 1 (fixed): `--stage1` was not an exact search.** It pinned stage s to [s·II, (s+1)·II) relative
  to op 0, i.e. only the stage-boundary alignment where op 0 is the first stage-0 op. Splits that need a stage-1 op
  before op 0 were reported infeasible: `spanlow max --stage1 <shipped split>,Bk` said "no schedule up to II=20"
  (II=12 exists), `col gcc --stage1 Add,Bl,Ast,Bdd,Bc,Bst` said 9 (8 exists); 18 such misses among the one-op
  variations of the shipped splits. Fix: `solve_staged()` tries every alignment that keeps op 0 in its stage, and
  checks the result's stages. None of the documented results changed: with all alignments the shipped splits still
  give col 8/8/9/9, collow 9/9/10/10, span 11, spanlow 12 (pess/opt/gcc/max), and the free-search minimum IIs for all
  kernels, tables and P = 1..4 are identical (same output hash; ≈16 s total).
- **modsched.py defect 2 (fixed): the printed body could contradict its stage labels.** `show()` listed rows from 0
  with stages relative to the earliest op; when an op is scheduled before op 0 the listing put it after its consumers
  as the same stage (`span opt`: `c_A`/`j_A` printed at c9 after `d_A` at c2, all "st0"), a body that computes
  garbage if typed in as listed. Now the listing starts at the earliest op (or the `--stage1` window). The shipped
  loops were not affected (their schedules have op 0 first).
- **Over-claim (README fixed): collow "optimal under each model".** The proof (II=10 under gcc/max) holds for the
  shipped register allocation (one colour register). With a second colour register the model gives II=9 under "gcc"
  with the shipped two-stage split (and II=9 under "max" with three stages); RecMII drops 8 → 6 under gcc. Recorded as
  a hardware-conditional option in README §9; no code change (pess/opt are already 9).
- **Refuted:** WAR latency 0 (the SH-4A pairing rule forbids only RAW/WAW with the first instruction; the shipped Span
  loop already depended on it); `max` table soundness (elementwise max ⇒ stall-free under each table for modelled
  edges); search pruning (window narrowing, slot counts and the bipartite slot matching are relaxations, so
  "infeasible" stays a proof); unrolling shortcut (every unrolled schedule is re-verified against the P-pixel model);
  col "two-stage best under max is 9" (re-proved at all alignments); the `--shipped` gcc violations of the column loops
  are exactly the colour-load → store edges as documented.
- **Gates:** `make -C src/engine/sh4/test run` (63,966 + 6,507 + 600, 0 failures, asmcheck green), `sched`, `pipe`,
  `sizes`, `bench`; `make check-sh`; `fxsdk build-cg` (no warnings); `make test-sim` 3/3. No C changed, so
  test-host/asan/unit and gint-host not re-run.
