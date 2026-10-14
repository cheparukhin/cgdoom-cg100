# fx-CG100 / Graph Math+ add-in precedents: how other ports solved the practical problems

Research date: 2026-10-09. Scope: add-ins that are known to run on the fx-CG100 / Graph Math+ / fx-1AU Graph (the "CG100 family") through MPM, and what they teach us for a CGDoom port.

Tags used throughout:
- **[V]** verified in source code (path:line given) or in a primary forum post by the developer.
- **[G]** guess or inference. The reasoning is given.

Doom-related clones remain under `/Users/cheparukhin/casio_doom/ref/`. MPM and its
forks moved to `/Users/cheparukhin/casio-mpm-220/ref/`; table paths are relative
to the Doom repository root:

| Local path | Upstream | HEAD date |
|---|---|---|
| `../casio-mpm-220/ref/mpm` (main, origin/dev, origin/dynload) | git.planet-casio.com/PlaneteCasio/mpm | main cbd1a41, 2026-08-17 (v1.1) |
| `ref/gint` (dev) | Lephenixnoir/gint | 463f180 (2.11.0-91) |
| `ref/extra/gpsp-sh4-jit` | github.com/KaraRyougi/gpsp-sh4-jit | d72a09c, release v00.10.0804 (2026-08-05) |
| `ref/extra/CalcLoverHK_nesizm` (branch fxsdk) | git.planet-casio.com/CalcLoverHK/nesizm | bff20cc, 2026-09-11 (v1.10) |
| `ref/extra/CalcLoverHK_prizoop` (branch fxsdk) | git.planet-casio.com/CalcLoverHK/prizoop | 9db8002, 2026-09-11 (v1.11) |
| `ref/extra/CalcLoverHK_fxcraft` | git.planet-casio.com/CalcLoverHK/fxcraft (mirror: github.com/0v0katai/fxcraft) | 590b8c1, 2026-09-11 |
| `ref/extra/CalcLoverHK_libsyscall` | git.planet-casio.com/CalcLoverHK/libsyscall | 653ce22, 2026-08-31 |
| `ref/extra/CalcLoverHK_Add-in_Push` | git.planet-casio.com/CalcLoverHK/Add-in_Push | 84f2151, 2026-09-13 |
| `ref/extra/CalcLoverHK_Xtune` | git.planet-casio.com/CalcLoverHK/Xtune (Ptune4) | 2026-04-10 |
| `../casio-mpm-220/ref/extra/CalcLoverHK_mpm` | CalcLoverHK fork of MPM | same as main |
| `ref/extra/Kin1009_gint`, `/Users/cheparukhin/casio-mpm-220/ref/extra/Kin1009_mpm`, `../casio-mpm-220/ref/extra/Kin1009_mpm-cfg` | git.planet-casio.com/Kin1009/* | 2026-09-26 |
| `ref/extra/Slyvtt_SquishIt`, `Slyvtt_OutRun`, `Slyvtt_casio-emu` | git.planet-casio.com/Slyvtt/* | 2026-03-28, 2025-02-25, 2023 |
| `ref/extra/Lephenixnoir_CGDoom` | git.planet-casio.com/Lephenixnoir/CGDoom | same as `ref/CGDoom`, plus `in330-backup` |
| `ref/extra/ClassPadDev_CPDoom` | git.planet-casio.com/ClassPadDev/CPDoom | 2025-03-08 (fx-CP400, not CG100) |

---

## 0. Executive summary for the CGDoom → CG100 port

1. **No Doom port exists for the CG100.** I searched GitHub (repos and code), the Planète Casio forge (`/explore/repos` for "doom", "cg100", "math+", "mpm") and the MPM threads. **[V]** Two near misses:
   - CGDoom's `origin/in330` branch (Dec 2024) is a university practical (TP) for porting CGDoom to a **RISC-V 64** emulator. It is not a CG100 attempt. **[V]** See `git show origin/in330-backup:README.md` and `CGDoom.x` with `OUTPUT_ARCH(riscv32)`.
   - Critor (Planète Casio admin) named Doom in Dec 2024 as the typical program that would not fit the CG100's 4.5 MB storage. **[V]** (MPM thread p1, 03/12/2024)
2. **Two software stacks are proven on the CG100, and both use the fxSDK toolchain:**
   - **(a) fxSDK + gint `@dev`.** Used by gpSP-sh4-jit, PythonExtra, OutRun, SquishIt, SDL Tetris, Ptune4/Xtune and Upsilon. **[V]**
   - **(b) fxSDK toolchain + CalcLoverHK's `libsyscall` (`libsyscall-cw.a`).** This keeps the PrizmSDK/libfxcg API (`Bfile_*_OS`, `Bdisp_*`, `GetKey`, `sys_malloc`…) and calls the OS functions at fixed addresses chosen per OS version, because the CG100 OS has no syscall table. Used by NESizm, Prizoop and fxcraft, all ported from PrizmSDK in Aug 2026. **[V]**
   - **Stack (b) is the closest analogue to how CGDoom is written today** (CGDoom uses libfxcg).
3. **Large-file loading has two hardware-proven patterns:**
   - **gpSP:** call `Bfile_GetBlockAddress` for every 4 KiB block inside a *single* `gint_world_switch`, then convert each P2 (uncached, 0xA…) pointer to its P1 cached alias. It maps contiguous 32 KiB pages zero-copy. Fragmented, unaligned or "weird" blocks fall back to a RAM gather cache or to `BFile_Read`. It re-queries the block table after every file write. **[V]**
   - **NESizm/Prizoop:** build the same 4 KiB block-pointer table, then `memcpy` from flash into an LRU cache of 8 KiB banks (NESizm) or 4 KiB pages (Prizoop). **[V]**
   - MPM itself reads the whole add-in into RAM with one `BFile_Read`. KhiCAS stores its code compressed (miniz) and decompresses it into RAM. **[V]**
4. **RAM is 8 MiB at 0x8c000000–0x8c7fffff. Touching 0x8c800000 cold-boots the calculator.** **[V]** (gpSP linker script comment.)
   - Under **MPM main (builds since 2025-02-22)**, gint gets an arena `"_ld1"` = `0x8c200000 .. load address of the add-in code`.
     - In v1.1 (2026-08) the load address is `(0x8c7f0000 − code_size) & ~0xffff`. For a ~350 KB add-in this is ≈ 0x8c7a0000, so `_ld1` ≈ **5.6 MiB**.
     - In the early-2025 builds the code sat at a fixed 0x8c400000, so `_ld1` was only 2 MiB.
     - gint also gets `"_uram"` = 512 KiB (virtual 0x08100000).
   - Under the **`dynload` MPM build** (shipped by gpSP and KhiCAS), no arena is passed. The add-in must find free RAM itself.
5. **Storage is only ~4.5 MB** (the CG50 had 16 MB). The CGDoom sound-stripped shareware `doom.wad` is 3,393,736 B and fits. Stock DOOM1.WAD is 4,196,020 B and barely fits. Ultimate Doom, the single-episode WADs (5.2–6.6 MB) and Doom II (12.4 MB) do not fit. **[V]** (CGDoom-WADs listing via the Gitea API; forum.)
   - Compression precedents: KhiCAS (miniz), Prizoop (`.gbz`, zx7 per 4 KiB page), SquishIt (zlib/LZ4 of whole g3a files).
6. **The public MPM installer (`mpm-installer-1.0bw.exe`, Windows only) currently patches only OS 02.00.xx0x machines that have the old display** (bootcode DateA 2017.0106.2008).
   - Not supported as of 2026-10-08: OS 2.10, OS 2.20, and 2.00.xx1x (the 2025 boot with the new display). **[V]** (Lephenixnoir, MPM thread p21.)
   - Add-in-side addresses for OS 2.10 are nevertheless already known (mpm `casiowin.yaml`, libsyscall, Kin1009's gint).
7. **Display:** everyone DMAs RGB565 to the LCD data port at 0xB4000000 through DMAC channel 0. **New-display units** (panel ID 0x32/50 or 0x52/82, read through register 0xDA) need command `0x2C` instead of the R61524 `0x202`. **[V]**
   - gint dev handles this (commit 2700096).
   - The libsyscall ports use the OS `Bdisp_DefineDMARange` plus their own register select.
8. **Keyboard:** direct KEYSC reads at 0xA44B0000 still work. The physical layout of the top 3 rows (plus row 6 and row 5) changed. There are no F1–F6 keys, and HOME/MENU does not return to the OS. Add-ins must `return` from `main` to go back to MPM. **[V]**
9. **Overclock:** gint `CLOCK_SPEED_F4` on the CG100 gives 235.93 MHz CPU with a 58.98 MHz bus. gint aliases F5 to F4 because some Graph Math+ units cannot handle the faster bus. gpSP applies a custom "Ptune4 F5" (235.93 MHz CPU, 117.96 MHz bus) and warns about bit flips. **[V]**

---

## 1. Platform facts relevant to all precedents

| Fact | Value | Source |
|---|---|---|
| CPU/MPU | Same SH7305 / SH-4A as the CG50. The Math+ is "first of all a reskin of the 90+E": same CPU and MPU, different keyboard. | [V] Lephenixnoir, MPM thread p1 02/12/2024 13:41 |
| RAM | 8 MiB, 0x8c000000–0x8c7fffff. 0x8c800000 is past the end on real hardware and causes a cold boot when touched. | [V] `gpsp-sh4-jit/ports/fxcg100/gint-gpsp/fxcg100_gpsp.ld:13-30` |
| OS RAM | The OS uses the first 2 MiB. "The zone at 0x8c200000 is actually 6 MB" (free). | [V] Lephenixnoir, MPM thread p1 02/12/2024 17:30 |
| Initial RAM content | The OS fills the regions originally meant for add-ins with 0x55, not 0. MPM does not clear RAM between launches, so data persists from the previous run. | [V] Lephenixnoir + Slyvtt, MPM thread p8 13/02/2025 |
| Flash | 32 MiB NOR. About 16 MB is OS, about 4.7 MB is the user FS (Fugue), about 11.5 MB is unused. | [V] Parisse, MPM thread p1 03/12/2024 |
| User storage | "4.5 MB" in user docs. A measured free storage figure of 4,793,072 B was reported on hpmuseum (that page returned 403, so I could not fetch it myself). Lephe (2026-02-06): there is no way to enlarge it and probably never will be. | [V] forum; [G] for the exact byte count |
| Storage restore | KaraRyougi restored his CG100 storage to 11,820 KiB with an **unreleased** tool, held back because of regulatory concerns. | [V] Cemetech post p=317309 |
| Syscall table | **Removed.** There is nothing usable at 0x80020070. Every former syscall must be called at its OS-version-specific address. | [V] Lephenixnoir, MPM thread p10 30/07/2025 |
| OS version string | At 0x80020020 (P1) / 0xa0020020 (P2). Values: `"01.00.0000"`, `"02.00.0000"`, `"02.00.0010"` (=2.00.1, new display), `"02.10.0000"`. The user-visible build ("02.00.0202", "02.00.2202") is *not* in that string. | [V] `/Users/cheparukhin/casio-mpm-220/ref/mpm/start.c:27-44`; installer error "Unknown OS (2.10.0000)" (forum p15) |
| Model detection | gint: stack ≥ 0x8c000000 and model string at 0x80000300 not in {"810A","811A","810F"} ⇒ `HWCALC_FXCG100` (= 9). | [V] `gint/src/kernel/hardware.c:139-146`, `include/gint/hardware.h:114-115` |
| Display panel | 396×224 panel. The OS VRAM is 384×216. 2025 boot units have a new controller (ID 0x32/0x52) and older OSes leave the screen black on them. | [V] gint commit 2700096; forum p17–p18 |
| Exam mode | MPM does not launch in exam mode. | [V] beta announcement topic18626 |

---

## 2. MPM (the loader every CG100 add-in runs under)

### 2.1 What it is
- **Two parts. [V]**
  - A **closed-source OS patch**, installed by `mpm-installer-1.0bw.exe` through the OS-update USB mode. It makes **TOOLS** on the HOME screen load the raw binary `\\fls0\mpm.bin` at **0x8c700000** and jump to it.
  - The **open-source `mpm.bin`**: an add-in menu and loader. Sources are in `../casio-mpm-220/ref/mpm`.
- MPM itself is built with the fxSDK toolchain only. It is not a gint program. **[V]** `../casio-mpm-220/ref/mpm/README.md`

### 2.2 How it loads and runs an add-in (main branch, v1.1, `../casio-mpm-220/ref/mpm/main.c`)
- **[V] Lists `\\fls0\*.g3a`** with `CW_BFile_FindFirst`.
  - It accepts any non-folder file type, because g3a files written by add-ins have type 0x01, not 0x41 (`main.c:48-52`).
  - It requires a file size ≥ 0x7002 (`main.c:54`).
  - It reads the header and icons **by block address** with `CW_BFile_Block(fd, offset, &ptr)` instead of copying them (`addins.c:21-42`).
- **[V] `load_addin` (`main.c:208-283`):**
  - Code size = file size − 0x7000 (the header is skipped). The maximum is **0x370000 (3,604,480 B)**. Larger files give error −1001.
  - If the OS add-in buffer `CW_AppMemory()` is available (always 0x8c160000 in `casiowin.yaml:71`):
    - Code goes to `romAddress = (0x8c7f0000 − size) & −0x10000`.
    - User RAM is the OS buffer at 0x8c160000 (512 KiB).
  - Otherwise code goes to `(0x8c770000 − size) & −0x10000` and user RAM to 0x8c770000.
  - The whole binary is read with **one** `CW_BFile_Read(fd, romAddress, size, 0x7000)`. MPM expects the return value to equal `size` (`main.c:245-248`).
  - MMU set-up:
    - **55 UTLB entries of 64 KiB** map 0x00300000 → romAddress.
    - 8 × 64 KiB map 0x08100000 → user RAM.
    - Entry 63 maps **virtual page 0 (4 KiB) to physical 0** (the start of the flash ROM) with user RW, so NULL dereferences do not fault (`main.c:251-266`; `loader.c:54-92` sets PR=3, C=1, D=1).
  - Cache handling: `ocbwb` over the loaded code, then ICI+OCI through CCR at 0xff00001c (`main.c:272-280`).
- **[V] Calling convention (`main.c:345-360`):**
  - `code(0x4d504d30 /* 'MPM0' */, loadinfo)` with `loadinfo = {1,1, 0x10,0x8c200000, 0x11,romAddress, 0}`.
  - gint recognises 'MPM0' and turns each 0x10/0x11 pair into a default kmalloc arena named `"_ld1"` (`gint/src/kernel/start.c:180-182`, `kernel.c:140-166`).
- **[V] After the add-in returns:** MMU off, `CW_DrawFrameC3(0)`, then the add-in list is rescanned. Before launching, MPM sets the OS `CurrentAppIndex` to 0xffff so that the native app is re-initialised (`main.c:362-372`).
- **[V] mpm.bin relocates itself** from 0x8c700000 to **0x8c7f0000** (64 KiB) at start (`reloc.S`, `loader-cg.x:5-9`).
- **[V] OS API selection (`start.c:27-44`):** 01.00→0, 02.00→1, 02.10→2.
  - `"02.00.0010"` is a TODO and silently stays on API 0, the OS 1.00 addresses.
  - All OS entry points are in `casiowin.yaml`.
- **[V] Keys in the MPM menu (`main.c:444-505`):**
  - Arrows move, **EXE/OK** launches, **FORMAT** launches after showing the mapping.
  - **VARS** shows add-in details, **CATALOG** opens the USB connection popup.
  - **SETTINGS** receives an add-in over USB straight into RAM ("Add-in Push"). It waits for a 4-byte big-endian size, then the data. **AC** aborts.
  - `+` opens a UTLB viewer. EXIT/HOME/TOOLS quit.

### 2.3 MPM variants in the wild (important: they change your memory map)

| Variant | Code load address | User RAM (0x08100000) | `_ld1` arena passed? | OS versions | Used by |
|---|---|---|---|---|---|
| main ≥ 2026-08 (v1.1) | `(0x8c7f0000−size)&~0xffff` | 0x8c160000 (OS buffer) | yes, 0x8c200000..load addr | 1.00, 2.00, 2.10 | official repo |
| main Feb–Mar 2025 (forum zip 2025-02-27, 9,305 B) | 0x8c400000 fixed; file ≤ 3 MiB; 48 × 64 KiB mapped | 0x8c780000 | yes (from 68e7f6e, 2025-02-22). `_ld1` = 0x8c200000–0x8c400000, only 2 MiB | 1.00, 2.00 | forum first post (I could not tell which exact build is attached now) |
| **dynload** branch (12,820 B) | `(0x8c700000−size)&~0xffff`; mpm.bin stays at 0x8c700000..0x8c703214 | 0x8c780000 | **no** (called as `void (*)(void)`) | 1.00, 2.00 | **gpSP release asset; Parisse's KhiCAS `mpm.bin`** |
| Kin1009 "1.2 Modded" | same as main | same as main | yes | same as main | community fork (AI-assisted, low confidence) |

- **[V]** dynload: `git show origin/dynload:main.c` lines 187-228 and 396-397.
- **[V]** The gpSP release `mpm.bin` and Parisse's `/Users/cheparukhin/casio-mpm-220/ref/mpm/mpm.bin` are both exactly 12,820 B (GitHub API and HTTP HEAD).
- **[V]** gpSP's linker script says mpm.bin lives at 0x8c700000..0x8c703214 (0x3214 = 12,820 B).
- **[G]** So both are the same dynload build.

- **[V]** Early-2025 main: `git show 3133f75:main.c:190-210`.

**Consequence for us. [G]**
- **The usable high-RAM size depends on which `mpm.bin` the user has:** about 2 MiB with the early-2025 build, about 5.5 MiB with main v1.1, and an undefined amount (no arena) with dynload. Either require main ≥ v1.1 (ship our own `mpm.bin`, as gpSP and KhiCAS do with theirs) or degrade gracefully.
- Allocate the big heap from `kmalloc_max(&size, "_ld1")` when the arena exists, as PythonExtra and OutRun do.
- Otherwise fall back to `0x8c200000 .. mmu_translate(0x00300000)` (the physical load address of our code). gint exposes `mmu_translate()` in `include/gint/mmu.h:27`, and Lephe suggested reading the MMU on the forum (p7 13/02/2025).
- Never hard-link a NOLOAD section at 0x8c200000 if you also let gint create `_ld1`.
  - gint initialises the `_ld1` heap headers at 0x8c200000.
  - Default `malloc` falls through from `_uram` to `_ld1`, because arenas are tried in insertion order (`gint/src/kmalloc/kmalloc.c:55-66,174-186`).

### 2.4 Historical MPM quirks reported on the forum [V]
- **Leaving an add-in leaves RAM dirty.** After KhiCAS with the dynload loader, you had to launch a CASIO app before the next add-in, or the calculator crashed. Lephe saw crashes when launching two add-ins in a row with dynamic loading (p8 22/02/2025, KhiCAS thread 08/05/2025).
- **MPM does not close the running native app.** A live Python heap or an open MEMORY app makes the OS heap fuller; older mpm.bin versions ran out of memory with error −99 (p8 13/02/2025). Current main invalidates `CurrentAppIndex`, but does not close the app.
- **No return-to-menu.** `GetKey` believes it is already in the menu, so MENU/HOME does nothing. Add-ins must return from `main()`. `SetQuitHandler` is pointless (p5 02/02/2025). gint: `gint_osmenu_native()` returns immediately on the CG100 (`gint/src/kernel/osmenu.c:97-100`), and `gint_setrestart` is forced off (`start.c:43-48`).
- **Any OS update removes MPM.** Re-run the installer (p15/p16).

---

## 3. Precedents, one by one

### 3.1 gpSP-sh4-jit (KaraRyougi) — GBA emulator with a SH-4 JIT. The most relevant precedent.

- **Build.** fxSDK + gint `find_package(Gint 2.9)` (dev branch). Commands: `fxsdk build-cg -c -DCGBA_DYNAREC=ON && fxsdk build-cg`, or `ports/fxcg100/build-calc-jit.sh`. The CG50 build uses `-DCGBA_FXCG50=ON`. **[V]** `ports/fxcg100/gint-gpsp/CMakeLists.txt:1-25`, `README.md`
  - Flags: `-O2 -fomit-frame-pointer -ffunction-sections -fdata-sections`, `--gc-sections`, `--allow-multiple-definition` (to override libgint OS stubs), and a custom linker script `fxcg100_gpsp.ld`. **[V]** CMakeLists.txt:565-607
  - **Measured:** `-O3` on the hot interpreter and renderer files *regressed* worst-case fps on CG100 hardware and added ~40 KB, which they blame on I-cache bloat. Keep `-O2`. **[V]** CMakeLists.txt:575-586
- **OS calls.** They bypass gint's BFile wrappers and call **OS 2.00 addresses directly** inside `gint_world_switch(GINT_CALL(addr, ...))`. **[V]** `src/nor_rom.c:35-44,93-152`; `fxcg100_config.c:74-77`; `no_osheap.c:66-132` stubs gint's `BFile_*` on the CG100.

  | Function | OS 2.00 address |
  |---|---|
  | `BFile_Open` | 0x803338d0 |
  | `BFile_Size` | 0x80333b04 |
  | `BFile_Read` | 0x80333dc2 |
  | `BFile_Close` | 0x80333a4e |
  | **`Bfile_GetBlockAddress`** | **0x80333cf2** |
  | `BFile_FindFirst` / `FindNext` / `FindClose` | 0x803345c8 / 0x80334846 / 0x80334950 |
  | `BFile_Create` | 0x80333ef0 |
  | `BFile_Remove` | 0x80334212 |
  | `BFile_Write` | 0x80333f9e |

  - **[V]** No OS-version check: gpSP runs only on OS 2.00.
  - **[V]** It also disables gint's OS-heap fallback arena (`no_osheap.c:7-64`), so allocation failure returns NULL instead of going through "the unsupported 0x80020070 syscall gate".
- **Large ROM loading (up to 32 MiB GBA ROMs). This is the WAD analogue.** `src/nor_rom.c`:
  1. Open the file. For every 4 KiB block call `Bfile_GetBlockAddress(fd, block*0x1000, &ptr)`, **all inside ONE world switch** (`os_bfile_query_blocks`, lines 162-196). **[V]**
     - Reason: doing one world switch per block while overclocked made the RTC lose about 1–2 minutes on a 16–32 MiB ROM. Each switch restores the OS clock and re-applies the overclock, losing one 128 Hz tick (`nor_rom.c:163-172`; README "Builds with storage access enabled").
  2. **Pointer sanitising** (`cached_nor_pointer`, lines 390-417). **[V]**
     - The OS returns P2 (uncached) NOR addresses such as 0xA1B97000.
     - They convert them to the **P1 cached alias** (subtract 0x20000000) because that is "much friendlier" to loads.
     - **Some blocks come back as TLB-mapped P0 addresses (observed 0x001129xx).** These work only while the OS TLB entry is warm and later fault as "TLB miss" (two field crashes), so they are rejected and served by world-switched `BFile_Read` instead.
     - Unaligned pointers are rejected. The partial last block is not mapped, so bytes past EOF read as 0xFF.
  3. Newly mapped NOR lines get `ocbi` (no write-back), then one `synco`. Reason: Fugue may have relocated records, and P1 cache lines could be stale (lines 419-435, 658-685). **[V]**
  4. Classify 32 KiB pages. Physically contiguous and 4-aligned pages are used **zero-copy, directly from flash**. Fragmented pages use the per-4 KiB table. "Unsafe" blocks are gathered into a 1 MiB / 32-page aligned RAM cache, and the same buffer doubles as savestate scratch (lines 609-728; README "Resolves fragmented cartridges through a hybrid map"). **[V]**
  5. **Page 0 is always copied to RAM**, because a stray write to a NOR address "can latch a flash command sequence, not merely fault" (lines 718-727). **[V]** for the code; [G] for the hardware claim.
  6. **After ANY file write** (save state, SRAM save), the whole block table is re-queried. Fugue is log-structured and may relocate the open file's records. The header is compared to detect breakage, with a fallback to pure `BFile_Read` mode (`nor_rom.c:730-775`; `gpsp_runner.c:895-905`). **[V]**
  7. `BFile_Read` success check: `result == size || result == 0`. Their comment: Fugue returns bytes read, CASIOWIN returns bytes remaining (`nor_rom.c:139-143`). **[V]** [G] for which convention CG100 really uses, but MPM relies on "bytes read".
- **Memory.** Not from `_ld1` (they recommend the dynload MPM, which passes none). Instead a linker NOLOAD section `.cgba.highbss` at **0x8c200000**, with a hard assert on its end ≤ **0x8c655300** (`fxcg100_gpsp.ld:31,148-159`; `main.c:136-146,179-196`). **[V]**
  - It holds GBA memories, framebuffer, JIT caches (ROM 1 MiB, RAM 512 KiB) and the ROM fallback cache.
  - They report that ends of 0x8c6b5300 and 0x8c715300 "hard-reset the calculator", and blamed MPM at 0x8c700000 and "live OS state". **[V]** (CMakeLists.txt:126-130; docs/performance-history.md "arena wall")
  - **[G] My reading:** with the dynload MPM their own 459,324-byte code is loaded at `(0x8c700000−0x7023C)&~0xffff = 0x8c680000`, so any arena end above 0x8c680000 overwrote **their own add-in code**. The 0x8c655300 ceiling is binary-size-specific, not a general CG100 limit.
  - On-chip RAM use: **ILRAM** 0xE5200000 (4 KiB, dispatch stubs), **XRAM/YRAM** 0xE5007000 / 0xE5017000 (8 KiB each, LCD strip buffers), optionally XYRAM fastmem at 0xE500E000 (`fxcg100_gpsp.ld:32-33,111-133`; `gint_platform.c:22-50`). **[V]**
- **Display.**
  - Menus use gint `dclear/dtext/dupdate`.
  - Gameplay uses a **direct R61524 window**. `r61524_start_frame(x0,x1,y0,y1)` is gint's, so the new panel works. Then `dma_transfer_async(0, DMA_32B, blocks, strip, DMA_INC, 0xb4000000, DMA_FIXED)` sends 16-row strips from XRAM/YRAM, **double-buffered**: the next strip is prepared while the previous DMA runs, and the last DMA is left in flight (`gint_platform.c:255-266,461-515`). **[V]**
  - Pitfalls they hit **[V]** (`gint_platform.c:52-61,72-82,250-253`):
    - After narrowing the window you must restore the full 396×224 window before any gint `dupdate`. Otherwise you get a black screen with a stray line.
    - **The OS paints its BFile busy indicator directly into LCD GRAM during a world switch.** Redraw the border after any file I/O.
  - "Scanline streaming" of 16-row groups during rendering was **slower on hardware** than end-of-frame strip DMA (Ace Attorney A/B). **[V]** CMakeLists.txt:363-372
- **Keyboard.** gint `keydown()` with CG100 logical keycodes. The config stores "skin positions" (col×10+row) and translates them (`gint_platform.c:85-127`; `fxcg100_keys.c:60-91`). The full table is in §5. **[V]**
  - Menu key is **ON** (CG100) / **AC/ON** (CG50). Defaults: A=OK(48), B=CATALOG(37), Select=PgUp(29), Start=PgDn(28), L=BEGIN/PrevTab(59), R=END/NextTab(39).
  - Before exiting: wait until all keys are released, finish the LCD DMA, and restore the window before the final BFile calls and `kquit` (commit 65c1edf). **[V]**
- **Overclock.** Default ON, "Ptune 4 F5" via `cpg_set_overclock_setting()`. gint restores the launcher's clock at every world switch and at exit, then re-applies F5. **[V]** `src/overclock.c:6-38`

  | Register | Value |
  |---|---|
  | FLLFRQ | 0x00004384 |
  | FRQCR | 0x1f001103 |
  | CS0BCR | 0x44900400 |
  | CS3BCR | 0x04904400 |
  | CS0WCR | 0x000004c0 |
  | CS3WCR | 0x000024d2 |
  | CS5aBCR / CS5aWCR | 0x17df0400 / 0x000203c1 |

  - Result: CPU 235.93 MHz, SuperHyway 117.96, SDRAM/bus 117.96 (CL2, TRC6), peripheral 29.49.
  - They chose this explicitly because gint aliases F5 to F4 (bus 58.98 MHz).
  - Release notes: aggressive overclocking may cause bit flips. Hardware crash class "EXC=1A0 in-arena under overclock" led to a JIT memory canary (docs/performance-history.md). **[V]**
- **Files / saves. [V]** `fxcg100_config.c:294-337`; `gpsp_runner.c:1120-1155`
  - If a file of the right size exists, open it write-only and overwrite it (fast path). Otherwise `BFile_Remove` + `BFile_Create(path, FILE, &size)` + write.
  - Savestates are rounded up to **64 KiB size buckets** so that repeated saves hit the fast overwrite path instead of delete+create.
  - Backup (SRAM) writes are deferred to game switch or exit.
- **Performance. [V]** README; docs/performance-history.md "Emerald hardware A/B"; Cemetech post
  - The author reports "50+ FPS" in Advance Wars and Ace Attorney, overclocked.
  - Emerald in Oldale Town on a physical CG100: 44 → 47 emulated fps after an optimisation bundle.
  - Most other numbers are cache-simulator "modeled fps" (118e6/cycles). Treat them as indicative only.
- **Testing infrastructure.** A private extended `casio-emu` ("calcemu" with HLE, cache simulation and `HLE_FLASHFILE`). Hardware-only bug classes it missed: **misaligned longword loads (EXC=0E0) are tolerated by the emulator but fault on hardware.** **[V]** performance-history.md
- Also: the dynarec is described as AI-written (GPT/Claude). Treat any comment-level claim as a hypothesis.

### 3.2 NESizm and Prizoop (CalcLoverHK fxSDK ports of tswilliamson's PrizmSDK emulators)

- **Build.** fxSDK toolchain, **not gint**. They link `libsyscall-${PLATFORM}` from the repo (`libsyscall-cw.a` for the CG100, `libsyscall-cg.a` for the CG50). **[V]** `CalcLoverHK_nesizm/CMakeLists.txt`
  - Configure with `fxsdk build-cg -c -DPLATFORM=cw && fxsdk build-cg`. Output `nesizm_cg100.g3a` / `prizoop_cw.g3a`.
  - Custom `prizm.ld`: rom 0x00300000 (2M), ram 0x08100000 (512k), ilram 0xE5200000 (4k), xyram 0xE500E000 (16k), entry `initialize`.
  - Flags: `-O2 -funroll-loops -mpretend-cmove -fdelayed-branch -gc-sections`, C++11 with `-fno-rtti -fno-exceptions`.
  - Dependencies come via FetchContent from github.com/0v0katai: prizm-zx7, calctype, prizm-snd, scope_timer, ptune2_simple.
- **Porting diff was tiny** (commit 3e7a9eb):
  - A `config.h` macro `_(CG,CW)` selects CG50 vs CG100 values for key codes and labels ("MENU" vs "HOME").
  - An "Exit" menu item was added, because there is no return-to-menu.
  - `fxlibc_hal.c` routes malloc to `sys_malloc`.
  - The OS functions used come from a project `casiowin.yaml` with **[OS 2.00, OS 2.10]** address pairs. **[V]** `CalcLoverHK_nesizm/casiowin.yaml`. Highlights:

  | Function | OS 2.00 | OS 2.10 |
  |---|---|---|
  | `Bfile_GetBlockAddress` | 0x80333cf2 | 0x8039675a |
  | `Bfile_OpenFile_OS` | 0x803338d0 | 0x80396338 |
  | `Bfile_ReadFile_OS` | 0x80333dc2 | 0x8039682a |
  | `Bfile_WriteFile_OS` | 0x80333f9e | 0x80396a06 |
  | `Bdisp_DefineDMARange` | 0x800742a0 | 0x80074dd8 |
  | `Bdisp_DDRegisterSelect` | 0x80074268 | 0x80074da0 |
  | `Bdisp_WriteDDRegister3_bit7` | 0x800743de | 0x80074f86 |
  | `Bdisp_PutDisp_DD` | 0x8007a2d4 | 0x8007af9a |
  | `GetVRAMAddress` | 0x8007569e | 0x80076262 |
  | `GetKey` | 0x801d2bba | 0x8022b47e |
  | `RTC_GetTicks` | 0x8007cc0c | 0x8007db68 |
  | `CMT_Delay_micros` | 0x80230b4e | 0x802925fa |
  | `sys_malloc` / `sys_free` | 0x80366708 / 0x803664d4 | 0x803c9ccc / 0x803c9a98 |

  - CalcLoverHK confirmed several of these with "Parisse's syscall table generator and fxos" (forum p16 11/01/2026). **[V]**
- **ROM loading = block-pointer table + RAM bank cache. [V]**
  - NESizm `nes_cart::BuildFileBlocks()` calls `Bfile_GetBlockAddress(handle, i*0x1000, &blocks[i])` for every 4 KiB block; the table holds `unsigned char* blocks[1024]` (max 4 MiB). `BlockRead()` then `memcpy`s from those flash pointers into 8 KiB banks (`src/nes_cart.cpp:402-431`).
    - Cache: `STATIC_CACHED_ROM_BANKS 24` (192 KiB on the stack in `main`) plus up to `MAX_CACHED_ROM_BANKS 32` with the rest from `malloc` (`src/nes.h:46-47`; `src/main.cpp:34-35`). LRU via `request` counters (`nes_cart.cpp:445-497`).
  - Prizoop: same table, `BlockAddresses[1024]` (`src/mbc.cpp:146-155`). `mbcReadFile` memcpys 4 KiB pages (`mbc.cpp:157-176`).
    - **Compressed `.gbz` ROMs:** a header of N 16-bit compressed page sizes, then zx7-compressed 4 KiB pages decompressed on demand. Pages that do not compress are stored raw (size 4098) (`src/rom.cpp:105-128`, `mbc.cpp:182-200`). README: some games compress to ~10%. A Windows tool `comp-gb.exe` creates them.
  - Neither project checks P0/unaligned pointers, nor re-queries after writes. **[G]** They get away with it because ROM files are not written while mapped.
- **Display.** Their own DMAC ch0 to the LCD at 0xB4000000, from VRAM or from YRAM 0xE5017000 (`prizoop/src/display_directlcd.cpp`; `fxcraft/src/fastBdisp.c`).
  - Sequence: `Bdisp_WriteDDRegister3_bit7(1); Bdisp_DefineDMARange(x0,x1,y0,y1); SelectVRAMDataRegister();`. The last one selects register 0xDA and reads the panel ID; 0x32 or 0x52 ⇒ select **0x2C**, else **0x202** (commits 5a4f353 / ce19411 "DMA support for fx-CG100 with new LCD driver"). **[V]**
  - DMA: `MSTPCR0 (0xA4150030) &= ~(1<<21)`, then SAR/DAR/TCR/CHCR at 0xFE008020/24/28/2C, `CHCR=0x00101400`-style (32-byte units), and DMAOR 0xFE008060. **[V]**
- **Keyboard.** `keyDown_fast()` reads KEYSC at 0xA44B0000 directly, with CG100 key positions substituted (see §5). **[V]**
- **Overclock.** `ptune2_simple` presets (PT2_NOMEMWAIT / HALFINC / DOUBLE) are applied only if `Ptune2_GetSetting()==PT2_DEFAULT`. **[V]** (`prizoop/src/screen_play.cpp:87-97`)
  - Those defaults are the fx-CG10/20 values (FRQCR 0x0F102203).
  - **[G]** On the CG50/CG100, whose default FRQCR is 0x0F011112, this check probably fails, so no overclock is applied.
- **Performance claim:** "runs most titles at 60 FPS with no overclocking on the FX-CG50 and FX-CG100" (NESizm README). **[V]** (claim)

### 3.3 libsyscall (CalcLoverHK) — the bridge that made 3.2 and 3.4 possible

- **What it contains. [V]**
  - `libcg`: classic Prizm syscall stubs via 0x80020070 (`SYSCALL(_Bdisp_PutDisp_DD, 0x025F)` etc.).
  - `libcw`: CASIOWIN direct-address dispatch for the CG100, generated from a `casiowin.yaml` with `api_versions: 2` (columns OS 2.00, OS 2.10) by `casiowin_gen.py`, which was adapted from MPM.
  - The libcw template yaml lists ~160 functions, all commented out. Each project ships its own yaml and its own prebuilt `libsyscall-cw.a`.
- **OS selection, `libcw/crt0.S:36-50`. [V]**
  - Read the first 4 bytes at 0xa0020020; if they equal `"02.1"` (0x30322e31) use API 1, otherwise API 0 (= 2.00).
  - **OS 1.00 is not supported. Any unknown version silently uses the 2.00 addresses** ⇒ crash risk on 2.20 / 2.00.1.
- **`GetKey` wrapper (`libcw/GetKey.c`). [V]**
  - Optional "better GetKey": uses `GetKeyWait_OS` and remaps the new keys to the old `KEY_CTRL_F1..F6`, `KEY_CTRL_MENU`, etc.
  - Mapping: ON→F1, HOME→F2, PrevTab→F3, PgUp→F4, NextTab→F5, PgDn→F6, SETTINGS→SETUP, CATALOG/TOOLS→CATALOG/OPTN.
- **`relocate` (`libcw/reloc.S`). [V]** Copies the image from 0x00300000 to `_reloc_target` with `ocbwb/synco/icbi`. With the stock `prizm.ld`, target = 0x00300000.
- Build: `fxsdk build-cg` (`FXSDK_PLATFORM_LONG = fxCG50` produces both `syscall-cg` and `syscall-cw`), `-Os -fstrict-volatile-bitfields`.
- **[G] Relevance:** CGDoom's `src-cg` layer uses libfxcg (Bfile, Bdisp, PRGM_GetKey, GetKeyWait…). A libsyscall-cw-style table of ~20–30 entries for 2.00/2.10 would let the existing CGDoom platform layer run on the CG100 with minimal changes. The alternative is moving to gint (§3.5).

### 3.4 fxcraft (CalcLoverHK; 3D Minecraft-like, a performance-heavy 3D renderer)

- **Build.** Same as 3.2: `fxsdk build-cg -c -DPLATFORM=cw && fxsdk build-cg`, links `-lc -lgcc -lsyscall-cw`, `prizm.ld`. **[V]** `CalcLoverHK_fxcraft/CMakeLists.txt`, `README.md`
  - **Flags: `-Ofast -fschedule-insns -flto=auto -fno-strict-aliasing -fdata-sections`.**
- **Memory: hard-coded RAM addresses in the 0x8C2xxxxx–0x8C5xxxxx region. [V]** `src/engine.c:314-315,353,412-415`
  - lightmap 0x8C228800, blockData 0x8C282800, assetsInputBuffer 0x8C500000, textures 0x8C540000, itemIcons 0x8C580000, misc 0x8C5B0000.
  - **ILRAM 0xE5200000** is used as a scratch `screenColor` buffer.
  - The earlier MPM build added `ALLOC_OFFSET 0x100000`, because the old MPM loaded code at 0x8c400000. It was removed on 2026-08-24 for the current MPM, which loads code at the top of RAM (commit 46b4922). **[V]**
  - Also uses `tlsf.c` (TLSF allocator) and `lz4.c`.
- **Files.** `\\fls0\fxcraft\world%d\worldData` and similar. Read whole files with `Bfile_ReadFile_OS` into RAM. Create files with `Bfile_CreateEntry_OS(path, 1, &size)` (final size up front), then open mode 3 and write. **[V]** `engine.c:17130-17460`. Texture packs live in `\\fls0\fxcraft\` (README).
- **Display.** DMA as in 3.2 (`src/fastBdisp.c:17-79`, `src/DMA_Transfer.c`), including the new-panel 0x2C select. YRAM at 0xE5017000 is used as a strip source. **[V]**
- **Keyboard.** `keyupdate()` memcpys 16 bytes from KEYSC 0xA44B0000; `keydownlast(code)` uses `row=code%10`, `col=code/10-1`, `word=row>>1`, `bit=col+8*(row&1)` (`src/controls.c:3-26`). CG100 codes come from `_(cg,cw)` in `controls.h:7-41`. **[V]**
- No perf numbers published.

### 3.5 KhiCAS for CG100 (Bernard Parisse) — the "huge add-in, small storage" precedent

- **Files. [V]** HTTP HEAD on `https://www-fourier.univ-grenoble-alpes.fr/~parisse/casio/mpm/`:
  - `mpm.bin` 12,820 B (special build, 2025-05-07)
  - `khicas1z.g3a` **72,416 B** (2025-05-26)
  - `cas100a.mzs` 1,273,372 B (2026-08-13)
  - `cas100b.mzs` 1,126,124 B
  - Launch: HOME → TOOLS → Khicas1z. KhiCAS works only on OS version 2.
- **Mechanism. [V]** Parisse, KhiCAS thread 08/05/2025; MPM thread p1–p7
  - The full KhiCAS is about 3.5–3.8 MB of code. It is stored **compressed with miniz** as two `.mzs` files (about 2.3–2.4 MB total).
  - A tiny g3a stub decompresses them into RAM and runs them, using the dynload loader. Parisse says he also uses 0x8c780000–0x8c800000 (the virtualised add-in static RAM) for the same purpose.
  - Chunked decompression was needed so as not to hold 2.4 MB compressed + 3.8 MB decompressed in RAM at once (p4/p7). Signatures were added to the compressed files (2025-05-09).
- **Measured speed claims. [V]**
  - Slyvtt: zlib decompression of 500 KB in RAM takes about 300 ms.
  - Lephe: reading about 200 KB from storage (icons of 7 add-ins) feels instantaneous (p3 14/01/2025).
- **Syscalls. [V]** Parisse rebuilt libfxcg with a `syscalls.c` of OS 2.00 addresses and no-op stubs for unknown ones (`.../casio/mpm/syscalls.c`). He removed the `GlibAddinAplExecutionCheck` (syscall 0x29) call from `crt0.S`.
  - **`GetKey` cannot see the ON key, but `GetKeyWait_OS` can** (p6/p7, 09–10/02/2025).
- **Overclock.** KhiCAS offers an overclock prompt at start-up. CalcLoverHK checks per-unit "F5 preset" SDRAM tests and keeps a ranking spreadsheet: [docs.google.com/spreadsheets/d/1EI69Zm1d0ogmuQ40Z9p_LPUsblqWt15_Ocg0lDrmLk0](https://docs.google.com/spreadsheets/d/1EI69Zm1d0ogmuQ40Z9p_LPUsblqWt15_Ocg0lDrmLk0) (forum p17). **[V]**

### 3.6 gint-dev add-ins: PythonExtra, OutRun, SquishIt, SDL Tetris, Ptune4/Xtune, Upsilon

- **Toolchain.** `giteapc install fxsdk@dev gint@dev fxlibc@dev`. Lephe, 2026-06-17: no release has been made in ages, so everything is still on dev. **[V]** forum p19/p20
  - The g3a works on the CG50 too, with runtime detection through `gint[HWCALC]==HWCALC_FXCG100`.
- **Memory.** On the CG100 the big arena is **`"_ld1"`**, which replaces `"_ostk"` (that one is not created on the CG100; `gint/src/kernel/kernel.c:124-138`). `"_uram"` still works. **[V]** Slyvtt p8 27/02/2025
  - PythonExtra: `kmalloc_max(&size, "_ld1")` gives the whole arena to the MicroPython GC (`TheRainbowPhoenix/PythonExtra ports/sh/main.c:500-505`).
  - OutRun: the same call wrapped into its own `"extram"` arena (`Slyvtt_OutRun/src/main.cc:521-533`).
  - gint VRAM on the CG100 comes from the default arena, because `_ostk` is absent (`gint/src/render-cg/dvram.c:93-110`).
- **Display.** OutRun: `r61524_display(gint_vram, 0, DHEIGHT, R61524_DMA_WAIT)` directly instead of `dupdate()` (`main.cc:1355`). **[V]**
- **Overclock.** OutRun: `clock_set_speed(CLOCK_SPEED_F4)` on the CG100 as well (`main.cc:608`). **[V]**
- **Files.** SquishIt uses gint stdio (`fopen/fread/fwrite`) and wraps **a whole compress/decompress job in one `gint_world_switch(GINT_CALL(...))`** (`Slyvtt_SquishIt/src/core/squishit.c:807-830`). **[V]**
  - SquishIt finding: writing a g3a from a running add-in crashes CG50 OSes but works on the Math+ (topic19185).
- **Keyboard.** Use the new gint keycodes (`include/gint/keycodes.h:90-103`). **[V]**
  - KEY_ON 0xa6, KEY_PREVTAB 0xa7, KEY_NEXTTAB 0xa8, KEY_PAGEUP 0xa9, KEY_PAGEDOWN 0xaa, KEY_SETTINGS 0xab, KEY_OK 0xac, KEY_CATALOG 0xad, KEY_EXPFUN 0xaf.
  - Aliases: KEY_HOME=KEY_MENU, KEY_BACK=KEY_EXIT, KEY_TOOLS=KEY_OPTN, KEY_FORMAT=KEY_FD.
  - Compatibility: holding **CATALOG + a row-6 key** produces KEY_F1..F6 (`gint/src/keysc/keydev.c:274-279`).
  - Slyvtt's cSDL supports both keyboards, so one g3a runs on both models (SDL Tetris, p6 09/02/2025).

### 3.7 Kin1009 forks (Sep 2026; low confidence, partly AI-generated)

- **gint fork:** adds CASIOWIN API 3 with OS 2.10 addresses, as a 4th column (commit ee12efd). It also drops the `HWCALC==FXCG100` condition, because "a CG100 whose ROM model string is a CG50 string" fell back to API 0 and every fs call returned −1 (commit 376c55c, `src/kernel/kernel.c:44-68`). **[V]** for the code; [G] for the claim.
- **MPM "1.2 Modded" + MPMCfg.** To write a config reliably, MPMCfg deletes the file, recreates it with its final size, writes it, reads it back and verifies, with one retry. It says this avoids growing a file in place. **[V]** README; [G] for the reliability claim.

### 3.8 Related Doom ports (not CG100)

- **CGDoom (CG50).** Reads the WAD by BFile or by "mmap" (scanning flash fragments at `FLASH_CACHED_START`). "Experimental memory" scans 0xac200000 for zeroed RAM (`FindZeroedMemory`, `cgdoom/cgdoom.c:351-364,684-690`). Reads KEYSC at 0xa44b0000 directly (`cgdoom-kbd.c:157-159`). g3a size 349,292 B. **[V]**
  - **[G]** On the CG100 the zero-scan is unreliable: the OS initialises add-in RAM to 0x55 and MPM never clears it. Use the `_ld1` arena instead.
- **CGDoom-WADs (Lephenixnoir). [V]**
  - `doom.wad` 3,393,736 B (1.9 shareware, sound removed)
  - `doomu.wad` 10,302,661 B
  - `doomu1..4.wad` 5,200,635 / 6,645,489 / 6,416,573 / 5,826,766 B
  - `doom2.wad` 12,412,808 B
- **CPDoom (fx-CP400 via Hollyhock).** The closest "Doom on a Casio without a syscall table" precedent, but different hardware and loader (code linked at 0x8C052800). Not reusable directly. **[V]**

### 3.9 Summary table

| Add-in | Stack | Big data load | Arena / RAM | Display | Keyboard | OC | OS supported |
|---|---|---|---|---|---|---|---|
| gpSP | gint dev + direct OS addrs | NOR block map (P1), zero-copy pages, BFile fallback, refresh after writes | linked `.cgba.highbss` 0x8c200000–0x8c655300, X/Y/IL-RAM | gint r61524 window + DMA strips from XRAM/YRAM | gint keydown | custom F5 236/118 MHz | 2.00 only |
| NESizm / Prizoop | libsyscall-cw | NOR block table → memcpy into 8 KiB/4 KiB LRU banks; zx7 pages (Prizoop) | stack + `sys_malloc` | OS DefineDMARange + own DMAC; 0x2C/0x202 select | KEYSC direct | Ptune2 (likely inactive) [G] | 2.00, 2.10 |
| fxcraft | libsyscall-cw | whole files via `Bfile_ReadFile_OS` | hard-coded 0x8C22–0x8C5B…, ILRAM | as NESizm | KEYSC direct | — | 2.00, 2.10 |
| KhiCAS | PrizmSDK libfxcg + direct addrs | miniz-compressed code decompressed to RAM | 0x8c200000+ and 0x8c780000+ | OS | GetKeyWait_OS | prompt | 2.00 (dynload mpm.bin) |
| PythonExtra / OutRun | gint dev | gint stdio | `kmalloc_max("_ld1")` | `dupdate` / `r61524_display` | gint | F4 (OutRun) | gint: 1.00, 2.00 |
| SquishIt | gint dev + zlib/LZ4 | fopen in one world switch | gint malloc | JustUI | gint | — | 2.00 |

---

## 4. Lessons for loading a multi-MB WAD on CG100

1. **Use `Bfile_GetBlockAddress` (OS 2.00: 0x80333cf2; OS 2.10: 0x8039675a; OS 1.00: 0x8023ffb2) per 4 KiB block. Do the whole query in one OS-world visit.** **[V]** gpSP; addresses from `/Users/cheparukhin/casio-mpm-220/ref/mpm/casiowin.yaml:37`
   - With gint: one `gint_world_switch(GINT_CALL(fn, ctx))` where `fn` loops over all blocks.
   - This matters doubly when overclocked, because of the RTC drift.
2. **Accept only P1/P2 pointers that are 4-aligned. Use the P1 (0x8…) alias for cached reads. Treat P0 results as "not mappable".** **[V]** gpSP field crashes with TEA 0x001129xx
3. **Lumps that cross non-contiguous blocks** need either a copy (NESizm-style LRU or a CGDoom-style fragment table) or a fragment-aware accessor. CGDoom already has both a lump cache and `CGD_Frag`. **[V]** (CGDoom); [G] for the reuse.
4. **Never write to the mapped region. After any storage write (savegame, config, demo), re-query the block table** before touching WAD pointers again. Fugue may relocate records of other files. **[V]** gpSP
   - Simplest policy [G]: write files only from the menu or at level transitions, then rebuild the table.
5. **Invalidate the operand cache (`ocbi` per 32 B line) for re-mapped blocks.** **[V]** gpSP
6. **Unaligned 16/32-bit loads fault on hardware (EXC 0x0E0)** but not in casio-emu. CGDoom's "TrustUnalignedLumps" option is a known hazard. **[V]** gpSP note; CGDoom option
7. **Storage budget.** 4.5 MB total ⇒ ship the 3.39 MB stripped `doom.wad`. Optionally support a compressed WAD format (per-lump LZ4 or zx7 pages à la Prizoop `.gbz`) decompressed into the ~5.5 MiB `_ld1` heap. **[G]** The design is a suggestion; the numbers are [V].
8. **Code runs from RAM on the CG100** (MPM copies the whole image and maps it with 64 KiB TLB pages), not from flash as on the CG50. **[V]** `/Users/cheparukhin/casio-mpm-220/ref/mpm/main.c:241-263`
   - **[G]** Expect faster instruction fetch than the CG50 and no ROM TLB-miss stalls. gpSP even copies hot code into RAM on the CG50 (`hottext.c`) to get this effect.

---

## 5. Keyboard on the CG100 (raw matrix and gint names)

"Code" = PRGM_GetKey / `keyDown_fast` numbering (col×10+row, row 9 is the top row), as used by CGDoom, NESizm and fxcraft. CG50 key at the same physical position is in parentheses. **[V]** `gpsp-sh4-jit/ports/fxcg100/fxcg100_keys.c:60-84`, `gint/src/keysc/keydev.c:49-61`, beta announcement topic18626.

| Row | col 7 | col 6 | col 5 | col 4 | col 3 | col 2 |
|---|---|---|---|---|---|---|
| 9 | 79 **ON** (F1) | 69 **HOME** (F2) | 59 **PREVTAB / ⇤** (F3) | 49 **UP** (F4) | 39 **NEXTTAB / ⇥** (F5) | 29 **PAGEUP** (F6) |
| 8 | 78 **SETTINGS** (SHIFT) | 68 **BACK** (OPTN) | 58 **LEFT** (VARS) | 48 **OK** (MENU) | 38 **RIGHT** (←) | 28 **PAGEDOWN** (↑) |
| 7 | 77 **SHIFT** (ALPHA) | 67 **ALPHA** (x²) | 57 **VARIABLE** (^) | 47 **DOWN** (EXIT) | 37 **CATALOG** (↓) | 27 **TOOLS "…"** (→) |
| 6 | 76 X,θ,T | 66 FRAC | 56 √ | 46 ^ | 36 x² | 26 eˣ (EXPFUN) |
| 5 | 75 , | 65 sin | 55 cos | 45 tan | 35 ( | 25 ) |
| 4 | 74 7 | 64 8 | 54 9 | 44 DEL | 34 AC (KEYSC row 0, as CG50 AC/ON) | – |
| 3 | 73 4 | 63 5 | 53 6 | 43 × | 33 ÷ | – |
| 2 | 72 1 | 62 2 | 52 3 | 42 + | 32 − | – |
| 1 | 71 0 | 61 . | 51 ×10ˣ | 41 FORMAT (was (−)) | 31 EXE | – |

Notes:
- AC is tested by MPM as `*(u16*)0xa44b0000 == 1` (KEYSC KIUDATA0, row 0) (`/Users/cheparukhin/casio-mpm-220/ref/mpm/main.c:192-200`). gpSP maps skin code 34 to `KEY_ACON`. **[V]**
- ON is at the old F1 matrix position. gint dev reads it as `KEY_ON`. Old gint builds did not see it. **[V]**
- `GetKey` cannot see ON; `GetKeyWait_OS` can (Parisse). **[V]**
- Default game layouts used by precedents **[V]**:
  - gpSP: A=OK, B=CATALOG, L/R=PrevTab/NextTab, Select/Start=PgUp/PgDn, menu=ON.
  - NESizm: A=SHIFT(77), B=ALPHA(67), D-pad=arrows, menu/exit=HOME(69).
- **[G]** For CGDoom the obvious mapping:
  - Arrows = move/turn, OK or SHIFT = fire, ALPHA = use.
  - PrevTab/NextTab = strafe, PgUp/PgDn = weapon cycle, BACK = menu.
  - **HOME = quit to MPM**, because there is no OS menu.

---

## 6. Overclocking on the CG100

| Preset | Source | FLLFRQ | FRQCR | CPU | SHW | Bus/SDRAM | Pφ |
|---|---|---|---|---|---|---|---|
| default / gint F1 | gint `settings_fxcg100[0]` | 0x4384 | 0x0F011112 | 117.96 MHz | 58.98 | 58.98 | 29.49 |
| gint F2 | | 0x4384 | PLL16, /4,/8,/8,/8 | 58.98 | 29.49 | 29.49 | 29.49 |
| gint F3 | | 0x4384 | PLL26, /4,/8,/8,/8 | 95.85 | 47.9 | 47.9 | 47.9 |
| **gint F4 = F5** | `overclock.c:448-505` | 0x4384 | PLL32, /2,/4,/8,/16 | **235.93** | 117.96 | **58.98** | 29.49 |
| gpSP "Ptune4 F5" | `gpsp .../overclock.c:17-28` | 0x4384 | 0x1f001103 | 235.93 | 117.96 | **117.96** (CS3WCR 0x24d2) | 29.49 |

- FLL = 32768 Hz × (900/2), because SELXM=1 halves the ratio (`gint/src/cpg/cpg.c:76-97`). **[V]**
- gint comment on F5: the Graph Math+ "cannot handle the higher bus speed". **[V]**
- CalcLoverHK collects per-unit SDRAM results; some units pass "F5" (forum p17). **[V]**
- Xtune "Ptune4 MPM" is the CG100 build of the overclocking/benchmark tool (`CalcLoverHK_Xtune/config.h.in:47-58`). **[V]**
- gint's world switch restores the OS clock and the add-in clock on every switch. Many switches cause RTC drift (gpSP). **[V]**
- **[G] Recommendation:** default to gint F4 (CPU ×2, bus unchanged). Offer the 117.96 MHz-bus F5 only as an opt-in with a warning.

---

## 7. CG100-specific pitfalls (collected)

1. **No syscall table.** Anything that jumps to 0x80020070 (libfxcg stubs, `crt0`'s `GlibAddinAplExecutionCheck` 0x29, gint API 0) crashes or misbehaves. **[V]**
2. **OS-version-specific entry points.**
   - Upstream gint dev handles only `"01.00"` and CG100 `"02.00"`; everything else falls to API 0 (`gint/src/kernel/kernel.c:47-56`).
   - MPM main handles 1.00/2.00/2.10, but 2.00.1 is a TODO.
   - libsyscall handles 2.00/2.10, defaulting to 2.00.
   - **[G]** Check the full 10-character version string and refuse unknown OSes with a message.
3. **Address-table discrepancy.** For OS 2.10 `BFile_Write`, mpm's `casiowin.yaml:35` says 0x80396a92. libsyscall/NESizm and Kin1009's gint say **0x80396a06**. **[V]**
   - Every other BFile entry differs by exactly +0x62A68 between 2.00 and 2.10. 0x80333f9e + 0x62A68 = 0x80396a06, so **[G]** 0x80396a06 is right and the mpm entry is probably wrong or a different function.
4. **`_ld1` vs hard-coded 0x8c200000** (see §2.3). The dynload MPM passes no `_ld1`. The main MPM does, and gint writes heap metadata at 0x8c200000. **[V]** for the code; [G] for the conflict scenario.
5. **Add-in code sits at the top of RAM.** A fixed high arena can overwrite your own code once the binary grows. gpSP's "0x8c655300 ceiling" is an instance of this. **[G]** (§3.1)
6. **NULL is mapped** (virtual page 0 → flash at physical 0, user RW) by MPM. Null-pointer bugs do not fault. **[V]** `/Users/cheparukhin/casio-mpm-220/ref/mpm/main.c:262-263`
   - **[G]** Stray writes there hit the NOR bus.
7. **`Bfile_GetBlockAddress` may return P0 addresses**, and block addresses change after any FS write. **[V]** gpSP
8. **OS draws its file-busy indicator into LCD GRAM** during BFile calls. **[V]** gpSP
9. **New display controller** on 2025-boot units (ID 0x32/0x52). Use 0x2C instead of 0x202, and the gint ≥ 2026-03 r61524 driver. **[V]**
   - MPM cannot be installed on those units yet, but gint is ready.
10. **No return-to-menu.** HOME in GetKey is a no-op, `gint_osmenu()` is a no-op, `gint_setrestart` is disabled. Provide an explicit Quit that returns from `main`. **[V]**
11. **RAM is not zeroed between launches** (OS fill 0x55; MPM leaves leftovers). **[V]**
12. **The running native app is not closed.** Fewer OS-heap resources are available (Python/MEMORY). **[V]**
13. **Writes.**
    - Prefer an "overwrite an existing same-size file" fast path (gpSP buckets). Create files with their final size.
    - Avoid many tiny writes or appends. The first creation of a big save file can be slow.
    - Generated `.g3a` files get type 0x01 (MPM accepts them since a66f1d4). **[V]**
14. **Wait for key release before returning to MPM**, otherwise the held key acts in MPM/OS. **[V]** gpSP commit 65c1edf
15. **Misaligned loads fault on hardware but not in casio-emu.** **[V]**
16. **One world switch per batch, not per block or file**, especially when overclocked. **[V]**

---

## 8. Reported performance numbers (CG100 hardware unless stated)

| Program | Number | Conditions | Source |
|---|---|---|---|
| gpSP JIT | "50+ FPS" | Advance Wars, Ace Attorney; overclocked (Ptune4) | Cemetech p=317309 [V claim] |
| gpSP JIT | 44 → 47 emulated fps | Pokémon Emerald, Oldale Town, two builds | docs/performance-history.md [V] |
| gpSP | `-O3` on hot files slightly *slower* than `-O2`, and +40 KB | CG100 hardware | CMakeLists.txt:575-580 [V] |
| gpSP | end-of-frame strip DMA faster than per-16-row streaming | CG100 hardware | CMakeLists.txt:363-366 [V] |
| NESizm | "most titles at 60 FPS, no overclocking" | CG50 and CG100 | README [V claim] |
| zlib | 500 KB decompress in RAM ≈ 300 ms | Slyvtt benchmark (CG50-class) | MPM thread p3 [V claim] |
| storage reads | ~200 KB read "instant", < 200 ms | MPM icon loading | MPM thread p3 [V claim] |
| SquishIt | compressed g3a = 40–50% of original on average (zlib max 58.6% saving) | 31 add-ins, 10 MB | topic19185 [V] |

---

## 9. How a user installs MPM and add-ins on a CG100 (for our README)

> State as of 2026-10-08. Sources: Planète Casio topic18534 (all 21 pages), topic18626, KhiCAS doc, gpSP README. **[V]** unless marked.

### 9.1 Check compatibility first
1. **Supported family:** fx-CG100, Graph Math+, fx-1AU Graph (the AU model was confirmed working on 2026-01-19).
2. **OS version** (Settings → About, or the test menu):
   - Required: **02.00.xx0x**, e.g. 02.00.0202 or 02.00.2202.
   - **Not yet supported by the public installer:** 01.00 (prototypes and early units; update to 2.00 first), **02.00.xx1x** (e.g. 02.00.0212), **02.10.x** and **02.20.x**. The installer prints "Unknown OS" or the calculator hangs.
3. **Bootcode / display check.** With the calculator off, hold **HOME + ×10ˣ + AC**, press **ON**. In the popup press **SETTINGS** then **9** (nothing is erased), then **4 (VERSION)**. Read **DateO / DateA**:
   - DateA `2017.0106.2008` (old display): you may **downgrade** an unsupported 2.10/2.20/xx1x OS to 02.00.0202 (§9.4) and then install MPM.
   - DateA `2025.0304.1404` (new display): **do not downgrade**, the screen stays black. Wait for a newer MPM installer. (CalcLoverHK and Lephenixnoir, p21 08/10/2026.)
   - Some users press only ×10ˣ+AC+ON; Lephe's instruction includes HOME.

### 9.2 Install the MPM OS patch (Windows only; a VM works)
1. Get `mpm-installer-1.0bw.exe` (zip on Planète Casio, e.g. `https://www.planet-casio.com/storage/forums/mpm-installer-1.0bw-199064.zip`) and `mpm.bin`.
   - Use the forum zip, the release asset of the add-in you use, or build `../casio-mpm-220/ref/mpm` with `fxsdk build-cg` using fxsdk/fxlibc @dev.
2. Requirements: 64-bit Windows. Windows 8+ has WinUSB built in; on Vista/7 install WinUSB via **Zadig**. In a VM, attach the calculator's USB device to the VM.
3. Connect the USB cable. On the calculator popup choose **OS Update** ("2: Mise à jour SE").
4. Open `cmd` and run `mpm-installer-1.0bw.exe`.
5. When the PC shows **"Communicating more..."**, the calculator reconnects under a new identity ("CASIO fx-CG family on gint") and shows **"Press OK to continue"**. Press **OK/EXE within about 10 s**.
6. Follow the on-calculator steps: roughly **TOOLS → SETTINGS → RESTART**. Press the RESTART button on the back with a pen; alternatively unplug USB and take out one battery.
7. A final PC message `receive_on_link_medium: USB device is no longer available` is harmless if the calculator completed the steps.

### 9.3 Install MPM's menu program and add-ins
1. Connect in **USB flash drive / file transfer** mode.
2. Copy **`mpm.bin`** to the **root** of storage memory, *not* into `@MainMem`.
3. Copy the add-in **`.g3a`** files to the root, plus any data files. For us: the WAD.
4. Disconnect safely. On the **HOME** screen press **TOOLS ("…")**. The MPM add-in grid opens. Use the arrows, then **OK/EXE** to launch. **VARS** shows details. **SETTINGS** receives an add-in over USB directly into RAM (developer "Add-in Push").
5. Updating MPM later = overwrite `mpm.bin`. **Any official OS update removes the patch**; re-run the installer.
6. MPM does not run in exam mode.
7. Exiting an add-in returns to the MPM menu. **HOME inside an add-in does nothing unless the add-in handles it.** Use the add-in's own Quit.
8. Some add-ins need a specific `mpm.bin`:
   - KhiCAS and gpSP ship the "dynload" build (12,820 B).
   - [G] Check which one our port needs and document it.

### 9.4 Troubleshooting
- **Downgrade / recovery through OS ERROR mode.** Use it for the OS 2.00 updater (Planète Casio "showid=153") or to go back to 2.10 after a black screen. Method from CalcLoverHK's topic16402 as summarised by Charlesbug:
  1. Press **HOME, 4, AC** + RESTART.
  2. Release RESTART and wait 1 s.
  3. Release all keys.
  4. Hold **9** for 1 s.
  5. Hold **×** until "OS ERROR" appears.
  6. Connect and run the updater.
  - If it keeps failing: leave the calculator off ≥ 1 hour first (Parisse), try another PC, or delete stale Casio USB devices in Device Manager (`pnputil /remove-device ...`).
  - **[V]** Mac hardware test, 14 Oct 2026: Casio's Mac updater restored stock 2.20 over casio-mpm-220's patched 2.20 from OS ERROR, on the first attempt after the calculator had been off overnight. Attempts made minutes after a RESTART failed at the first handshake: the recovery USB link handed back the host's own packets. Details are in casio-mpm-220 `docs/REVIEW.md`, "Recovery on hardware".
- **Zadig side effect.** If Zadig assigned WinUSB to the calculator, the official OS updater cannot see it. The installer and OS Update share VID:PID 07cf:6102. Uninstall that device and its driver (Device Manager, "Show hidden devices") to restore the official updater.
- **Black screen + reboot when launching an add-in** = the add-in is not CG100-compatible. No damage (Lephe).
- **No public Linux installer** (a dev build exists but is private).

### 9.5 Developer fast loop
- MPM main has **Add-in Push** built in: press SETTINGS in MPM, then on the PC run `Add-in_Push/cli` (`main.c`, built on Cahute). It strips the 0x7000 g3a header and sends a 4-byte big-endian size plus the body; maximum 6 MB. **[V]** `CalcLoverHK_Add-in_Push/cli/main.c`; `/Users/cheparukhin/casio-mpm-220/ref/mpm/main.c:473-484`
- `fxsdk build-cg-push` / `fxlink -pw` target the CG50 Add-in Push helper. **[V]** `fxsdk/fxsdk/fxsdk.sh:36-41,292-300`
  - **[G]** It may need the CLI above for the CG100.

---

## 10. Open questions / items to verify on hardware

- **[G]** Is `0x8c200000 .. 0x8c7fffff`, minus our code and mpm.bin, really free during an add-in on OS 2.00 *and* 2.10? MPM's authors treat it as free (they pass it as `_ld1`). gpSP's resets are better explained by self-overlap.
- **[G]** Does `BFile_Read` on CG100 OS 2.00 return bytes read (MPM assumes yes) or 0?
- **[G]** Fragmentation profile of a freshly copied 3.4 MB WAD on a 4.5 MB Fugue volume: how many 32 KiB runs are contiguous? gpSP only says "contiguous pages stay direct".
- **[G]** Exact free-storage figure after installing mpm.bin and our g3a; 4,793,072 B is a second-hand report.
- **[G]** Do the current NESizm/Prizoop Ptune2 presets do anything on the CG100? The source suggests not.

---

## Sources

Local code (paths relative to `/Users/cheparukhin/casio_doom/ref/`, except the
explicit absolute MPM paths):
- `/Users/cheparukhin/casio-mpm-220/ref/mpm/main.c`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/start.c`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/loader.c`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/addins.c`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/casiowin.yaml`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/loader-cg.x`, `/Users/cheparukhin/casio-mpm-220/ref/mpm/reloc.S`; `git show origin/dynload:main.c`
- `gint/src/kernel/{kernel.c,start.c,hardware.c,osmenu.c,syscalls.S}`, `gint/src/cpg/{overclock.c,cpg.c}`, `gint/src/keysc/keydev.c`, `gint/include/gint/{keycodes.h,hardware.h,mmu.h}`, `gint/src/render-cg/dvram.c`, `gint/src/kmalloc/kmalloc.c`, commit 2700096
- `extra/gpsp-sh4-jit/ports/fxcg100/gint-gpsp/{CMakeLists.txt,fxcg100_gpsp.ld,src/nor_rom.c,src/nor_filestream.c,src/overclock.c,src/no_osheap.c,src/gint_platform.c,src/main.c,src/gpsp_runner.c}`, `ports/fxcg100/{README.md,fxcg100_keys.c,fxcg100_config.c,fxcg100_platform.h}`, `docs/{fxcg100-port-plan.md,performance-history.md}`, `README.md`
- `extra/CalcLoverHK_nesizm/{CMakeLists.txt,casiowin.yaml,src/nes_cart.cpp,src/nes.h,src/main.cpp}`, commit 3e7a9eb; `extra/CalcLoverHK_prizoop/{src/mbc.cpp,src/rom.cpp,src/display_directlcd.cpp,src/screen_play.cpp}`, commit 5a4f353
- `extra/CalcLoverHK_fxcraft/{CMakeLists.txt,prizm.ld,casiowin.yaml,src/engine.c,src/fastBdisp.c,src/DMA_Transfer.c,src/controls.c,src/controls.h}`, commits 46b4922, 99eefab
- `extra/CalcLoverHK_libsyscall/{CMakeLists.txt,prizm.ld,libcw/crt0.S,libcw/reloc.S,libcw/casiowin.S,libcw/GetKey.c,libcw/GetKey.h.in}`
- `extra/CalcLoverHK_Add-in_Push/{README.md,cli/main.c}`; `extra/CalcLoverHK_Xtune/{README.md,config.h.in}`
- `extra/Kin1009_gint` (commits ee12efd, 376c55c), `/Users/cheparukhin/casio-mpm-220/ref/extra/Kin1009_mpm`, `/Users/cheparukhin/casio-mpm-220/ref/extra/Kin1009_mpm-cfg/README.md`
- `extra/Slyvtt_SquishIt/src/core/squishit.c`, `extra/Slyvtt_OutRun/src/main.cc`
- `CGDoom/cgdoom/{cgdoom.c,cgdoom-kbd.c,cgdoom-frag.h}`, `git show origin/in330-backup:README.md`
- `extra/ClassPadDev_CPDoom/{README.md,linker.ld}`

Web:
- [Planète Casio — MPM : Mod add-ins Math+ (topic18534, pages 1–21)](https://www.planet-casio.com/Fr/forums/topic18534-1-mpm-mod-add-ins-math.html)
- [Planète Casio — Bêta-test public MPM (topic18626)](https://www.planet-casio.com/Fr/forums/topic18626-1-beta-test-public-mpm-mod-graph-math-et-fx-cg100-pour-lancer-add-ins-g3a.html)
- [Planète Casio — KhiCAS thread (topic15269)](https://www.planet-casio.com/Fr/forums/topic15269-12-khicas-add-in-calcul-formel-pour-graph-90e-et-35eii.html)
- [Planète Casio — Squish It !! (topic19185)](https://www.planet-casio.com/Fr/forums/topic19185-1-squish-it-un-addin-pour-les-ecrabouiller-tous.html)
- [KhiCAS documentation (xcas backup HTML)](https://xcas.univ-grenoble-alpes.fr/backup/casio/khicasioen.html) and [PDF](https://xcas.univ-grenoble-alpes.fr/backup/casio/khicasioen.pdf)
- KhiCAS CG100 files: `https://www-fourier.univ-grenoble-alpes.fr/~parisse/casio/mpm/{mpm.bin,khicas1z.g3a,cas100a.mzs,cas100b.mzs}` (HEAD requests only)
- [Cemetech — gpSP for CASIO fx-CG100 thread](https://www.cemetech.net/forum/viewtopic.php?p=317309)
- [GitHub — KaraRyougi/gpsp-sh4-jit](https://github.com/KaraRyougi/gpsp-sh4-jit) (release v00.10.0804 assets: `gpSP-v00.10.0804-fx-CG100.g3a` 487,996 B, `mpm.bin` 12,820 B)
- [TI-Planet — MPM 1.0 news](https://tiplanet.org/forum/viewtopic.php?f=51&t=26948)
- Planète Casio Gitea: `https://git.planet-casio.com/{PlaneteCasio/mpm, CalcLoverHK/*, Kin1009/*, Slyvtt/*, Lephenixnoir/CGDoom-WADs}` (API `/api/v1/repos/Lephenixnoir/CGDoom-WADs/contents` for WAD sizes)
- [GitHub — TheRainbowPhoenix/PythonExtra `ports/sh/main.c`](https://github.com/TheRainbowPhoenix/PythonExtra)
- Storage-size reports: hpmuseum.org posts 203160/203451/203571 (returned 403 to direct fetch; figure taken from a search-engine summary), plus the Planète Casio threads above.
