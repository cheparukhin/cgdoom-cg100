# Performance baseline: portable C engine in the SH simulator

This is the starting point for optimisation work. Later measurements are kept per step in `notes/engine-changes.md`;
step 1 (vanilla-exact fixed point) cut the per-frame cost by about 21% on e1m1/map01 and 39% on demo1, and step 2
(per-column glue, divisions, renderer limits) cut about another 4–5%. Step 3 left the default mode within +0.15% and
added optional modes (low detail, view size, potato floors). The table **"After the engine C optimisations"** below
has the C-only numbers; **"After the SH4 drawers"** and **"After the renderer polish"** have the current default ELF.

This baseline was measured on 2026-10-09 on the engine and `src/host` as left by the foundation work: the C renderer, CGDoom's `FixedMul`/`FixedDiv`, no assembly.

## What was measured

- **Build:** `make sim` produces `build-sim/cgdoom-sim.elf`. It compiles the same engine and host sources as `make host`
  with `sh-elf-gcc 14.1 -m4-nofpu -mb -O2 -std=gnu11 -fno-strict-aliasing -DPL_SIM -ffunction-sections -fdata-sections`.
  RANGECHECK is off, as on the calculator. The ELF is linked with the `src/sim` runtime and fxlibc, with
  `.text` at 269 KiB.
- **Simulator:** `build-sim/tools/sh-elf-run64` (sim64, from `make sim-tool`). It is the GDB 15.2 SH simulator with 64-bit
  counters and an exact per-PC profile.
- **Game settings:** view size 10, which is a 320x168 view with the status bar, full detail and `-frameskip 0`.
- **Window:** frames 42 to the end of each scenario.
  - Frames 0–41 are the level-start screen wipe, and the level loads before frame 0.
  - Frames in which `P_SetupLevel` ran are left out and counted under "load". This happens once in e1m3, when the player
    dies and the level restarts, and once in demo2-1, on a level change.
- **What counts as frame cost:** everything between two `PL_FinishUpdate` calls, including game tics, the status bar
  and menus. The work inside `PL_FinishUpdate` is excluded. It is the test platform's CRC, about 0.78 M instructions per
  frame, and it does not exist on the calculator.
  - "render" is `R_RenderPlayerView`.
  - "game" is `G_Ticker`.
  - "other" is the rest: status bar, HUD, wipes and `D_Display` glue.
- **Cycles** come from the simulator's model: 1 per instruction plus 2 per taken branch. The model has no dual issue,
  no cache and no memory wait states. Use cycles only to compare variants; they do not predict hardware time. Instruction
  counts are exact.
- **Correctness:** every run below reproduced the host goldens bit-exactly, frame for frame (CRC column).

## Per-frame cost

| scenario | frames | insts/frame | cycles/frame | median | p90 | max | render | game | other | CRC |
|---|---|---|---|---|---|---|---|---|---|---|
| e1m1 | 899 | 2017661 | 2219747 | 2065491 | 3178318 | 4881056 | 93.0% | 3.6% | 3.4% | ok |
| e1m2 | 899 | 2140728 | 2374393 | 2184409 | 4124084 | 11644148 | 90.6% | 3.4% | 6.0% | ok |
| e1m3 | 939 (+1 load) | 1617624 | 1774804 | 1704510 | 2726020 | 3809303 | 92.6% | 4.1% | 3.4% | ok |
| e2m2 | 599 | 1634253 | 1832426 | 404222 | 6275031 | 13078414 | 88.2% | 2.2% | 9.6% | ok |
| e3m4 | 599 | 1883890 | 2070369 | 1989240 | 2865905 | 3541052 | 91.8% | 4.9% | 3.2% | ok |
| demo1 | 1999 | 4044224 | 4463376 | 4372958 | 4962282 | 6573489 | 96.8% | 2.0% | 1.2% | ok |
| map01 | 899 | 1715048 | 1878984 | 1554127 | 3079080 | 5636023 | 93.3% | 3.2% | 3.5% | ok |
| map07 | 699 | 1792072 | 1982551 | 2132925 | 3283456 | 10112355 | 87.3% | 4.6% | 8.0% | ok |
| demo2-1 | 2703 (+1 load) | 2868025 | 3174992 | 3440431 | 4878038 | 8116912 | 95.9% | 2.2% | 2.0% | ok |

- The median, p90 and max columns are per-frame cycles.
- The render, game and other columns are shares of the average cycles per frame.
- The noclip scenarios (e1m2, e2m2, map07) fly through walls and into the void, so their spread is very wide.
- **e1m1, map01 and the two demos are the most representative.** Their averages are 1.7–4.0 M instructions per frame.
  - At 118 MHz and an idealised 1 instruction per cycle, 2 M instructions would take 17 ms. That figure is an illustration
    only, not a prediction.
- demo1 is the heaviest. Sprites weigh much more in it: `R_DrawMaskedColumn` alone is 10% of its cycles.

## After the engine C optimisations (steps 1–3, 2026-10-10)

Same method, scenarios and window as above, measured on a **C-only** sim ELF (no `src/engine/sh4` code:
`make SIM_BUILD=<dir> CGSIM_ASM_SRC= 'CGSIM_C_SRC=$(ENGINE_SRC) $(HOST_SRC)' sim`; since the SH4 integration the
recipe is `make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim`). At the time the default `make sim` did not use
`r_draw_sh4.S` yet. This is the C reference that the asm drawers will be compared against. Every run
reproduced its golden (the scenarios' frames changed in step 1, see `tests/golden/CHANGES.md`, so "before" and "after"
do not show exactly the same frames).

| scenario | insts/frame baseline | after steps 1–3 | Δ | cycles/frame baseline | after | Δ | median | p90 | max |
|---|---|---|---|---|---|---|---|---|---|
| e1m1 | 2,017,661 | 1,500,945 | −25.6% | 2,219,747 | 1,626,629 | −26.7% | 1,610,872 | 2,052,760 | 3,202,084 |
| e1m2 | 2,140,728 | 1,304,290 | −39.1% | 2,374,393 | 1,409,748 | −40.6% | 1,473,828 | 2,284,614 | 5,786,625 |
| e1m3 | 1,617,624 | 1,126,761 | −30.3% | 1,774,804 | 1,215,429 | −31.5% | 1,182,760 | 1,504,625 | 2,274,035 |
| e2m2 | 1,634,253 | 809,289 | −50.5% | 1,832,426 | 877,369 | −52.1% | 255,594 | 2,096,330 | 5,695,431 |
| e3m4 | 1,883,890 | 1,453,684 | −22.8% | 2,070,369 | 1,574,661 | −23.9% | 1,501,307 | 1,993,900 | 2,763,120 |
| demo1 | 4,044,224 | 2,378,752 | −41.2% | 4,463,376 | 2,576,746 | −42.3% | 2,420,336 | 3,097,378 | 3,763,886 |
| map01 | 1,715,048 | 1,301,013 | −24.1% | 1,878,984 | 1,402,326 | −25.4% | 1,233,544 | 1,981,308 | 4,958,559 |
| map07 | 1,792,072 | 1,138,143 | −36.5% | 1,982,551 | 1,228,089 | −38.1% | 1,324,961 | 1,944,779 | 5,884,052 |
| demo2-1 | 2,868,025 | 1,742,870 | −39.2% | 3,174,992 | 1,882,322 | −40.7% | 1,972,587 | 2,611,627 | 3,254,137 |

**Optional modes (step 3), instructions per frame** (same runs plus the mode flags; the game state is identical):

| scenario | default | `-lowdetail` | `-flatfloors` | `-viewsize 9` | `-viewsize 7` | low + flat |
|---|---|---|---|---|---|---|
| e1m1 | 1,500,945 | 933,620 (−37.8%) | 1,372,591 (−8.6%) | 1,248,763 (−16.8%) | 892,518 (−40.5%) | 845,930 (−43.6%) |
| map01 | 1,301,013 | 808,113 (−37.9%) | 1,140,916 (−12.3%) | 1,065,902 (−18.1%) | 740,353 (−43.1%) | 700,133 (−46.2%) |
| demo1 | 2,378,752 | 1,533,152 (−35.5%) | 2,016,194 (−15.2%) | 2,046,266 (−14.0%) | 1,541,322 (−35.2%) | 1,285,660 (−46.0%) |
| demo2-1 | 1,742,870 | 1,129,272 (−35.2%) | 1,386,669 (−20.4%) | 1,458,397 (−16.3%) | 1,056,937 (−39.4%) | 881,096 (−49.4%) |

- Cycles follow within 0.6 points.
- The e1m1 profile after step 3:
  - `R_DrawColumn` 53.0%;
  - `R_RenderSegLoop` 13.3%;
  - `R_DrawSpan` 7.7%;
  - `R_MakeSpans` 3.1%;
  - `R_DrawMaskedColumn` 3.1%;
  - libgcc division 4.2% (both symbols);
  - `R_StoreWallRange` 1.7%;
  - `V_DrawPatch` 1.7%.
- The low-detail gain is with the C drawers. The asm low-detail drawers (`src/engine/sh4/README.md`) model 2.25 (column)
  and 3.0 (span, 3.25 before sh4 round 3) cycles per pixel; measured with them: "After the SH4 drawers" below.
- Per-step details: `notes/engine-changes.md`.

**Mode runs by hand:** `src/sim/simrun --stats <elf> <scenario args> -lowdetail -simstats -simwin 42 0`. `perf-sim.sh`
only runs scenarios of `tests/scenarios.txt`; `e1m1-low`, `demo1-low`, `map01-flat` and `e1m1-view7` are scenarios,
so `-only e1m1-low` etc. work.

## After the SH4 drawers (2026-10-10)

The default `make sim` ELF now uses the SH4 assembly drawers of `src/engine/sh4` (column, span, low detail, fuzz and
translated), selected by `R_ExecuteSetViewSize` under `__sh__` with the rotated colormap registered in
`R_InitColormaps`, exactly as in the calculator build (sim default: `-DCGD_SH4_NO_ILRAM`, see `src/sim/cgdoom.mk`).
"C" is the same sources built with `CGSIM_SH4=0` (`-DCGD_NO_SH4`): `make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim`.
Same method, scenarios and window as above. Every run reproduced its golden (CRC column), and the 24 mode runs below
render the same frames with both ELFs (`test-sim-full`: 35/35 with the SH4 drawers, in both section layouts).

**Instructions and simulator cycles per frame:**

| scenario | insts C | insts SH4 | Δ | cycles C | cycles SH4 | Δ | median | p90 | max | render | CRC |
|---|---|---|---|---|---|---|---|---|---|---|---|
| e1m1 | 1,500,989 | 1,144,003 | −23.8% | 1,626,632 | 1,210,398 | −25.6% | 1,231,227 | 1,571,040 | 2,132,818 | 92.7% | ok |
| e1m2 | 1,304,433 | 1,031,408 | −20.9% | 1,409,880 | 1,097,726 | −22.1% | 1,063,873 | 1,844,643 | 5,521,418 | 85.6% | ok |
| e1m3 | 1,126,823 | 804,865 | −28.6% | 1,215,457 | 839,984 | −30.9% | 796,401 | 1,104,274 | 1,772,280 | 89.7% | ok |
| e2m2 | 809,438 | 695,892 | −14.0% | 877,509 | 746,060 | −15.0% | 218,823 | 1,881,730 | 5,605,628 | 79.7% | ok |
| e3m4 | 1,453,725 | 1,103,147 | −24.1% | 1,574,660 | 1,166,062 | −25.9% | 1,105,833 | 1,567,801 | 1,876,201 | 89.7% | ok |
| demo1 | 2,378,934 | 2,026,047 | −14.8% | 2,576,928 | 2,174,384 | −15.6% | 2,022,914 | 2,688,759 | 3,359,539 | 95.7% | ok |
| map01 | 1,301,064 | 942,498 | −27.6% | 1,402,337 | 986,113 | −29.7% | 838,344 | 1,518,677 | 3,188,528 | 91.8% | ok |
| map07 | 1,138,290 | 889,025 | −21.9% | 1,228,227 | 944,578 | −23.1% | 1,015,819 | 1,484,178 | 5,384,986 | 80.7% | ok |
| demo2-1 | 1,743,049 | 1,392,076 | −20.1% | 1,882,501 | 1,477,970 | −21.5% | 1,559,819 | 2,183,328 | 2,853,431 | 94.4% | ok |

(median/p90/max/render: SH4 ELF, cycles. The C column differs from the table above by +44 to +185 instructions per
frame: engine changes since then, e.g. the demo-recording flag; frames are unchanged.)

**Optional modes, SH4 instructions per frame** (Δ against the C ELF in the same mode):

| scenario | default | `-lowdetail` | `-flatfloors` | `-viewsize 9` | `-viewsize 7` | low + flat |
|---|---|---|---|---|---|---|
| e1m1 | 1,144,003 (−23.8%) | 710,200 (−23.9%) | 1,069,311 (−22.1%) | 979,224 (−21.6%) | 734,850 (−17.7%) | 668,637 (−21.0%) |
| map01 | 942,498 (−27.6%) | 579,296 (−28.3%) | 851,541 (−25.4%) | 794,143 (−25.5%) | 580,330 (−21.6%) | 530,669 (−24.2%) |
| demo1 | 2,026,047 (−14.8%) | 1,276,435 (−16.8%) | 1,829,874 (−9.2%) | 1,780,315 (−13.0%) | 1,387,300 (−10.0%) | 1,172,023 (−8.9%) |
| demo2-1 | 1,392,076 (−20.1%) | 886,692 (−21.5%) | 1,172,088 (−15.5%) | 1,195,876 (−18.0%) | 906,742 (−14.2%) | 754,769 (−14.4%) |

Against the SH4 default, the modes save: low detail 36–39%, potato floors 6.5–16%, view size 9 12–16%, view size 7
31–38%, low + flat 42–46%. Cycles drop 0.2–2.1 points more than instructions (the repeat loops have no taken branch).

**Read these as a lower bound of the gain.** The simulator charges 1 cycle per instruction (+2 per taken branch), so it
sees only the drawers' instruction count (1.6–2.4× fewer per pixel, src/engine/sh4/README.md §4), not their dual
issue. The drawer loops were scheduled for SH-4A pairing: about 4.0 c/px for columns against 14 for the C loop.

**Modelled hardware cycles of the drawers** (src/engine/sh4/README.md §3 per-call models, pess / opt / gcc,
applied to the exact call, pixel and loop-iteration counts of the PC profiles; the C side uses the README's model
of the GCC -O2 C drawers; count < 0 calls at 12 cycles; no cache misses):

| scenario | drawer calls/frame | C drawers (pess / opt / gcc) | SH4 drawers | speed-up | frame estimate C → SH4 (pess) |
|---|---|---|---|---|---|
| e1m1 | 873 col + 101 span | 937k / 748k / 1000k | 294k / 292k / 352k | 3.2× / 2.6× / 2.8× | 1.57M → 0.93M (1.69×) |
| map01 | 630 col + 101 span | 912k / 730k / 973k | 284k / 281k / 338k | 3.2× / 2.6× / 2.9× | 1.35M → 0.72M (1.87×) |
| demo1 | 3863 col + 126 span | 1146k / 940k / 1218k | 423k / 411k / 473k | 2.7× / 2.3× / 2.6× | 2.57M → 1.84M (1.39×) |
| e1m1-low | 436 col + 97 span | 593k / 491k / 636k | 168k / 168k / 185k | 3.5× / 2.9× / 3.4× | 1.02M → 0.60M (1.71×) |
| demo1-low | 1968 col + 124 span | 757k / 632k / 826k | 238k / 234k / 259k | 3.2× / 2.7× / 3.2× | 1.70M → 1.18M (1.44×) |

- Column-call mix (SH4 profile counters, per frame): e1m1 603 multi-pixel + 19 two-pixel + 21 one-pixel + 229 with
  count < 0 (vanilla's unconditional mid-texture call, R_RenderSegLoop; the asm hands them to its C fallback, which
  returns at once); demo1 1440 multi + 458 two + 1909 one + 57 negative. The fuzz/translated drawers are not in the
  model (rare).
- The frame estimate adds the modelled drawers to the rest of the frame at the simulator's 1 cycle per instruction
  (identical in both ELFs: e1m1 637k, map01 439k, demo1 1.42M cycles). That part dual-issues too on hardware but also
  misses caches, so treat the frame figures as an illustration: at 118 MHz, e1m1 goes from ≈13.3 to ≈7.9 ms per frame
  of engine work (presentation not included).
- After the SH4 drawers the drawer loops are no longer the bulk of the model frame: e1m1's profile (sim cycles) is
  `cgd_col_rs` (column loop) 36.2%, `R_RenderSegLoop` 17.8%, `cgd_span_rs` 4.7%, `R_MakeSpans` 4.2%,
  `R_DrawMaskedColumn` 4.2%, column set-up 3.3%, libgcc division 5.6%; demo1: `R_RenderSegLoop` 16.4%, `cgd_col_rs`
  14.4%, `R_DrawMaskedColumn` 13.0%, `cgd_span_rs` 7.8%, column set-up and 1-pixel path 7.8%. In the sim those loops
  still count 1 cycle per instruction; on hardware the C glue (`R_RenderSegLoop`, `R_DrawMaskedColumn`,
  `R_MakeSpans`) becomes the larger share.

## After the renderer polish (2026-10-10)

Two bit-exact C changes on top of "After the SH4 drawers" (`notes/engine-changes.md` "Renderer polish"):
- **`R_RenderSegLoop` skips empty mid-texture columns.** Vanilla called `R_GetColumn` + `colfunc` even when
  `yh < yl` (the drawer returns at once; the SH4 drawers via their C fallback). The light lookup, `dc_x` and the
  `dc_iscale` division now also run only before the first tier a column actually draws, so a column that draws
  nothing costs no division either.
- **`R_DrawMaskedColumn` keeps its loop state in locals** (`sprtopscreen`, `spryscale`, the two clip values,
  `colfunc`), as step 2 did for `R_RenderSegLoop`; `dc_yl`/`dc_yh`/`dc_source`/`dc_texturemid` are stored only
  for the posts that are drawn.

Same method, scenarios and window as above, default `make sim` ELF (SH4 drawers, `.text` layout); "before" is the
ELF of "After the SH4 drawers" (its numbers reproduce that table exactly). Every run reproduced its golden.

| scenario | insts before | insts after | Δ | cycles before | cycles after | Δ | CRC |
|---|---|---|---|---|---|---|---|
| e1m1 | 1,144,003 | 1,077,734 | −5.8% | 1,210,398 | 1,135,015 | −6.2% | ok |
| e1m2 | 1,031,408 | 1,018,188 | −1.3% | 1,097,726 | 1,082,460 | −1.4% | ok |
| e1m3 | 804,865 | 797,327 | −0.9% | 839,984 | 831,805 | −1.0% | ok |
| e2m2 | 695,892 | 690,644 | −0.8% | 746,060 | 739,704 | −0.9% | ok |
| e3m4 | 1,103,147 | 1,062,941 | −3.6% | 1,166,062 | 1,120,562 | −3.9% | ok |
| demo1 | 2,026,047 | 1,934,915 | −4.5% | 2,174,384 | 2,072,292 | −4.7% | ok |
| map01 | 942,498 | 934,689 | −0.8% | 986,113 | 977,446 | −0.9% | ok |
| map07 | 889,025 | 880,804 | −0.9% | 944,578 | 934,532 | −1.1% | ok |
| demo2-1 | 1,392,076 | 1,367,712 | −1.8% | 1,477,970 | 1,449,898 | −1.9% | ok |
| e1m1-low | 710,200 | 675,813 | −4.8% | 753,230 | 714,386 | −5.2% | ok |
| e3m4-low | 705,416 | 684,502 | −3.0% | 749,890 | 726,389 | −3.1% | ok |
| demo1-low | 1,276,435 | 1,229,563 | −3.7% | 1,373,332 | 1,320,909 | −3.8% | ok |
| map07-low | 615,996 | 611,749 | −0.7% | 658,314 | 653,156 | −0.8% | ok |

(insts and cycles per frame; the low-detail runs are `perf-sim.sh -only e1m1-low -only e3m4-low -only demo1-low
-only map07-low`.)

**Where it comes from** (PC profiles, instructions per frame, before → after):

| scenario | column calls | `R_RenderSegLoop` (incl. inlined `R_GetColumn`) | libgcc `__udivsi3_i4i` | column set-up + C fallback | `R_DrawMaskedColumn` |
|---|---|---|---|---|---|
| e1m1 | 872 → 643 | 190.0k → 166.5k | 34.0k → 22.3k | 53.2k → 43.8k | 48.0k → 42.9k |
| map01 | 629 → 609 | 124.1k → 122.1k | 19.7k → 17.2k | 43.3k → 42.4k | 18.7k → 18.2k |
| demo1 | 3863 → 3806 | 319.7k → 309.3k | 75.3k → 64.8k | 160.0k → 157.7k | 258.7k → 204.4k (−21%) |
| e1m1-low | 436 → 321 | 98.5k → 86.6k | 22.2k → 16.4k | 31.7k → 26.0k | 24.0k → 21.4k |
| demo1-low | 1967 → 1938 | 168.9k → 163.4k | 53.3k → 48.0k | 92.7k → 91.2k | 132.0k → 104.2k (−21%) |

- The column loops (`cgd_col_rs` etc.) execute exactly the same instructions: only empty calls were removed.
- After the change no profile has a single `cgd_sh4_slow_column[_low]` call: no call site passes a negative count
  any more, so the SH4 drawers' count < 0 path (count check → C fallback → return) was left as it is rather than
  given an early exit that would lengthen every real call's set-up (`src/engine/sh4/README.md` §7.4).
- On hardware the saving is larger than the instruction share suggests for `R_RenderSegLoop`: each skipped column
  also saves an indirect call, the drawer's register saves and a 32-bit division (≈33 instructions, several dozen
  cycles on the SH-4A, which has no integer divide instruction).

## Where the time goes

The figures below are exact PC profiles over the same windows, excluding the CRC work and level loads.

**Shares of cycles by group:**

| group | e1m1 | map01 | demo1 |
|---|---|---|---|
| `R_DrawColumn` + `R_DrawSpan` (drawers) | 44.2% | 51.2% | 27.7% |
| `FixedMul` + `FixedDiv` (CGDoom's bit-serial versions) | 22.0% | 18.4% | 26.6% |
| libgcc division (`__udivsi3_i4i`, `__sdivsi3_i4i`) | 7.7% | 5.8% | 10.8% |
| other `R_*` / `V_*` in the top 25 | 18.4% | 17.5% | 29.1% |

**Top functions** (`simprof -n 25`; the full lists are in `build-sim/perf/<name>.top`):

```
e1m1 (899 frames)                          map01 (899 frames)                         demo1 (1999 frames)
37.5%  R_DrawColumn                        42.8%  R_DrawColumn                        24.3%  FixedMul
20.4%  FixedMul                            17.3%  FixedMul                            16.1%  R_DrawColumn
 7.9%  R_RenderSegLoop                      8.4%  R_DrawSpan                          11.6%  R_DrawSpan
 6.7%  R_DrawSpan                           7.0%  R_RenderSegLoop                      9.9%  R_DrawMaskedColumn
 5.4%  __udivsi3_i4i                        4.2%  __udivsi3_i4i                        7.8%  R_RenderSegLoop
 2.4%  R_MakeSpans                          3.6%  R_MakeSpans                          7.6%  __udivsi3_i4i
 2.3%  __sdivsi3_i4i                        1.6%  __sdivsi3_i4i                        3.2%  __sdivsi3_i4i
 1.8%  R_DrawMaskedColumn                   1.4%  R_GetColumn                          2.3%  FixedDiv
 1.6%  FixedDiv                             1.3%  R_DrawPlanes                         2.3%  R_MakeSpans
 1.4%  R_GetColumn                          1.3%  V_DrawPatch                          1.9%  R_GetColumn
 1.3%  V_DrawPatch                          1.1%  FixedDiv                             1.7%  R_DrawSprite
 0.9%  R_DrawPlanes                         1.0%  R_DrawMaskedColumn                   1.0%  R_StoreWallRange
 0.8%  memcpy (fxlibc byte loop)            0.9%  memcpy (fxlibc byte loop)            1.0%  R_DrawPlanes
 0.7%  W_CacheLumpNumConst                  0.7%  W_CacheLumpNumConst                  0.6%  R_PointToAngle
 0.7%  R_MapPlane                           0.7%  R_MapPlane                           0.6%  R_MapPlane
```

> **Correction (step 2, `notes/engine-changes.md`):** the `__sdivsi3_i4i` lines below are mostly **unsigned** division.
> In libgcc's SH code, `__udivsi3_i4i` branches into code that lies after the `__sdivsi3_i4i` symbol, and `simprof`
> charges those instructions to `__sdivsi3_i4i`. Signed divisions were about 190 calls per frame at about 20
> instructions each; 154 of them came from `P_UpdateSpecials`. The real cost is one unsigned division of about 48
> instructions, about 1,200 calls per frame in e1m1: `dc_iscale` in `R_RenderSegLoop` and `SlopeDiv`. The libgcc
> division groups should be read as one group.

**Calls per frame and cost per call:** the call count is the number of times the function's first instruction ran.

| function | e1m1 calls/frame | insts/call | map01 calls/frame | insts/call | demo1 calls/frame | insts/call |
|---|---|---|---|---|---|---|
| `FixedMul` | 1814 | 216 | 1300 | 216 | 4291 | 219 |
| `FixedDiv` | 147 | 202 | 90 | 203 | 438 | 203 |
| `__udivsi3_i4i` | 3357 | 33 | 2104 | 34 | 9654 | 32 |
| `__sdivsi3_i4i` | 184 | 261 | 175 | 166 | 236 | 580 |
| `R_DrawColumn` | 788 | 989 | 630 | 1194 | 4326 | 157 |
| `R_DrawSpan` | 116 | 1218 | 101 | 1507 | 183 | 2705 |
| `R_RenderSegLoop` | 29 | 5452 | 19 | 6270 | 88 | 3555 |
| `R_GetColumn` | 584 | 51 | 477 | 51 | 1522 | 53 |
| `W_CacheLumpNumConst` | 473 | 32 | 397 | 32 | 714 | 32 |
| `KeyDown` (cgdoom-kbd) | 78 | 127 | 78 | 127 | 78 | 127 |

The figures point to these optimisations, in order of payoff:

1. **P1:** an inline `dmuls.l` `FixedMul` would remove about 0.4 M instructions per frame in e1m1 and 0.94 M in demo1.
   `FixedDiv` is about 200 instructions per call.
2. **P3:** rework the column and span drawers.
3. **P4:** cut the libgcc divisions (6–11% of cycles).
   - `__sdivsi3_i4i` takes 160–580 instructions per call, against about 33 for `__udivsi3_i4i`.
   - The callers are not identified yet; the profile is flat and has no call graph.
4. **Smaller items:**
   - `KeyDown` costs about 10 K instructions per frame for keyboard polling.
   - fxlibc `memcpy` copies about 2.8 K bytes per frame through its byte loop.

## How to reproduce

```sh
cd /Users/cheparukhin/casio_doom
make sim-tool        # once: builds build-sim/tools/sh-elf-run64 (64-bit counters, PC profile)
make sim             # build-sim/cgdoom-sim.elf (SH4 drawers; C-only: make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim)
make test-sim        # quick check: first 60 tics of e1m1, map01 and demo1 match the goldens (~6 s)
make test-sim-full   # all 35 scenarios in the simulator vs the goldens (~245 s)
make perf-sim        # this baseline: prints the tables (~90 s, 4 parallel runs); logs/profiles in build-sim/perf/
```

**Single runs by hand:**

```sh
src/sim/simrun --stats build-sim/cgdoom-sim.elf -iwad test-wads/fd/freedoom-0.13.0/freedoom1.wad \
    -warp 1 1 -tics 900 -input builtin:explore -crc /tmp/e1m1.crc -simstats -simwin 42 0
src/sim/simrun --pcprof /tmp/e1m1.prof build-sim/cgdoom-sim.elf <same args> -simwin 42 0
src/sim/simprof -n 40 build-sim/cgdoom-sim.elf /tmp/e1m1.prof
src/sim/simprof --disasm R_DrawColumn build-sim/cgdoom-sim.elf /tmp/e1m1.prof
```

**Options handled by `src/sim/cgdoom-hooks.c`** (removed from argv before the host's `main`):

| option | effect |
|---|---|
| `-simwin A B` | frame window `[A, B)`; `B = 0` means to the end. The PC profile is paused outside it. |
| `-simstats` | one `SIMFRAMES key=value ...` line at exit |
| `-simmarks` | one `SIMMARK <frame> <insts> <cycles>` line per frame, for cost curves |

`perf-sim.sh` options:
- `-only NAME` (repeatable) picks the scenarios.
- `-prof NAME` picks the scenarios to profile.
- `-from F` sets the first frame of the window.

All numbers are deterministic, so reruns give the same values to the instruction.

## After the demo sync (2026-10-10)

Measured with `make perf-sim` on the default ELF after the demo-sync work (`notes/engine-changes.md` "Demo sync" and
"Demo sync review"). The renderer is unchanged. The numbers move because the game logic now matches vanilla
(restored sound-related `P_Random` calls, the vanilla `P_LookForPlayers` loop, nightmare tables copied to RAM, a range
check per state change), so monsters behave differently and the scripted runs and demos see different scenes; demo1
now plays the real recording to the end instead of desyncing. Compare with the "After the renderer polish" table.

| scenario | frames | insts/frame | cycles/frame | before (insts) | Δ insts | CRC |
|---|---|---|---|---|---|---|
| e1m1 | 899 | 1,116,529 | 1,176,499 | 1,077,734 | +3.6% | ok |
| e1m2 | 899 | 1,020,992 | 1,085,524 | 1,018,188 | +0.3% | ok |
| e1m3 | 931 (+1 load) | 843,734 | 881,521 | 797,327 | +5.8% | ok |
| e2m2 | 599 | 691,373 | 740,555 | 690,644 | +0.1% | ok |
| e3m4 | 599 | 1,086,994 | 1,146,170 | 1,062,941 | +2.3% | ok |
| demo1 | 1999 | 1,701,884 | 1,814,625 | 1,934,915 | −12.0% | ok |
| map01 | 899 | 1,002,011 | 1,050,404 | 934,689 | +7.2% | ok |
| map07 | 699 | 882,213 | 936,090 | 880,804 | +0.2% | ok |
| demo2-1 | 2663 | 1,677,914 | 1,782,104 | 1,367,712 | +22.7% | ok |

demo1 and demo2-1 are different scenes than before (the frame counts changed too), so their deltas are not a
cost change of the code. Per-frame cost of the frame window (frames 42..end, no level-load frames) as above.
