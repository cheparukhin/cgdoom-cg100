# CGDoom for the fx-CG100: developer guide

This guide covers how the port is built, how it is tested without hardware, what was measured, why the
main design choices were made, and what to check on hardware. For users, see [`../README.md`](../README.md)
and the [user guide](USER-GUIDE.md).

The other developer documents:

- [`PLATFORM.md`](PLATFORM.md): facts about the fx-CG100, MPM and gint that the port relies on, each marked
  as verified, observed on hardware or guessed.
- [`src/engine/sh4/README.md`](../src/engine/sh4/README.md): the SH4 assembly, its pipeline model and its tests.
- [`src/sim/README.md`](../src/sim/README.md): the GDB SH simulator harness.
- [`tests/perf-baseline.md`](../tests/perf-baseline.md): performance measurements and how to reproduce them.

**Hardware status:** CGDoom plays normally on an fx-CG100 with the new display under OS 02.20. Everything
else that depends on the hardware is listed in [§7](#7-hardware-verification) with the probe that settles it.
The HD add-in ([§2.7](#27-screen-formats-and-the-hd-add-in)) has not run on hardware.

## 1. Repository layout

```
CMakeLists.txt         calculator build (fxsdk build-cg -> CGDoom.g3a + CGDoomHD.g3a, build-cg/*.bin)
Makefile               host and simulator builds and tests (includes src/sim/cgdoom.mk)
warnings.mk            the warning flags of every build; errors in the test builds (§4)
src/platform/platform.h  the PL_* platform API: the only interface between engine and platform
src/platform/cgd_config.h  build switches: screen size, extras (§2.7)
src/engine/            portable DOOM engine (CGDoom's cgdoom/ + vanilla fixes); includes only platform.h
src/engine/sh4/        SH4AL-DSP assembly: column/span drawers, palette expansion, FixedDiv helper
src/gint/              fx-CG100 platform on gint: launcher, video, input, time, files, WAD, memory, UTLB
src/host/              headless macOS/clang platform; also compiled for the simulator with -DPL_SIM
src/sim/               GDB SH simulator harness: crt0, linker script, trap syscalls, libc glue, profilers
tools/                 build-toolchain.sh, gint/source.env, wadtool.py, demosync-oracle/
tests/                 golden CRCs and scenarios, unit tests, tests/gint-host (platform logic on the host)
assets/                g3a icons
docs/                  user guide, developer guide, platform reference
notes/                 ideas for follow-up projects
ref/                   upstream clones (read-only; the toolchain is built from some of them)   [not in git]
toolchain/             built SH toolchain + fxSDK + patched gint + fxlibc                     [not in git]
toolchain-build/       the toolchain's build tree                                             [not in git]
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
        patched gint, MPM, fxlibc  headless, CRC per frame   + src/sim (sh-elf-run)
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

Platform *policy* lives in the platform: the launcher, keymap presets, the clock, delayed-write flushing
and diagnostics. The engine exports a few hooks that the platform sets. gint references
`CGD_EscapeKeycode` and `CGD_EnterKeycode2` weakly, so it still links against an engine without them:

| Hook | Meaning |
|---|---|
| `CGD_EscapeKeycode` | The Doom menu key. gint sets it to BACK (0x82) on the fx-CG100. |
| `CGD_EnterKeycode2` | A second Enter key: OK (0x84) on the fx-CG100, unless an action is bound to it. |
| `CGD_AlphaKeycode` | The save-name digit modifier: 0x72 on the fx-CG100. |
| `CGD_AutoFrameskip` | Auto frameskip: 0 means fixed, N is the maximum number of consecutive skips (the default is 3). |
| `CGD_IsPaused()` | A mask that drives the in-game idle policy. |
| `CGD_WADFileName`, `CGD_SingleEpisodeUltimate` | Launch parameters. `CGD_WADFileName` is the full base name; the engine shortens it itself. |
| `CGD_RecordDemoSlot`, `CGD_PlayDemoOnly`, `CGD_TitleDemos` | Demo recording, demo-only playback and the title demos (on by default). Only the host sets them (`-record`, `-playdemo`, `-demos`). |

Three contracts must hold on every platform (`src/engine/cgdoom.h`, `platform.h`):

- **Exit order:** `PL_WadClose()` comes first, then `CGD_FlushDelayedWrites()`. A storage write moves
  flash records and invalidates a block-mapped WAD.
- **8.3 file names.** `CGD_FilePrefix` turns any WAD name into a prefix of at most 5 characters, so the
  file names are `CGDOOM.CFG`, `<P>_<s>.DSG` and `<P>D<nn>.LMP`. The mapping is in
  `src/engine/cgdoom-names.c`.
- **Some PL_* calls may not return.** On gint, `PL_GetTics`, `PL_WadRead`, `PL_WadMap` and `PL_Fatal` can
  `longjmp` to the single exit path (quit chord, in-game idle exit). That is safe only because the engine
  runs once per launch. `PL_WadOpen` is deliberately not a poll point: the tic timer is paused during
  world switches, so the chord's 2-second hold cannot build up there.

**Engine rules:**

- The engine compiles for 64-bit clang (host) and for 32-bit big-endian `sh-elf-gcc` (calculator and
  simulator). WAD data is read with the little-endian readers of `m_swap.h`; no unaligned multi-byte loads,
  because the SH4 faults on them.
- `I_Error` ends in `PL_Fatal`.
- `FixedMul` is a header inline, `(int64)a*b >> 16` as in vanilla. Nothing may take its address or call
  `_FixedMul` from assembly. `FixedDiv` is vanilla-exact; its one deviation is that `a == MININT` with a
  quotient that does not fit saturates instead of calling `I_Error`. `make test-fixed-sim` and
  `make test-div-sim` are the exactness gate for any SH change to either.
- Any code that sets `lumpcache[i]` to a mapped WAD pointer must also set `W_LumpMapped[i]`.
- No code may keep a `PU_CACHE` pointer across `P_SetupLevel`: in slow-WAD mode, `W_SlowLevelStart` purges
  every cached block there.
- The column drawers and `R_GetColumn` must not read the `rw_*`, `topfrac`/`bottomfrac`, `pixhigh`/`pixlow`,
  `walllights`, `markceiling`/`markfloor` or plane globals: `R_RenderSegLoop` keeps them in registers.
- A new growable renderer array follows `R_AllocZeroed`/`R_FreeArray`: zero-filled, never moved while
  something points into it, with a `-rlimits small` path and a scenario that must reproduce a golden.
- A new archived savegame field that indexes an array is checked in its `P_UnArchive*` function, and every
  record calls `P_SaveRoom`/`P_LoadCheck` before it is written or read.
- The `info.c` sound fields are game state (`A_Look`, `A_Chase` and `A_Scream` read them for `P_Random`).
  An action function emptied because it "only plays a sound" must be checked against vanilla for
  `P_Random` calls.

### 2.2 SH4 assembly (`src/engine/sh4`)

The assembly has these parts:

- **Drawers:** eight hand-scheduled drawers (column, span, their low-detail versions, fuzz, translated),
  for any `SCREENWIDTH` that is a multiple of 4 below 512 (320 or 396). The four hot ones are placed in
  `.ilram`, the on-chip instruction RAM.
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
| `memory.c` | Removes the `_os` arena. `PL_GetZones` (malloc reserve 768 KiB) over gint's arenas: `_uram` and mpm.bin's `_ld1` |
| `wad.c` | `PL_Wad*`, three back-ends: **RAM image** (primary, whenever it fits), **flash block map** (`Bfile_GetBlockAddress`, checked against the file at open), **BFile** reads (fallback) |
| `files.c` | World-switched BFile helpers (P1 bounce buffers), `PL_File*`, delayed writes flushed in one world switch at exit (a replacement that does not fit puts the old file back from RAM), failure reporting |
| `video.c` | Async DMA from an SDRAM RGB565 frame (the palette expansion); CPU writes when that frame could not be allocated. A 320x200 frame is centered in the 396x224 panel, a 396x224 frame fills it |
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
7. Run the memory check (at least 3 MiB, which refuses mpm.bin builds older than 1.1). Load the settings,
   list the WADs (one world switch), then run the launcher.

**Game** (`run_game()`):

1. `PL_WadOpen`: RAM image if the image fits in the largest block and 768 KiB + 1 MiB remain; otherwise
   the block map; otherwise BFile.
2. Set the engine parameters (the WAD name, automatic frameskip, the key hooks) and the F4 clock, then
   run `D_DoomMain()` on the private stack. The engine starts the title loop with its demos. The engine is
   not re-entrant, so after a game the add-in exits.

**Exit.** Every way out but a hang ends in `exit_path()`: a normal quit, `PL_Fatal` (by `longjmp`), the
quit chord, and the idle exit.

1. Wait for the keys to be released, and for the LCD DMA.
2. `PL_WadClose()`.
3. Write the pending files in one world switch.
4. Report write failures, then stop the timers.

The `platform_quit()` destructor then runs on every path, `abort()` included. It closes the WAD file if
it is still open and restores MPM's UTLB layout. Finally, gint's `kquit` restores the OS clock, the LCD
window and the drivers. There is no return to the OS menu: gint's power-off and OS-menu calls panic on the
fx-CG100 ([PLATFORM.md](PLATFORM.md)), so quitting means returning from `main()` to MPM.

### 2.4 Memory

- **Static:** all `.data`/`.bss` sit in gint's 512 KiB `_uram`, shared with gint's VRAM. Do not grow
  static arrays; allocate big arrays at init or from the zone.
  - The current build has `.bss` 70 KiB, `.data` 2.7 KiB, `.text` 178 KiB, `.rodata` 139 KiB and
    `.ilram` 1552 B (of 4 KiB). The X and Y on-chip RAMs are unused.
  - The binary is 329,816 B and the g3a 358,492 B. The binary limit is 2 MiB (gint's linker script).
- **Heap:**
  - Only the arenas gint creates: `_uram` and the `_ld1` that mpm.bin announces (about 5.5 MiB with
    mpm.bin 1.1 and casio-mpm-220's build). No RAM address is hard-coded. Older mpm.bin builds announce less
    or none and are refused by the 3 MiB check.
  - RAM is not zeroed between launches.
  - Zones get the largest blocks, after the malloc reserve.
  - A packed doom1.wad (3318 KiB image) should fit in RAM. That is a calculation only; only the timedemo's
    result screen shows the WAD mode a game used. Diagnostics → Memory shows the largest free block,
    against which the image size plus 768 KiB + 1 MiB can be compared.
- **Savegames** are built in a 176 KiB buffer (`SAVEGAMESIZE`), malloc'd for the duration of the save
  (the zone is the fallback). In the zone it needed 176 KiB in one piece, which the delayed copies of
  earlier saves (`PU_STATIC`, kept until exit) and the levels loaded around them could break up, and
  `Z_Malloc` then ended the game. Shareware levels fit in the buffer; a few larger levels of other IWADs do
  not (§8).

### 2.5 UTLB protection

MPM maps the add-in image at 0x00300000 with 64 KiB UTLB entries, and sets URB=54. The OS runs `LDTLB`
during world switches, so it can evict one of our code pages. gint's own miss handler would then jump
into the fx-CG50 syscall gate and crash. There are four layers (`src/gint/tlb.c`):

1. **Startup:** take a snapshot of the image entries, and install our own handler at VBR+0x400 that
   reloads them.
2. **Pinning:** move the image entries just below URB and lower URB, using a P2 executor with interrupts
   masked. The executor never leaves two valid entries for one page (a multiple hit resets the CPU). The
   OS's `LDTLB` replaces the entry at MMUCR.URC, which then cannot reach the pinned slots, in either world.
3. **Repair:** after every world switch, check and rewrite the slots and URB. This also catches an OS that
   writes explicit slots, which pinning cannot prevent; a nonzero repair count on Diagnostics → System
   would show it.
4. **Restore at exit:** everything goes back to MPM's slots and URB. mpm.bin 1.1 also rewrites all 64
   UTLB entries and resets URC before the next launch, so this is defence in depth.

Without the executor nothing is pinned or repaired; the miss handler still reloads evicted pages in the
gint world. **Safe start** (DEL held) uses that path on purpose. It is a held key rather than a setting
because reading `CGDOOM.CFG` is itself a world switch, and pinning must happen before the first one.

### 2.6 Engine changes compared with CGDoom

The speed-ups are in [§5.1](#51-where-the-speed-comes-from). The other changes are below.

**Structure.** CGDoom's engine files called the Casio OS (`Bfile_*`, `Bdisp_*`, ...) directly, with an
SDL2 emulation of those calls for the PC build, and read the WAD through their own flash-fragment code
(`cgdoom-frag.c`). Here the engine is portable C behind `PL_*` (§2.1): the OS calls, the flash mapping and
the WAD back-ends are in `src/gint`, and `src/host` runs the same engine headless for the tests.

**CGDoom bugs fixed:**

- `I_Error` returned to its caller, which then carried on with a NULL block or a −1 lump. It now ends in
  `PL_Fatal`.
- `W_InitMultipleFiles` ran twice and leaked the first lump directory.
- `I_GetTime` counted at 64 Hz instead of 35 Hz.
- `R_GenerateLookup` counted patches in a 320-byte stack array, which textures wider than 320 overflowed;
  the array now grows to the texture's width.
- Full-screen pictures (title, help, finale) were read from flash fragment by fragment, with a post
  offset that shifted their lower part by a row. They are now cached and drawn with `V_DrawPatch`.
- The aligned-lump test checked 2-byte alignment where 4 bytes are needed.
- Savegames were built in the screen buffer; they now have their own 176 KiB buffer (§2.4).

**Vanilla parity.** CGDoom's arithmetic and parts of its game logic differed from linuxdoom-1.10, so
id's demos desynced. The engine now matches linuxdoom tic by tic on DEMO1-3 of the shareware DOOM v1.9
`doom1.wad` (`make test-demosync`):

- `FixedMul` was a bit-serial loop that rounded toward zero, and `FixedDiv` never computed the last
  quotient bit: about 47% of opposite-sign `FixedMul` pairs and 34–37% of `FixedDiv` pairs gave
  non-vanilla results. Both are now vanilla's (rules in §2.1).
- CGDoom removed three `P_Random` calls along with the sound code: the sound picks in `A_Look`, `A_Chase`
  and `A_Scream`. It also zeroed the sound fields of `info.c`. Both are back (`sounds.h` holds linuxdoom's
  sound numbers), but still no sound is played.
- `P_LookForPlayers` has vanilla's loop over the players again. CGDoom looked at player 0 directly, which
  changes when monsters wake up.
- `A_CloseShotgun2` calls `A_ReFire` again, so a held fire button refires the super shotgun.
- Nightmare and `-fast`: `P_SetFastMonsters` keeps RAM copies of the demon run-state tics and the three
  missile speeds, and patches them as vanilla's `G_InitNew` does (CGDoom's tables are `const`).
  `respawnparm` is honoured.
- The menu does not pause the game during demo playback. A dead player is reborn at level load, and
  demo-only playback stops at the demo marker.

**Renderer:**

- renderer limits raised (complex views overflowed vanilla's), with the arrays allocated from the zone
  because static RAM is the scarcest budget:

  | Array | Vanilla | Grows by | Hard limit |
  |---|---|---|---|
  | visplanes | 128 | blocks of 64 | 1024 |
  | drawsegs | 256 | doubling | 2048 |
  | vissprites | 128 | doubling | 1024 |
  | openings | 320×64 shorts | blocks of the same size | 16 blocks |

  Growth never changes a pixel. Visplanes are hashed (64 buckets) without changing the match order. The
  worst case at the hard limits is about 1.5 MiB; every Freedoom map peaks well below it;
- low detail with Chocolate Doom's semantics, using vanilla's `colfunc`/`spanfunc` pointers again;
- view sizes 3–11 (CGDoom allowed 1–10). The reduced-view border is redrawn after menus, the automap and
  size changes (CGDoom left garbage there), `R_VideoErase` clears to black, and the full-screen automap
  redraws the status bar;
- the midpoints of absent upper and lower wall textures no longer overflow (`R_StoreWallRange`; undefined
  behaviour in vanilla).

**Pacing and WAD access:**

- auto frameskip: tic *g* is due at a fixed time, `D_Display` is skipped while the next tic is already due
  (at most N times in a row), and the lag is capped at N tics, so after a stall the game slows down instead
  of fast-forwarding. Game logic and demos are unaffected;
- the `PL_WadIsSlow` policy: when WAD pointers are in flash, lumps are copied into the zone (and the level
  precached) until a reserve of max(25% of the zone, 256 KiB) is left;
- a lump is referenced in place only if it and the `W_LUMP_PAD` bytes after it lie inside the WAD;
  otherwise it is copied with a zero-filled pad, so the renderer never reads past the WAD.

**Robustness.** Vanilla trusted its savegames and demos:

- A savegame is written only if every record fits the buffer. Otherwise it is refused, and the slot keeps
  its previous file.
- A load checks the header before touching the current game, then every index it reads ("Bad savegame").
- A demo's length, players and console player are checked before playback. Save commands in a damaged
  demo are ignored.
- The special keys no longer write through the missing player on the title screen.
- Gameplay keys held into save-name entry are released.
- A recording takes no ticcmd while the menu pauses the level, and a recording that never began is not
  saved.
- Fast monsters follow the skill of a loaded save.
- Save-name entry: empty slots are pre-filled with the map name (`E1M3`, `MAP07`), digits and space can be
  typed, a letter or digit counts only when its own key goes down (they share keys through ALPHA), and the
  presses of the scan that opens or closes the entry are not typed into the name.
- `CGDOOM.CFG` is parsed strictly: exact key names, numbers of 1–9 digits within the line, and
  `Keymap.<action>=NONE` unbinds an action. Unknown keys and malformed values are ignored; an empty or
  wrong `Version` rejects the file.

### 2.7 Screen formats and the HD add-in

Two compile-time switches in `src/platform/cgd_config.h` (only `#define`s, so the assembly reads them
too) select the build; everything else derives from `SCREENWIDTH` and `SCREENHEIGHT`. Pixels are 8-bit
palette indices in every build.

| Build | Switch | Screen | Extras | Host | Goldens |
|---|---|---|---|---|---|
| `CGDoom.g3a` | none | 320x200 | none | `make` | `tests/golden` |
| (`CGDoomW.g3a`, optional) | `CGD_WIDESCREEN` | 396x224 | none | `make VARIANT=wide` | `tests/golden-wide` |
| `CGDoomHD.g3a` | `CGD_HD` | 396x224 | all (`CGD_EXTRAS`) | `make VARIANT=hd` | `tests/golden-hd` |

The 320x200 build compiles as before: no golden changed. The two add-ins share `CGDOOM.CFG` and the
savegames (the archived structures are the same; `cgdoom-host -savelayout` prints identical layouts).

**Widescreen** follows Crispy Doom's "Hor+": the focal length stays the 320-wide view's (`projection`
is half of `viewwidth_nonwide`, 320 at full size and `blocks*32` below, halved in low detail), and the
angle tables cover the wider `viewwidth`, so the view shows more to the sides at vanilla's scale. Plane
steps (`basexscale`), `yslope`, the light tables and the weapon scale use the non-widescreen width. The
smaller view sizes keep vanilla's proportions of the screen. 2D art keeps its 320x200 layout:
`V_DrawLayoutPatch` adds `WIDESCREENDELTA` and `HEIGHTDELTA` (38 and 12, both 0 in the 320x200 build) for
menus, intermission, finale and title pages; `V_DrawFullscreenPatch` centres full-screen pictures and
clears the borders; the status bar is at the bottom (`ST_Y`), its 320 columns centred (`ST_LX`, `ST_LY`)
with the view border's flat and bottom edge in the side strips; HUD text and the automap use screen
coordinates; the bunny scroll shows 396 of the 640 panorama columns.

**Extras.** `CGD_EXTRAS` compiles the rendering and HUD features below, taken from Crispy Doom. Each is
a bit of `CGD_Features` (`cgdoom.h`), all on in the HD add-in; the host's `-features` sets them by name.
None changes game state, so demos and savegames stay compatible.

| Bit | Name | What | Where |
|---|---|---|---|
| 0x01 | `aspect` | The 3D view scaled 6/5 vertically (`projectiony`), as 320x200 looked on a 4:3 monitor | `r_main.c`, `r_plane.c`, `r_things.c` |
| 0x02 | `colornums` | The ready ammo, health and armour numbers coloured by level: `V_DrawPatch` draws them through a translation (`v_patchmap`) that maps the digits' reds to the nearest palette entries of a green, gold, blue or gray (`v_crmap`, built once from PLAYPAL); the palette flashes tint them with the rest of the bar | `st_stuff.c`, `v_video.c` |
| 0x04 | `crosshair` | A yellow cross at the centre of the view | `r_main.c` |
| 0x08 | `automap` | The Map key's second press: the map over the view, turned with the player; kills, items, secrets and time | `am_map.c` |
| 0x10 | `fullhud` | At the largest view size: ammo, health, armour and keys over the view, with a black shadow (`v_blackmap`) | `st_stuff.c` |

The HD add-in also has quicksave and quickload (`M_QuickSave`, `M_QuickLoad`; TOOLS and SETTINGS in both
presets), which need two more keymap entries (`CGDOOM_KEYMAP_SIZE`).

**Memory.** The engine's five screens (0-3 full screens, 4 the status bar's 32 rows) come from the
768 KiB malloc reserve: 266,240 B at 320x200, 367,488 B at 396x224, which leaves the save buffer's
176 KiB (§2.4) room in both. The presenter's RGB565 frame, allocated at startup before the zones, grows
from 128,000 B to 177,408 B; with the larger `.bss` (+3,136 B), the HD add-in leaves about 51 KiB less
heap for the WAD and the zones. By calculation the packed shareware WAD still fits in RAM.

## 3. Builds

**Toolchain.** This needs cmake, libpng, libusb, ncurses and Python 3 with Pillow.
`tools/build-toolchain.sh` was written for macOS with Homebrew (its library paths are Homebrew's; on Linux
it may need adjusting). It builds sh-elf binutils 2.42, stock GCC 16.2 (its SH libgcc already uses soft-fp,
which the fxSDK patches into 14.1), OpenLibm, fxlibc, the pinned CG100 gint fork,
the SH simulator, and the fxSDK into `toolchain/`, from clones in `ref/`.
`ref/`, `toolchain/`, `test-wads/` and the build directories are gitignored.

```sh
tools/build-toolchain.sh            # everything (or one step: fxsdk binutils gcc openlibm fxlibc sim gint)
tools/build-toolchain.sh sim        # build GDB's SH simulator before a standalone gint rebuild
```

The release builds use these revisions. The gint step fetches and exports the exact fork commit
in `tools/gint/source.env`, builds it, and runs its GCC 16 startup regressions before installing.
The default build includes the simulator; a standalone gint rebuild requires it to be installed first.
No source patch is applied, and the upstream clone in `ref/gint` is left unchanged.
For fxsdk and fxlibc the script checks out `dev`; check out the listed commits to reproduce a release build exactly:

| Path | Repository | Commit |
|---|---|---|
| `ref/fxsdk` | `git.planet-casio.com/Lephenixnoir/fxsdk` (dev) | `a6e46eb6df060dc4e6e931521205e516c92b6c7f` |
| `ref/gint-cg100` | `github.com/cheparukhin/gint` (CG100 fork) | `4562f2a76d1fd7556d3fa37bba1f05aa3670d751` |
| `ref/fxlibc` | `git.planet-casio.com/Vhex-Kernel-Core/fxlibc` (dev) | `a6903066cf4777aa96556a6da9699d4704867185` |
| `ref/OpenLibm` | `git.planet-casio.com/Lephenixnoir/OpenLibm` | `9828d2e3f265a882af6a5bba8ffe6a00c3eeb8d7` |
| `ref/sh-elf-binutils` | `git.planet-casio.com/Lephenixnoir/sh-elf-binutils` | `a963554d12c0236ee10c7545daa4c422bf107de6` |
| `ref/CGDoom` | `git.planet-casio.com/Lephenixnoir/CGDoom` (upstream engine) | `8516c16d9d3c65b45882433b74abdef171265c99` |
| MPM (built by casio-mpm-220) | `git.planet-casio.com/PlaneteCasio/mpm` (`mpm.bin` 1.1) | `cbd1a414d96b55e14adc20a88e2140f2c8723b5a` |

### OS compatibility

One `CGDoom.g3a` runs on OS 02.00 and OS 02.20; there is no OS-specific build. gint picks its OS-call
table at startup, and `sys.c` then accepts only the exact internal versions below. Stock gint has no
OS 02.20 table, so `tools/build-toolchain.sh gint` uses the maintained
[CG100 fork](https://github.com/cheparukhin/gint), pinned in `tools/gint/source.env`. The fork:

- adds a fourth column to gint's OS-call table, with the OS 02.20 addresses (API 3);
- selects API 2 and API 3 only on the exact strings `02.00.0000` and `02.20.0000` (stock gint accepts any
  `02.00` prefix);
- skips gint's LCD window save and restore during world switches on the new display controllers (IDs
  `0x32` and `0x52`). gint wrote the old controller's registers 0x210-0x213, which are other commands on
  the new one. OS 02.20 sets its own window before every screen update, so nothing needs restoring;
- compiles gint's startup relocation of permanently mapped code (`start.c`) for the fx-9860G only, as its
  comment intends. `fxcg50.ld` sets `_lreloc` and `_sreloc` to 0, and GCC 16 assumes a 4-aligned object
  is at address 4 or above, so it ran that loop 2^30 times from address 0: a GCC 16 build stopped
  at launch with System ERROR, TLB ERROR, `TARGET=00001000`.

The fork also materializes numeric linker values before startup loops and boundary calculations.
Its 96 target startup cases cover empty/populated sections and tables, FX/CG/CP paths,
optimization levels, and LTO. See its [audit](https://github.com/cheparukhin/gint/blob/main/docs/linker-symbol-audit.md).

CMake requires `GINT_CG100_OS_COMPAT`, and the link requires
`gint_cg100_get_CASIOWIN_API()` from the same CG100 runtime. After
updating an existing toolchain, run `tools/build-toolchain.sh gint` once.

| User-visible OS | Internal string | CGDoom | MPM |
|---|---|---|---|
| 02.20.0202 | `02.20.0000` | Accepted, API 3 | casio-mpm-220's patch and `mpm.bin`; the setup the user guides describe |
| 02.00.0202 / 02.00.2202 | `02.00.0000` | Accepted, API 2 | The original MPM installer and `mpm.bin` 1.1; not covered by the user guides |
| 02.10, 02.00.xx1x and others | other strings | Refused | casio-mpm-220 updates them to 02.20 |

The OS 02.20 addresses come from the stock image with SHA-256
`1413bb39864b9590c80c3ba761b7a1791d5701b9f4fb3fc25c6d7bd9e89d95e7`. They were checked against it: the
code at each address matches the OS 02.00 function (64 bytes, PC-relative fields masked), and the BFile
wrappers that look alike call inner functions that all moved by the same offset. MPM itself (the OS
patch, the `mpm.bin` builds and the firmware research) lives in casio-mpm-220, which is private.

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
fxsdk build-cg          # calculator: CGDoom.g3a + CGDoomHD.g3a, build-cg/cgdoom{,-hd}.{bin,map} (warning-free)
fxsdk build-cg -c -DCGDOOM_WIDE_G3A=ON   # once: also CGDoomW.g3a (396x224 8-bit, no extras)
make host               # build-host/cgdoom-host (clang -O2 -g, RANGECHECK on)
make host-asan          # build-host-asan/cgdoom-host (ASan + UBSan)
make VARIANT=hd host    # build-host-hd/cgdoom-host (VARIANT=wide|hd: every host and test target, §2.7)
make sim-tool           # once: build-sim/tools/sh-elf-run64 (64-bit counters, PC profiles)
make sim                # build-sim/cgdoom-sim.elf (SH4 drawers); a C-only ELF: make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim
```

**Host CLI.** Run `build-host/cgdoom-host` without arguments for the full list. Common options:

- `-iwad F`, `-warp E M`, `-skill N`, `-tics N`;
- `-playdemo LUMP`, `-record N`, `-timedemo`;
- `-input builtin:<name>|<file>`;
- `-crc FILE`, `-shot PREFIX`;
- `-lowdetail`, `-viewsize N`, `-demos` (title demos, which the calculator always plays);
- `-features all|none|LIST`: the HD build's extras (`CGD_Features`, by name, §2.7);
- `-autoframeskip N`, `-rendercost T`, `-ticcost T` (deterministic pacing: a frame or a tic advances the
  virtual clock by T tics);
- `-nodelaysaves`, `-zone KiB,...`, `-fragment N`, `-poison B`, `-rlimits small`, `-slowwad`, `-gamelog`,
  `-stats`;
- `-demotrace FILE`: one line of game state per tic, in the format of the linuxdoom-1.10 oracle
  (`tools/demosync-oracle/`), to compare demo sync.
- `-savelayout`: the savegame record layout of the build (struct sizes and the offsets of index fields), which
  `tests/savetool.py` needs to patch savegames.

The host is deterministic: by default every presented frame advances the virtual clock by one tic, so
CRCs are reproducible.

**Dev loop on hardware:** press **SETTINGS** in the MPM menu, then run `fxlink -pw build-cg/cgdoom.bin`.
Push the header-less `.bin`; MPM maps it at 0x00300000.

## 4. Tests

Hardware runs are manual, so automated correctness rests on three things:

- **Golden frames:** the host renders fixed scenarios, and every frame's CRC must match.
- **Bit-exact simulation:** the SH build in the GDB simulator must reproduce the host CRCs.
- **Host unit tests of the calculator platform's pure logic.**

| Command | What it checks |
|---|---|
| `make test-host` | The 36 scenarios of `tests/scenarios.txt` against `tests/golden/*.crc`. These cover Freedoom E1–E3 maps, Doom II maps, the title, demos, finales, renderer limits, the optional modes, and the menus and help screens over a level. Variant lines (`golden=<other>`) must reproduce another scenario's CRCs exactly: the WAD copy path (`-fragment`), small or multiple zones, poisoned zone memory, renderer arrays that start tiny (`-rlimits small`), and the slow-WAD precache path (`-slowwad`). |
| `make VARIANT=wide test-host`, `make VARIANT=hd test-host` | The same scenarios in the widescreen and HD builds against `tests/golden-wide` and `tests/golden-hd` (the HD build with all its extras on) |
| `make test-quicksave` | `tests/quicksave.sh`, HD build: the quicksave and quickload actions (bound in a `CGDOOM.CFG`, which the host reads from `-data`): slot pick, confirmed overwrite, confirmed load, cancelled load; then 40 save/load rounds over two level exits in a zone too small for the old in-zone save buffer |
| `make test-variants` | Both variants' scenarios against their goldens, also under ASan/UBSan, plus `test-quicksave` |
| `make test-asan` | The same 36 scenarios under ASan/UBSan, plus the save-entry, unbegun-demo, map-limit, robustness, title-keys, skill-load and timedemo tests |
| `make test-unit` | `test-fixed` (FixedMul/FixedDiv vs vanilla), `test-div`, `test-names` (8.3 mapping), `test-roundtrip` (record and replay a demo bit-exactly, also with the Doom menu opened while recording), `test-settings` (CGDOOM.CFG), `test-saveentry`, `test-demo-unbegun`, `test-map-limit`, `test-robust`, `test-title-keys`, `test-skill-load`, `test-timedemo` |
| `make test-fixed-sim`, `make test-div-sim` | The fixed-point and division tests in the simulator, where `FixedDiv` is inline asm |
| `make test-title-keys` | Every special key (gamma, cheat, noclip, FPS, view size) pressed on the title screen before any level: no access through the missing player (ASan/UBSan in `test-asan`). Then, with title demos on, a key pressed during a demo opens the Doom menu, as in vanilla |
| `make test-skill-load` | Fast monsters follow the skill of a loaded save: a fresh session loading a Nightmare save, a Nightmare game loading a normal save, and the reverse (`-gamelog` `skill=` / `sargtics=`) |
| `make test-timedemo` | `-timedemo` reports every demo of `freedoom1.wad` with one frame per tic of the demo. Also part of `test-unit`; `test-asan` runs it under ASan. |
| `make test-map-limit` | `-warp 33` on `freedoom2.wad` starts MAP32: `G_InitNew` limits a Doom II map to 32, since the engine's HUD titles, par times and intermission names end there. Also part of `test-unit`; `test-asan` runs it under ASan. |
| `make test-robust` | `tests/save-demo-robustness.sh`: damaged, foreign and oversized savegames and damaged demos fail cleanly, with no access outside a buffer under ASan (`test-asan`). Loads of a savegame patched by `tests/savetool.py` (which reads the record layout from `cgdoom-host -savelayout`): a bad header is refused before the current game is touched; a truncation, an index out of range or another level layout is a "Bad savegame" error. A save in a generated copy of `freedoom2.wad` whose MAP02 has 3000 extra things is refused ("level too large") and keeps the slot's previous save. Demos that are too short, have a bad console player or player set, or lack their end marker (played like the marker), from a file, a lump and the title loop. |
| `make check-sh` | Every engine file compiles with sh-elf-gcc, without warnings (the test builds' set, below) |
| `make test-demosync` | Optional. DEMO1-3 of the shareware DOOM v1.9 `doom1.wad` named by `CGD_DOOM1_WAD` must play in sync: the per-tic game-state traces are compared with `tests/demosync/*.ckpt`, which were checked tic by tic against linuxdoom-1.10. Skipped when the variable is not set. `make test-demosync-sim` runs it in the simulator. |
| `make -C src/engine/sh4/test run` | Asm vs the C reference: randomized and bit-exact, with stray-write, stack-canary and calling-convention checks; `asmcheck` |
| `sh tests/gint-host/run.sh` | 11 suites of src/gint logic with stub gint headers, built with ASan/UBSan (`SANITIZE=0` to disable); a suite that fails to build counts as failed: sys (model/OS/API gate, table reset and open wrapper), expand, files (delayed writes, rollback of failed replacements, RAM image, packed images, refusal counting), hints, input, keymaps, memory (OS heap arena, zones), names, time (idle and chord policy), tlb (pin/repair/unpin planning), wadmap (block map, verification). The platform sources have `CGD_HOST_TEST` seams (OS string in `sys.c`, image size in `memory.c`, KIUDATA in `input.c`); some suites `#include` the `.c` under test, and `test_files` interposes `malloc`. |
| `python3 -m unittest discover -s tests -p 'test_gint_runtime.py' -v` | After rebuilding gint: exercises the fork's actual OS selection and old/new LCD world-switch callbacks on the host |
| `make test-sim` | The first 60 tics of e1m1, map01 and demo1 in the simulator vs the goldens (about 6 s) |
| `make test-sim-full` | All 36 scenarios in the simulator vs the goldens (about 4 min) |
| `fxsdk build-cg` | The calculator build, warning-free; `uram_check.ld` link guard |

**Warnings.** Every build uses one warning set, `warnings.mk`'s `CGD_WARN`: `-Wall -Wextra` plus `-Wshadow`,
`-Wstrict-prototypes`, `-Wimplicit-fallthrough` (an intended fall-through is marked `FALLTHROUGH`,
`doomdef.h`), `-Wformat=2`, `-Wvla`, `-Wundef`, `-Wpointer-arith` and `-Wdouble-promotion`. The test builds
(host, ASan, unit tests, `check-sh`, simulator, the SH4 test harness, the gint-host tests) treat them as
errors. The calculator build (`CMakeLists.txt` reads the set from `warnings.mk`) does not, so that a newer
compiler's new warning does not stop it; `check-sh` compiles the same engine with the same compiler and the
set as errors.

Other targets:

- `make perf-sim`: the per-frame cost tables and profiles (`build-sim/perf/`).
- `make golden`: regenerates the goldens. Only do this for an intentional output change; the commit message
  names the goldens that changed and why.
- `make -C src/engine/sh4/test bench|pipe|sched`: the asm design tools.

**Demo sync.** Any change to game logic must keep `make test-demosync` passing. The trace format is defined
twice and must stay identical: `demotrace_tic()` in `src/host/host_main.c` and `ORACLE_TraceTic()` in
`tools/demosync-oracle/oracle_sys.c`. If a trace has to change, rebuild the oracle
(`tools/demosync-oracle/build.sh`), compare tic by tic, and only then rewrite the `.ckpt` files with
`tests/demo-sync.sh -update`.

The golden scenarios need `test-wads/fd/freedoom-0.13.0/freedoom1.wad` and `freedoom2.wad`.
`tests/gint-host/run.sh` runs its WAD tests on the WADs listed in `CGD_TEST_WADS` (default: the Freedoom WADs under `test-wads/`).

**What these tests cannot cover:**

- gint calls and OS calls;
- timing, DMA and the LCD;
- the UTLB on real hardware;
- flash layout;
- the overclock;
- key legends;
- wild pointers. On hardware a NULL or stray read does not fault (page 0 is mapped onto flash), which is
  why the host builds keep `RANGECHECK` and run under ASan/UBSan.

[§7](#7-hardware-verification) lists them.

## 5. Performance

Details: [`tests/perf-baseline.md`](../tests/perf-baseline.md). These are simulator instructions per frame,
from frame 42 to the end, with view size 10 and full detail. They exclude the host's CRC work.

| Scenario (Freedoom) | CGDoom | Current | Change |
|---|---|---|---|
| e1m1 | 2,017,661 | 1,031,352 | −48.9% |
| map01 | 1,715,048 | 908,500 | −47.0% |
| demo1 | 4,044,224 | 1,602,103 | −60.4% |
| demo2-1 | 2,868,025 | 1,562,152 | −45.5% |

The engine now follows vanilla's arithmetic and game logic, so the demos show different scenes than in
CGDoom; e1m1 and map01 are the closest comparisons.

- **Optional modes**, compared with the default:
  - low detail: −33 to −39%;
  - view size 9: −13 to −17%;
  - view size 7: −33 to −40%.
- **Screen formats** (§2.7), against the 320x200 build ([perf-baseline](../tests/perf-baseline.md#screen-formats)):
  - 396x224: +19 to +32%, for 41% more pixels;
  - HD: +23 to +30% in the demos and the bot and walk scenarios, about the same as 396x224: its extras cost
    nothing measurable (−1.7 to −0.1% of the 396x224 frame). The automap overlay costs more while it is open (it
    keeps the 3D view and turns the map lines it draws).
- **The simulator charges 1 cycle per instruction (plus 2 per taken branch)** and models no cache or dual
  issue, so the SH4 drawers' gain is a lower bound. Under the SH-4A pipeline model
  (`src/engine/sh4/README.md` §3), the drawers run 2.4–3.2× faster than GCC's C drawers over a frame's call mix
  (`tests/perf-baseline.md`); column drawing costs
  about 4.0 cycles/pixel, against about 14 for GCC's loop.
- The DMA transfer of a frame is bus-bound, about 8 ms by estimate, and overlaps the next frame.

### 5.1 Where the speed comes from

All of these are bit-exact: no golden changed, except for the fixed-point change, which made the engine
vanilla's. The effects are simulator instructions per frame, measured when each change went in, against
the engine just before it.

| Change | Where | Effect |
|---|---|---|
| **Vanilla-exact fixed point.** `FixedMul` is a header inline (`dmuls.l`, about 4 instructions, against 196 for CGDoom's bit-serial loop). `FixedDiv` is a `div0u`/`div1` sequence (93 instructions and no libgcc call, against about 202 instructions and 16 libgcc divisions). `finecosine` and `SlopeDiv` are inline | `m_fixed.h`, `m_fixed.c`, `tables.h` | −21% e1m1 and map01, −39% demo1 (`FixedMul` was 17–24% of cycles) |
| **Division helpers.** `IntDivPos` runs a signed division with a positive divisor through the faster unsigned divider (`R_StoreWallRange`). `IntCycleFill` does one division per animation instead of two per animated picture (`P_UpdateSpecials`) | `m_fixed.h` | −0.2% |
| **Per-column glue** (the next four rows together) | | −3.5% |
| `R_GetColumn` is a header inline. When the lump is mapped or already cached with the right tag, `W_CacheLumpNumConstFast` returns the pointer without the `W_CacheLumpNumConst` → `Z_ChangeTag2` → `PL_IsWadPointer` chain | `r_data.h`, `w_wad.h` | about 70k instructions per frame in e1m1 |
| The WAD byte readers are inline, and patch column offsets are read with one aligned 32-bit load when the patch is aligned | `m_swap.h`, `r_things.c`, `v_video.c` | |
| Short row copies use words instead of fxlibc's byte-by-byte `memcpy` below 64 bytes (status-bar widgets, sprite clips) | `v_video.c`, `r_segs.c` | |
| The keymap decode is skipped when the key scan has not changed | `cgdoom-kbd.c` | about 10k |
| **`R_RenderSegLoop` loop state in locals.** GCC no longer reloads and stores the globals around every column call | `r_segs.c` | −1.4% |
| **Visplane hashing** (64 buckets) | `r_plane.c` | `R_FindPlane` from 17k instructions per frame to almost nothing in demo1; ±0.2% overall; the gain grows with the plane count |
| **SH4 drawers** (columns, spans, low detail, fuzz, translated, palette expansion) | `src/engine/sh4` | −15% (demo1) to −29% against the C drawers |
| **Empty wall columns skipped.** A single-sided wall no longer calls `R_GetColumn` and the drawer when `yh < yl` (26% of the column calls in e1m1). Lighting and the `dc_iscale` division are computed only before the first tier a column draws | `r_segs.c` | −0.8% (map01) to −5.8% (e1m1); libgcc division −34% in e1m1 |
| **`R_DrawMaskedColumn` loop state in locals.** Only drawn posts store the `dc_*` globals | `r_things.c` | −21% of the function in demo1 |
| **No repeated sight check.** In single player, vanilla's `P_LookForPlayers` loop comes back to player 0 and checks sight again when the first look fails (about 45% of the `P_CheckSight` calls in id's demos). A player already looked at in the call is skipped; the loop variables and the result advance exactly as in vanilla | `p_enemy.c` | −0.6% in demo1 |
| **Empty plane columns skipped.** `R_DrawPlanes` calls `R_MakeSpans` only for a column whose top or bottom differs from the previous column's; for the others, most of a plane's columns, its four loops were all empty | `r_plane.c` | −3.0% (e1m1) to −5.4% (map01) |
| **Unchanged status-bar numbers not redrawn.** Vanilla redrew all 11 numbers of the bar, digit by digit over a copy of their background, each frame. Like the icons, they are now drawn only when they change and when the whole bar is refreshed: nothing else draws over the bar during play. While a help screen is up, and in the frame after it, before the bar's refresh, they are drawn every frame as before | `st_lib.c` | −1.5% (demo2-1) to −3.3% (map01) |
| **Texture column computed only when used.** `R_RenderSegLoop` computes it before the first tier a column draws, or for a masked texture, instead of for every column | `r_segs.c` | −0.4% (map01) to −1.1% (e1m1) |
| **The automap overlay rotates only the lines it draws** (HD build). The colour of a line is chosen first; vanilla's order would rotate every line of the level, mapped or not | `am_map.c` | −4.4% (e2m2) to −9.9% (e1m2) in the noclip scenarios, whose overlay is on |

Tried and left out:

- a `div1` sequence for the `dc_iscale` and `SlopeDiv` divisions: 2 instructions per quotient bit, no
  faster than libgcc. An exact reciprocal is still open ([perf-baseline](../tests/perf-baseline.md));
- an inexact reciprocal: it changes pixels;
- inlining `R_PointOnSide`: about 1.5k instructions per frame, below the noise;
- `colfunc` in a local of `R_RenderSegLoop`: the register pressure cost more than one global load per call;
- an early exit for negative counts in the asm drawers: no call site passes one any more, and it would
  lengthen every call's set-up;
- `-O3`: gpSP measured it slower than `-O2` on fx-CG100 hardware (I-cache).

### 5.2 Not yet measured on hardware

- the real frame rate (the game holds its 35 fps cap in the places played so far, but no figures were
  recorded);
- the cache-miss costs;
- flash vs SDRAM line-fill costs (the reason the RAM image is the primary path);
- the presenter costs at each clock.

Diagnostics → Benchmarks measures the last two, and its timedemo (key **1**) the first: the WAD's demos
play uncapped at F4, every tic rendered, without wipes (`CGD_TimeDemo`, `cgdoom.h`; the host's
`-timedemo` runs the same code).

## 6. Design decisions

| Decision | Why | Where |
|---|---|---|
| Portable engine behind `PL_*`; host and sim platforms | Golden-frame testing and bit-exact asm verification without hardware | `platform.h`, §2.1 |
| Copy the WAD to RAM (minus sounds), with the block map as fallback | NOR line fills are much slower than SDRAM, and the overclock does not speed them up ([PLATFORM.md](PLATFORM.md)) | `wad.c` |
| All storage writes deferred to exit; `PL_WadClose` first | A write moves flash records and invalidates the block map | `cgdoom.h`, `files.c` |
| A file of the same size is overwritten in place, a file of another size is replaced by remove + create + write. Either way the old file is kept in RAM first and written back if the new one does not fit | create + rename would need room for both copies when storage is nearly full, and `BFile_Rename` would be one more OS entry point never tested on hardware. Without memory for the backup, the replacement still goes ahead, so a failure then loses the old file. There is no safe free-space call, so failures are reported at exit rather than prevented | `files.c` (`os_flush`) |
| Block map checked against the file at open | A wrong pointer would make the game read garbage; on a mismatch, CGDoom uses BFile reads instead | `wad.c` (`map_verify`) |
| Model and full OS-string check before any OS call; anything but the fx-CG100 with exact `02.00.0000` or `02.20.0000` quits | An unknown OS table would reach the dead fx-CG50 gate; the g3a also starts on the fx-CG50 | `sys.c` |
| mpm.bin 1.1 only; `_os` removed | 1.1 announces 5.5 MiB to gint; older builds hid RAM that needed per-build discovery. Out-of-memory must return NULL, not call the OS | `memory.c`, `no_osheap.c` |
| UTLB pinning + repair + exit restore; safe start | The OS can evict code pages during world switches; stale entries can reset the next launch | §2.5, `tlb.c` |
| Raw KEYSC only; matrix codes; weak hooks for menu, Enter and ALPHA keys | gint remaps fx-CG100 rows 5–9; CGDoom keymaps are matrix codes | `input.c`, `cgdoom-kbd.c` |
| BACK = Doom menu, OK = second Enter | 0x84 (CGDoom's MENU) is OK on the fx-CG100, the launcher's select key | `keymaps.c` |
| Overclock F4 for every game, no option | F4 is gint's supported preset and the game plays normally with it; Ptune4 F5's 118 MHz bus fails on some units | `clock.c` |
| Async DMA presenter only (CPU writes as the fallback) | Overlaps the bus-bound transfer with rendering, and it works on the new display | `video.c` |
| Hot drawers in ILRAM | On-chip memory is re-initialized by world switches (code is fine, data is not); gint backs it up | `CMakeLists.txt`, `main.c` |
| File work batched into as few world switches as possible, none during play | A world switch is slow, re-initializes on-chip memory, reprograms the clock and can lose an RTC tick | `files.c`, `main.c` |
| 128 KiB private game stack | The OS stack's size and location are unknown | `main.c`, `stack.S` |
| One exit path; the quit chord is latched by the tic interrupt; `abort()` only for a hang; one destructor | Saves survive every exit but a hang; the UTLB is restored even after `abort()` | `main.c`, `time.c` |
| Idle exit (10 min, or 30 min in a user game) | The add-in owns the interrupts, so the OS auto power-off cannot run | `main.c`, `time.c` |
| Auto frameskip (max 3), no option | Game speed stays at 35 Hz; frames are dropped instead | `i_system.c` |
| Launcher: the WADs, Controls and Diagnostics only; vanilla's title loop with its demos | Every other setting has one right value on the fx-CG100, or is chosen in the game (skill, episode, view size, detail) | `main.c` |
| One game per launch | The engine's static state is not re-entrant | `main.c` |
| Strict 8.3 names | The calculator's Memory app shows longer names truncated (`~1`) and cuts extensions to 3 characters | `cgdoom-names.c` |
| Widescreen renders 396x224 natively, "Hor+" as Crispy Doom | Stretching 320x200 to the panel doubles every 4th column and 9th row; a native render shows more to the sides at vanilla's scale | `r_main.c`, §2.7 |
| The extras exist only in the HD add-in, as run-time bits | `CGDoom.g3a` stays the build tested on hardware; each extra can be turned off and tested alone (`-features`) | `cgdoom.h` |

## 7. Hardware verification

### 7.1 What only hardware can answer

The probes are on the Diagnostics screen (launcher → *Diagnostics...*). Results are shown on screen only;
there is no fxlink text channel.

| Question | Probe | Where it is shown |
|---|---|---|
| Model, OS | Model string, OS string | System, line 1 |
| Which LCD controller | `r61524_get_version()` | System: "LCD controller id" |
| Where the stack is | r15 at entry | System: "r15 at entry" |
| Which mpm.bin | `gint_load_info`, phys(0x00300000), phys(0x08100000), image size; the `_ld1` arena | System: loadinfo / phys / Image lines; Memory |
| Stock clock vs F1 | FRQCR, FLLFRQ, `clock_get_speed()`, CS0WCR/CS3WCR/CS3BCR, Iφ/Bφ/Pφ | System (read at the current clock; the launcher runs at the OS clock) |
| Heap layout | Arena table: name, start, end, free, largest; OS heap removed; zones and reserve once a game has run | Memory |
| Flash layout and block-map pointers | Blocks, mapped, P1/P2/P0/other/error/unaligned counts, contiguous runs, longest run, physical range, "map did not match" | Storage (probe a WAD with OK/EXE) |
| Does the OS reload TLB entries during world switches | UTLB slots changed by the probe's world switches; URB before and after; repairs and misses | Storage after a probe; System UTLB lines |
| UTLB pinning worked | "UTLB pinned N pages: URB x (was 54)", or "executor off", or "SAFE START" | System |
| Flash vs SDRAM speed | 64 KiB cold read, flash P1 vs SDRAM P1 | Benchmarks (OK/EXE) |
| Presenter cost | 320x200 push with the game's path (DMA, or CPU without the frame buffer), at the OS clock and at F4 | Benchmarks |
| Real frame rate | Timedemo: the WAD's demos uncapped at F4, frames and time of each | Benchmarks, key 1 (then a results screen) |
| Tic timer obtained | TMU number, or "polled clock" | System, last line |
| `BFile_Read` return convention | Both conventions are accepted (`gfs_read_ok`); a failure shows as an open error | Launcher error / Log |
| Partial-window DMA | The picture is correct | In game |
| Exit leaves a clean UTLB | Another add-in launches normally after CGDoom | MPM |

Not implemented: an SH-4A performance-counter check and cache-size stride sweeps. The other open
assumptions (cache geometry, flash timing, the stock clock) are marked [G] in [PLATFORM.md](PLATFORM.md).

**Results so far:** on an fx-CG100 with the new display and OS 02.20, CGDoom starts, loads
DOOM1.WAD, plays normally and exits to the MPM menu. The probes above have not been recorded yet, and the
title-screen demos, the old display, OS 02.00, saves written and reloaded, and relaunch are untried.

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
   - "speed F…" at the OS clock.
7. Memory page: record the arena table and the total free KiB (about 5.5 MiB expected by calculation;
   under 3 MiB CGDoom would have refused to start). Expected: "OS heap arena: removed".
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
    on exit". Load it back in the same session. Then save to slot 1 and type a name that uses the letters
    D, E and G–L (their legends on the fx-CG100 are not confirmed; see the
    [user guide](USER-GUIDE.md#typing-a-save-name)). Record which letters appear.
14. Play for at least 10 minutes over E1M1–E1M3, including a level change.
15. Quit from the Doom menu (*Quit Game*, then EXE).
    - Expected: "Writing N files...", then a return to MPM without an error.
16. Over USB, check that `DOOM1_0.DSG` and `CGDOOM.CFG` exist. Check that the calculator clock is still
    right (each world switch can lose an RTC tick).

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

Use these results to update [§7.1](#71-what-only-hardware-can-answer), the [G] items in
[PLATFORM.md](PLATFORM.md), and the user guide's performance section.

### 7.3 CGDoom HD

`CGDoomHD.g3a` has not run on hardware. With `CGDoom.g3a` installed and tested as above, copy it next to
it (storage permitting), then:

1. Diagnostics → Benchmarks in each add-in: record both blocks (the HD push is 396x224) and the timedemo
   results (key **1**; the result screen also names the WAD mode, which should be "RAM image" in both).
2. In a game: the picture fills the screen with no border, tearing or garbage; menus, help screens, the
   intermission and the E1 text screen are centred.
3. The extras: coloured status-bar numbers, the crosshair, the full-screen HUD at the largest view size,
   the automap overlay (the Map key twice) and its statistics.
4. Quicksave (TOOLS) to a new slot, play on, quickload (SETTINGS), quit: the slot's file is written, and
   loads in `CGDoom.g3a` too.

## 8. Known engine limits

These follow vanilla or cannot happen with the shareware WAD, so they are left as they are:

- **Multiplayer:** only player 0 is supported; co-op demos do not play correctly, and *New Game* is refused
  while one plays.
- **Vanilla overflows** (`spechit` beyond 8 entries, intercepts, REJECT) run the same code, but the memory an
  overflow clobbers differs from linuxdoom's.
- **The lost-soul floor bounce** follows linuxdoom-1.10 (the Ultimate/Final Doom order). The registered Doom
  and Doom II v1.9 executables bounce differently, so Doom II demos with close lost-soul fights can desync.
  doom1.wad has no lost souls.
- **Repeated `-fast` demos** accumulate vanilla's halving of the demon run states; no IWAD demo sets it.
- **Savegames:** the 176 KiB buffer is too small at the start of Freedoom E3M3 and E4M2 and Doom II MAP12;
  those saves are refused ("level too large"). `mobj->target` is NULL after a load (vanilla), so a Doom II
  boss cube in flight can crash after loading. Stopped perpetual platforms and fire-flicker lights are not
  archived (vanilla; no shareware map has them). A save made by the 64-bit host does not load on the
  calculator.
- **Texture patch indices** are not range-checked (vanilla); that needs a corrupted texture table.
- **Differences that do not touch game state:** `R_PointToDist`'s slope clamp, the switch list built from
  every episode's textures, the keyboard `angleturn` table and the medikit message (nDoom's fix).
