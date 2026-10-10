# CGDoom for fx-CG100 — design

Port of CGDoom (fx-CG50 DOOM, GPLv2) to the Casio fx-CG100 / Graph Math+ running add-ins through
MPM + gint (dev). Same `.g3a` should also run on fx-CG50 with gint (bonus, not a hard requirement).
Research notes this design is based on: `notes/research-*.md` (read the relevant one before coding).

## 1. Repository layout (all paths relative to /Users/cheparukhin/casio_doom)

```
CMakeLists.txt            fxSDK project: `fxsdk build-cg` -> CGDoom.g3a (calculator)
Makefile                  host + sim builds and tests (`make host`, `make sim`, `make test`)
src/engine/               portable DOOM engine (from CGDoom cgdoom/), includes only "platform.h"
src/engine/sh4/           SH4 assembly (renderer inner loops etc.), used by calculator and sim builds
src/platform/platform.h   the PL_* platform API (single source of truth, see §3)
src/gint/                 fx-CG100/CG50 platform: main+launcher UI, video, input, time, files, WAD map, memory
src/host/                 host platform (clang/macOS) — ALSO compiled for the SH simulator with -DPL_SIM
src/sim/                  sh-elf-run harness: crt0.s, sim.ld, syscalls.c (trapa #34), libc glue
tools/                    build-toolchain.sh, wadstrip.py, run-tests.sh, ...
tests/                    golden frame CRCs, scripts
assets/                   g3a icons (92x64 PNG, uns/sel)
ref/                      upstream clones (read-only, never modified, not built)
toolchain/                built SH toolchain (sh-elf-gcc 14.1, gint dev, fxlibc dev, sh-elf-run)
test-wads/fd/freedoom-0.13.0/freedoom{1,2}.wad   test IWADs (BSD license). No id WADs available locally.
```

Toolchain: `export PATH=/Users/cheparukhin/casio_doom/toolchain/bin:$PATH` gives `sh-elf-gcc`, `fxsdk`,
`fxgxa`, `fxconv`, `sh-elf-run`. `fxsdk build-cg` works (tested). `fxsdk new` does NOT (BSD getopt); don't use it.
Host compiler: Apple clang (C11). Note: macOS filesystem is case-insensitive (avoid a binary named like a dir).

## 2. Target facts that drive the design (see research notes for sources)

- CPU SH7305 (SH-4A, no FPU used), 118 MHz default; gint `CLOCK_SPEED_F4` = 236 MHz CPU / 59 MHz bus (safe,
  gint-supported on CG100). "Ptune4 F5" (bus 118 MHz) is opt-in only. 32-byte cache lines. Misaligned 16/32-bit
  loads FAULT on hardware.
- 8 MiB RAM. Under MPM the add-in code+rodata run FROM RAM (copied, mapped at 0x00300000); static RAM 512 KiB at
  0x08100000 (`_uram` arena). Heap: arena `_ld1` = [0x8c200000, load address) ≈ 5.3–5.6 MiB with MPM 1.1, only
  2 MiB with the public Feb-2025 mpm.bin. NEVER hardcode RAM addresses; take memory from gint arenas.
- Storage ≈ 4.5 MiB total → only shareware-sized WADs fit (doom1.wad 4.2 MB; sound-stripped ≈ 3.4 MB).
- Display: R61524-compatible 396x224 RGB565; gint `r61524_start_frame(x0,x1,y0,y1)` + `dma_transfer_async(0, DMA_32B,
  …, 0xb4000000, DMA_FIXED)` is proven on CG100 (gpSP). Bus-bound: ~8 ms for 320x200.
- No syscall table on CG100. gint dev knows OS 1.00 and 2.00 entry points only (2.10 falls back to the CG50 gate →
  crash). Bfile_GetBlockAddress is not wrapped by gint: OS1.00 0x8023ffb2, OS2.00 0x80333cf2, OS2.10 0x8039675a;
  CG50 syscall 0x1DAA.
- World switches (any BFile call) are expensive, re-init on-chip RAM (ILRAM/XYRAM) by default, restore the OS clock
  (RTC drift when overclocked) and may let the OS draw into LCD GRAM. Batch file work; none during gameplay.
- No return to OS menu on CG100: quit = return from main(). Wait for key release before returning.
- Keyboard: same KEYSC matrix as CG50; rows 7–9 (and some labels) differ: no F1–F6/MENU/OPTN/EXIT keys. Raw matrix
  codes (row<<4 | (7-col)), as used by CGDoom's KEYCODE_* constants, still identify physical keys.

## 3. Platform API (`src/platform/platform.h`)

The engine never includes gint/OS headers. Everything platform-specific goes through these functions
(implemented in src/gint/ and src/host/). All sizes/offsets in bytes.

```c
#include <stdint.h>
#include <stddef.h>

/* ---- Fatal errors / logging ---------------------------------------------- */
void PL_Fatal(const char *msg) __attribute__((noreturn));   /* show msg, wait key, clean exit */
void PL_Log(const char *fmt, ...);                         /* debug log (host: stderr; gint: no-op or fxlink) */

/* ---- Time ------------------------------------------------------------------ */
int      PL_GetTics(void);       /* 35 Hz monotonic counter since start (TICRATE) */
uint32_t PL_GetMicros(void);     /* free-running µs counter (wraps) for FPS/profiling */
void     PL_WaitTics(int until); /* sleep/idle until PL_GetTics() >= until (host: may return immediately) */

/* ---- Memory ----------------------------------------------------------------- */
/* Fill up to `max` zones for Z_Zone (largest first). Returns count. Each zone must be
   4-byte aligned, cached RAM. Platform keeps enough heap for itself and for malloc(). */
int PL_GetZones(void **base, int *size, int max);

/* ---- Video ------------------------------------------------------------------ */
/* The engine renders 320x200 8-bit into screens[0] (cached RAM, 32-byte aligned). */
void PL_SetPalette(const uint16_t pal565[256]);  /* gamma already applied */
void PL_FinishUpdate(const uint8_t *fb);         /* present fb (may return before the transfer ends) */
void PL_VideoReset(void);                         /* redraw borders etc. after errors / file I/O */

/* ---- Input ------------------------------------------------------------------ */
/* Physical keys are matrix codes (row<<4 | (7-col)), the KEYCODE_* constants of cgdoom-kbd.h. */
void PL_ScanKeyboard(uint8_t state[12]);         /* raw matrix state, CGDoom layout (byte per row) */

/* ---- WAD access -------------------------------------------------------------- */
int         PL_WadOpen(const char *name);         /* returns size in bytes, <0 on error */
void        PL_WadClose(void);
int         PL_WadRead(void *dst, int offset, int size);   /* returns bytes read; gathers fragments */
/* Zero-copy pointer to [offset, offset+size) if the range is contiguous in readable (cached) memory,
   else NULL. The engine only uses it for lumps; pointer stays valid until PL_WadClose. */
const void *PL_WadMap(int offset, int size);
int         PL_IsWadPointer(const void *p);       /* true if p was returned by PL_WadMap (never freed) */

/* ---- Small files (config, saves, demos) -------------------------------------- */
/* Names are plain ASCII file names in the storage root (e.g. "doom1.cfg", "doom1-0.dsg"). */
int  PL_FileSize(const char *name);                       /* <0 if missing */
int  PL_FileRead(const char *name, void *buf, int size);  /* bytes read, <0 on error */
int  PL_FileWrite(const char *name, const void *buf, int size); /* create or overwrite; <0 on error */
int  PL_FileDelete(const char *name);
```

Platform-specific *policy* (WAD selection launcher, options UI, delayed saves, keymap configuration, overclocking,
developer-info screens, profiler display) lives in the platform directory, not in the engine. The gint platform
re-implements CGDoom's `cgdoom.c`, `cgdoom-ui.c`, `cgdoom-kbd.c` features on top of gint. The engine keeps CGDoom's
glue hooks (`CGD_*` callbacks used by st_stuff.c/i_system.c such as CGD_Cheat, CGD_CycleFrameskip, CGD_FreeMem,
CGD_ProfilerResults, CGD_KeyWasJustPressed …): a portable default implementation lives in the engine
(`src/engine/cgdoom.c` trimmed) and only truly platform-dependent parts call PL_*.

Rules:
- Engine code must compile cleanly for both clang (host, 64-bit) and sh-elf-gcc (32-bit, big-endian).
- No unaligned multi-byte loads in engine code (SH4 faults). WAD data accessed bytewise or via aligned copies.
- `I_Error` → formats message → `PL_Fatal` (never returns).

## 4. Builds

1. **Calculator** (`fxsdk build-cg` with CMakeLists.txt): engine + src/engine/sh4 + src/gint, `-O2` default, selected
   hot files may get specific flags; link Gint::Gint; `generate_g3a(... NAME "CGDoom" ICONS ...)`. Output `CGDoom.g3a`.
   Constraint: g3a ≤ 3 MiB (public MPM) — expect ~350–450 KB.
2. **Host** (`make host` → `build-host/cgdoom-host`): engine (C renderer, no asm) + src/host. Headless. CLI:
   `-iwad <path> [-warp E M | -playdemo <lump>] [-skill N] [-tics N] [-input <script>] [-crc <file>] [-shot <prefix> -shotevery N] [-noasm]`.
   Deterministic: PL_GetTics advances by 1 per rendered frame (singletics), so runs are reproducible.
   Writes one CRC32 of screens[0] (and palette) per frame to the CRC file → golden-test material.
3. **Sim** (`make sim` → `build-sim/cgdoom-sim.elf`, run with `sh-elf-run`): engine + src/engine/sh4 + src/host compiled
   with sh-elf-gcc `-m4-nofpu -mb -O2 -DPL_SIM`, linked with src/sim (crt0, trap-#34 syscalls, fxlibc libc.a). Same CLI.
   Purpose: (a) bit-exact verification of SH4 asm vs the C renderer (CRCs must equal host CRCs);
   (b) instruction/cycle counts for optimization work (sh-elf-run reports them).

## 5. Test strategy (no hardware available)

- Golden tests: host runs fixed scenarios on freedoom1.wad (e.g. warp E1M1..E1M4 with a scripted input of N tics,
  plus WAD demos) and records per-frame CRCs (`tests/golden/*.crc`). Any optimization that is meant to be
  bit-exact must reproduce them on host AND sim. Intentional output changes require regenerating goldens with a note.
- Sim perf: run the same scenario in sh-elf-run, record instructions/cycles per frame → `tests/perf.md` table.
- Calculator-only code (src/gint) cannot be executed: it must compile warning-free, follow the research notes
  (gint API usage, CG100 pitfalls), and be reviewed adversarially.

## 6. Performance plan (details in notes/research-performance.md when ready)

Portable: inline FixedMul (dmuls.l), fast FixedDiv, inline SHORT/LONG, RANGECHECK off, raise renderer limits,
remove per-frame fragmented title redraw, cached zone/screens. SH4: asm R_DrawColumn/R_DrawSpan (+low-detail,
fuzz, translated variants) unrolled for dual issue; hot loops in ILRAM (GILRAM), colormap/tables in XYRAM (re-copied
after world switches); fast 8→16-bit palette expansion + async DMA overlapped with the next frame; overclock F4 by
default; auto frameskip.

## 7. Amendments after research critique (notes/research-critique.md) — these OVERRIDE earlier sections

- Heap: three mpm.bin variants (v1.1 ≈5.5 MiB `_ld1`; Feb-2025 public 2 MiB `_ld1` + unannounced 0x8C400000+img..0x8C700000;
  dynload: no `_ld1`, free 0x8C200000..phys(0x00300000)). gint platform does arena discovery; never ≥0x8C800000.
- OS: CG100 requires the exact string "02.00.0000" at 0x80020020 (only MPM-capable OS today); check before any OS call.
- gint restores the OS clock on every world switch and at exit — no manual restore.
- On-chip: X and Y are ONE 8 KiB bank each (aliases 0xE5007000=0xE500E000, 0xE5017000=0xE5010000). Reserved for
  presenter DMA strips. Nothing goes in GXRAM/GYRAM. ILRAM (~3.8 KiB free) = hot drawers (asm), placed with
  `.section .ilram,"ax"` / GILRAM. ILRAM/XYRAM are re-initialized after world switches (code fine; data must be rebuilt).
- No DSP repeat loops (ldrs/ldre/repeat) in code that may run in interrupt context (gint doesn't save RS/RE).
  Foreground drawers may use repeat loops ONLY if no interrupt-context code uses them.
- Keyboard: raw KEYSC read only; never gint keydown()/getkey() with CG50 codes on CG100.
- UTLB (fx-CG100/MPM only; src/gint/tlb.c, tlbh.S, tlbplan.c). MPM maps the image with 64 KiB UTLB entries at
  0x00300000 and sets URB=54, and the OS runs LDTLB during our world switches, so it can evict a page of our code
  (gint's own miss handler would then jump to the CG50 syscall gate and crash). Current design, four layers:
  1. Startup: snapshot the entries of the image pages; replace gint's TLB-miss handler at VBR+0x400 with one that
     reloads image pages from the snapshot (only reached while gint's VBR is installed, NOT in the OS-world window
     of a world switch, where the OS's VBR is active).
  2. Pinning (covers that window): move the image entries into the slots just below MMUCR.URB and lower URB under
     them, so no URC-based LDTLB (the OS's or ours) can replace them, in either world. Moves run from a P2 executor
     (cgd_tlb_apply) with interrupts masked.
  3. Repair after every world switch (gfs_os_call): verify the pinned slots and URB, rewrite what the OS changed.
  4. Exit restore (destructor gtlb_quit, also reached after abort()): every page back in MPM's original slot, other
     copies dropped, original URB restored — required because the Feb-2025 public mpm.bin does not flush the TLB
     between launches (a stale copy in 49..53 would reset the next launch). Without the P2 executor, URB is only
     lowered to the slots MPM rewrites, and kept there until exit.
  fx-CG50: none of this (gint's handler must stay: the OS demand-pages the add-in through it).
  Safe start (final review): DEL held when the add-in starts skips the P2 executor (no pinning, no ldtlb; the
  no-executor path: handler + contained URB), the overclock and the private stack for that session only.
- WAD-in-RAM is the primary path when heap ≥ WAD + ~1.5 MiB (NOR line fills ~5–7x slower than SDRAM; overclock doesn't
  speed flash). Flash block map is the fallback; map invalid after any storage write → defer all writes. Exit order
  (cgdoom.h contract, both platforms): PL_WadClose() THEN CGD_FlushDelayedWrites(). The block map is checked against
  the file at open (block 0 = header, sampled blocks = BFile reads) and dropped to BFile on a mismatch.
- fx-CG50 memory (final review): without experimental RAM (OS ≤ 3.60, option on) the zone is ~150 KiB, and doom1.wad
  needs ~400 KiB (title) to ~640 KiB (E1M6): the CG50 bonus target needs experimental RAM for doom1.wad. The launcher
  says so (zone estimate < 700 KiB). The experimental RAM is zeroed again at exit so the next launch finds it.
- Files created by the port: strict 8.3 names (CGDOOM.CFG, DOOM1_0.DSG ...). Bounce OS-call buffers to P1 heap.
- g3a limit: 2 MiB (gint linker script).
- Vanilla-exact FixedMul/FixedDiv (perf P1) intentionally changes pixels vs CGDoom → regenerate goldens once, with a note.
- Freedoom (28.8 MB) cannot go on the device; on-device testing needs the user's shareware doom1.wad (wadtool pack).
- Performance plan order (notes/research-performance.md §5, amended): P1 math → P4 C hot-path cleanup → P2 presenter
  → WAD/level residency in RAM → P3 asm drawers → P5 low detail/view size/potato → P8 flags → P7 ILRAM → P10 autoskip.

## 8. Cross-agent integration requirements (added after the gint platform landed) — engine agents MUST honor these

- **Static memory budget:** all .bss/.data lives in the 512 KiB `_uram` (gint linker script RAM limit 491k), shared with
  gint's 177 KB VRAM. Current bss ≈ 215 KB (visplanes 84 KB, openings 40 KB, viewangletox 16 KB, drawsegs 12 KB).
  When raising renderer limits (MAXVISPLANES/DRAWSEGS/VISSPRITES/OPENINGS), allocate the arrays DYNAMICALLY at init
  (malloc/Z_Malloc PU_STATIC) instead of growing static arrays; do not increase total .bss. Moving existing big
  static arrays to the heap is welcome.
- **Escape/menu key hook:** add `int CGD_EscapeKeycode = KEYCODE_MENU;` to cgdoom-kbd.c (declared in cgdoom-kbd.h, type
  `int`, gint references it as a weak `extern int`); `DoomKeyToKeycode()` must return it for KEY_ESCAPE. (On CG100 the
  0x84 matrix position is the OK key in the middle of the D-pad; gint sets this to BACK.) Final review:
  `int CGD_EnterKeycode2 = -1` (same weak-hook pattern): a second key for KEY_ENTER besides EXE when no keymap
  action uses it; gint sets OK (0x84) on the fx-CG100, so OK selects in the Doom menu as in the launcher.
- **Auto frameskip flag:** expose `int CGD_AutoFrameskip` (0 = fixed CGD_Frameskip semantics, non-zero = auto),
  referenced by gint (a strong reference since the engine defines it; gint sets CGD_AUTOFRAMESKIP_DEFAULT for Auto). Auto semantics: game tics stay at 35 Hz; D_Display is skipped while rendering is
  behind PL_GetTics(); at least one frame is drawn every N tics (N = CGD_AutoFrameskip max skip, document).
- **8.3 names:** demo names must be strict 8.3 too (gint rejects others). Engine helper must produce valid 8.3 names
  regardless of the WAD name length (done: CGD_FilePrefix; gint passes the WHOLE base name as CGD_WADFileName since
  gint review round 1 — it used to truncate it to 6 characters, which made freedoom1/freedoom2 share saves).
- Remove the CG100-keymap TODO in cgdoom-kbd.c; CG100 keymaps live in src/gint/keymaps.c (platform copies a preset into
  CGD_Options.Keymap).
- `PL_WadIsSlow()` (step 3 addition), implemented by gint (round 3.3): 1 in flash block-map/BFile modes, 0 in the
  RAM-image mode (or with no WAD); constant between PL_WadOpen and PL_WadClose (a MAP->BFILE fallback stays slow).
- D_DoomMain is not re-entrant: after a game the add-in exits to MPM (as CGDoom did).
- **Low-detail semantics (for engine step 3):** the SH4 drawers (src/engine/sh4, done) implement low detail with
  Chocolate Doom's corrected semantics (dc_x NOT modified; span count taken before doubling; ds_x1/ds_x2 doubled on the
  globals like vanilla) — linuxdoom's C versions corrupt dc_x and overdraw spans. The C reference low-detail functions
  in r_draw.c MUST be copied from src/engine/sh4/test/ref_draw.c (R_DrawColumnLow, R_DrawSpanLow, fuzz/translated low)
  so host goldens and SH4 output agree bit-exactly.
- **Drawer function pointers (for engine step 3):** CGDoom calls drawers directly. Restore vanilla's pointer plumbing:
  r_segs.c's three R_DrawColumn() calls → colfunc(); r_plane.c R_DrawSpan() → spanfunc() and the sky column →
  colfunc(); r_things.c `colfunc = R_DrawTranslatedColumn` → `colfunc = transcolfunc`; R_ExecuteSetViewSize selects
  colfunc/basecolfunc/fuzzcolfunc/transcolfunc/spanfunc by detailshift. Leave a single clearly-marked place where an
  SH4 build (`#ifdef __sh__` and not `CGD_NO_SH4`) selects the *_SH4 versions (done in the SH4 integration: `CGD_SH4` in r_draw.h; engine-changes.md "SH4 integration").

## 9. Scope (2026-10-11) — overrides earlier sections

The target is the fx-CG100 / Graph Math+ with OS 02.00 ("02.00.0000"), mpm.bin 1.1 and the packed shareware
`doom1.wad`, single player. Removed as modes beyond that target:
- fx-CG50 support (the "bonus" of the header above): its OS path, syscall trampoline, OS-heap forwarding,
  keymap presets, experimental RAM and memory warnings. On any other model or OS string, `main()` shows
  "Unsupported calculator" and returns before any OS call, UTLB or heap change.
- The "experimental OS" tables for fx-CG100 OS 01.00 and 02.10.
- RAM discovery for older mpm.bin builds (Feb-2025, dynload, pre-Feb-2025: private arenas) and the URB
  containment that only did something under Feb-2025 without the executor. Those builds are now refused
  at startup (`CGD_MIN_TOTAL_HEAP` 3 MiB; 1.1 leaves about 5.5 MiB by calculation, Feb-2025 about 2 MiB).
- The debug options *WAD access: BFile reads* and *Trust unaligned lumps* (BFile reads remain the
  automatic fallback). *Developer info* stays until the first hardware run (its after-game screen is the
  only place the stack peak, zone peak and UTLB repairs of a game can be seen).
Old `CGDOOM.CFG` lines for removed options (`WADMethod`, `TrustUnalignedLumps`, `CG.Model`, `CG.ExtraRAM`)
are ignored.

**Later: restore OS 02.10** once MPM's installer supports it (as of 2026-10-11 it does not; the bundled
mpm.bin 1.1 already selects its 02.10 table, `ref/mpm/start.c:40`). The pinned gint has no 02.10 table
(API 0, the dead fx-CG50 gate), so CGDoom needs its own: `use_os210_table()` and the `OS210_BFILE_*`
addresses in `src/gint/sys.c` at commit 447de85 (11 entries, Bfile_GetBlockAddress included). Accept
"02.10.0000" in `gsys_init()` with that table, as a supported OS (no prompt). If gint gains a 02.10 API
by then (Kin1009's fork has one, `ref/extra/Kin1009_gint` commit ee12efd), use its BFile wrappers instead
and keep only the block-address entry. Settle `BFile_Write` first: mpm's `casiowin.yaml:35` says 0x80396a92,
libsyscall and Kin1009's gint say 0x80396a06; only 0x80396a06 is +0x62A68 from 02.00 like every other
entry. A wrong address fails at the first file write (settings and saves at exit). Disassembling both
addresses in a 02.10 OS image against 02.00's 0x80333f9e would decide it without hardware.

**New-display units** (bootcode 2025.0304.1404, OS 02.00.xx1x / 02.10 / 02.20) need nothing in CGDoom's
display code: it only uses gint's `r61524_start_frame()` (both controllers) and the data port. Open
outside CGDoom: MPM's installer, OS addresses for 02.00.0010 (unknown everywhere; MPM TODO) and 02.20,
and gint's world-switch `hsave`/`hrestore`, which save only the old controller's window registers
(0x210-0x213).
