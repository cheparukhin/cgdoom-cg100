# Performance baseline: the engine in the SH simulator

These are the per-frame costs of the engine as it is now, compared with the CGDoom engine this port started from,
measured with `make perf-sim`. Every number is deterministic, so a rerun on the same sources with the same compiler
gives the same values to the instruction.

## What is measured

- **Build:** `make sim` builds `build-sim/cgdoom-sim.elf` from the same engine and host sources as `make host`, plus
  the SH4 assembly drawers of `src/engine/sh4`, exactly as the calculator build selects them. The compiler is
  `sh-elf-gcc 16.2 -m4-nofpu -mb -O2 -std=gnu11 -fno-strict-aliasing -DPL_SIM`, with RANGECHECK off as on the
  calculator.
- **Simulator:** `build-sim/tools/sh-elf-run64` (from `make sim-tool`): the GDB 15.2 SH simulator with 64-bit
  counters and an exact per-PC profile.
- **Game settings:** view size 10 (a 320x168 view with the status bar), full detail, `-frameskip 0`.
- **Window:** frames 42 to the end of each scenario of `tests/scenarios.txt`. Frames 0–41 are the level-start screen
  wipe, and the level loads before frame 0. A frame in which `P_SetupLevel` ran is counted as "load" and left out
  (once in e1m3, where the player dies and the level restarts).
- **Frame cost** is everything between two `PL_FinishUpdate` calls: game tics, rendering, status bar and menus. The
  work inside `PL_FinishUpdate` (the test platform's CRC, about 0.78 M instructions per frame, which does not exist on
  the calculator) is left out, and so is presentation. "render" is `R_RenderPlayerView`, "game" is `G_Ticker`, and
  "other" is the rest: status bar, HUD, wipes and `D_Display` glue.
- **Cycles** come from the simulator's model: 1 per instruction plus 2 per taken branch. It has no dual issue, no
  cache and no memory wait states. Use cycles only to compare variants of the same code; they do not predict
  hardware time. Instruction counts are exact.
- **Correctness:** every run reproduced its host golden frame for frame (CRC column). The mode runs that have no
  golden of their own were checked against a `build-host/cgdoom-host` run with the same arguments.

## Against the CGDoom engine

"CGDoom (upstream)" is the starting point, measured with the same method (and sh-elf-gcc 14.1): CGDoom's C renderer
and its bit-serial `FixedMul`/`FixedDiv`, no assembly. "Now" is the current default ELF.

| scenario | insts/frame CGDoom (upstream) | insts/frame now | Δ | cycles/frame CGDoom (upstream) | cycles/frame now | Δ |
|---|---|---|---|---|---|---|
| e1m1 | 2,017,661 | 1,031,352 | −48.9% | 2,219,747 | 1,084,797 | −51.1% |
| e1m2 | 2,140,728 | 951,190 | −55.6% | 2,374,393 | 1,009,514 | −57.5% |
| e1m3 | 1,617,624 | 787,775 | −51.3% | 1,774,804 | 818,956 | −53.9% |
| e2m2 | 1,634,253 | 642,000 | −60.7% | 1,832,426 | 687,893 | −62.5% |
| e3m4 | 1,883,890 | 1,012,496 | −46.3% | 2,070,369 | 1,064,501 | −48.6% |
| demo1 | 4,044,224 | 1,602,103 | −60.4% | 4,463,376 | 1,704,685 | −61.8% |
| map01 | 1,715,048 | 908,500 | −47.0% | 1,878,984 | 944,300 | −49.7% |
| map07 | 1,792,072 | 822,130 | −54.1% | 1,982,551 | 871,342 | −56.0% |
| demo2-1 | 2,868,025 | 1,562,152 | −45.5% | 3,174,992 | 1,650,241 | −48.0% |

The two columns do not show exactly the same frames. The engine now follows vanilla's fixed-point arithmetic and game
logic, so monsters act differently and the scripted runs see slightly different scenes. The demos changed most: under
CGDoom they desynced (demo2-1 even reached a level change), and now they play the real recordings. e1m1 and map01 are
the closest like-for-like comparisons.

## Per-frame cost now

| scenario | frames | insts/frame | cycles/frame | median | p90 | max | render | game | other | CRC |
|---|---|---|---|---|---|---|---|---|---|---|
| e1m1 | 899 | 1,031,352 | 1,084,797 | 990,644 | 1,585,909 | 2,801,926 | 95.2% | 3.6% | 1.2% | ok |
| e1m2 | 899 | 951,190 | 1,009,514 | 974,757 | 1,678,067 | 5,501,627 | 87.8% | 6.1% | 6.2% | ok |
| e1m3 | 931 (+1 load) | 787,775 | 818,956 | 775,654 | 1,101,516 | 1,870,272 | 92.5% | 5.7% | 1.8% | ok |
| e2m2 | 599 | 642,000 | 687,893 | 172,779 | 1,837,013 | 5,580,903 | 83.4% | 3.8% | 12.8% | ok |
| e3m4 | 599 | 1,012,496 | 1,064,501 | 1,021,554 | 1,460,152 | 1,762,813 | 92.4% | 6.5% | 1.1% | ok |
| demo1 | 1999 | 1,602,103 | 1,704,685 | 1,584,337 | 2,496,486 | 2,931,113 | 92.8% | 6.4% | 0.8% | ok |
| map01 | 899 | 908,500 | 944,300 | 937,035 | 1,321,814 | 3,284,198 | 95.5% | 3.2% | 1.4% | ok |
| map07 | 699 | 822,130 | 871,342 | 943,553 | 1,383,982 | 5,260,376 | 83.3% | 7.9% | 8.9% | ok |
| demo2-1 | 2663 | 1,562,152 | 1,650,241 | 1,638,007 | 2,298,424 | 3,019,250 | 96.7% | 2.3% | 1.0% | ok |

Median, p90 and max are per-frame cycles; render, game and other are shares of the average cycles per frame. The
noclip scenarios (e1m2, e2m2, map07) fly through walls and into the void, so their spread is very wide. **e1m1, map01
and the two demos are the most representative**; demo1 is the heaviest, with the most sprites.

## Optional modes

Instructions per frame, same window, with `-lowdetail` (160 columns), `-viewsize 9` (288x144) or `-viewsize 7`
(224x112). The game state is the same as in the default run; only the rendering changes.

| scenario | default | low detail | view size 9 | view size 7 |
|---|---|---|---|---|
| e1m1 | 1,031,352 | 640,008 (−37.9%) | 870,777 (−15.6%) | 639,756 (−38.0%) |
| map01 | 908,500 | 554,010 (−39.0%) | 755,166 (−16.9%) | 541,280 (−40.4%) |
| demo1 | 1,602,103 | 1,047,766 (−34.6%) | 1,391,350 (−13.2%) | 1,075,006 (−32.9%) |
| demo2-1 | 1,562,152 | 1,044,974 (−33.1%) | 1,342,088 (−14.1%) | 1,023,725 (−34.5%) |

Low detail saves 33–39%, view size 9 saves 13–17% and view size 7 saves 33–40%. Cycles drop by about the same
amount (within 0.9 points). Game tics and the status bar cost the same in every mode; only rendering gets cheaper.

## Screen formats

The same measure in the three builds of `src/platform/cgd_config.h` (DEVELOPMENT.md §2.7), each with its SH4 drawers,
the HD build with its extras on as shipped: `make perf-sim`, `make VARIANT=wide perf-sim` and `make VARIANT=hd
perf-sim`. Every run reproduced its goldens (`tests/golden`, `tests/golden-wide`, `tests/golden-hd`).

| scenario | input | 320x200 | 396x224 | HD (396x224, extras) |
|---|---|---|---|---|
| e1m1 | explore | 1,031,352 | 1,322,924 (+28.3%) | 1,321,917 (+28.2%) |
| e1m2 | noclip | 951,190 | 1,209,007 (+27.1%) | 1,536,533 (+61.5%) |
| e1m3 | walk | 787,775 | 1,031,878 (+31.0%) | 1,024,929 (+30.1%) |
| e2m2 | noclip | 642,000 | 764,217 (+19.0%) | 882,285 (+37.4%) |
| e3m4 | explore | 1,012,496 | 1,298,872 (+28.3%) | 1,294,682 (+27.9%) |
| demo1 | demo | 1,602,103 | 1,988,468 (+24.1%) | 1,980,719 (+23.6%) |
| map01 | explore | 908,500 | 1,195,268 (+31.6%) | 1,177,584 (+29.6%) |
| map07 | noclip | 822,130 | 1,041,074 (+26.6%) | 1,196,168 (+45.5%) |
| demo2-1 | demo | 1,562,152 | 1,959,905 (+25.5%) | 1,926,687 (+23.3%) |

- **Widescreen:** the view has 41% more pixels (396x192 against 320x168); the frame costs 19–32% more. The presenter
  also expands 39% more pixels for the DMA (not counted here).
- **The extras** cost between −1.7% and −0.1% of the 396x224 frame in the other scenarios: the 4:3 proportions, the
  coloured numbers and the crosshair draw no more pixels, and the full-screen HUD shows only at the largest view
  size (there it costs 1.4–2.7%).
- **The `noclip` scenarios are not like for like:** their script presses the Map key twice, which in the HD build
  leaves the automap overlay on (the 3D view plus the map's drawn lines, rotated each frame), where the other builds
  show the map alone.

## Where the time goes

Shares of simulator cycles from the PC profiles (`build-sim/perf/<name>.top` has the top 25 functions):

| group | e1m1 | map01 | demo1 |
|---|---|---|---|
| column drawer (`R_DrawColumn_SH4`, `cgd_col_*`) | 47.8% | 45.7% | 29.1% |
| `R_RenderSegLoop` (with the inlined `R_GetColumn`) | 16.8% | 17.3% | 14.7% |
| planes (`R_MakeSpans`, `R_DrawPlanes`, `R_MapPlane`, ...) | 5.5% | 8.0% | 7.5% |
| span drawer (`R_DrawSpan_SH4`, `cgd_span_*`) | 5.8% | 13.6% | 13.0% |
| sprites and masked walls (`R_DrawMaskedColumn`, `R_DrawSprite`, ...) | 7.8% | 2.9% | 13.6% |
| BSP walk and wall set-up (`R_StoreWallRange`, `R_AddLine`, `FixedDiv`, ...) | 7.8% | 3.9% | 10.4% |
| libgcc division | 3.6% | 3.8% | 4.5% |
| `V_*` (status bar, HUD) | 0.4% | 0.5% | 0.5% |

The simulator charges the drawers' repeat loops exactly 1 cycle per instruction (they have no taken branch), so this
table overstates the drawers' share of hardware time; their speed comes from dual issue.

**Modelled hardware cycles of the drawers.** These apply the per-call models of
[`src/engine/sh4/README.md` §3](../src/engine/sh4/README.md#3-pipeline-model-and-loop-schedules) (pess / opt / gcc,
no cache misses) to the exact call, pixel and loop-iteration counts of the profiles. The C side applies the same
README's model of the GCC -O2 C drawers to the same pixels. Per frame:

| scenario | column calls (multi / 2 px / 1 px) | span calls | SH4 drawers (pess / opt / gcc) | C drawers (pess / opt / gcc) | speed-up (pess / opt / gcc) | frame estimate C → SH4 (pess) |
|---|---|---|---|---|---|---|
| e1m1 | 642 (600 / 19 / 23) | 99 | 303k / 301k / 364k | 969k / 772k / 1035k | 3.2× / 2.6× / 2.8× | 1.47M → 0.81M (1.83×) |
| map01 | 709 (695 / 8 / 7) | 145 | 295k / 292k / 344k | 935k / 755k / 995k | 3.2× / 2.6× / 2.9× | 1.32M → 0.68M (1.94×) |
| demo1 | 2626 (1109 / 431 / 1086) | 246 | 398k / 389k / 445k | 1130k / 928k / 1199k | 2.8× / 2.4× / 2.7× | 2.12M → 1.38M (1.53×) |
| e1m1-low | 320 (300 / 9 / 11) | 95 | 174k / 173k / 191k | 611k / 505k / 654k | 3.5× / 2.9× / 3.4× | 0.96M → 0.52M (1.84×) |
| demo1-low | 1312 (554 / 215 / 543) | 241 | 230k / 227k / 248k | 767k / 640k / 840k | 3.3× / 2.8× / 3.4× | 1.48M → 0.94M (1.57×) |

- No column or span call takes the C fallback (count out of range or an unregistered colormap), and the fuzz and
  translated drawers run less than once per 100 frames. In low detail the counts are texels, which are 2 pixels wide.
- Wall-heavy scenes draw long columns (e1m1: about 92 pixels per call). In demo1, 58% of the column calls are sprite
  posts of 1 or 2 pixels, so per-call set-up weighs more there and the speed-up is smaller.
- The frame estimate adds the modelled drawers to the rest of the frame, counted at the simulator's cycles (e1m1
  503k, map01 384k, demo1 987k, e1m1-low 346k, demo1-low 709k). The rest is the same code in both builds. On hardware
  it dual-issues too, but it also misses the caches and waits on memory, so the estimate is only an illustration. At
  the game's F4 CPU clock (235.93 MHz), e1m1's 0.81M cycles would be 3.4 ms of engine work per frame, as an idealised
  lower bound that leaves out presentation.

## Profiler artefacts

- **libgcc division is reported under the wrong symbol.** In libgcc's SH code, `__udivsi3_i4i` branches into code
  that lies after the `__sdivsi3_i4i` symbol (at `+0x2c` and `+0x52`). `simprof` attributes by symbol, so most
  unsigned-division work shows up as `__sdivsi3_i4i`. Read the two lines as one libgcc division group. Almost all of
  it is unsigned: about 46–47 instructions per call and about 800 calls per frame in e1m1 (1,500 in demo1), mostly
  `dc_iscale = 0xffffffffu / scale` in `R_RenderSegLoop` and `SlopeDiv` under `R_PointToAngle`. Signed divisions
  enter `__sdivsi3_i4i` about 3 times per frame.
- **The profile is flat: it has no call graph.** A function's call count is the execution count of its first
  instruction. To count calls per call site, count the `jsr` instruction in the per-PC profile and resolve its target
  from the preceding `mov.l ... ! <sym>` in `sh-elf-objdump -d`.
- **The asm drawers' local labels appear as functions.** `simprof` lists `R_DrawColumn_SH4` (the dispatch head),
  `cgd_col_disp` (set-up), `cgd_col_rs` (the loop), `cgd_col_end` (epilogue) and `cgd_col_one` (the 1-pixel path)
  separately, and the same goes for `cgd_coll_*`, `cgd_span_*` and `cgd_spanl_*`.

## Remaining levers

None of these is done; the figures are for the current code.

1. **Register-argument entry points for the column drawers**
   ([`src/engine/sh4/README.md` §9](../src/engine/sh4/README.md#9-further-work-not-done)). The callers already hold
   the values in registers, so set-up drops by about 40 modelled cycles per multi-pixel call and about 20 per 1-pixel
   call. With the call mix above, that is about 25k cycles per frame in e1m1 (about 3% of the modelled frame) and
   83k in demo1 (about 6%).
2. **An exact reciprocal for `dc_iscale` and `SlopeDiv`**: a table, a Newton step, a multiply and a ±1 correction, at
   about 25–30 instructions against 46–47. That saves about 1.5% of the frame. It needs its own exactness proof, and
   SH4 has no count-leading-zeros instruction for the normalisation. An inexact reciprocal would change pixels.
3. **Owning the `dc_*`/`ds_*` globals in one asm block** (README §9): one base literal plus `@(disp,Rn)` loads in
   place of about 10 literal loads per call. This overlaps with item 1.
4. **The locals pattern for `R_RenderMaskedSegRange`.** `R_RenderSegLoop` and `R_DrawMaskedColumn` keep their loop
   state in locals; `R_RenderMaskedSegRange` still loops on the globals `dc_x` and `spryscale` around every
   `R_DrawMaskedColumn` call. It is 0.5% of e1m1 and 0.8% of demo1, so the gain is a fraction of that.
5. **Re-scheduled column loops for 3-cycle loads** (README §9): worth doing only if hardware timing shows GCC's
   "gcc" latencies.

Ideas measured and left out are listed in [`docs/DEVELOPMENT.md` §5.1](../docs/DEVELOPMENT.md#51-where-the-speed-comes-from).

## How to reproduce

From the repository root, with the project toolchain on the `PATH` (`export PATH=$PWD/toolchain/bin:$PATH`):

```sh
make sim-tool        # once: builds build-sim/tools/sh-elf-run64 (64-bit counters, PC profile)
make sim             # build-sim/cgdoom-sim.elf (SH4 drawers; C only: make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim)
make test-sim        # quick check: the first 60 tics of e1m1, map01 and demo1 against the goldens
make test-sim-full   # all 36 scenarios in the simulator against the goldens (about 4 min)
make perf-sim        # the per-frame table and the e1m1/map01/demo1 profiles (about 1 min); output in build-sim/perf/
make VARIANT=hd sim-tool   # once per screen format (VARIANT=wide|hd): build-sim-hd/tools/sh-elf-run64
make VARIANT=hd perf-sim   # the per-frame table for that format; output in build-sim-hd/perf/
```

`src/sim/perf-sim.sh -bin <elf>` takes `-only NAME` (repeatable: the scenarios of `tests/scenarios.txt`), `-prof NAME`
(the scenarios to profile), `-from F` (first frame of the window) and `-out DIR`.
The mode scenarios that have goldens run the same way (`-only e1m1-low -only demo1-low -only e1m1-view7`). The other
mode runs go by hand, with a host run for the CRC check:

```sh
W=test-wads/fd/freedoom-0.13.0
ARGS="-iwad $W/freedoom2.wad -warp 1 -tics 900 -input builtin:explore -viewsize 9"
src/sim/simrun --stats build-sim/cgdoom-sim.elf $ARGS -crc build-sim/perf/m.crc -simstats -simwin 42 0
build-host/cgdoom-host $ARGS -crc build-sim/perf/m.host.crc && cmp build-sim/perf/m.crc build-sim/perf/m.host.crc
src/sim/simrun --pcprof build-sim/perf/m.prof build-sim/cgdoom-sim.elf $ARGS -simwin 42 0
src/sim/simprof -n 40 build-sim/cgdoom-sim.elf build-sim/perf/m.prof
src/sim/simprof --disasm R_RenderSegLoop build-sim/cgdoom-sim.elf build-sim/perf/m.prof
```

`src/sim/cgdoom-hooks.c` takes `-simwin A B` (frame window `[A, B)`, `B = 0` to the end; the PC profile is paused
outside it), `-simstats` (one `SIMFRAMES key=value ...` line at exit) and `-simmarks` (one `SIMMARK <frame> <insts>
<cycles>` line per frame), and removes them from argv before the host's `main`.
