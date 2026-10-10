# Research critique: contradictions, open questions and resolutions

Date: 2026-10-09. Scope: the five `notes/research-*.md` reports plus `notes/design.md`, checked against the sources in `ref/` (gint dev `463f180`, mpm `cbd1a41` and branches, CGDoom `8516c16`, fxsdk dev, gint PR heads in `ref/extra/gint-prs.git`, gpSP in `ref/extra/gpsp-sh4-jit`) and a few web pages.

Tags:
- **[V]** I verified it in source here (file:line given).
- **[V-ext]** A third party reports a hardware observation in its source or docs. I did not test it.
- **[G]** Inference or estimate that still needs hardware.

Report short names: HW = cg100-hardware, API = gint-api, INT = cgdoom-internals, PERF = performance, PREC = cg100-precedents.

---

## 0. Bottom line (what changes in the design)

1. **Heap detection must handle three `mpm.bin` variants, not two.** gpSP and KhiCAS both ship the `dynload` mpm.bin, and it passes **no** arena to gint. Under it gint creates no `_ld1`, so Doom would only get what is left of `_uram` (≈150–300 KB). The design needs its own arena discovery (§B3).
2. **XRAM and YRAM are one 8 KiB bank each, visible at several addresses.** gint's `GXRAM`/`GYRAM` sections at `0xE500E000`/`0xE5010000` use the **same** physical memory as the DMA strip buffers at `0xE5007000`/`0xE5017000`. The total X+Y budget is 16 KiB, and design.md §6 (colormaps in XYRAM) and PERF P2 (strip banks) cannot both have it (§A2).
3. **gint restores the OS clock on every world switch and on exit.** HW §1.2/§7.8 relied on a stale header comment; the driver code says otherwise. Do not write our own restore. Expect one CPG transition (and about one lost RTC tick) per switch (§A1).
4. **Keyboard:** reading raw matrix codes is correct (design.md §2), but never pass them to `keydown()` on the CG100, because gint remaps rows 5–9 there. Read `KEYSC` (or `keydev_std()->state_now`) instead, then convert the byte order (§A4).
5. **New risks found, missed by every report:**
   - (a) gint interrupt callbacks do not save the DSP repeat registers `RS`/`RE`. Running a repeat-loop strip converter inside a DMA-end callback will corrupt repeat-loop drawers running in the foreground (§C2).
   - (b) While the OS runs, UTLB entries 0–53, which hold our code pages, can be replaced. A later miss in our code jumps to the non-existent CG50 syscall gate (§C3).
   - (c) NOR flash reads are probably 5–7× slower per cache line than SDRAM. This argues for keeping the WAD, or at least each level's graphics, in RAM when the heap allows (§C1).
6. **The firmware window is small.** Only OS string `"02.00.0000"` (02.00.0202 and 02.00.2202) can run MPM today, and only on the old R61524 display. Target exactly that string; on anything else show a message and exit before the first OS call (§B6).
7. Still unresolvable without hardware: where the stack is and how big it is, the stock clock, the fragmentation profile, whether the OS really reloads TLB entries, and the real NOR/SDRAM timings. §E lists runtime probes for a diagnostics screen.

---

## A. Contradictions between reports, and how they resolve

### A1. Is the overclock kept across world switches and on exit?
- HW §1.2 and §7.8 say no: "The clock is not reset on world switch or on exit (`clock.h:136-137`). Save … and restore it on exit."
- API §7, PREC §6 and gpSP say yes: the OS clock is restored.
- **Resolved: the OS clock IS restored.** [V]
  - `drv_cpg` has `hsave` = `cpg_get_overclock_setting` and `hrestore` = `cpg_set_overclock_setting` (`gint/src/cpg/cpg.c:137-156`).
  - Every non-`GINT_DRV_SHARED` driver is saved and restored in `gint_world_switch_out`/`_in` (`gint/src/kernel/world.c:93-98, 135-138`). `kquit` does the same on exit.
  - Only `cpg_set_overclock_permanent(true)` makes the clock stick (`overclock.c:146-155`).
  - The comment at `clock.h:136-137` is stale.
- **Impact:**
  - No manual restore at exit.
  - Every world switch costs two full CPG reprogrammings (`gint_world_sync` + timer rescale) and can lose about one 128 Hz RTC tick [V-ext, gpSP `nor_rom.c:163-172`].
  - Batch file work into as few switches as possible, as already planned.

### A2. XRAM/YRAM addresses and aliasing
- HW §1.4 says gint's `0xE500E000` is an alias of X memory. API §1.3 calls this "unverified [G]". PERF §1.4 reports gpSP's aliasing claim.
- **Resolved (strong indirect evidence):** there is one 8 KiB X bank, aliased across the `0xE500xxxx` window, and one 8 KiB Y bank across `0xE501xxxx`.
  - gint's `GINT_ONCHIP_BACKUP` saves exactly 8 KiB at `0xE500E000` as "XRAM" and 8 KiB at `0xE5010000` as "YRAM" (`world.c:155-170`). That is the whole on-chip X/Y memory as gint sees it.
  - WikiPrizm gives X = `0xE5007000–0xE5008FFF` (8 KiB, pages at `…7000` and `…8000`) and Y = `0xE5017000–0xE5018FFF` (8 KiB).
  - gpSP moved its LCD strips out of XRAM "so it cannot overwrite fastmem", with fastmem linked at `0xE500E000..0xE5010000`, and ran that layout on a physical CG100 (`docs/performance-history.md:560-582`) [V-ext].
- **Impact:**
  - The on-chip budget is ILRAM 4 KiB (gint uses about 180 B) + X 8 KiB + Y 8 KiB, and nothing more.
  - A 12-row strip (7,680 B) at `0xE5007000` covers essentially the whole X bank, including anything linked into `.xyram`.
  - **Decision needed:** spend X+Y on presenter strips (recommended, because gpSP found on hardware that large strips win and 4-row strips are "substantially slower") **or** on colormaps/tables. Not both.
  - For DMA, use the addresses gint's `dma_translate` whitelists (`0xE5007000`, `0xE5017000`; `dma.c:47-56`). Other aliases are passed through untranslated.

### A3. Which LCD controller do MPM-capable units have?
- INT §0.3 and §11 read gpSP's "freestanding direct-R61524 build only draws a single line" as a sign of the new controller. API §1.2 says "Unknown".
- **Resolved:**
  - Units that can run MPM today (bootcode `DateA 2017.0106.2008`, OS 02.00.xx0x) have the classic R61524 [V-ext, forum p21 per HW/PREC].
  - The new controller (version 50 or 82) is only on 2025-bootcode units, which cannot install MPM yet.
  - The "single line" came from a freestanding smoke build without gint init. gpSP itself blames a "black screen with a stray line" on window state (`gint_platform.c:47-50`). It is not evidence of a controller change.
- **Impact:** none on the code. Always use `r61524_start_frame()` (`r61524.c:136-176`), which handles both controllers, and test the classic path first.

### A4. Keyboard: what changed, and how to read keys
- HW §5 says "only rows 7–9 and (1,3) change meaning". **Wrong:** the legends of rows 5–6 moved too.
  - CG100 row 6 is XOT/FRAC/√/^/x²/eˣ and row 5 is `,`/sin/cos/tan/`(`/`)` (`keydev.c:49-55`).
  - On the CG50 these positions hold different keys (e.g. `KEY_FRAC` = 0x51 sits at (5,6) on the CG50 but (6,5) on the CG100).
- API §2.3 describes the inconsistency correctly [V, `keydev.c:56-92`]:
  - `ev.key` remaps only rows 7–9 and (1,3).
  - `keydown()` remaps rows 5–9 through `CG100_keymap`.
  - Any code that is not in that table and has row > 4 is forced to the dead position (0,1).
- design.md §2 ("raw matrix codes still identify physical keys") is right, but only if the **state is read raw**. `keydown(0x91)` is always false on the CG100, even though 0x91 is the physical ON key.
- **Resolution [V]:** `PL_ScanKeyboard` reads the raw state with no gint translation:
  - Read the 6 × u16 at `0xA44B0000` exactly as CGDoom does (`cgdoom-kbd.c:154-159`). Reading the data registers is side-effect free; MPM does it too.
  - Or copy `keydev_std()->state_now[12]`, but its layout is `gint[r] == CGDoom[r ^ 1]` (`keysc.c:57-58` vs CGDoom's `row = (code>>4) ^ 1`).
  - AC is row 0, bit 0 (`KEY_ACON` = 0x07).
  - Never call `keydown()`, `getkey()` or events with CG50 codes on the CG100. For menus, use row/col or the CG100 names in the agreeing rows 1–4 and 7–9.
- Two side notes:
  - gint aborts the add-in on SHIFT+7+3+AC (`keysc.c:71-76`). No default binding should require that chord.
    - **Correction (gint review rounds 2-3):** gint's own chord can never fire in CGDoom (it reads keydev state that only
      advances when events are dequeued, and nothing dequeues them). CGDoom implements the chord itself: the 35 Hz tic
      interrupt (src/gint/time.c) checks it on a raw KEYSC scan (SHIFT is (7,1) on the CG100, (8,1) on the CG50) and
      only latches a request. The foreground takes it at its next safe point (`gquit_poll()` from PL_GetTics and the UI
      waits, `gquit_poll_load()` from every WAD read/map) and leaves through the normal exit path: WAD closed, a demo
      being recorded finished, the delayed saves and the settings written. `abort()` (gint's .dtors: WAD close and
      UTLB restore, then kquit, without the saves) is only the fallback when nothing takes the request while the chord
      stays held for 2 s (a hang). AC/ON is unbindable, so no preset can trigger it.
  - CATALOG + a row-6 key synthesizes F1–F6 events only.

### A5. MPM variants and the heap (HW knows 2, PREC knows 3)
- HW §2.6 and design.md know v1.1 (about 5.5 MiB `_ld1`) and the public Feb-2025 zip (2 MiB `_ld1`).
- PREC §2.3 adds **`dynload`**, which is exactly the 12,820-byte mpm.bin shipped by gpSP and KhiCAS.
  - It calls the add-in as `void (*)(void)` (`git show origin/dynload:main.c:396`), so gint sees no `'MPM0'` and creates no `_ld1` (`start.c:180-182`) [V].
  - Code sits at `(0x8C700000 − size) & ~0xFFFF`, user RAM at `0x8C780000`, and mpm.bin at `0x8C700000–0x8C703214`.
- I confirmed the public link: the first post of topic18534 still links `mpm-bin-199226.zip` (the post of 2025-02-27 "pour fournir la RAM supplémentaire", ≈ `68e7f6e`). [V-ext, fetched page]
- **Resolution:** see §B3 for the detection algorithm.

### A6. The MPM "else" branch (`0x8C770000`)
- INT §5.3 and PREC §2.2 describe the `0x8C770000` fallback as live. HW §2.2 says it is dead.
- **Resolved: dead in v1.1.** `CW_AppMemory` is the constant `0x8C160000` for all OS versions (`casiowin.yaml:71`). [V]

### A7. Default overclock
- design.md: F4 by default. HW/PREC: F4 by default, Ptune4-F5 opt-in. PERF P9 says "keep off by default", with ambiguous scope.
- **Resolution:**
  - gint F4 (236 MHz CPU / 59 MHz bus) by default. OutRun ships `clock_set_speed(CLOCK_SPEED_F4)` on the CG100 (PREC §3.6).
  - Ptune4-F5 (118 MHz bus) is opt-in with a warning. gpSP reports bit flips on some units.
  - Remember that F4 leaves the bus, the LCD and **flash and SDRAM latency** unchanged (§C1).

### A8. Where the CG100 file system sits in flash
- HW §3 cites unmerged gint PR 62 ("fygue"): FS = physical `0x00C80000–0x01420000`.
- gpSP (and INT §4.6) report a real `Bfile_GetBlockAddress` result of `0xA1B97000`, i.e. physical `0x01B97000`, **outside** that range (`nor_rom.c:396-401`).
- The fygue values for the CG100 are guesses:
  - Commit `f1bf037` says "support Math+ device (untested)".
  - `c99a18d` changed `0x00BC0000/0x26` to `0x00C80000/0x3D` (= the CG50 start).
  - The PR is unmerged.
- The gpSP value is a field observation, although it comes from an AI-maintained repo.
- **Resolution:**
  - Do not range-check against either value. Accept any P2 (`0xA0000000–0xA1FFFFFF`) or P1 (`0x80000000–0x81FFFFFF`) result, and convert it to P1.
  - Reject everything else, P0 in particular (gpSP saw `0x001129xx`).
  - If the CGDoom sector-scan fallback is kept, scan the full 32 MiB (`HWROM` = 32 MiB, `hardware.c:157`). The scan cost is only relevant if `GetBlockAddress` fails.

### A9. Smaller inconsistencies
- **Where gint's VRAM lands.** INT §5.3 says it is "taken from `_ld1`". It actually comes from `_uram` first:
  - `kinit` creates `_uram`, then `_ld1`, then calls `dvram_init()` with arena NULL (`kernel.c:113-196`, `dvram.c:93-110`).
  - It only spills into `_ld1` if `.data + .bss` exceeds about 314 KiB.
- **g3a size limit.** design.md §4 says "g3a ≤ 3 MiB". The binding limit is gint's linker script, `rom l = 2M` (`fxcg50.ld.c:22`), unless a custom script is used. MPM 1.1 allows 0x370000; the Feb-2025 build allows a 3 MiB file.
- **Development push loop.** PREC §9.5 doubts that `fxlink -pw` works with MPM. **Resolved: it works** [V]:
  - `fxsdk/fxlink/modes/push.c` claims the CESG502 interface, waits for the 17-byte "USB loader ready", then sends a 4-byte big-endian size plus the body (≤ 6 MB).
  - That is exactly what MPM's SETTINGS push expects (`mpm/main.c:466-484`).
  - Push the header-less `build-cg/<target>.bin` that `generate_g3a` produces.
  - `fxlink` is already built in `toolchain/bin`.
- **Restoring the window before `dupdate`.** INT §3.2/§6.5 and PREC say to restore the full window before `dupdate()`. This is redundant: `r61524_display()` already calls `r61524_start_frame(0,395,…)` (`r61524.c:178-187`). It is harmless; keep it only around non-gint pushes.
- **`I_GetTime`.** INT checklist item 26 suggests `rtc_ticks()*35/128`. API §3.4 suggests a timer. Use the timer: `rtc_ticks` is slow (BCD reads) and wraps at midnight.
- **Arena naming.** HW notes that `kinit` never increments `count`, so every loader arena is named `_ld1` (`kernel.c:146-156`) [V]. This is harmless with a single pair.

---

## B. Design-blocking questions: status after this pass

### B1. Storage versus WAD size — RESOLVED (budget known; exact figure second-hand)
- **Capacity.** The only byte figure is 4,793,072 B, from one hpmuseum poster, repeated in two posts. Casio's Memory-app manual gives no capacity (fetched). The forum says "≈4.5 MB" and "4.7 Mo de flash user". Treat **≈4.5 MB** as the budget. `tools/wadtool.py` already assumes 4,500,000.
- **Fit:**

  | Contents | Total | Room left for cfg and saves |
  |---|---|---|
  | Stripped shareware 3,393,736 B + ≤ 450 KB g3a + ≤ 13 KB mpm.bin | ≈3.86 MB | ≈0.6–0.9 MB |
  | Full doom1.wad 4,196,020 B + g3a + mpm.bin | ≈4.66 MB | about nothing (≈130 KB at best) |

  - Registered, Ultimate, Doom II and Freedoom (28.8 MB each, as in `test-wads/`) cannot fit.
  - **Freedoom therefore cannot be used for on-device tests.** Hardware testing needs a user-supplied shareware `doom1.wad`, stripped with `wadtool.py pack`.
- **File names [V, Casio manual]:**
  - The calculator shows names longer than 8 characters truncated (`AAAABB~1.py`), cuts extensions after 3 characters, and is case-insensitive.
  - Only `A–Z a–z 0–9 ! # $ % , ( ) + – . ; = @ [ ] ^ _ `` ` `` and space are allowed.
  - **Every file the port creates must be a strict 8.3 name.** For example `DOOM1_0.DSG` and `CGDOOM.CFG`.
  - CGDoom's demo names `<wad>_demoNN.lmp` (12+ characters) violate this.

### B2. Direct flash pointers to file data — RESOLVED (with caveats)
- **Function and address.** `Bfile_GetBlockAddress(fd, off4k, void **p)`, OS 02.00.0000 at `0x80333CF2` [V `casiowin.yaml:37`; V-ext: used on hardware by gpSP, NESizm, Prizoop, and MPM's own icon reader]. CG50 equivalent: syscall `0x1DAA`.
- **Call pattern.** One `gint_world_switch` for the whole table (gpSP `nor_rom.c:162-196`).
- **Results seen:**
  - P2 pointers: convert to P1.
  - P0 pointers (`0x001129xx`): reject, and serve those blocks by gather or `BFile_Read`.
  - `rc < 0` or NULL: treat as a failure.
- **Writes.** After any storage write, re-query the table and `ocbi` the newly mapped lines. This is gpSP's policy and CGDoom's delayed-saves rationale. design.md's `PL_WadMap` "valid until `PL_WadClose`" must be qualified as "until the next `PL_FileWrite`/`Delete`", or all writes must be deferred until after `PL_WadClose`.
- **BFile_Read return value.** On 02.00 it returns the bytes read: MPM requires `rc == binary_size` to launch every add-in (`main.c:245-248`) [V]. gpSP's "or 0" branch is unnecessary but harmless.
- **New risk:** see §C3 (P0 results imply the OS loads TLB entries).

### B3. Heap size and arena discovery — RESOLVED as an algorithm (numbers [V] from loader code)
Figures below assume a ~450 KB binary.

| mpm.bin | How to detect | Code physical address | Free RAM gint knows about | Extra free RAM to add ourselves |
|---|---|---|---|---|
| v1.1 (git main) | `_ld1` exists and `_ld1.end == phys(0x00300000)` | `(0x8C7F0000−sz)&~0xFFFF` ≈ `0x8C780000` | `_ld1` ≈ **5.5 MiB** | none |
| Feb-2025 public zip | `_ld1.end == 0x8C400000 == phys(0x00300000)` | `0x8C400000` | `_ld1` = **2 MiB** | `[0x8C400000+roundup64K(image), 0x8C700000)` ≈ 2.5 MiB (unannounced) |
| dynload (gpSP/KhiCAS) | no `_ld1`; `mmu_translate(0x08100000) == 0x0C780000` | `(0x8C700000−sz)&~0xFFFF` ≈ `0x8C690000` | none | `[0x8C200000, phys(0x00300000))` ≈ **4.5 MiB** |

- `_uram` adds about 502 KiB − (`.data` + `.bss`) − 177.5 KB VRAM.
- **Algorithm:**
  1. `phys = mmu_translate(0x00300000, NULL) | 0x80000000` (`mmu.c:15-18`).
  2. Pick the case from the table.
  3. For the unannounced regions, `kmalloc_init_arena` + `kmalloc_add_arena` a private arena. Never go above `0x8C700000` (old mpm.bin is resident) and never touch ≥ `0x8C800000` (gpSP: cold boot).
  4. Remove `_os` (`kmalloc_remove_arena`) so failure returns NULL.
  5. Refuse to start below a minimum (e.g. 1.5 MiB total) and name the fix ("install mpm.bin ≥ 1.1").
- RAM is not zeroed between launches: the OS fills it with 0x55 and MPM leaves leftovers. Never assume zeroed memory (gint clears `.bss` itself).

### B4. Display path on the CG100 — RESOLVED
- Use `r61524_start_frame(38,357,12,211)` followed by `dma_transfer_async(0, DMA_32B, …, 0xB4000000, DMA_FIXED)` from a contiguous 320-wide buffer.
- A partial-width window with DMA works on CG100 hardware: gpSP sends 240-, 320- and 384-wide strips [V-ext, `gint_platform.c:23-45, 255-272`].
- Keep strips large (≥ 8–12 rows) and use 4-row multiples, as gpSP asserts.
- Remaining unknowns: real µs per frame and how much the DMA contends with rendering. Measure them (§E).

### B5. Key mapping — RESOLVED (see §A4)
- PREC §5 has the full physical table, and it agrees with gint's `CG100_keymap`.

### B6. OS versions — RESOLVED by scope
- Practically, every MPM user has `"02.00.0000"` at `0x80020020`:
  - The installer supports only 02.00.xx0x.
  - Any OS update removes MPM.
  - 01.00, 02.00.0010 ("2.00.1"), 02.10 and 02.20 cannot install MPM as of 2026-10-08.
- gint dev selects API 2 for that string. Other strings would fall to wrong addresses (02.00.0010 matches the 5-character prefix) or to the dead CG50 gate (02.10).
- **Policy:** at the top of `main`, before any BFile call or `_os` malloc, compare the full 10-character string. On a mismatch, show a message with gint `dtext` and exit.
- gint makes no OS call before `main` [V, `kernel.c:43-196`: arenas and VRAM come from gint arenas, and `_os` is only a fallback].
- The CG50 bonus uses API 0 (syscall `0x1DAA`).

### B7. Stack — UNRESOLVED (needs hardware); mitigation defined
- gint never sets `r15` (`start.c`; no `r15` writes in `src/kernel`). MPM calls the add-in on the OS stack (`main.c:354` TODO). gint's 16 KiB "stack" at the end of `_uram` is only a canary location.
- [G] The OS layout probably matches the CG50, since MPM's AppMemory `0x8C160000` equals the CG50 add-in RAM. That would put the stack just below `0x8C160000` with tens of KiB free.
- gint's own comment: Fugue BFile has been seen overflowing 8 and 12 KiB stacks (`kernel.c:66-68`).
- **Mitigation (choose one):**
  - (a) Log `r15` at entry in the diagnostics screen and keep Doom's peak stack below about 16 KiB. Move `R_DrawSprite`'s `clipbot`/`cliptop` (1.3 KB) and `patchcount` to static storage.
  - (b) Run `D_DoomMain` on a private 64–128 KiB stack taken from the heap, through a small asm trampoline. `exit()` longjmps back to gint's original-stack `setjmp` (`start.c:196-206`), so leaving is safe. OS code called during world switches then also runs on this stack, which is fine because it is P1 RAM.

### B8. Stock clock equals gint F1? — UNRESOLVED, low impact
- Correctness does not depend on it, because gint saves and restores whatever the OS had (§A1). Only performance expectations do.
- Probe: `clock_get_speed()` plus raw FRQCR and CS0WCR/CS3WCR, shown in the diagnostics screen.

### B9. Exit path — RESOLVED
- Return from `main`. `kquit` restores drivers, the window and the clock.
- Before returning:
  - Wait until all KEYSC words read 0 (MPM has a "TODO: wait for keys released").
  - Wait for LCD DMA.
  - Flush delayed writes.
  - Close every fd: fxlibc's `exit` does not.
- Never call `gint_poweroff`, `gint_osmenu`, `gint_set_quit_handler`, or `getkey` with `GETKEY_POWEROFF`/`GETKEY_MENU`. They are missing on the CG100 and panic with `0x10C0` (API §8) [V].
- **Correction (as implemented, src/gint/main.c):** every way out — normal quit, PL_Fatal, the in-game idle exit and the
  abort chord — ends in the same `exit_path()`, in the foreground: the chord's tic interrupt only latches a request
  (see §A4), never exits from interrupt context except through the 2 s hang fallback (`abort()`, which skips the saves
  but still runs the destructor: WAD close, UTLB restore).

---

## C. Issues no report raised

### C1. NOR flash is probably the dominant memory stall, so keep the WAD in RAM when possible [G, derived from register values]
- **Flash timing:**
  - CS0 (flash) is a 16-bit, normal-type space with no burst/page mode: `CS0BCR` `0x36DA0400`/`0x24920400`, TYPE = 0, BSZ = 16 bit (`overclock.c:452-500`).
  - WR wait states are F1: 8 at 59 MHz (`CS0WCR` 0x3C0), F4: 5 at 59 MHz (0x2C0), Ptune4-F5: 12 at 118 MHz (0x4C0).
  - Each 16-bit access therefore takes about 120–170 ns, and one 32-byte cache-line fill (16 accesses) about **1.9–2.7 µs**.
  - That is roughly the same at every preset, so **overclocking does not speed up flash**.
- **SDRAM timing [G]:** an x16 SDRAM line fill is about 20–25 bus cycles, ≈ 0.35–0.45 µs at 59 MHz. Ptune4-F5 halves that.
- **Consequence.** A frame that streams about 150 KB of distinct texture, flat and sprite data from NOR, which does not fit the presumed 32 KiB O-cache, could spend about 10 ms just in line fills. CGDoom's measured +35% from "cached ROM" (commit `55f9d43`) shows flash latency mattered even on the CG50.
- **Design change (re-rank PERF P6 from 7th to 3rd):**
  - When the heap is at least WAD + about 1.5 MiB (v1.1 ≈ 5.5 MiB; dynload ≈ 4.5 MiB):
    - copy every **non-sound** lump into RAM at startup, each **4-byte aligned**. With `wadtool align`, a contiguous mapped image already has aligned offsets;
    - make that the primary path.
    - Bonus: no fragmentation, no remapping after saves, no P0-pointer issue, and aligned lumps permit `swap.b`/`swap.w` instead of bytewise `SHORT()`/`INTFromBytes()`.
  - Otherwise (Feb-2025 mpm.bin, 2 MiB) use the flash map, plus per-level residency of the level's wall, flat and sprite lumps in the zone (`PU_LEVEL`/`PU_CACHE`).
  - Measure flash versus RAM fetch on hardware (§E).

### C2. DSP repeat loops and gint interrupt callbacks do not mix [V]
- gint's callback wrapper saves `r0–r7_bank`, `spc`, `ssr`, `gbr`, `mach`, `macl`, `pr` and `sr`, but **not `RS`/`RE`** (or `MOD`, `DSR`, `A0/A1/X0/X1/Y0/Y1/M0/M1`) (`inth.S:183-299`). `SR` (and so `RC`) is restored.
- PERF §3.7 proposes converting the next strip **inside the DMA-end callback**, and PERF §3.2 proposes `ldrs`/`ldre` repeat-loop drawers in the foreground.
- If the callback code uses repeat loops, it overwrites `RS`/`RE` of the interrupted drawer loop, which then jumps into the wrong code.
- **Rule:**
  - Code that can run in interrupt context uses `dt`/`bf` loops, or saves and restores `RS`/`RE` itself (`stc rs`/`stc re` … `ldc`).
  - Or follow gpSP: double-buffer strips in the foreground with `dma_transfer_wait` (hardware-proven).
- Also confirmed: gint clears `dma_callbacks[ch]` **after** calling the callback (`dma.c:163-164`), so a callback cannot re-arm the same channel. PERF's ch2/ch5 ping-pong idea is correct.

### C3. UTLB replacement can silently unmap our code [G mechanism, with V pieces]
- **The pieces [V]:**
  - MPM maps code with UTLB entries 0–54 and sets `URB = 54` (`loader.c:20-27`, `main.c:253-266`). Entries 0–53, which include all of our code pages, are therefore candidates for any `LDTLB`.
  - gint calls the OS with the MMU **on**: `drv_mmu` saves only PASCR/IRMCR (`mmu.c` hsave/hrestore), and `syscalls.S` does not touch MMUCR. MPM itself turns the MMU off around OS calls "because the OS will crash in many spots".
  - gpSP saw `GetBlockAddress` return P0 pointers that "dereference while the OS's TLB entry is warm" [V-ext]. So the OS **does load TLB entries** during our world switches, and each such `LDTLB` replaces a pseudo-random entry 0–53.
  - If it hits one of our ~7–8 live code pages, the next instruction fetch there misses. gint's TLB handler then jumps to `0x80020070` with `r0=12` (`tlbh.S:9-12, 33-48`), which crashes on the CG100.
- **Mitigation (cheap):**
  - At startup, snapshot the UTLB address and data arrays for entries 0..N (`0xF6000000`/`0xF7000000 | i<<8`, as MPM's `MMU_UTLB_Get` reads them).
  - After every `gint_world_switch`, verify those entries and rewrite any that changed.
  - Better still, also replace gint's TLB-miss stub at `VBR+0x400` with a 20-instruction handler that reloads from the snapshot (`PTEH`/`PTEL` + `ldtlb`) for addresses in `[0x00300000, 0x00300000+image)`.
  - Few world switches during gameplay limits the exposure anyway.
  - Probe on hardware: dump entries 0–54 before and after `map_file()`.

### C4. Wild pointers inside the mapped window [V]
- All 55 code pages are mapped, regardless of image size (`main.c:253`).
- Under v1.1, the pages past our image map onto mpm.bin (`0x8C7F0000`) and then past the end of RAM (`≥ 0x8C800000`, reported by gpSP to cold-boot).
- NULL (`0x0–0xFFF`) is mapped U:rw onto flash. NULL bugs read flash; NULL writes are cached and written back to the flash bus.
- Hence: keep `RANGECHECK`-style asserts in the host and sim builds, where NULL faults.

### C5. The CG50 "bonus" target is under-provisioned
- Under gint on the CG50 there is no `_ld1`: only `_uram` (~300 KB after BSS) and `_ostk` (350 KiB, which hosts the 177.5 KB VRAM) [V `kernel.c:126-139`, `dvram.c:96-110`]. That leaves about 0.45 MiB.
- CGDoom needed about 0.8 MiB of zone plus 160 KB of PRAM0.
- A working CG50 build needs:
  - a PRAM0 arena for 4-byte-only arrays (gint supports it: `arena_gint.c` `d8005b5`);
  - the old "experimental RAM" probe of `0x8C200000` (OS ≤ 3.60);
  - and the same-g3a CG50 path would run its code from flash through the OS TLB.
- Keep it explicitly out of the v1 scope, or budget for it.

### C6. P0 buffers passed to the OS (low) [G]
- With the MMU on, the OS can dereference our P0 pointers (`.bss` at `0x081xxxxx` lives in protected entries 55–62; rodata at `0x003xxxxx` lives in replaceable entries).
- If any OS file routine uses DMA internally, a P0 address would be misread as physical.
- MPM only ever passes P1 addresses. gpSP passes P0 path strings successfully.
- Cheap insurance: copy path strings and bulk-read destinations into heap (P1) buffers before calling the OS.

### C7. No auto power-off while the add-in runs (low)
- gint owns the interrupts and `PowerOff` is missing for 02.00 in gint dev; PR 50 adds `0x802A2428`.
- A paused game drains the batteries.
- Add an idle timeout: dim the backlight with `r61525_brightness_set`, then offer to quit.
- **Correction (as implemented):** no dimming (the r61525 backlight registers are not known to be valid on the CG100's
  new controller). Launcher screens offer to quit after 10 min without a key change; in a game the limit follows the
  engine's `CGD_IsPaused()`: 10 min for demos and title pages, 30 min for the player's own game (also paused, in the
  menu or on the automap), with a 60 s warning, quitting through the normal exit path (saves and settings written).

### C8. Test sequencing
- PERF P1 (vanilla-exact `FixedMul`/`FixedDiv`) **changes pixels**: CGDoom's helpers round differently in 48% and 34.5% of cases [V, source `m_fixed.c:40-115`; mismatch rate measured by PERF].
- Land P1 **before** recording `tests/golden/*.crc`, or record it as an intentional golden change. Otherwise every later bit-exactness check compares against non-vanilla arithmetic.

---

## D. Claims that remain weak (keep them tagged as guesses)

| Claim | Where | Status |
|---|---|---|
| "35 FPS at stock clock is plausible" | PERF §0.6 | Optimistic. The frame-budget table excludes memory stalls, and C1 suggests those dominate. Plan for auto-frameskip and low detail, and measure. |
| Storage = 4,793,072 B | HW, INT, PREC | One forum poster; no official figure. Budget ≈4.5 MB. |
| Fugue relocates other files' records when writing | gpSP, CGDoom | Plausible and conservative. Keep "defer writes + remap". |
| P0 `0x001129xx` results, `0xA1B97000`, the `0x8C655300` RAM ceiling | gpSP (AI-written comments) | First two: handle defensively (§A8, §C3). The ceiling is self-overlap with gpSP's own dynload-loaded code (PREC §3.1 reading is correct: `(0x8C700000−0x7023C)&~0xFFFF = 0x8C680000`). |
| SH7305 cache = 32+32 KiB, 4-way | HW, PERF | Unknown; probe. |
| SH-4A performance counters exist | PERF §6.4 | Unknown; probe. A bad register access only faults, so the probe is safe behind a menu item. |
| Stock clock = F1 | all | Unknown; display it. |
| Ptune4-F5 stability | PREC, PERF | Per-unit; opt-in only. |
| `0x80396A92` vs `0x80396A06` for 2.10 `BFile_Write` | PREC §7.3 | Irrelevant: no MPM on 2.10 yet. |

---

## E. Hardware probes to build into a diagnostics screen (no hardware available here)

Report all of these through `fxlink` text and/or on screen:

1. OS string, model string, `r61524_get_version()`, `r15` at entry, the `gint_load_info` contents, and `phys(0x00300000)` / `phys(0x08100000)`. These identify the mpm.bin variant and the stack location.
2. FRQCR, FLLFRQ, CS0WCR, CS3WCR, `clock_get_speed()`, with `Iφ`/`Bφ`/`Pφ` from `clock_freq()`.
3. Arena table: name, start, end, free, largest block.
4. WAD map statistics:
   - blocks, P1/P2/P0/invalid counts;
   - contiguous runs, and the lowest and highest physical addresses (this settles §A8);
   - mapping time.
5. UTLB entries 0–54 before and after the block-map world switch (settles §C3).
6. Microbenchmarks with `prof_exec`:
   - 64 KiB sequential read from flash P1 versus SDRAM P1, cold;
   - 320×200 push by CPU versus DMA from SDRAM versus DMA from X/Y strips;
   - each at F1, F4 and opt-in F5.
7. Optional: whether the SH-4A PMU is present (§D), and stride sweeps for the cache size.

---

## F. Concrete amendments to `notes/design.md`

- **§2 memory bullet:** replace it with the three-variant arena discovery (§B3), the minimum-heap refusal, `_os` removal, and the `0x8C700000`/`0x8C800000` limits.
- **§2 on-chip memory:** X and Y are 8 KiB each and aliased (§A2). Reserve both for presenter strips at `0xE5007000`/`0xE5017000`. Put nothing in `GXRAM`/`GYRAM`. ILRAM (≈3.8 KiB free) holds the drawers.
- **§2 world switches:** the clock *is* restored by gint (§A1). After each switch, verify and repair UTLB entries (§C3).
- **§3 `PL_WadMap`:**
  - Pointer validity ends at the next storage write unless writes are deferred.
  - Add a "WAD in RAM" backend chosen when the heap allows (§C1). It drops sound lumps and keeps 4-byte alignment.
  - Accept any P1/P2 block address (§A8).
- **§3 `PL_ScanKeyboard`:** raw KEYSC (CGDoom layout), never `keydown()` with CG50 codes on the CG100 (§A4).
- **§3 file API:** 8.3 names only. Bounce paths and bulk buffers to P1 (§B1, §C6).
- **§4:** the g3a limit is 2 MiB (gint linker script), not 3 MiB. Use the `fxlink -pw build-cg/CGDoom.bin` dev loop (§A9).
- **§6:**
  - Move "level or whole-WAD RAM residency" up to right after P1/P2.
  - No DSP repeat loops in interrupt context (§C2).
  - Generate goldens after P1 (§C8).
  - Treat the 35 FPS target as unproven.
- **§1/§5:** Freedoom cannot be used on the device (28.8 MB). Hardware testing needs the user's shareware `doom1.wad` (§B1).
- **Startup:** full OS-string check (§B6). Record `r15` at entry, or switch to a private stack (§B7).

---

## Sources

- Local, verified in this pass:
  - `ref/gint` `463f180`: `src/cpg/cpg.c`, `src/cpg/overclock.c`, `src/kernel/{world.c,kernel.c,start.c,hardware.c,tlbh.S,inth.S}`, `src/dma/dma.c`, `src/keysc/{keydev.c,keysc.c}`, `src/mmu/mmu.c`, `src/render-cg/{dvram.c,dupdate.c}`, `src/r61524/r61524.c`, `src/fs/fugue/fugue.c`, `include/gint/{clock.h,mmu.h,bfile.h,kmalloc.h}`, `fxcg50.ld.c`.
  - `ref/mpm` (`main.c`, `loader.c`, `start.c`, `casiowin.yaml`, `loader-cg.x`, commit `e99a7d1`, `origin/dynload:main.c`).
  - `ref/extra/gint-prs.git` (PR 62 fygue history: `f1bf037`, `c99a18d`).
  - `ref/extra/gpsp-sh4-jit` (`ports/fxcg100/gint-gpsp/src/{nor_rom.c,gint_platform.c,main.c}`, `fxcg100_gpsp.ld`, `ports/fxcg100/README.md`, `docs/performance-history.md`).
  - `ref/fxsdk/fxlink/modes/push.c`.
  - `ref/CGDoom` (`cgdoom/m_fixed.c`, `cgdoom/cgdoom-kbd.c`).
- Web:
  - [Planète Casio topic18534 page 1](https://www.planet-casio.com/Fr/forums/topic18534-1-mpm-mod-add-ins-math.html): `mpm-bin-199226.zip` link.
  - [Casio fx-CG100 manual, Memory app](https://support.casio.com/global/en/calc/manual/fx-CG100_1AUGRAPH_en/BONDSYacwdbyyk.html): 8-character names, 3-character extensions, no capacity given.
  - [WikiPrizm On-chip Memory](https://prizm.cemetech.net/Technical_Documentation/Peripherals/On-chip_Memory/): X/Y 8 KiB each, page layout.
  - [hpmuseum post-203451](https://www.hpmuseum.org/forum/post-203451.html): the single 4,793,072 B storage figure, seen via search only.
