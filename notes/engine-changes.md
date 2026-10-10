# Engine changes log

Each optimisation or engine step adds a section here. A section records:
- what changed (files and functions);
- the measured effect: `make perf-sim` instructions and cycles per frame before and after, on the scenarios of
  `tests/perf-baseline.md`;
- risks;
- what a later agent must know.

---

## Step 1 (2026-10-09): vanilla-exact fixed point (P1), inline helpers, 8.3 file names

### What changed

**Fixed point** (`src/engine/m_fixed.h`, `m_fixed.c`)

- `FixedMul` is now `static inline` in `m_fixed.h` and computes `(fixed_t)(((int64_t)a * b) >> 16)`, as in vanilla.
  - GCC `-m4-nofpu` compiles it to `dmuls.l` / `sts mach` / `sts macl` / `xtrct`.
  - The out-of-line function no longer exists. Nothing may take its address or call `_FixedMul` from assembly.
- `FixedDiv` stays out of line in `m_fixed.c` and is vanilla-exact.
  - **Guard:** `(abs(a) >> 14) >= abs(b)` with a wrapping `abs`, so `abs(MININT) == MININT` as on the original x86. As a
    result, `b == MININT` always saturates, and `a == MININT` never trips the guard.
  - **Quotient:** truncated toward zero.
    - On SH (`__sh__`) it is an inline-asm `div0u` followed by 32 × (`rotcl`; `div1`) and a final `rotcl` (64/32
      unsigned). It runs on the magnitudes, which are computed branch-free so that GCC keeps a single copy of the asm.
    - Elsewhere it is `((int64_t)a * 65536) / b`.
  - `m_fixed.c` proves in a comment that the int64 quotient equals vanilla's `double` `FixedDiv2`. The double error is
    below 2^-6/|b|, so it can never cross an integer.
  - **The only deviation from vanilla:** when `a == MININT` and the quotient does not fit (`|b| < 1.0`, `b == -1.0` or
    `b == 0`), vanilla calls `I_Error("FixedDiv: divide by zero")`. This version saturates with the guard's sign rule
    instead, in the cold out-of-line `FixedDiv_MinA`.
  - `FixedDiv2` and `FixedDivU` were removed. They had no callers.
- **T clobber:** GCC 14 SH **does** accept `"t"` as a clobber. `T_REG` is named `"t"` in `REGISTER_NAMES`
  (`gcc/config/sh/sh.h:565`), and a test showed GCC does not keep T live across such an asm. The M and Q bits that
  `div1` changes are never live across statements in GCC code.
  - The asm agent's `src/engine/sh4/cgd_sh4.h` uses the same idiom. No `.S` file was needed. A `.S` file in
    `src/engine/` would not have been picked up by CMake anyway, which only globs `src/engine/*.c` and
    `src/engine/sh4/*.S`.

**Inline helpers** (bit-exact; the code is unchanged, only moved)

- `finecosine()` was out of line in `r_main.c` and is now `static inline` in `tables.h`.
- `SlopeDiv()` was in `tables.c` and is now `static inline` in `tables.h`.

**8.3 file names** (`src/engine/cgdoom-names.c` is new; the API is in `cgdoom.h`)

The full mapping is documented at the top of `cgdoom-names.c`.

- **Settings:** `CGDOOM_SETTINGS_FILE` is now `"CGDOOM.CFG"`. The file system is case-insensitive, so the old
  `CGDoom.cfg` is the same file.
- **Saves:** `<P>_<s>.DSG` from `CGD_SaveFileName()`. Used in `g_game.c` (`G_DoLoadGame`, `G_DoSaveGame`) and `m_menu.c`
  (`M_ReadSaveStrings`).
- **Demos:** `<P>D<nn>.LMP` from `CGD_DemoFileName()`, used in `d_main.c`. `G_RecordDemo` now takes a complete file
  name and appends vanilla's `.lmp` only when the name has no extension.
- **The prefix `<P>` (at most 5 characters), from `CGD_FilePrefix(CGD_WADFileName)`:**
  - Take the base name: no directory, cut at the first `.`, upper-cased.
  - If it is ≤ 5 characters, all in `[A-Z0-9_-]`, it is used as is: `DOOM1_0.DSG`, `DOOM2D07.LMP`, `TNT_3.DSG`.
  - Otherwise the prefix is the first 3 characters, with other characters mapped to `_`, followed by 2 base-36 digits
    of the FNV-1a hash of the whole upper-cased name. For example `freedoom1` gives `FRE0P`, `freedoom2` gives `FREXS`,
    `freedm` gives `FREZ4`, `plutonia` gives `PLUFN` and `doomu1` gives `DOO9I`.
  - An empty or NULL name gives `DOOM`.
  - The mapping is deterministic. The slot part is parsed from the end, so names never collide across slots or types.
  - Long names collide only when they share the same first 3 characters and the same hash mod 1296. The 24 common WAD
    names in the test are all distinct.
- `CGD_ValidName83()` is the strict 8.3 check: 1–8 characters, an optional `.` and 1–3 characters, from
  `A-Z a-z 0-9 ! # $ % , ( ) + - ; = @ [ ] ^ _ \`` and space.

**Host and tests** (`src/host`, `tests`, `Makefile` host/test targets)

- Host `PL_FileWrite` and `PL_FileDelete` call `PL_Fatal` on any name that is not 8.3, so tests catch violations.
- New host options:
  - `-record N`: record demo slot N; it is written at exit when `-data` is given.
  - `-playdemofile NAME`: play a demo file from the `-data` directory.
  - `-wadname NAME`: override `CGD_WADFileName`.
  - `-gamelog`: print `GAMELOG` lines to stderr for level start, level completed, player death and the final state
    (map, gametic, health, armor, kills, items, secrets).
- `tests/fixed_exact.c` is rewritten as a PASS/FAIL test.
  - It compares the engine's `FixedMul`/`FixedDiv` with a partial-product `FixedMul` reference, an int64 vanilla
    reference and vanilla's `double` `FixedDiv2`.
  - Inputs are all pairs of 66 edge values (`MININT` included), the `a == MININT` sweeps, inputs near the guard boundary
    and random values of every magnitude.
- New Makefile targets:
  - `make test-fixed` (host, 51 M pairs, about 1.5 s).
  - `make test-fixed-sim`: the same test built with the sim flags and run in the SH simulator, about 12 s with the
    default `FIXED_SIM_PAIRS=3000000`.
  - `make test-names` (`tests/names_test.c`, ASan+UBSan).
  - `make test-roundtrip` (`tests/demo-roundtrip.sh`): records `FRE0PD05.LMP` on E1M3 with `builtin:walk`, plays it
    back, and requires identical frames.
  - `make test-unit` runs all three host tests.
- Goldens were regenerated. All but `title` changed; see `tests/golden/CHANGES.md`.

### Test results

| test | result |
|---|---|
| `make test-host` | 19/19 PASS |
| `make test-asan` | 19/19 PASS |
| `make test-unit` | PASS: 0 mismatches in 51 M pairs; names; round trip of 941 frames |
| `make test-fixed-sim FIXED_SIM_PAIRS=20000000 FIXED_SIM_DOUBLE_EVERY=2` | PASS: 20.46 M pairs; 0 mismatches against int64; 0 mismatches against double on 10.26 M pairs |
| `make check-sh` | all engine files compile |
| `test-sim` | PASS (3/3) |
| `test-sim-full` | 19/19 PASS |

- An exhaustive host check over all 2^32 values of `b`, for `a` in {MININT, ±MAXINT, ±2^30, 12345678, −1}, gave 0
  mismatches against vanilla's double version. 131,072 of those cases are vanilla `I_Error` cases and were checked
  against the documented saturation.
- `test-sim` and `test-sim-full` ran on a C-only sim ELF. It was built with
  `make SIM_BUILD=<dir> CGSIM_ASM_SRC= 'CGSIM_C_SRC=$(ENGINE_SRC) $(HOST_SRC)'`. At the time,
  `src/engine/sh4/r_draw_sh4.S`, which the asm agent was still writing, did not assemble ("pcrel too far"), so the
  default `make sim` failed for reasons outside this step.

### Performance (`make perf-sim`, C-only sim ELF, frames 42..end, same scenarios as `tests/perf-baseline.md`)

| scenario | insts/frame before | after | Δ | cycles/frame before | after | Δ |
|---|---|---|---|---|---|---|
| e1m1 | 2,017,661 | 1,578,304 | −21.8% | 2,219,747 | 1,711,555 | −22.9% |
| e1m2 | 2,140,728 | 1,355,399 | −36.7% | 2,374,393 | 1,469,969 | −38.1% |
| e1m3 | 1,617,624 | 1,179,345 | −27.1% | 1,774,804 | 1,275,504 | −28.1% |
| e2m2 | 1,634,253 | 846,840 | −48.2% | 1,832,426 | 923,264 | −49.6% |
| e3m4 | 1,883,890 | 1,518,998 | −19.4% | 2,070,369 | 1,648,696 | −20.4% |
| demo1 | 4,044,224 | 2,479,911 | −38.7% | 4,463,376 | 2,689,190 | −39.7% |
| map01 | 1,715,048 | 1,360,375 | −20.7% | 1,878,984 | 1,470,074 | −21.8% |
| map07 | 1,792,072 | 1,190,355 | −33.6% | 1,982,551 | 1,288,844 | −35.0% |
| demo2-1 | 2,868,025 | 1,829,810 | −36.2% | 3,174,992 | 1,979,733 | −37.6% |

**Caveat:** game math changed, so the scenarios no longer show the same frames. The input scripts and demos lead
somewhere slightly different, and the noclip runs (e1m2, e2m2, map07) diverge most. The representative scenarios
(e1m1, map01) should be read as about −21 to −23%. The demos gain more because they have more `FixedMul` calls per
frame.

**Profile changes:**
- `FixedMul` was 17–24% of cycles and has disappeared from the profile. It is inlined to about 4 instructions.
- `FixedDiv` went from about 202 to **93 instructions per call**: e1m1 171 calls per frame, demo1 451 calls per frame.
  It is now about 1% of cycles.
- libgcc division fell, because CGDoom's `FixedDiv` made 16 `__udivsi3` calls:
  - `__udivsi3_i4i` from 5.4% to 2.2% of e1m1;
  - its calls from 3,357 to about 1,070 per frame in e1m1, and from 9,654 to about 2,300 in demo1. These counts are
    estimated from the instructions at about 33 per call.
- The top of the e1m1 profile is now:
  - `R_DrawColumn` 50.4%;
  - `R_RenderSegLoop` 12.3%;
  - `R_DrawSpan` 7.4%;
  - `R_MakeSpans` 3.0%;
  - `R_DrawMaskedColumn` 3.0%;
  - `__udivsi3_i4i` 2.2%;
  - `R_GetColumn` 2.1%;
  - `__sdivsi3_i4i` 2.0%.
- The full lists, made with the same command, are in the perf output: `perf-sim.sh -out`.

### Demo compatibility (evidence)

**Method:** `cgdoom-host -playdemo DEMOn -tics 200000 -gamelog`, with old and new math. The old-math host was rebuilt
from the same tree with CGDoom's `m_fixed.c` and reproduces the old goldens. Freedoom 0.13.0 demos are all version 109.

| demo | header | old math | new (vanilla) math |
|---|---|---|---|
| freedoom1 DEMO1 | E1M6, skill 2, 1 player, 7117 tics | died at tic 541; at the end health 40, 5 kills | died at tic **590**; at the end health 10, 3 kills |
| freedoom2 DEMO1 | MAP03, skill 2, 1 player, 2664 tics | died at tic 754 (5 kills) | died at tic **1019** (9 kills) |
| freedoom2 DEMO3 | MAP22, skill 3, 1 player | died at tic 524 | died at tic 524; same end state |
| freedoom1 DEMO2/3/4, freedoom2 DEMO2/4 | 2–3 player co-op demos | unplayable | unplayable |

- In the single-player demos, vanilla math makes the player survive longer (DEMO1 of both WADs). Not one demo completes
  its level, so they **still desync**: vanilla math is necessary but not sufficient.
- Five of the eight Freedoom demos are co-op recordings. CGDoom's engine is single-player:
  - `G_Ticker` reads ticcmds only for player 0;
  - `G_InitNew` only reborns player 0;
  - so playback consumes the 2–3-player stream one player per tic and runs 2–3× too long. For example DEMO2 lasts
    21,267 tics for 7,089 recorded tics.
  - These demos can only sync once multiplayer ticcmd and spawn support is restored.
- Single-player demos recorded by this engine are self-consistent: `make test-roundtrip` replays a 941-frame recording
  with a death and respawn bit-exactly.
- **Remaining single-player desync sources** were not found in this step. Ruled out by reading:
  - `rndtable`/`P_Random`;
  - `R_PointToAngle`/`SlopeDiv`;
  - `P_AproxDistance`;
  - the `P_Random` call sites;
  - the demo header and ticcmd parsing.
- **Known non-vanilla code that might matter:**
  - action functions moved and dispatched through a switch (`p_enemy.c` `TA_*`);
  - `p_switch.c` loads every switch;
  - `G_InitNew` lost the `-fast`/nightmare state and speed changes ("make mobjinfo const"); this matters only for
    nightmare and `-fast` demos;
  - nDoom-era fixes listed in the CGDoom history ("medikit message", "switches", "skull keys").
  - A tic-by-tic comparison against a vanilla engine (Chocolate Doom) would find the first divergent tic. No vanilla
    engine is available offline in this tree.
- **Resolved later:** the single-player desyncs came from CGDoom's sound removal, `P_LookForPlayers`,
  `A_CloseShotgun2` and the nightmare changes. Found tic by tic against linuxdoom-1.10, see "Demo sync" below.

### Risks

- **Pixels and gameplay changed on purpose.** Saves made with the old math still load, because the format did not
  change, but they play on with different rounding.
- `FixedMul` inline grows code slightly (each call site is about 4 instructions instead of a `jsr`). The sim ELF `.text`
  grew from 269,552 to 272,076 bytes (+2.5 KiB), which also includes `cgdoom-names.c` and the host additions.
- **8.3 names change where saves live:**
  - Old CGDoom names such as `doom1_0.dsg` were already 8.3 for `doom1`, so those saves remain.
  - Old demo names like `doom1_demo00.lmp` were not valid 8.3 and will not be found under the new names.

### For later agents

- **The engine's `FixedDiv` is the reference.** `src/engine/sh4/cgd_sh4.h`'s `cgd_fixed_div` matches it everywhere,
  except the vanilla-`I_Error` cases with `a == MININT`. In those cases the engine saturates: `b >= 0` gives MININT and
  `b < 0` gives MAXINT. For `b == -65536` the asm agent's version returns MININT. Align it if bit-exactness in those
  never-seen cases matters.
- **Platforms must set `CGD_WADFileName` to the full WAD base name** (`"freedoom1"`, not a truncated `"freedo"`). The
  engine shortens it itself.
  - `src/gint/main.c` (`run_game`) currently truncates it to 6 characters. Then `freedoom1` and `freedoom2` share
    `FRED2_*.DSG` (the prefix of `"freedo"`). Plain `doom1`/`doom2` are unaffected.
  - The gint launcher's demo list should match `*.LMP` names of the form `<P>D<nn>`. `CGD_FilePrefix()` from
    `cgdoom.h` gives `<P>` for a WAD. The old CGDoom pattern was `*_demo*.lmp`.
- `make test-fixed-sim` is the exactness gate for any future SH change to `FixedDiv` or `FixedMul`, for example an asm
  `FixedDiv` in `src/engine/sh4`.
- The next big items in the profile are:
  - the drawers (P3): `R_DrawColumn` is about 50% of e1m1;
  - `R_RenderSegLoop`;
  - the remaining libgcc divisions, `dc_iscale` and `SlopeDiv` (P4/P12).

### Step 1 review (2026-10-10, adversarial reviewer)

No defects confirmed in the step 1 code; no source changes. Extra checks run:

- Independent SH stress of `FixedDiv`/`FixedMul` against a libgcc `__udivdi3`/int64 reference, biased to the hard
  cases (maximum quotient `|a| = |b|*2^14 - 1`, every divisor magnitude up to 2^31-1, quotients near powers of two):
  3 M pairs at `-O2`, 300 k each at `-O0`, `-Os`, `-O3` and `-O2 -m4a-nofpu` (so the asm constraints survive other
  register allocations), 50 M on the host: 0 mismatches.
- Generated code read back: `FixedMul` is `dmuls.l`/`sts mach`/`sts macl`/`xtrct` with no `__muldi3`; `FixedDiv`'s
  guard branches match vanilla for all four sign combinations and for `MININT` on either side; the only 64-bit
  libcall left is `__divdi3` in the cold `FixedDiv_MinA`.
- `make test-host` 19/19, `make test-asan` 19/19, `make test-unit` PASS, `make check-sh` PASS,
  `make test-fixed-sim` PASS; `test-sim` 3/3 and `test-sim-full` 19/19 on the C-only sim ELF (the default
  `make sim` still fails in `src/engine/sh4/r_draw_sh4.S`, outside this step).
- Still open, for the gint agent (not engine-owned): `src/gint/main.c` truncates `CGD_WADFileName` to 6 characters,
  so `freedoom1` and `freedoom2` share one hashed save/demo prefix.

---

## Step 2 (2026-10-10): division hotspots, per-column glue, renderer limits (P4, B10)

The whole step is meant to be bit-exact, and no existing golden changed: `make golden` rewrote all 14 existing files
byte for byte. One golden was added, `e2m4-limits`, for frames the old engine could not render (see
`tests/golden/CHANGES.md`).

### (a) Divisions

**Finding: the baseline's "slow `__sdivsi3_i4i`" was a profiler attribution artifact.** In libgcc's SH code,
`__udivsi3_i4i` branches into code that lies after the `__sdivsi3_i4i` symbol, at `+0x2c` and `+0x52`. `simprof`
attributes by symbol, so most unsigned-division work was reported as `__sdivsi3_i4i`.

I counted call sites with the per-PC profile: the count of each `jsr` instruction, with its target taken from the
`mov.l` that loaded the register. The script was a throwaway in the scratchpad, about 50 lines: it parses
`sh-elf-objdump -d` and the `.prof` file. Per frame in e1m1:

| call site | divisions/frame | kind |
|---|---|---|
| `R_RenderSegLoop` `dc_iscale = 0xffffffffu / rw_scale` | 928 | unsigned |
| `R_PointToAngle` → `SlopeDiv` | 289 | unsigned |
| `P_UpdateSpecials` `(leveltime/speed + i) % numpics` | 154 | **signed**, 2 per animated pic |
| `R_RenderMaskedSegRange` `dc_iscale` | 35 | unsigned |
| `R_StoreWallRange` `scalestep = (scale2-scale1)/(stop-start)` | 33 | **signed** |
| `I_StartTic`, `D_DoomLoop`, host `Input_GetKeys` | 1 each | signed `%` |

- The real cost is about **48 instructions per division**, averaged over both symbols: 57.4 M instructions for 1.198 M
  calls in e1m1. That is about 64 K instructions per frame (4%) in e1m1 and 133 K (5.4%) in demo1.
- Almost all of it is the vanilla-exact **unsigned** `dc_iscale` and `SlopeDiv` divisions.
- The signed divisions were cheap, about 20 instructions each, because their operands were small.

**Changes** (`m_fixed.h`):
- `IntDivPos(n, d)` is `n / d` for `d > 0`, computed through the unsigned divider: `|n|`, `q = |n|/d`, then the sign is
  put back. The proof is in the header comment.
  - `R_StoreWallRange`'s `scalestep` uses it.
- `IntCycleFill(dest, base, count, speed, time)` is `P_UpdateSpecials`' animation loop.
  - One unsigned `time/speed` per animation, then the remainder steps by one and wraps, instead of 2 signed divisions
    per pic.
  - `P_UpdateSpecials` calls it only when `leveltime >= 0`, `speed > 0`, `basepic >= 0` and `numpics > 0`, which
    always hold (`leveltime` is saved in 24 bits). Otherwise the vanilla loop runs.
- **Test:** `tests/div_exact.c`, run by `make test-div` (host, UBSan) and `make test-div-sim` (SH, about 45 s). Both are
  in `test-unit`. 0 mismatches.
  - `IntDivPos` is checked against the 32-bit C `/` and against an int64 quotient (`__divdi3` on SH, an independent
    routine):
    - every `d` in 1..65536 with edge numerators;
    - every `n` in ±2^18 for `d` in 1..8, 319 and 320;
    - random pairs: 20 M on the host, 2 M in the simulator.
  - `IntCycleFill` is checked against the vanilla loop for all times 0..2000 × speeds 1..17 × counts 1..40 × 6
    bases, plus 200 k random cases.
- **Not changed:** `dc_iscale` and `SlopeDiv`, the cost that is really there.
  - A `div1` sequence needs 2 instructions per quotient bit, so 24 bits cost about 50 instructions: no gain over
    libgcc.
  - An exact reciprocal (table, Newton step, multiply, ±1 correction) looks possible at about 25–30 instructions. SH4
    has no count-leading-zeros instruction, so normalisation needs a small search. That would save about 1.5% and needs
    its own exactness proof. It is left for later.
  - P12's inexact reciprocal would change pixels.
- `FixedDiv` callers are each below 0.5%: `R_ScaleFromGlobalAngle` 69 per frame, `R_PointToDist` 36,
  `R_ProjectSprite` 19, at 93 instructions each. They are unchanged.

### (b) Per-column glue and small per-frame costs

- **`R_GetColumn` is `static inline` in `r_data.h`.** Its lump path goes through the new `W_CacheLumpNumConstFast`
  (`w_wad.h`).
  - The fast path returns the cached pointer with no call when `W_CacheLumpNumConst`'s `Z_ChangeTag` would change
    nothing:
    - the lump is mapped in place (new `byte *W_LumpMapped`, numlumps bytes, set where `W_ReadLumpWithZ_Malloc`
      stores a `PL_WadMap` pointer; such pointers are never purged);
    - or it is a zone block that already has the requested tag (new `Z_BlockTag()` in `z_zone.h`).
  - Otherwise it calls `W_CacheLumpNumConst`. The result and the zone state are the same in every case: a PU_STATIC
    lump is still downgraded to PU_CACHE, as vanilla does.
  - It removed the `R_GetColumn` → `W_CacheLumpNumConst` → `Z_ChangeTag2` → `PL_IsWadPointer` chain, which cost about
    70 K instructions per frame in e1m1.
  - The composite path is `R_GetColumnComposite()`. CGDoom's disabled `#if 0` composite-size hack went with the old
    function.
- **`R_RenderSegLoop` keeps its loop state in locals** and writes back `rw_x`, `rw_scale`, `topfrac`, `bottomfrac`,
  `pixhigh` and `pixlow` at the end.
  - Before, GCC reloaded and stored every global around each `R_DrawColumn` call.
  - Nothing called from the loop touches those globals: the drawers read only `dc_*`, and `R_GetColumn` reaches the
    zone and WAD code only.
  - The loop body is otherwise unchanged, line for line. Gain: about 21 K instructions per frame in e1m1.
- **`SHORTFromBytes` and `INTFromBytes` are `static inline`** in `m_swap.h`, still byte by byte. New helpers:
  - `INTFromAligned()`: one 32-bit load plus `__builtin_bswap32`, which GCC SH compiles to `swap.b`/`swap.w`/`swap.b`.
    For 4-aligned data only.
  - `PatchColumnOfs(patch, aligned, col)`.
  - `R_DrawVisSprite`, `V_DrawPatch` and `V_DrawPatchFlipped` test the patch's alignment once and use the aligned load
    when it holds.
  - Misaligned lumps still read bytewise. They happen with `TrustUnalignedLumps` or in copy mode.
  - A `_Static_assert` in `r_defs.h` pins `columnofs` at byte 8.
- **Small `memcpy` calls.** fxlibc's `memcpy` copies anything below 64 bytes byte by byte.
  - `V_CopyRect` rows: the status-bar widgets do about 100 per frame of a few tens of bytes. They now use word copies
    (`V_CopyRow`) when source and destination are equally aligned and `n < 64`; otherwise `memcpy`.
  - `R_StoreWallRange`'s sprite-clip saves now use a short loop (`R_CopyClip`). I checked that GCC 14 does not turn it
    back into `memcpy`.
  - The gint build links gint's own `memcpy`, so the sim gain overstates the calculator gain here.
- **Keyboard:** `CGD_UpdateKeyboardState` records whether the scan changed (`st_changed`). If it did not,
  `CGD_KeyWasJustPressed` and `CGD_KeyWasJustReleased` return 0 at once.
  - That is exact: with equal scans `KeyDown(prev) == KeyDown(now)` for every key.
  - It was about 10 K instructions per frame of keymap decoding. Now it costs nothing while the keys do not change. The
    scripted inputs change keys often, so `KeyDown` still shows about 3 K per frame in e1m1.
- **Design §8 items in the keyboard file:**
  - Added `int CGD_EscapeKeycode = KEYCODE_MENU;` (`cgdoom-kbd.h`). `DoomKeyToKeycode(KEY_ESCAPE)` returns it when the
    keymap has no entry. gint's `keymaps.c` already declares it weak and sets it to BACK.
  - Replaced the stale "CG100 keymaps: TODO" comment.
- **Not done:** inlining `R_PointOnSide`. It has 49 calls per frame in e1m1 and 152 in demo1, so it would save about
  1.5 K instructions per frame, below the noise of the other items.
- **Left as it is:** `gametic % (CGD_Frameskip+1)`, 3 signed divisions per frame. Negligible.

### (c) Renderer limits (design B10)

**Arrays:** visplanes, drawsegs, vissprites and openings are no longer static arrays. They come from the zone through
`R_AllocZeroed` (PU_STATIC, zero-filled), are allocated at `R_Init` and the first frame, and grow on demand.

| array | initial (vanilla) | grows by | hard limit | at the limit |
|---|---|---|---|---|
| visplanes | 128 | blocks of 64; visplanes never move | 1024 | `I_Error("R_FindPlane: no more visplanes")`, as vanilla at 128 |
| drawsegs | 256 | doubling, with `ds_p` rebased (`R_GrowDrawSegs`) | 2048 | the wall is dropped, as vanilla at 256 |
| vissprites | 128 | doubling, with `vissprite_p` rebased (`R_GrowVisSprites`) | 1024 | `overflowsprite`, as vanilla |
| openings | 320×64 shorts | further blocks of the same size; `R_ReserveOpenings` reserves `3*(stop-start+1)` per wall | 16 blocks | the wall is dropped (vanilla had **no** check at all and overwrote memory) |

- **Why growth never changes pixels:**
  - Each slot keeps its storage once allocated and starts zeroed, exactly like a slot of vanilla's zero-initialized
    static array.
  - The stale contents matter: `R_DrawPlanes` reads `bottom[]` of columns a visplane does not cover. Vanilla left
    zeros or earlier frames' valid rows there.
  - Openings are always written before they are read.
  - Only the BSP walk holds drawseg and vissprite pointers while the arrays grow (`ds_p`, `vissprite_p`).
- **Visplane hashing in `R_FindPlane`:** 64 buckets, chains appended in creation order, new `visplane_t.next` field.
  - All planes with one key share a bucket, so the first match in the chain is vanilla's first match in its linear
    scan.
  - `R_DrawPlanes` walks planes in creation order.
  - Effect: `R_FindPlane` drops from 17 K instructions per frame to almost nothing in demo1. The bookkeeping costs about
    the same in e1m1, so the net is −0.2% in demo1 and +0.1% in e1m1. It grows in value with plane count, which is the
    point of the higher limits.
- **New statistics in `CGD_Stats` (`cgdoom.h`):**
  - `VisplanesPeak`, `DrawsegsPeak`, `VisspritesPeak`, `OpeningsPeak`, recorded in `R_DrawPlanes`;
  - `FramesOverVanillaLimits`, `DrawsegsDropped`, `VisspritesDropped`;
  - `RenderArrayBytes` and its peak.
  - The host's `-stats` prints them on a `render limits:` line.
- **Test knob:** `CGD_SmallRenderLimits`, set with host `-rlimits small`, starts every array tiny: 64 visplanes,
  4 drawsegs, 4 vissprites, openings blocks of 960 shorts. Every frame then runs the growth code.
  - New scenarios `e2m4-rsmall` (with `-poison`) and `map01-rsmall` must reproduce `e2m4-limits` and `map01` exactly.
    They do, on the host, under ASan and in the simulator.
- **Memory (SH sim ELF):**
  - `.bss` went from 292,160 to 146,976 bytes (−145 KB). This meets design §8's request to move the big renderer
    arrays out of `.bss`.
  - The zone now holds 143 KiB at start: 2 visplane blocks of 64 × 672 B, 40 KiB of openings, 256 drawsegs and 128
    vissprites.
  - In `e2m4-limits` it grows to 270 KiB, with a peak of 294 KiB while drawsegs double.
  - Worst case at the hard limits is about 1.5 MiB (visplanes alone 688 KiB). It is reached only by pathological
    views. Scanning every Freedoom map with `builtin:noclip` for 700 tics found peaks of 251 visplanes, 547 drawsegs,
    215 vissprites and 4,737 openings.
- **Old limits in practice:** that scan found 14 of 32 Doom II maps and 9 of 36 Doom maps going over a vanilla limit.
  In 7 of them, MAP01, MAP24 and MAP27 among them, it was the visplane limit, which made the old engine stop with
  `I_Error`.

### Performance (`make perf-sim`, C-only sim ELF, instructions per frame, frames 42..end)

Every stage reproduced the goldens (CRC `ok`). The stages are cumulative.

| scenario | step 1 | (a) div | (b) glue | (c) limits | (d) seg loop | total Δ insts | step 1 cycles | final cycles | Δ cycles |
|---|---|---|---|---|---|---|---|---|---|
| e1m1 | 1,578,304 | 1,574,852 | 1,519,589 | 1,520,917 | 1,499,962 | −5.0% | 1,711,555 | 1,625,596 | −5.0% |
| e1m2 | 1,355,399 | 1,352,071 | 1,313,809 | 1,311,516 | 1,302,923 | −3.9% | 1,469,969 | 1,408,280 | −4.2% |
| e1m3 | 1,179,345 | 1,175,875 | 1,135,571 | 1,136,277 | 1,126,034 | −4.5% | 1,275,504 | 1,214,711 | −4.8% |
| e2m2 | 846,840 | 843,482 | 812,340 | 809,300 | 808,070 | −4.6% | 923,264 | 876,044 | −5.1% |
| e3m4 | 1,518,998 | 1,515,558 | 1,465,081 | 1,466,208 | 1,452,659 | −4.4% | 1,648,696 | 1,573,636 | −4.6% |
| demo1 | 2,479,911 | 2,476,898 | 2,400,213 | 2,394,762 | 2,376,491 | −4.2% | 2,689,190 | 2,574,402 | −4.3% |
| map01 | 1,360,375 | 1,356,785 | 1,312,911 | 1,313,183 | 1,300,114 | −4.4% | 1,470,074 | 1,401,357 | −4.7% |
| map07 | 1,190,355 | 1,186,932 | 1,143,389 | 1,144,274 | 1,137,034 | −4.5% | 1,288,844 | 1,226,909 | −4.8% |
| demo2-1 | 1,829,810 | 1,826,491 | 1,767,673 | 1,768,066 | 1,740,878 | −4.9% | 1,979,733 | 1,880,174 | −5.0% |

- **What each stage includes:**
  - (a) `IntDivPos` and `IntCycleFill`.
  - (b) everything in the glue section above except the `R_RenderSegLoop` locals.
  - (c) growable arrays, visplane hashing, `R_ReserveOpenings` inline, a local `drawsegs` base in `R_DrawSprite`.
  - (d) the `R_RenderSegLoop` locals and `drawsegs_end`.
- **The e1m1 profile now:** `R_DrawColumn` 53.1%, `R_RenderSegLoop` 13.3%, `R_DrawSpan` 7.7%, `R_MakeSpans` 3.1%,
  `R_DrawMaskedColumn` 3.1%, unsigned division 4.2% (split over the two libgcc symbols), `R_StoreWallRange` 1.7%,
  `V_DrawPatch` 1.7%.
- `.text` grew from 272,076 to 277,024 bytes, from the inlined `R_GetColumn` and byte readers and the new code.
- **Reproduce:**
  - Build: `make SIM_BUILD=<dir> CGSIM_ASM_SRC= 'CGSIM_C_SRC=$(ENGINE_SRC) $(HOST_SRC)' sim`.
  - Measure: `sh src/sim/perf-sim.sh -bin <dir>/cgdoom-sim.elf -out <dir>/perf`.

### Test results

| test | result |
|---|---|
| `make test-host` | 22/22 PASS: 19 old scenarios, plus `e2m4-limits`, `e2m4-rsmall`, `map01-rsmall` |
| `make test-asan` | 22/22 PASS |
| `make test-unit` | PASS: fixed, div, names, round trip |
| `make test-fixed-sim`, `make test-div-sim` | PASS, 0 mismatches |
| `make check-sh` | all engine files compile, no warnings |
| `test-sim`, C-only ELF | 3/3 PASS |
| `test-sim-full`, C-only ELF | 22/22 PASS (148 s) |

The default `make sim` still fails because `src/engine/sh4/r_draw_sh4.S` does not assemble ("opcode not valid for this
cpu variant"). That is in the asm agent's code: its DSP opcodes need the `sh4al-dsp` ISA, and the `.S` rule in
`src/sim/cgdoom.mk` passes only `CGSIM_CFLAGS`. `test-sim` and `test-sim-full` were therefore run with
`SIM_BUILD=<scratch> CGSIM_ASM_SRC= 'CGSIM_C_SRC=$(ENGINE_SRC) $(HOST_SRC)'`.

### Risks

- **`W_CacheLumpNumConstFast` skips the `numlumps` range check.** Only `R_GetColumn` uses it, with lump numbers from
  validated texture definitions.
  - Any new code that sets `lumpcache[i]` to a mapped pointer must also set `W_LumpMapped[i]`. The only such place is
    `W_ReadLumpWithZ_Malloc`.
- **Renderer arrays live in the zone now.**
  - Growth is a PU_STATIC `Z_Malloc` in the middle of a frame. It can purge PU_CACHE lumps, which vanilla's own
    mid-frame `W_CacheLumpNum` calls do as well.
  - On zone exhaustion it fails with `Z_Malloc` `I_Error`.
  - Doubling drawsegs briefly holds the old and the new array.
- `R_RenderSegLoop` assumes the column drawers and `R_GetColumn` do not touch the `rw_*`, `topfrac`/`bottomfrac`,
  `pixhigh`/`pixlow`, `walllights`, `markceiling`/`markfloor` or `ceilingplane`/`floorplane` globals. **An asm drawer
  must not read them.** `R_DrawColumn` reads only `dc_*` and the screen tables.
- `visplane_t` grew by one pointer, to 672 bytes on SH.

### For later agents

- **Division cost is unsigned now.** `dc_iscale` and `SlopeDiv` are about 48 instructions each, 4–5% of the frame. An
  exact reciprocal is the remaining lever (see (a)). Read `__sdivsi3_i4i` in `simprof` output as libgcc division in
  general.
- **Call counts per call site:** the per-PC profile makes them exact. Count the `jsr` instruction and resolve its
  register from the preceding `mov.l ... ! <sym>` in `objdump -d`.
- `R_GetColumn` is a header inline now. Code that needs the column directory uses `texturecolumnlump`,
  `texturecolumnofs`, `texturewidthmask` and `texturecomposite` from `r_data.h`.
- **Renderer limit and memory data:** `-stats` prints the `render limits:` line.
- **Adding a growable renderer array:** follow `R_AllocZeroed` and `R_FreeArray`. Zero-fill it, never move storage that
  something still points to, and add a `-rlimits small` path plus a golden-matching scenario.
- **The `R_RenderSegLoop` locals pattern** probably also pays in `R_RenderMaskedSegRange` and `R_DrawMaskedColumn`.
  `R_DrawMaskedColumn` is 10% of demo1 and calls `colfunc` per post.

### Step 2 review (2026-10-10, adversarial reviewer)

No functional defect confirmed; the only source change is a comment. All step-2 changes were read against
`ref/CGDoom/cgdoom` and checked as below.

- **Fixed (documentation):** the `m_fixed.h` comment above `IntDivPos` still said libgcc's signed `__sdivsi3_i4i`
  costs 160-580 instructions against about 30 for the unsigned one, contradicting finding (a). A sim micro-benchmark
  (noinline `n / d` against a noinline `n + d`, 20 k precomputed operand pairs per kind) measured the two as equal
  within one instruction: about 33 per call for small operands (`leveltime/8`, `% numpics`), 48 for `scalestep`-like
  operands (|n| < 2^17, d in 1..320), 58 for |n| near 2^30. The comment now says so. The gain of (a) comes from
  `IntCycleFill` doing fewer divisions, not from the unsigned divider being cheaper.
- **Checked, no defect:**
  - `R_RenderSegLoop` locals: nothing reached from the loop (`R_GetColumn`, `R_GetColumnComposite`, zone, WAD,
    `R_DrawColumn`) reads or writes the cached globals; `R_RenderMaskedSegRange` (which writes `rw_scalestep` and
    `walllights`) runs only from `R_DrawMasked`. The gint build's `R_RenderSegLoop` was read back too.
  - `W_CacheLumpNumConstFast`: `W_LumpMapped[lump]` can only be 1 while `lumpcache[lump]` is the mapped pointer
    (mapped pointers are never purged or cleared, and `W_ReadLumpWithZ_Malloc` has no outside callers);
    `KEY_ESCAPE` is not in `CGD_KeymapEntries`, so `CGD_EscapeKeycode` is always used.
  - Growth paths at the hard limits: a scratch ASan+UBSan build with `MAXDRAWSEGS 64`, `MAXVISSPRITES 16`,
    `MAXOPENINGS_BLOCKS 3` and `-rlimits small` ran e2m4 noclip, demo1 (`-poison 0xff`) and map07 (`-poison 0x5a`)
    with tens of thousands of dropped walls and sprites: no ASan/UBSan report.
  - `-rlimits small -poison 0xa5` under ASan reproduces the goldens of demo1, demo2-1, e3m4, map07, map30-cast and
    e1m1-exit (`-fragment 4096`), beyond the two scenarios in `tests/scenarios.txt`.
  - `R_CopyClip` and `V_CopyRow` stay loops (no `memcpy` relocation in `R_StoreWallRange`/`V_CopyRect`, sim and gint
    objects). The gint build (`cmake` + fxsdk toolchain into a scratch directory) compiles with no warnings;
    `.bss` 71,872 bytes.
- **Not changed, noted as risks:**
  - Growth is a PU_STATIC `Z_Malloc`; when the zone cannot supply it the game stops with `Z_Malloc` `I_Error`
    where vanilla dropped the wall or sprite (vanilla's 256 drawsegs / 128 vissprites). Only possible on a nearly
    full, fragmented zone.
  - `drawsegs`/`vissprites` are first allocated on the first rendered frame, after `P_SetupLevel`, so their
    PU_STATIC blocks sit among level data. Allocating them in `R_Init` instead was tried: the smallest zone that
    runs e1m1-exit in copy mode (`-fragment 4096`) went from 1950 to 1900 KiB, demo2-1 (`-fragment 4096`) stayed
    at 2000 KiB, and the thresholds were not monotonic. Not significant, so not kept.
- Tests after the change: `make test-host` 22/22, `make test-asan` 22/22, `make test-unit` PASS, `make check-sh`
  PASS, `make test-div-sim` and `make test-fixed-sim` PASS; on the C-only sim ELF `test-sim` 3/3 and
  `test-sim-full` 22/22. The default `make sim` still fails in `src/engine/sh4/r_draw_sh4.S` (outside the engine
  ownership: the `.S` rule in `src/sim/cgdoom.mk`).

---

## Step 3 (2026-10-10): low detail, view size, potato floors, auto frameskip, WAD RAM residency (P5, P10, §7)

The default mode (high detail, view size 10, textured floors, fixed frameskip) is unchanged: `make golden` rewrote
all 14 earlier goldens byte for byte. Each new mode is optional and has its own golden scenarios
(`tests/golden/CHANGES.md`, step 3 entry): 10 new goldens and 3 new `-slowwad` variants of existing goldens, 35
scenarios in all.

### (a) Low detail (`detailshift = 1`)

- **`r_draw.c`:** new `R_DrawColumnLow`, `R_DrawFuzzColumnLow`, `R_DrawTranslatedColumnLow` and `R_DrawSpanLow`.
  - They are copied from `src/engine/sh4/test/ref_draw.c`, the reference the SH4 versions are bit-exact against.
  - They follow Chocolate Doom's semantics (design §8): `dc_x` is not modified; the span count is taken before
    `ds_x1`/`ds_x2` are doubled on the globals.
  - The only additions are `RANGECHECK` blocks. They remain plain reference code.
- **Vanilla drawer pointers restored** (design §8):
  - `r_main.c` defines `transcolfunc` and `spanfunc` next to `colfunc`/`basecolfunc`/`fuzzcolfunc`, declared in
    `r_main.h`.
  - `r_segs.c`: the three `R_DrawColumn()` calls are now `colfunc()`.
  - `r_plane.c`: `R_MapPlane` calls `spanfunc()`, and the sky calls `colfunc()`.
  - `r_things.c`: `colfunc = transcolfunc`.
- **`R_ExecuteSetViewSize` is the only place that picks the drawers.**
  - The choice follows `detailshift = setdetail`. Low detail uses the Chocolate drawers, fuzz and translated included.
  - It has a marked comment where an SH4 build (`__sh__ && !CGD_NO_SH4`) will select the `*_SH4` versions.
  - The asm integration step must also keep `R_DrawSpanFlat[Low]` for potato floors (C only).
- **`R_SetViewSize(blocks, detail)`** has vanilla's signature again.
- **Menu:** `M_ChangeDetail` toggles detail as in Chocolate Doom ("High detail"/"Low detail"). CGDoom showed a
  "press [+] or [-]" message instead.
- **Options and host flag:**
  - New option `CGD_Options.LowDetail`, read in `R_Init`, saved and loaded as `LowDetail=` in `CGDOOM.CFG`.
  - Run-time setter `CGD_SetLowDetail()` in `cgdoom.h`. It also keeps `CGD_Options` up to date.
  - Host flag `-lowdetail`.
- **`hu_lib.c`:** HUD text erase uses `scaledviewwidth` for the right border. Vanilla's `viewwidth` was half the view in
  low detail.

### (b) View size

- **`screenblocks`** comes from `CGD_Options.ViewSize` (default 10, kept in `CGDOOM.CFG`).
  - The `SKEY_DECVP`/`SKEY_INCVP` keys step it through 3..11, vanilla's range. They call `CGD_SetViewSize()`.
  - CGDoom had a private static counter limited to 1..10.
  - Size 11 is full screen with no status bar, now reachable.
  - Host flag `-viewsize N`.
- **Reduced-view border** (`d_main.c`, `D_Display`):
  - The border is redrawn black (`R_DrawViewBorder`) when the menu is active or was active in the previous frame, when
    the automap was active in the previous frame, after a help screen, and after a game-state or view-size change.
  - CGDoom never redrew it, so the automap, menus and intermission screens left garbage in the border.
  - It is done before `HU_Drawer` (vanilla did it after), so HUD messages drawn in the border stay.
- **`R_VideoErase` clears to 0.** It used to copy from `screens[1]`, which CGDoom uses for the intermission background,
  so HUD text erases pasted pieces of the last intermission into the border.
- **Full-screen automap:** at size 11 the status bar is redrawn in full when the automap opens. CGDoom's automap does
  not send vanilla's `AM_MSGENTERED` to `ST_Responder`.
- The existing `builtin:noclip` goldens already went down to size 8 and back, and are unchanged.

### (c) Potato floors (`CGD_Options.FlatFloors`, `CGD_SetFlatFloors()`, host `-flatfloors`)

- **Drawers:** `R_DrawSpanFlat` and `R_DrawSpanFlatLow` (`r_draw.c`) fill the span with `ds_colormap[ds_flatcolor]`.
  The fill writes bytes up to a 4-byte boundary, then 32-bit words. Lighting and the invulnerability colormap still
  apply.
- **`R_FlatColor`** (`r_plane.c`) computes the colour once per flat lump, the first time the flat is drawn, and caches it
  in `flat_colors` (2 bytes per flat, PU_STATIC).
  - It takes the rounded average RGB of the flat's pixels in PLAYPAL palette 0, then the nearest palette entry by
    squared RGB distance, the lowest index on ties.
  - Animated flats are translated first, so each frame of the animation has its own colour.
- **Planes:** `R_DrawPlanes` does not load the flat lump in this mode. It uses `R_MakeSpansFlat` and `R_MapPlaneFlat`:
  only the row distance for lighting is computed, with no `xfrac`/`yfrac`/`step` math.
  - The choice is made once per visplane, so the default `R_MakeSpans`/`R_MapPlane` path tests nothing more per span.
- **Switching:** the mode is read in `R_ExecuteSetViewSize` (`r_flatfloors`), so it only changes between frames.

### (d) Automatic frameskip (`CGD_AutoFrameskip`, design §8, P10)

- **`CGD_AutoFrameskip`:** `0` keeps CGDoom's fixed `CGD_Frameskip` exactly (same code path). `N > 0` turns on
  automatic frameskip (`i_system.c`):
  - tic *g* is due at `auto_base + g` in `PL_GetTics()` time, and `I_StartTic` waits while it is early;
  - after the tic, `I_DisplayDue()` skips `D_Display` when the next tic is already due, at most N times in a row;
  - the lag is capped at N tics (`auto_base` moves forward). After a long stall, or when rendering costs more than N+1
    tics, the game slows down instead of fast-forwarding.
- `CGD_AUTOFRAMESKIP_DEFAULT` is 3. Game logic is unaffected: same ticcmds, same demos. The end state is identical
  across all modes (checked with `-gamelog`).
- **`D_DoomLoop`:** `if (I_DisplayDue()) { D_Display(); I_FrameDisplayed(); }`.
  - The FPS counter (`I_FrameDisplayed`) now counts rendered frames instead of dividing tics by the fixed frameskip.
- **Frameskip key:** fixed 0 → 1 → 2 → 3 → auto (the last automatic N) → fixed 0. The message says "Frameskip: auto
  (max N)".
- **Host:** the virtual clock is unchanged by default (one tic per presented frame), so every existing run is
  reproducible as before.
  - `-autoframeskip N` sets the auto mode.
  - `-rendercost T` / `-ticcost T` make a presented frame or a game tic advance the virtual clock by T tics (decimal,
    1/1000 resolution). This is a deterministic "slow renderer" for goldens.
  - `-realtime` (host only, not in the simulator) paces against the wall clock.
- **Measured** (e1m1, explore, 900 tics, max skip 3):

| render cost (tics) | frames shown |
|---|---|
| 1.6 | 587 |
| 2.5 | 377 |
| 6 | 233 (one frame every 4 tics, the cap) |

  The wipe frames are included and depend on the cost.

### (e) WAD RAM residency (`PL_WadIsSlow`, design §7, research-critique C1)

- **`platform.h` addition:** `int PL_WadIsSlow(void);` is 1 when `PL_WadMap` pointers are in slow memory (NOR flash),
  0 when they are as fast as the zone.
  - `cgdoom.c` has a **weak default returning 0**, so the calculator build links before gint implements it.
  - The host implements it, controlled by `-slowwad` (default 0).
  - `W_InitMultipleFiles` reads it once into `W_WadSlow`.
- **`W_ReadLumpWithZ_Malloc`:** with a slow mapping, a mappable (and aligned) lump is copied into the zone with the
  caller's tag, so PU_CACHE copies stay purgeable, as long as the zone keeps a reserve after the copy.
  - The reserve is max(25% of the total zone, 256 KiB). Free means zone size minus `CGD_Stats.ZoneUsed`, so cached
    copies count as used and never evict each other.
  - Otherwise the lump is referenced in place, as before.
  - Copies carry the same `W_LUMP_PAD` bytes, so the output is identical: `-slowwad` reproduces all 35 goldens on the
    host and under ASan with `-poison`, including 2–3 MiB zones.
- **`W_SlowLevelStart()`** runs in `P_SetupLevel` after `Z_FreeTags(PU_LEVEL..)`, slow mode only.
  - It purges every PU_PURGELEVEL..PU_CACHE block and forgets the lumps referenced in place.
  - Without it, the previous level's copies would keep the zone at the reserve, and every lump of the next level would
    stay in flash.
  - A forgotten mapped pointer that something still holds stays valid; `Z_Free`/`Z_ChangeTag` ignore WAD pointers.
- **`R_PrecacheLevel`** (`r_data.c`, vanilla) is enabled again and called by `P_SetupLevel` when `W_WadSlow`. It runs
  during demos too.
  - Order: flats (skipped with potato floors), then wall-texture patches, then the sprites of the things in the level,
    until the reserve is reached.
- **Statistics:** new `CGD_Stats.SlowLumpsCopied`, `SlowLumpsCopiedTotal` and `SlowLumpsInPlace`, printed by host
  `-stats` on the `lumps:` line.
- **Extra zone usage** (SH simulator, 32-bit). By design, the copies fill the zone up to the reserve, and they are all
  purgeable. The non-purgeable peak grows only by the PU_STATIC lumps that are now copies (status bar, fonts,
  `COLORMAP`):

| run | zone | peak used, default | peak used, `-slowwad` | static peak, default → slow | copied (count, KiB) | in place |
|---|---|---|---|---|---|---|
| e1m1, 900 tics | 4 MiB | 1141 KiB | 3225 KiB | 1115 → 1120 KiB | 910, 4433 KiB | 1684 |
| e1m1-exit (level change) | 4 MiB | 1484 KiB | 3226 KiB | 1338 → 1356 KiB | 1279, 6366 KiB | 1974 |
| demo2-1 (level change) | 4 MiB | 1536 KiB | 3237 KiB | 1440 → 1452 KiB | 1424, 6036 KiB | 2259 |
| e1m1 | 16 MiB | 1141 KiB | 12426 KiB | 1115 → 1120 KiB | 2075, 13487 KiB | 519 |

  Freedoom's E1M1 touches about 12.6 MiB of lumps in 900 tics, which is more than a 4 MiB zone holds. Lumps purged
  by later allocations are copied again when they are used next, so the copied total can exceed the zone. The
  shareware doom1.wad that fits on the calculator has much smaller levels.

### (f) Performance (`make perf-sim`, C-only sim ELF, frames 42..end)

**Default mode, step 2 → step 3:** +0.06% to +0.15% instructions. The vanilla `colfunc`/`spanfunc` pointers cost one
load per column and span call, about 1–2 K instructions per frame. Every run reproduced its golden.

| scenario | insts/frame step 2 | step 3 | Δ | cycles/frame step 2 | step 3 | Δ |
|---|---|---|---|---|---|---|
| e1m1 | 1,499,962 | 1,500,945 | +0.07% | 1,625,596 | 1,626,629 | +0.06% |
| e1m2 | 1,302,923 | 1,304,290 | +0.10% | 1,408,280 | 1,409,748 | +0.10% |
| e1m3 | 1,126,034 | 1,126,761 | +0.06% | 1,214,711 | 1,215,429 | +0.06% |
| e2m2 | 808,070 | 809,289 | +0.15% | 876,044 | 877,369 | +0.15% |
| e3m4 | 1,452,659 | 1,453,684 | +0.07% | 1,573,636 | 1,574,661 | +0.07% |
| demo1 | 2,376,491 | 2,378,752 | +0.10% | 2,574,402 | 2,576,746 | +0.09% |
| map01 | 1,300,114 | 1,301,013 | +0.07% | 1,401,357 | 1,402,326 | +0.07% |
| map07 | 1,137,034 | 1,138,143 | +0.10% | 1,226,909 | 1,228,089 | +0.10% |
| demo2-1 | 1,740,878 | 1,742,870 | +0.11% | 1,880,174 | 1,882,322 | +0.11% |

A first version kept `colfunc` in a local of `R_RenderSegLoop`. That cost about 1.6 K instructions per frame in register
pressure, more than one global load per call. Its first potato-floor test sat in `R_MapPlane` and cost 0.5 K. Both
were changed before these numbers were taken.

**Modes, instructions per frame** (same scenarios plus the mode flags; the game state is the same as the default run).
The runs that are also golden scenarios reproduced their goldens: `e1m1-low`, `demo1-low`, `map01-flat` and
`e1m1-view7`.

| scenario | default | `-lowdetail` | `-flatfloors` | `-viewsize 9` | `-viewsize 7` | low + flat |
|---|---|---|---|---|---|---|
| e1m1 | 1,500,945 | 933,620 (−37.8%) | 1,372,591 (−8.6%) | 1,248,763 (−16.8%) | 892,518 (−40.5%) | 845,930 (−43.6%) |
| map01 | 1,301,013 | 808,113 (−37.9%) | 1,140,916 (−12.3%) | 1,065,902 (−18.1%) | 740,353 (−43.1%) | 700,133 (−46.2%) |
| demo1 | 2,378,752 | 1,533,152 (−35.5%) | 2,016,194 (−15.2%) | 2,046,266 (−14.0%) | 1,541,322 (−35.2%) | 1,285,660 (−46.0%) |
| demo2-1 | 1,742,870 | 1,129,272 (−35.2%) | 1,386,669 (−20.4%) | 1,458,397 (−16.3%) | 1,056,937 (−39.4%) | 881,096 (−49.4%) |

Cycles move the same way, within 0.6 points. For example, e1m1 goes from 1,626,629 to 1,006,209 cycles in low detail
(−38.1%), 1,493,783 with potato floors (−8.2%), and 971,826 at view size 7 (−40.3%).

- **Low detail with the C drawers:** the gain is about 36–38%. The C low drawers still do two byte stores per texel.
  The asm agent's `R_DrawColumnLow_SH4`/`R_DrawSpanLow_SH4` model 2.25–3.25 cycles per pixel, so the gain on hardware
  should be larger.
- **Potato floors** remove `R_DrawSpan` (7.7% of e1m1, 11% of map01 in C) and most of `R_MapPlane`.
  - What remains is `R_MakeSpans`' bookkeeping, now inlined into `R_DrawPlanes`: 6% of map01-flat.
  - The fill itself (`R_FillSpanBytes`) is 1% of map01-flat.

### Tests

| test | result |
|---|---|
| `make test-host` | 35/35 PASS: 22 old, 10 new goldens, 3 `-slowwad` variants |
| `make test-asan` | 35/35 PASS |
| all 22 earlier scenarios with `-extra "-slowwad"` (host) and `"-slowwad -poison 0x77"` (ASan) | 22/22 PASS against their goldens |
| `make test-unit` | PASS: fixed, div, names, round trip |
| `make check-sh` | PASS, no warnings |
| `test-sim`, C-only ELF | 3/3 PASS |
| `test-sim-full`, C-only ELF | 35/35 PASS |
| gint build (`cmake` with the fxSDK toolchain, scratch build directory) | compiles and links, no warnings |

- The default `make sim` still fails in `src/engine/sh4/r_draw_sh4.S`, which needs `-Wa,--dsp` in `src/sim/cgdoom.mk`.
  That is outside the engine's ownership.
- The C-only ELF was built with `make SIM_BUILD=<dir> CGSIM_ASM_SRC= 'CGSIM_C_SRC=$(ENGINE_SRC) $(HOST_SRC)' sim`.
- `generate_g3a` writes `CGDoom.g3a` into the source directory, so the gint build check rewrote the repository's
  `CGDoom.g3a` with a build of the current tree.

### Risks

- **Weak `PL_WadIsSlow`:** until gint defines it, the calculator always behaves as a fast mapping. That is the old
  behaviour, so nothing is lost, but nothing is gained in flash mode either.
- **Zone usage in slow mode:** the zone sits at about 75% used, by purgeable copies.
  - Allocations then purge copies (vanilla `Z_Malloc`), which costs a copy again later. Lumps purged in the middle of
    a level are copied again at their next use if there is room.
  - Fragmentation can make a large PU_STATIC allocation purge many cached lumps.
- **`W_SlowLevelStart` drops all PU_CACHE blocks at level start,** composite textures included. Code that keeps a
  PU_CACHE pointer across `P_SetupLevel` without re-caching would break. That is a vanilla rule (any `Z_Malloc` may
  purge), and nothing in the engine does it, but asm or platform code must not either.
- **Low detail:** `R_DrawTranslatedColumnLow` is not covered by any golden. Translated sprites only exist for other
  players in netgames. `R_DrawFuzzColumnLow` is covered by `map07-low`.
- **View size 11:** the status bar appears only with the automap. The full-screen automap path was broken in CGDoom
  (no `AM_MSGENTERED`) and is fixed here.
- **Settings file:** `CGDOOM.CFG` gains `LowDetail=`, `FlatFloors=` and `ViewSize=` lines. The file version is
  unchanged, because unknown keys were already ignored. An old file leaves the defaults.

### For later agents

- **gint (platform agent) — required:**
  1. Implement `int PL_WadIsSlow(void)`: **1 in flash block-map or BFile mode, 0 when the WAD image is in RAM.** The
     engine's weak default returns 0.
  2. Set `CGD_AutoFrameskip` to the maximum number of skipped frames, e.g. `CGD_AUTOFRAMESKIP_DEFAULT` (3), not to 1.
     `src/gint/main.c` currently sets it to `(g_opt.frameskip < 0)`, i.e. 1, which allows only one skipped frame in a
     row.
  3. Optionally expose `CGD_Options.LowDetail`, `FlatFloors` and `ViewSize` in the launcher. They are already saved and
     loaded with `CGDOOM.CFG`. The engine updates them when the player changes detail or view size in the game, so
     saving the options after the game keeps them.
- **asm integration:**
  - Replace the drawer choice in `R_ExecuteSetViewSize`, the single marked block.
  - Keep `spanfunc = R_DrawSpanFlat[Low]` when `r_flatfloors` is set.
  - The low-detail C drawers are the bit-exact references, copied from `ref_draw.c`.
  - `R_FillSpanBytes` is a candidate for `cgd_fill32`-style code if potato floors matter.
- **Tests:** add a golden whenever a mode changes. `-rendercost`/`-ticcost` give deterministic frame pacing, so any
  pacing change can be tested without real time.

### Step 3 review (2026-10-10, adversarial reviewer)

One defect was confirmed and fixed. The rest of step 3 was read against `ref/CGDoom/cgdoom`, Chocolate Doom's
semantics and `src/engine/sh4/test/ref_draw.c`, and checked as below.

- **Fixed: slow-mode precaching ignored the reserve for unmappable lumps (`r_data.c`, `w_wad.c`).**
  - `W_ReadLumpWithZ_Malloc` applies the reserve test (`W_CopySlowLump`) only to mappable lumps. Unmappable lumps are
    always copied: all of them in BFile mode, which gint is asked to report as slow, and lumps that cross flash blocks
    or are misaligned in block-map mode.
  - So `R_PrecacheLevel` read the level's whole flat/texture/sprite set from slow storage, filled the zone to 100%,
    and purged most of it again before use, contrary to "until the reserve is reached".
  - Evidence, host e1m1 explore with 900 tics, `-fragment 16 -zone 2048` (BFile-like). Lumps read:
    - without `-slowwad`: 2349 lumps, 14,050 KiB (12+ MiB of it is `R_InitSprites`' startup scan);
    - with `-slowwad` before the fix: 2862 lumps, 15,781 KiB, i.e. 1.7 MiB of extra slow reads per level start, in a
      zone with only about 0.6 MiB to spare;
    - after the fix: 2388 lumps, 14,197 KiB. With a 4 MiB zone the extra reads stay about 1.4 MiB, now bounded by the
      reserve and kept in the zone.
  - **Fix:** new `W_SlowPrecacheWanted(lump)` (`w_wad.h`/`.c`). `R_PrecacheLevel` caches a lump only when it is not
    cached yet and a copy keeps the zone above the reserve.
    - Already cached lumps are skipped: re-caching them with PU_CACHE only changed their tag, which would have demoted
      a lump someone holds PU_STATIC.
    - Mappable lumps past the reserve are no longer pinned in place by the precache. They are referenced in place at
      first use, as before, or copied if room has been freed meanwhile.
  - Output unchanged: `-slowwad` reproduces every golden, also with `-fragment 4096` and with `-fragment 16 -zone 2560`
    (35/35 each, ASan with `-poison`).
- **Checked, no defect:**
  - **Low-detail drawers:** identical to `ref_draw.c` apart from the RANGECHECK blocks; `dc_x` is not modified; the
    span count is taken before the doubling.
  - **Drawer selection:** `colfunc`/`transcolfunc`/`spanfunc` are selected in one place.
  - **Low-detail tables:** `yslope`, `scalelight`, `pspritescale` and the sky `dc_iscale` follow vanilla.
  - **ASan+UBSan matrix:** view sizes 3..11 × {`-lowdetail`, `-flatfloors`, both}, with `-slowwad -poison 0x5a
    -zone 3000` on demo2-1 (1500 tics). No report, and no RANGECHECK `I_Error`.
  - **Border and HUD erase:** read through, and a 50-frame montage of `builtin:options` inspected (low detail, sizes
    7/9/10/11, automap and menu over reduced and full-screen views): no leftovers in the border, status bar present
    with the full-screen automap.
    - `R_DrawViewBorder` never touches the status-bar rows (`viewheight` is a multiple of 8).
  - **Potato floors:**
    - `R_FlatColor` pins the flat and PLAYPAL as PU_STATIC while it averages, so its own `Z_Malloc` cannot purge them.
    - The `cachedheight` sharing with `R_MapPlane` is safe: `R_ClearPlanes` resets it every frame, and the mode only
      changes between frames.
    - The word fill is aligned and endian-neutral.
    - GCC turns the 0..3-byte tail loop of `R_FillSpanBytes` into a `memset` tail call. Unrolling it was measured at
      −0.05% (map01-flat) and −0.2% (e2m2-lowflat) instructions per frame, so it was not kept.
  - **Auto frameskip:**
    - The game state is identical with and without it (`-gamelog` end state, e1m1 900 tics, `-rendercost 50` and
      `-ticcost 5`).
    - `-rendercost 50` with max skip 3 shows 227 frames in 900 tics, one every 4 tics as documented.
    - `-autoframeskip 100 -rendercost 0.001` and mixed costs terminate normally.
    - The fixed path is CGDoom's exact code.
  - **`W_SlowLevelStart` purge at level start:**
    - No engine code keeps a PU_CACHE pointer across `P_SetupLevel`. Composites and lumpcache entries have owners;
      `demobuffer`, save buffers, ST/HU/WI/AM graphics and `colormaps` are PU_STATIC.
    - `WI_unloadData` does not demote the shared STFST01/STFDEAD0 faces.
    - Forgotten mapped pointers stay valid, and `Z_Free`/`Z_ChangeTag` ignore them.
  - **Zone size, slow vs default:** the smallest working zone was compared by bisection (e1m1-exit, demo2-1, demo1,
    map07, e1m8-finale, with and without `-fragment 4096`).
    - The thresholds are non-monotonic and differ by −90..+45 KiB in the realistic fragmented case; +165 KiB in one
      unfragmented case.
    - Running all 35 scenarios with `-fragment 4096 -zone 2304`: 33 pass and the same two fail (e2m4-limits,
      e2m4-rsmall: `Z_Malloc` failure) with and without `-slowwad`.
  - **Other items:**
    - `CGD_SaveSettings`: 3 more lines still fit the 2048-byte buffer (27 keymap lines).
    - The weak `PL_WadIsSlow` is not LTO-inlined: the gint build has no `-flto`, and all sources are linked as
      objects.
    - The SH4 integration point in `R_ExecuteSetViewSize` is the one marked block (design §8).
- **Pre-existing, not changed (noted for later):**
  - `M_DrawSaveLoadBorder` (`m_menu.c`, already so in CGDoom) caches M_LSLEFT/M_LSCNTR/M_LSRGHT as PU_CACHE and draws
    them afterwards.
    - With copied lumps, a `Z_Malloc` for the third could in principle purge the first two. The rover only reaches
      them after wrapping the whole zone, so this was not reproducible.
    - Vanilla caches each patch just before drawing it.
  - `CGD_LoadSettings` matches keys by prefix (`strncmp(line, key, eq-line)`), so a truncated key such as `View=`
    matches the first key it is a prefix of.
- **Tests after the fix:** see the reviewer's report (`make test-host`/`test-asan` 35/35, `-slowwad` variants above,
  `test-unit`, `check-sh`, C-only sim `test-sim` 3/3 and `test-sim-full` 35/35).

---

## Follow-ups (2026-10-10): save-name entry, settings, lump pad at the WAD end, idle signal, hardening

Engine-side items of `notes/integration-todo.md` raised by the gint review (`notes/gint-review.md` §12, §13, §17,
R2-7, R3.1-5) and by the step 3 review. Default-mode output is unchanged: no golden changed.

### What changed

- **`CGD_AlphaKeycode`** (`cgdoom-kbd.c`, declared in `cgdoom-kbd.h`): `int CGD_AlphaKeycode = KEYCODE_ALPHA;`, the
  modifier of save-name entry, used by `KeyDown()` instead of the hardwired `KEYCODE_ALPHA`.
  - Plain `int`, like `CGD_EscapeKeycode`. `src/gint/keymaps.c` (`gkm_apply_engine_hooks`) already writes it through
    a weak `extern int`: 0x72 on the fx-CG100 (0x71 is SHIFT there), 0x71 on the fx-CG50. **Nothing to do in gint.**
  - `tests/gint-host/test_keymaps.c` defined the symbol itself in its `-DWITH_HOOKS` build, which now clashes with
    the engine's definition: the definition was removed, both builds pass (`sh tests/gint-host/run.sh`: 10/10). The
    `-Wl,-U,_CGD_AlphaKeycode` flag in `tests/gint-host/run.sh` is now unnecessary but harmless.
- **Digits and space in save-name entry** (`i_system.c`, `I_StartTic`): the `(uint8_t)` casts turned `-'0'..-'9'` and
  `-' '` into unknown keys, so digits and space could not be typed on any model. The keys are now passed as they
  are; every gameplay key is positive and below 256, so gameplay events are unchanged. The events carry `'0'..'9'`
  and `' '`.
- **Text keys need a press of their own key** (`CGD_KeyWasJustPressed`): letters (no ALPHA), digits (ALPHA held) and
  space share physical keys. Releasing ALPHA while still holding 7 used to type 'M' after the '7' (and pressing ALPHA
  over a held 7 typed '7' after the 'M'). A text key now counts as pressed only when its physical key went down in
  this scan. Gameplay keys are not text keys.
- **fx-CG100 letters**: no separate table. The fx-CG100 manual (v2.10, `fx-CG100_1AUGRAPH_EN_PDF.pdf` from
  casio.com) gives these ALPHA sequences: `ALPHA [X,θ,t]` = A, `[Frac]` = B, `[√]` = C, `[e^x]` = F, `[7]` = M,
  `[×]` = S, `[1]` = U, `[+]` = X, `[−]` = Y, and `[×10^x]` = `"`. Its getkey() figure (p. 142) and
  `src/gint/keymaps.c` place those keys at the same matrix codes as the fx-CG50's A, B, C, F (0x61, 0x62, 0x63, 0x66)
  and M, S, U, X, Y. So the fx-CG50 table, which is by matrix code, is also the fx-CG100's for every documented letter.
  - **Inferred, not documented:** D and E (row 6, between C and F) and G..L (row 5, the only six keys between F and
    M), all in reading order as on the fx-CG50. No web source with the full legend set was found (Casio's HTML
    manual, TI-Planet, Planète Casio, Cemetech searches). Only a hardware photo can confirm them.
  - Space stays on "." without ALPHA, which is the same key on both models.
  - Because the tables coincide, no `CGD_KeyboardModel` hint was added.
- **Pre-filled save names** (`m_menu.c`, `M_SaveSelect`): picking an empty slot fills in the map name, `E1M3` or `MAP07`
  (`gamemode == commercial`), as Crispy Doom does. EXE saves at once, DEL and typing edit the name, and MENU (escape)
  restores "empty slot". Occupied slots keep their description.
- **Settings** (`cgdoom.c`, `CGD_LoadSettings`):
  - Keys and values match exactly (new `SettingIs`). `View=` no longer sets `ViewSize`, and `WADMethod=MMAPX` is
    ignored.
  - `Keymap.<action>=NONE` unbinds the action (-1). This is what `CGD_SaveSettings` writes for an unbound action. gint's
    `gkm_apply_unbound()` workaround is now redundant but harmless.
  - Unknown values (`NONEX`, `F`) leave the binding alone. Lines without `=`, garbage and binary lines are ignored. A
    NUL byte ends the file. CRLF is accepted.
  - `CGD_DoomKey_FromTechnicalName`/`CGD_PhysicalKey_FromTechnicalName` (`cgdoom-kbd.c`) match exactly, so `Weapon` is
    no longer `Weapon1` and `F` is no longer `F1`. `"NONE"` still returns -1 from the physical lookup, and the gint
    test relies on that.
- **Lump pad at the WAD end** (`w_wad.c`, `W_ReadLumpWithZ_Malloc`; gint review R2-7):
  - A lump is referenced in place only if `[position, position + size + W_LUMP_PAD)` lies inside the WAD and
    `PL_WadMap` maps it.
  - A lump whose pad would cross `wad_end` is copied with a zero-filled pad, on every platform. The renderer never
    reads what follows the WAD in the mapping.
  - The freedoom WADs have their directory last, so no lump is affected and the goldens are unaffected trivially.
    Checked with a rebuilt freedoom1 (directory first, COLORMAP moved to the very end): e1m1 reproduces its golden
    by default, with `-slowwad` and with `-fragment 4096`, and under ASan with `-slowwad -poison 0x77`.
  - COLORMAP is now copied (`lumps: loaded=2`, was 1).
- **Idle signal** (`cgdoom.h`, `g_game.c`): `int CGD_IsPaused(void)` returns a mask, 0 when a level is being played
  (automap included).
  - `CGD_IDLE_PAUSED` (1): Pause.
  - `CGD_IDLE_MENU` (2): menu, menu message, help screens or save-name entry. In single player `P_Ticker` freezes the
    world behind the menu.
  - `CGD_IDLE_NOLEVEL` (4): title loop, intermission or finale. It is also set before the first level is set up
    (`gamemap == 0`), when `gamestate` is still 0 (GS_LEVEL).
  - `CGD_IDLE_DEMO` (8): demo playback.
  - It only reads globals, so the platform may call it from `PL_GetTics`/`gquit_poll`.
  - It does not say whether progress would be lost. A paused level still has unsaved progress (`usergame`).
- **`M_DrawSaveLoadBorder`** (`m_menu.c`): each of M_LSLEFT/M_LSCNTR/M_LSRGHT is cached right before its draws, as
  in vanilla. Fetching all three first let the second or third `Z_Malloc` (zone copies in slow or unmappable mode)
  purge the first. Pixels are unchanged.
- **Save padding on 64-bit hosts** (`p_saveg.c`, `PADSAVEP`): found by the new save test under UBSan.
  - Saving stored `player_t` (which holds pointers) at a 4-byte boundary: a misaligned 8-byte-pointer store.
  - It now pads to `max(4, sizeof(void *))`. The calculator and the SH simulator are unchanged (4).
  - Host saves were not portable before either, because struct sizes differ.
- **Host:** `-gamelog` lines end with `idle=<CGD_IsPaused()>`, and a `GAMELOG idle` line is written whenever the
  value changes.

### Tests

| test | result |
|---|---|
| `make test-host` | 35/35 PASS (goldens unchanged) |
| `make test-asan` | 35/35 PASS, plus `save-entry.sh` under ASan/UBSan PASS |
| `make test-unit` | PASS: fixed, div, names, roundtrip, **settings**, **saveentry** |
| `make check-sh` | PASS |
| `test-sim`, C-only ELF | 3/3 PASS |
| `test-sim-full`, C-only ELF | 35/35 PASS |
| `sh tests/gint-host/run.sh` | 10/10 PASS (after the `test_keymaps.c` fix above) |

- **New `tests/settings_test.c` (`make test-settings`):** links `cgdoom.c` and `cgdoom-kbd.c` with an in-memory file
  and checks:
  - the round trip of every option and of a keymap with unbound and AC/ON entries, with LF and CRLF line ends;
  - `NONE`;
  - prefix and extended keys or values, garbage and binary lines, `=` with an empty key, and a last line without
    `\n`;
  - wrong version, empty or missing file, and NUL inside the file.
- **New `tests/save-entry.sh` (`make test-saveentry`, also run by `make test-asan`)**, with
  `tests/input/save-entry.txt` (raw matrix keys). It runs on freedoom1 and freedoom2 and checks:
  - the pre-filled `E1M1`/`MAP01` is saved with EXE;
  - DEL ×5, then "A1 B0" typed with letters, ALPHA digits (ALPHA released before the digit, and ALPHA and the digit in
    the same tic) and space;
  - typing in slot 2 and then MENU writes no file;
  - the typed save loads through Load Game into E1M1;
  - `CGD_IsPaused` follows the sequence title 4 → menu 6 → level 0 → Pause 1 → 0.

### For later agents

- **gint (optional):**
  - Use `CGD_IsPaused()` (weak `extern int CGD_IsPaused(void)` or a direct call) in `game_idle_check()`. For example,
    use the short timeout when `CGD_IDLE_NOLEVEL|CGD_IDLE_DEMO` is set and no level was entered, and the long one
    otherwise.
  - The current `demoplayback || gamestate == GS_DEMOSCREEN` test is equivalent to `CGD_IDLE_DEMO` or (title ⊂
    `CGD_IDLE_NOLEVEL`).
  - `gkm_apply_unbound()` can be dropped.
  - `PL_WadMap` no longer needs to guard against engine ranges past `wad_end`, but it should keep its EOF refusal.
- **Hardware check:** on an fx-CG100, ALPHA + `^`, `x²`, `,`, `sin`, `cos`, `tan`, `(`, `)` should show the orange
  legends D, E, G, H, I, J, K, L. If they do not, add a per-model letter table in `DoomKeyToKeycode` selected by a
  platform-set int.

### Follow-ups review (2026-10-10, adversarial reviewer)

Scope: the six engine-side items and the extra fixes above. Two defects were confirmed and fixed. Default-mode output
is unchanged: no golden changed.

**Fixed**

- **Key presses leaked across the switch into and out of save-name entry** (`i_system.c`, `I_StartTic`).
  - `D_PostEvent` hands each event to `M_Responder` at once, inside I_StartTic's loop. So the EXE press that picks a
    save slot switched on text entry, and the later presses of the same scan were then typed into the name.
  - With Interact on EXE (CG50 preset "CGDoom 0.3", and any custom map that puts Interact or a Weapon key on EXE),
    picking an empty slot gave `E1M1 ` (with a trailing space) or `E1M1 1` instead of `E1M1`.
  - In the other direction, MENU and EXE pressed in the same tic cancelled the entry and then picked the slot again.
  - The bug predates the follow-ups (CGDoom typed a leading space). The pre-filled name makes it visible.
  - Fix: once `saveStringEnter` changes during the scan, the remaining key presses of that scan are dropped. Releases
    are still posted, so `G_Responder` never keeps a stuck key.
  - Test: `tests/input/save-entry.txt` now picks slot 0 with `enter use w1` (ThumbsOnly: EXE, SQUARE and F1 in one
    tic). Without the fix, `save-entry.sh` fails with `'E1M1 1'`. With it, the test passes.
- **Numeric settings were read across line ends** (`cgdoom.c`, `CGD_LoadSettings`).
  - `atoi(value)` skips leading whitespace, including `\n`. So an empty value read the digits of the next line:
    `LowDetail=\n1\n` set LowDetail to 1, and an empty `Version=` followed by a line holding the version number was
    accepted.
  - `atoi` overflow (`Autostart=99999999999`) is undefined behaviour.
  - Fix: new `SettingInt` accepts only an optional `-` followed by 1..9 digits, within the line.
    - An empty or malformed value of a numeric option is now ignored, like unknown values of the other options.
      Before, it set the option to 0 (or to the next line's number).
    - An empty or malformed `Version` rejects the file, as a wrong version does.
    - `CGD_SaveSettings` output is unaffected.
  - Test: `tests/settings_test.c` has a new `test_numeric_values` case covering:
    - empty values followed by a digits line, with LF and CRLF;
    - `" 5"`, `"1x"`, `"-"` and an overflowing value;
    - valid values, including a negative one;
    - an empty Version;
    - 6000-character lines with and without `=`.

    Under the old parser it fails twice.

**Checked, no defect**

- **Text entry:**
  - Backspace at index 0 is guarded.
  - The length is capped at 23 characters and at 176 pixels of `M_StringWidth`. The longest pre-fill, `MAP32`, uses
    `snprintf` into the 24-byte slot.
  - Only `A..Z`, `0..9` and space can reach the default branch. Base keys that leak are now dropped (see above). Other
    base keys are either below 32 or above `HU_FONTEND`, and the font check rejects them.
  - Escape restores `saveOldString`, so an empty slot reads "empty slot" again.
  - ALPHA is a held modifier, not a latch, because the platform reads the raw matrix:
    - a held ALPHA followed by 1 types `1`;
    - releasing ALPHA over the held 1 types nothing;
    - pressing ALPHA again over it types nothing.
  - No letter, digit, space, DEL, EXE or escape key sits on 0x71 or 0x72, so neither model's `CGD_AlphaKeycode`
    collides with one.
  - When ALPHA is released over a held key, a keyup with no matching keydown is posted for the letter. Both
    responders ignore it.
- **Settings:**
  - CRLF, a bare `\r` line, a missing `=`, `=` with an empty key and binary lines are handled.
  - A `NONE` value unbinds the action. An empty `Keymap.X=` is ignored (it was never a valid key name).
  - Long lines are fine: the parser keeps no fixed-size line buffer.
- **`w_wad.c` EOF rule:** tested with rebuilt freedoom1 WADs. The directory comes first, lumps are 4-aligned, and
  COLORMAP is the last lump, followed by a `PADLUMP` of 136 bytes, of 135 bytes, or by nothing.
  - With 136 bytes the pad ends exactly at `wad_end` and COLORMAP is referenced in place (`lumps: loaded=1`).
  - With 135 bytes or nothing it is copied (`loaded=2`).
  - e1m1 matches its golden in each case: by default, with `-slowwad`, with `-fragment 4096`, and under ASan with
    `-slowwad -poison 0x77`.
  - Zero-size lumps: `wad_end` covers `position + size` of every lump, so the clamp never goes negative. A zero-size
    lump at `wad_end` (the end of the file for a well-formed WAD) gets a 136-byte zero block (host `PL_WadRead`
    with size 0 returns 0 there).
- **`CGD_IsPaused`:**
  - Checked by reading the code (the save-entry test covers title, menu, level and Pause): title (4), title with
    the menu (6), level (0), Pause (1), menu message and help screens (menu bit),
    intermission and finale (4), demo in the title loop (8, plus 2 with the menu), before the first level (4), after
    End Game (4), and the wipe (reads the new state).
  - Note for gint: a dead player waiting to respawn reads 0, because the level is still being played. An idle timeout
    keyed on 0 does not fire there.
- **`M_DrawSaveLoadBorder`:** it matches vanilla's per-patch caching. `V_DrawPatchDirect` does not allocate.
- **`PADSAVEP`:**
  - It aligns on the absolute address of `save_p`, as vanilla does. Save and load buffers are zone blocks, which are
    pointer-aligned, so the two agree.
  - The SH build is unchanged (4).

**Gates after the fixes:**
- `make test-host`: 35/35.
- `make test-asan`: 35/35, plus saveentry.
- `make test-unit`: fixed, div, names, roundtrip, settings and saveentry.
- `make check-sh`: PASS.
- `sh tests/gint-host/run.sh`: 10/10.
- C-only sim ELF (built as in step 3): `test-sim` 3/3 and `test-sim-full` 35/35.

## SH4 integration (2026-10-10): asm drawers wired in, demo-recording flag

Integration of `src/engine/sh4` (read-only here) into the engine, the sim build and the calculator build, plus the
engine side of gint review round 3.3's open item (a demo recording finished before it had begun).

### What changed
- **`r_draw.h`:** `CGD_SH4` is defined when `__sh__` and not `CGD_NO_SH4`. Calculator and sim builds (both SH) use the
  asm drawers; the host never does; `-DCGD_NO_SH4` builds a C-only SH ELF (the reference for perf).
- **`r_main.c`, `R_ExecuteSetViewSize`** (the single marked place): under `CGD_SH4` selects `R_DrawColumn[Low]_SH4`,
  `R_DrawFuzzColumn[Low]_SH4`, `R_DrawTranslatedColumn[Low]_SH4` and `R_DrawSpan[Low]_SH4` with the same
  `detailshift` split as the C drawers. Potato floors keep the C `R_DrawSpanFlat[Low]` (a byte fill of one row:
  `cgd_fill32` needs 32-byte-aligned, 32-byte-multiple destinations and does not fit).
- **`r_data.c`, `R_InitColormaps`:** under `CGD_SH4` registers the lump with
  `cgd_sh4_set_colormaps(colormaps, W_LumpLength(COLORMAP)/256, rot)`, `rot` a 32-byte-aligned `Z_Malloc(n*256+31,
  PU_STATIC)` (34 maps: 8,704 + 31 bytes of zone, kept for the session like the lump). Contract per sh4 README §6.5/§7.2
  and `cgd_sh4.h` (nmaps capped at 256; nothing registered = C fallbacks, still exact).
- **`g_game.c`, recording begun flag** (`demorecbegun`, static) and the recording buffer kept apart (`demorecbuffer`):
  - `G_RecordDemo` arms (buffer, `demo_p = NULL`, begun = false). `G_BeginRecording` re-selects the recording buffer
    as `demobuffer`/`demoend` (a title demo's `G_DoPlayDemo` may have replaced them with its lump) and sets begun.
  - `G_Ticker` writes a ticcmd only when begun. `G_CheckDemoStatus` drops an armed recording that has not begun
    (`demorecording = false`, nothing saved, `return false`); this covers `D_DoomLoop`'s final call and `I_Error`
    during the first level load. After a real finish the buffer belongs to the delayed save (begun cleared,
    `demorecbuffer = NULL`).
  - `G_WriteDemoTiccmd`: after the MENU key ends the recording, vanilla still stores that tic after the marker. With
    `DelaySaves` off the buffer has already been written and `Z_Free`d by then (a 4-byte use-after-free); it now
    stores nothing and applies the same quantized `angleturn`, so the game state is unchanged.
- **Builds:** `src/sim/cgdoom.mk` (`.S`: `-Wa,--dsp -DCGD_SH4_DSP_MNEMONICS -MMD -MP`, `-DCGD_SH4_NO_ILRAM` by default;
  `CGSIM_SH4=0` C-only and `CGSIM_ILRAM=1` variants with a per-directory variant stamp); `CMakeLists.txt` defines
  `CGD_SH4_DSP_MNEMONICS` for the `.S` files (the fxSDK toolchain file already passes `-m4-nofpu -mb -Wa,--dsp`).

### Tests
- `tests/demo-unbegun.sh` (`make test-demo-unbegun`, in `test-unit` and, with the ASan binary, in `test-asan`):
  `-record` on the title (`-tics 60`), the same with title demos (`-demos -tics 400`, frames must equal the run without
  `-record`), and `I_Error` in the first level load (`-warp 1 1 -zone 1000`, Z_Malloc failure after the 128 KiB demo
  buffer). Each must exit normally (0 / 0 / 2), save nothing and keep the slot's previous file. Mutation: without the
  flag checks all three fail (two segfaults, one frame mismatch).
- Sim with the SH4 drawers (default ELF, sections in `.text`): `test-sim` 3/3, `test-sim-full` 35/35 bit-exact vs the
  host goldens (all low-detail, fuzz, translated, flat, view-size, auto-frameskip and zone variants). The `.ilram`
  layout (`CGSIM_ILRAM=1`) also 35/35. The C-only ELF `test-sim` 3/3. The profile shows the asm paths in use and no
  colormap fallback: the only `cgd_sh4_slow_*` calls are count < 0 columns (vanilla's unconditional mid-texture call).
- Host: `test-host` 35/35, `test-asan` 35/35 + saveentry + unbegun, `test-unit` all PASS, `check-sh` PASS.

### Performance
`tests/perf-baseline.md` "After the SH4 drawers": simulator instructions per frame −14.8% (demo1) to −28.6% (e1m3),
cycles −15.0% to −30.9%, against the C-only ELF; the modelled hardware cycles of the drawers (README §3 per-call
models on the profiles' exact call/iteration counts) drop 2.6–3.2× (columns+spans, e1m1/map01) and 2.3–2.7× (demo1).

### For later agents
- r_segs.c's mid-texture branch calls `R_GetColumn` + `colfunc` even when `yl > yh` (vanilla): 229 of 873 column calls
  per frame in e1m1 (26%). A `yl <= yh` guard (keeping the clip updates) would save ≈15–20k sim cycles per frame in
  e1m1; not done here (it changes texture-cache traffic, which the goldens do not see, and the C reference timings).
  **Done** in "Renderer polish" below (with the lighting/division deferred too).
- On the calculator the drawers run from IL memory; gint re-copies `.ilram` after every world switch (code only, fine).

### Wiring/integration review (2026-10-10)
Adversarial review of the SH4 integration above (engine wiring, sim/CMake builds, demo flag).
- **Confirmed and fixed: a begun recording kept writing into a played demo** (`g_game.c`, `G_DoPlayDemo`).
  End Game from a recorded game with title demos on: `G_DoPlayDemo` replaced `demobuffer`/`demo_p` with
  the demo lump while `demorecording && demorecbegun`, so `G_Ticker` wrote every ticcmd 4 bytes ahead of
  the reader into the lump. The title demo desynced (freedoom1 DEMO1: death at tic 854 instead of 847),
  skipped over its marker and never ended (12,000 tics), so reads and writes ran on past the lump into the
  rest of the WAD image (zone copy with `-slowwad`); the final `G_CheckDemoStatus` would have saved from the
  lump and later `Z_Free`d it. Fix: `G_DoPlayDemo` first ends a begun recording (`G_CheckDemoStatus`, not
  during playback or a timedemo): the demo is saved with the game and title tics up to that point.
  Not reachable from the gint launcher (recording forces Skip, no title demos), reachable on the host
  (`-record -demos`). Test: `tests/demo-unbegun.sh` case `endgame` (`tests/input/demo-endgame.txt`):
  frames equal to the run without `-record`, demo saved (1,046 bytes, ends with 0x80); without the fix
  the frames differ. In `test-unit` and, with the ASan binary, `test-asan`.
- **Refuted: "the MENU key ends a recording"** (and the 4-byte use-after-free fixed after it):
  `M_Responder` eats every `KEY_ESCAPE` press (it opens or closes the menu), so `G_Responder` never sets
  `gamekeydown[KEY_ESCAPE]` and that branch of `G_WriteDemoTiccmd` is unreachable (checked: MENU pressed
  twice at tic 71, the demo still holds all 501 tics). Kept as a harmless guard, commented. As in
  CGDoom 0.3, a recording ends only at quit, a full buffer (128 KiB, ~15 min) or now a played demo; a
  New Game from the menu restarts it (`G_DoNewGame` -> `G_BeginRecording`), losing the earlier game.
- Checked, no defect: drawer selection (one place, potato floors C, `colfunc` restored from `basecolfunc`,
  no direct drawer calls left); `CGD_SH4` visible before the `cgd_sh4.h` includes; engine globals'
  types and the fuzz table match the asm; nothing changes COLORMAP after registration (gamma is the
  palette); every colormap pointer is `colormaps + k*256`; the only repeat-loop code besides the drawers is
  fxlibc `memchr` (foreground only), gint's interrupt paths (tic callback, DMA with no callback) have none
  and no USB; `.ilram` (drawers 0xE5200000..0xE5200548 + gint's handler) is backed up/restored across world
  switches on the CG100 (`GINT_ONCHIP_BACKUP`, `src/gint/main.c`) and reloaded on the CG50; C fallbacks
  are reached from `.ilram` by absolute address (the g3a links); the drawers use no `pref` (only the
  expansion, on P1 sources); sim stamp/`-MMD -MP` rules and CMake flags are right.
- Not covered by any golden: the translated drawers (`MF_TRANSLATION` only in netgames/netdemos), so
  `R_DrawTranslatedColumn[Low]_SH4` are verified by the sh4 randomized test only.

## Renderer polish (2026-10-10): empty wall columns, R_DrawMaskedColumn locals, docs

### What changed
- **`r_segs.c`, `R_RenderSegLoop`: empty mid-texture columns are skipped.** Vanilla called `R_GetColumn` +
  `colfunc` for a single-sided wall even when `yh < yl` (229 of 873 column calls per frame in e1m1). Every column
  drawer returns at once for `dc_yh < dc_yl` (C: `count < 0`; SH4: count check → C fallback → return), so the
  call is now made only when `yh >= yl` (`yl`, `yh` are within ±2^20, so this is exactly `count >= 0`). The clip
  updates (`ceilingclip[x] = viewheight`, `floorclip[x] = -1`) stay unconditional. The top and bottom tiers were
  already guarded (`mid >= yl`, `mid <= yh`).
- Same function: the light index, `dc_colormap`, `dc_x` and the `dc_iscale` division are computed by
  `SEG_COLUMN_SETUP()` (a local macro with a per-column `lit` flag) before the first tier a column draws, not
  for every textured column. The values at each `colfunc` call are vanilla's; after the loop `dc_colormap`/`dc_x`/
  `dc_iscale` may hold an earlier column's values, and nothing reads them before setting them (masked segs,
  sprites, psprites and the sky all set all three first; checked with grep over src/).
- **`r_things.c`, `R_DrawMaskedColumn`: loop state in locals** (as step 2 for `R_RenderSegLoop`):
  `sprtopscreen`, `spryscale`, `mfloorclip[dc_x]`, `mceilingclip[dc_x]` and `colfunc` are read once;
  `dc_yl`/`dc_yh`/`dc_source`/`dc_texturemid` are stored only for drawn posts (vanilla also stored the clipped
  bounds of skipped posts; no caller reads them afterwards), `dc_texturemid` is restored at the end as before.
  The drawers write only the frame buffer and, for fuzz, `dc_yl`/`dc_yh` (recomputed per post), so the cached
  values cannot go stale.
- Zone/texture cache: fewer `R_GetColumn` calls means fewer `W_CacheLumpNumConstFast`/`R_GenerateComposite`
  touches for textures that end up invisible. Only cache traffic changes; all 35 goldens (incl. the
  `-zone`/`-slowwad`/fragmentation/poison scenarios) are unchanged on the host and in the sim.
- **SH4 drawers' count < 0 path: unchanged, on purpose.** After the change no call site passes a negative count
  (`R_DrawMaskedColumn` and the sky were already guarded): the sim profiles of e1m1, map01, demo1, e1m1-low and
  demo1-low have 0 calls of `cgd_sh4_slow_column[_low]` (before: 229/frame in e1m1). An early exit in the asm
  would lengthen the scheduled set-up of every real call, so `src/engine/sh4/*.S` is untouched (sh4 README §7.4).
- **Docs:** sh4 README (top: sim/CMake `.S` flags are now `-Wa,--dsp -DCGD_SH4_DSP_MNEMONICS -MMD -MP`, asmcheck
  checks the raw-opcode form; `CGD_SH4` comes from `r_draw.h`; §1/§7.6 and `cgd_sh4.h`: the presenter does only a
  `synco` after the purging expansion; §7 rewritten as the as-built integration), `sh4_asm.h` and
  `test/Makefile` comments.
- **gint (dead code):** `end_game_session()` calls `G_CheckDemoStatus()` directly; `gdemo_begun()`
  (`cgd_gint.h`), its `demobuffer`/`demo_p`/`demoend` externs and its host test checks are gone. The engine's
  `demorecbegun` flag makes `G_CheckDemoStatus()` drop an armed, unbegun recording (`demorecording = false`,
  nothing saved), which is what the gint check did. Only difference: the developer log line "Demo recording had
  not begun" is no longer written. `PL_FileWrite`'s size guard and its test stay (defence in depth).

### Tests
- Host `test-host` 35/35, `test-asan` 35/35 (+ saveentry, unbegun), `test-unit`, `check-sh`; `gint-host` 9/9;
  `make -C src/engine/sh4/test run` PASS (63,966 + 6,507 + 600 cases, `.ilram` 1352 B unchanged);
  `test-sim` 3/3, `test-sim-full` 35/35; `fxsdk build-cg` warning-free (g3a `.ilram` 0x610 = 1552 B, unchanged).

### Performance
`tests/perf-baseline.md` "After the renderer polish": sim instructions per frame −5.8% e1m1, −4.5% demo1,
−3.6% e3m4, −0.8% map01; low detail −4.8% e1m1-low, −3.7% demo1-low. Column calls per frame 872 → 643 (e1m1);
`R_DrawMaskedColumn` −21% in demo1; `__udivsi3_i4i` −34% in e1m1 (deferred `dc_iscale`).

## Final whole-system review fixes (2026-10-10)

Full record: notes/final-review.md. Engine-side changes only:
- **`G_DoSaveGame` (g_game.c):** with `DelaySaves=0` the `PL_FileWrite()` result is checked; a failure shows
  `GGSAVEFAILED` ("save failed: file not written!") instead of "game saved." and logs
  `G_DoSaveGame: <name> not written` through `PL_Log`. `G_CheckDemoStatus` logs a failed immediate demo write.
  Host: new `-nodelaysaves` option; tests/save-entry.sh checks both outcomes (read-only and writable `-data`).
- **`PRESSYN` (d_englsh.h):** "press exe to confirm." (was "press y or n.", but no key posts 'y'/'n'); covers ENDGAME,
  NIGHTMARE, QSPROMPT, QLPROMPT. No golden shows these prompts (35/35 unchanged).
- **`CGD_EnterKeycode2` (cgdoom-kbd.c/.h):** a second physical key for KEY_ENTER besides EXE, honoured only while no
  keymap entry uses it (default -1, host unaffected). gint sets OK (0x84) on the fx-CG100.
- **cgdoom.h contract:** `PL_WadClose()` before `CGD_FlushDelayedWrites()` (the calculator order, with the reason);
  host_main.c now follows it.

## Maps past MAP32 (2026-10-10)

**Defect fixed: a Doom II WAD with a MAP33 read out of bounds.** The launcher offered start maps up to 99 (for PWADs),
and `run_game` only checked that the lump exists. The engine is vanilla's and has per-map data for the 32 Doom II maps:
`HU_Start` read `mapnames2[32]` at MAP33 (ASan: global-buffer-overflow, 0 bytes after `mapnames2`; the BFG/Unity
`doom2.wad` has a MAP33), and leaving the level would have read `cpars[32]`, `lnames[]` and then loaded MAP34.
- **Launcher (`src/gint/main.c`).** The start map is limited to 1..`LAUNCH_MAP_MAX` (32) in the arrow and digit
  handling and when `launcher()` starts. It is not part of `CGDOOM.CFG`, so nothing else sets it. USER-GUIDE updated.
- **`G_InitNew` (g_game.c).** A Doom II map above 32 is limited to 32, next to vanilla's clamp of a Doom map to 9.
  Every start funnels through it (launcher, `-warp`, `G_DoLoadGame`, `G_DoPlayDemo`), and level exits never lead
  past MAP32, so `gamemap`/`gameepisode` stay inside every per-map table. Maps 1-32 and every valid demo are
  unaffected; the goldens are unchanged.
- **Test.** `tests/map-limit.sh` (`make test-map-limit`, in `test-unit`, run under ASan by `test-asan`): `-warp 33`
  on `freedoom2.wad` starts MAP32.

## Demo sync (2026-10-10): id's v1.9 demos play tic for tic like vanilla

**Goal:** DEMO1-3 of the shareware DOOM v1.9 `doom1.wad` (md5 f0cefca49926d00903cf57551d901abe, version byte 109)
used to desync:

| demo | map | first death before | after |
|---|---|---|---|
| DEMO1 | E1M5 | tic 424 | — |
| DEMO2 | E1M3 | tic 1795 | — |
| DEMO3 | E1M7 | tic 1598 | — |

Each run also died repeatedly afterwards. All three now match vanilla on every tic, up to the demo marker.

### Oracle: linuxdoom-1.10, headless (`tools/demosync-oracle/`)

**Which copy is pristine:**
- `ref/extra/ClassPadDev_CPDoom/src/doom` is **not** pristine.
  - It is the December 1997 release, under the DOOM Source Licence, with the CVS directory, `FILES` and `d_french.h`.
  - CPDoom changed it heavily:
    - globals moved into `d_vars.c`;
    - fields renamed (`backsector` → `backsec`);
    - `d_net.c`, `i_video.c`, `s_sound.c`, `m_menu.c`, `w_wad.c`, `wi_stuff.c` and `p_setup.c` rewritten in part;
    - every file differs from id's.
- `ref/extra/EDiasAlberto_DoomCasioPrizm/doomsrc/linuxdoom-1.10.src.tgz` is id's GPL re-release, untouched (CRLF
  line ends). The unpacked tree next to it is identical apart from `i_video.c/h`. The oracle is built from the tgz.

**`tools/demosync-oracle/build.sh`:**
- unpacks the tgz into `build-host/oracle`;
- drops `i_main`/`i_net`/`i_sound`/`i_system`/`i_video`;
- applies `oracle.patch`;
- builds it with `oracle_sys.c`, which provides stub I/O, `main` and the trace.

The patch does not touch the game logic:
- demo version 109 is accepted, as in Chocolate Doom;
- the trace hook is called at the end of `G_Ticker`;
- 64-bit fixes:
  - pointer arrays get 8-byte entries;
  - the obsolete pointer in the on-disk `maptexture_t` becomes an `int`;
  - `intptr_t` alignment casts;
  - 8-byte zone alignment;
  - no string pointers in the `int` defaults;
- `sprnames[]` is NULL-terminated, because `R_InitSpriteDefs` scans for NULL and linuxdoom read past the end.

The oracle runs with `singletics` and draws nothing.

**Validation of the oracle:**

| demo | ends at the marker | player dies at | kills |
|---|---|---|---|
| DEMO1 | gametic 5026 | tic 4972 | 61 |
| DEMO2 | gametic 3836 | tic 3777 | 41 |
| DEMO3 | gametic 2134 | tic 2078 | 20 |

- Each demo plays to its marker, and the length matches the lump: (size − 14) / 4 tics.
- The player dies 54-59 tics before the end of each demo, as id's recordings do (they stop shortly after the death).
  A desynced playback would die at an arbitrary tic, as ours did.
- The single-player Freedoom demos also end this way in the oracle.

### Method

- **The trace.** `cgdoom-host -demotrace FILE` (new; host only; `G_TicEndHook`) and the oracle write the same line at
  the end of each `G_Ticker`:
  - `gametic`, `gamestate`, the map and `prndindex`;
  - the player mobj's x/y/z/angle/momx/momy;
  - health, armor, the 4 ammo counts and `readyweapon`;
  - the mobj count and an FNV hash of every mobj's x/y/z/health/state, in thinker order.
- `G_TicEndHook` is a NULL function pointer on the calculator, so the goldens do not change.
- **Root-causing a first divergent tic.** Two throw-away debug builds were used, kept in the scratchpad and not in the
  repository:
  - `P_Random` logs `gametic`, the index and its caller chain, through `dladdr`, at -O0;
  - a dump of every mobj field (target and tracer as thinker indices), every sector (heights, light, special, flats)
    and the player (psprites, powers, cards, weapons, counters) at each tic.
- **Audit.** A function-by-function normalised diff of `p_*.c`, `g_game.c`, `r_main.c`, `m_*.c`, `wi_stuff.c`, the
  `info.c` tables, the data tables, the struct definitions and the `#define`s against linuxdoom-1.10. The audit is
  what found the nightmare and audit-only items below.

### Divergences, root causes and fixes

1. **Sound picks that consume `P_Random`.**
   - **First divergent tic:** DEMO1 tic 47, DEMO2 tic 59, DEMO3 tic 76. At that tic vanilla used 2-3 more random numbers
     than we did, and no mobj had moved yet.
   - **Root cause:** CGDoom removed sound together with three game-state `P_Random` calls:
     - `A_Look`: `sfx_posit1+P_Random()%3` or `sfx_bgsit1+P_Random()%2` for the sight sound, **before**
       `P_SetMobjState`;
     - `A_Chase`: `activesound && P_Random() < 3`, on every chase step of a monster that has an active sound;
     - `A_Scream`: `sfx_podth1+P_Random()%3` or `sfx_bgdth1+P_Random()%2`.

     It had also zeroed every sound field in `info.c`.
   - **Fix:**
     - new `src/engine/sounds.h`, holding only linuxdoom's `sfxenum_t` numbers;
     - `info.c` gets its 670 vanilla sound values back (the fields already existed);
     - the `P_Random` calls are back in vanilla order (sound pick first, then the state change), without
       `S_StartSound`.
   - `states[]` and `mobjinfo[]` are otherwise identical to vanilla's (checked field by field).

2. **`P_LookForPlayers`.**
   - **First divergent tic:** DEMO1 tic 67, DEMO2 tic 93, DEMO3 tic 87. At tic 67 vanilla's `A_Chase` → `P_NewChaseDir`
     → `P_TryWalk` used a random number that ours did not.
   - **Root cause:** the mobj dump already differed at tic 0, in `lastlook`.
     - CGDoom had replaced the vanilla loop over players with a direct look at player 0.
     - In single player they are not equivalent. `lastlook` starts at `P_Random() % MAXPLAYERS`. When it is 1, the first
       call stops at player 0 (== `stop`) and returns false **without looking**, and `lastlook` becomes 0. So a
       quarter of the monsters wake up one look later.
   - **Fix:** the vanilla loop is restored verbatim.

3. **The tic after the demo marker.**
   - **Symptom:** after fixes 1-2 every traced tic matched, but our trace had one more line.
   - **Root cause:**
     - Vanilla's `-playdemo` (singledemo) calls `I_Quit` inside `G_CheckDemoStatus`, at the marker.
     - CGDoom only set its quit flag and finished that tic. `P_Ticker` ran with the stale last demo command, because
       during playback our `players[0].cmd` is not rebuilt.
   - **Fix:**
     - when the quit flag is set while reading a demo command (`CGD_PlayDemoOnly`), `G_Ticker` returns at once;
     - at the marker the command is zeroed, as vanilla's locally built command is during playback. This is for the
       title-loop case, where the tic still runs.

   Final frames and goldens are unaffected: no frame was drawn after that tic.
4. **`A_CloseShotgun2` without `A_ReFire`.**
   - **Where:** Freedoom2 DEMO3 (MAP22), tic 2231, in the player's psprite: vanilla at `S_DSGUN1` with `refire=1`, ours
     at `S_DSGUN9`.
   - **Root cause:** CGDoom emptied the function together with its sound. A held fire button no longer refires the super
     shotgun.
   - **Fix:** the vanilla `A_ReFire(player, psp)` call is restored.
5. **Nightmare and `-fast` (found by the audit).**
   - **Root cause:** vanilla `G_InitNew` patches `states[S_SARG_RUN1..S_SARG_PAIN2].tics` (halved) and the speed of
     `MT_BRUISERSHOT`/`MT_HEADSHOT`/`MT_TROOPSHOT` (20) for nightmare or a demo with `fastparm`, and undoes it when a
     game leaves nightmare. CGDoom's tables are const, so it did nothing.
   - **Fix:** `p_mobj.c` `P_SetFastMonsters`:
     - RAM copies of those 13 tics and 3 speeds;
     - vanilla's transitions are applied, including the quirk that repeated `-fast` games halve again;
     - used by `P_SetMobjState`/`P_SpawnMobj` (`P_StateTics`) and `P_SpawnMissile` (`P_MissileSpeed`).
   - **Also restored:** `respawnmonsters` honours `respawnparm` (demo header).
   - **Evidence:** four nightmare bot demos recorded by this engine match the oracle. With `P_SetFastMonsters(true)`
     disabled, they diverge at tics 1466, 1563, 17 and 1571.
6. **Audit-only, vanilla restored:**
   - the menu no longer pauses `P_Ticker` during demo playback (`!netgame && menuactive && !demoplayback`);
   - `G_DoLoadLevel` turns a dead player into `PST_REBORN`.

### Evidence

- **doom1.wad DEMO1-3:**
  - traces are identical to the oracle's on every tic (5027/3837/2135 lines, including `END gametic=`);
  - the debug builds' full mobj, sector and player dumps are identical on every tic;
  - the `P_Random` call sequence (gametic, index) is identical: 25,013, 15,691 and 22,991 calls.

  The game log now ends like the oracle:

  | demo | player dies at | kills | items | secrets |
  |---|---|---|---|---|
  | DEMO1 | tic 4973 | 61/91 | 7/29 | 1/9 |
  | DEMO2 | tic 3778 | 41/74 | 14/95 | 0/7 |
  | DEMO3 | tic 2079 | 20/84 | 9/84 | 0/4 |

  The game log reports these deaths one tic later than the oracle traces show them, because it logs at the next tic.
- **Freedoom 0.13 single-player demos:** the oracle runs with `freedoom1.wad` as `doom.wad` and `freedoom2.wad` as
  `doom2.wad`.

  | demo | map | tics | match | player dies at |
  |---|---|---|---|---|
  | freedoom1 DEMO1 | E1M6 | 7117 | every tic | tic 7042 |
  | freedoom2 DEMO1 | MAP03 | 2664 | every tic | tic 2602 |
  | freedoom2 DEMO3 | MAP22 | 2743 | every tic | tic 2609 |

  Before this work they died at tics 590, 1019 and 524.

  The co-op demos (freedoom1 DEMO2-4, freedoom2 DEMO2/4) are still unplayable: CGDoom is single-player (see Step 1).
- **Recorded bot demos:** 22 demos were recorded with this engine (`-record`, cheat-free `builtin:walk`/
  `builtin:random`, 2000-3000 tics, many deaths). They were played in both engines and matched on every tic:
  - doom1 E1M1-E1M9;
  - freedoom1 E1M9, E2M3, E3M6, E4M2 and E4M5. The E4 maps were run with the oracle in retail mode
    (`freedoom1.wad` as `doomu.wad`).
  - freedoom2 MAP01, 07, 11, 15, 20, 24, 29 and 30.

  Together they cover skills 1-5.
- **SH simulator:** the three doom1 demos in `cgdoom-sim.elf` give the same per-frame CRCs as the host, with full
  rendering (5067/3877/2175 frames), and traces identical to the oracle's. `make test-demosync-sim` passes.

### Tests

- **`make test-demosync`** (`tests/demo-sync.sh`): plays DEMO1-3 of `$CGD_DOOM1_WAD` with `-demotrace -frameskip 7`
  and compares each trace with `tests/demosync/demoN.ckpt`.
  - A `.ckpt` file holds the SHA-256 and line count of the whole trace, every 128th line and the last two lines. It
    is derived only from our engine's state, never from WAD data.
  - The test is skipped, exit status 0, when the variable is unset or the file's md5 is not that of the v1.9
    shareware IWAD.
  - A failure prints the first differing checkpoint.
  - `-update` rewrites the `.ckpt` files. Do this only after checking the traces against the oracle again.
- **`make test-demosync-sim`:** the same test with the simulator ELF (about 7 s).
- **Goldens:** 21 regenerated and one added (`e1m1x-auto`). See tests/golden/CHANGES.md: one fix was reverted at a time
  to attribute each golden change.
- **Gates:** `test-host` 36/36, `test-asan` 36/36 (+ saveentry, unbegun), `test-unit`, `check-sh`, the sh4 asm tests,
  `gint-host` 10/10, `test-sim`, `test-sim-full` 36/36, `fxsdk build-cg` warning-free.

### Known non-vanilla behaviour left as it is (not game state, or out of scope)

- **Multiplayer and deathmatch.** Co-op demos, `G_DeathMatchSpawnPlayer`, netgame weapon pick-ups.
- **Vanilla overflows, which are undefined in linuxdoom too.** `spechit` beyond 8 entries, intercepts and REJECT. The
  code is the same, but the memory that an overflow clobbers differs.
- **Sliding doors.** Unused and absent.
- **`R_PointToDist`'s slope clamp.** Renderer only.
- **`FixedDiv`'s `MININT` saturation.** See Step 1.
- **The switch list.** It is built from all episodes. Only textures are affected, and only by switch textures that are
  missing from the WAD.
- **Input-only differences.** The keyboard `angleturn` table; no menu on a key press during title demos.
- **Cosmetic.** The medikit message (nDoom fix).

### Risks and cost

- The game logic changed on purpose, so goldens and old CGDoom recordings differ.
  - CGDoom-recorded demos made before this change will desync, because they were recorded with the old RNG use.
  - Saves still load: the format is unchanged.
- **Runtime cost:**
  - `P_StateTics` adds a range check per state change;
  - `A_Chase` adds one `P_Random` per step for monsters with an active sound;
  - `P_LookForPlayers` runs at most one sight check per player and call (vanilla runs two in single player; see
    "Demo sync review" below).

  These costs are small next to the renderer.
- **RAM:** 13 ints and 3 fixed-point values.

### For later agents

- Any change to game logic must keep `make test-demosync` passing; run it with `CGD_DOOM1_WAD` set.
- If a trace has to change, rebuild the oracle (`tools/demosync-oracle/build.sh`) and compare tic by tic before running
  `-update`.
- The trace format is defined twice and must stay identical: `demotrace_tic()` in `src/host/host_main.c` and
  `ORACLE_TraceTic()` in `tools/demosync-oracle/oracle_sys.c`.
- The `info.c` sound fields are game state: `A_Look`, `A_Chase` and `A_Scream` read them for `P_Random`. Do not zero
  them to save space.
- Any action function emptied "because it only plays a sound" must be checked against vanilla for `P_Random` calls and
  for other calls inside it, such as `A_CloseShotgun2` → `A_ReFire`.

### Demo sync review (2026-10-10, adversarial review of the work above)

**Re-checked independently:**
- **Oracle provenance.** `linuxdoom-1.10.src.tgz` is id's October 1999 GPL re-release (files dated 2 Oct 1999); its
  `p_enemy.c` is byte-identical to the unpacked tree beside it. `oracle.patch` was read hunk by hunk: demo version
  109, the trace hook, 64-bit allocation sizes, `intptr_t` casts, 8-byte zone alignment, the dropped pointer-in-int
  defaults and the NULL-terminated `sprnames[]`. No hunk touches game logic. `oracle_sys.c` stubs I/O only; sound
  stubs cannot shift `P_Random` (linuxdoom's `s_sound.c` uses `M_Random`). linuxdoom's floating-point `FixedDiv2`
  is exact for every input that passes the overflow test (the error bound needs |a| >= 2^37), so it equals DOS's
  integer divide.
- **Rebuilt the oracle** from the tgz with `build.sh` into the scratchpad. DOOM1-3 traces of `cgdoom-host` are
  byte-identical to it (5027/3837/2135 lines). freedoom1 DEMO1 and freedoom2 DEMO1/DEMO3 match too.
- **Each fix against vanilla source, not only the oracle.** The `A_Look`, `A_Chase`, `A_Scream`, `P_LookForPlayers`,
  `A_CloseShotgun2`, `G_InitNew` fast/nightmare, `P_Ticker` menu and `G_DoLoadLevel` changes were compared with
  linuxdoom-1.10 line by line. `states[]` and `mobjinfo[]` (normalised text) and the `sfxenum_t` order equal
  linuxdoom's. All 74 action functions named in vanilla's `states[]` are dispatched (`A_SetState`/`A_PSetState`); the
  only bodies still emptied (`A_Pain`, `A_XScream`, `A_PlayerScream`, `A_VileStart`, `A_BFGsound`,
  `A_OpenShotgun2`, `A_LoadShotgun2`, `A_BrainPain`, …) only play a sound in vanilla. Filtered diffs of
  `p_mobj.c`, `p_inter.c`, `p_map.c`, `p_user.c`, `p_doors.c`, `p_plats.c`, `p_floor.c`, `p_ceilng.c`,
  `p_switch.c`, `p_telept.c` and `p_tick.c` show only sound calls, casts, the thinker switch and the documented
  multiplayer/switch-list differences. The `P_DamageMobj` thrust and `A_Saw` angle rewrites are equivalent to
  vanilla's wrap-around arithmetic on their whole input range.
- **No WAD data in the tree.** No `*.wad`/`*.lmp` and nothing over 100 KB under `src/`, `tests/`, `dist/`, `docs/`,
  `tools/` or `notes/` apart from source tables and the built add-in. `tests/demosync/*.ckpt` hold only our trace
  lines (12 KB). `test-wads/` holds Freedoom 0.13 (BSD), as before. `doom1.wad` is only in `ref/` (md5
  unchanged).
- **`test-demosync`.** Passes with `CGD_DOOM1_WAD`. Exit status 0 with a SKIP line when the variable is unset,
  points to a missing file or to a file with another md5. `test-demosync-sim` passes too.

**Defect fixed: the restored `P_LookForPlayers` loop doubled the sight checks.**
- **Cause.** In single player, vanilla's loop comes back to player 0 and calls `P_CheckSight` a second time when the
  first look fails. That is the common case for idle monsters. Instrumented counts for DEMO1/2/3: 26,390/17,781/10,975
  repeated checks out of 58,773/36,847/24,180 `P_CheckSight` calls in total, about 45%. The old CGDoom code
  did one look.
- **Fix.** A per-call bitmask skips a player who was already looked at in this call. The skip comes after vanilla's
  `c++ == 2 || lastlook == stop` test, so `c`, `lastlook` and the return value advance exactly as in vanilla. The
  second look cannot answer differently: `P_CheckSight` and the angle test change no game state. They change only
  `validcount`, `sightcounts` and the sight-trace scratch globals, and those are compared or overwritten, never
  carried over.
- **Evidence.** DEMO1-3 traces are still byte-identical to the oracle, and so are the three Freedoom single-player
  demos. Goldens are unchanged (`test-host`, `test-sim-full`). In the simulator, `test-sim` demo1 (101 frames)
  drops from 343,365,519 to 341,160,190 instructions (-0.64%, rendering included). e1m1 and map01 also drop slightly.

**Refuted suspicions:**
- Fix 3 (`return` in `G_Ticker` when the quit flag is set) affects only `CGD_PlayDemoOnly` and a quit that is already
  in progress. The title loop still runs the marker tic with a zeroed command, as vanilla does.
- The menu not pausing demos changes no platform contract: `CGD_IsPaused()` still reports `CGD_IDLE_MENU` and
  `CGD_IDLE_DEMO`, and the idle policy treats demos as "nothing to lose".
- `P_StateTics`/`P_MissileSpeed` read every state's tics and the speed of the 3 missiles where vanilla reads the
  patched tables (`P_SetMobjState`, `P_SpawnMobj`, `P_SpawnMissile`). The other `info->speed` readers (monster walk,
  `A_Tracer`, `A_Fat*`, player missiles) use types that vanilla does not patch.

**Left as is, documented (not defects of this work):**
- **The oracle is linuxdoom-1.10, which behaves like the Ultimate/Final Doom v1.9 executables in one game-logic
  spot.** In `P_ZMovement` the lost soul floor bounce (`momz = -momz` before `momz = 0`) is the corrected version.
  The registered Doom v1.9 and Doom II v1.9 executables apply it after `momz` is zeroed and `z` clamped. Chocolate
  Doom's `correct_lost_soul_bounce = gameversion >= exe_ultimate` and the cph/fraggle comment in PrBoom/Chocolate
  describe this; it desyncs Doom II v1.9 demos with close lost soul fights. CGDoom has linuxdoom's version.
  - Not relevant to doom1.wad: it has no lost souls (no `SKUL` sprites).
  - The Freedoom demos and bot demos that matched the oracle evidently had no such event.
  - A fix would key on `gamemode` (`commercial`, `shareware` and `registered` use the old order). It would change
    goldens with lost souls, and the linuxdoom oracle could no longer check it. It is left for a separate change.
- **Repeated `-fast` demos.** Vanilla's halving accumulates and is never undone outside nightmare. After two fast
  demos in a title loop, the `S_SARG_RUN*` tics reach 0, and `P_SetMobjState`'s zero-tic loop can then spin.
  CGDoom now shares this vanilla bug. No IWAD demo sets the fast flag.

## Savegame and demo hardening (2026-10-10): bounded saves, checked loads and demos

Vanilla trusted its savegames and demos: three memory-safety bugs, each reproduced under ASan first (reproductions:
`tests/save-demo-robustness.sh`, 59 of its 68 cases fail on the previous engine).

### What changed

- **Saves (`G_DoSaveGame`, p_saveg.c):** the archive is bounded by `save_end`: `P_SaveRoom` checks every record
  (players, the world block, each thinker and special with its class byte and padding, the end markers) before it is
  written, and stops writing at the first one that does not fit (`save_overflow`). Vanilla wrote past the 0x2c000-byte
  buffer and only then failed ("Savegame buffer overrun"): about 1100 things on the calculator (160-byte mobjs), 780
  on the 64-bit host (232 bytes); Freedoom E3M3, E4M2 and MAP12 overflow on both (see below). Now nothing is written and the slot keeps its file
  and any delayed copy (it used to be freed first); the HUD shows `GGSAVETOOBIG` ("save failed: level too large!").
  The format of saves that fit is unchanged (compared with the previous engine, raw pointers and padding aside).
- **Loads (`G_DoLoadGame`, p_saveg.c):** `save_end` = buffer + length. The header is read into locals and checked by
  `G_ValidSaveHeader` before `G_InitNew` (bad skill, episode or map for the game mode, a map the WAD lacks, player
  flags other than 0/1, player 0 or consoleplayer not in the game): the load is refused and the current game goes on.
  After it, `P_LoadCheck` precedes every read and the indices are checked: states (mobjs, psprites), sprite frames the
  renderer would draw (it checks them only with RANGECHECK), thing types, mobj player numbers (and in game),
  `lastlook`, `movedir`, sectors of specials, flats and textures of the world block and of donut/lowerAndChange
  floors, weapons, and the player fields the status bar or the renderer index with (health, damage and bonus counts,
  fixedcolormap). Every player in the game needs a body after the thinkers. A violation is `I_Error("Bad savegame:
  ...")`, before any out-of-bounds access. `mobj->tracer` is cleared like `target` (vanilla kept a pointer of the
  saving session; every user accepts NULL), and the link pointers of MF_NOSECTOR/MF_NOBLOCKMAP things are cleared.
- **Demos (`G_DoPlayDemo`, `G_ReadDemoTiccmd`):** the demo length is known (`W_LumpLength`, `M_ReadFile`'s result)
  and `demoplayend` ends playback exactly like the 0x80 marker. A demo shorter than its 13-byte header, with player
  flags other than 0/1, consoleplayer outside the game (vanilla indexed `players[]` with any byte), or without player
  0 (the only player this engine runs, renders and reads ticcmds for; with no player, monsters looked for one
  forever) is rejected before anything changes, like a demo of another version (`G_RejectDemo`: fatal in demo-only
  mode, otherwise the title loop goes on; a lump is made purgeable again, a file buffer freed). The bodies of all
  players are cleared before `G_InitNew`, and a player in the game whose start the map lacks is a fatal error.
- **Demo ticcmds that save (`G_Ticker`):** `BTS_SAVEGAME` with slot 6 or 7, which only a damaged demo can carry, is
  ignored (slot 6 was the recorded demo's delayed write, 7 past `CGD_DelayedSaves`).
- **gint hints:** "Demo is damaged" gets the demo hint, "Bad savegame" a new one (delete the .DSG).

### Tests

- `make test-robust` (`tests/save-demo-robustness.sh`, in `test-unit`; `test-asan` runs it under ASan): see
  docs/DEVELOPMENT.md §4. `tests/savetool.py` patches savegames using the layout printed by `cgdoom-host
  -savelayout` (also for the 32-bit big-endian simulator build) and generates the test WADs.
- Goldens, demo round trip and demo sync unchanged.

### For later agents

- A new archived field that indexes an array must be checked in its `P_UnArchive*` function, and a new record must
  call `P_SaveRoom`/`P_LoadCheck` before it is written or read.
- Known and left: `mobj->target` is still NULL after a load (vanilla), so `A_SpawnFly` (a boss cube in flight) can
  dereference NULL after loading such a save (`A_Fire` now stops at the cleared tracer first); a save made by the
  64-bit host is not loadable on the calculator (struct images; as before).
- The buffer is too small for some stock levels: with the calculator's struct sizes (simulator, a 1 MiB buffer to
  measure) Freedoom E3M3 needs 185,610 bytes at its start, E4M2 213,886 and Doom II MAP12 243,898 (MAP20 124,634,
  MAP29 80,558). These saves are now refused ("level too large"); before, they wrote past the buffer.

## External review fixes (2026-10-11)

A review (ChatGPT, made before the fx-CG100 scope cleanup) reported six bugs. Two can happen on the target
(fx-CG100, shareware `doom1.wad`, single player) and are fixed; the other four cannot and are left.

- **Special keys before the first level (`st_stuff.c`):** the status bar's `plyr` is set by `ST_Start()`, so on
  the title screen before any level it is NULL, and frameskip, gamma, memory statistics, cheat, noclip and the
  profiler report wrote through it (a crash on the calculator; the host's ASan/UBSan run reports the NULL access).
  The setting keys now apply without a message there; cheat and noclip do nothing. Test: `make test-title-keys`
  (`tests/title-keys.sh`, `tests/input/title-keys.txt`; the host input script gained a `profiler` key).
- **Fast monsters after a load (`G_DoLoadGame`):** vanilla set `gameskill` to the save's skill before `G_InitNew`,
  which switches the fast monsters by comparing the old and the new skill. Each calculator launch is a fresh
  session, so a Nightmare save loaded from the title had normal-speed monsters, and a normal save loaded during
  a Nightmare game kept fast ones. `G_InitNew` now gets the save's skill while `gameskill` still holds the current
  one. Demos never load saves (demo sync unchanged). `P_SargRunTics()` reports the demon run-state tics for the
  host's `-gamelog` (new fields `skill=` and `sargtics=`, before `idle=`). Test: `make test-skill-load`
  (`tests/skill-load.sh`: a fresh session loads a Nightmare save, a Nightmare game loads a normal save, and the
  reverse).

Left, with the reason:
- Boss cube in flight after a load (`A_SpawnFly`, NULL `target`): Doom II boss brain only (already listed above).
- Stopped perpetual platforms not archived (`P_ArchiveSpecials`, vanilla): a platform only stops through
  linedef types 54/89, which no shareware map has (nor perpetual platforms, 53/87).
- Settings written without the platform's `CG.*` lines when `PL_FileWrite()` cannot copy them: only reached if a
  ~1.4 KiB malloc fails right after `CGD_SaveSettings()`'s 2 KiB one succeeded (it stops with an error otherwise);
  the model marker whose loss reset the keymap no longer exists.
- Texture patch indices not range-checked (`R_InitTextures`, vanilla): needs a corrupted texture table inside an
  otherwise valid WAD; the release WAD is built from a checksummed download.
