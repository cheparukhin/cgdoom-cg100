# fx-CG100 platform reference

Facts about the fx-CG100 / Graph Math+ / fx-1AU Graph, the MPM add-in loader and gint that CGDoom relies
on, and that any other add-in for this calculator needs. What CGDoom builds on them (memory budget, UTLB
pinning, WAD back-ends, exit path, OS gate) is in [DEVELOPMENT.md](DEVELOPMENT.md).

Revisions: gint `463f180` plus [`tools/gint/cgdoom-compat.patch`](../tools/gint/cgdoom-compat.patch), fxSDK
`a6e46eb`, MPM `cbd1a41` (`mpm.bin` 1.1, which casio-mpm-220 builds on). Source paths are in those trees.

Tags: **[V]** verified in source code, an OS image or primary documentation; **[HW]** observed on an
fx-CG100 running CGDoom; **[F]** reported by others (forums, other projects' code or docs), not checked
here; **[G]** guess or estimate.

## 1. Hardware status

- [HW] CGDoom starts, loads DOOM1.WAD, plays normally and exits to the MPM menu on an fx-CG100 with the new
  display (bootcode DateA `2025.0304.1404`), OS 02.20 and casio-mpm-220's MPM, at F4 with the DMA presenter.
- [HW, anecdotal] At the OS clock (safe start) it still held its 35 fps cap; the places were not recorded.
- Not observed yet: the old display, OS 02.00, saves and settings written and reloaded, relaunching, and
  every Diagnostics value (clock registers, r15, arenas, block-map statistics, flash and push timings).

## 2. CPU, clocks and on-chip resources

- **MPU:** SH7305 as in the fx-CG50: SH-4A with the DSP extension (SH4AL-DSP), big-endian, no FPU, 8 MiB
  SDRAM [F: Lephenixnoir, "same CPU, same MPU"]. Misaligned 16/32-bit loads raise an address error [V];
  casio-emu tolerates them, so only hardware catches them [F: gpSP].
- **DSP:** gint sets SR.DSP, so `ldrs`/`ldre`/`ldrc` repeat loops work, but its interrupt wrapper does not
  save RS/RE or the DSP registers: no repeat loops in code that can run in an interrupt [V `kernel/inth.S`].
- **Clocks:** 32,768 Hz × 450 (FLLFRQ `0x4384`) × PLL. gint's fx-CG100 presets, in MHz [V `cpg/overclock.c`]:

  | Preset | FRQCR | CPU Iφ | Sφ | Bus Bφ (SDRAM, LCD) | Pφ (timers, USB) | CS0WCR (flash) |
  |---|---|---|---|---|---|---|
  | F1 | `0x0F011112` | 117.96 | 58.98 | 58.98 | 29.49 | `0x3C0` |
  | F2 | PLL×16 /4 /8 /8 /8 | 58.98 | 29.49 | 29.49 | 29.49 | `0x340` |
  | F3 | PLL×26 /4 /8 /8 /8 | 95.85 | 47.92 | 47.92 | 47.92 | `0x240` |
  | F4 | `0x1F001203` | 235.93 | 117.96 | 58.98 | 29.49 | `0x2C0` |
  | F5 | = F4, "the Graph Math+ cannot handle the higher bus speed" | | | | | |

  - CGDoom plays at F4; the launcher and safe start run at the OS clock (`src/gint/clock.c`). That the OS
    clock is F1 is [G]; Diagnostics → System shows the registers.
  - F4 doubles the CPU only: the SDRAM and LCD timings (CS3WCR, CS5aWCR) are the same at every preset [V].
  - CalcLoverHK's "Ptune4 F5" also runs the bus at 117.96 MHz (CS3WCR TRC 6); some units fail it or flip
    bits [F]. CGDoom does not use it.
  - gint restores the OS clock on every world switch and at exit (`drv_cpg`) unless
    `cpg_set_overclock_permanent(true)`; the `clock.h` comment saying otherwise is stale [V].
- **Caches:** 32-byte lines [V]. Size and ways unknown; assumed 32 KiB I + 32 KiB O, 4-way, as on the
  SH7724 [G]. Whether the SH-4A performance counters exist is unknown [G].
- **On-chip memory** [V `fxcg50.ld.c`, `kernel/world.c`, `dma/dma.c`]:
  - ILRAM: 4 KiB at `0xE5200000`; gint uses about 180 B. CGDoom's `.ilram` is 1,552 B with gint's.
  - X and Y RAM: one 8 KiB bank each, 16 KiB in all. gint links `.xyram` at `0xE500E000` (X) and
    `0xE5010000` (Y); WikiPrizm gives `0xE5007000` and `0xE5017000`. Same banks: gint's backup saves exactly
    8 KiB at each of its addresses, and gpSP found all `0xE500xxxx`/`0xE501xxxx` windows aliased [F].
    `dma_translate` passes only the `0xE5007000`/`0xE5017000` windows (8 KiB each) and ILRAM, so DMA must
    use those. CGDoom uses no X/Y RAM (`.xyram` 0 B).
  - World switches reload ILRAM and X/Y from their load images by default (`GINT_ONCHIP_REINITIALIZE`):
    code survives, data does not. `gint_set_onchip_save_mode(GINT_ONCHIP_BACKUP, buf)` with a 20 KiB buffer
    keeps them; CGDoom does this.
  - RS memory (`0xFD800000`) holds OS code, flash writing included: never touch it [F: WikiPrizm]. SPU
    PRAM0 (`0xFE200000`, 160 KiB, 32-bit accesses only) is untested under MPM [G].
- **DMA:** 6 channels at `0xFE008020`. gint uses 0 for the LCD, 1 for `dma_memcpy`/`dma_memset`, 3 and 4
  for USB; 2 and 5 are free. P0 addresses are not translated: DMA from P1/P2 or the on-chip windows, after
  writing cached data back. gint clears a callback after calling it, so a callback cannot restart its own
  channel [V].
- **Timers:** 3 TMUs and 6 ETMUs (32,768 Hz), all owned by gint. The keyboard scan takes an ETMU (128 Hz),
  `prof_init()` a TMU (Pφ/4, ~136 ns); `TIMER_ANY` searches from the last ETMU down. Timers are rescaled
  on clock changes [V]. `rtc_ticks()` (128 Hz) is slow and wraps at midnight [V].
- **USB** (fxlink) works under gint [F]; Pφ is 29.49 MHz at both F1 and F4 [V]. **Sound:** none [F].

## 3. Memory map under MPM 1.1

| Virtual | Backing | Size | Contents |
|---|---|---|---|
| `0x00000000`–`0x00000FFF` | flash `0xA0000000` | 4 KiB | NULL page, user read/write: NULL reads return boot ROM, no fault |
| `0x00300000`–`0x0066FFFF` | SDRAM from `romAddress` | 55 × 64 KiB | add-in code, rodata, `.data` image |
| `0x08100000`–`0x0817FFFF` | SDRAM `0x8C160000` (OS AppMemory) | 8 × 64 KiB | VBR (5 KiB), `.data`/`.bss`, `_uram` |
| P1 `0x8C200000`–`romAddress` | SDRAM | variable | gint's `_ld1` arena |
| P1 `0x8C7F0000`–`0x8C7FFFFF` | SDRAM | 64 KiB | relocated `mpm.bin`: never touch |

- [V `mpm/main.c`, `loader.c`] `romAddress = (0x8C7F0000 − binary) & ~0xFFFF`, binary = g3a − 0x7000 B.
  MPM reads the whole binary into RAM with one `BFile_Read`: no demand paging; code and rodata count
  against RAM and run at SDRAM speed. UTLB entries 0–54 map the image, 55–62 the static RAM, 63 the NULL
  page, all user read/write and cached, so code is writable. MMUCR: AT=1, SV=1, URB=54. All 55 image pages
  are mapped; past the image they reach `mpm.bin`, then the end of RAM. MPM rewrites all 64 entries before
  each launch.
- Entry: `code('MPM0' = 0x4D504D30, loadinfo)`, loadinfo `{1,1, 0x10,0x8C200000, 0x11,romAddress, 0}`,
  which gint turns into the default arena `_ld1` [V `kernel/start.c`, `kernel/kernel.c`].
- `_ld1` = 0x5F0000 − ceil(binary / 64 KiB) × 64 KiB: 6,160,384 B for a tiny add-in, 2,621,440 B at MPM's
  limit, 5,832,704 B (5.56 MiB) for CGDoom's 329,816-byte binary [V by calculation; not read on hardware].
- `_uram` = 512 KiB − 5 KiB VBR − 16 KiB gint stack reserve − static data = 502,784 B − (`.data` + `.bss`
  + `.gint.bss`) [V]. Before `main()` gint takes about 182 KB of it, including its uncached 177,504 B
  VRAM (`src/gint/uram_check.ld`). The fx-CG50's `_ostk` arena does not exist here [V].
- `malloc` tries `_uram`, `_ld1`, then `_os` (the OS heap, through OS calls). Blocks are 4-byte aligned
  and at most 2^24 B; align DMA buffers yourself [V `kmalloc/`].
- **Limits:** gint's linker script caps the binary at 2 MiB; MPM refuses one over 0x370000 B (−1001) [V].
- **Other `mpm.bin` builds** on OS 02.00 installs: the Feb-2025 forum zip announces a fixed 2 MiB `_ld1`;
  the `dynload` build shipped with gpSP and KhiCAS passes no loadinfo, so there is no `_ld1` [F; both are
  in MPM's git history]. CGDoom
  refuses both (under 3 MiB of heap). casio-mpm-220's hook runs only its own sealed `mpm.bin` [V].
- **Physical:** SDRAM `0x0C000000` (P1 `0x8C…`, P2 `0xAC…`); 32 MiB NOR flash at 0 (P1 `0x80…`, P2
  `0xA0…`), bootcode in the first 128 KiB, OS version string at `0x80020020`, model string at `0x80000300`
  [V]. gint treats any model string but `810A`/`811A`/`810F` as an fx-CG100 [V `kernel/hardware.c`].
  Touching `0x8C800000` or above reportedly cold-boots the calculator [F: gpSP].
- **OS RAM:** the OS uses the first 2 MiB [F]; OS 02.00 and 02.20 both load stack pointer `0x8C1E0000` and
  VRAM `0xAC000000` [V: casio-mpm-220 `docs/ARCHITECTURE.md`]. RAM is not zeroed between launches: the OS
  fills add-in areas with 0x55 and MPM leaves the last add-in's data [F].
- **Stack:** the add-in starts on the OS's stack (MPM: "TODO: Move the stack pointer"); its free depth is
  unknown [G]. gint never sets r15, so its 16 KiB reserve and canary guard nothing [V]; gint notes Fugue
  overflowing 8–12 KiB stacks [F]. CGDoom plays on a private 128 KiB stack.

## 4. OS calls and world switches

- No syscall table: the fx-CG50 gate at `0x80020070` is dead on every fx-CG100 OS; each function is called
  at its address for the exact OS version (§5) [F: Lephenixnoir; V for gint's table].
- gint calls the OS only inside `gint_world_switch(GINT_CALL(f, up to 4 args))`; its POSIX and stdio file
  functions do not switch by themselves [V `bfile.h`].
- A world switch saves and restores every driver, restores the OS clock and then re-applies the add-in's
  (two CPG reprogrammings, each with a timer rescale), reloads on-chip memory unless backed up, and stops
  gint's timers while the OS runs [V `kernel/world.c`]. Overclocked, each switch can lose about one 128 Hz
  RTC tick; gpSP saw minutes of drift from one switch per 4 KiB block [F].
- While the OS runs [F: gpSP], it may draw its busy indicator straight into LCD GRAM, and its `LDTLB`s can
  replace UTLB entries 0–53, the add-in's code pages included; gint's miss handler then jumps to the dead
  gate. CGDoom's countermeasure: [DEVELOPMENT.md §2.5](DEVELOPMENT.md#25-utlb-protection).
- gint leaves the MMU on during OS calls; MPM turns it off around its own ("The OS will crash in many
  spots") [V]. BFile works with it on [HW]; other OS functions are untried. CGDoom hands the OS only P1
  buffers and paths, as a precaution [G].
- So batch file work into as few switches as possible, and none during gameplay.

## 5. OS versions and call addresses

| Internal (`0x80020020`) | Shown | Display | gint (patched) | MPM |
|---|---|---|---|---|
| `01.00.0000` | 01.00 | first Graph Math+ | API 1 | no installer [F] |
| `02.00.0000` | 02.00.0202, 02.00.2202 | old only | API 2 | original installer (Windows) + `mpm.bin` 1.1 |
| `02.00.0010` | 02.00.0212 ("2.00.1") | new | API 0: dead gate | casio-mpm-220 updates it to 02.20 |
| `02.10.0000` | 02.10.x202 | both [F] | API 0: dead gate | casio-mpm-220 updates it to 02.20 [HW] |
| `02.20.0000` | 02.20.0202 | both [F] | API 3 | casio-mpm-220 |

CGDoom accepts only `02.00.0000` and `02.20.0000`. Stock gint matches only the prefix `02.00` (so it picks
API 2 for `02.00.0010` too) and has no 02.20 table; the patch fixes both [V]. Addresses [V `syscalls.S`,
the patch, `src/gint/sys.c`]:

| Function | 02.00 | 02.20 | | Function | 02.00 | 02.20 |
|---|---|---|---|---|---|---|
| OS `malloc` | `0x80366708` | `0x803CBFB0` | | `BFile_GetPos` | `0x80333D7C` | `0x80398AA8` |
| OS `free` | `0x803664D4` | `0x803CBD7C` | | `BFile_Read` | `0x80333DC2` | `0x80398AEE` |
| OS `realloc` | `0x803672C8` | `0x803CCB74` | | `BFile_Write` | `0x80333F9E` | `0x80398CCA` |
| `BFile_Remove` | `0x80334212` | `0x80398F3E` | | `BFile_Create` | `0x80333EF0` | `0x80398C1C` |
| `BFile_Rename` | `0x803341C2` | `0x80398EEE` | | `BFile_FindFirst` | `0x803345C8` | `0x803992F4` |
| `BFile_Open` | `0x803338D0` | `0x803985FC` | | `BFile_FindNext` | `0x80334846` | `0x80399572` |
| `BFile_Close` | `0x80333A4E` | `0x8039877A` | | `BFile_FindClose` | `0x80334950` | `0x8039967C` |
| `BFile_Size` | `0x80333B04` | `0x80398830` | | `Bfile_GetBlockAddress` | `0x80333CF2` | `0x80398A1E` |
| `BFile_Seek` | `0x80333C6C` | `0x80398998` | | | | |

- Every BFile entry moved by `+0x64D2C`. The 02.20 addresses were checked against the stock image (hash
  and method: DEVELOPMENT.md §3). The timer, `GetKeyWait` and `GetVRAMAddress` entries are in the patch.
- gint wraps everything above except `Bfile_GetBlockAddress`, which CGDoom calls directly. `PutKeyCode`,
  `ClearKeyBuffer`, `SetQuitHandler`, `PowerOff` and `Reset` have no fx-CG100 address: gint panics with
  `0x10C0` "Missing syscall for this OS version" [V].
- `BFile_Read(fd, buf, size, whence)`: `whence` ≥ 0 is an absolute offset, −1 the current position; on
  02.00 it returns the bytes read (MPM launches add-ins only if `rc == size`) [V].
- If OS 02.10 is ever wanted: MPM 1.1 gives `BFile_Write` `0x80396A92`, libsyscall and Kin1009's gint
  `0x80396A06` (02.00 + `0x62A68`, the offset of every other entry). Check both in a 02.10 image first.

## 6. Storage

- **Size:** about 4.5 MB; the one byte figure is 4,793,072 B, from a single forum post [F].
  `tools/wadtool.py` budgets 4,500,000 B. It cannot be enlarged [F]. The packed DOOM1.WAD (3,389,236 B)
  and CGDoom.g3a (358,492 B) fit; the full shareware WAD (4,196,020 B) barely; bigger IWADs do not.
- **Files:** Fugue file system, 4 KiB clusters [F]. Names are 8.3 and case-insensitive: the Memory app
  truncates longer names (`~1`) and extensions [V: Casio manual]. Over USB ("USB flash drive") add-ins and
  data go in the root, not `@MainMem` [F]. A file an add-in creates may not be listed by
  `BFile_FindFirst` until after a USB connection [F].
- **Direct pointers:** `Bfile_GetBlockAddress(fd, offset, void **ptr)` returns the flash address of one
  4 KiB block (offset 4 KiB aligned); `rc < 0` is an error [V: MPM reads add-in icons this way]. From gpSP
  on hardware [F]: query all blocks in one world switch; convert P2 results (e.g. `0xA1B97000`) to the
  cached P1 alias; reject P0 results (`0x001129xx`), which fault once the OS's TLB entry is gone; `ocbi`
  newly mapped lines once; query again after any storage write, since Fugue may move records, other files'
  included [G]. Where the file system sits in flash is unsettled (gint PR 62 guesses physical
  `0x00C80000`–`0x01420000`; gpSP saw `0x01B97000`), so accept any P1/P2 flash address.
- **Writes** are slow, one world switch each. Overwriting a same-size file in place is the fast path;
  otherwise remove, create with the final size, write [F: gpSP].
- **Flash reads** [G, from register values]: CS0 is 16 bits wide with no burst mode, so a 32-byte line
  fill takes about 1.9–2.7 µs, against 0.35–0.45 µs from SDRAM, at any preset. Diagnostics → Benchmarks
  measures it; no result recorded yet.

## 7. Display

- 396 × 224 RGB565; the OS uses only 384 × 216 [V]. Data port `0xB4000000` (P2, area 5A, 16-bit); register
  select is PRDR bit 4 (`0xA405013C`) [V `r61524/r61524.c`].
- **Two controllers**, told apart by reading register `0xDA` [V, gint commit `2700096`]: the old display's
  Renesas R61524 (window registers `0x210`–`0x213`, X mirrored, then `0x202`), and the new display's ID
  `0x32` or `0x52` (MIPI-DCS-style `0x2A`/`0x2B` window and `0x2C` write, X not mirrored). [HW] only on
  the new one so far.
- `r61524_start_frame(x0, x1, y0, y1)` handles both. `r61524_win_get/set` and stock gint's world-switch
  window save/restore use the old registers, which are other commands on the new controller; the patch
  skips the save/restore there, and OS 02.20 sets its own window before every screen update [V].
- **Push:** `r61524_start_frame()`, then RGB565 halfwords to `0xB4000000` by CPU or DMA (channel 0,
  `DMA_32B`, fixed destination). gint's `r61524_display()` uses DMA only for full 396-pixel rows and
  heights that are multiples of 4 (its VRAM stride); a contiguous buffer for a narrower window works:
  CGDoom sends 320 × 200 as 4,000 blocks [HW], gpSP 240- to 384-wide strips [F].
- **Speed:** the LCD timings are the same at every gint preset [V]. gint's author measures about 11 ms per
  full frame by DMA [F: `display-cg.h`]; 320 × 200 takes about 8 ms by proportion [G]. DMA reads from SDRAM
  compete with the CPU's cache misses [G]. gint's VRAM is uncached, 13–14 cycles per write [F: gint];
  render into cached RAM and write it back before DMA.
- The OS status bar is not drawn while gint owns the screen; MPM redraws its frame afterwards [V]. gint's
  backlight call (`r61525_brightness_set`) is untested on the new controller [G].

## 8. Keyboard

Physical layout as matrix codes `(row << 4) | (7 − col)` [V `src/gint/keymaps.c`, gint `keysc/keydev.c`]:

| Row | col 6 | col 5 | col 4 | col 3 | col 2 | col 1 |
|---|---|---|---|---|---|---|
| 9 | ON `91` | HOME `92` | ⇤ prev. tab `93` | ▲ `94` | ⇥ next tab `95` | page up `96` |
| 8 | SETTINGS `81` | BACK `82` | ◀ `83` | OK `84` | ▶ `85` | page down `86` |
| 7 | SHIFT `71` | ALPHA `72` | VARIABLE `73` | ▼ `74` | CATALOG `75` | TOOLS `76` |
| 6 | X,θ,T `61` | fraction `62` | √ `63` | ^ `64` | x² `65` | eˣ `66` |
| 5 | , `51` | sin `52` | cos `53` | tan `54` | ( `55` | ) `56` |
| 4 | 7 `41` | 8 `42` | 9 `43` | DEL `44` | none `45` | |
| 3 | 4 `31` | 5 `32` | 6 `33` | × `34` | ÷ `35` | |
| 2 | 1 `21` | 2 `22` | 3 `23` | + `24` | − `25` | |
| 1 | 0 `11` | . `12` | ×10ˣ `13` | FORMAT `14` | EXE `15` | |
| 0 | AC `07` | | | | | |

- KEYSC: six 16-bit words at `0xA44B0000`, side-effect free to read. Word *i* holds row 2*i*+1 in its high
  byte and row 2*i* in its low byte; bit *col* is the key [V `src/gint/input.c`].
- No F1–F6, MENU, OPTN or EXIT keys. gint aliases `KEY_HOME` = `KEY_MENU`, `KEY_BACK` = `KEY_EXIT`,
  `KEY_TOOLS` = `KEY_OPTN`, `KEY_FORMAT` = `KEY_FD`; CATALOG + a row-6 key gives `KEY_F1`–`F6` events [V].
- **gint's key names disagree with matrix codes here** [V `keydev.c`]: `ev.key` remaps only rows 7–9 and
  FORMAT, so rows 5–6 report fx-CG50 codes (fraction gives `KEY_LOG`); `keydown()` remaps rows 5–9 and
  treats other codes above row 4 as missing keys. Read raw KEYSC (as CGDoom does) or `ev.row`/`ev.col`.
- gint's SHIFT + 7 + 3 + AC/ON abort chord reads `keydown()` state, which only advances as events are
  dequeued: an add-in that never drains events never gets it [V]. CGDoom checks the chord itself.
- The OS's `GetKey` cannot see ON; `GetKeyWait` can [F].

## 9. MPM

- Two parts [V]: an OS patch (HOME → TOOLS loads `\\fls0\mpm.bin` to `0x8C700000` and runs it) and
  `mpm.bin`, the open-source menu and loader.
- **Installing:** casio-mpm-220 patches Casio's OS 02.20 Mac updater and installs 02.20 with MPM over an
  older OS [HW: from 02.10, new display]. The original Windows installer patches only 02.00.xx0x on
  old-display units [F]. Downgrading to 02.00 works only on old-display units; new ones stay black [F].
  Exam mode disables MPM; any Casio OS update removes it [V].
- **Recovery** [HW, 14 Oct 2026, new display]: Casio's unchanged Mac updater reinstalled stock 02.20 over
  casio-mpm-220's patched 02.20 from the OS ERROR screen, on the first attempt after the calculator had
  been off overnight; attempts minutes after a RESTART failed at the first handshake. Details: casio-mpm-220
  [`docs/UPDATER.md`](https://github.com/cheparukhin/casio-mpm-220/blob/main/docs/UPDATER.md); the patch
  and its build: `docs/ARCHITECTURE.md`, `docs/BUILDING.md` there.
- **Menu** [V `mpm/main.c`, `addins.c`]: lists non-folder `*.g3a` files of at least 0x7002 B in the root;
  OK/EXE launches, VARS shows details. Icons: 92 × 64 at g3a offsets 0x1000 and 0x4000. SETTINGS receives
  a 4-byte big-endian size and a header-less binary over USB straight into RAM (AC cancels):
  `fxlink -pw build-cg/<target>.bin`.
- After the add-in returns, MPM turns the MMU off, redraws and rescans. It does not wait for keys to be
  released, so a held key acts in the menu [V], and it does not close the OS app that was open [F].

## 10. Leaving an add-in

- No return to the OS menu: `gint_osmenu()` returns at once here and `gint_setrestart()` is forced off
  [V]. Quit by returning from `main()`: `kquit` restores the drivers and the clock, then the MPM menu
  returns [V, HW].
- Never call `gint_poweroff()` or `gint_set_quit_handler()` or use the panic screen's reset key: they panic
  with `0x10C0` (§5). `getkey()`'s defaults include `GETKEY_POWEROFF` (SHIFT + AC, same panic) and
  `GETKEY_MENU` (a pointless world switch): mask both [V].
- Before returning, wait until all KEYSC words read 0 and the LCD DMA is done, and close every file
  (fxlibc's `exit()` does not) [V].
- Nothing powers the calculator off while an add-in runs (gint owns the interrupts; `PowerOff` has no
  address) [G]. CGDoom quits after an idle period instead.

## 11. Toolchain and prior art

- **fxSDK:** no fx-CG100 target; the `fxsdk build-cg` g3a runs on both models and gint tells them apart at
  run time (`gint[HWCALC] == HWCALC_FXCG100`). Flags `-m4-nofpu -mb -fno-builtin -nostdlib -Wa,--dsp`, gint's
  `fxcg50.ld` [V `cmake/FXCG50.cmake`]. `-fno-builtin` sends `memcpy`/`memset` to fxlibc's assembly.
  `generate_g3a()` also writes the header-less `build-cg/<target>.bin` for MPM's push. sh-elf-gcc 14.1 has
  the `.` and `m4-nofpu` multilibs; pass `-m4a-nofpu` at compile time only [V].
- **gint** is used from its dev branch (no release in a long time [F]). Any gint add-in needs
  `tools/gint/cgdoom-compat.patch` for OS 02.20 and the new LCD; casio-mpm-220's README points to it.
- **Prior art** [F]:
  - gpSP-sh4-jit (GBA emulator, gint): `-O3` on hot files ran slower than `-O2` on hardware (I-cache);
    large DMA strips from X/Y RAM beat 4-row strips and per-scanline streaming; the block-map rules of §6.
  - Azur (Lephenixnoir): writing a whole 396 × 224 frame to RAM takes 6.1 ms; an on-chip clear 0.5 cycle
    per pixel.
  - NESizm, Prizoop, fxcraft (CalcLoverHK): libsyscall instead of gint, with per-OS address tables in a
    `casiowin.yaml`; its crt0 uses the 02.10 table for `02.1…` and the 02.00 table for anything else.
  - CGDoom's drawers use techniques from GBADoom, D32XR and Azur:
    [`src/engine/sh4/README.md`](../src/engine/sh4/README.md).

## 12. Sources

- Casio, [fx-CG100 manual, Memory app](https://support.casio.com/global/en/calc/manual/fx-CG100_1AUGRAPH_en/BONDSYacwdbyyk.html);
  [WikiPrizm, On-chip memory](https://prizm.cemetech.net/Technical_Documentation/Peripherals/On-chip_Memory/).
- Planète Casio, [MPM thread (topic18534)](https://www.planet-casio.com/Fr/forums/topic18534-1-mpm-mod-add-ins-math.html):
  the forum facts above unless another source is named.
- [gpSP-sh4-jit](https://github.com/KaraRyougi/gpsp-sh4-jit) (`ports/fxcg100`, `docs/performance-history.md`);
  CalcLoverHK's [libsyscall](https://git.planet-casio.com/CalcLoverHK/libsyscall);
  [casio-mpm-220](https://github.com/cheparukhin/casio-mpm-220).
- Local clones (not in git): `ref/` holds gint, fxsdk, fxlibc, OpenLibm, sh-elf-gcc, sh-elf-binutils and
  CGDoom at the pinned commits, and `ref/extra/` the projects above (gpsp-sh4-jit, Azur, d32xr, GBADoom,
  CalcLoverHK's ports and libsyscall, Kin1009's gint, unmerged gint PRs in `gint-prs.git`). MPM's source
  is `../casio-mpm-220/ref/mpm`.
