# fx-CG100 / Graph Math+ hardware and the MPM add-in environment (research for a CGDoom port)

Revisions inspected: mpm `cbd1a41` (main = origin/dev, "MPM 1.1", 2026-08-17), gint `463f180` (dev, same as the remote dev head on 2026-10-09; gint 2.11.0), fxsdk `a6e46eb` (dev). I also cloned unmerged gint PR heads into `/Users/cheparukhin/casio_doom/ref/extra/gint-prs.git` (PR 50 = CalcLoverHK's CPG/overclock rework, PR 62 = "fygue" direct-flash FAT reader) and CalcLoverHK's Ptune4 repo into `/Users/cheparukhin/casio_doom/ref/extra/Xtune`.

Tags used below:
- **[V]**: verified in source code or a primary, reliable post. The file and line, or the source, is given next to it.
- **[V-forum]**: stated by MPM or gint developers (Lephenixnoir, CalcLoverHK, Critor) in the forum threads.
- **[G]**: my guess or inference. Treat it as unverified until it is tested on hardware.

---

## 0. Most important findings

1. **Who can run add-ins today [V-forum, as of 2026-10-08].** MPM can only be installed on OS **02.00.xx0x**: `02.00.0202` on fx-CG100 and `02.00.2202` on Graph Math+. Units with the **2025 bootcode** (`DateA 2025.0304.1404`) have a new display and cannot downgrade to that OS. The same applies to units sold with `02.00.0212` ("2.00.1"), `02.10.x` or `02.20.x`. Those units cannot run MPM yet. Any OS update removes MPM. Exam mode disables the MPM menu.
   - Source: Planète Casio thread topic18534, page 21, posts by Lephenixnoir and CalcLoverHK on 2026-10-08.
   - In practice, the Doom port can target only old-display units on OS 2.00.x202 for now.
2. **There are two MPM memory layouts in the wild, and they are very different.**
   - **(A) The official public `mpm.bin`.** It is the attachment `mpm-bin-199226.zip` (9,305 bytes) posted on 2025-02-27 and is still the one linked from the first post. It is approximately git `68e7f6e`. The heap arena is a **fixed 2 MiB** and the g3a limit is **3 MiB**.
   - **(B) git main "1.1"** (`cbd1a41`, Aug 2026). It places the add-in dynamically at the top of RAM and gives the add-in a heap of about **5.3 MiB** for a 600 KB add-in. It allows a binary up to **0x370000** bytes.
   - `mpm.bin` is a user-replaceable file in storage, so the port could ship or require a newer build. Either way, it must take its heap from the arenas that the loader announces, which gint does automatically. Never hardcode RAM addresses.
3. **The add-in code runs from RAM, not flash.** MPM copies the whole post-header binary into SDRAM and maps it at 0x00300000 with 64 KiB pages. There is no demand paging, so all code and rodata count against RAM.
   - This removes flash wait states for code (good for speed).
   - The g3a must be smaller than the mapped window.
   - Reaching an unmapped page in 0x00300000+ crashes, because gint's TLB-miss path calls a CG50 syscall that does not exist on the CG100.
4. **Storage is only about 4.5 MiB** (4,793,072 bytes reported). The shareware `doom1.wad` (4,196,020 B) plus a ~350 KB g3a barely fits. A full `doom.wad` (11.16 MB) or `doom2.wad` (14.6 MB) cannot fit.
5. **The CPU, MPU, SDRAM and display controller are the same as on the CG50** on old-display units (SH7305, R61524, 8 MiB SDRAM). Only the keyboard, the OS and the absence of a syscall table differ. The default clock is 117.96 MHz CPU / 58.98 MHz bus.
   - gint dev caps the CG100 at 235.93 MHz CPU but keeps the bus at 58.98 MHz.
   - The community "Ptune4 F5" preset (235.93 MHz CPU, 117.96 MHz bus, SDRAM TRC=6) is used by KhiCAS on CG100. It is in unmerged gint PR 50 and can be applied by hand with `cpg_set_overclock_setting()`.
6. **gint dev knows the syscall-free OS entry points only for OS 1.00 and 2.00.**
   - On OS 2.10, gint falls back to the CG50 syscall table at 0x80020070, which does not exist, so the first BFile or OS-malloc call crashes. MPM main already has the 2.10 addresses.
   - On "2.00.1" (`02.00.0010` internally), gint compares only "02.00" and would use the 2.00.0 addresses, which are wrong.

---

## 1. CPU, clocks, caches, on-chip memories, DMA, SPU

### 1.1 MPU identity
- **[V-forum]** Lephenixnoir, 2024-12-02 (topic18534 p.1): "la Math+ est avant tout un reskin de la 90+E… Le matériel est le même sauf le clavier, le CPU est le même, MPU est le même." So it is the SH7305 (a custom Renesas SH-4A, closest documented relative SH7724), big-endian.
- **[V]** gint does not probe the CG100 MPU. On every CG it simply sets `gint[HWMPU] = HWMPU_SH7305` and stores PVR/PRR (`gint/src/kernel/hardware.c:124-128`).
  - The CG100 is detected by model ID at 0x80000300: anything that is not "810A" (CG50), "811A" (Graph 90+E) or "810F" (CG50 AU) is treated as a CG100 (`hardware.c:139-146`; commits 31f2506 and 982055c, April 2026).
  - Older gint dev builds used the OS version and date instead. Build against gint dev ≥ 982055c.
- **[V]** The toolchain targets SH-4A without an FPU and with DSP instructions enabled in the assembler: `-m4-nofpu -mb -Wa,--dsp` (`fxsdk/fxsdk/cmake/FXCG50.cmake:20`). There is no CG100-specific fxSDK target; the same `fxsdk build-cg` g3a runs on CG50 and CG100. There is no FPU, so Doom's fixed-point code is a good fit.
- **[V-forum/Cemetech]** The SDRAM is the same as the CG50's: ESMT **M12L64164A**, 64 Mbit = 8 MiB, x16, in **-7TI** (143 MHz rated) or **-5TI** (200 MHz rated) grades. Serial numbers with "A3C" or "A2C" were seen with 5TI. Source: CalcLoverHK, Cemetech "Casio fx-CG/CP overclocking insights", p=311770.

### 1.2 Clock tree and default speed
- **[V]** gint computes frequencies as follows (`gint/src/cpg/cpg.c:69-103`):
  - base = 32768 Hz × FLL × PLL.
  - FLL = FLLFRQ.FLF, halved when SELXM=1. Every preset uses `FLLFRQ = 0x4000 + 900`, so FLL = 450 and the FLL output is 14.7456 MHz.
  - Each divider is `base >> (field+1)`.
- **[V]** gint's CG100 table is `settings_fxcg100[5]` (`gint/src/cpg/overclock.c:448-507`; comment: "This is fxcg50[0,1,2,3,3]"). The computed frequencies:

| Preset | FRQCR | PLL | CPU Iφ | Sφ | Bus Bφ (SDRAM, display bus) | Pφ (timers, USB) |
|---|---|---|---|---|---|---|
| F1 = OS default [G: assumed identical to the CG50, as gint assumes] | `0x0F011112` | ×16 | **117.96 MHz** | 58.98 | **58.98** | 29.49 |
| F2 | ×16, I/4, S/8, B/8, P/8 | ×16 | 58.98 | 29.49 | 29.49 | 29.49 |
| F3 | ×26, I/4, S/8, B/8, P/8 | ×26 | 95.85 | 47.92 | 47.92 | 47.92 |
| F4 | `0x1F001203` | ×32 | **235.93** | 117.96 | **58.98** | 29.49 |
| F5 (CG100) = F4 | `0x1F001203` | ×32 | 235.93 | 117.96 | 58.98 | 29.49 |
| (CG50-only F5, not used on CG100) | `0x18001102` | ×25 | 184.32 | 92.16 | 92.16 | 46.08 |

- **[V]** `overclock.c:495-496`: "CLOCK_SPEED_F5 is made identical to CLOCK_SPEED_F4 because clearly the Graph Math+ cannot handle the higher bus speed." (commit 232a419, 2025-01-10).
- **[V-forum/Cemetech]** CalcLoverHK later found that the bus limit comes from SDRAM timing **CS3WCR.TRC**. Raising TRC from 4 to 6 cycles lifted his Math+ from 105 MHz to 146 MHz maximum RAM clock, and 5TI units exceed 150 MHz with TRC=9.
  - His **new F5** is in unmerged gint PR 50 (`refs/pull/50:src/cpg/overclock.c:373-433`, `settings_fxcg_50_100`), shared by CG50 and CG100. KhiCAS uses it ("236 MHz CPU + 118 MHz RAM", Cemetech post of 2025-11-23).
  - The register values, computed from the PR macros:
    - `FLLFRQ=0x4384`, `FRQCR=0x1F001103` (PLL×32, I/2, S/4, B/4, P/16 → 235.93 / 117.96 / **117.96** / 29.49 MHz)
    - `CS0BCR=0x44900400`, `CS2BCR=0x36DA3400`, `CS3BCR=0x04904400`, `CS5aBCR=0x17DF0400`
    - `CS0WCR=0x000004C0` (ROM wait 12), `CS2WCR=0x000003C0`, **`CS3WCR=0x000024D2`** (TRP2, TRCD2, CL2, TRWL2, **TRC6**), `CS5aWCR=0x000203C1`.
  - PR 50 also writes FLLFRQ in two stages and fixes "soft brick due to insufficient ROM waits" (08cab0f).
  - Apply these values with `cpg_set_overclock_setting()` (public, `gint/include/gint/clock.h:145-160`). gint dev's `cpg_low_level_set_setting` (`overclock.c:97-119`) also handles SDMR3 CL2/CL3.
  - **[G]** Pφ stays at 29.49 MHz, so timers and USB are unaffected. Make overclocking user-selectable; per CalcLoverHK, a few Math+ units failed the old Ptune3 F5.
- **[V]** The clock is not reset on world switch or on exit (`clock.h:136-137`). Save the setting with `cpg_get_overclock_setting()` at start and restore it on exit.
- **[V-forum, search-engine excerpt of CalcLoverHK's hpmuseum post; full text not fetchable (403)]**
  - Full-screen `dupdate` takes about 11 ms at default and 6.91 ms with the Ptune3 F5 bus (+59%).
  - Dhrystone goes from 118k/s to 204k/s (+73%).
  - The bus is the bottleneck on the fx-CG.

### 1.3 Caches
- **[V]** The cache line is 32 bytes. MPM writes back with `ocbwb` every 32 bytes (`mpm/main.c:272-274`), and `reloc.S:11-36` copies whole 32-byte lines.
- **[G]** Cache sizes and associativity of the SH7305 are not documented anywhere I could find. The SH7724 and SH7730 have 32 KiB I-cache + 32 KiB O-cache, 4-way. Measure on hardware, for example with a strided-read latency sweep.
- **[V]** gint puts its VRAM in uncached P2 space: `region = (region & 0x1fffffff) | 0xa0000000` (`gint/src/render-cg/dvram.c:114`). Do not render pixel by pixel into `gint_vram`. Render into a cached buffer, for example Doom's 8-bit screen, then convert and push.

### 1.4 On-chip memories (present and usable on CG100)
- **[V]** gint's fx-CG linker script, used unchanged for the CG100, declares the regions below (`gint/fxcg50.ld.c:19-31`):
  - `ilram (rwx): o = 0xe5200000, l = 4k`
  - `xyram (rwx): o = 0xe500e000, l = 16k`
- **[V]** At every start on SH4, `gint_load_onchip_sections()` clears and loads them on all SH4 except fx-CP (`gint/src/kernel/start.c:97-111`).
- **[V]** gint puts its interrupt-handler code (`.gint.mapped`, `gint/src/kernel/inth.S:183`) and a 32-byte DMA memset buffer (`gint/src/dma/memset.c:4`) in ILRAM. gint add-ins run on the CG100, so ILRAM and XYRAM exist and work there **[V by inference]**.
- **[V, CG50 doc, prizm.cemetech.net "On-chip Memory"]** On-chip memory map:
  - RS memory 0xFD800000–0xFD803FF8 (16 KiB) holds OS values and code, **including the flash-write code**. Do not touch it.
  - IL memory 0xE5200000–0xE5200FFF, 4 KiB.
  - X memory 0xE5007000–0xE5008FFF, 8 KiB. CGDoom uses 0xE5007000; gint's alias is 0xE500E000.
  - Y memory 0xE5017000–0xE5018FFF, 8 KiB. gint's alias is 0xE5010000.
  - gint's DMA translation accepts ILRAM 0xE5200000–0xE5204000 and XRAM/YRAM at 0xE5007000 / 0xE5017000 (`gint/src/dma/dma.c:47-56`).
- **[V]** World-switch caveat:
  - The default `onchip_save_mode = GINT_ONCHIP_REINITIALIZE` (`gint/src/kernel/world.c:63`). After **every world switch**, which includes every BFile call, ILRAM and XYRAM are reloaded from their initial images and runtime contents are lost.
  - Use `gint_set_onchip_save_mode(GINT_ONCHIP_BACKUP, buf20k)` (`GINT_ONCHIP_BUFSIZE = 20 KiB`, `gint/include/gint/gint.h:91-127`), or avoid world switches during gameplay.

### 1.5 DMA
- **[V]** The SH7305 DMAC sits at 0xFE008020 (`gint/include/gint/mpu/dma.h:91`). It has 6 channels and block sizes of 1, 2, 4, 8, 16 and 32 bytes (`gint/include/gint/dma.h`).
- **[V]** gint uses **channel 0 for the display** (`r61524.c:182-209`) and channel 1 for `dma_memcpy`/`dma_memset` (`dma/memcpy.c:19`). SPU memory 0xFE200000–0xFE3FFFFF must use 4-byte units (`memcpy.c:10-16`).

### 1.6 SPU / PRAM0
- **[V]** gint has an SPU driver (`gint/src/spu/spu.c`): SPU at 0xFE2FFC00, DSP0 at 0xFE2FFD00, DSP1 at 0xFE3FFD00, FSI at 0xFE3C0000. It saves and restores PBANKC/XBANKC on world switch, and `spu_configure()` is not called by default.
- **[V]** gint's kmalloc supports a PRAM0 arena with 32-bit-only access (commit d8005b5, `kmalloc/arena_gint.c:439`), but gint never creates it. The user must call `kmalloc_init_arena` and `kmalloc_add_arena` for 0xFE200000–0xFE228000.
- **[V]** CGDoom on CG50 uses PRAM0 at **0xFE200000–0xFE228000 (160 KiB)** for the WAD fragment map and 4-byte arrays (`CGDoom/src-cg/platform.h:59-60`).
- **[G]** PRAM0 should exist on the CG100 because the MPU is the same, but no source tests it under MPM. Probe it (32-bit write/read) before use. If the SPU module clock were stopped, an access might hang. There is no audio hardware to use anyway: the 3-pin port only disables exam mode (Critor, p.1).

---

## 2. Memory map and what an MPM add-in can use

### 2.1 Physical
- **[V]** 8 MiB SDRAM at physical 0x0C000000 (P1 0x8C000000, P2 0xAC000000). gint sets `gint[HWRAM] = 8 MiB` for the CG100 (`hardware.c:159`).
- Flash is on area 0 at physical 0 (P1 0x80000000 cached, P2 0xA0000000 uncached).
  - The OS header is at 0x80020000 and the OS version string at **0x80020020**.
  - The bootcode model ID is at 0x80000300.
- **[V-forum]** OS usage of RAM: "Il y a de la mémoire après les 2 premiers Mo" (Lephe, p.1). The OS uses 0x8C000000–0x8C200000.
  - Above 0x8C200000 it is free today, but "CASIO est quasiment certain d'utiliser toute cette RAM dans le futur". This is why the loader announces arenas (p.7).
- **[V]** Known OS variables in the low 2 MiB:
  - `CurrentAppIndex` at 0x8C0AFE28 (2.00) and 0x8C0B1308 (2.10).
  - "AppMemory" at **0x8C160000** (512 KiB, all versions) (`mpm/casiowin.yaml:69-71`).
- **[G]** By analogy with Simon Lothar's fx-CG20 memory map (system stack 0x880F0000–0x8815FFFF, add-in RAM 0x88160000–0x881DFFFF, heap 0x881E0000–0x881FFFFF), shifted to 0x8C…, the CG100 low RAM probably looks like this:
  - OS system stack 0x8C0F0000–0x8C160000
  - app/add-in RAM 0x8C160000–0x8C1E0000 (matches MPM's AppMemory)
  - OS heap 0x8C1E0000–0x8C200000 (128 KiB).

### 2.2 How MPM loads and runs an add-in (git main 1.1, `mpm/main.c`)
- **[V]** The add-in must be a `*.g3a` in the storage root (`\\fls0\*.g3a`, `main.c:34`), not a folder, with a size ≥ 0x7002 (`main.c:51-55`). Up to 256 add-ins are listed (`addins.c:5`). The add-in menu opens with **TOOLS** on the HOME screen. The forum menu also offers USB "push" (SETTINGS) and a mapping-debug launch (FORMAT).
- **[V]** `load_addin` (`main.c:208-283`):
  - `binary_size = filesize - 0x7000` (header skipped). If it is `> 0x370000`, the error is -1001 (`:211-213`).
  - `ramAddress = CW_AppMemory()`, which is **0x8C160000** for all three OS versions. `romAddress = (0x8C7F0000 - binary_size) & -0x10000` (`:219-225`). The else branch at `:226-230` is dead because AppMemory is never NULL.
  - The binary is read with `CW_BFile_Read(fd, romAddress, binary_size, 0x7000)` (`:245`).
  - MMU: **55 × 64 KiB** pages at 0x00300000 → romAddress (UTLB entries 0–54); 8 × 64 KiB at 0x08100000 → ramAddress (entries 55–62); **NULL page** 0x00000000 → 0xA0000000 (flash), 4 KiB (entry 63) (`:251-263`). Every entry is PR=3 (U:rw), C=1, WT=0 (write-back), SH=0 (`loader.c:54-92`). **[V]** Code and rodata are therefore writable, and NULL dereferences read flash bytes instead of faulting.
  - MMUCR when MPM enables the MMU: AT=1, SV=1 (single virtual memory), URB=54, URC=0 (`loader.c:15-38`). **[V]** The OS normally runs with the MMU off. MPM's own OS-call wrapper turns the MMU off around every OS call, because "The OS will crash in many spots if we keep it on" (`start.c:49-51`, `casiowin.S:43-63`).
  - **[V]** gint's wrapper does **not** do this (`gint/src/kernel/syscalls.S:158-195`). BFile works in practice. **[G]** Other OS functions called from gint might not.
- **[V]** `run_addin` (`main.c:345-360`) calls the code at 0x00300000 as `code(0x4d504d30 /* 'MPM0' */, loadinfo)` with this loadinfo array:
  ```
  { 0x01, 0x01,          // format version 1
    0x10, 0x8C200000,    // start of an available RAM area
    0x11, loadAddress,   // end of that area (= romAddress)
    0x00 }
  ```
  - The comment `// TODO: Move the stack pointer!!` (`:354`) means **the add-in runs on MPM's (= the OS's) stack**.
  - After return, MPM disables the MMU and redraws (`main.c:499-500`). On exit from the menu it deinstalls OS timers 5–9 (`:510-511`).
- **[V]** mpm.bin is loaded by the mod at 0x8C700000. In 1.1 it relocates itself to **0x8C7F0000** (64 KiB) (`loader-cg.x:5-9`, `reloc.S:4-47`). The add-in image ends at 0x8C7F0000 and must never touch 0x8C7F0000–0x8C800000.

### 2.3 How gint dev consumes this
- **[V]** `start()` passes `load_info` through only if `load_type == 'MPM0'` (0x4d504d30) and the pointer is 4-aligned (`gint/src/kernel/start.c:178-182`).
- **[V]** `kinit()` (`gint/src/kernel/kernel.c:43-196`):
  - **VBR** at the start of user RAM: `VBR = mmu_uram()`, the P1 address of the physical page behind 0x08100000 (`:84`, `mmu/mmu.c:20-39`). The linker script reserves 0x1400 (5 KiB) for it (`fxcg50.ld.c:26`, `ram: o = 0x08101400, l = 491k`).
  - `gint_stack_top = uram + uram_size - 0x4000` (`:85-86`). This is **only a canary location** (`world.c:151-161`): gint never sets r15. Under MPM the real stack is elsewhere, so the 16 KiB at the end of the uram is wasted and the overflow canary is meaningless.
  - Arena **"_uram"**, default: from `uram + (euram - 0x08100000)` to `uram_end` (`:113-123`).
  - Arena **"_ostk"** (the CG50 OS-stack region 0x8C0F0000 + 350 KiB) is **disabled on the CG100** (`:126-139`; commit a62ba8a: "the region might simply not exist").
  - Arenas **"_ld1".."_ld4"** come from the 0x10/0x11 pairs, are default, and use the gint allocator (`:141-166`). Note the bug: `count` is never incremented, so every arena is named "_ld1". This is harmless with a single pair.
  - Arena order for `malloc`: `_uram` → `_ld1` → `_os` (OS heap via the OS malloc, slot 7, default) (`kmalloc/kmalloc.c:11-23`, `arena_osheap.c:30-40`).
  - **VRAM**: `dvram_init()` allocates `396*224*2 + 96` = **177,504 B** from the first default arena with room, normally `_uram`, and uses it through P2 (uncached) (`render-cg/dvram.c:93-121`).

### 2.4 Virtual address map seen by the add-in (MPM 1.1)
| Virtual | Physical (P1 view) | Size | Content |
|---|---|---|---|
| 0x00000000–0x00000FFF | 0x80000000 (flash) | 4 KiB | NULL page, readable (writable through the TLB) |
| 0x00300000–0x0066FFFF | romAddress … +0x370000 | 55×64 KiB | g3a code, rodata and .data LMA (in RAM) |
| 0x08100000–0x0817FFFF | 0x8C160000–0x8C1DFFFF | 512 KiB | static RAM: VBR (5 KiB), .bss/.data/.gint.bss, then `_uram` arena, then 16 KiB unused "stack" reserve |
| (P1) 0x8C200000 – romAddress | – | variable | `_ld1` arena |
| (P1) r15 at entry | OS stack | unknown | **[G]** probably the OS system stack, about 0x8C0F0000–0x8C160000 (CG50 analogy); gint confirms only r15 ≥ 0x8C000000 (`hardware.c:136`) |

### 2.5 Concrete heap numbers
The arena size is `romAddress − 0x8C200000 = 0x5F0000 − ceil(binary / 64 KiB) × 64 KiB`.
- A tiny add-in gets 0x5E0000 = 6,160,384 B. A maximum-size add-in (binary 0x370000) gets 0x280000 = 2,621,440 B.

| Case (MPM 1.1) | binary | romAddress | `_ld1` size |
|---|---|---|---|
| binary = 600 KiB (614,400 B = 0x96000) | 0x96000 | 0x8C750000 | **0x550000 = 5,570,560 B (5.31 MiB)** |
| g3a file = 600 KiB → binary 585,728 B (0x8F000) | 0x8F000 | 0x8C760000 | 0x560000 = 5,636,096 B (5.38 MiB) |
| CGDoom.g3a as in the repo (349,292 B → binary 320,620 B) | 0x4E46C | 0x8C7A0000 | 0x5A0000 = 5,898,240 B (5.63 MiB) |

`_uram` = 0x80000 − 0x4000 − 0x1400 − S = **502,784 B − S**, where S is the size of .bss + .data + .gint.bss.
- The 491k in the linker script is exactly this number.
- Then the 177,504 B VRAM is subtracted, which leaves about **325,280 B − S**.

Max add-in size in 1.1: binary ≤ **0x370000 = 3,604,480 B** (g3a ≤ 3,633,152 B), mapped by 55 × 64 KiB.

### 2.6 The currently distributed public mpm.bin (Feb 2025) is different
- **[V-forum]** The first post of topic18534 still links `mpm-bin-199226.zip`, which comes from Lephenixnoir's post #199226 of **2025-02-27**: "Voici le mpm.bin à jour pour fournir la RAM supplémentaire…". **[G]** It corresponds to git `68e7f6e` (2025-02-22, "send arena info to add-in"). Its `load_addin` (`git show 68e7f6e:main.c`):
  - g3a filesize ≤ **3 MiB** (3<<20), so binary ≤ 3,117,056 B.
  - romAddress is **fixed at 0x8C400000**, mapped with 48 × 64 KiB (0x00300000–0x005FFFFF → 0x8C400000–0x8C6FFFFF).
  - Static RAM **0x8C780000–0x8C7FFFFF** (512 KiB) is mapped at 0x08100000.
  - mpm.bin sits at 0x8C700000 and is not relocated.
  - The loadinfo arena is **0x8C200000–0x8C400000 = exactly 2 MiB**, whatever the add-in size.
  - **[G]** The space between 0x8C400000 + binary and 0x8C700000 (about 2.4 MiB for a 600 KB add-in) is unused but not announced. Using it would rely on an implementation detail.
  - It invalidates the operand cache with CCR.OCI instead of writing it back. This was fixed in 1.1 by ea0ff7b. **[G]** That bug may be related to the known "crash when launching another add-in after a big one" (KhiCAS page; Lephe, p.12: "KhiCAS requires mapping more memory… leads to crashes when loading the next add-in").
- **[V-forum]** KhiCAS ships its own modified mpm.bin (3.5 MB limit). Users must swap mpm.bin to run KhiCAS (p.16-17, Parisse's KhiCAS page).
- The `origin/dynload` branch (`1dd901e`, 2025-02-21, unmerged) is an earlier experiment: ROM goes just below 0x8C700000 with a 0x370000 limit and the static RAM at 0x8C780000. It has no loadinfo.
- **[V]** `origin/dev` == `main`.

**Recommendation for the Doom port.** Take all large buffers (zone, caches) from gint's default arenas through `malloc` and `kmalloc(…, "_ld1")`, and size the zone dynamically, for example with `kmalloc_max`.
- On the public Feb-2025 mpm.bin, this gives about 2 MiB + ~320 KiB. That is still more than CGDoom gets on the CG50, where the zone is about 500 KiB (OS stack) + ~200 KiB + an optional zone above the 2 MB line.
- On MPM 1.1, it gives about 5.3–5.6 MiB.
- Allocate a private stack (for example 64–128 KiB from `_ld1`) and switch r15 in `main`, because the inherited OS stack has unknown size **[G]**. Alternatively, measure r15 at entry first.

---

## 3. Storage memory

- **Size.**
  - **[V-forum]** User storage is about **4.5 MB**: "on a juste 4,5Mo utilisateur, loin des 16Mo de la G90+E" (Slyvtt, p.3); "4.7Mo de flash user" (Parisse, p.2).
  - A forum review quoted by search excerpts gives **4,793,072 bytes**, about 4.57 MiB **[V, secondary source]**.
  - **[V]** The raw Fugue region according to gint PR 62's "fygue" driver (`refs/pull/62:src/fs/fygue/fat/flash/flash.c`):
    - CG100: `phy_start = 0x00C80000`, 128 KiB sectors, `fsector_count = 0x3D` (61) → **7.625 MiB raw, physical 0x00C80000–0x01420000**, with 32 × 4 KiB flash clusters per sector (corrected in commit c99a18d "fix Math+ Flash information").
    - CG50: 0x00C80000 + 0x9C × 128 KiB = 19.5 MiB, up to 0x02000000.
  - The flash chip is still at least 0x01420000 bytes and **[G]** 32 MiB like the CG50. The rest (0x01420000–0x02000000, about 11.9 MiB) is unused. Critor: "11.5M de flash inutilisée".
  - Lephe found no safe way to enlarge the filesystem (2026-01-20: "unlikely to ever be possible").
- **Filesystem.**
  - **[V]** Fugue (Kyoto Software Research FAT), `gint[HWFS] = HWFS_FUGUE` (`hardware.c:148`).
  - gint accesses files through BFile inside a **world switch** (`gint/include/gint/bfile.h:1-30`; `fs/fugue/fugue.c:13-35`).
  - Under gint dev, BFile calls go to fixed OS addresses (§6).
- **Memory-mapped access.**
  - **[V]** The OS function `BFile_Block` (= `Bfile_GetBlockAddress`, CG50 syscall 0x1DAA) returns **direct pointers into flash**.
    - MPM reads g3a headers and icons through it at 4 KiB steps (`mpm/addins.c:22,35`).
    - The prototype is `int CW_BFile_Block(int fd, int whence, void **block_ptr)` (`casiowin.h:180`).
    - OS addresses: **1.00: 0x8023FFB2, 2.00: 0x80333CF2, 2.10: 0x8039675A** (`casiowin.yaml:37`). CalcLoverHK confirms 0x80333CF2 for 0x1DAA (NESizm thread).
  - gint does not wrap BFile_Block. Its TODO lists "fs: support read-only files backed with GetBlockAddress()".
  - **[G]** The pointers are expected to fall in 0x80C80000–0x81420000 (P1) or the 0xA0… alias.
  - **[V]** CGDoom's CG50 approach scans flash 0xA0000000–0xA2000000 for 512-byte sectors that match the file, with hint 0xA0C00000 (`CGDoom/src-cg/platform.h:10-42`). On the CG100, restrict the search to 0xA0C80000–0xA1420000: 7.6 MiB, 1,952 4-KiB clusters, versus 4,096 indexed today. Alternatively, use BFile_Block per 4 KiB cluster.
  - **[V]** Fugue clusters are 4 KiB (fygue `fcluster_per_fsector = 32` per 128 KiB).
- **File size limits and fit.**
  - No size limit was found beyond the free space.
  - `doom1.wad` 1.9 shareware is 4,196,020 B. With the current `CGDoom.g3a` (349,292 B), the total is 4,545,312 B. That leaves about **247 KB** of a 4,793,072 B store, before mpm.bin (≤64 KiB; the public zip is 9.3 KB), cluster rounding and save files.
  - The registered `doom.wad` (11,159,840 B), Ultimate (12,408,292 B) and `doom2.wad` (14,604,584 B) **cannot fit**.
  - **[V-forum]** Critor (p.1): "rien que le .wad très incomplet du shareware fait 4.1M… Nous avons une version allégée (sans musique et bruitages) … 3.2M." CGDoom has no sound, so stripping DS*/DP*/D_*/GENMIDI/DMX lumps is a valid way to make room.
  - **[G]** Other options are CGDoom's per-episode split WADs (`doomu1..4.wad`, `cgdoom.c:705-713`), or storing the WAD compressed and inflating it into the `_ld1` arena. That only fits for shareware-size WADs on MPM 1.1.
- **USB.**
  - **[V-forum]** In "USB file transfer" mode the storage appears as a USB mass-storage drive (VID:PID 07CF:6103; OS update mode uses 07CF:6102).
  - Copy files to the **root**, not `@MainMem` (Lephe, 2026-09-02).
  - **[V-forum]** A BFile caching quirk: a g3a created by a program is not found by `BFile_FindFirst` until after a USB connection (p.12).
  - Writing is slow and needs a world switch per call. Keep saves small.

---

## 4. Display

- **[V]** Old-display units use the same controller as the CG50: **Renesas R61524**. It is driven over a 16-bit parallel bus at P2 **0xB4000000** (area 5, so CS5A timings apply), and **port R bit 4 (PRDR 0xA405013C)** is the RS line (`gint/src/r61524/r61524.c:33-48`).
  - gint drives the full **396×224** RGB565 panel (`display-cg.h:29-30`; window 0..395 × 0..223).
  - The OS uses a 384×216 VRAM: MPM writes `VRAM[384*y+x]` (`addins.c:54-67`, `main.c:399-400`).
- **[V]** New 2025–2026 displays (gint commit 2700096, 2026-03-03):
  - Reading register 0xDA returns 50 or 82. The window is then set with MIPI-DCS-style commands 0x2A (columns), 0x2B (rows), 0x2C (memory write), each coordinate as two 8-bit writes (`r61524.c:61-66, 136-176`). Old displays use R61524 registers 0x210–0x213 with mirrored X: `395-xmax..395-xmin`, then HADDR/VADDR and REG_DATA 0x202.
  - **[V-forum]** These units have bootcode `DateA 2025.0304.1404`, cannot run OS 2.00.0202, and are **not supported by the MPM installer yet** (2026-10-08).
  - **[G] Risk to test:** gint's `hsave`/`hrestore` still use the old R61524 window registers 0x210–0x213 on world switch (`r61524.c:352-360`). Their effect on the new controller is unknown.
- **[V]** Transfer method: `dupdate()` → `r61524_display(gint_vram, 0, 224, R61524_DMA[_WAIT])` → DMA channel 0, 32-byte blocks, `99 × (224/4) = 5,544` blocks = 177,408 B, to the fixed destination 0xB4000000 (`r61524.c:178-210`; `render-cg/dupdate.c:19-33`).
  - Partial updates by DMA need the full 396-pixel width and an aligned height; otherwise CPU writes are used (`r61524_display_rect`).
  - **[G]** A contiguous 320-wide buffer works fine with DMA after `r61524_start_frame(x, x+319, y, y+199)`: 640 B per row = 20 × 32 B, 4,000 blocks.
- **Speed.**
  - **[V-forum, secondary]** About 11 ms per full frame at default clocks (≈ 16 MB/s) and 6.91 ms with a 92 MHz bus.
  - **[G]** A 320×200 frame (128,000 B) takes about 7.9 ms at F1 and about 5 ms with the 118 MHz-bus preset. The DMA runs asynchronously, but it contends with the CPU for SDRAM.
  - CGDoom on CG50 writes converted pixels directly to the controller ("direct R61524", `platform.h:45-48`). With gint, the cheapest route is probably to convert 8-bit to RGB565 into a 32-byte-aligned cached buffer and DMA it, or to CPU-write during conversion.

---

## 5. Keyboard

- **[V]** The physical matrix is the same KEYSC with 10 rows (0–9) of 6 columns, scanned by gint's KEYSC driver. Only rows 7–9 and the (1,3) key change meaning on the CG100 (`gint/src/keysc/keydev.c:48-93`).
  - `keymatrix_to_keycode` remaps rows 7–9 through `CG100_keymap`, and (row 1, col 3) → `KEY_FORMAT`. Every other key keeps the CG50 code `(row<<4) + (7-col)`.
- Physical layout, top to bottom and left to right (col 6 → col 1). It is confirmed by TI-Planet's equivalence table (critor, 2025-02-09, topic t=26948).

| Matrix row | col6 | col5 | col4 | col3 | col2 | col1 |
|---|---|---|---|---|---|---|
| 9 (old F1–F6) | **ON** `KEY_ON` 0xA6 | **HOME** `KEY_HOME`=`KEY_MENU` 0x84 | **⇤ prev tab** `KEY_PREVTAB` 0xA7 | **▲** `KEY_UP` 0x86 | **⇥ next tab** `KEY_NEXTTAB` 0xA8 | **⇞ page up** `KEY_PAGEUP` 0xA9 |
| 8 | **SETTINGS** `KEY_SETTINGS` 0xAB | **BACK ↩** `KEY_BACK`=`KEY_EXIT` 0x74 | **◀** `KEY_LEFT` 0x85 | **OK** `KEY_OK` 0xAC | **▶** `KEY_RIGHT` 0x76 | **⇟ page down** `KEY_PAGEDOWN` 0xAA |
| 7 | **SHIFT** `KEY_SHIFT` 0x81 | **ALPHA** `KEY_ALPHA` 0x71 | **VARIABLE** `KEY_VARS` 0x83 | **▼** `KEY_DOWN` 0x75 | **CATALOG** `KEY_CATALOG` 0xAD | **TOOLS** `KEY_TOOLS`=`KEY_OPTN` 0x82 |
| 6 | x,θ,t `KEY_XOT` | fraction `KEY_FRAC` | √ `KEY_SQRT` 0xAE | ^ `KEY_POWER` | x² `KEY_SQUARE` | eˣ-type `KEY_EXPFUN` 0xAF |
| 5 | `KEY_COMMA` | sin `KEY_SIN` | cos `KEY_COS` | tan `KEY_TAN` | ( `KEY_LEFTP` | ) `KEY_RIGHTP` |
| 4 | 7 | 8 | 9 | DEL | (none) | – |
| 3 | 4 | 5 | 6 | × | ÷ | – |
| 2 | 1 | 2 | 3 | + | − | – |
| 1 | 0 | . | ×10ˣ `KEY_EXP` | **FORMAT** `KEY_FORMAT` (was (−) on CG50) | EXE `KEY_EXE` | – |
| 0 | AC = `KEY_ACON` 0x07 (separate line) |

- Row 6/5 names come from `CG100_keymap` (`keydev.c:53-54`); the full table is at `keydev.c:49-55`, and the CG100 keycode values at `gint/include/gint/keycodes.h:89-103`.
- **[V] There are no F1–F6, MENU, OPTN or EXIT keys.** Aliases: HOME = MENU, TOOLS = OPTN, BACK = EXIT, FORMAT = F↔D. F1–F6 exist only as an event-stream combo: **CATALOG held + a row-6 key → `KEY_F1 + (6-col)`** (`keydev.c:274-277`). `keydown(KEY_F1)` is never true on the CG100.
- **[V] Inconsistency in gint dev (likely a bug).**
  - For rows 5–6, events report the **CG50 codes** (`keymatrix_to_keycode` does not remap them). For example, the fraction key yields `KEY_LOG` 0x62 and sin yields `KEY_FD` 0x52.
  - `keydown()` and `keycode_to_keymatrix` use the **CG100 names** from `CG100_keymap` (`keydev.c:66-93`). For example, `keydown(KEY_SIN)` checks (5,5), while `keydown(KEY_LOG)` reports a non-existent key.
  - For Doom controls, use `keydown()` with CG100 names, or use `ev.row`/`ev.col` from events (`key_event_t` has row and col fields since 7a479e4).
- Doom-control implications:
  - The D-pad is in the top cluster (▲ r9c3, ◀/OK/▶ r8, ▼ r7c3) and the large OK sits in the middle.
  - SHIFT, ALPHA, VARIABLE, SETTINGS and BACK are under the left thumb. CATALOG, TOOLS, PAGEUP, PAGEDOWN and NEXTTAB are on the right.
  - CGDoom's CG50 bindings that use F1–F6, OPTN, MENU or EXIT must be remapped.
  - The OS GetKey cannot see ON (Lephe, p.6). With gint scanning KEYSC, ON is a normal matrix key (r9c6).

---

## 6. OS versions and gotchas

| OS (string at 0x80020020) | Displayed as | Units | MPM installer | mpm.bin (git main) API index | gint dev CASIOWIN API |
|---|---|---|---|---|---|
| `01.00.0000` | 01.00 | launch Graph Math+ 2024 | **no** ("upgrade to OS 2 first") | 0 | 1 |
| `02.00.0000` | 02.00.0202 (fx-CG100) / 02.00.2202 (Math+) | old display (DateA 2017.0106.2008) | **yes** (mpm-installer-1.0bw.exe, Windows, WinUSB/Cahute) | 1 | 2 |
| `02.00.0010` ("2.00.1") | 02.00.0212 | new display; UK/Peru batches | no ("new OS layout: TODO") | (TODO in `start.c:37-39`; stays 0 = wrong) | **2 (wrong, compares only "02.00")** |
| `02.10.0000` | 02.10.x202 (released about 2025-12-18) | both | no (as of 2026-10-08) | 2 (b1a91e1, 2026-08-12) | **0 → jumps to the CG50 syscall table 0x80020070 → crash** |
| `02.20.…` | 02.20.2202 (Sept 2026) | | no | – | 0 (crash) |

- **[V]** gint's API selection (`gint/src/kernel/kernel.c:48-56`):
  - "01.00" → API 1.
  - CG100 && "02.00" → API 2.
  - Otherwise API 0, which is the normal syscall table at `0x80020070` (`syscalls.S:167-193`).
- **[V]** gint's address table (`syscalls.S:210-237`) covers malloc, free, realloc, BFile_Remove, Rename, Create, Open, Close, Size, Seek, GetPos, WriteFsync, Read, FindFirst, FindNext, FindClose, Timer_Install, Start, Stop, Deinstall, GetKeyWait and GetVRAMAddress.
  - It has **0** for PutKeyCode, ClearKeyBuffer, SetQuitHandler, PowerOff and Reset. A zero entry → `gint_panic(0x10C0)`, "Missing syscall for this OS version" (`syscalls.S:175-191`).
  - **Do not call** `gint_poweroff()`, `gint_set_quit_handler()`, `gint_osmenu()` (a no-op on CG100, `osmenu.c:96-101`), or the panic screen's F1+F6 reset.
  - Unmerged PR 50 adds PowerOff and Reset for 2.00 (b8ca7b7).
- **[V]** OS 2.10 support in gint would need a 4th column. The addresses come from `mpm/casiowin.yaml`, third column:
  - malloc 0x803C9CCC, free 0x803C9A98, realloc 0x803CA890
  - BFile_Remove 0x80396C7A, Rename 0x80396C2A, Create 0x80396958, Open 0x80396338, Close 0x803964B6, Size 0x8039656C, Seek 0x803966D4, GetPos 0x803967E4, Write 0x80396A92, Read 0x8039682A
  - FindFirst 0x80397030, FindNext 0x803972AE, FindClose 0x803973B8
  - Timer_Install 0x8010F398, Start 0x8010F4E2, Stop 0x8010F53E, Deinstall 0x8010F460
  - GetKeyWait 0x80299DAA, GetVRAMAddress 0x80076262
  - Keep the check as a full-string compare.
- **[V-forum]** There is **no syscall table** on the CG100. PrizmSDK/libfxcg add-ins, including CGDoom master, do not run unless every syscall is replaced by a per-OS fixed address (the NESizm/Prizoop approach: works on 2.00.x202 only). UBC-based syscall emulation in mpm.bin is planned but not done (p.4: "mpm.bin va poser un breakpoint à 0x80020070"; still pending as of 2026-09-21). **Port to gint dev** (plus fxlibc@dev and fxsdk@dev: `giteapc install fxsdk@dev gint@dev fxlibc@dev`).
- **[V-forum]** MPM ships through a closed installer (anti-abuse). Development happens on Windows. Downgrades work only through OS ERROR mode and only on old-display units.

---

## 7. Other things that can trip up a Doom port

1. **No return-to-menu.**
   - `gint_osmenu_native()` returns immediately on the CG100 (`osmenu.c:96-101`) and `gint_setrestart()` is forced off (`start.c:43-48`).
   - Leaving `main()` returns to MPM's list. Provide an explicit quit, for example BACK or HOME, and restore the clock before exiting.
2. **Stack.** It is inherited from MPM and the OS, and its size is unknown **[G]**. gint's 16 KiB uram "stack" and canary are not where r15 points. Consider switching to a private stack.
3. **TLB.** There is no demand paging. gint's TLB-miss handler calls `syscall 12` via 0x80020070 for faults in [0x00300000, 0x00800000) (`gint/src/kernel/tlbh.S:9-12, 33-48`). On the CG100 that is not a valid entry point, so a stray access beyond the mapped image crashes. **Keep the g3a ≤ 3 MiB** (public mpm.bin) or ≤ 0x370000 (1.1).
4. **The NULL page is mapped to flash** (entry 63, `main.c:262-263`, U:rw, cached). NULL reads silently return bootcode bytes. **[G]** Writes are cached and later written back onto the flash bus; a single stray write is harmless on NOR without the unlock sequence, but do not rely on it.
5. **Code and rodata live in RAM.** This makes them fast with no flash waits, and also writable. Big rodata, such as precomputed tables or embedded assets, directly reduces the heap.
6. **World switches are expensive.**
   - Every BFile read (gint fs) does a full driver save and restore. It also wipes the ILRAM and XYRAM contents by default and flushes async DMA (`world.c:150-190`).
   - **[G]** During gameplay, read the WAD through memory-mapped flash pointers (CGDoom scan or BFile_Block) or from a RAM copy. Do not call `read()` per lump.
   - Fugue writes (saves) are slow.
7. **Timers.** gint owns all **9 timers** (3 TMU + 6 ETMU, `timer.h:61`). The keyboard scan uses one (`keysc.c:154`). Use `timer_configure(TIMER_ANY, …)` for the 35 Hz tic. The OS user timers (IDs 5–9) are deinstalled by MPM when its menu exits.
8. **Overclock persistence.** It is not undone automatically (§1.2). Restore it on exit so the OS and MPM are not left at 236 MHz.
9. **Pφ is fixed at 29.49 MHz** in the F1, F4 and Ptune4-F5 presets, so USB (fxlink) keeps working while overclocked.
10. **USB for development.**
    - **[V-forum]** gint's USB stack works on the CG100: the installer itself runs gint USB code, and the device appears as "CASIO fx-CG family on gint". fxlink logging and screenshots are usable.
    - MPM has an **add-in push** (SETTINGS in the MPM menu). It receives a 4-byte size, then the raw post-header binary in 0x100-byte chunks, straight into RAM, with AC to cancel (`main.c:190-206, 472-484, 235-239`). This avoids rewriting storage on each iteration. **[G]** The host tool is not documented in the repo.
11. **Exam mode** disables the MPM menu. Installing **any OS update** removes MPM.
12. **No sound hardware path.** The 3-pin port only disables exam mode, and there is no speaker. Skip sound code and strip sound lumps.
13. **OS heap fallback.** Under gint, `malloc` falls back to the OS heap (`_os`). That heap is small and much fuller when the MEMORY app is open (Lephe, p.8). Do not depend on it.
14. **Two layouts.** Under the Feb-2025 public mpm.bin, the arena is 0x8C200000–0x8C400000 and the static RAM is 0x8C780000. Under 1.1, the static RAM is 0x8C160000 and the image is at the top of RAM. Code must not hardcode CG50 addresses such as CGDoom's `SystemStack = 0xAC0F0000` (`cgdoom.c:717`), `0x8C200000` "2MB line" memory (`i_system.c:91-95`) or the OS secondary VRAM. Under MPM 1.1, 0x8C200000 is the start of `_ld1` and is owned by gint's allocator.

---

## Sources

- Local source:
  - `/Users/cheparukhin/casio_doom/ref/mpm` (main.c, loader.c, loader-cg.x, reloc.S, start.c, casiowin.yaml/.S/.h, addins.c; branches main/dev = cbd1a41, dynload = 1dd901e; commit 68e7f6e).
  - `/Users/cheparukhin/casio_doom/ref/gint` (dev 463f180).
  - `/Users/cheparukhin/casio_doom/ref/fxsdk` (dev a6e46eb).
  - `/Users/cheparukhin/casio_doom/ref/CGDoom` (src-cg/platform.h, cgdoom/cgdoom.c, i_system.c).
- Cloned:
  - `/Users/cheparukhin/casio_doom/ref/extra/gint-prs.git` (gint PR heads 47/50/62/64…).
  - `/Users/cheparukhin/casio_doom/ref/extra/Xtune` (Ptune4).
- [Planète Casio: MPM : Mod add-ins Math+ (topic18534, pages 1–21, 2024-12 → 2026-10-08)](https://www.planet-casio.com/Fr/forums/topic18534-1-mpm-mod-add-ins-math.html)
- [Planète Casio: NESizm and Prizoop on fx-CG100 (topic19383)](https://www.planet-casio.com/Fr/forums/topic19383-1-nesizm-and-prizoop-now-on-fx-cg100graph-math.html)
- [TI-Planet: MPM bêta news, with the key-equivalence table (t=26948)](https://tiplanet.org/forum/viewtopic.php?f=51&t=26948)
- [Cemetech: Casio fx-CG/CP overclocking insights (CalcLoverHK)](https://www.cemetech.net/forum/viewtopic.php?p=311770)
- [WikiPrizm: SH7305 On-chip Memory](https://prizm.cemetech.net/Technical_Documentation/Peripherals/On-chip_Memory/)
- [Simon Lothar: fx-CG20 memory map](https://media.taricorp.net/prizm/simon_chm/fxCG20_MemoryMap.htm)
- [KhiCAS page, FXCG100 section (Parisse)](https://www-fourier.univ-grenoble-alpes.fr/~parisse/casio/khicasioen.html)
- Search-engine excerpts only (hpmuseum returned 403):
  - [hpmuseum post-203451](https://www.hpmuseum.org/forum/post-203451.html): 4,793,072-byte storage.
  - [hpmuseum thread-23597](https://www.hpmuseum.org/forum/thread-23597-lastpost.html): dupdate and Dhrystone numbers.
