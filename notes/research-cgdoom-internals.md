# CGDoom internals and a porting checklist for gint on the fx-CG100

Scope: CGDoom `master` (commit `8516c16`, 2021-10-01, Lephenixnoir) in `/Users/cheparukhin/casio_doom/ref/CGDoom`, plus the `origin/in330` and `origin/in330-backup` branches. The aim is to port it to the fx-CG100 / Graph Math+ using gint (dev, `463f180`) and fxlibc (`e2f458f`), loaded by MPM (`cbd1a41`).

Legend:
- **[V]**: verified in source (file:line given) or by an experiment I ran.
- **[D]**: from documentation, a forum post or another project's notes.
- **[G]**: my guess or inference.

All `cgdoom/…` and `src-cg/…` paths are relative to the CGDoom repo root.

---

## 0. Summary

1. **Platform code is small and isolated [V].** Almost all of it lives in `cgdoom/cgdoom.c` (WAD mapping, settings, delayed saves, `main`), `cgdoom/i_system.c` (zones, timing, input events), `cgdoom/i_video.c` (palette and LCD), `cgdoom/cgdoom-kbd.c` (KEYSC), `cgdoom/cgdoom-ui.c` (menus), `cgdoom/m_misc.c` / `cgdoom/m_menu.c` (BFile), `cgdoom/cgdoom-alloc.c` (PRAM0) and `src-cg/*`. The engine (`p_*`, `r_*`, `g_*` and so on) only reaches the platform through the `I_*` layer, `W_*`, `Z_*`, `CGD_*` and a few BFile calls.
2. **Every memory trick CGDoom uses on the fx-CG50 is wrong or unneeded on the fx-CG100 [V/D]:**
   - System-stack heap at `0xac0f0000`.
   - "2 MB line" RAM at `0x8c200000` (MPM gives this to the add-in as the `_ld1` arena).
   - PRAM0 at `0xfe200000`.
   - SecondaryVRAM as heap.
   - XRAM `0xe5007000` used as a BFile buffer.

   Under MPM, gint receives one large default arena of about 4–5.5 MiB (`[0x8c200000, loadAddress)`). Replace all of it with plain `malloc`/`kmalloc` zones.
3. **The direct-LCD code (`CGDOOM_DIRECT_R61524`, `cgdoom/i_video.c:64-175`) must go.** It programs R61524 registers 0x200/0x201/0x202/0x210–0x213. gint now detects "new 2025-2026 displays" (controller version 50 or 82) that use DCS commands 0x2A/0x2B/0x2C instead (`gint/src/r61524/r61524.c:136-176`). A freestanding direct-R61524 build on a real fx-CG100 "only draws a single line" [D: gpsp-sh4-jit `ports/fxcg100/README.md:40`]. Use `r61524_start_frame()` with DMA, or `dupdate()`.
4. **WAD memory-mapping can be redone much better on the fx-CG100 [D].** The OS function `Bfile_GetBlockAddress` (fx-CG100 OS 2.00 direct address `0x80333cf2`; fx-CG50 syscall `0x1DAA`) returns the flash address of each 4 KiB block of an open file. This replaces CGDoom's brute-force 512-byte sector search over 32 MiB. gpsp-sh4-jit uses it on real fx-CG100 hardware, including a P2→P1 conversion and rejection of P0 (TLB-mapped) addresses.
5. **Storage is the real constraint on the fx-CG100 [D].** Storage memory is about 4,793,072 bytes, so doom1.wad (about 4.2 MB) fits but doom2.wad and doomu.wad (12–15 MB) do not. CGDoom's split Ultimate Doom WADs (`doomu1..4.wad`) were made for the same problem on the fx-CG50AU.
6. **Big, cheap speed-ups exist in the portable code [V]:**
   - `FixedMul` is a 32-iteration shift-and-add loop (`cgdoom/m_fixed.c:40-82`); `dmuls.l` would replace it.
   - `FixedDiv` makes 16 software `/` plus 16 `%` calls (`cgdoom/m_fixed.c:90-115`).
   - `SHORT()` and `INTFromBytes()` are out-of-line byte-assembling functions (`cgdoom/m_swap.c:58-76`).
   - `finecosine()` is a function call (`cgdoom/r_main.c:127-130`).
   - `RANGECHECK` is on (`cgdoom/doomdef.h:79`).
   - Zone 0 is placed at an **uncached** P2 address (`cgdoom/cgdoom.c:717`, `cgdoom/i_system.c:80`).
   - Overclocking is a gint one-liner (`clock_set_speed`).
7. **The SDL2 build works on macOS arm64 with three small fixes [V, experiment in §8]:** use the in330 `platform.h`, fix the pkg-config cflags, and rename the binary. With a fourth fix to a real out-of-bounds bug (`r_data.c:289`, `patchcount[320]`), it ran Freedoom 0.13 E1M1 headless for 300 tics and produced a correct frame. It is a good host test harness.

---

## 1. Repository layout and build (master)

- `cgdoom/`: 59 `.c` files and headers. This is the id engine (nDoom → CGDoom lineage) plus the CGDoom modules:
  - `cgdoom.c`: main, WAD mapping, settings, delayed writes.
  - `cgdoom-alloc.c`: PRAM0 allocator.
  - `cgdoom-frag.c`: reading fragmented lumps in flash.
  - `cgdoom-kbd.c`: keyboard and keymaps.
  - `cgdoom-ui.c`: launcher UI.
- `src-cg/`: calculator-only files.
  - `platform.h`: memory layout macros and libfxcg includes.
  - `cgdoom-asm.s`: fast 512-byte sector memcmp.
  - `libprof.c/.h`: TMU0 profiler, adapted from gint's libprof.
- `src-sdl2/`: host emulation.
  - `emul.c`: Bdisp, GetKey, RTC and Bfile emulation.
  - `platform.h`.
  - `keyboard.hpp`: libfxcg `KEY_CTRL_*` / `KEY_CHAR_*` constants.
  - `libprof.c/.h`: `clock_gettime`-based profiler.
- `Makefile`: two targets [V].
  - `all-cg`: `sh-elf-gcc -m4a-nofpu -mb -mhitachi -fstrict-volatile-bitfields -O3 -fgcse-sm -fgcse-las -fgcse-after-reload -fmerge-all-constants -D_FXCG_MINICOMPAT`, linked with `-lfxcg -lgcc -nostartfiles -T $(LIBFXCG_ROOT)/toolchain/prizm.x`.
    - It needs Lephe's modified libfxcg fork: the linker script must export `sextra`/`eextra`, used at `cgdoom/i_system.c:86`. Upstream Jonimoose `prizm.x` does not define them [V, cloned into `ref/extra/Jonimoose_libfxcg`].
    - `-fstrict-volatile-bitfields` is required for the PRAM0 structures (`Makefile:24`).
  - `all-sdl2`: `cc -DCG_EMULATOR -I src-sdl2 …`. See §8.
- Output is `CGDoom.g3a` via `mkg3a`. The checked-in `CGDoom.g3a` is 349,292 bytes, so the code is about 320 KiB.

---

## 2. Architecture: startup, game loop, timing, frameskip

### 2.1 Startup (`cgdoom/cgdoom.c:581-837`, `main`)

1. VRAM pointers (`cgdoom/cgdoom.c:587-590`).
   - `VRAM = GetVRAMAddress()`.
   - `SecondaryVRAM = (GetSecondaryVRAMAddress()+3)&-4`.
   - `EnableColor(1); EnableStatusArea(3); Bdisp_FrameAndColor(3,16); Bdisp_FrameAndColor(1,0);` (`:592-595`).
   - `prof_init()` (`:597`).
2. Experimental RAM gate (`:600-605`): allowed only if `GetOSVersion()` (the string at `0x80020020`, `src-cg/platform.h:103`) starts with `03.` and `osv[3] <= '6'`, which means OS 3.60 or older.
3. Default options (`:611-619`):
   - `WADMethod=MMAP`, `TrustUnalignedLumps=1`, `AutoStart=0`, `EnableDemos=0`, `ExperimentalMemory=0`.
   - Keymap `CGD_Keymap_ThumbsOnly`.
   - Then `LoadSettings()` (`:622`) parses `\\fls0\CGDoom.cfg`.
4. File discovery (`:639-643`): `FindFiles(u"\\\\fls0\\*.wad", …, 16)` and `FindFiles(u"\\\\fls0\\*_demo*.lmp", …, 32)`. FindFiles is at `:400-421` and uses `Bfile_FindFirst`, `FindNext`, `FindClose` and `NameToStr_ncpy`.
5. `UI_Main(...)` (`cgdoom/cgdoom-ui.c:583-700`) returns the selected WAD or demo. For a demo, the WAD is found by matching the name prefix and the code calls `G_DeferedPlayDemoFile` with `CGD_PlayDemoOnly=1` (`cgdoom/cgdoom.c:650-667`).
6. Post-menu setup:
   - With experimental RAM: `CGD_2MBLineMemory = FindZeroedMemory(0xac200000)` (`:685-688`).
   - `SaveSettings` runs if the options changed (`:691-692`). This happens before the WAD is mapped, which matters (§7.4).
7. WAD name handling:
   - Single-episode Ultimate detection by file name (`:701-708`).
   - `CGD_WADFileName` is the name without `.wad` (`:711-714`).
   - `SystemStack = 0xac0f0000` (`:717`).
8. WAD access setup:
   - **BFILE**: `gWADfd = Bfile_OpenFile_OS(path,0,0)` (`:727`).
   - **MMAP** (default):
     - Build `FLASH_INDEX` on SystemStack (`:731-745`).
     - `CreateFileMapping` into PRAM0 (`:747-753`).
     - Show error codes −1/−2/−3 (`:755-766`).
     - Optional developer-info screen (`:767-787`).
     - Advance the PRAM allocator past the fragment table (`:789`).
9. `CGD_PRAM_Init(PRAM0_alloc_start, PRAM0_END)` (`:793`), `memset(VRAM,0,384*216*2)` (`:795`), then `D_DoomMain()` (`:796`).
10. After `D_DoomMain` returns:
    - Close the WAD.
    - Flush every delayed file write: `DelayedWriteFile`, `:423-467`, called at `:801-805`.
    - Optional stats screen (`:807-833`).
    - `prof_quit()`, then return to the OS.

### 2.2 `D_DoomMain` (`cgdoom/d_main.c:423-578`)

- `Z_Init()`, then `Z_AddZone(I_ZoneBase(&size, i))` for every zone the platform supplies (`:437-446`).
- `W_InitMultipleFiles()` runs at `:448`, game mode is detected from lump names (`:450-462`), and it runs **again** at `:514`. See bug B3.
- The single-episode Ultimate override is at `:475-492`.
- Init sequence: `V_Init`, `I_Init` (empty), `M_Init`, `R_Init`, `P_Init`, `HU_Init`, `ST_Init` (`:509-557`).
- Optional `G_RecordDemo("%s_demo%02d")` (`:560-564`).
- Then a demo, `G_DeferedInitNew` (autostart), or `D_StartTitle`, and finally `D_DoomLoop()`.

### 2.3 Game loop (`cgdoom/d_main.c:259-287`) [V]

```c
while (!fuck) {               // "fuck" is the quit flag, set by I_Quit()
  I_StartTic();               // rate limiter + FPS counter + keyboard → D_PostEvent
  if (advancedemo) D_DoAdvanceDemo();
  M_Ticker(); G_Ticker();     // exactly ONE game tic per iteration
  if (fuck) break;
  gametic++;
  if(!(gametic % (CGD_Frameskip + 1))) D_Display();
}
```

- There is no `TryRunTics`, no netcode and no catch-up. Every loop iteration is exactly one 1/35 s game tic, and the display runs every `(CGD_Frameskip+1)` tics.
- If the CPU is too slow, the game runs in slow motion. It never drops more frames than the fixed frameskip. "Auto frameskip?" is a README TODO (`README:38-41`).

### 2.4 Timing and rate limiting (`cgdoom/i_system.c:235-257`) [V]

- When `gametic % (CGD_Frameskip+1) == 0`, `I_StartTic` busy-waits until at least `28571 * (CGD_Frameskip+1)` µs have passed since the previous such point. It measures with libprof (`prof_enter`/`prof_leave` on TMU0).
- `28571` µs is 1/35 s, so the game runs at most 35 tics/s.
- The default `CGD_Frameskip = 1` (`cgdoom/cgdoom.c:129`) gives at most 17.5 rendered FPS (commit `97b31a9`).
- `SKEY_FRAMESKIP` cycles the value through 0..3 (`cgdoom/st_stuff.c:496-503`).

### 2.5 `I_GetTime` (`cgdoom/i_system.c:158-163`) [V]

- It returns `RTC_GetTicks()/2`, which is a **64 Hz** clock, not 35 Hz (the comment says "1/70 ~ 2/128").
- It is used only by the screen wipe loop (`cgdoom/d_main.c:234-247`) and `G_CheckDemoStatus`'s timedemo. The melt wipe therefore runs about 1.8× faster than vanilla. This is cosmetic.

### 2.6 FPS counter and profiler [V]

- FPS counter (`cgdoom/i_system.c:263-284`): every 16 tics, `fps = 128*16 / (ΔRTC * (Frameskip+1))`, which is the rendered FPS. It is shown by an `hu_stext` widget at x = `SCREENWIDTH-44` (`cgdoom/hu_stuff.c:233-238`).
- Profiler (`:321-336`): `SKEY_PROFILER` resets the five `prof_t` counters in `CGD_Perf` (DynamicAllocation, GraphicsRendering, DisplayInterface, LumpLoading, UnalignedLumpLoading; `cgdoom/cgdoom.h:126-134`). After 40 tics it prints them as a HUD message (`cgdoom/st_stuff.c:515-525`).

---

## 3. Every platform dependency and its gint/fxlibc replacement

### 3.1 OS syscalls and libfxcg functions [V]

| Call | Sites | Purpose | gint / fxlibc replacement |
|---|---|---|---|
| `GetVRAMAddress()` | `cgdoom/cgdoom.c:587`, `cgdoom/cgdoom-ui.c:154`, `cgdoom/i_video.c:220`, `src-cg/platform.h:72` (`CGDOOM_SCREENS_BASE`) | OS VRAM, 384×216 RGB565 | `gint_vram` (396×224, stride 396, **uncached P2**, allocated by `dvram_init`, `gint/src/render-cg/dvram.c:93-121`). Doom screens should become ordinary `malloc` buffers. |
| `GetSecondaryVRAMAddress()` | `cgdoom/cgdoom.c:589` | Extra 165,888 B used as zone heap | Drop it; use the heap. |
| `EnableColor(1)`, `EnableStatusArea(3)`, `Bdisp_FrameAndColor(…)` | `cgdoom/cgdoom.c:592-595, 827-828`; `cgdoom/cgdoom-ui.c:752-753, 800-801`; `cgdoom/i_video.c:184-187` | OS status bar and frame color | Drop them. gint owns the full 396×224 screen; `dclear(C_BLACK)` covers the border. |
| `Bdisp_PutDisp_DD()` | `cgdoom/cgdoom.c:784, 829`; `cgdoom/cgdoom-ui.c:192, 422, 488, 568, 648, 754, 802`; `cgdoom/i_video.c:199, 221` | Push VRAM to the LCD | `dupdate()`, or the custom strip DMA (§6.5). |
| `Bdisp_AllClr_VRAM()` | `cgdoom/cgdoom.c:770, 810`; `cgdoom/cgdoom-ui.c:402, 451, 559, 598, 713, 786` | Clear to white | `dclear(C_WHITE)` |
| `PrintMini(...)` | `cgdoom/cgdoom.c:367` (in `abort()`) | Abort message | Delete CGDoom's `abort()`. gint/fxlibc provide `abort()`, and gint's `kinit` calls it (`gint/src/kernel/kernel.c:177`). CGDoom's version would call OS syscalls from inside gint. |
| `GetKey(&key)` | `cgdoom/cgdoom.c:370, 786, 832`; `cgdoom/cgdoom-ui.c:425, 502, 507, 571, 651, 758` | Blocking menu input (`KEY_CTRL_*`) | `getkey()` / `getkey_opt(GETKEY_NONE or GETKEY_REP_ARROWS, NULL)` → `key_event_t.key`. Map `KEY_CTRL_UP/DOWN/LEFT/RIGHT/EXE/EXIT/DEL/F1/F6` to `KEY_UP…KEY_EXE/KEY_EXIT/KEY_DEL`. On fx-CG100 there are **no F1–F6** (§7.2). Avoid `GETKEY_MENU`: `gint_osmenu_native` is a no-op on fx-CG100 (`gint/src/kernel/osmenu.c:96-100`). |
| `PRGM_GetKey()` | `cgdoom/cgdoom.c:830`; `cgdoom/cgdoom-ui.c:757` | Wait for key release | `clearevents(); while(keydown_any(…))` or `waitevent` until no key is pressed. |
| `RTC_GetTicks()` | `cgdoom/i_system.c:162, 272, 276`; `cgdoom/cgdoom.c:736, 744, 747, 753` | 128 Hz tick counter | `rtc_ticks()` (`gint/include/gint/rtc.h:52`, also 128 Hz, wraps at midnight). Better: libprof / `prof_time` (µs). |
| `Bfile_OpenFile_OS(path, mode, 0)` | `cgdoom/cgdoom.c:446, 727, 749`; `cgdoom/m_misc.c:112, 136`; `cgdoom/m_menu.c:485` | Open file | `BFile_Open(path, mode)` (`gint/include/gint/bfile.h:115`) or `open()`/`fopen()`. **Always inside `gint_world_switch(GINT_CALL(...))`** (`bfile.h:5-7, 24-26`). |
| `Bfile_ReadFile_OS(fd, buf, size, pos)` | `cgdoom/cgdoom.c:157, 378`; `cgdoom/m_misc.c:160`; `cgdoom/m_menu.c:492` | Read | `BFile_Read(fd, buf, size, whence)` (`bfile.h:148`) or `pread`/`read`, inside a world switch. |
| `Bfile_GetFileSize_OS(fd)` | `cgdoom/cgdoom.c:247`; `cgdoom/m_misc.c:143` | Size | `BFile_Size(fd)` or `lseek`/`fstat` |
| `Bfile_CloseFile_OS(fd)` | `cgdoom/cgdoom.c:466, 751, 799`; `cgdoom/m_misc.c:119, 147, 156, 161`; `cgdoom/m_menu.c:494` | Close | `BFile_Close` / `close` |
| `Bfile_DeleteEntry`, `Bfile_CreateEntry_OS(path, BFILE_CREATEMODE_FILE, &size)`, `Bfile_WriteFile_OS` | `cgdoom/cgdoom.c:436-466`; `cgdoom/m_misc.c:103-119` | Create and write saves, demos, cfg | `BFile_Remove`, `BFile_Create(path, BFile_File, &size)`, `BFile_Write`. Or simply `fopen("wb")`/`fwrite`/`fclose` from fxlibc (gint `fs/fugue` handles create and size), inside a world switch. |
| `Bfile_FindFirst`, `FindNext`, `FindClose`, `Bfile_NameToStr_ncpy` | `cgdoom/cgdoom.c:406-419` | List `*.wad` and `*_demo*.lmp` | `BFile_FindFirst`/`FindNext`/`FindClose` (`bfile.h:287-303`) or `opendir`/`readdir` + `fnmatch`-style filter. Inside a world switch, batched into one switch. |
| `GetOSVersion()` = `(char*)0x80020020` | `src-cg/platform.h:103`, `cgdoom/cgdoom.c:601` | Gate experimental RAM | Drop. gint reads the same address for API selection (`gint/src/kernel/kernel.c:43-54`). On fx-CG100 it is `"01.00"` or `"02.00"`. |
| `malloc`, `realloc`, `free` | `cgdoom/cgdoom.c:19, 36, 38`; `cgdoom/i_system.c:66` (SDL); `cgdoom/r_data.c:562`; `cgdoom/w_wad.c:127` | OS heap (128 KiB on fx-CG50) for temporary buffers | fxlibc `malloc` → gint `kmalloc` arenas. **Disable gint's OS-heap fallback arena on fx-CG100**: gpsp notes the `0x80020070` syscall gate is unsupported there [D: gpsp `ports/fxcg100/README.md:115-116`]. |
| libc: `qsort`, `sprintf`, `vsnprintf`, `atoi`, `strchr`, `strcmp`, `strncmp`, `strncpy`, `strcpy`, `strcat`, `strlen`, `memcpy`, `memset`, `memmove`, `memcmp`, `abs` | many | — | All present in fxlibc (`fxlibc/src/stdlib`, `src/stdio`, `src/string`). CGDoom's `strnicmp` (`cgdoom/cgdoom.c:45-55`) can stay or become `strncasecmp`. |
| `printf` | engine-wide | Debug | Already `#define printf(...)` (`src-cg/platform.h:104`); keep that. |

### 3.2 Direct hardware and memory accesses [V]

| Access | Where | What | Replacement on gint / fx-CG100 |
|---|---|---|---|
| **R61524 LCD**: PRDR `0xA405013C` bit 4 (RS), data/index port `0xB4000000`, regs `0x200/0x201/0x202/0x210-0x213` | `cgdoom/i_video.c:66-100` (`SelectLCDReg`, `WriteLCDReg`, `SetWindow`); `I_Flip` `:108-157` | Direct 16-bit CPU writes of palette-converted pixels into a 320×200 window at (38,12). Windows are reset in `I_ShutdownGraphics` (`:164-167`) and `I_ReinitAfterError` (`:169-175`). | `r61524_start_frame(xmin,xmax,ymin,ymax)` (handles old R61524 and the new DCS controller; `gint/src/r61524/r61524.c:136-176`), then DMA (`dma_transfer_async(0, DMA_32B, blocks, src, DMA_INC, (void*)0xb4000000, DMA_FIXED, GINT_CALL_NULL)`) or CPU writes. Or simply `dupdate()`. Restore the full window with `r61524_win_set(0,395,0,223)` before any `dupdate()`, as gpsp does (`gint_platform.c:80`). |
| **PRAM0** `0xfe200000–0xfe228000` (160 KiB, 32-bit-access-only SPU2 memory) | `src-cg/platform.h:59-60`; `cgdoom/cgdoom-alloc.c` (whole file); fragment table `cgdoom/cgdoom.c:94-99, 748`; wipe buffers `cgdoom/i_system.c:129-132` + `SPU2_read16` `cgdoom/f_wipe.c:144-161`; lumpcache `cgdoom/w_wad.c:172`; texture arrays `cgdoom/r_data.c:474-480, 555, 578` | Extra RAM | **Drop.** Use `malloc` and turn `CGD_PRAM_*` into `Z_Malloc`/`Z_Free` wrappers. gint can host a PRAM0 kmalloc arena (commit `d8005b5`, `gint/src/kmalloc/arena_gint.c:439`), but it needs SPU clocks (`gint/src/spu/spu.c:57-61`) and is untested on fx-CG100 [G]. It is unnecessary with about 5 MiB of `_ld1`. |
| **Flash scan**: `0xA0000000–0xA2000000` (uncached), `0x80000000–0x82000000` (cached), hint `0xA0C00000` | `src-cg/platform.h:10-25`; `cgdoom/cgdoom.c:207-349` | Memory-map the WAD (§4) | Use `Bfile_GetBlockAddress` per 4 KiB block in **one** world switch, convert P2→P1 (`addr - 0x20000000`), reject non-P1/P2 results [D: gpsp `nor_rom.c:182-214, 375-405`]. Keep the sector scan only as a fallback. Reading flash directly needs no world switch. |
| **XRAM** `0xe5007000` as a 4 KiB BFile read buffer | `cgdoom/cgdoom.c:243` | Fast buffer for sector memcmp | Not needed with GetBlockAddress. If kept, note that gint manages on-chip memories (`gint_set_onchip_save_mode`, `gint/include/gint/gint.h:128`) and that BFile writes into XRAM happen in the OS world [G]. XRAM/YRAM are better used as DMA strip buffers (§6.5). |
| **System stack** `0xac0f0000`, 448 KiB (P2, uncached) | `cgdoom/cgdoom.c:717`; `cgdoom/os.h:3-8`; `cgdoom/i_system.c:76-82`; also the FLASH_INDEX build area `cgdoom/cgdoom.c:737` | Zone 0 | **Do not touch on fx-CG100.** gint deliberately creates no `_ostk` arena there (`gint/src/kernel/kernel.c:126-139`). Use `malloc`. |
| **"2 MB line"** `0x8c200000` (cached) / scan `0xac200000` up to 6 MiB | `cgdoom/cgdoom.c:351-363, 685-688`; `cgdoom/i_system.c:90-95`; `src-cg/platform.h:79-82` | Experimental extra zone (+~3 MB) | On fx-CG100 this exact region is MPM's free RAM, given to gint as arena `_ld1` (`mpm/main.c:345-357` → `gint/src/kernel/kernel.c:142-166`). Use it through `malloc`/`kmalloc_max`. |
| **User-stack leftover** `[sextra, eextra)` | `cgdoom/i_system.c:83-89` (libfxcg-fork linker symbols) | Zone 1 (~200 KiB per comment) | gint already turns unused static RAM into the `_uram` arena (`gint/src/kernel/kernel.c:115-124`). |
| **KEYSC** `0xa44b0000`, 6 × u16 | `cgdoom/cgdoom-kbd.c:155-160` | Raw key matrix, one byte per row (row index `^1` for big-endian u16 packing, `:166`) | gint keyboard: `clearevents()` then `keydown(KEY_x)`, or `pollevent()`. CGDoom `KEYCODE_*` values equal gint `KEY_*` on the fx-CG50 layout (e.g. `KEYCODE_F1=0x91=KEY_F1`, `KEYCODE_ACON=0x07=KEY_ACON`, `KEYCODE_PLUS=0x24=KEY_ADD`; compare `cgdoom/cgdoom-kbd.h:93-142` and `gint/include/gint/keycodes.h`). The fx-CG100 needs new keymaps (§7.2). KEYSC is at the same address on fx-CG100 (MPM reads `0xa44b0000`, `mpm/main.c:192-195`), but don't bypass gint. |
| **TMU0** `0xa4490004/08/0c/10` + CPG `0xa4150000/24/50` | `src-cg/libprof.h:26-29`; `src-cg/libprof.c:3-41` | Profiler and rate limiter | `#include <gint/prof.h>` (same API: `prof_init`, `prof_make`, `prof_enter`, `prof_leave`, `prof_time`; picks a free TMU, uses `clock_freq()->Pphi_f`; `gint/include/gint/prof.h`, `gint/src/prof/prof.c`). For frame pacing, prefer `timer_configure(TIMER_ANY, 28571, GINT_CALL(cb))` + `sleep()` over busy-waiting. |
| `synco` inline asm | `cgdoom/i_video.c:66` | LCD ordering | Gone with the gint driver. |
| `CGD_sector_memcmp` (SH4 asm) | `src-cg/cgdoom-asm.s:1-105` | Fast 512-byte flash compare (16-bit reads, 2 cycles per compare) | Only needed if the sector-scan fallback is kept. It is gint-compatible plain SH-4 code (it uses `.align 4`; under gint use `.section .text` and keep the `_` prefix). |

### 3.3 Things that disappear entirely under gint

- The `fuck`/`I_Quit` exit path just returns from `main` → gint `kquit` → MPM. `gint_setrestart` is disabled on fx-CG100 (`gint/src/kernel/start.c:47`).
- `I_Init`/`I_Restore` are already empty (`cgdoom/i_system.c:141-152`).

---

## 4. WAD access design

### 4.1 Two methods (`cgdoom/cgdoom.h:51-59`) [V]

- **`CGDOOM_WAD_BFILE` (0)**:
  - Every lump read is `Bfile_ReadFile_OS(gWADfd, buf, size, pos)` (`cgdoom/cgdoom.c:375-378`).
  - `FindInFlash` returns 0 (`:325-326`), so **every** lump is copied into the zone heap.
  - It is slow and needs the most RAM. This is the only mode on SDL2 (`cgdoom/cgdoom.c:674-678`).
- **`CGDOOM_WAD_MMAP` (1, default)**: CGDoom finds the physical flash location of each file fragment once at startup. Lumps that are contiguous in flash are then used **in place** (pointer into flash); the others are copied.

### 4.2 Building the map (fragment search) [V]

1. **Index** (`cgdoom/cgdoom.c:731-745`, only with `#define FLASH_INDEX`, `src-cg/platform.h:39`):
   - Every 4 KiB-aligned sector between `FLASH_FS_HINT=0xA0C00000` and `FLASH_END=0xA2000000` is recorded as `{sector ptr, first u32}`.
   - That is `FLASH_INDEX_SIZE = 0x1400000/4096 = 5120` entries × 8 B = 40,960 B. The comment says "4096", which is wrong.
   - The index is stored temporarily at `SystemStack` (0xac0f0000) and `qsort`ed by first word (`IndexCompareSectors`, `:174-179`).
2. **File read** through a 4 KiB BFile cache (`FileAccessCache`, `:146-171`, `FLASH_BFILE_UNIT=4096`, buffer at XRAM `0xe5007000`, `:242-245`). The file is walked in 512 B units (`FLASH_PAGE_SIZE`, `src-cg/platform.h:30`).
3. **`FindSectorInFlash`** (`:207-237`):
   - First a binary search in the index on the first 4 bytes (`IndexSearchSector`, `:182-204`), comparing candidates with `CGD_sector_memcmp` (full 512 B) or `memcmp` (short last sector).
   - Then a linear scan of **all 65,536 sectors**, starting at `FLASH_FS_HINT` and wrapping to `FLASH_START`.
   - It returns the sector index (0..65535).
4. **`CreateFileMapping`** (`:239-306`):
   - For each new fragment it records `{flash_address:16, file_address:16}` (a 4-byte volatile bitfield in PRAM0, `:94-99`).
   - It then extends the fragment while the next 512 B of the file equals the next 512 B in flash (`:279-301`).
   - Up to `MAX_FRAGMENTS=2048` (`:105`). The comment notes Ultimate Doom WADs with up to about 500 fragments.
   - Error codes: −1 read error, −2 "Page not found", −3 "File too fragmented" (`:755-766`).
   - Progress bar every 5% (`:258-261`).
   - Limits: the 16-bit fields cap both flash and file at 65,536 × 512 B = 32 MiB.
5. Note on duplicate content: an identical 512 B sector elsewhere in flash (e.g. one full of zeros) can be matched instead of the real one. The data is identical, so it is harmless unless that other copy is later erased [G].

### 4.3 Lookup (`FindInFlash`, `cgdoom/cgdoom.c:323-349`) [V]

- Binary search the fragment table by file sector (`FindFragmentInMap`, `:309-321`).
- Return `*buf = FLASH_CACHED_START (0x80000000) + flash_sector*512 + offset_in_fragment`, which is a **P1 cached** pointer. The comment says cached is much faster for in-game loads; uncached is better for the scan.
- The return value is the number of bytes available before the fragment ends.
- `Flash_ReadFile` (`:375-397`) loops `FindInFlash` + `memcpy` to gather a range across fragments.

### 4.4 Lump cache: `W_CacheLumpNum*` flow (`cgdoom/w_wad.c:283-407`) [V]

```
W_CacheLumpNum{Const,Patch}/W_CacheLumpName{Const,Patch}  → iEnableFlash = 1
W_CacheLumpNumVolatile/NameVolatile                         → iEnableFlash = 0  (only P_LoadThings, cgdoom/p_setup.c:323)
  W_CacheLumpNumCommon (:340-361):
    if lumpcache[lump]==NULL → W_ReadLumpWithZ_Malloc
    else Z_ChangeTag(lumpcache[lump], tag)        // no-op for flash pointers (cgdoom/z_zone.c:452)
  W_ReadLumpWithZ_Malloc (:283-334):
    c = FindInFlash(&lumpcache[lump], size, position)   // writes the flash ptr straight into lumpcache
    full    = (c == size)                               // lump lies inside one fragment
    aligned = !((uintptr_t)ptr & 1) || TrustUnalignedLumps   // NB: only 2-byte alignment is tested
    if(!full || !aligned): Z_Malloc(size, tag, &lumpcache[lump]); W_ReadLump()   // copy (gathers fragments)
    else: keep the flash pointer ("referenced")
```

- `PTR_TO_FLASH(x)` (`src-cg/platform.h:23-25`) is true for both the `0xA0…` and `0x80…` flash ranges.
  - `Z_Free` and `Z_ChangeTag2` ignore such pointers (`cgdoom/z_zone.c:145-146, 452-453`).
  - Flash pointers in `lumpcache` are never cleared.
- `lumpcache` is `numlumps*4` B in PRAM0 (`cgdoom/w_wad.c:172`). It falls back to the zone if PRAM0 is full (`cgdoom/cgdoom-alloc.c:73`).
- All engine reads of multi-byte WAD fields go through byte-wise helpers, so lumps at odd addresses are safe on SH4:
  - `SHORT(x)` → `SHORTFromBytes(&x##LE)` (`cgdoom/m_swap.h:44`, `cgdoom/m_swap.c:58-64`).
  - `INTFromBytes` (`:66-76`).
  - The `patch_t` fields were renamed `widthLE` / `columnofsLE` etc. (`cgdoom/r_defs.h:358-362`).
  - Column, post, flat, colormap and REJECT data are byte arrays.
- **Fragmented full-screen pictures** (TITLEPIC, CREDIT, HELP, INTERPIC, BOSSBACK…) are drawn without loading:
  - `CGD_Frag_Map` builds up to 33 `{ptr,size}` pieces (`cgdoom/cgdoom-frag.c:6-36`, `cgdoom/cgdoom-frag.h:28`).
  - `V_DrawFullscreenPatchFragmented` reads each pixel through `CGD_Frag_u8` (linear fragment walk per byte; `cgdoom/v_video.c:253-276`; `cgdoom/cgdoom-frag.c:38-69`).
  - This is very slow, and runs every displayed frame on the title page (`cgdoom/d_main.c:314-318`). It also has an off-by-one bug (B4).
- The developer-info screens (`cgdoom/cgdoom.c:767-787, 807-833`) report fragments, index hits, lowest fragment, index and map build time, and lumps loaded / referenced / unaligned with their totals (`CGDoom_Stats`, `cgdoom/cgdoom.h:142-165`).

### 4.5 What stays in ROM and what goes to RAM [V for mechanism; amounts depend on fragmentation]

**Referenced in ROM (zero heap) whenever the lump lies in one fragment:**
- COLORMAP: 8,704 B, `PU_STATIC` (`cgdoom/r_data.c:620-633`).
- PLAYPAL: `cgdoom/d_main.c:204`, `cgdoom/st_stuff.c:849`.
- PNAMES, TEXTURE1/2 during init (`cgdoom/r_data.c:438-461`).
- All wall patches through `R_GetColumn` for single-patch columns (`cgdoom/r_data.c:383-384`).
- All sprites (`cgdoom/r_things.c:412`; `R_InitSpriteLumps` touches all of them, `cgdoom/r_data.c:593-612`).
- All flats (`cgdoom/r_plane.c:408`).
- Status bar, HUD font and intermission patches (`PU_STATIC` loads in `cgdoom/st_stuff.c:944-1008`, `cgdoom/hu_stuff.c:197`, `cgdoom/wi_stuff.c:1003-1112`).
- Menu graphics (`cgdoom/m_menu.c`).
- REJECT (`cgdoom/p_setup.c:695`).
- Raw map lumps read once at level load: VERTEXES, SECTORS, SIDEDEFS, LINEDEFS, SSECTORS, NODES, SEGS (`cgdoom/p_setup.c:136-482`, each `Z_Free(data)` after conversion).
- Demos from the WAD (`cgdoom/g_game.c:1360`).

**Always in RAM:**
- All converted level structures (`vertexes`, `sectors`, `sides`, `lines`, `segs`, `subsectors`, `nodes`; `PU_LEVEL`), BLOCKMAP (explicit `Z_Malloc` + `W_ReadLump`, byte-swapped in place, `cgdoom/p_setup.c:489-514`), `blocklinks`, and thinkers/mobjs.
- THINGS (Volatile copy, freed).
- Multi-patch composite textures (`R_GenerateComposite`, `cgdoom/r_data.c:215-264`, `PU_CACHE`).
- `texturecolumnlump`/`texturecolumnofs` (2 B × width per texture, `cgdoom/r_data.c:533-534`), `texture_t`s, sprite width/offset tables, `lumpinfo` (16 B × numlumps).
- Any lump crossing a fragment boundary: it is copied on demand. `PU_CACHE` copies are purgeable; `PU_STATIC` copies (status bar etc.) are permanent.

**Savings:** essentially the whole graphics working set (textures, sprites, flats, HUD). Vanilla DOS Doom needs several MiB for that; CGDoom runs Doom/Ultimate in about 0.8 MiB of zone heap plus PRAM0 [V for heap sizes, §5; G for the vanilla comparison].

### 4.6 What changes on the fx-CG100

- **Storage size** [D]: storage memory is 4,793,072 bytes (about 4.57 MiB; HP Museum forum post `https://www.hpmuseum.org/forum/post-203451.html`; not official). doom1.wad v1.9 is 4,196,020 B / 1,264 lumps [G: well-known figures, not checked locally], so it fits next to MPM and a ~350 KB g3a. doom2.wad (14,604,584 B) and doomu.wad (12,408,292 B) do not fit. Plan for shareware plus split or stripped IWADs (the `doomu1..4.wad` support is already there: `cgdoom/cgdoom.c:701-708`, `cgdoom/d_main.c:471-492`). Freedoom phase 1/2 (28.8 MB each) also do not fit.
- **Mapping API** [D]:
  - Use `Bfile_GetBlockAddress(fd, offset, &addr)`: fx-CG100 OS 2.00 address `0x80333cf2`; fx-CG50 syscall `0x1DAA` (gpsp `ports/fxcg100/gint-gpsp/src/nor_rom.c:13-44, 157-198`).
  - Call it once per 4 KiB block, all in **one** `gint_world_switch`. Each world switch while overclocked restores the OS clock and loses RTC time (gpsp `README.md:100-104`).
  - Returned addresses are P2 (`0xA…`, e.g. `0xA1B97000` seen on a real fx-CG100). Convert them to P1 for cached reads.
  - Some blocks come back as TLB-mapped P0 addresses (`0x001129xx`) that fault later. Reject those and fall back to BFile reads (`nor_rom.c:375-405`).
  - `ocbi` freshly mapped P1 lines once (`nor_rom.c:413-431`).
- **Fallback when everything fits in RAM**: with about 5 MiB `_ld1`, the 4.2 MB shareware WAD could even be loaded whole into RAM [G: tight, but it removes every flash and Fugue concern]. Keep that as an option.
- **Keep "delayed writes"** (§7.4): Fugue relocates live records when another file is created or written (gpsp `nor_rom.c:413-415`), which invalidates the map.

---

## 5. Memory

### 5.1 Zone allocator (`cgdoom/z_zone.c`) [V]

- Vanilla `Z_Malloc` extended to **multiple zones**: up to `ZONE_MAX=16` (`cgdoom/z_zone.h:66`), registered with `Z_AddZone(buf, size)` (`cgdoom/z_zone.c:110-134`).
- `Z_Malloc` uses a next-fit rover that also rotates across zones (`zone_rover`). The **last zone is kept out of the rotation** to absorb large blocks and reduce fragmentation (`:66-71, 300-321, 363-364`).
- On failure it calls `I_Error("Z_Malloc(%d) failure")` and returns NULL (`:302-307`). `I_Error` is not fatal (B1).
- Other details:
  - `MINFRAGMENT=64`.
  - `memblock_t` is 24 B on SH4 (`cgdoom/z_zone.h:69-77`).
  - `ZONEID=0x1d4a11`.
  - `user > 0x100` means owned (`:171`).
  - `CGD_Stats.MemoryAllocated` counts cumulative bytes, not peak (`:231`).
- `Z_FreeMemory(zone)` and `Z_LargestFreeBlock(zone)` (`:475-534`) feed `CGD_FreeMem` (`SKEY_FREEMEM`, `cgdoom/st_stuff.c:477-494`) and the `Z_*` error screen (`cgdoom/cgdoom-ui.c:733-750`).

**Zones on the fx-CG50** (`I_ZoneBase`, `cgdoom/i_system.c:74-106`):

| # | Region | Size | Note |
|---|---|---|---|
| 0 | `SystemStack` = **`0xac0f0000` (P2 uncached)** | `SYSTEM_STACK_SIZE = 512K-64K = 458,752 B` (`cgdoom/os.h:3-7`) | Uncached zone; performance bug P3 |
| 1 | `[sextra, eextra)` | "~200 KiB" (comment; linker-script dependent) | User RAM leftover |
| 2 (opt.) | `0x8c200000` (P1) | `CGD_2MBLineMemory` = 4 KiB-rounded run of zero longwords probed every 16 B from `0xac200000`, ≤ `6<<20` (`cgdoom/cgdoom.c:351-363`, `src-cg/platform.h:82`) | Only with "Use experimental RAM" on OS ≤ 3.60; "+3 MB mostly" (commit `b536a8e`) |
| last | `SecondaryVRAM` | `384*216*2 = 165,888 B` | Only with `CGDOOM_DIRECT_R61524`; screens then live in main VRAM (`src-cg/platform.h:66-76`) |

- **SDL2**: one 8 MiB `malloc` zone (`cgdoom/i_system.c:61-69`).

**Other RAM:**
- PRAM0 (§5.2).
- OS `malloc` heap: `CGD_malloc` for WAD-directory parsing `numlumps*16` B (`cgdoom/w_wad.c:109`), `patchlookup` (`cgdoom/r_data.c:441`). The engine comment says "only 128 KB heap" (`cgdoom/r_data.c:560`).
- BSS in the 512 KiB user RAM, including:
  - `visplanes[128]` × 664 B ≈ 85 KB (`cgdoom/r_plane.c:52-53`, `cgdoom/r_defs.h:455-475`).
  - `openings[320*64]` = 40,960 B (`cgdoom/r_plane.c:59-60`).
  - `drawsegs[256]`, `vissprites[128]`.

### 5.2 PRAM0 allocator (`cgdoom/cgdoom-alloc.c`, header essay `cgdoom/cgdoom-alloc.h:6-45`) [V]

- PRAM0 = `0xfe200000..0xfe228000` = 163,840 B; it allows only 32-bit accesses.
- The allocator is a trivial first-fit doubly-linked list with split (minimum remainder 32 B) and merge with neighbours.
- Node header: `{prev, next, size:24, free:8}` as a `volatile` struct (12 B), built with `-fstrict-volatile-bitfields`.
- `CGD_PRAM_Malloc` rounds up to 4 and **falls back to `Z_Malloc(size, PU_STATIC, 0)`** (`:73`).
- `CGD_PRAM_Free` sends non-PRAM pointers to `Z_Free` (`:84`).
- `CGD_PRAM_Zalloc` uses a manual 32-bit loop (`:92-98`), so GCC cannot turn it into `memset` (commit `7d5a81d`).

**Users:**
1. WAD fragment table: 4 B × fragments, at the start of PRAM0.
2. `lumpcache` (4 × numlumps).
3. Wipe buffers `screens[2]`, `screens[3]`: 2 × 64,000 B, read with `SPU2_read16` (`cgdoom/f_wipe.c:144-161`).
4. Seven per-texture arrays: `numtextures × 4` each, plus `texturetranslation` and `flattranslation` (`cgdoom/r_data.c:474-480, 555, 578`).

### 5.3 RAM needed (doom1 vs doom2)

- **Not directly measurable without hardware [G].** From the source:
  - Without experimental RAM the zone is about 448 KiB + ~200 KiB + 162 KiB ≈ 0.8 MiB, plus whatever of PRAM0 is left after the ~125 KiB wipe buffers.
  - README (`README:20-26`): `doom.wad`, `doomu.wad` and the split `doomu1-4.wad` are fine; **`doom2.wad` "should be played with Use experimental RAM"** (about +3 MiB) and "risk[s] running out of memory" otherwise.
  - `G_RecordDemo` buffer: 64 KiB, or 128 KiB with experimental RAM (`cgdoom/g_game.c:1299-1300`).
- **Host measurement [V, but 64-bit pointers inflate structures]:** SDL2 build, BFILE mode (every lump loaded into the zone as `PU_CACHE`), Freedoom 0.13 `freedoom1.wad` (3,163 lumps), E1M1 after 300 tics. `Z_FreeMemory(-1)` = 7,109,088 of 8,388,608 B, so about **1.25 MiB** is non-purgeable on arm64. On 32-bit SH4 with MMAP it will be noticeably smaller.
- **Per-WAD fixed costs [V formula]:**
  - `lumpinfo` = 16 B × numlumps in the zone. doom1 (1,264 lumps): 20 KB; doom2 (2,919 lumps): 46.7 KB [G for lump counts].
  - **It is allocated twice** (B3).
  - `lumpcache` = 4 B × numlumps.
- **On fx-CG100:** MPM passes `[0x8c200000, loadAddress)` as free RAM, where `loadAddress = (0x8c7f0000 - binsize) & -0x10000` (or `0x8c770000 - …` if no OS app buffer; `mpm/main.c:208-230, 345-357`). For a ~350 KiB binary that is about 5.5 MiB, of which gint's VRAM (≈177.5 KB) is also taken. gpsp only uses `0x8c200000..0x8c655300` (≈4.33 MiB) as "hardware-proven" [D: gpsp `README.md:109-114`]. Either way, this is 4–5× what CGDoom had without experimental RAM. **Doom fits comfortably; drop every trick.**

### 5.4 "Experimental RAM" option [V]

- UI checkbox "Use experimental RAM" (`cgdoom/cgdoom-ui.c:636-638`). It is shown only if the OS is ≤ 3.60 (`cgdoom/cgdoom.c:600-605`) and saved as `ExperimentalMemory`.
- When enabled: `FindZeroedMemory(0xac200000)` (`cgdoom/cgdoom.c:686`) adds a zone at `0x8c200000` and doubles the demo buffer.
- Risk: newer OS versions use that RAM, hence the version gate.
- Not applicable on fx-CG100; that region is the loader's free arena there.

---

## 6. Rendering

### 6.1 Frame buffers [V]

- Doom renders 8-bit palettized pixels at **320×200** (`SCREENWIDTH`/`SCREENHEIGHT`, `cgdoom/doomdef.h:97-100`; `RANGECHECK` on at `:79`).
- `I_ScreenBase` (`cgdoom/i_system.c:110-135`) carves the screens out of `CGDOOM_SCREENS_BASE`. In direct mode this is the **main VRAM through its cached P1 alias**, `(GetVRAMAddress() & 0x1fffffff) | 0x80000000` (`src-cg/platform.h:70-73`). This gave a 3.4× faster `I_Flip` (commit `624df28`).

| Screen | Location | Size | Use |
|---|---|---|---|
| `screens[0]` | base + 0 | 64,000 B | Main view |
| `screens[1]` | base + 64,000 | 64,000 B | Background, intermission |
| `screens[4]` | base + 128,000 | 320×32 = 10,240 B | Status bar background (set in `ST_Init`, `cgdoom/st_stuff.c:1181`) |
| `screens[2]` | PRAM0 | 64,000 B | Wipe start |
| `screens[3]` | PRAM0 | 64,000 B | Wipe end |

- The whole 165,888 B VRAM doubles as the **savegame buffer** (`SAVEGAMESIZE=384*216*2`, `cgdoom/g_game.c:70-71`; `save_p = savebuffer = screens[0]`, `:1036`).
- The wipe keeps the buffers row-major instead of vanilla's column-major transpose, to avoid a temporary 64 KB buffer (`cgdoom/f_wipe.c:119-127, 164-226`).

### 6.2 Palette (`I_SetPalette`, `cgdoom/i_video.c:39-62`) [V]

- 256-entry `uint16_t gPaletteData[256]`.
- Per entry: `r=gamma[R]>>3, g=gamma[G]>>2, b=gamma[B]>>3; (r<<11)|(g<<5)|b` (RGB565).
- `pal==NULL` re-applies gamma after `SKEY_GAMMA` (`cgdoom/st_stuff.c:505-513`).
- Damage, pickup and radsuit palettes come from `ST_doPaletteStuff`: `PLAYPAL + palette*768`, `cgdoom/st_stuff.c:849`.

### 6.3 Presenting the frame [V]

**Direct mode (`CGDOOM_DIRECT_R61524`, defined at `src-cg/platform.h:51`):**
- First frame after init or an error (`cgdoom/i_video.c:112-141`):
  - Window = full 396×224.
  - Write 12 black lines, then 200 × (38 black + 320 pixels + 38 black), then 12 black lines (88,704 writes).
  - Then set the window to `(38,12)`, 320×200.
- Steady state (`:142-154`): reset GRAM X/Y, select GRAM, then **64,000 CPU writes** of `gPaletteData[screens[0][j]]` to `0xB4000000`, unscaled and centered.
- `I_ShutdownGraphics` sets the window back to (6,0) 384×216, which is the OS view.

**Fallback (`:179-202`):**
- Convert into the OS VRAM at offset (32,8) of the 384×216 buffer, then `Bdisp_PutDisp_DD()`.
- The border is black because of `Bdisp_FrameAndColor(3,17)`.

**General:** no scaling at all; the 320×200 image is centered on the 396×224 panel.

### 6.4 Renderer hot paths [V]

- `R_DrawColumn` (`cgdoom/r_draw.c:104-138`): vanilla. `dest=ylookup[]+columnofs[]`, `*dest = dc_colormap[dc_source[(frac>>16)&127]]`, `dest += 320`.
- `R_DrawSpan` (`:410-452`): vanilla 64×64 flat stepping.
- Fuzz and translated columns are vanilla.
- No low-detail mode: `detailshift=0` is forced (`cgdoom/r_main.c:590`).
- View size: `R_SetViewSize(10)` by default (`cgdoom/r_main.c:676`). `SKEY_DECVP`/`INCVP` change it (`cgdoom/i_system.c:221-233`). `V_Clear()` clears 320×168 on resize (`cgdoom/v_video.c:402-405`).
- Wall columns, sprite posts and flats are read **directly from flash** via P1 when referenced (§4.5).
- `src-cg/cgdoom-asm.s` contains **only** `CGD_sector_memcmp`, a weave-scheduled 16-bit-read compare of two 512 B sectors (returns 0 if equal, 1 otherwise). The first 32 bytes are checked separately for early exit; then 30 iterations of 16 bytes each. It is used only for WAD mapping. No rendering code is in assembly.

### 6.5 Recommended gint presentation path (fx-CG100) [G, based on gint and gpsp]

**Simplest option:**
- Convert into `gint_vram + 396*12 + 38` (stride 396), then `dupdate()`.
- That is 64,000 uncached stores + 177,408 B of DMA per frame.
- `dupdate` waits for DMA unless triple buffering is set with `dsetvram` (`gint/src/render-cg/dupdate.c:19-32`).

**Best option** (what gpsp does on a real fx-CG100, `gint_platform.c:39-50, 259-275`):
- Call `r61524_start_frame(38, 357, 12, 211)` once per frame.
- Convert 320×N rows (N a multiple of 4, e.g. 8 → 5,120 B) into an on-chip XRAM/YRAM strip (`0xe5007000` / `0xe5017000`).
- `dma_transfer_async(0, DMA_32B, 320*N*2/32, strip, DMA_INC, (void*)0xb4000000, DMA_FIXED, GINT_CALL_NULL)`.
- Convert the next strip into the other bank while the DMA runs.
- That is 128,000 B on the bus instead of 177,408 B, and no uncached-RAM traffic.
- Caveats:
  - gint's own comment says full-width windows are needed for DMA (`gint/include/gint/drivers/r61524.h:71-73`), but gpsp DMAs partial-width windows on hardware [D].
  - Restore the window (`r61524_win_set(0,395,0,223)`) before any `dupdate()`.
  - The OS paints its BFile busy icon into GRAM during world switches, so redraw the border afterwards (gpsp `gint_platform.c:58-61, 283-310`). CGDoom already calls `I_ReinitAfterError()` after file I/O (`cgdoom/m_menu.c:499`, `cgdoom/g_game.c:965`).
- Pixel conversion: load 4 source bytes as one `u32`, do 4 palette lookups, store two `u32`s. The 512 B LUT stays in cache.

**Optional:** a stretched mode (e.g. 320→396 horizontally with a 5:4 LUT, 200→224 vertically) can be done in the same strip stage.

---

## 7. UI, keyboard, saves, demos

### 7.1 Launcher UI (`cgdoom/cgdoom-ui.c`) [V]

- Text uses gint's 8×11 proportional font embedded as bitmaps (`font_width`/`font_glyphs`, `:12-60`), drawn pixel by pixel into the OS VRAM (`SetPixel`, `:62-66`, 384×216).
- An immediate-mode layout system (`Layout`, `:197-393`) with focusable text, checkboxes and integer inputs, driven by `GetKey` `KEY_CTRL_*` codes.
- **Main screen** (`UI_Main`, `:583-700`):
  - One line per WAD with labels like "Play Doom Shareware" / "Play Doom II".
  - "Start at episode" and "Start at map" (integers; editing them forces Title = Skip).
  - Title screen: Skip / No demos / Full.
  - "Use experimental RAM", "Record on demo slot" (None / 0..99), "Replay demo…", "Customize controls…", "Advanced options…".
- **Advanced** (`:395-439`): Developer info, WAD access method (BFile / Memory map), Trust unaligned lumps.
- **Controls editor** (`:441-550`):
  - Two columns of 14 entries; load a default (Thumbs only / Full hands / CGDoom 0.3).
  - EXE then "Press a key…" uses `CGD_PRGM_Getkey` (direct KEYSC) followed by an extra `GetKey` to unbuffer.
  - **F1** = save and quit, **F6** = cancel.
- **Errors** (`UI_Errorv`, `:707-759`): word-wrapped message. For `Z_*` errors it lists each zone's free/total and largest block. Waits for a key.
- **Delayed-write progress** (`UI_DelayedWrites`, `:769-803`).
- **Port**:
  - Reimplement drawing with `dclear`/`dtext_opt`/`drect`/`dupdate` on 396×224, or keep the custom font and draw into `gint_vram` with stride 396.
  - Replace `GetKey` with `getkey()`.
  - Replace F1/F6 in the controls editor (fx-CG100 has no F-keys).

### 7.2 Keyboard (`cgdoom/cgdoom-kbd.c`) [V]

- 27 bindable Doom actions (`CGD_KeymapEntries`, `:10-40`): arrows, use, fire, weapons 1–7, map, pause, strafe L/R, run, and the CGDoom special keys:
  - `SKEY_DECVP`, `SKEY_INCVP`, `SKEY_CHEAT` (god mode + all weapons, ammo and keys, `cgdoom/st_stuff.c:434-462`), `SKEY_NOCLIP`, `SKEY_GAMMA`, `SKEY_FREEMEM`, `SKEY_FPSCOUNTER`, `SKEY_FRAMESKIP`, `SKEY_PROFILER`.
  - Codes 0x101–0x109 (`cgdoom/cgdoom-kbd.h:48-56`).
- Three default keymaps (`:43-131`). `ThumbsOnly` is the default:
  - Arrows: move/turn; x² (SQUARE): use; ALPHA: fire; F1–F6 and 7: weapons; EXIT: map; AC/ON: pause; X,θ,T / log: strafe; tan: run; − / +: viewport; `,`: cheat; →: noclip; Frac: gamma; S⇔D: free mem; `(`: FPS; VARS: frameskip; `)`: profiler.
- Fixed keys (`DoomKeyToKeycode`, `:192-248`):
  - Escape = MENU, Enter = EXE.
  - Letters A–Z on the alpha-key positions (`KeyDown`, `:250-261`); they count only if ALPHA is **not** held, and digits only if ALPHA **is** held.
  - Backspace = DEL.
- The scan happens once per tic in `I_StartTic` (`CGD_UpdateKeyboardState` keeps prev/now 12-byte states, `:263-277`). Edges become `ev_keydown`/`ev_keyup` (`cgdoom/i_system.c:342-375`).
- Two event sets: gameplay (`base_keys`) and text entry for save names (`input_keys`, chosen by `saveStringEnter`).
- Keymaps are saved in `CGDoom.cfg` by technical name (`CGD_*_TechnicalName`, `:283-416`).

**fx-CG100 layout [V, gint]:** rows 9–7 are physically different.

| Row | Keys (left to right) |
|---|---|
| 9 | ON, HOME (=0x84 MENU), PREVTAB, UP, NEXTTAB, PAGEUP |
| 8 | SETTINGS, BACK (=0x74 EXIT), LEFT, OK, RIGHT, PAGEDOWN |
| 7 | SHIFT, ALPHA, VARS, DOWN, CATALOG, TOOLS (=0x82 OPTN) |
| 6 | XOT, FRAC, SQRT, POWER, SQUARE, EXPFUN |
| 5 | COMMA, SIN, COS, TAN, LEFTP, RIGHTP |

- Sources: `gint/include/gint/keycodes.h` (CG-100 section) and `gint/src/keysc/keydev.c:49-56`.
- `keydown(KEY_F1..F6)` is **always false** on fx-CG100 (`keydev.c:56-93`). gint only synthesizes F1–F6 in *events* when CATALOG is held with a row-6 key (`keydev.c:274-277`).
- **Port**: replace `CGD_ScanKeyboard` with `clearevents()` and evaluate bindings with `keydown(KEY_*)`, which goes through gint's per-model reverse map. Define fx-CG100 default keymaps, e.g.:
  - Weapons on the digit keys 1–7.
  - OK = use, SHIFT or ALPHA = fire.
  - PREVTAB/NEXTTAB = strafe, TOOLS = map, PAGEUP = pause, PAGEDOWN = run.
  - HOME = Doom menu (Escape).
  - This needs playtesting.
- Update the display-name tables (`:283-304`) for the fx-CG100 legends.

### 7.3 Settings file [V]

- `\\fls0\CGDoom.cfg`, text `Key=Value`, `CGDOOM_SETTINGS_FILE_VERSION 1`. Written with `M_WriteFile` from a buffer at `CGDOOM_SCREENS_BASE`; read with `M_ReadFile` into the same buffer (`cgdoom/cgdoom.c:473-569`).
- Keys: `Version`, `WADMethod` (`BFILE`/`MMAP`), `DeveloperInfo`, `TrustUnalignedLumps`, `EnableDemos`, `Autostart`, `ExperimentalMemory`, `Keymap.<DoomKey>=<PhysKey>`.

### 7.4 Saves and delayed writes [V]

- Save files are `<wadname>_<slot>.dsg` (slots 0–5) (`cgdoom/g_game.c:1033`; slot listing `cgdoom/m_menu.c:461-500`).
- **Why writes are delayed** (`#define CGDOOM_DELAY_SAVES`, `cgdoom/cgdoom.h:61-62`): writing a file makes Fugue reshuffle storage, which invalidates the WAD flash map. The non-delayed path therefore saves and then quits immediately (`cgdoom/g_game.c:467-474`, "the storage memory is shuffled, we need to leave").
- Delayed path (`G_DoSaveGame`, `cgdoom/g_game.c:1025-1101`):
  - Serialize into `screens[0]` (whole VRAM, max 165,888 B).
  - Copy into a `Z_Malloc(PU_STATIC)` buffer recorded in `CGD_DelayedSaves[slot]` (`cgdoom/cgdoom.h:33-49`).
  - Show a HUD message with the size.
- Loading prefers the pending buffer (`G_DoLoadGame`, `cgdoom/g_game.c:956-965`) and the menu shows pending saves (`cgdoom/m_menu.c:470-477`).
- At exit, `DelayedWriteFile` deletes, creates and opens each file and writes it in 4 KiB chunks with a progress bar (`cgdoom/cgdoom.c:423-467, 801-805`).
- **Port**:
  - Keep the delay (or remap after writing).
  - Do all writes in a world switch, one per chunk so the gint UI can update between chunks. The OS also draws a busy icon.
  - Use a dedicated save buffer, e.g. vanilla `0x2c000` = 180,224 B, instead of the screens.

### 7.5 Demos [V]

- **Recording**: the main-menu slot creates `<wad>_demoNN.lmp` (`cgdoom/d_main.c:560-564`, `G_RecordDemo` `cgdoom/g_game.c:1292-1306`).
  - The buffer is 64 or 128 KiB in the zone.
  - On finish, `G_CheckDemoStatus` puts it into `CGD_DelayedSaves[CGD_DELAYEDSAVES_DEMO=6]` (`cgdoom/g_game.c:1463-1471`).
- **Replay**: "Replay demo…" lists `*_demo*.lmp`. `G_DeferedPlayDemoFile` → `M_ReadFile` into the zone (`cgdoom/g_game.c:1345-1366`). With `CGD_PlayDemoOnly` the game quits when the demo ends (`:1443-1447`).
- Demo versions 108, 109 and 110 are all accepted (`:1369-1377`). Old WAD demos desync, which is why they are off by default (`cgdoom/d_main.c:397-400`).

---

## 8. SDL2 build (`src-sdl2/`) and a macOS host build

### 8.1 How it emulates the platform [V]

**`src-sdl2/emul.c`:**
- `_VRAM`/`_VRAM2` are 384×216 `uint16_t` arrays (`:20-21`). `Bdisp_PutDisp_DD` converts RGB565 to an RGB888 surface and blits it to a 384×216 window (`:64-89`).
- `Bdisp_AllClr_VRAM` fills with 0xff (`:98-101`). `Bdisp_FrameAndColor` is a no-op.
- Keyboard: `CGD_ScanKeyboard` builds the 12-byte matrix state from `SDL_GetKeyboardState` (`:175-195`) using a PC-key table (`MatrixCodeToPCKey`, `:105-173`). For example F1–F6 → F1–F6, SHIFT/OPTN/VARS/MENU → Q/W/E/R, ALPHA → A, EXE → Return, AC/ON → Esc.
- `GetKey` is a blocking `SDL_WaitEvent` loop that maps arrows, Return, F1–F6 and digits; Esc exits (`:205-239`). `PRGM_GetKey` returns 0 (`:197-203`).
- `RTC_GetTicks` uses `CLOCK_MONOTONIC` scaled to 128 Hz (`:243-249`).
- `char _PRAM0[160*1024]` (`:16`).
- Bfile: stdio + `glob()` with a 16-slot fd table (`:265-467`). Paths drop the `\\fls0\` prefix and resolve relative to the current directory. `Bfile_DeleteEntry`/`CreateEntry` only print; `OpenFile(BFILE_WRITE)` really writes via `fopen("wb")`.

**Other files:**
- `src-sdl2/platform.h` (master): `PRAM0_START/END` point at `_PRAM0`, `CGDOOM_SCREENS_BASE = SecondaryVRAM`, `CreateFileMapping`/`FindInFlash`/`FindZeroedMemory` are macros returning 0, `PTR_TO_FLASH(p)=0`, `GetOSVersion()="03.60.0000"`. It also includes `"../../../include/fxcg/file.h"`, which only exists inside a libfxcg tree.
- `src-sdl2/libprof.*`: `clock_gettime(CLOCK_REALTIME)` ns-based `prof_t`.
- `cgdoom/cgdoom.c:674-678` forces BFILE, untrusted lumps and no experimental RAM. `I_ZoneBase` gives 8 MiB (`cgdoom/i_system.c:61-69`).

### 8.2 Experiment on this Mac (arm64, Apple clang 21, GNU make 3.81, Homebrew sdl2-compat 2.32.72) [V]

Steps:
- Exported `origin/in330` into the scratchpad.
- `make all-sdl2` failed with *"No rule to make target build-sdl2/am_map.c.o"*. GNU make 3.81 does not apply the `%/:` directory rule. Fix: `mkdir -p build-sdl2` or use gmake ≥ 4.
- `SDL2_CFLAGS` contains `$(pkg-config sdl2 --cflags)` (`Makefile:47`). That is not `$(shell …)`, so it expands to nothing and `<SDL2/SDL.h>` is not found under `/opt/homebrew/include`. Fix: `$(shell pkg-config sdl2 --cflags)`.
- **All 63 C files compiled with clang.** Only warnings appeared:
  - Clang ignores `-fgcse-sm -fgcse-las -fgcse-after-reload` with a warning.
  - Some unused variables.
  - "`pSrc` uninitialized" in `Flash_ReadFile`, which is unreachable in BFILE mode.
- Linking failed with `ld: open() failed, errno=21 (Is a directory) for 'CGDoom'`. The output `CGDoom` collides with the `cgdoom/` directory on case-insensitive APFS. Fix: rename the output (e.g. `cgdoom-sdl2`).
- The first run on `freedoom1.wad` (headless, `SDL_VIDEODRIVER=dummy`, test hooks added) **aborted** in `R_GenerateLookup`. `byte patchcount[320]` (`cgdoom/r_data.c:289`) is overrun by textures wider than 320; `ASSERT(texture->width <= 320)` fires at `:305`, then the `memset` overflows the stack and macOS FORTIFY traps. On the calculator `ASSERT` is empty (`src-cg/platform.h:102`), so this is **silent stack corruption** and a likely cause of README's "Freedoom phase 1: E1 works, E2 crashes".
- With `patchcount[4096]`, it ran 300 tics of E1M1, and a dumped frame (`shot.png`) shows a correct view with status bar. **The engine is 64-bit-clean enough for a host harness.**
- Cosmetic: `printf("%m")` prints a literal "m" on macOS.

Scratch artifacts:
- `/private/tmp/claude-501/-Users-cheparukhin-casio-doom/30f4ebb0-ca54-41b2-9ef7-f89c23cf57cf/scratchpad/cgdoom-sdl2-test/` (patched tree; `src-sdl2/emul.c` has `CGD_TEST_FRAMES` auto-run and PPM dump hooks).
- `…/runtest2/shot.png`.

### 8.3 What a proper macOS host build needs [V/G]

1. Use in330's `src-sdl2/platform.h`, which inlines the BFILE enums, `Bfile_FileInfo` and prototypes (`git diff master origin/in330 -- src-sdl2/platform.h`).
2. Makefile: `$(shell pkg-config sdl2 --cflags)`, rename the binary, pre-create the build directories (or require gmake), and drop or condition the GCC-only flags.
3. Fix `r_data.c:289` (needed on the calculator too).
4. Optional:
   - An env-driven auto-start or scripted-input mode for CI.
   - A frame-dump hook (CRC per frame, for golden tests of renderer optimizations).
   - Window scaling.
   - A `-m32`-like size check is impossible on arm64 macOS, so track memory with 32-bit structure sizes via `static_assert`s instead.
5. Better long term: make the SDL layer emulate the **new** gint-facing platform API (`I_Flip` → strip writer, keydown-based input, file layer), so the same `i_*.c` runs on both. sdl2-compat over SDL3 works as-is.

---

## 9. `origin/in330` and `origin/in330-backup` [V]

- **What it is.** Teaching material for a course lab ("TP") called **IN330**, written by Sébastien Michelland (Lephe) in Dec 2024.
  - Students port CGDoom to **their own RISC-V emulator**: `rv64i`/`lp64`, freestanding, newlib + libgcc built with `riscv-gnu-toolchain --with-arch=rv64i --with-abi=lp64`.
  - Platform services are implemented as `ecall`s (`src-riscv/syscall.s`): `Bdisp_PutDisp_DD`=1001, `CGD_ScanKeyboard`=1002, `Getkey`=1003.
  - README (French): "no need to touch `cgdoom/`, only modify `src-riscv/`".
- **`origin/in330`** (4 commits on top of master: `be4d350`, `b2a3105`, `c5b723f`, `f233243`):
  - Makes `src-sdl2/platform.h` self-contained (BFile enums, `Bfile_FileInfo`, prototypes, `<stddef.h>`).
  - Reorders link arguments.
  - Adds `CGDoom.x`, a flat-binary linker script (`OUTPUT_ARCH(riscv32)`, `ENTRY(main)`, sections from 0). This is inconsistent with the rv64 README.
- **`origin/in330-backup`** (`66226ad`, `dfbd1be`):
  - Removes the fx-CG files: `src-cg/*`, icons, G3A.
  - Adds `src-riscv/{emul.c, keyboard.hpp, libprof.h, platform.h, syscall.s}` and a RISC-V Makefile target.
- **Relevance.** It confirms the core is meant to be OS-free behind a small platform layer and runs with 64-bit pointers. Its `platform.h` is the cleanest list of what the core needs:
  - `Bdisp_PutDisp_DD`, `Bdisp_AllClr_VRAM`, `GetVRAMAddress`, `GetSecondaryVRAMAddress`, `Bdisp_FrameAndColor`.
  - The Bfile family.
  - `PRGM_GetKey`, `GetKey`, `RTC_GetTicks`, `CGD_ScanKeyboard`.
  - `PRAM0_START/END`, `CGDOOM_SCREENS_BASE`, `PTR_TO_FLASH`.

---

## 10. Known bugs, limitations and TODOs

**Explicit markers [V]:**
- `README:37-41`: "Built-in overclocking?", "Auto frameskip?", "Improve SDL2 build for video capture", "Darken background in menu".
- `src-sdl2/emul.c:199, 446`: `PRGM_GetKey`; `Bfile_FileInfo` fields.
- `cgdoom/hu_stuff.c:250` (FIXME).
- `cgdoom/r_draw.c:210, 318`: detail-mode FIXMEs, dead WATCOM code.
- `cgdoom/p_mobj.c:411, 419`, `cgdoom/p_tick.c:83`: NOP thinker FIXMEs.
- `cgdoom/d_main.c:331, 336`: `map=1 //HACK`, demo numbering FIXME.
- `cgdoom/r_data.c:373-381`: disabled composite-size limit.
- Removed `R_DrawSpriteRange` texturecolumn check (`cgdoom/r_things.c:437-438`, commit `17147b9`, "causes issues in Freedoom E1M5").

**Status per README (`README:28-35`):**
- Plutonia, TNT, Heretic and HellRaiser: not tested.
- Heretic SoSR: some crashes.
- Freedoom phase 1: E1 works, E2 crashes.

**Bugs I found:**

| # | Severity | Where | Bug |
|---|---|---|---|
| B1 | Design | `cgdoom/i_system.c:191-212` | `I_Error` **returns** (shows `UI_Errorv`, then `I_ReinitAfterError`). Callers assume it never returns: e.g. `Z_Malloc` returns NULL after it (`cgdoom/z_zone.c:302-306`), and `W_GetNumForName` continues with −1. Port: make it fatal (exit or longjmp back to the launcher) or audit callers. |
| B2 | High | `cgdoom/r_data.c:289, 305` | `byte patchcount[320]` stack overflow for textures wider than 320 (Freedoom, PWAD skies). **Verified** under macOS FORTIFY. Fix: size it to the maximum width or allocate it. |
| B3 | Medium | `cgdoom/d_main.c:448` and `:514` | `W_InitMultipleFiles()` runs twice. The second call sets `lumpinfo=NULL` and `Z_Malloc`s a new array (`cgdoom/w_wad.c:114, 155`), leaking `16×numlumps` B of `PU_STATIC` zone (doom2 ≈ 46.7 KB, Freedoom 50.6 KB). It also re-reads the directory through BFile or flash. |
| B4 | Low (visual) | `cgdoom/v_video.c:273` | `V_DrawFullscreenPatchFragmented` uses `offset + y + 3 + 3*(y>=h0)`; the post layout requires `+4` (3-byte header + 1 trailing pad). **Verified** with a Python model on Freedoom TITLEPIC/CREDIT/HELP1/HELP2/INTERPIC/WIMAP0/BOSSBACK: 9.7k–16.3k of 64k pixels wrong with `3`, **0** with `4`. The bottom part (rows ≥ h0, usually 128) is shifted by one row. It also assumes exactly two posts per column (true for those lumps). |
| B5 | Low | `cgdoom/i_system.c:158-163` | `I_GetTime` is 64 Hz, not 35 Hz, so wipes are too fast and timedemo is wrong. |
| B6 | Perf | `cgdoom/cgdoom.c:717`, `cgdoom/i_system.c:80` | Zone 0 (448 KiB, first in rotation) is at P2 `0xac0f0000`, so all those heap accesses are **uncached**. gint's `_ostk` uses the cached `0x8c0f0000`. |
| B7 | Low | `cgdoom/w_wad.c:297` | The unaligned-lump test checks `&1` (2-byte), while `cgdoom/cgdoom.h:78-81` says "not 4-aligned". |
| B8 | Doc | `src-cg/platform.h:40-42`, `cgdoom/cgdoom.c:75-76` | The index has 5,120 entries, not 4,096 as the comments say. |
| B9 | Robustness | `cgdoom/cgdoom-frag.h:28` | More than 32 fragments per lump means `CGD_Frag_Map` calls `I_Error`. |
| B10 | Limitation | `cgdoom/r_plane.c:52-60`, `cgdoom/r_defs.h:55`, `cgdoom/r_things.h:31` | Vanilla static limits (128 visplanes, 256 drawsegs, 128 vissprites, 320×64 openings) mean "no more visplanes" errors on detailed PWADs and some Doom II maps. Raise them on fx-CG100. |
| B11 | Limitation | `cgdoom/w_wad.c:151-152` | Only one WAD can be loaded (no PWAD + IWAD); "may not work due to the allocation of lumpcache". |
| B12 | Limitation | — | **No sound** (the I_Sound layer was removed; `cgdoom/cgdoom.c:139-140`). No music. |
| B13 | Limitation | `cgdoom/i_system.c:246-257` | Busy-wait rate limiter (power), no auto frameskip, slow motion when CPU-bound. |
| B14 | Hazard | `cgdoom/g_game.c:1035`, `:1067-1101` | The savegame buffer is the screens area (165,888 B max). An `I_Error` during a save destroys the save (`cgdoom/p_saveg.c:249, 315, 575` comments). |
| B15 | Cosmetic | `src-sdl2/emul.c:351` | `printf %m` is a glibc extension. |
| B16 | Build | `Makefile:47` | `$(pkg-config …)` should be `$(shell …)`. |
| B17 | Build | `src-sdl2/platform.h:50` | Depends on a libfxcg tree path; fixed in in330. |

---

## 11. fx-CG100 facts relevant to this port (from local sources)

**gint:**
- Model detection [V]: model string at `0x80000300` is not `810A`/`811A`/`810F`, so the calculator is an fx-CG100 (`gint/src/kernel/hardware.c:131-145`).
- `HWRAM` = 8 MiB, `HWROM` = 32 MiB as constants (`:155-156`).
- CASIOWIN API selection (`gint/src/kernel/kernel.c:43-54`): OS `01.00` → API 1; `02.00` on fx-CG100 → API 2 (direct call addresses, e.g. BFile_Open `0x803338d0`; `gint/src/kernel/syscalls.S:214-250`). **Any other version (e.g. 2.10) falls back to API 0, the syscall table, which may be unsupported on fx-CG100** [G; MPM `dynload` branch mentions "support OS 2.10"].
- No `_ostk` arena on fx-CG100 (`kernel.c:126-139`). Loader arenas `_ld1.._ld4` come from MPM `load_info` pairs `0x10`/`0x11` (`kernel.c:141-166`, `gint/src/kernel/start.c:180-182`).
- No restart and no OS main menu on fx-CG100 (`start.c:47`, `osmenu.c:96-100`).
- Overclock tables for fx-CG100: F1 = stock `FRQCR 0x0F011112`; F4 = PLL ×32, `Iφ/2` ≈ 235.9 MHz; **F5 is clamped to F4** because "the Graph Math+ cannot handle the higher bus speed" (`gint/src/cpg/overclock.c:450-507`). gpsp runs at 235.93 MHz CPU / 117.96 MHz bus on hardware [D].
- Display: same `r61524` driver, 396×224 RGB565, with the new-controller branch (versions 50/82) [V]. A freestanding R61524 build drew "a single line" on a real fx-CG100, which suggests the new controller [D/G].

**MPM [V]:**
- Loads `\\fls0\*.g3a` (`mpm/main.c:34`) **into RAM**, skipping the 0x7000-byte header. Binary ≤ `0x370000` B (`main.c:212-213`).
- Load address `(0x8c7f0000 - size) & -0x10000` (or `0x8c770000 - …`), mapped at virtual `0x00300000` by 55 × 64 KiB UTLB pages (`main.c:252-263`).
- User RAM `0x08100000` → 512 KiB, either the OS app buffer (`CW_AppMemory`, `0x8c160000`) or `0x8c770000`.
- NULL page → ROM `0xa0000000`.
- Entry `code('MPM0', loadinfo)` with `loadinfo = {1,1, 0x10,0x8c200000, 0x11,loadAddress, 0}` (`main.c:345-357`).
- Consequence: code and `const` tables (e.g. `finesine`, 40 KB; `info.c`) run from RAM, which is fast.

**fxSDK [V]:** the `FXCG50.cmake` toolchain uses `-m4-nofpu -mb -fno-builtin -nostdlib -Wa,--dsp` (`fxsdk/fxsdk/cmake/FXCG50.cmake:20-22`). Add `-m4a-nofpu -O2/-O3` per target. `-fno-builtin` blocks memcpy/memset inlining, so use `__builtin_*` in hot code. Template: `fxsdk/fxsdk/assets/CMakeLists.txt` with `find_package(Gint 2.9 REQUIRED)` and `generate_g3a(TARGET … OUTPUT … NAME … ICONS …)`.

**Storage [D]:** about 4.79 MB user storage on fx-CG100 / Math+ (HP Museum forum; also a Cemetech claim of 4.7 MB on Math+). Not official.

---

## 12. Porting checklist (CGDoom → fxSDK/gint, fx-CG100 first, fx-CG50 as a bonus)

**Build:**
1. Create an fxSDK CMake project. Sources: `cgdoom/*.c` minus `cgdoom-alloc.c` (or keep it as a shim). Add a new `src-gint/` with `platform.h`, `i_video_gint.c`, `kbd_gint.c`, `files_gint.c`, `wadmap_gint.c`.
2. Flags: `-m4a-nofpu -O2` (or `-O3`), `-fstrict-volatile-bitfields` only if PRAM is kept, and the warning set from `Makefile:13`. Link `Gint::Gint`. Generate `CGDoom.g3a` with icons. Check binary ≤ 3.43 MiB (MPM limit; currently about 320 KiB).

**Platform header:**
3. Replace `src-cg/platform.h` with gint includes:
   - `#define printf(...)`, `ASSERT(x)` (debug-enable it on host).
   - Remove `GetOSVersion`, `FLASH_*` constants (move them into `wadmap_gint.c`), `PRAM0_*`, `CGDOOM_DIRECT_R61524`, `CGDOOM_2MBLINEMEMORY_MAX`.
   - `PTR_TO_FLASH(x)` → `((uintptr_t)(x) >= 0x80000000 && < 0x82000000) || (0xa0000000..0xa2000000)`.
4. Replace `src-cg/libprof.*` with `<gint/prof.h>`.
5. Delete `abort()` (`cgdoom/cgdoom.c:365-371`).

**Memory:**
6. `I_ZoneBase`: one or two zones from `kmalloc_max(&size, "_ld1")` (leave about 256 KiB free for gint, fxlibc and file buffers), optionally plus `_uram`.
7. `I_ScreenBase`: `malloc` 64,000 B each for screens 0–3 and 10,240 B for screen 4, in **cached** RAM.
8. `CGD_PRAM_*`: forward to `Z_Malloc`/`Z_Free`/zeroing.
9. `SAVEGAMESIZE` buffer: dedicated `malloc(0x2c000)`.
10. Remove `SystemStack`, `FindZeroedMemory`, the experimental RAM UI and options, and `SecondaryVRAM`.

**Files** (all BFile/fs calls in `gint_world_switch`):
11. `FindFiles` → one world switch running `BFile_FindFirst/Next/Close` (or `opendir`).
12. `M_ReadFile`/`M_WriteFile`/`M_ReadSaveStrings`/`DelayedWriteFile` → fxlibc `fopen`/`fread`/`fwrite`, or `BFile_*`, each wrapped in a world switch.
13. Call `I_ReinitAfterError()` (full LCD redraw) after every switch.

**WAD map** (new `wadmap_gint.c`):
14. Open the WAD, then one world switch to query `Bfile_GetBlockAddress` for all `ceil(size/4096)` blocks. Address `0x80333cf2` on OS 2.00; syscall `0x1DAA` on fx-CG50 via `gint_world_switch` and an `sh` trampoline. **Verify the address/syscall on your exact OS version.**
15. P2→P1, reject other address ranges, `ocbi` + `synco`.
16. Represent the map as an array of block pointers; `FindInFlash` becomes O(1): `ptr = blk[pos>>12] + (pos&4095)`, available = up to the end of the physically contiguous run.
17. Lumps fully inside a contiguous run are referenced; others are copied with `Flash_ReadFile`, which gathers from block pointers or falls back to a world-switched BFile read.
18. Keep the old sector scan as a fallback behind an option. It needs `CGD_sector_memcmp` and a 32 MiB scan; the fx-CG100 flash layout is unverified [G].
19. Option: "load whole WAD into RAM" when `size + ~1.5 MiB` fits.

**Video:**
20. Delete the direct-R61524 block (`cgdoom/i_video.c:64-175`). Implement `I_Flip` with the XRAM/YRAM strip DMA design (§6.5) or `dupdate()`, and `I_ShutdownGraphics`/`I_ReinitAfterError` with `r61524_win_set(0,395,0,223)` + `dclear` + `dupdate`.
21. Keep `I_SetPalette` (RGB565 already matches gint `C_RGB` packing).
22. UI: port `cgdoom/cgdoom-ui.c` to gint drawing (396×224) and `getkey`. Replace F1/F6 actions.

**Input:**
23. Replace `CGD_ScanKeyboard`/`KeycodeDown` with gint `clearevents()` + `keydown()`. Keep keymaps as gint `KEY_*` codes, which already equal CGDoom's on the fx-CG50 layout.
24. Add fx-CG100 default keymaps and key-name tables, chosen at runtime by `gint[HWCALC] == HWCALC_FXCG100`.
25. The `CGD_PRGM_Getkey` equivalent: wait for `pollevent()` with `KEYEV_DOWN`.

**Time:**
26. `I_GetTime` → 35 Hz from `prof`/TMU, e.g. `rtc_ticks()*35/128` or a timer callback counter.
27. Frame pacing via `timer_configure(TIMER_ANY, 28571*(fs+1), GINT_CALL(set_flag))` + `sleep()` until the flag is set.
28. FPS counter via `rtc_ticks()`.

**Exit:** return from `main`, so gint quits back to MPM. Flush delayed writes first (world switch).

**Safety:**
29. Make `I_Error` fatal or longjmp to the launcher.
30. Fix B2 (patchcount) and B4 (fragmented patch offset).
31. Optionally fix B3 (double `W_InitMultipleFiles`).

**Host harness:**
32. Keep `src-sdl2` compiling against the new platform API (§8.3) and add frame-CRC golden tests before optimizing.

---

## 13. Optimization opportunities (ranked by expected gain on SH4) [G unless noted]

1. **`FixedMul`** (`cgdoom/m_fixed.c:40-82`, a bit-serial loop of up to 32 iterations) → `(fixed_t)(((int64_t)a*b)>>16)`, which compiles to `dmuls.l` + `sts mach/macl` + `xtrct`, a few cycles. Make it `static inline` in `m_fixed.h`. It is called throughout `R_ScaleFromGlobalAngle`, `R_ProjectSprite`, `R_MapPlane`, `P_*` and the automap.
2. **`FixedDiv`/`FixedDivU`** (`:90-115`, 16 iterations of `/` and `%`, i.e. 32 libgcc division calls) → `((int64_t)a<<16)/b` (one `__divdi3`), or a reciprocal or `div1` routine. Also check `SlopeDiv` (`cgdoom/tables.c:48`).
3. **`SHORT`/`INTFromBytes`/`finecosine`** → `static inline` (or macros). For aligned data use `__builtin_bswap16/32` (SH4 `swap.b`/`swap.w`); `movua.l` handles unaligned 32-bit loads on SH4A.
4. **Turn off `RANGECHECK`** in release builds (`cgdoom/doomdef.h:79`): removes branches in every column and span draw.
5. **Cached zone memory** (fixes B6 for free on gint) and screens in cached RAM.
6. **Display path** (§6.5): DMA strips overlapped with conversion, 4-pixel word loads, LUT in cache. That cuts `DisplayInterface` time to roughly the DMA time of 128,000 B.
7. **Overclock**: `clock_set_speed(CLOCK_SPEED_F4)` on fx-CG100 (F5 == F4). gint restores the clock around world switches and at exit [D]. Provide a menu toggle.
8. **Column/span inner loops in SH4 asm** (unrolled, `dt` loop, colormap pointer in a register; the span could use 2-pixel writes). Put hot functions (`R_DrawColumn`, `R_DrawSpan`, `R_DrawMaskedColumn`, `R_MapPlane`) and their tables in ILRAM (4 KiB at `0xe5200000`) with gint's `GILRAM` attribute (and `GXRAM`/`GYRAM` for strip buffers), defined at `gint/include/gint/defs/attributes.h:17-19` [V].
9. **Fullscreen-patch path**: replace `CGD_Frag_u8` per-pixel walking with per-column gathering (or draw once into `screens[1]` and reuse it). The title screen currently redraws every frame.
10. **Auto frameskip**: replace fixed `CGD_Frameskip` with a deadline-based skip (render if ahead of schedule).
11. **Raise renderer limits** (B10) and reconsider the `PU_CACHE` thrash: with about 5 MiB, composite textures and fragmented lumps can stay cached.
12. Optional: low-detail mode (`R_DrawColumnLow`, 160-wide), or a native 396×224 or 384×216 render (bigger engine change: status bar and menus assume 320×200).

---

## 14. Open questions and things to verify on hardware

1. `Bfile_GetBlockAddress` address and signature on **each** fx-CG100 OS version you target (2.00 is `0x80333cf2` per gpsp; 2.10 is unknown). Also whether gint dev's API selection supports OS 2.10 at all (`kernel.c:49-54` only matches `02.00`).
2. Whether the free-RAM range MPM advertises `[0x8c200000, loadAddress)` is fully safe. gpsp restricts itself to `0x8c200000..0x8c655300` and reports that touching `0x8c800000` was unsafe in an earlier build [D].
3. Whether DMA to a partial-width LCD window works on the fx-CG100 panel (gpsp says yes; gint's comment says use the full width).
4. Real WAD fragmentation on fx-CG100 Fugue: the share of lumps referenced vs copied (the developer-info screen should be kept).
5. Exact user-storage size of your unit (WAD choice depends on it).
6. Whether the SPU/PRAM0 is usable under gint on fx-CG100 (only if extra RAM is ever needed).

Sources used beyond local repos:
- [HP Museum forum: fx-CG100 memory sizes](https://www.hpmuseum.org/forum/post-203451.html)
- [HP Museum forum: CG100 vs CG50 user memory](https://www.hpmuseum.org/forum/post-203571.html)
- [Cemetech: fx-CG100 announcement thread](https://www.cemetech.net/forum/viewtopic.php?t=19927&start=0)
- [Cemetech: Prizm memory map](https://www.cemetech.net/forum/viewtopic.php?t=8066&start=0)
- [Wikipedia: Casio graphic calculators](https://en.wikipedia.org/wiki/Casio_graphic_calculators)

Local third-party reference: `/Users/cheparukhin/casio_doom/ref/extra/gpsp-sh4-jit` (fx-CG100 gint port of gpSP: NOR mapping, LCD strip DMA, memory ranges).
