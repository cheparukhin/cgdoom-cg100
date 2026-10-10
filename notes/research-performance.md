# Performance strategy for DOOM on fx-CG100 / Graph Math+ (SH7305, SH4AL-DSP)

Scope: the hot paths of CGDoom (`/Users/cheparukhin/casio_doom/ref/CGDoom`, fx-CG50 port), SH-4A/SH4AL-DSP techniques, prior art, and a ranked optimization plan for the fx-CG100 port (fxSDK + gint dev, loaded by MPM). Each claim is tagged:

- **[VERIFIED]**: seen in source code, measured in this session, or from a primary document.
- **[MEASURED]**: produced in this session by host instrumentation or the GDB SH simulator. The harness is in `/Users/cheparukhin/casio_doom/notes/perf-harness/`.
- **[REPORTED]**: a third-party forum post or secondary source, not independently checked.
- **[GUESS]**: my estimate or inference. These need checking on hardware.

---

## 0. Executive summary

1. **CGDoom's fixed-point helpers are its biggest CPU sink, and they are not vanilla-exact.**
   - `FixedMul` is a bit-serial shift-and-add loop (`cgdoom/m_fixed.c:40-83`). It costs **196 SH instructions per call**, against 4–5 for `dmuls.l`+`xtrct` [MEASURED, sim].
   - `FixedDiv` makes 16 calls to `__udivsi3` (`m_fixed.c:90-115`). It costs **862 instructions per call**, against 85 for a `div1` sequence [MEASURED].
   - Per rendered frame CGDoom makes 2.2k–4.2k `FixedMul` and 170–415 `FixedDiv` calls [MEASURED, host instrumentation]. That is about **0.6–1.2 M instructions, or roughly 5–10 ms at 118 MHz, spent on arithmetic helpers** [GUESS: 1 instruction ≈ 1 cycle].
   - Both helpers also differ from vanilla. `FixedMul` mismatches on 48% of random inputs (every opposite-sign product rounds toward zero). `FixedDiv` mismatches on 34.5% (it drops the last quotient bit) [MEASURED]. This breaks vanilla demo compatibility.
   - Fixing it is trivial and safe: the int64 forms are bit-exact with vanilla (0 mismatches in 20 M samples).
2. **CGDoom's present step (`I_Flip`) is CPU-driven.** It makes 64,000 single 16-bit writes to the LCD data port (`cgdoom/i_video.c:144-153`).
   - gint documents a full-screen DMA push (396×224×2 B) at about **11 ms** at stock clocks [VERIFIED, `gint/include/gint/display.h:54-62`]. Scaled to 320×200 that is **~8 ms of blocked CPU per frame** [GUESS].
   - Replacement: palette-convert into on-chip XRAM/YRAM strips and DMA them to the LCD, chained from DMA-end interrupts. CPU cost drops to ~1.4 ms and the ~8 ms transfer overlaps the next frame. This is the same pattern as gpSP for CG100 and Azur.
3. **GCC's code for the inner loops is poor.**
   - The vanilla C column loop compiles to 13–14 instructions per pixel. The span loop compiles to 21–22, because `byte*` stores alias the `dc_*`/`ds_*` globals and force reloads every pixel [VERIFIED, GCC 14.1 `-O3 -m4a-nofpu` output].
   - A hand-written SH4AL-DSP column loop runs in 8 instructions per pixel with a zero-overhead repeat loop, and is bit-exact with the C reference in the simulator [MEASURED].
   - Expected cycles per pixel: columns ~7.5 → ~4; spans ~11.5 → ~6 [GUESS, from pipeline-resource counting].
4. **Low-detail mode (half horizontal resolution) is the biggest optional switch.** It roughly halves column and span cost, and CGDoom removed it (`r_main.c:590`). Also offer the D32XR-style "potato" flat floors and the stock view sizes.
5. **Overclocking.**
   - gint's CG100 F4 profile = F5 = 235.9 MHz CPU but only a 59 MHz bus [VERIFIED, `gint/src/cpg/overclock.c:450-507`].
   - The gpSP port uses a "Ptune4 F5" profile: 236 MHz CPU, 118 MHz bus, CL2 [VERIFIED in gpsp source]. It is reported to give "at least 2x" [REPORTED], with a bit-flip risk on some units [REPORTED].
   - Ship it as an opt-in setting.
6. **Plan.** After items 1–4 plus overclock as an option, **35 FPS (the tic rate) at stock clock is plausible for typical scenes** [GUESS]. Section 5 has the ranked plan; section 6 explains how to validate each step without hardware.

---

## 1. Platform facts that drive performance

### 1.1 CPU core and toolchain

| Fact | Status | Source |
|---|---|---|
| SH7305 = SH4AL-DSP: SH-4A integer core with DSP extension, **no FPU**. CGDoom builds with `-m4a-nofpu -mb -mhitachi` | VERIFIED | `CGDoom/Makefile:27` |
| fxSDK fx-CG target flags are `-m4-nofpu -mb -fno-builtin -nostdlib -Wa,--dsp`. DSP instructions are allowed in asm. CG100 add-ins use the same fxCG50 toolchain file | VERIFIED | `fxsdk/fxsdk/cmake/FXCG50.cmake:19` |
| gint sets `SR.DSP = 1` at driver configure, so DSP repeat loops (`ldrs`/`ldre`/`ldrc`) and DSP registers work in add-ins | VERIFIED | `gint/src/cpu/cpu.c:28-33` |
| The fxSDK template defaults to `-Os -flto=auto`. DOOM's hot files should use `-O2`/`-O3` | VERIFIED | `fxsdk/fxsdk/assets/CMakeLists.txt:33` |
| GCC uses division strategy `call-table` for `-m4*-nofpu` (`TARGET_HARD_SH4`): `__udivsi3_i4i`/`__sdivsi3_i4i`, table-driven for divisors ≤128 and `div1`-based otherwise | VERIFIED | `gcc-14.1.0/gcc/config/sh/sh.cc:850-881`, `libgcc/config/sh/lib1funcs.S:1618-1754` |
| Cost of `0xffffffffu / s` through `__udivsi3_i4i`: **~47 instructions per call** for divisors 2^8–2^22 | MEASURED (sim) | `perf-harness/sim/divb.c` |
| `(int64)a*b >> 16` compiles to `dmuls.l; sts macl; sts mach; xtrct` (4 instructions) | VERIFIED (GCC 14.1 `-S`) | §3.1 |
| `((int64)a << 16) / b` compiles to a call to `___divdi3` (generic, slow) | VERIFIED (GCC 14.1 `-S`) | §3.1 |

### 1.2 Pipeline (SH-4A)

- **Dual issue.** At most one instruction can use the LS (load/store) pipe and one the EX pipe per cycle. "MT" moves (`mov Rm,Rn`, `mov #imm`) can use either pipe. In GCC's model, branches reserve the EX address calculator, so two branches never issue together [VERIFIED, `gcc/config/sh/sh4a.md:34` `ID_or = ex|ls`, `:50` `sh4a_addrcalc = sh4a_ex`, `:100` MOV latency 0].
- **Latencies in GCC's model:** load 3, store 0, EX 0 (forwarded), taken branch 2, `jsr`/`rts` 3, `dmuls.l` 3 [VERIFIED, `sh4a.md:58,66,82,108,121,147`].
- **Lephe's measured note:** "a 2-cycle delay for the RAW dependency between each mov.b and the corresponding use" [VERIFIED, `CGDoom/src-cg/cgdoom-asm.s:36-38`]. Azur's P8 shader comment: "omitting this nop slows the loop to 7 cycles/i" [VERIFIED, `Azur/azur/src/gint/shaders/image_p8_normal.S:69`]. Instruction pairing and alignment do matter.
- **Only one memory operation per cycle.** Any loop doing `texel load + colormap load + store` has a hard floor of **3 cycles per pixel**. Since `@(r0,Rn)` is the only indexed addressing mode, **every table lookup must go through r0**. This serializes the texel→colormap chain.
- **SH4AL-DSP zero-overhead repeat loop:** `ldrs start; ldre end; ldrc Rn` (RC is a 12-bit count). It removes `dt`/`bf` and the taken-branch penalty.
  - Azur and gint use it everywhere (e.g. `Azur/azur/src/gint/r61524.s:9-19`, `Azur/azur/src/gint/shaders/image_macros.S` `START` macro). A 1-instruction body is allowed (`clear.S`: `1: 2: mov.l r1,@-r6`).
  - The GDB simulator executes these correctly when the ELF is tagged `sh4al-dsp` [MEASURED].
  - [GUESS] The exact restrictions follow the SH-DSP manual (no branches inside, short-loop special cases, RC ≤ 4095), which was not consulted in this session. Copy Azur's `START`/`END` macro shape.
- **Cache.** [GUESS, needs confirmation] Assumed 32 KiB I-cache + 32 KiB O-cache, 4-way, 32-byte lines, as on SH7724. gpSP's emulator models "SH7305-like 32 KiB 4-way, 32 B lines" with miss penalties of 25 cycles and write-back 10 [REPORTED, `extra/gpsp-sh4-jit/docs/testing-harness.md`, `performance-history.md:5-8`].

### 1.3 Clocks

Decoded from gint register values: FLL = 32768 × 900 / 2 = 14.7456 MHz; PLL ×16 or ×32.

| Profile | FRQCR | CPU | SHW | Bus (BFC) | Pφ | Status |
|---|---|---|---|---|---|---|
| gint CG100 F1 (= CG50 F1, assumed stock) | `0x0F011112` | 117.96 MHz | 58.98 | 58.98 | 29.49 | VERIFIED values (`overclock.c:450-461`). That this is CG100 stock is GUESS (gint uses F1 as the CG100 reference) |
| gint CG100 F4 = F5 ("Graph Math+ cannot handle the higher bus speed") | PLL×32, IFC/2, SFC/4, BFC/8, P/16 | 235.9 | 117.96 | 58.98 | 29.49 | VERIFIED (`overclock.c:484-507`) |
| gpSP "Ptune4 F5" for CG50/CG100 | `0x1F001103`, CS3WCR `0x24D2` (CL2) | 235.93 | 117.96 | 117.96 | 29.49 | VERIFIED in `extra/gpsp-sh4-jit/ports/fxcg100/gint-gpsp/src/overclock.c:6-27`. Stability is REPORTED ("aggressive overclocking may cause bit flips") |

- **The fx-CG is bus-bound** [REPORTED, Cemetech "Casio fx-CG/CP overclocking insights"]: raising BFC helps more than raising IFC.
- **Graph Math+ display update** is reported at 6.91 ms under a 92 MHz-bus preset (+59% vs stock), so stock is ≈11 ms [REPORTED, via search summary of the HP Museum and Cemetech threads].
- Consequence: **gint F4 doubles CPU-bound code but not LCD or cache-miss-bound code**. A Ptune4-F5-style profile doubles both [GUESS].

### 1.4 Memory map and speeds relevant to rendering

| Region | Address | Notes | Status |
|---|---|---|---|
| ILRAM | `0xE5200000`, 4 KiB | gint linker `.ilram` (code+data, `GILRAM` attribute). Reloaded from the ROM image after OS world switches by default (`GINT_ONCHIP_REINITIALIZE`) | VERIFIED: `gint/fxcg50.ld.c` MEMORY, `gint/include/gint/defs/attributes.h:17`, `gint/src/kernel/world.c:63,186` |
| XRAM / YRAM | gint view: `0xE500E000` 16 KiB contiguous. Banks also at `0xE5007000` (X, 8 KiB) and `0xE5017000` (Y, 8 KiB) | gint linker `.xyram` (`GXRAM`/`GYRAM`). DMA translation accepts `0xE5007000-0xE5009000` and `0xE5017000-0xE5019000` (`gint/src/dma/dma.c:47-56`). gpSP notes "aliasing of all e500xxxx XRAM windows and all e501xxxx YRAM windows" | VERIFIED (code); aliasing REPORTED |
| Uncached RAM write | — | "At default overclock levels, the RAM can register a write every 13-14 cycles, regardless of size" (gint VRAM is uncached, `dvram.c` uses `0xA…`) | VERIFIED: `gint/src/render-cg/image/image_rgb16_normal.S:9-11`, `gint/src/render-cg/dvram.c:113-115` |
| Full-screen RGB565 write cost | — | "Just writing out 396x224 pixels ... takes 6.1 ms" (to RAM). An Azur on-chip clear takes 400 µs (0.5 cycle/px) | VERIFIED: `Azur/azur/include/azur/gint/render.h:9-10`, `shaders/clear.S:1-14` |
| LCD data port | `0xB4000000` (RS on PRDR bit 4) | gint uses `DMA_32B` blocks with a fixed destination | VERIFIED: `gint/src/r61524/r61524.c:32-35,192-210` |
| Full-screen dupdate | — | about 11 ms at stock. Triple buffering showed "minimal performance improvements, because writing to main RAM does not parallelize with DMA transfers" (uncached VRAM) | VERIFIED: `gint/include/gint/display-cg.h:85-96`, `display.h:54-62` |
| New 2025–26 LCDs | — | Controller versions 50/82 use DCS-style `0x2A`/`0x2B`/`0x2C` window commands; old R61524 uses `0x210`–`0x213` | VERIFIED: `gint/src/r61524/r61524.c:136-176` |
| NOR flash (CS0) | `0x8…` cached / `0xA…` uncached | gint F1 CS0WCR `0x3C0` (WR field = 0b0111). gint uses WR=11 → "18 cycles" during clock changes. Lephe: ROM reads are fastest by 16-bit words. CGDoom moving WAD reads to cached ROM gave +35% game speed | VERIFIED: `overclock.c:99,450-461`, `cgdoom-asm.s:11-14`, commit `55f9d43` message |
| CG100 WAD/file mapping | — | gpSP maps CG100 files directly from NOR via the BFile "get block address" syscall at `0x80333cf2` (per 4 KiB block) and reads through P1 | VERIFIED: `extra/gpsp-sh4-jit/ports/fxcg100/gint-gpsp/src/nor_rom.c:37-46,390-406` |

---

## 2. Profile-by-reading of CGDoom, backed by counts

### 2.1 Where CGDoom spends time (code walk)

**Arithmetic helpers** (called from everywhere):
- `FixedMul`, `cgdoom/m_fixed.c:40-83`. A bit-serial loop with one iteration per bit of `min(|a|,|b|)`, then the result is negated. This rounds toward zero, whereas vanilla `(int64)a*b>>16` rounds toward −∞.
- `FixedDiv`/`FixedDivU`, `m_fixed.c:90-115`. Sixteen iterations of `ua / ub` and `ua % ub`, i.e. 16 libgcc divisions. It also shifts the result one place too many and loses quotient bit 0.
- `SlopeDiv`, `tables.c:48-58`. One unsigned division per `R_PointToAngle` (`r_main.c:279`).
- `finecosine()` is an out-of-line function (`r_main.c:127-130`), not a pointer alias.

**Wall loop** `R_RenderSegLoop` (`r_segs.c:195`). Per column:
- one `FixedMul` (`:253`);
- one 32-bit division `dc_iscale = 0xffffffffu / rw_scale` (`:263`);
- a `R_GetColumn` → `W_CacheLumpNumConst` → `W_CacheLumpNumCommon` → `Z_ChangeTag2` call chain (`r_data.c:363-390`, `w_wad.c:340-360`, `z_zone.c:448-468`);
- a direct `R_DrawColumn()` call (`:281,304,334`).

**Wall setup** `R_StoreWallRange` calls:
- `R_PointToDist` (2× `FixedDiv`, `r_main.c:370-394`);
- `R_ScaleFromGlobalAngle` twice (2× `FixedMul` + 1× `FixedDiv` each, `r_main.c:404-436`).

**Planes** `R_MapPlane` (`r_plane.c:120-176`):
- 3 `FixedMul` when the row's plane height changes (cached per row);
- 3 more `FixedMul` per span;
- then `R_DrawSpan()`.
- `R_DrawPlanes` caches the flat per visplane (`:408`). `MAXVISPLANES 128` (`:52`).

**Sprites:**
- `R_DrawSprite` builds `clipbot`/`cliptop` (2×320 shorts on the stack) and scans all drawsegs per sprite (`r_things.c:841-951`).
- `R_DrawVisSprite` reads `columnofs` bytewise (`INTFromBytes`, `:440`) because of unaligned flash.
- `R_DrawMaskedColumn` calls through `colfunc` (`:390`).

**Drawers** `cgdoom/r_draw.c`:
- vanilla C `R_DrawColumn` (`:104-147`), `R_DrawFuzzColumn` (`:178-261`), `R_DrawTranslatedColumn` (`:278-338`), `R_DrawSpan` (`:410-452`);
- `RANGECHECK` is enabled globally (`doomdef.h:79`), so hot paths carry extra checks;
- low detail is hard-disabled (`detailshift = 0`, `r_main.c:590`);
- default view is size 10 = 320×168 (`r_main.c:676`, `:583-587`).

**Present** `I_Flip` (`i_video.c:108-157`):
- with `CGDOOM_DIRECT_R61524`, 64,000 iterations of `LCDC = gPaletteData[screens[0][j]]`, writing to the 16-bit LCD port with the CPU;
- the framebuffer `screens[0]` is the cached P1 alias of VRAM (`src-cg/platform.h:70-73`; commit `624df28` "Cache accesses to VRAM (I_Flip 3.4x)").

**Pacing:**
- frames are rate-limited to 35 tics/s with frameskip 0–3, default 1 = 17.5 FPS (`i_system.c:241-257`; `cgdoom.c:129` `CGD_Frameskip = 1`);
- Lephe: "really hard to clear the gap between 17.5 FPS and 35 FPS, even with full overclock"; Computer_Freak_2004: "even with full overclock, some areas struggle to reach 20 or 25 FPS" [REPORTED, Cemetech CGDoom thread, Sept 2021].

**Existing optimizations in CGDoom:**
- the WAD is mapped from cached flash (`cgdoom.c:323-397`, `FindInFlash`/`Flash_ReadFile`);
- sector `memcmp` in asm (`src-cg/cgdoom-asm.s`), for load time only;
- libprof counters `CGD_Perf` for allocation, rendering, display, lump load and unaligned lump load (`cgdoom.h:125-136`, read out in `st_stuff.c:516-525`). There is no per-renderer-phase breakdown.

### 2.2 Measured operation counts per frame

Method: the CGDoom sources plus the `in330` freestanding SDL `platform.h`, instrumented with counters (`perf-harness/hostcount/cgdoom-instrumentation.patch`) and built for macOS with SDL dummy video. Settings: `freedoom1.wad` 0.13.0, view size 10 (320×168), one tic per frame. These counts do not depend on the platform; only their cost does.

- **Run A:** attract-mode DEMO1–4 playback, 6,000 frames. The low variance suggests the demos desynced into near-static views; treat it as one data point.
- **Run B:** scripted walker with god mode, wall-unsticking, use/fire, cycling E1M1–E1M8 every 1,050 tics, 8,400 frames.

| Per rendered frame | Run A mean | Run B mean | Run B p95 | Run B max |
|---|---|---|---|---|
| `FixedMul` (renderer) | 4,152 | 2,216 | 4,758 | 7,853 |
| `FixedDiv` (renderer) | 415 | 171 | 467 | 718 |
| `SlopeDiv` (renderer) | 730 | 314 | 862 | 1,563 |
| `dc_iscale` divisions (wall columns) | 1,556 | 757 | 1,460 | 2,431 |
| wall columns in `R_RenderSegLoop` | 1,905 | 1,154 | 2,347 | 3,878 |
| `R_DrawColumn` pixels (walls + sky + sprites + weapon) | 36,121 | 46,823 | 60,062 | 184,568 |
| `R_DrawSpan` pixels | 22,991 | 11,013 | 27,572 | 37,385 |
| `R_DrawColumn` calls | 4,261 | 964 | 2,678 | 7,179 |
| `R_MapPlane` (= spans) | 174 | 205 | 612 | 1,868 |
| `R_StoreWallRange` | 83 | 39 | 108 | 178 |
| `R_GetColumn` / `W_CacheLumpNum*` | 1,526 / 782 | 703 / 515 | 1,357 / 970 | — |
| `R_PointOnSide` / `R_PointToAngle` | 170 / 731 | 187 / 314 | 668 / 862 | 7,872 / 1,563 |
| visplanes drawn / vissprites | 23.5 / 37.2 | 15.5 / 7.1 | 43 / 25 | 79 / 83 |
| `FixedMul` / `FixedDiv` per tic (game logic) | 26 / 3 | 107 / 13 | 264 / 50 | 6,787 / 638 |

Fuzz and translated columns were essentially zero in these runs. Spectres and multiplayer colours are rare.

### 2.3 Measured SH instruction costs (GDB SH simulator)

`sh-elf-run -v` reports "# instructions executed". Built with the local GCC 14.1 (`toolchain-build/build-gcc/gcc/xgcc -m4-nofpu -mb -O2`) and libgcc's `lib1funcs.S` (`L_div_table`). Operands are shaped like DOOM's: magnitudes of 8–26 bits with random signs.

| Routine | Instructions per call | Notes |
|---|---|---|
| CGDoom `FixedMul` (bit-serial) | **196.1** | data-dependent loop with branches; cycles ≥ instructions [GUESS] |
| `(int64)a*b>>16` (out-of-line) | **5.0** | inline would be 4 instructions; `dmuls.l` latency 3 |
| CGDoom `FixedDiv` | **861.6** | 16 calls to `__udivsi3_i4i` |
| Proposed `FixedDiv` (64/32 via `div0u` + 32×`rotcl`/`div1`) | **84.8** | bit-exact vs a restoring-division reference on 4,000 samples [MEASURED] |
| `0xffffffffu / s` (`__udivsi3_i4i`) | **47** | the per-wall-column `dc_iscale` |
| C `R_DrawColumn` loop (GCC, locals) | **12.4/px** | includes per-call overhead |
| Hand asm column, `dt`/`bf` loop | **10.2/px** | bit-exact vs C over 2,000 random columns (98,851 px) |
| Hand asm column, DSP repeat loop | **9.3/px** | 8 instructions per pixel in the body; bit-exact |

Exactness on the host (20 M random pairs, `perf-harness/fixed_exactness_host.c`) [MEASURED]:
- CGDoom `FixedMul` ≠ vanilla on 48.4% of inputs, all of them opposite-sign;
- CGDoom `FixedDiv` ≠ vanilla on 34.5% of inputs;
- int64 `FixedDiv` vs vanilla's `double` `FixedDiv2`: **0 mismatches**.

### 2.4 Estimated frame budget at 117.96 MHz

[GUESS] Assumes instructions ≈ cycles for scalar code. Loop cycles come from pipe-resource counting (§3.2/3.3), without memory stalls.

| Item | Current CGDoom (Run B / Run A) | After items P1–P5 (§5) |
|---|---|---|
| Column inner loop (~7.5 → ~4 cycles/px) | 351k / 271k | 187k / 144k |
| Span inner loop (~11.5 → ~6 cycles/px) | 127k / 264k | 66k / 138k |
| `FixedMul` (renderer) | 434k / 814k | 10k / 19k |
| `FixedDiv` (renderer) | 147k / 358k | 15k / 35k |
| udiv (`iscale` + `SlopeDiv`) | 50k / 107k | 50k / 107k (later: reciprocal, P12) |
| Per-column glue (`GetColumn` chain, calls) | ~115k / ~190k | ~70k / ~115k |
| BSP, sprites, planes bookkeeping | ~150k | ~100k |
| Present | ~930k (≈7.9 ms CPU-blocked) | ~160k CPU (≈2.5 cyc/px conversion), DMA overlapped |
| **Total, excluding memory stalls and game logic** | **≈2.3M / 3.1M ≈ 19.5 / 26 ms** | **≈0.66M / 0.82M ≈ 5.6 / 6.9 ms** |

**Memory stalls are the unknown.** Texture, flat and sprite fetches from flash or SDRAM, plus framebuffer line fills, are not in this table. The observed "17.5–25 FPS even overclocked" on the CG50 implies the real CGDoom cost is about 1.5–2× the table [GUESS]. Game logic is ~0.03M cycles of helper arithmetic per tic on average (spikes to 1.3M cycles in a heavy tic with the old `FixedMul`), plus thinker work that was not measured.

---

## 3. SH4-specific techniques

### 3.1 Fixed point (do first, P1)

```c
/* m_fixed.h: static inline; GCC 14.1 -m4-nofpu emits: dmuls.l r5,r4; sts macl,r0; sts mach,r1; xtrct r1,r0 */
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{ return (fixed_t)(((long long)a * b) >> FRACBITS); }

/* 64/32 unsigned divide: 66 EX-group instructions (~66 cycles), exact; requires hi < d */
static inline uint32_t udiv64_32(uint32_t hi, uint32_t lo, uint32_t d) {
  __asm__("div0u\n\t.rept 32\n\trotcl %1\n\tdiv1 %2,%0\n\t.endr\n\trotcl %1"
          : "+r"(hi), "+r"(lo) : "r"(d) : "t");
  return lo;
}
static inline fixed_t FixedDiv(fixed_t a, fixed_t b) {
  if ((abs(a) >> 14) >= abs(b)) return (a ^ b) < 0 ? MININT : MAXINT;   /* vanilla guard */
  uint32_t ua = abs(a), ub = abs(b);
  uint32_t q = udiv64_32(ua >> 16, ua << 16, ub);   /* hi < ub is implied by the guard */
  return (a ^ b) < 0 ? -(int)q : (int)q;            /* truncation toward 0 == vanilla */
}
```

- [MEASURED] This `FixedDiv` is bit-exact with exact long division (sim). The host showed that int64 `FixedDiv` equals vanilla's `double` version on 20 M samples.
- `div1` and `rotcl` are both EX-group, so they do not pair with each other. Count ~70–85 cycles [GUESS].
- **Renderer-only divisions** (`dc_iscale`, `R_ScaleFromGlobalAngle`, `R_ProjectSprite`) do not affect game state. They could use a reciprocal table plus multiply, as GBADoom does (`extra/GBADoom/source/m_recip.c`: 65,537-entry `reciprocalTable`, 256 KiB). That changes pixels slightly, so it is lower priority (P12).
- **Game-logic divisions must stay exact** to keep vanilla demo sync.

### 3.2 Column drawer (P3)

**Problems in the current C loop** [VERIFIED, GCC 14.1 `-O3 -m4a-nofpu`, CGDoom's own flags]. The loop body is:

```
swap.w r2,r0; mov.l @r4,r7 (reload dc_source); exts.w r0,r0; and #127,r0; mov.b @(r0,r7),r0;
dt r1; mov.l @r5,r7 (reload dc_colormap); add r6,r2; extu.b r0,r0; mov.b @(r0,r7),r7;
mov.b r7,@r3; mov.w .L20,r7 (reload constant 320); bf/s; add r7,r3
```

That is 6 LS operations per pixel, three of them useless, and 7 EX operations. Estimated ~7–8 cycles per pixel.

**Proposed loop.** Row-major 8-bit; verified bit-exact in the simulator (`perf-harness/sim/col.s` `_col_asm_rpt`):

```
; r1 = frac<<9, r2 = step<<9, r3 = -25, r4 = dest, r5 = texture column,
; r6 = rotated colormap base, r8 = 320, r10 = next texel index
      ldrs 2f ; ldre 3f ; ldrc r7 ; nop
2:    mov   r10,r0        ; MT   index n
      mov   r1,r10        ; MT   start index n+1
      mov.b @(r0,r5),r0   ; LS   texel n (sign-extended)
      shld  r3,r10        ; EX   (frac<<9)>>>25 == (frac>>16)&127, no AND needed
      add   r2,r1         ; EX
      mov.b @(r0,r6),r9   ; LS   colour n
      mov.b r9,@r4        ; LS   store
3:    add   r8,r4         ; EX
```

- Resources per pixel: 3 LS + 3 EX + 2 MT = 8 operations, so **≥4 cycles per pixel** on a dual-issue core. A second software-pipeline stage that delays the store by one iteration hides the load-use latency [GUESS: ~4 cycles/px reachable, ~2× the C loop].
- **Sign-extension trick, no `extu.b`.** `mov.b` sign-extends. Index the colormap through `base = T + 128` with `T[j] = colormap[j ^ 0x80]`, a 256-byte rotated copy. Then `base[(int8)texel] == colormap[texel]`.
  - D32XR solves the same problem with doubled tables in low-detail mode (`extra/d32xr/sh2_drawlow.s`).
- **`COLEXTRABITS` trick.** Pre-shift `frac` and `step` left by 9 so a single shift yields the masked 7-bit row index. This is GBADoom's `COLEXTRABITS 9` (`extra/GBADoom/source/r_hotpath.iwram.c:535-546`).
- **Calling convention.** Pass arguments in registers; do not touch `dc_*` globals inside asm. Inline the `R_GetColumn` fast path.
- **Non-power-of-two textures.** DOOM masks with `&127`, so tall or odd textures wrap at 128. To stay vanilla-exact, keep the mask. D32XR has an NPO2 variant if the port ever wants correct tall textures (`sh2_draw.s` `_I_DrawColumnNPo2A`).
- **Fuzz and translated drawers.** The same structure applies. Fuzz needs `dest[±320]` loads, giving 4 LS per pixel; it is rare, so keep it simple.

### 3.3 Span drawer (P3)

**Current C loop:** 21 instructions plus the delay slot per pixel [VERIFIED]. It reloads `ds_source`, `ds_colormap`, `ds_xstep` and `ds_ystep` every pixel because of `char` aliasing: 7 LS and ~11 EX, so **~11–12 cycles per pixel** [GUESS]. Simply copying the globals to locals in C removes 4 LS (a cheap first step).

**Asm design options:**
- **D32XR style** (`extra/d32xr/sh2_draw.s` `_I_DrawSpanA`). Keep x and y fractions separately, with y pre-shifted by 6:

  ```
  swap.w X,r0; and #63,r0; swap.w Y,r1; and r11(=0xFC0),r1; or r1,r0
  ```

  Add `add`×2 for stepping, plus 3 LS with `mov.b r,@-r8`, drawing right-to-left with a pre-decrement store and no pointer add. That is ≈7 EX per pixel, ~7 cycles.
- **Chocolate/GBADoom packed position** (`r_hotpath.iwram.c:1278-1290`): `spot = ((pos>>4)&0xfc0) | (pos>>26)`, one `add` per step. That is 6 EX + 2 MT + 3 LS = 11 operations, **~5.5–6 cycles per pixel** [GUESS].
- **Not worth it:** packing 4 pixels into `mov.l`. Spans are EX-bound, and byte packing adds EX operations.

### 3.4 Low detail, view size, potato floors (P5)

- **Low detail** (`detailshift = 1`, 160×N): columns and spans compute one texel per two pixels. Use a 16-bit "doubled" colormap (`c | c<<8`, 32 levels × 512 B = 16 KiB) and a single `mov.w` store. The same 8 operations then cover two pixels: **~2 cycles per output pixel for columns, ~3 for spans** [GUESS].
  - Prior art: D32XR `_I_DrawColumnLowA` uses `mov.w` with a doubled colormap (`sh2_drawlow.s`). GBADoom renders its whole view this way (`R_DrawColumnPixel` writes `color | color<<8`, `r_hotpath.iwram.c:538-549`).
  - Re-enable vanilla's `R_DrawColumnLow`/`R_DrawSpanLow` path (`r_main.c:600-612` commented out) and the menu's detail option.
- **View size.** Expose the vanilla screen-size steps (CGDoom keys `SKEY_DECVP`/`SKEY_INCVP`, `i_system.c:220-232`). The column and span counts scale with the view area.
- **Potato floors.** Solid-colour flats (D32XR feature list, `extra/d32xr/README.md`). Precompute an average palette index per flat and run spans as `mov.l` fills, ~0.25–0.5 cycles per pixel [GUESS]. It is an opt-in visual downgrade.
- **Half vertical resolution** (render 320×100 and double rows in the presenter). About 2× on all drawing. It needs projection changes, so it is only for a "potato" preset [GUESS].

### 3.5 Framebuffer layout: row-major vs transposed

| | Row-major (vanilla) | Column-major (transposed) |
|---|---|---|
| Column store | `mov.b r,@rD` + `add 320`: 1 EX | `mov.b r,@-rD`: 0 EX → ~3.5 vs ~4 cycles/px (−12%) [GUESS] |
| Span store | `mov.b r,@-rD`: 0 EX | `add 200`: +1 EX → ~+15% on spans |
| Cache | 32 adjacent columns share 200 lines (6.4 KiB), which fit easily | columns sequential, spans scatter |
| Code churn | none | every `V_*`, `ST_*`, `HU_*`, `AM_*`, wipe, fuzz (`FUZZOFF` = 1) |
| Present | sequential reads | per strip, read 12 contiguous bytes per column (no LCD reconfiguration needed). Alternatively use the controller's vertical-increment mode (R61524 `REG_ENTRY_MODE 0x003`, defined in `gint/src/r61524/r61524.c:21` but unused). That mode would have to be implemented and tested for both controller generations: risky |

Verdict: **keep row-major.** With walls at ~60–80% of pixels in Run B the net is a few percent at best, and the code churn is large [GUESS].

### 3.6 8-bit plus conversion vs rendering directly in RGB565

Verdict: **keep 8-bit and convert in the presenter.**
- **Memory.** Direct RGB565 doubles the framebuffer (128 KB vs 64 KB), so cache footprint, write-backs and line fills double.
- **Lookups.** A 16-bit colormap lookup needs `texel*2` in r0: +1 EX per pixel, so the column loop goes from ~4 to ~4.5 cycles.
- **Effects.** Fuzz (colormap 6 on the *underlying index*) cannot be reproduced exactly.
- **Code churn.** Every 2D path (`V_DrawPatch`, wipe, automap, status bar) needs a 16-bit version.
- **Payoff.** Only the presenter's ~2.5 cycles/px conversion (~1.4 ms) is saved, and it can be hidden under DMA anyway.

### 3.7 Presenter: on-chip strips plus chained DMA (P2)

Design [GUESS for timings; mechanisms VERIFIED in gint, gpSP and Azur]:

1. Before the first strip, call `r61524_start_frame(38, 357, 12, 211)` once. It handles both controller generations (`gint/src/r61524/r61524.c:136-176`). The GRAM address auto-increments, as Azur's `azrp_render_fragments` relies on (`Azur/azur/src/gint/render.c:236-282`).
2. Convert 12 rows (320×12×2 = 7,680 B) of the 8-bit frame into a strip in **XRAM bank** `0xE5007000`, the next 12 into the **YRAM bank** `0xE5017000`. 640 B per row is a multiple of 32, so any row count works with `DMA_32B`. gint's "height multiple of 4" constraint applies only to 396-px rows (`r61524.c:195-199`).
3. `dma_transfer_async(ch, DMA_32B, 240, strip, DMA_INC, (void*)0xB4000000, DMA_FIXED, cb)`. In `cb`, convert the next strip into the free bank and launch it. That is 17 strips per 200-row frame.
4. **Chaining caveat** [VERIFIED]. gint's DMA interrupt handler calls the callback and *then* clears the slot (`gint/src/dma/dma.c:163-164`), so a callback that restarts the **same** channel loses its own callback.
   - Ping-pong two channels. gint uses ch0 for display, ch1 for `dma_memcpy`/`dma_memset`, ch3/4 for USB (`r61524.c:202`, `dma/memcpy.c:18`, `usb/pipes.c:357-361`), so **ch2/ch5 are free**.
   - Alternatively, patch gint so the slot is cleared before the call.
5. Double-buffer the 8-bit frame (2 × 64,000 B, cached P1 RAM). The renderer writes frame N+1 while interrupts stream frame N.
6. **Conversion loop**, per 2 pixels:

   ```
   mov.b @rS+,r0; shll2 r0; mov.l @(r0,rP),r2; mov.b @rS+,r0; shll2 r0; mov.l @(r0,rP),r3; xtrct r2,r3; mov.l r3,@-rD
   ```

   It uses a 1 KiB `palette32` with duplicated colours (`c<<16|c`), rotated by 0x80 for the sign-extension trick, so `xtrct` merges two pixels in one EX op. That is 5 LS per 2 pixels, **≈2.5 cycles/px → ≈160k cycles ≈1.4 ms per frame at stock** (0.7 ms at 236 MHz) [GUESS]. Azur uses the same `swap.w`/`xtrct` duplication in `shaders/clear.S`.
7. **Bus contention** [GUESS]. DMA reads come from on-chip RAM, but LCD writes and the CPU's SDRAM cache misses share the external BSC bus. Expect renderer slowdowns while the ~8 ms transfer runs, and measure them.
8. **Prior art.** gpSP's production presenter double-buffers DMA strips in full 8 KiB XRAM/YRAM banks while composing the next strip (`extra/gpsp-sh4-jit/ports/fxcg100/gint-gpsp/src/gint_platform.c:23-41,255-272,281-365`). Its documented hardware findings:
   - shrinking strips to 4 rows ("doubling or tripling DMA launch/wait frequency") was "substantially slower";
   - a "scanline-streamed" variant was also slower on hardware [REPORTED, `docs/performance-history.md:560-617`].

   Use large strips (≥12 rows) and keep the DMA count per frame low.
9. **Cheaper fallback** (no interrupts): CPU-push from strips with an ILRAM repeat loop of `mov.l` writes to `0xB4000000`, as in Azur's `azrp_r61524_fragment_x1` (`Azur/azur/src/gint/r61524.s:1-21`). This is not overlapped; it only removes the per-pixel palette work from the bus-bound loop.
10. **Status bar.** When unchanged, the status bar (32 rows, 16% of pixels) and borders can be skipped. Track dirtiness from `ST_Drawer`/`V_MarkRect` and set a smaller window.

### 3.8 On-chip RAM placement (P7)

- **ILRAM (4 KiB): inner loops.** Column and span drawers, their low-detail variants, the conversion loop, `udiv64_32`. Use the `GILRAM` attribute or `.section .ilram,"ax"` in asm, as Azur's `r61524.s` does. Benefit: no I-cache misses after game logic evicts the drawers. Gain is small, 1–3% [GUESS].
- **XRAM (8 KiB): colormaps 0–31** in the rotated layout, 256 B each, exactly 8 KiB. That guarantees 1-cycle, never-evicted lookups. Gain unclear, since colormaps are only 8 lines each and normally stay cached [GUESS].
  - This conflicts with using XRAM as a strip bank. Decide by measurement; the priority is the presenter.
- **XYRAM contents are lost on world switches.** By default gint *re-initializes* XYRAM from the ROM image after OS calls (`world.c:186`). Runtime-generated tables must be rebuilt after `gint_world_switch` (file I/O, saves), or use `GINT_ONCHIP_BACKUP` (20 KiB buffer, `gint/include/gint/gint.h:91-128`).

### 3.9 Memory hierarchy (P6, P10)

- **Textures in RAM.** At level load, copy the level's wall-texture columns, flats, and optionally sprite lumps into cached RAM instead of reading them from NOR on every frame.
  - [GUESS] NOR line fills are far slower than SDRAM: CS0 wait states of 8+ bus cycles per 16-bit access, versus SDRAM bursts. Each new wall column touches up to 4 lines (128 B).
  - The CG100 has much more free RAM than the CG50: gpSP proves an arena at `0x8c200000..0x8c655300` [VERIFIED, `extra/gpsp-sh4-jit/ports/fxcg100/gint-gpsp/fxcg100_gpsp.ld:16-31`].
  - Measure with the perf counters' operand-miss wait events before committing.
- **Never render into uncached memory.** gint's own VRAM is the uncached P2 alias (`dvram.c:113-115`), at 13–14 cycles per write. Use cached P1 buffers for `screens[]`.
- **`pref @Rn`** to prefetch the next texture column, or next span row, one column ahead. It is an LS op, so it costs a slot. Try it only in the per-column glue, not inside the pixel loop [GUESS].
- **`movca.l`** (allocate without fetch) avoids read-for-ownership fills when writing *whole* 32-byte lines (`memset`/clear, status-bar copies). DOOM's renderer does not write whole lines in order, so it is limited to clears.
- **MMU/TLB.** MPM maps the add-in at `0x00300000` (55 × 64 KiB pages) and user RAM at `0x08100000` (8 × 64 KiB) via the UTLB (`mpm/main.c:load_addin`). Large buffers placed in P1 (`0x8c…`) avoid TLB pressure. Check with perf events `0x0220`/`0x0222`/`0x02a0` (§6.4).

### 3.10 Compiler and code-level items (P4, P8)

- Make `FixedMul` and `finecosine` `static inline` in headers.
- Copy `dc_*`/`ds_*` globals to locals or use `restrict` in any remaining C loops (§3.2/3.3 evidence).
- Undefine `RANGECHECK` in `r_draw.c`, `r_plane.c` and `r_segs.c` for release builds.
- Use `-O2` (or `-O3` for `r_*.c`) instead of the fxSDK template's `-Os`. LTO is worth trying (CGDoom had LTO disabled, README).
- Prefer `-m4a-nofpu` for SH-4A scheduling (`sh4a.md`). Linking with gint built `-m4-nofpu` should be fine because the ABI is the same [GUESS].
- Do not use `-mhitachi`: it is a CGDoom-only ABI flag and must not be mixed with gint.
- Cut per-column glue: an inline `lumpcache` fast path in `R_GetColumn`, no `Z_ChangeTag` for level-pinned textures, and no `W_CacheLumpNum` per column.
- Replace `INTFromBytes` with an aligned `columnofs` copy or `movua.l` (`r_things.c:440`).
- Align loop heads to 4 bytes (`.balign 4`). D32XR uses `.p2alignw` with NOPs and Azur uses `.balign 4`.

### 3.11 Pacing

- Vanilla renders nothing new between tics, so 35 FPS is the useful cap.
- Implement **auto frameskip**: skip `D_Display` when behind schedule, keep `G_Ticker` at 35 Hz. It is on CGDoom's own TODO list (`README` "Auto frameskip?").
- Keep the 35 tic/s limiter (`i_system.c:241-257`). Use gint's TMU-based `prof_*` (`gint/include/gint/prof.h`) instead of CGDoom's libprof copy, whose TMU0 addresses are hard-coded in `src-cg/libprof.h:26-29`.

---

## 4. Prior art: what transfers

| Project | Relevant technique | Applicability |
|---|---|---|
| **Azur** (Lephe; `ref/extra/Azur`) | Tiled rendering into XYRAM (16 rows × 396 px in `0xE500E000+32`, `render.c:10-11`). DSP repeat loops in every shader. ILRAM-resident LCD push loop (`r61524.s`). `xtrct` colour duplication. Documented costs: 6.1 ms RAM frame write vs 400 µs on-chip clear | Direct for the presenter and loop style. DOOM's renderer itself cannot be tiled without re-running the BSP per tile |
| **gint** (`ref/gint`) | Pipelined, 2-unrolled P8/RGB16 loops with the "edge pixel" trick (`src/render-cg/image/image_p8.S:1-60`). DMA driver. Overclock tables. TMU profiler | Use gint primitives; copy loop idioms |
| **gpSP-sh4-jit** (`ref/extra/gpsp-sh4-jit`) | CG100 Ptune4-F5 overclock profile. XRAM/YRAM double-buffered DMA LCD strips. ILRAM-resident hot dispatch stubs. NOR-mapped ROM through P1. Emulator cache model and FBSTAT-hash parity testing | Strongest CG100-specific precedent. Hardware findings: big strips win; scanline streaming lost |
| **D32XR** (`ref/extra/d32xr`; SH-2, MIT for original code) | Hand asm column/span/fuzz/low-detail drawers (`sh2_draw.s`, `sh2_drawlow.s`, 4-bit variants). `dmuls.l`/`xtrct` `FixedMul` and HW-divider `FixedDiv` (`sh2_fixed.s`). Low-res and potato modes. Two-CPU split renderer | Drawer structure and tricks map directly to SH4. Take ideas, not Jaguar-licensed code: only files with MIT headers are reusable (README "License") |
| **GBADoom** (`ref/extra/GBADoom`; prBoom-based) | Half-width rendering with doubled pixels. `COLEXTRABITS` index trick. Packed span position. 16× unrolled C loops in fast RAM (`r_hotpath.iwram.c`). Reciprocal table. Column cache. Removed z-depth lighting. Mip-mapping | Same constraints class (no divider, small fast RAM). ~35 FPS simple / ~10 FPS complex areas on a 16.8 MHz ARM7 [REPORTED, README] |
| **CPDoom** (`ref/extra/ClassPadDev_CPDoom`, fx-CP400 SH4) | doomgeneric-style C port, `-O2 -flto -m4a-nofpu` | Little hand optimization; only build-flag reference |
| **Dreamcast ports** (Doom 64 DC, chocolate-doom DC) | PowerVR GPU rendering, FPU (`fipr`/`ftrv`), store queues | [GUESS] Mostly not applicable (no FPU, no GPU on SH7305). Store queues might exist on SH-4A but DMA already gives 32-byte bursts |
| **CGDoom lineage** (nDoom → CGDoom → CG50 CGDoom) | Flash-mapped WAD, cached ROM reads, asm `memcmp` | Keep the "direct pointers into cached flash/RAM" philosophy; drop the slow arithmetic |

---

## 5. Prioritized optimization list

Gains are shares of current per-frame CPU time on the CG100 at stock clocks, from §2.4 [GUESS unless marked]. "Validate" refers to the methods in §6.

| # | Optimization | Expected gain | Effort | Risk | Validate |
|---|---|---|---|---|---|
| **P1** | Inline `FixedMul` (`dmuls.l`) and `div1` `FixedDiv`, both vanilla-exact. Also inline `finecosine` | **−25 to −38% of current CPU time** (removes ~0.6–1.2M instructions per frame) and kills game-logic spikes. Restores vanilla demo compatibility | 0.5 day | Very low | Host exactness test (done: 0 mismatches); sim exactness (done); vanilla demo playback must not desync (§6.3) |
| **P2** | Presenter: palette conversion into XRAM/YRAM strips plus DMA to the LCD, chained by interrupts (ch2/ch5 ping-pong), double-buffered 8-bit frame. Fallback: synchronous strips | **−6 to −8 ms per frame** (I_Flip from ~8 ms CPU to ~1.4 ms CPU) | 2–3 days | Medium: IRQ/DMA races, two LCD controller variants, bus contention | Host: converter vs C reference (byte-exact strips). Sim: conversion instruction count. Hardware: libprof present time, visual check on both LCD types |
| **P3** | Asm column and span drawers (repeat loops, rotated colormap, `COLEXTRABITS`, pipelining), plus fuzz and translated variants | Columns ~7.5 → ~4 cycles/px, spans ~11.5 → ~6: **−0.23 to −0.25M cycles per frame (≈−10% of today's total; ≈35% of the post-P1/P2 total)** | 2–4 days | Low–medium (exactness is checkable) | Sim harness: random columns and spans vs C (done for columns, 98,851 px exact); static pipeline count; hardware libprof |
| **P4** | Remove `RANGECHECK` in hot files, localize globals and use `restrict` in C loops, trim the `R_GetColumn`/`W_Cache` per-column chain, pass drawer arguments in registers | −3 to −6% | 0.5–1 day | Very low | Host counters (calls per frame), sim counts |
| **P5** | Low detail (re-enable `detailshift`, 16-bit doubled stores), screen-size steps, optional potato floors | **Low detail ≈ −40% of drawing time.** Potato: spans ~10× cheaper | 1–2 days | Low (visual only) | Host screenshots vs reference; demo determinism unaffected (renderer only) |
| **P6** | Level-load residency of textures, flats and sprites in cached RAM (no per-frame NOR fetch). Avoid uncached targets | Unknown. Possibly the largest remaining stall source [GUESS: 5–20% in texture-heavy scenes] | 1–2 days | Medium (RAM budget per WAD) | Hardware perf counters (O-cache miss and wait events) or calcemu-style cache model |
| **P7** | Hot loops in ILRAM; optionally colormaps in XRAM if a bank is free | 1–3% | 0.5 day | Low (remember XYRAM re-init on world switch) | Hardware A/B with libprof (I-cache effects are not simulatable here) |
| **P8** | Build flags: `-O2`/`-O3` on `r_*.c`, `-m4a-nofpu`, LTO, loop alignment | 3–10% | 0.5 day | Low | Sim instruction counts for hot functions; hardware A/B |
| **P9** | Opt-in overclock: (a) gint F4 = 236 MHz CPU, 59 MHz bus; (b) gpSP's Ptune4-F5 profile (bus 118 MHz). Restore on exit and world switch (gint does this) | (a) ~1.5–1.8× on CPU-bound parts; (b) ~2× overall [REPORTED "at least 2x" for KhiCAS with Ptune4 F5] | 0.5 day | (b) bit-flip/crash risk on some units [REPORTED] | Hardware only; keep off by default |
| **P10** | Auto frameskip and status-bar/border dirty tracking in the presenter | Smoothness; −16% present traffic when the status bar is static | 0.5–1 day | Low | Host: frame-hash equality; hardware feel |
| **P11** | Visplane hashing (prBoom-style), sprite clip: per-sprite drawseg scan pruning (Run B p95 `spr_dsscan` 1,932, max 9,272) | Few %, more in large rooms or heavy sprite scenes | 1 day | Low | Host counters |
| **P12** | Renderer-only fast reciprocal for `dc_iscale` and `R_ScaleFromGlobalAngle` (table plus `dmulu.l`) | ~0.05–0.1M cycles per frame | 1 day | Low–medium (sub-pixel wobble) | Host pixel diff vs exact; visual |
| **P13** | Column-major framebuffer, or LCD vertical-increment mode | ±few % | Large | High | Not recommended now |

Recommended order: **P1 → P4 → P2 → P3 → P5 → P8 → P6 → P7 → P10 → P9 → P11/P12**. Measure after each step (§6.4).

---

## 6. Validation without hardware

### 6.1 Host equivalence tests (C reference vs new code)

- `perf-harness/fixed_exactness_host.c` compares CGDoom, vanilla `double` and int64 `FixedMul`/`FixedDiv` (`cc -O2 fixed_exactness_host.c && ./a.out`). Result: CGDoom `FixedMul` 48.38% mismatch, `FixedDiv` 34.52% mismatch, int64 `FixedDiv` 0.000000%.
- **Renderer golden frames.** Run the instrumented SDL build (§6.3) and hash `screens[0]` after each `R_RenderPlayerView`, using FNV-1a like gpSP's `@@CGBA_FBSTAT` (`docs/testing-harness.md`).
  - Any change that should be pixel-exact (P1 is *not*, because it fixes rounding; P3/P4/P8 are) must keep the hash stream identical.
  - For non-exact changes (low detail, reciprocal), diff images with a threshold.

### 6.2 SH instruction counts and equivalence in the GDB simulator

Recipe (works today with the local toolchain; `perf-harness/sim/`):

```sh
G=/Users/cheparukhin/casio_doom/toolchain-build/build-gcc/gcc; T=/Users/cheparukhin/casio_doom/toolchain/bin
$T/sh-elf-as -big -isa=sh4al-dsp crt0.s -o crt0.o          # _start -> main -> trapa #34 exit
$T/sh-elf-as -big -isa=sh4al-dsp col.s  -o col.o           # tags ELF sh4al-dsp => DSP repeat loops work
$G/xgcc -B$G/ -m4-nofpu -mb -O2 -ffreestanding -fno-builtin -DMODE=9 -c bench.c -o b.o
$T/sh-elf-ld -T link.ld crt0.o b.o col.o -o b.elf
$T/sh-elf-run -v b.elf; echo "exit=$?"    # exit code = mismatches (0 = bit-exact); -v prints "# instructions executed"
```

- For libgcc division routines (the full libgcc build currently fails), assemble `gcc-14.1.0/libgcc/config/sh/lib1funcs.S` with `-DL_div_table` (gives `__udivsi3_i4i`/`__sdivsi3_i4i`).
- `MODE=0/1/2` builds isolate the C loop, the `dt` asm loop and the repeat-loop asm, so per-pixel instruction counts are the difference against a no-draw baseline (`MODE=7`). A deliberately broken table returned exit 200, so the check works.
- **Limits.** The simulator's "cycles" is a crude SH-1-style model (`gdb-15.2/sim/sh/interp.c:2275-2296`). It does **not** model SH-4A dual issue or caches. Use it for exactness and instruction counts only.

### 6.3 Determinism and representative workload

- The instrumented host build (`perf-harness/hostcount/cgdoom-instrumentation.patch` + `build.sh`) gives per-frame counters (table §2.2) with mean, p50, p95 and max.
  - It needs SDL2, `in330`'s `src-sdl2/platform.h`, and a patched `patchcount[2048]`.
  - **Bug found:** CGDoom's `R_GenerateLookup` has a fixed `byte patchcount[320]` (`r_data.c:290,306-308`) that overflows on Freedoom's wider-than-320 textures. That may be a cause of the "Freedoom E2 crashes" noted in the README.
  - Run: `SDL_VIDEODRIVER=dummy BENCH_FRAMES=8400 ./cgdbench` (in a dir containing `freedoom1.wad`). `BENCH_MAPTICS` changes map-cycling.
- **Vanilla demo sync** is the classic regression test. After P1, play the IWAD's DEMO1–3 (or recorded `.lmp`s) and compare the final player position, health and gametic against Chocolate Doom.
  - The CGDoom code comments that demos of version 108/109 "probably" fail due to AI (`g_game.c:1369-1370`). The non-vanilla `FixedMul`/`FixedDiv` are a more concrete cause.
  - Once in sync, demos give deterministic, user-independent benchmark runs on calculator and host alike (as gpSP does with its headless harness).

### 6.4 Pipeline estimates and on-hardware measurement

- **Static pipeline counting.**
  - [GUESS-level tool] Write a small script that parses `objdump -d`, classifies instructions into LS/EX/MT/BR by the groups and latencies in `gcc/config/sh/sh4a.md`, and simulates in-order dual issue for a loop body.
  - Calibrate it against Azur's documented loop speeds: P8 loop ~3 cycles/px (≈6 per 2 px, "7 cycles/i" without the nop); clear loop 0.5 cycle/px.
- **On hardware, timing.** gint `prof_enter`/`prof_leave` use a TMU at Pφ/4 = 7.37 MHz, about 16 CPU cycles per tick at 118 MHz (`gint/src/prof/prof.c`). Instrument:
  - `R_RenderBSPNode`, `R_DrawPlanes`, `R_DrawMasked`, column/span drawers, present, `G_Ticker`;
  - accumulate per frame and show as a HUD line, extending CGDoom's `CGD_ProfilerResults` (`st_stuff.c:516-525`).
- **On hardware, SH-4A performance counters** [GUESS that SH7305 implements them]. Per Linux `arch/sh/kernel/cpu/sh4a/perf_event.c`:
  - registers: `CCBR(i)` at `0xFF200800 + 4i`, `PMCTR(i)` at `0xFC100000 + 4i`, `PMCAT` at `0xFC100080`, 2 counters;
  - events: `0x0000` cycles, `0x0202` instructions, `0x0203` instructions executed in parallel, `0x002a` I-cache miss, `0x0032`/`0x003a` O-cache read/write miss, `0x0033`/`0x003b` wait cycles from O-cache misses, `0x02b4` operand-read wait cycles, `0x0236`/`0x023e` X/Y-memory reads/writes, `0x0220`/`0x0222`/`0x02a0` TLB misses;
  - enable sequence: set `CLR` in `PMCAT`; write `CCBR = (event<<6) | CMDS | PPCE`; then set `DUC`.

  A one-hour hardware experiment would confirm whether these exist. If they do, they give exact IPC and miss numbers for every item above.
- **Emulators.** gpSP's docs reference a "casio-emu"/"calcemu" with an SH7305-like cache model and PC profiler [REPORTED; not public as far as checked: `KaraRyougi/CLASSWIZ-CG-EMU` on GitHub contains only a LICENSE]. If it becomes available it is the best no-hardware cycle estimator.

---

## 7. Open questions to settle early

1. Confirm the CG100 stock clock is gint F1 (117.96 MHz): read FRQCR/FLLFRQ at startup and display them [GUESS now].
2. Measure the LCD transfer rate on the CG100 for 320×200 (DMA from XRAM vs RAM) and the overlap efficiency while rendering.
3. Check the SH7305 for SH-4A perf counters (§6.4). Check the true cache geometry by probing with a stride/size microbenchmark.
4. Measure the NOR-vs-RAM texture fetch cost: render one frame with textures in NOR vs copied to RAM, using libprof.
5. Decide the RAM budget for P6 together with the memory/WAD research (MPM `loadinfo` RAM range `0x8c200000..loadAddress`, `mpm/main.c:run_addin`).
6. Check which LCD controller the target units have (gint `r61524_get_version()` 50/82 vs classic) and test the presenter on both.

---

## 8. Files produced in this session

- `/Users/cheparukhin/casio_doom/notes/perf-harness/sim/`
  - `crt0.s`, `link.ld`: freestanding SH simulator start-up and exit;
  - `col.s`: asm column drawers, `dt` and DSP repeat-loop versions;
  - `bench.c`: random-column equivalence and timing harness;
  - `fixbench.c`: CGDoom vs proposed `FixedMul`/`FixedDiv`, exactness and counts;
  - `divb.c`: `__udivsi3_i4i` cost.
- `/Users/cheparukhin/casio_doom/notes/perf-harness/hostcount/cgdoom-instrumentation.patch` (plus `build.sh`): counter instrumentation and scripted-walker driver for CGDoom's SDL build, applied to a copy of `master` with `origin/in330:src-sdl2/platform.h`.
- `/Users/cheparukhin/casio_doom/notes/perf-harness/fixed_exactness_host.c`: host exactness test.

### Sources (web)

- [gpSP for CASIO (KaraRyougi/gpsp-sh4-jit)](https://github.com/KaraRyougi/gpsp-sh4-jit) — cloned at `ref/extra/gpsp-sh4-jit`
- [D32XR (viciious/d32xr)](https://github.com/viciious/d32xr) — cloned at `ref/extra/d32xr`
- [GBADoom (doomhack/GBADoom)](https://github.com/doomhack/GBADoom) — cloned at `ref/extra/GBADoom`
- [Azur (Lephenixnoir)](https://git.planet-casio.com/Lephenixnoir/Azur) — cloned at `ref/extra/Azur`
- [Cemetech: CGDoom thread (FPS/frameskip comments, Sept 2021)](https://www.cemetech.net/forum/viewtopic.php?p=295533)
- [Cemetech: Casio fx-CG/CP overclocking insights](https://www.cemetech.net/forum/viewtopic.php?t=20468&start=0), [follow-up post](https://www.cemetech.net/forum/viewtopic.php?p=313333)
- [HP Museum thread on Graph Math+ overclocking](https://www.hpmuseum.org/forum/thread-23597-lastpost.html) — 403 to direct fetch; numbers via search summary only [REPORTED]
- [Linux SH-4A perf counter driver](https://github.com/torvalds/linux/blob/master/arch/sh/kernel/cpu/sh4a/perf_event.c)
- [Prizm wiki: On-chip memory](https://prizm.cemetech.net/Technical_Documentation/Peripherals/On-chip_Memory)
