# CGDoom for the fx-CG100: developer guide

This guide covers how the port is built, how it is tested without hardware, what was measured, why the
main design choices were made, and what to check on the first hardware run. For users, see
[`../README.md`](../README.md) and the [user guide](USER-GUIDE.md).

Some notes are authoritative. Read them before you change the parts they cover:

- [`notes/design.md`](../notes/design.md): §7 and §8 override the earlier sections.
- [`notes/research-critique.md`](../notes/research-critique.md): the open hardware questions (§B, §C)
  and the probes (§E).
- [`notes/engine-changes.md`](../notes/engine-changes.md): every engine change, with measurements.
- [`notes/gint-review.md`](../notes/gint-review.md) and
  [`notes/final-review.md`](../notes/final-review.md): the platform review findings.
- [`notes/sh4-review.md`](../notes/sh4-review.md) and
  [`src/engine/sh4/README.md`](../src/engine/sh4/README.md): the assembly.
- [`src/sim/README.md`](../src/sim/README.md): the simulator harness.
- [`tests/perf-baseline.md`](../tests/perf-baseline.md): performance.

> **Nothing in this repository has run on a calculator yet.** The calculator code compiles warning-free.
> Its pure logic is unit-tested on the host, and it has been reviewed several times. Every
> hardware-dependent assumption is listed in [§7](#7-hardware-verification) with the probe that settles it.

## 1. Repository layout

```
CMakeLists.txt         calculator build (fxsdk build-cg -> CGDoom.g3a, build-cg/cgdoom.bin)
Makefile               host and simulator builds and tests (includes src/sim/cgdoom.mk)
src/platform/platform.h  the PL_* platform API: the only interface between engine and platform
src/engine/            portable DOOM engine (CGDoom's cgdoom/ + vanilla fixes); includes only platform.h
src/engine/sh4/        SH4AL-DSP assembly: column/span drawers, palette expansion, FixedDiv helper
src/gint/              fx-CG100 platform on gint: launcher, video, input, time, files, WAD, memory, UTLB
src/host/              headless macOS/clang platform; also compiled for the simulator with -DPL_SIM
src/sim/               GDB SH simulator harness: crt0, linker script, trap syscalls, libc glue, profilers
tools/                 build-toolchain.sh, wadtool.py
tests/                 golden CRCs and scenarios, unit tests, tests/gint-host (platform logic on the host)
assets/                g3a icons
notes/                 design, research, reviews, change logs
ref/                   upstream clones (read-only; the toolchain is built from some of them)   [not in git]
toolchain/             built SH toolchain + fxSDK + gint (dev) + fxlibc                       [not in git]
test-wads/             Freedoom 0.13.0 (test data only)                                       [not in git]
```

## 2. Architecture

```
            +-------------------------------------------------------------+
            | src/engine  (DOOM + CGDoom glue: cgdoom.c, cgdoom-kbd.c, ...)|
            |   renderer -> colfunc/spanfunc ------> src/engine/sh4 (__sh__)|
            +------------------------------+------------------------------+
                                           | PL_*  (platform.h)  + CGD_* hooks
              +----------------------------+-------------------+
              |                            |                   |
        src/gint (calculator)      src/host (clang, macOS)   src/host -DPL_SIM
        gint dev, MPM, fxlibc      headless, CRC per frame   + src/sim (sh-elf-run)
```

### 2.1 Engine and platform API

The engine never includes gint or OS headers. Everything platform-specific goes through
`src/platform/platform.h`:

- fatal errors and logging (`PL_Fatal`, `PL_Log`);
- 35 Hz tics and microseconds;
- zones for `Z_Zone` (`PL_GetZones`);
- palette and frame presentation;
- the raw keyboard matrix (`PL_ScanKeyboard`, in CGDoom's layout);
- WAD access (`PL_WadOpen`, `PL_WadRead`, `PL_WadMap` zero-copy, `PL_WadIsSlow`);
- small files (config, saves, demos).

Platform *policy* lives in the platform: the launcher, keymap presets, overclock, delayed-write flushing
and diagnostics. The engine exports a few hooks that the platform sets. gint references
`CGD_EscapeKeycode` and `CGD_EnterKeycode2` weakly, so it still links against an engine without them:

| Hook | Meaning |
|---|---|
| `CGD_EscapeKeycode` | The Doom menu key. gint sets it to BACK (0x82) on the fx-CG100. |
| `CGD_EnterKeycode2` | A second Enter key: OK (0x84) on the fx-CG100, unless an action is bound to it. |
| `CGD_AlphaKeycode` | The save-name digit modifier: 0x72 on the fx-CG100. |
| `CGD_AutoFrameskip` | Auto frameskip: 0 means fixed, N is the maximum number of consecutive skips (the default is 3). |
| `CGD_IsPaused()` | A mask that drives the in-game idle policy. |
| `CGD_WADFileName`, `CGD_SingleEpisodeUltimate` | Launch parameters. |
| `CGD_RecordDemoSlot`, `CGD_PlayDemoOnly`, `CGD_TitleDemos` | Demo recording, demo-only playback and the title demos (on by default). Only the host sets them (`-record`, `-playdemo`, `-demos`). |

Two contracts must hold on every platform (`src/engine/cgdoom.h`):

- **Exit order:** `PL_WadClose()` comes first, then `CGD_FlushDelayedWrites()`. A storage write moves
  flash records and invalidates a block-mapped WAD.
- **8.3 file names.** `CGD_FilePrefix` turns any WAD name into a prefix of at most 5 characters, so the
  file names are `CGDOOM.CFG`, `<P>_<s>.DSG` and `<P>D<nn>.LMP`. The mapping is in
  `src/engine/cgdoom-names.c`.

Engine changes compared with CGDoom are logged in `notes/engine-changes.md`:

- vanilla-exact `FixedMul`/`FixedDiv`;
- exact division helpers;
- renderer limits raised, with the arrays allocated dynamically so that `.bss` stays small;
- low detail with Chocolate Doom's semantics, and view sizes 3–11;
- auto frameskip;
- the `PL_WadIsSlow` precache policy;
- the demo-recording "begun" flag;
- save-name entry fixes.

### 2.2 SH4 assembly (`src/engine/sh4`)

The assembly has these parts:

- **Drawers:** eight hand-scheduled drawers (column, span, their low-detail versions, fuzz, translated).
  The four hot ones are placed in `.ilram`, the on-chip instruction RAM.
- **Palette expansion:** 8-bit to RGB565 (`expand_sh4.S`, generated by `test/gen_expand.py`).
- **Other helpers:** a cache-line fill and `cgd_fixed_div`.

Rules:

- Every routine is bit-exact with its C reference and writes no memory the C reference does not write.
- The engine selects the `*_SH4` drawers in exactly one place: `R_ExecuteSetViewSize`, under `CGD_SH4`
  in `r_draw.h`, which means `__sh__` and not `CGD_NO_SH4`.
- The host always uses the C drawers.
- DSP repeat loops are only used in foreground drawers. Nothing that can run in interrupt context uses
  them, because gint does not save RS/RE.

### 2.3 gint platform (`src/gint`)

| File | Role |
|---|---|
| `main.c` | Entry point, launcher, game start, the single exit path, `PL_Fatal`, idle exit, quit-chord polling, destructor |
| `sys.c` | Model check and a full 10-character OS-string check: the fx-CG100 with `02.00.0000` (gint API 2) or `02.20.0000` (API 3). Builds the BFile call table |
| `memory.c` | Removes the `_os` arena. `PL_GetZones` (malloc reserve 768 KiB) over gint's arenas: `_uram` and mpm.bin 1.1's `_ld1` |
| `wad.c` | `PL_Wad*`, three back-ends: **RAM image** (primary, whenever it fits), **flash block map** (`Bfile_GetBlockAddress`, checked against the file at open), **BFile** reads (fallback) |
| `files.c` | World-switched BFile helpers (P1 bounce buffers), `PL_File*`, delayed writes flushed in one world switch at exit (a replacement that does not fit puts the old file back from RAM), failure reporting |
| `video.c` | Async DMA from an SDRAM RGB565 frame; CPU writes when that frame could not be allocated. The 320x200 frame is centered in the 396x224 panel |
| `input.c`, `keymaps.c` | Raw KEYSC reads only (never gint `getkey`). Launcher keys by physical position. fx-CG100 presets and key names. Rebind checks |
| `time.c` | 35 Hz TMU tic. Microseconds from gint's profiler TMU. The quit chord and key activity sampled in the tic interrupt |
| `clock.c` | The OS clock (launcher, safe start) and F4 (gint `CLOCK_SPEED_F4`) for every game |
| `tlb.c`, `tlbh.S`, `tlbplan.c` | UTLB protection of the code pages (§2.5). `tlbplan.c` is pure logic, tested on the host |
| `diag.c`, `hints.c`, `ui.c` | Diagnostics pages and benchmarks, `PL_Log` ring buffer, fatal-error hints, launcher drawing |
| `stack.S` | Runs the game on a private 128 KiB stack |
| `wadnames.c` | Pure logic: the WAD base name given to the engine |
| `no_osheap.c` | Replaces gint's OS-heap arena at link time: every request fails (NULL) instead of calling OS code before `main()` |
| `uram_check.ld` | Link-time guard: `.data` + `.bss` must leave at least 192 KiB of gint's 512 KiB static RAM (`_uram`) free for gint's own start-up allocations |

**Startup** (`main()`):

1. Record r15. Before any OS call, check the model and the OS string (`gsys_init`). On anything else,
   show "Unsupported calculator" and return, with nothing else set up.
2. Read DEL (safe start).
3. Remove the OS heap arena (`gmem_init`). Snapshot the UTLB and install the miss handler, then pin the
   image (`gtlb_init`).
4. Start the clock state and the timers.
5. Back up on-chip memory across world switches (`GINT_ONCHIP_BACKUP`).
6. Allocate the RGB565 frame (`gvid_init`) and the private stack.
7. Run the memory check (at least 3 MiB, which refuses the older mpm.bin builds). Load the settings, list the WADs (one world
   switch), then run the launcher.

**Game** (`run_game()`):

1. `PL_WadOpen`: RAM image if the image fits in the largest block and 768 KiB + 1 MiB remain; otherwise
   the block map; otherwise BFile.
2. Set the engine parameters (the WAD name, automatic frameskip, the key hooks) and the F4 clock, then
   run `D_DoomMain()` on the private stack. The engine starts the title loop with its demos. The engine is not re-entrant, so after a game the add-in exits.

**Exit.** Every way out but a hang ends in `exit_path()`: a normal quit, `PL_Fatal` (by `longjmp`), the
quit chord, and the idle exit.

1. Wait for the keys to be released, and for the LCD DMA.
2. `PL_WadClose()`.
3. Write the pending files in one world switch.
4. Report write failures, then stop the timers.

The `platform_quit()` destructor then runs on every path, `abort()` included. It closes the WAD file if
it is still open and restores MPM's UTLB layout.
Finally, gint's `kquit` restores the OS clock, the LCD window and the drivers.

### 2.4 Memory

- **Static:** all `.data`/`.bss` sit in gint's 512 KiB `_uram`, shared with gint's VRAM. Do not grow
  static arrays; allocate big renderer arrays at init (design §8).
  - The current build has `.bss` 70 KiB, `.data` 2.7 KiB, `.text` 178 KiB, `.rodata` 139 KiB and
    `.ilram` 1552 B (of about 4 KiB).
  - The binary is 329,832 B and the g3a 358,508 B. The binary limit is 2 MiB (gint's linker script).
- **Heap:**
  - Only the arenas gint creates: `_uram` and the `_ld1` that mpm.bin 1.1 announces (about 5.5 MiB). No
    RAM address is hard-coded. Older mpm.bin builds announce less or none (critique §B3) and are not
    supported.
  - RAM is not zeroed between launches.
  - Zones get the largest blocks, after the malloc reserve.
  - With mpm.bin 1.1, a packed doom1.wad (3318 KiB image) should fit in RAM. That is a calculation only;
    no screen shows the mode a game used since Developer info was removed (2026-10-14). Diagnostics →
    Memory shows the largest free block, against which the image size plus 768 KiB + 1 MiB can be
    compared.

### 2.5 UTLB protection

MPM maps the add-in image at 0x00300000 with 64 KiB UTLB entries, and sets URB=54. The OS runs `LDTLB`
during world switches, so it can evict one of our code pages. gint's own miss handler would then jump
into the fx-CG50 syscall gate and crash. There are four layers (design §7, `src/gint/tlb.c`):

1. **Startup:** take a snapshot of the image entries, and install our own handler at VBR+0x400 that
   reloads them.
2. **Pinning:** move the image entries just below URB and lower URB, using a P2 executor with interrupts
   masked. The OS cannot replace them then, in either world.
3. **Repair:** after every world switch, check and rewrite the slots and URB.
4. **Restore at exit:** everything goes back to MPM's slots and URB. mpm.bin 1.1 flushes the TLB and
   rewrites MMUCR before the next launch anyway; older builds (Feb-2025) did not, which is why this
   exists.

Without the executor nothing is pinned or repaired; the miss handler still reloads evicted pages in the
gint world. **Safe start** (DEL held) uses that path on purpose.

## 3. Builds

**Toolchain.** This needs cmake, libpng, libusb, ncurses and Python 3 with Pillow.
`tools/build-toolchain.sh` was written for macOS with Homebrew (its library paths are Homebrew's; on Linux
it may need adjusting). It builds sh-elf binutils 2.42, GCC 14.1, OpenLibm, fxlibc, gint (pinned source plus this project's OS compatibility patch) and the fxSDK into `toolchain/`, from clones in `ref/`. `ref/`, `toolchain/`,
`test-wads/` and the build directories are gitignored.

```sh
tools/build-toolchain.sh            # everything (or one step: fxsdk binutils gcc openlibm fxlibc gint)
tools/build-toolchain.sh sim        # optional: GDB's SH simulator, for the simulator tests
```

The original release used these upstream revisions. `tools/build-toolchain.sh` still checks out
`dev` for fxsdk and fxlibc; check out the listed commits to reproduce those versions. The gint step
exports its pinned commit and applies `tools/gint/cgdoom-compat.patch` without changing the clone:

| Path | Repository | Commit |
|---|---|---|
| `ref/fxsdk` | `git.planet-casio.com/Lephenixnoir/fxsdk` (dev) | `a6e46eb6df060dc4e6e931521205e516c92b6c7f` |
| `ref/gint` | `git.planet-casio.com/Lephenixnoir/gint` (dev) | `463f180ffb71ccdcef05c928ad19f2a480b14bc1` |
| `ref/fxlibc` | `git.planet-casio.com/Vhex-Kernel-Core/fxlibc` (dev) | `a6903066cf4777aa96556a6da9699d4704867185` |
| `ref/OpenLibm` | `git.planet-casio.com/Lephenixnoir/OpenLibm` | `9828d2e3f265a882af6a5bba8ffe6a00c3eeb8d7` |
| `ref/sh-elf-gcc` | `git.planet-casio.com/Lephenixnoir/sh-elf-gcc` | `fdbc1be7a84e88f45f9b768b7428ff33b77eb9bb` |
| `ref/sh-elf-binutils` | `git.planet-casio.com/Lephenixnoir/sh-elf-binutils` | `a963554d12c0236ee10c7545daa4c422bf107de6` |
| `ref/CGDoom` | `git.planet-casio.com/Lephenixnoir/CGDoom` (upstream engine) | `8516c16d9d3c65b45882433b74abdef171265c99` |
| MPM (built by casio-mpm-220) | `git.planet-casio.com/PlaneteCasio/mpm` (`mpm.bin` 1.1) | `cbd1a414d96b55e14adc20a88e2140f2c8723b5a` |

### OS compatibility

One `CGDoom.g3a` runs on OS 02.00 and OS 02.20; there is no OS-specific build. gint picks its OS-call
table at startup, and `sys.c` then accepts only the exact internal versions below. Stock gint has no
OS 02.20 table, so `tools/build-toolchain.sh gint` exports gint `463f180` and applies
`tools/gint/cgdoom-compat.patch` (the clone in `ref/gint` is left unchanged). The patch:

- adds a fourth column to gint's OS-call table, with the OS 02.20 addresses (API 3);
- selects API 2 and API 3 only on the exact strings `02.00.0000` and `02.20.0000` (stock gint accepts any
  `02.00` prefix);
- skips gint's LCD window save and restore during world switches on the new display controllers (IDs
  `0x32` and `0x52`). gint wrote the old controller's registers 0x210-0x213, which are other commands on
  the new one. OS 02.20 sets its own window before every screen update, so nothing needs restoring.

CMake refuses a stock gint, and CGDoom links against a symbol only the patched gint exports. After
updating an existing toolchain, run `tools/build-toolchain.sh gint` once.

| User-visible OS | Internal string | CGDoom | MPM |
|---|---|---|---|
| 02.00.0202 / 02.00.2202 | `02.00.0000` | Accepted, API 2 | MPM installer and `mpm.bin` 1.1 |
| 02.20.0202 | `02.20.0000` | Accepted, API 3 | casio-mpm-220 patch and `mpm.bin` (release v1.0) |
| 02.10 | `02.10.0000` | Refused | casio-mpm-220 updates it to 2.20; see below |
| 02.00.xx1x and others | other strings | Refused | casio-mpm-220 updates it to 2.20 |

The OS 02.20 addresses come from the stock image with SHA-256
`1413bb39864b9590c80c3ba761b7a1791d5701b9f4fb3fc25c6d7bd9e89d95e7`. They were checked against it: the
code at each address matches the OS 02.00 function (64 bytes, PC-relative fields masked), and the BFile
wrappers that look alike call inner functions that all moved by the same offset. MPM itself (the OS
patch, the `mpm.bin` builds and the firmware research) lives in
[casio-mpm-220](https://github.com/cheparukhin/casio-mpm-220). **On hardware,** CGDoom plays normally on
an fx-CG100 with the new display under OS 02.20: startup, WAD access, a game, and exit to the MPM menu.
Still to check: the old display controller, settings and save write and reload, and relaunch.

**OS 02.10 is feasible, but not enabled by guessing.** MPM 1.1 already contains a
2.10 table. Its `BFile_Write` is `0x80396a92`, while Kin1009's gint commit `ee12efd`
uses `0x80396a06` (the same delta as the adjacent BFile routines). Before enabling
it, compare both addresses with the known write routine in an actual 2.10 image,
check the remaining table, and validate loading under a compatible MPM setup.
No 2.10 image was available locally during this change. See `notes/design.md` §10.

**Test WADs.** The host and simulator tests use Freedoom 0.13.0 (BSD license):

```sh
mkdir -p test-wads/fd && cd test-wads/fd
curl -fLO https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
unzip freedoom-0.13.0.zip && cd ../..
```

The demo-sync tests also need the shareware `doom1.wad` v1.9 in `$CGD_DOOM1_WAD`
(`python3 tools/wadtool.py fetch-wad DOOM1.WAD --save-original doom1.wad` downloads it).
`tools/demosync-oracle/build.sh` takes id Software's `linuxdoom-1.10` source tarball as its argument.

**Build commands:**

```sh
export PATH=$PWD/toolchain/bin:$PATH   # after tools/build-toolchain.sh (+ "sim" step)
fxsdk build-cg          # calculator: CGDoom.g3a + build-cg/cgdoom.bin + build-cg/cgdoom.map (must be warning-free)
make host               # build-host/cgdoom-host (clang -O2 -g, RANGECHECK on)
make host-asan          # build-host-asan/cgdoom-host (ASan + UBSan)
make sim-tool           # once: build-sim/tools/sh-elf-run64 (64-bit counters, PC profiles)
make sim                # build-sim/cgdoom-sim.elf (SH4 drawers); a C-only ELF: make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim
```

**Host CLI.** Run `build-host/cgdoom-host` without arguments for the full list. Common options:

- `-iwad F`, `-warp E M`, `-skill N`, `-tics N`;
- `-playdemo LUMP`, `-record N`;
- `-input builtin:<name>|<file>`;
- `-crc FILE`, `-shot PREFIX`;
- `-lowdetail`, `-viewsize N`, `-demos` (title demos, which the calculator always plays);
- `-autoframeskip N`, `-nodelaysaves`, `-zone KiB,...`, `-fragment N`, `-slowwad`, `-gamelog`, `-stats`;
- `-demotrace FILE`: one line of game state per tic, in the format of the linuxdoom-1.10 oracle
  (`tools/demosync-oracle/`), to compare demo sync.
- `-savelayout`: the savegame record layout of the build (struct sizes and the offsets of index fields), which
  `tests/savetool.py` needs to patch savegames.

The host is deterministic: by default every presented frame advances the virtual clock by one tic, so
CRCs are reproducible.

**Dev loop on hardware** (MPM 1.1): press **SETTINGS** in the MPM menu, then run
`fxlink -pw build-cg/cgdoom.bin`. Push the header-less `.bin`; MPM maps it at 0x00300000.

## 4. Tests

Hardware runs are manual, so automated correctness rests on three things:

- **Golden frames:** the host renders fixed scenarios, and every frame's CRC must match.
- **Bit-exact simulation:** the SH build in the GDB simulator must reproduce the host CRCs.
- **Host unit tests of the calculator platform's pure logic.**

| Command | What it checks |
|---|---|
| `make test-host` | The 35 scenarios of `tests/scenarios.txt` against `tests/golden/*.crc`. These cover Freedoom E1–E3 maps, Doom II maps, the title, demos, finales, renderer limits and the optional modes. Variant lines (`golden=<other>`) must reproduce another scenario's CRCs exactly: the WAD copy path (`-fragment`), small or multiple zones, poisoned zone memory, renderer arrays that start tiny (`-rlimits small`), and the slow-WAD precache path (`-slowwad`). |
| `make test-asan` | The same 35 scenarios under ASan/UBSan, plus the save-entry, unbegun-demo, map-limit, robustness, title-keys and skill-load tests |
| `make test-unit` | `test-fixed` (FixedMul/FixedDiv vs vanilla), `test-div`, `test-names` (8.3 mapping), `test-roundtrip` (record and replay a demo bit-exactly, also with the Doom menu opened while recording), `test-settings` (CGDOOM.CFG), `test-saveentry`, `test-demo-unbegun`, `test-map-limit`, `test-robust`, `test-title-keys`, `test-skill-load` |
| `make test-title-keys` | Every special key (gamma, cheat, noclip, FPS, view size) pressed on the title screen before any level: no access through the missing player (ASan/UBSan in `test-asan`). Then, with title demos on, a key pressed during a demo opens the Doom menu, as in vanilla |
| `make test-skill-load` | Fast monsters follow the skill of a loaded save: a fresh session loading a Nightmare save, a Nightmare game loading a normal save, and the reverse (`-gamelog` `skill=` / `sargtics=`) |
| `make test-map-limit` | `-warp 33` on `freedoom2.wad` starts MAP32: the launcher stops at MAP32 and `G_InitNew` limits a Doom II map to 32, since the engine's HUD titles, par times and intermission names end there. Also part of `test-unit`; `test-asan` runs it under ASan. |
| `make test-robust` | `tests/save-demo-robustness.sh`: damaged, foreign and oversized savegames and damaged demos fail cleanly, with no access outside a buffer under ASan (`test-asan`). Loads of a savegame patched by `tests/savetool.py` (which reads the record layout from `cgdoom-host -savelayout`): a bad header is refused before the current game is touched; a truncation, an index out of range or another level layout is a "Bad savegame" error. A save in a generated copy of `freedoom2.wad` whose MAP02 has 3000 extra things is refused ("level too large") and keeps the slot's previous save. Demos that are too short, have a bad console player or player set, or lack their end marker (played like the marker), from a file, a lump and the title loop. |
| `make check-sh` | Every engine file compiles with sh-elf-gcc |
| `make test-demosync` | Optional. DEMO1-3 of the shareware DOOM v1.9 `doom1.wad` named by `CGD_DOOM1_WAD` must play in sync: the per-tic game-state traces are compared with `tests/demosync/*.ckpt`, which were checked tic by tic against linuxdoom-1.10. Skipped when the variable is not set. `make test-demosync-sim` runs it in the simulator. |
| `make -C src/engine/sh4/test run` | Asm vs the C reference: randomized and bit-exact, with stray-write, stack-canary and calling-convention checks; `asmcheck` |
| `sh tests/gint-host/run.sh` | 11 suites of src/gint logic with stub gint headers, built with ASan/UBSan (`SANITIZE=0` to disable); a suite that fails to build counts as failed: sys (model/OS/API gate, table reset and open wrapper), expand, files (delayed writes, rollback of failed replacements, RAM image, packed images, refusal counting), hints, input, keymaps, memory (OS heap arena, zones), names, time (idle and chord policy), tlb (pin/repair/unpin planning), wadmap (block map, verification) |
| `python3 -m unittest discover -s tests -p 'test_gint_runtime.py' -v` | After rebuilding gint: exercises its actual patched OS selection and old/new LCD world-switch callbacks on the host |
| `make test-sim` | The first 60 tics of e1m1, map01 and demo1 in the simulator vs the goldens (about 6 s) |
| `make test-sim-full` | All 35 scenarios in the simulator vs the goldens (about 4 min) |
| `fxsdk build-cg` | The calculator build, warning-free; `uram_check.ld` link guard |

Other targets:

- `make perf-sim`: the per-frame cost tables and profiles (`build-sim/perf/`).
- `make golden`: regenerates the goldens. Only do this for an intentional output change, and add an
  entry to `tests/golden/CHANGES.md`.
- `make -C src/engine/sh4/test bench|pipe|sched`: the asm design tools.

The golden scenarios need `test-wads/fd/freedoom-0.13.0/freedoom1.wad` and `freedoom2.wad`.
`tests/gint-host/run.sh` runs its WAD tests on the WADs listed in `CGD_TEST_WADS` (default: the Freedoom WADs under `test-wads/`).

**What these tests cannot cover:**

- gint calls and OS calls;
- timing, DMA and the LCD;
- the UTLB on real hardware;
- flash layout;
- the overclock;
- key legends.

[§7](#7-hardware-verification) lists them.

## 5. Performance summary

Details: [`tests/perf-baseline.md`](../tests/perf-baseline.md). These are simulator instructions per frame,
from frame 42 to the end, with view size 10 and full detail. They exclude the host's CRC work.

| Scenario (Freedoom) | CGDoom baseline | After C steps 1–3 | + SH4 drawers | + renderer polish (current) | Total |
|---|---|---|---|---|---|
| e1m1 | 2,017,661 | 1,500,945 | 1,144,003 | 1,077,734 | −46.6% |
| map01 | 1,715,048 | 1,301,013 | 942,498 | 934,689 | −45.5% |
| demo1 | 4,044,224 | 2,378,752 | 2,026,047 | 1,934,915 | −52.2% |
| demo2-1 | 2,868,025 | 1,742,870 | 1,392,076 | 1,367,712 | −52.3% |

- **Optional modes**, compared with the SH4 default:
  - low detail: −36 to −39%;
  - view size 9: −12 to −16%;
  - view size 7: −31 to −38%.
- **The simulator charges 1 cycle per instruction (plus 2 per taken branch)** and models no cache or dual issue. That is why the
  SH4 drawers' gain is a lower bound.
  - Under the SH-4A pipeline model (`src/engine/sh4/README.md` §3), the drawers run 2.3–3.5× faster than
    the C ones.
  - Column drawing costs about 4.0 cycles/pixel, against about 14 for GCC's loop.
- **Illustration only.** Under that model, e1m1 engine work per frame drops from about 13.3 ms to about
  7.9 ms at 118 MHz (the notes' figure for the stock OS clock; F4 runs the CPU at 236 MHz), before the
  renderer polish and not counting presentation. The DMA transfer of a
  frame is bus-bound, about 8 ms, and overlaps the next frame with the default presenter.
- **Not yet known (hardware):**
  - the real frame rate;
  - the cache-miss costs;
  - flash vs SDRAM line-fill costs (the reason the RAM image is the primary path);
  - the presenter costs at each clock.

  Diagnostics → Benchmarks measures the last two.

## 6. Design decisions

| Decision | Why | Where |
|---|---|---|
| Portable engine behind `PL_*`; host and sim platforms | Golden-frame testing and bit-exact asm verification without hardware | design §3–5 |
| Vanilla-exact `FixedMul`/`FixedDiv` (goldens regenerated once) | CGDoom's versions were slower and non-vanilla (48% / 34.5% mismatches) | engine-changes step 1 |
| Renderer limits raised, arrays on the heap | Complex views overflowed vanilla limits; static RAM is the scarcest budget | design §8, step 2 |
| Copy the WAD to RAM (minus sounds), with the block map as fallback | NOR line fills are much slower than SDRAM, and the overclock does not speed them up | critique §C1, `wad.c` |
| All storage writes deferred to exit; `PL_WadClose` first | A write moves flash records and invalidates the block map | critique §B2, `cgdoom.h` |
| A file of the same size is overwritten in place, a file of another size is replaced by remove + create + write. Either way the old file is kept in RAM first and written back if the new one does not fit | create + rename would need room for both copies when storage is nearly full, and `BFile_Rename` would be one more OS entry point never tested on hardware. Without memory for the backup, the replacement still goes ahead, so a failure then loses the old file | `files.c` (`os_flush`) |
| Block map checked against the file at open | A wrong pointer would make the game read garbage; on a mismatch, CGDoom uses BFile reads instead | final review §16 |
| Model and full OS-string check before any OS call; anything but the fx-CG100 with exact `02.00.0000` or `02.20.0000` quits | An unknown OS table would reach the dead fx-CG50 gate; the g3a also starts on the fx-CG50 | critique §B6, `sys.c` |
| mpm.bin 1.1 only; `_os` removed | 1.1 announces 5.5 MiB to gint; older builds hid RAM that needed per-build discovery. Out-of-memory must return NULL, not call the OS | critique §B3, `memory.c` |
| UTLB pinning + repair + exit restore; safe start | The OS can evict code pages during world switches; stale entries can reset the next launch | design §7, `tlb.c` |
| Raw KEYSC only; matrix codes; weak hooks for menu, Enter and ALPHA keys | gint remaps CG100 rows 5–9; CGDoom keymaps are matrix codes | design §7–8, `input.c` |
| BACK = Doom menu, OK = second Enter | 0x84 (CGDoom's MENU) is OK on the fx-CG100, the launcher's select key | `keymaps.c` |
| Overclock F4 for every game, no option | F4 is gint's supported preset and the game plays normally with it; Ptune4 F5's 118 MHz bus fails on some units (an opt-in until 2026-10-14) | `clock.c` |
| Async DMA presenter only (CPU writes as the fallback) | Overlaps the bus-bound transfer with rendering, and it works on the new display; the X/Y strip presenter and the option to pick one were removed on 2026-10-14 | `video.c` |
| Hot drawers in ILRAM | On-chip memory is re-initialized by world switches (code is fine, data is not); gint backs it up | design §7 |
| 128 KiB private game stack | The OS stack's size and location are unknown (critique §B7) | `main.c`, `stack.S` |
| One exit path; the quit chord is latched by the tic interrupt; `abort()` only for a hang; destructor | Saves survive every exit but a hang; the UTLB is restored even after `abort()` | gint-review rounds 2–3, `time.c` |
| Idle exit (10 min, or 30 min in a user game) | The add-in owns the interrupts, so the OS auto power-off cannot run | critique §C7 |
| Auto frameskip (max 3), no option | Game speed stays at 35 Hz; frames are dropped instead | design §8 |
| Launcher: the WADs, Controls and Diagnostics only; vanilla's title loop with its demos | Every other setting has one right value on the fx-CG100, or is chosen in the game (skill, episode, view size, detail) | design §11 |
| One game per launch | The engine's static state is not re-entrant | design §8 |
| Strict 8.3 names | The calculator's Memory app shows longer names truncated (`~1`) and cuts extensions to 3 characters | critique §B1, `cgdoom-names.c` |

## 7. Hardware verification

### 7.1 What only hardware can answer

Every probe in critique §E that remains is on the Diagnostics screen (launcher → *Diagnostics...*).
Results are shown on screen only; there is no fxlink text channel. The Developer info screens, which also
showed statistics before and after a game (WAD mode, stack and zone peaks, UTLB repairs during the game),
were removed on 2026-10-14 with the other launcher options.

| Question | Probe | Where it is shown |
|---|---|---|
| Model, OS | Model string, OS string | System, line 1 |
| Which LCD controller (critique §A3) | `r61524_get_version()` | System: "LCD controller id" |
| Where the stack is (§B7) | r15 at entry | System: "r15 at entry" |
| Which mpm.bin (§A5, §B3) | `gint_load_info`, phys(0x00300000), phys(0x08100000), image size; the `_ld1` arena | System: loadinfo / phys / Image lines; Memory |
| Stock clock vs F1 (§B8) | FRQCR, FLLFRQ, `clock_get_speed()`, CS0WCR/CS3WCR/CS3BCR, Iφ/Bφ/Pφ | System (read at the current clock; the launcher runs at the OS clock) |
| Heap layout (§B3) | Arena table: name, start, end, free, largest; OS heap removed; zones and reserve once a game has run | Memory |
| Flash layout and block-map pointers (§A8, §B2, §C4) | Blocks, mapped, P1/P2/P0/other/error/unaligned counts, contiguous runs, longest run, physical range, "map did not match" | Storage (probe a WAD with OK/EXE) |
| Does the OS reload TLB entries during world switches (§C3) | UTLB slots changed by the probe's world switches; URB before and after; repairs and misses | Storage after a probe; System UTLB lines |
| UTLB pinning worked | "UTLB pinned N pages: URB x (was 54)", or "executor off", or "SAFE START" | System |
| Flash vs SDRAM speed (§C1) | 64 KiB cold read, flash P1 vs SDRAM P1 | Benchmarks (OK/EXE) |
| Presenter cost (§B4) | 320x200 push with the game's path (DMA, or CPU without the frame buffer), at the OS clock and at F4 | Benchmarks |
| Tic timer obtained | TMU number, or "polled clock" | System, last line |
| `BFile_Read` return convention | Both conventions are accepted (`gfs_read_ok`); a failure shows as an open error | Launcher error / Log |
| Partial-window DMA (§A2, §B4) | The picture is correct | In game |
| Exit leaves a clean UTLB | Another add-in launches normally after CGDoom | MPM |

Not implemented from §E: the SH-4A PMU check and the cache-size stride sweeps (item 7, optional).

**Results so far:** on an fx-CG100 with the new display and OS 02.20, CGDoom starts, loads
DOOM1.WAD, plays normally and exits to the MPM menu (with the default settings of that build: F4, the
async DMA presenter, automatic frameskip, the WAD copied to RAM when it fits, which are now the only
ones). The probes above have not been recorded yet.

### 7.2 Hardware test: step by step

Use a fresh build: `fxsdk build-cg`, warning-free. Write down the result of every step, and take photos of
every Diagnostics page. Stop at the first failure and report it with the step number.

**Prepare**

1. Calculator: fx-CG100 / Graph Math+ with MPM from casio-mpm-220 (OS 02.20.0202). Note the OS from
   HOME → System → Version, and the free storage from the Memory app.
2. On the PC, run `python3 tools/wadtool.py fetch-wad DOOM1.WAD --save-original doom1.wad`, then
   `python3 tools/wadtool.py verify doom1.wad DOOM1.WAD`.
3. Copy `CGDoom.g3a` and `DOOM1.WAD` to the storage root, next to MPM's `mpm.bin`.

**Startup and launcher**

4. Press HOME → TOOLS → CGDoom.
   - **Expected:** "Looking for WAD files...", then the launcher titled "CGDoom for fx-CG100 / Graph
     Math+", with "Play Doom (shareware) DOOM1.WAD (3.3M)".
   - If you get a black screen, a reset or a panic, retry while holding DEL (safe start), and record
     which one happened.
5. Check navigation: Up/Down, OK and EXE select, BACK returns.

**Diagnostics**

6. System page: record every line. Expected:
   - "fx-CG100/Graph Math+, OS 02.00.0000" or "OS 02.20.0000", matching the installed OS;
   - a loadinfo line;
   - "UTLB pinned N pages: URB x (was 54)";
   - "Tic timer: TMU n (35 Hz)";
   - "speed F…" at the OS clock (this answers §B8).
7. Memory page: record the arena table and the total free KiB (about 5.5 MiB expected by calculation
   with mpm.bin 1.1; under 3 MiB CGDoom would have refused to start). Expected: "OS heap arena: removed".
8. Storage page: select DOOM1.WAD and press OK/EXE.
   - Record: blocks, mapped, P1/P2/P0 counts, runs and longest run, physical range, map hits, and UTLB
     slots changed with URB before → after.
   - Expected: no "(map did not match)", and URB unchanged after the probe.
   - Probe it twice. The figures should be identical.
9. Benchmarks page: press OK/EXE and record both blocks: OS clock and F4.
    - Expected: flash is slower than SDRAM.
    - A picture flickers on the screen during the push tests; that is normal.
10. Log page: record the lines.

**First game**

11. Start the game from the WAD row.
    - **Expected:** the title screen, then the shareware demos (E1M5, E1M3, E1M7) in turn. Let at least one
      demo play for a minute: it must look like normal play (no desync, such as the player walking into
      walls).
    - Press any key during a demo: the Doom menu opens. Start *New Game*, episode 1, *Hurt me plenty*.
12. In the game, check:
    - the picture is centered and has correct colors, and there is no garbage in the borders;
    - the FPS counter (PageDown), in a few places (record the values);
    - every action of the *Thumbs* preset, from the [user guide](USER-GUIDE.md#presets) table;
    - BACK opens the Doom menu, and EXE and OK select;
    - *Options* → *Graphic Detail*, and the view-size keys. Leave the view one step smaller (**-**), so
      that `CGDOOM.CFG` is written at exit (it is only written when a setting changed).
13. Save to slot 0 (the slot is pre-filled with E1M1, so press EXE). Expected: "Save file will be created
    on exit". Load it back in the same session.
14. Play for at least 10 minutes over E1M1–E1M3, including a level change.
15. Quit from the Doom menu (*Quit Game*, then EXE).
    - Expected: "Writing N files...", then a return to MPM without an error.
16. Over USB, check that `DOOM1_0.DSG` and `CGDOOM.CFG` exist. Check that the calculator clock is still
    right.

**Second launch and other paths**

17. Relaunch CGDoom. The smaller view of the last game should have persisted. Load the save from the
    Doom menu.
18. Launch another add-in from MPM after CGDoom has exited (UTLB restore). Then relaunch CGDoom.
19. In a game, press SHIFT + 7 + 3 + AC/ON. Expected: the normal exit, with its files written.

**Report:**

- the step results and photos;
- the OS, the mpm.bin build and the WAD size;
- any fatal-error text, with its hint line;
- whether a safe start changed anything.

Use these results to update the "[G]" (guess) items in the research notes, and the user guide's performance
section.
