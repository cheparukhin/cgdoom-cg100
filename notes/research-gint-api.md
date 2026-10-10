# gint (dev) APIs for a high-performance DOOM port on the fx-CG100 (and fx-CG50)

Sources read (local clones, all on `dev` or `main`):

| Repo | Branch / commit | Notes |
|---|---|---|
| `ref/gint` | `dev` @ `463f180` (2026-10-09), version string `2.11.0-91` | Same hash is installed in `/Users/cheparukhin/casio_doom/toolchain/share/fxsdk/sysroot/sh3eb-elf/include/gint/config.h` (`GINT_HASH 0x463f180`) |
| `ref/fxsdk` | `dev` @ `a6e46eb` (2026-08-25) | |
| `ref/mpm` | `main` @ `cbd1a41` (MPM 1.1, 2026-08-17) | `origin/dev` = main; `origin/dynload` holds one old commit |
| `ref/fxlibc` | `dev` @ `a690306` | |
| `ref/extra/gint-prs` | Fresh clone of gint, plus `refs/pull/*` | Used to inspect unmerged PRs #50 and #64 |
| `ref/extra/libfxcg` | GitHub `Jonimoose/libfxcg` @ `daa76d9` | Used for the CG50 syscall `0x1DAA` |

Legend:
- **[V]** verified: seen in the source at the cited path and line.
- **[I]** inference: follows directly from the source, but not tested on hardware.
- **[G]** guess: plausible, needs a hardware test.

---

## 0. Key findings

1. **No CG100-specific build target.** [V] The fx-CG100 runs the same `fxsdk build-cg` g3a as the fx-CG50. gint detects the model at runtime, so code should test `gint[HWCALC] == HWCALC_FXCG100` (value 9; see `include/gint/hardware.h:115` and `src/kernel/hardware.c:139-146`).
2. **CG100 OS calls go through a per-OS-version table of fixed addresses** (`src/kernel/syscalls.S:150-273`). [V] gint dev only knows OS **01.00** (API 1) and **02.00** (API 2) (`src/kernel/kernel.c:47-56`). On **OS 02.10**, which MPM 1.1 supports, gint selects API 0, the fx-CG50 syscall gate at `0x80020070`. [I] Any BFile call, any OS-heap malloc, and `GetVRAMAddress` will then jump into garbage. **The DOOM port must patch this** (§4.4); MPM's `casiowin.yaml` already lists the 2.10 addresses.
3. **Direct memory pointers to file data exist, but gint does not wrap them.**
   - On the CG100, MPM calls `BFile_Block(fd, offset, void **ptr)` at `0x8023ffb2` (OS 1.00), `0x80333cf2` (2.00) and `0x8039675a` (2.10) [V] (`ref/mpm/casiowin.yaml:37`).
   - On the CG50 the equivalent is syscall **`0x1DAA`**, `int Bfile_GetBlockAddress(int handle, int blockAddress, unsigned char **outPtr)`. It returns a direct pointer to a 4 KiB-aligned block of the file [V] (`ref/extra/libfxcg/include/fxcg/file.h:45-52`).
   - gint's own TODO lists "support read-only files backed with GetBlockAddress() on fx-CG" [V] (`ref/gint/TODO`).
   - This allows a zero-copy WAD that is much faster to set up than CGDoom's flash sector search (§4.3).
4. **There is far more heap on the CG100.** [V]/[I] MPM passes gint a RAM range from `0x8c200000` up to the add-in's load address. gint turns it into the default arena `_ld1`, about **4.9-5.4 MiB** for a 0.5-1 MiB binary. The `_uram` arena adds about 300-450 KiB. On the CG50 you only get `_uram`, `_ostk` (350 KiB) and the OS heap (§5).
5. **Display.** [V] gint drives the full 396x224 panel. `r61524_start_frame()` handles both the classic R61524 and the "new 2025 displays", which are detected as controller version 50 or 82 and use MIPI-DCS-style commands `0x2a`/`0x2b`/`0x2c` (`src/r61524/r61524.c:136-176`). The fastest path writes RGB565 to `0xb4000000` after `r61524_start_frame()`, either by CPU or by DMA. Expect about 8 ms for 320x200, because the transfer is bus-bound (§1.5).
6. **The CG100 key matrix is different.** [V]/[I] For rows 5-6, the codes in key events do not match the names that `keydown()` expects (§2.3). Map keys by `ev.row`/`ev.col`, not by `ev.key`.
7. **Overclocking on the CG100** [V] uses the CG50 tables with F5 replaced by F4 (`src/cpg/overclock.c:450-512`). The maximum is F4: CPU at 235.9 MHz, bus at 59 MHz.
8. **Exiting on the CG100.** [V] There is no OS main menu. `gint_osmenu()` is a no-op, `gint_setrestart()` is forced to 0, and `PowerOff`, `Reset` and `SetQuitHandler` are missing, so calling them panics with code `0x10c0`. Return from `main()` to get back to the MPM add-in list.
9. **On-chip memory works on the CG100** [I]: ILRAM is 4 KiB at `0xe5200000` and XYRAM is 16 KiB at `0xe500e000`. gint itself zeroes and loads both on every CG model. However, both regions are **re-initialized after every world switch by default** (§6).
10. **File I/O must run inside `gint_world_switch()`** [V] (`include/gint/bfile.h:5-7`). fxlibc stdio uses a 512-byte `BUFSIZ` (`ref/fxlibc/include/stdio.h:111`), and `fugue_read` calls `BFile_Size` on every read. For bulk loading, call `BFile_Read` directly or use block mapping.

---

## 1. Display

### 1.1 High-level API (render-cg)

| Symbol | Signature | Location | Notes |
|---|---|---|---|
| `DWIDTH`, `DHEIGHT` | `396`, `224` | `include/gint/display-cg.h:29-30` | [V] gint uses the whole panel; the OS uses only 384x216 |
| `gint_vram` | `extern uint16_t *gint_vram;` | `display-cg.h:43` | [V] RGB565 VRAM with a 396-pixel stride |
| `dupdate` | `void dupdate(void);` | `display.h:63`, implemented in `src/render-cg/dupdate.c:19-32` | Calls `r61524_display(gint_vram, 0, 224, method)`. Method is `R61524_DMA_WAIT` (blocking DMA with no interrupts) unless triple buffering is on, in which case it is `R61524_DMA` (async), followed by `dvram_switch()` |
| `dsetvram` | `void dsetvram(uint16_t *main, uint16_t *secondary);` | `display-cg.h:106` | Enables triple buffering. Each buffer is a 32-aligned region of 177472 bytes, and you pass the pointer +32 (32-byte margin on each side) (`display-cg.h:96-98`) |
| `dgetvram` | `void dgetvram(uint16_t **main, uint16_t **secondary);` | `display-cg.h:110` | |
| `dupdate_set_hook` | `void dupdate_set_hook(gint_call_t function);` | `display.h:79` | |
| `dclear` | `void dclear(color_t color);` | `display.h:131`, implemented in `src/render-cg/dclear.c` | Uses `dma_memset` (DMA channel 1) when the window is full width and 4-aligned. gint's `TODO` notes "dclear() now takes 4.1 ms instead of 2.6 ms" |

**VRAM allocation** (`src/render-cg/dvram.c:93-121`) [V]:
- Size is `kmalloc(396*224*2 + 32*2 + 32, arena)`, i.e. 177,504 bytes.
- `arena` is `"_ostk"` if that arena exists (CG50 only), otherwise `NULL` (any default arena). On the CG100 the VRAM therefore lands in `_uram`, or in `_ld1` if `_uram` is full [I].
- The pointer is 32-aligned, offset by +32, and **converted to the uncached P2 alias** (`region = (region & 0x1fffffff) | 0xa0000000`, line 114).
- Triple buffering is off by default since gint 2.8.

From `display-cg.h:85-94` [V]: "sending frames with dupdate() ... takes about 11 ms", and "experience shows minimal performance improvements, because writing to main RAM does not parallelize with DMA transfers".

### 1.2 Low-level R61524 API (`include/gint/drivers/r61524.h`)

| Signature | Line | Notes |
|---|---|---|
| `enum { R61524_DMA, R61524_DMA_WAIT, R61524_CPU };` | 17-24 | |
| `void r61524_display(uint16_t *vram, int start, int height, int method);` | 36 | Source stride is 396*2. The DMA path uses `blocks = 99*(height>>2)` 32-byte blocks, so it needs `height % 4 == 0` (r61524.c:196-209). It always waits for DMA channel 0 first (r61524.c:182) |
| `void r61524_display_rect(uint16_t *vram, int xmin, int xmax, int ymin, int ymax);` | 49-50 | CPU only, inclusive bounds, source stride 396 |
| `void r61524_start_frame(int xmin, int xmax, int ymin, int ymax);` | 76 | **The key primitive.** It sets the window (inclusive) and leaves the controller in "write GRAM" mode, so subsequent 16-bit writes to `0xb4000000` stream pixels |
| `void r61524_win_get(uint16_t *HSA, uint16_t *HEA, uint16_t *VSA, uint16_t *VEA);` | 87 | Reads registers `0x210`-`0x213` |
| `void r61524_win_set(uint16_t HSA, uint16_t HEA, uint16_t VSA, uint16_t VEA);` | 88 | |
| `uint16_t r61524_get(int ID);` | 96 | |
| `void r61524_set(int ID, uint16_t value);` | 100 | |
| `uint16_t r61524_get_version(void);` | **not in the header**; defined at `r61524.c:61-66` | Exported (the installed `libgint-cg.a` has `T _r61524_get_version`). Declare it yourself with `extern` |
| `void r61525_brightness_set(int level);` | not in the header; `r61524.c:98-128` | Backlight levels 1..5 (the name has a typo) |

Hardware interface [V] (`r61524.c:32-48`):
- Data and command port: `volatile uint16_t *DISPLAY = (void *)0xb4000000;`. This is P2, which is physical `0x14000000` (bus area 5A) [I].
- The RS line is PRDR bit 4, `*(volatile uint8_t *)0xa405013c`.
- `select(reg)` clears RS, writes `reg`, then sets RS again, with `synco` after each step.

`r61524_start_frame()` (`r61524.c:136-176`) [V]:
1. It always calls `r61524_get_version()`, which selects `0xda`, does a dummy read, then a real read. That costs a few bus cycles per frame, which is negligible.
2. **New 2025-2026 displays (version 50 or 82)** (commit `2700096`, "r61524: support for latest 2025 bootcode with new displays"):
   - `select(0x2a)`, then write `xmin>>8&3`, `xmin&0xff`, `xmax>>8&3`, `xmax&0xff`.
   - `select(0x2b)`, then the same four writes for y.
   - `select(0x2c)` to enter GRAM write mode.
   - x is **not** mirrored.
3. **Classic R61524:**
   - `r61524_win_set(395-xmax, 395-xmin, ymin, ymax)`; the x axis is mirrored on this panel.
   - Set `HADDR=0`, `VADDR=0`.
   - `select(0x202)` to enter GRAM write mode.

Which controller does the CG100 have? **Unknown [G].** Do not hard-code CGDoom's old register sequence (`cgdoom/i_video.c:64-176` uses registers `0x210`-`0x213` and `0x200`-`0x202` only). Always go through `r61524_start_frame()`, which supports both.

Possible problem on new controllers [G]: the driver's `hsave`/`hrestore` (`r61524.c:352-360`) save and restore registers `0x210`-`0x213`, which belong to the old register map. On a DCS-style controller, the low bytes of those registers are the commands `0x10`-`0x13` (sleep in/out, partial/normal mode). gint seems to work on these displays anyway, but keep this in mind if the screen misbehaves after a world switch.

### 1.3 DMA API (`include/gint/dma.h`)

| Signature | Line |
|---|---|
| `bool dma_transfer_async(int channel, dma_size_t size, uint blocks, void const *src, dma_address_t src_mode, void *dst, dma_address_t dst_mode, gint_call_t callback);` | 62-64 |
| `void dma_transfer_wait(int channel);` | 68 |
| `bool dma_transfer_sync(int channel, dma_size_t size, uint blocks, void const *src, dma_address_t src_mode, void *dst, dma_address_t dst_mode);` | 72-74 |
| `void dma_transfer_atomic(int channel, dma_size_t size, uint blocks, void const *src, dma_address_t src_mode, void *dst, dma_address_t dst_mode);` | 82-84 |
| `void *dma_memset(void *dst, uint32_t pattern, size_t size);` | 109 |
| `void *dma_memcpy(void *dst, const void *src, size_t size);` | 121 |

Supporting enums:
- `dma_size_t`: `DMA_1B=0, DMA_2B=1, DMA_4B=2, DMA_8B=7, DMA_16B=3, DMA_32B=4, DMA_16B_DIV=11, DMA_32B_DIV=12`.
- `dma_address_t`: `DMA_FIXED=0, DMA_INC=1, DMA_DEC=2, DMA_FIXEDDIV=3`.

Facts that matter [V] (`src/dma/dma.c`):
- **Channel usage.** The display uses channel 0 (`r61524.c`). `dma_memcpy`/`dma_memset` use channel 1. Channels 3 and 4 get interrupt priority 9, "used by the USB driver" (dma.c:285-291). **Use channel 0 for your own LCD transfers** so that gint's display functions, which wait on channel 0, stay synchronized.
- `dma_transfer_async` returns `false` if the channel is busy (`CHCR.DE` set; dma.c:94). Call `dma_transfer_wait()` first.
- **Address translation** (`dma_translate`, dma.c:37-64) only converts these ranges:
  - P1/P2 addresses (`0x80000000`-`0xbfffffff`) are masked to physical with `& 0x1fffffff`.
  - ILRAM `0xe5200000`-`0xe5204000`, and the "XRAM" `0xe5007000`-`0xe5009000` / "YRAM" `0xe5017000`-`0xe5019000` windows, pass through unchanged.
  - **P0 addresses are not translated.** Static arrays in `.bss`/`.data` (around `0x081xxxxx`) and ROM (`0x003xxxxx`) cannot be DMA'd directly; use `malloc` memory, which is P1 on both models.
- **Cache coherency.** gint's VRAM is in uncached P2 for exactly this reason. If you DMA from a cached P1 buffer you must first write back the cache lines with `ocbwb` (gint has no public helper; `src/gdb/gdb.c:431-437` shows the loop).
- `dma_transfer_async` blocks sleep mode when either end is in on-chip or SPU memory (dma.c:111-122).
- The XYRAM addresses used by gint's linker script (`0xe500e000`) are **not** the ones whitelisted for DMA (`0xe5007000`/`0xe5017000`). CGDoom on the CG50 uses `0xe5007000` as "XRAM" for CPU access and BFile buffers (`cgdoom/cgdoom.c:243`). Whether `0xe500e000` and `0xe5007000` alias the same RAM is **unverified [G]**.

### 1.4 Video interface (`include/gint/video.h`)

`bool video_update(int x, int y, image_t const *fb, int flags);` with flags `VIDEO_UPDATE_ENABLE_DMA=0x01`, `VIDEO_UPDATE_ATOMIC=0x02`, `VIDEO_UPDATE_FOREIGN_WORLD=0x04`.

The R61524 backend (`r61524.c:309-346`) [V] only uses DMA when `x==0 && w==396 && stride==792 && h%4==0`. Otherwise it uses a CPU loop. A 320x200 framebuffer at (38,12) therefore goes through the CPU path; calling the primitives yourself is better.

### 1.5 Pushing a 320x200 8-bit frame to the 396x224 panel as fast as possible

Geometry: centre the image with the window `x = 38..357`, `y = 12..211` (inclusive). Clear the 38-pixel and 12-pixel borders to black once, e.g. a full-screen black fill at startup.

Cost model [I]: Lephe measures 11 ms for a 396x224 DMA frame, i.e. 88,704 pixels, or about **124 ns per pixel**. The LCD sits on bus area 5A, whose wait states are set by `CS5aWCR = 0x000203C1`, the same value at every CG100 F-level (`overclock.c:450-512`). The transfer is therefore bus-bound, and 320x200 = 64,000 pixels should take about **7.9 ms** whether the CPU or the DMA does the writes. At 35 fps (28.57 ms per tic) that is roughly 28% of the frame. The only way to win is to **overlap** the transfer with other CPU work.

**Option A: CPU streaming (CGDoom's approach, simplest)** [I]

```c
#include <gint/drivers/r61524.h>
#include <gint/dma.h>
#include <stdint.h>

static uint16_t pal16[256];                     /* RGB565, built from PLAYPAL + gamma */
#define LCD (*(volatile uint16_t *)0xb4000000)

void I_FinishUpdate_cpu(uint8_t const *src /* 320*200, 4-aligned */)
{
    dma_transfer_wait(0);                       /* never interleave with a running DMA */
    r61524_start_frame(38, 357, 12, 211);
    uint32_t const *s = (void *)src;
    uint16_t const *p = pal16;
    for(int i = 0; i < 320*200/4; i++) {        /* big-endian: MSB = leftmost pixel */
        uint32_t q = *s++;
        LCD = p[q >> 24];
        LCD = p[(q >> 16) & 0xff];
        LCD = p[(q >> 8) & 0xff];
        LCD = p[q & 0xff];
    }
}
```

The palette lookup hides inside the stall of each device write, so this loop is fully bus-bound and keeps the CPU busy for about 8 ms.

**Option B: expand, then async DMA, overlapped with the next frame (recommended to try first on the CG100)** [I]

- Keep a contiguous 320x200 RGB565 buffer of 128,000 bytes, which fits easily in the CG100 heap. Allocate it with `malloc`, which returns P1 memory, and align it to 32 bytes.
- Each row is 640 bytes = 20 x 32 B, so the frame is exactly **4000 `DMA_32B` blocks**. The "full horizontal range" restriction in the header comment only exists because gint's VRAM has a 396 stride. With a contiguous 320-wide buffer, any height works [I].

```c
static uint16_t *fb16;   /* 32-aligned; use the P2 alias, or ocbwb after filling */

void I_FinishUpdate_dma(uint8_t const *src)
{
    dma_transfer_wait(0);                         /* previous frame finished? */
    /* 1. Palette-expand src -> fb16. Either write through the uncached P2 alias,
          or write a cached P1 alias and then ocbwb each 32-byte line. */
    expand_p8_to_rgb565(fb16, src, pal16);
    /* 2. Point the controller at the window and start the async DMA */
    r61524_start_frame(38, 357, 12, 211);
    dma_transfer_async(0, DMA_32B, 320*200*2/32, fb16, DMA_INC,
                       (void *)0xb4000000, DMA_FIXED, GINT_CALL_NULL);
    /* 3. Return immediately: game logic and rendering of the next frame
          overlap the ~8 ms transfer. */
}
```

- The expansion itself costs about 64k byte loads + 64k halfword stores. At 236 MHz with cache hits that is well under 1-2 ms [G].
- Caveat from gint's author: main-RAM traffic and DMA do not parallelize well. Benchmark both options with `prof_exec` (§3).
- **Variant B2** [G]: double-buffered stripes in on-chip memory (e.g. 2 x 10 lines = 2 x 6400 B in XYRAM). Expand stripe *n+1* while the DMA sends stripe *n*, all after a single `r61524_start_frame()`. Consecutive DMA bursts keep streaming into GRAM. This needs the XYRAM DMA alias to be confirmed (§1.3).
- **Variant B3** [G]: gint's optimized P8 blitter.
  - Build an `image_t` with `format = IMAGE_P8_RGB565`, `color_count = 256`, `width = 320`, `height = 200`, `stride = 320`, `data = screen`, and `palette = rot`, where `rot[i] = pal16[i ^ 0x80]`. P8 data is a signed index shifted by 128 (`include/gint/image.h:108-110`).
  - Draw it with `dimage(38, 12, &img)` and push with `dupdate()`.
  - This writes into the **uncached** gint VRAM and then DMAs 88,704 pixels instead of 64,000, so it is probably slower than A or B. It is mainly useful for menus.

Things to avoid:
- `gint_vram` is uncached. Do not render DOOM columns into it.
- `r61524_display_rect` is CPU-only and uses a 396 stride.

**Status bar and frame.**
- gint has **no API** for the OS status bar or frame [V]. `___ConfigureStatusArea` is declared `.global` in `syscalls.S:51` but has no CG implementation. The OS status bar is simply not drawn while gint owns the display; you own all 396x224 pixels.
- On the CG100, after the add-in returns, MPM redraws its frame with `CW_DrawFrameC3(0)` and re-renders the add-in list [V] (`ref/mpm/main.c:499-503`, commit `05ec271`).
- On exit, gint restores the window registers through `drv_r61524.hrestore` [V]. On the CG50 the OS redraws its own screen.
- Nothing extra is needed when leaving. CGDoom's `I_ShutdownGraphics` `SetWindow(6,0,384,216)` is the equivalent of what gint does automatically [I].

---

## 2. Keyboard

### 2.1 API (`include/gint/keyboard.h`)

| Signature | Line |
|---|---|
| `key_event_t pollevent(void);` (returns `type==KEYEV_NONE` when the queue is empty) | 191 |
| `key_event_t waitevent(volatile int *timeout);` | 198 |
| `void clearevents(void);` | 201 |
| `void cleareventflips(void);` | 207 |
| `int keydown(int key);` | 216 |
| `int keydown_all(int key1, ...);` (0-terminated) | 221 |
| `int keydown_any(int key1, ...);` (0-terminated) | 226 |
| `int keypressed(int key);` / `int keyreleased(int key);` | 231 / 236 |
| `key_event_t getkey(void);` = `getkey_opt(GETKEY_DEFAULT, NULL)` | 257 |
| `key_event_t getkey_opt(int options, volatile int *timeout);` | 317 |
| `void keysc_set_scan_frequency(int freq);` (64..32768 Hz) | 180 |

Event structure:
- `key_event_t` holds `u16 time; mod:1, shift:1, alpha:1, type:3; u8 key;` followed by a union of `{u8 row, col}` and `{u16 x, y}`, with `GPACKED(4)`.
- Event types: `KEYEV_NONE=0, DOWN=1, UP=2, HOLD=3, OSMENU=4, TOUCH_DOWN/DRAG/UP=5/6/7`.

getkey options:
- `GETKEY_DEFAULT = 0x05df` (line 285). It includes `GETKEY_POWEROFF` (0x400), `GETKEY_MENU` (0x08), `GETKEY_BACKLIGHT`, `GETKEY_REP_ARROWS`, modifiers and features.

Driver behaviour [V] (`src/keysc/keysc.c`):
- The keyboard is scanned at **128 Hz** by an ETMU timer (`keysc_scan_Hz = 128`), reading 6 x 16-bit `KEYSC` registers at `0xa44b0000`.
- The event queue holds 32 events (`KEYBOARD_QUEUE_SIZE`, `drivers/keydev.h:16`).
- `keydown()` reflects the state *according to the events consumed so far*, so drain events every tic.
- Repeats are on by default (400 ms / 40 ms) and produce `KEYEV_HOLD` events. Ignore them, or disable them with `keydev_set_transform(keydev_std(), (keydev_transform_t){ 0, NULL })`.
- `SHIFT+7+3+AC/ON` calls `abort()` (keysc.c, "Freeze abort key combo").

### 2.2 Key codes (`include/gint/keycodes.h`)

Classic CG50 matrix codes are `(row << 4) | (7 - col)`:
- `KEY_F1..F6 = 0x91..0x96`
- `SHIFT 0x81, OPTN 0x82, VARS 0x83, MENU 0x84, LEFT 0x85, UP 0x86`
- `ALPHA 0x71, SQUARE 0x72, POWER 0x73, EXIT 0x74, DOWN 0x75, RIGHT 0x76`
- `XOT 0x61, LOG 0x62, LN 0x63, SIN 0x64, COS 0x65, TAN 0x66`
- `FRAC 0x51, FD 0x52, LEFTP 0x53, RIGHTP 0x54, COMMA 0x55, ARROW 0x56`
- `7 0x41, 8 0x42, 9 0x43, DEL 0x44`
- `4..DIV 0x31..0x35`, `1..SUB 0x21..0x25`, `0 0x11, DOT 0x12, EXP 0x13, NEG 0x14, EXE 0x15`
- `ACON 0x07`

CG100 additions (lines 89-103):
- New codes: `KEY_ON 0xa6, KEY_PREVTAB 0xa7, KEY_NEXTTAB 0xa8, KEY_PAGEUP 0xa9, KEY_PAGEDOWN 0xaa, KEY_SETTINGS 0xab, KEY_OK 0xac, KEY_CATALOG 0xad, KEY_SQRT 0xae, KEY_EXPFUN 0xaf`.
- Aliases: `KEY_HOME = KEY_MENU`, `KEY_BACK = KEY_EXIT`, `KEY_TOOLS = KEY_OPTN`, `KEY_FORMAT = KEY_FD`.

### 2.3 CG100 physical layout as implemented by gint (`src/keysc/keydev.c:49-95`)

`CG100_keymap[]` lists rows 9..5, columns 6..1. Translations are only applied when `gint[HWCALC]==HWCALC_FXCG100`.

| row \ col | 6 | 5 | 4 | 3 | 2 | 1 |
|---|---|---|---|---|---|---|
| 9 | ON | HOME (=MENU) | PREVTAB | UP | NEXTTAB | PAGEUP |
| 8 | SETTINGS | BACK (=EXIT) | LEFT | OK | RIGHT | PAGEDOWN |
| 7 | SHIFT | ALPHA | VARS | DOWN | CATALOG | TOOLS (=OPTN) |
| 6 | XOT | FRAC | SQRT | POWER | SQUARE | EXPFUN |
| 5 | COMMA | SIN | COS | TAN | LEFTP | RIGHTP |
| 4 | 7 | 8 | 9 | DEL | (0x45, unnamed) | — |
| 3 | 4 | 5 | 6 | MUL | DIV | — |
| 2 | 1 | 2 | 3 | ADD | SUB | — |
| 1 | 0 | DOT | EXP | **FORMAT** (where the CG50 has NEG) | EXE | — |
| 0 | AC is `KEY_ACON` (row 0, col 0); MPM reads `KEYSC[0]==1` as "the AC key" (`ref/mpm/main.c:192-198`) [V] | | | | | |

**Inconsistency (derived from the code [I]):**
- `keymatrix_to_keycode()`, which produces `ev.key`, only translates **rows 7-9** plus row 1/col 3. Rows 5-6 keep the **legacy CG50 codes**. Pressing the physical x^y key (r6 c3) gives `ev.key == KEY_SIN` (0x64), and the physical sin key (r5 c5) gives `ev.key == KEY_FD`, which is the same value as `KEY_FORMAT`.
- `keydown()`/`keycode_to_keymatrix()` translates through the **whole** 30-entry table. So `keydown(KEY_POWER)` checks r6 c3, while `keydown(KEY_SIN)` checks r5 c5.
- Codes that are not in the table and whose row is above 4, or which equal NEG, are forced to the nonexistent position (0,1) (lines 86-92). As a result, on the CG100 `keydown(KEY_F1..F6)`, `keydown(KEY_LOG)`, `keydown(KEY_LN)` and `keydown(KEY_NEG)` are **always false**.
- Holding **CATALOG** while pressing a row-6 key turns `ev.key` into `KEY_F1 + (6-col)` (keydev.c:274-277). This affects events only.
- **Recommendation:** translate events by physical position with `ev.row`/`ev.col` through a per-model table, or only use names whose two directions agree: rows 7-9, rows 1-4, and `KEY_ACON`.

Event loop for DOOM:

```c
key_event_t ev;
while((ev = pollevent()).type != KEYEV_NONE) {
    if(ev.type != KEYEV_DOWN && ev.type != KEYEV_UP) continue;   /* drop HOLD, OSMENU */
    int dk = keymap[(ev.row << 3) | ev.col];   /* keymap picked at init from gint[HWCALC] */
    if(dk) { event_t e = { ev.type == KEYEV_DOWN ? ev_keydown : ev_keyup, dk }; D_PostEvent(&e); }
}
```

**getkey on the CG100** [V]/[I]:
- `GETKEY_MENU` calls `gint_osmenu()`, which on the CG100 is only a world-switch round trip (`osmenu.c:96-101`).
- `GETKEY_POWEROFF` (SHIFT+AC) calls `gint_poweroff()`, then `__PowerOff`. That entry is 0 for API 1/2, so it panics with `0x10c0` "Missing syscall for this OS version". On OS 2.10 (API 0) it jumps to `0x80020070`.
- Use `getkey_opt(GETKEY_DEFAULT & ~(GETKEY_POWEROFF | GETKEY_MENU), NULL)` on the CG100.

---

## 3. Timing

### 3.1 Profiling (`include/gint/prof.h`; libprof was merged into gint by commit `bc5dd82`)

- `int prof_init(void);` (line 36). This must be called explicitly. It claims a TMU at `Pphi/4` with `TCOR=0xffffffff`, trying TMU2, then TMU1, then TMU0.
- `void prof_quit(void);`
- `prof_t` is `{ uint32_t rec, elapsed; }`, created with `prof_make()`.
- `prof_enter(p)` / `prof_leave(p)` (lines 67-78) are recursion-aware macros that read `*prof_tcnt`, a down-counter. `prof_enter_norec`/`prof_leave_norec` skip the recursion counter.
- `uint32_t prof_time(prof_t prof);` (line 110) returns microseconds, computed as `elapsed*4*1e6/Pphi_f` (`src/prof/prof.c:52-56`). It uses the **current** Pphi, so do not change the clock between enter and leave.
- `prof_exec({ code; })` returns the elapsed µs.
- Resolution: Pphi/4 = 7.3728 MHz at F1/F2/F4, about **136 ns** per tick.

### 3.2 Timers (`include/gint/timer.h`, `src/tmu/tmu.c`)

- `int timer_configure(int timer, uint64_t delay_us, gint_call_t callback);` (142)
- `void timer_start(int)` (147), `timer_pause`, `void timer_stop(int)` (157), `timer_wait`, `timer_spinwait`
- `uint32_t timer_delay(int timer, uint64_t delay_us, int clock);` (194), `timer_reload`

Available hardware: 3 TMUs (IDs 0-2, Pphi/{4,16,64,256}) and 6 ETMUs (IDs 3-8, 32768 Hz). `timer_count()` is 9 on SH4.
- `TIMER_ANY` searches **from ID 8 downward**, so it picks an ETMU for delays over 0.1 ms.
- `TIMER_TMU` and `TIMER_ETMU` force the timer kind.
- Callbacks return `TIMER_CONTINUE` (0) or `TIMER_STOP` (1).
- Predefined callbacks: `GINT_CALL_SET`, `GINT_CALL_INC`, `GINT_CALL_SET_STOP`, `GINT_CALL_INC_STOP` (`defs/call.h:192-230`).
- The keyboard owns one ETMU and `prof_init` owns one TMU.
- Timers are rescaled automatically when the clock speed changes (`timer_rescale` in `cpg_set_overclock_setting`, overclock.c:139-141).

### 3.3 Other clocks

- `uint32_t rtc_ticks(void);` (`rtc.h:52`) returns 128 Hz ticks since midnight, computed from `rtc_get_time()` (`src/rtc/rtc_ticks.c`). It is slow (several BCD register reads) and wraps at midnight.
- `bool rtc_periodic_enable(int frequency, gint_call_t callback);` supports `RTC_64Hz`, `RTC_256Hz`, etc. There is no 35 Hz option.
- `const clock_frequency_t *clock_freq(void);` gives `Iphi_f`, `Bphi_f`, `Pphi_f`, etc.
- `void sleep_us(uint64_t)`, `sleep_us_spin`, `sleep_ms(ms)` (`clock.h:169-181`).
- `void sleep(void)` (`cpu.h:102`) is the SH `sleep` instruction: it waits for the next interrupt and saves power.

### 3.4 A fixed 35 Hz tic counter

```c
#include <gint/timer.h>
static volatile int doom_tics;
void I_InitTimer(void) {
    /* TIMER_TMU gives ~0.0015% error: Pphi/4 * 28571 us = 210,646 counts.
       TIMER_ANY would pick an ETMU: 32768*28571/1e6 = 936 counts -> ~0.1% error. */
    int t = timer_configure(TIMER_TMU, 1000000/35, GINT_CALL_INC((int volatile *)&doom_tics));
    if(t < 0) t = timer_configure(TIMER_ANY, 1000000/35, GINT_CALL_INC((int volatile *)&doom_tics));
    timer_start(t);
}
int I_GetTime(void) { return doom_tics; }
/* To idle until the next tic: while(doom_tics == last) sleep(); */
```

TMUs are scarce (3 in total, one taken by `prof_init`). If you want TMUs for something else, the ETMU error (about 0.08-0.1%) is acceptable for DOOM.

---

## 4. Files

### 4.1 POSIX and stdio over Fugue

- gint implements `open`, `read`, `write`, `lseek`, `pread`, `pwrite`, `close`, `creat`, `mkdir`, `rmdir`, `unlink`, `rename`, `stat`, `opendir`, `readdir`, `closedir`, `rewinddir`, `seekdir`, `telldir` and `fdopendir` (`src/fs/*.c`), dispatched to `src/fs/fugue/*`. fxlibc's `fopen`/`fread` sit on top of those.
- **None of them world-switch internally** [V]; for example `open()` calls `fugue_open()` directly (`src/fs/open.c`).
- `bfile.h:5-7` says: "BFile cannot be used from within gint, so do a gint_world_switch() ... otherwise the add-in is likely to crash."

Performance pitfalls [V]:
- `fugue_read` calls `BFile_Size()` before every `BFile_Read(fd, buf, size, -1)` (`src/fs/fugue/fugue.c:13-33`).
- `pread` does three `lseek`s.
- stdio `BUFSIZ` is 512 (`ref/fxlibc/include/stdio.h:111`), and buffered `fread` refills 512 bytes at a time (`src/stdio/fileutil.c:81-140`).
- Fix: use `setvbuf(fp, NULL, _IONBF, 0)` or `read()` with large chunks; better still, call `BFile_Read` directly with an explicit offset.
- `exit()` does **not** flush or close streams (`ref/fxlibc/src/stdlib/exit.c:5-14`), and `abort()` has "TODO: Close BFile handles". Close every file before leaving.
- Paths use FONTCHARACTER, e.g. `u"\\\\fls0\\doom1.wad"`; MPM uses the same root.

### 4.2 Every BFile-like function available on the CG100 in gint dev

They are exported by `src/kernel/syscalls.S` and declared in `include/gint/bfile.h`. Call them only inside `gint_world_switch()`.

| # | gint symbol (bfile.h line) | CG50 syscall (API 0) | CG100 OS 01.00 (API 1) | CG100 OS 02.00 (API 2) | CG100 OS 02.10 (MPM `casiowin.yaml`, **missing in gint**) |
|---|---|---|---|---|---|
| 0 | `__malloc` (OS heap, arena `_os`) | 0x1f44 | 0x8025e0fc | 0x80366708 | 0x803c9ccc |
| 1 | `__free` | 0x1f42 | 0x8025dec8 | 0x803664d4 | 0x803c9a98 |
| 2 | `__realloc` | 0x1f46 | 0x8025ec3c | 0x803672c8 | 0x803ca890 |
| 3 | `int BFile_Remove(uint16_t const *path)` (79) | 0x1db4 | 0x802404d2 | 0x80334212 | 0x80396c7a |
| 4 | `int BFile_Rename(uint16_t const *old, uint16_t const *new)` (82) | 0x1db3 | 0x80240482 | 0x803341c2 | 0x80396c2a |
| 5 | `int BFile_Create(uint16_t const *path, int type, int *size)` (97) | 0x1dae | 0x802401b0 | 0x80333ef0 | 0x80396958 |
| 6 | `int BFile_Open(uint16_t const *path, int mode)` (115); gint forces r6=0 | 0x1da3 | 0x8023fb90 | 0x803338d0 | 0x80396338 |
| 7 | `int BFile_Close(int fd)` (118) | 0x1da4 | 0x8023fd0e | 0x80333a4e | 0x803964b6 |
| 8 | `int BFile_Size(int fd)` (121) | 0x1da6 | 0x8023fdc4 | 0x80333b04 | 0x8039656c |
| 9 | `int BFile_Seek(int fd, int offset)` (161) | 0x1da9 | 0x8023ff2c | 0x80333c6c | 0x803966d4 |
| 10 | `int BFile_GetPos(int fd)` (177) | 0x1dab | 0x8024003c | 0x80333d7c | 0x803967e4 |
| 11 | `int BFile_Write(int fd, void const *data, int even_size)` (130); "WriteFsync" | 0x1daf | 0x8024025e | 0x80333f9e | 0x80396a92 |
| 12 | `int BFile_Read(int fd, void *data, int size, int whence)` (148); `whence>=0` is an absolute offset, `-1` is the current position | 0x1dac | 0x80240082 | 0x80333dc2 | 0x8039682a |
| 13 | `int BFile_FindFirst(uint16_t const *pattern, int *shandle, uint16_t *found, struct BFile_FileInfo *info)` (287) | 0x1db6 | 0x80240888 | 0x803345c8 | 0x80397030 |
| 14 | `int BFile_FindNext(int shandle, uint16_t *found, struct BFile_FileInfo *info)` (299) | 0x1db8 | 0x80240b06 | 0x80334846 | 0x803972ae |
| 15 | `int BFile_FindClose(int shandle)` (303) | 0x1dba | 0x80240c10 | 0x80334950 | 0x803973b8 |
| 16-19 | `__Timer_Install/Start/Stop/Deinstall` (internal) | 0x08d9/0x08db/0x08dc/0x08da | 0x800b130c/1456/14b2/13d4 | 0x8010de28/df72/dfce/def0 | 0x8010f398/f4e2/f53e/f460 |
| 20 | `__PutKeyCode` | 0x12c6 | **0** | **0** | — |
| 21 | `__GetKeyWait` | 0x12bf | 0x8017be56 | 0x802382fe | 0x80299daa |
| 22 | `__ClearKeyBuffer` | 0x12c7 | **0** | **0** | — |
| 23 | `__GetVRAMAddress` | 0x01e6 | 0x8004579a | 0x8007569e | 0x80076262 |
| 24 | `__SetQuitHandler` | 0x1e6e | **0** | **0** | — |
| 25 | `__PowerOff` | 0x1839 | **0** | **0**; unmerged PR #50 adds `0x802a2428` | — |
| 26 | `__Reset` | 0x1187 | **0** | **0**; PR #50 adds `0x8022fcec` | — |
| — | `int BFile_Ext_Stat(uint16_t const *path, int *type, int *size)` (314) | Implemented in C over FindFirst (`src/fs/fugue/BFile_Ext_Stat.c`) | | | |
| — | **Not in gint:** `BFile_Block` / `Bfile_GetBlockAddress` | **0x1daa** (libfxcg) | **0x8023ffb2** | **0x80333cf2** | **0x8039675a** |
| — | Not in gint: `BFile_GetMediaFree` (unmerged PR #64) | 0x1da5 (inferred by the PR author) | 0x8023fd7a | 0x80333aba | ? |
| — | Not in gint: `BFile_NameToStr_ncpy` / `StrToName_ncpy` | 0x1ddb / 0x1ddc | 0x80241b66 / 0x80241bac | 0x803358a6 / 0x803358ec | 0x8039830e / 0x80398354 |

A `0` entry means `_CASIOWIN_call` jumps to `gint_panic(0x10c0)` (`syscalls.S:173-184`). `_BFile_FStat/_Mkdir/_Stat/_Flush` are declared `.global` but exist only on the fx-CP.

**How the dispatch works** [V] (`syscalls.S:155-208`):
1. Each stub does `bra _CASIOWIN_call; mov #id, r0`.
2. `_CASIOWIN_call` loads `table[id*3 + API]`.
3. If `API==0`, the value is a syscall number and it jumps to the CG50 gate at `0x80020070`. Otherwise it is a direct function address and it `jmp`s there; a value of 0 means a missing call, which panics.
4. `_CASIOWIN_call` is a **local** symbol (`t` in `nm`), so it cannot be reused. `gint_set_CASIOWIN_API(int)` and `gint_get_CASIOWIN_API(void)` are exported but only accept 0..2.
5. Unlike MPM's `_CASIOWIN_call` (`ref/mpm/casiowin.S:43-63`), gint does **not** turn off the MMU around OS calls. MPM's comment says "The OS will crash in many spots if we keep it on".

**API selection** [V] (`src/kernel/kernel.c:47-56`):
- Version string `"01.00"` at `0x80020020` selects API 1.
- `CG100 && "02.00"` selects API 2. This 5-character prefix also matches `02.00.0010`, for which MPM has no addresses (`ref/mpm/start.c:37-39`).
- Anything else selects API 0. This includes **CG100 OS 02.10**, which then crashes on the first OS call [I].

### 4.3 Direct (memory-mapped) access to file data

**CG50:** syscall `0x1DAA`, `int Bfile_GetBlockAddress(int handle, int blockAddress, unsigned char **outPtr);`. libfxcg's comment [V] (`ref/extra/libfxcg/include/fxcg/file.h:45-52`): "Given a file handle and a 4 KB aligned block address within the file, returns the direct memory access pointer for that file. !Warning! This address is no longer valid if any OS file operations are performed."

**CG100:** MPM declares `int CW_BFile_Block(int fd, int whence, void **block_ptr);` [V] (`ref/mpm/casiowin.h:171-172`), at the addresses listed in §4.2.
- MPM uses it with offsets `0`, `0x1000`, `0x2000`, ... and reads up to 0x1000 bytes per returned pointer (`ref/mpm/addins.c:22-42, 57-67`), so a block is 4 KiB.
- MPM **keeps the pointers after `BFile_Close` and after opening other files** to draw icons later [V]. In practice the pointers survive read-only activity [I].
- Its address sits between `BFile_Seek` and `BFile_GetPos` in every OS image, exactly where `0x1DAA` sits between `0x1DA9` (Seek) and `0x1DAB` (GetPos) in the CG50 numbering. That strongly suggests the same function [I].
- The meaning of a positive return value is unknown; only `rc < 0` is treated as an error [G].
- Whether the pointer is a P1 or P2 flash address is unknown. CGDoom found the cached flash alias `0x80000000`-`0x82000000` "much faster for actual data loads while in-game" (`ref/CGDoom/src-cg/platform.h:9-16`), so normalize with `(p & 0x1fffffff) | 0x80000000` [G].

Suggested wrapper, called from the OS world:

```c
/* asm (CG50):
     .global _cg50_Bfile_GetBlockAddress
   _cg50_Bfile_GetBlockAddress:
     mov.l 1f, r2 ; mov.l 2f, r0 ; jmp @r2 ; nop
     .balign 4
   1: .long 0x80020070
   2: .long 0x1daa                                                        */
extern int cg50_Bfile_GetBlockAddress(int fd, int off, void **ptr);
typedef int (*bblock_fn)(int fd, int off, void **ptr);

static bblock_fn find_bfile_block(void) {
    if(gint[HWCALC] != HWCALC_FXCG100) return cg50_Bfile_GetBlockAddress;
    char const *v = (void *)0x80020020;
    if(!memcmp(v, "01.00.0000", 10)) return (bblock_fn)0x8023ffb2;
    if(!memcmp(v, "02.00.0000", 10)) return (bblock_fn)0x80333cf2;
    if(!memcmp(v, "02.10.0000", 10)) return (bblock_fn)0x8039675a;
    return NULL;                          /* unknown OS: fall back to BFile_Read */
}
static int map_file(int fd, int size, void *table /* void *[size/4096+1] */) {
    bblock_fn f = find_bfile_block(); void **t = table;
    if(!f) return -1;
    for(int i = 0, off = 0; off < size; i++, off += 4096) {
        int rc = f(fd, off, &t[i]);
        if(rc < 0) return rc;
    }
    return 0;
}
/* in gint world: */
int rc = gint_world_switch(GINT_CALL(map_file, fd, size, (void *)blocks));
```

- Runs of blocks where `t[i+1] == t[i] + 4096` are contiguous. A lump that lies entirely inside one run can be returned as a read-only pointer, with no copy and no heap use.
- A 4 MB WAD needs only about 1000 table entries.
- Re-map after every **write**, for example after saving a game.

### 4.4 Required fix for CG100 OS 02.10

Choose one:
- **(a)** Patch gint: set `CASIOWIN_API_VERSIONS 4`, add a fourth column with the 2.10 addresses from MPM, and add `else if(CG100 && !memcmp(version, "02.10", 5)) gint_set_CASIOWIN_API(3);` in `kinit`.
- **(b)** Keep stock gint, but on unknown or 2.10 OS versions route all file I/O through the port's own table of function pointers (as in §4.3), and remove the `_os` arena with `kmalloc_remove_arena(kmalloc_get_arena("_os"))` so malloc never falls through to an OS heap call.

Also guard against unknown future OS versions: refuse file operations and show a message.

### 4.5 World switch (`include/gint/gint.h`, `src/kernel/world.c`)

- `int gint_world_switch(gint_call_t function);` (gint.h:36).
- Build the call with `GINT_CALL(func, a1..a4)`, **at most 4 arguments** of type int, float or pointer. `void **` must be cast to `void *`.
- Cost of each switch [V]:
  - Every driver unbinds and saves/restores its state (`world.c:115-147`).
  - The CPG driver restores the OS clock settings, unless `cpg_set_overclock_permanent(true)` was called (overclock.c:146-155). This runs `gint_world_sync()` and rescales the timers.
  - On-chip memory is reinitialized on return (§6).
  - A stack canary is checked at `gint_stack_top` (panic `0x1080`).
- Conclusion: batch file work into few switches. Load or map the WAD once at startup, and save games in a single switch.

---

## 5. Memory (`include/gint/kmalloc.h`, `src/kmalloc/*`, `src/kernel/kernel.c`)

### 5.1 API

| Signature | Line |
|---|---|
| `void *kmalloc(size_t size, char const *arena_name);` (`NULL` means all default arenas, in order) | 26 |
| `void *krealloc(void *ptr, size_t size);` (falls back to other arenas) | 37 |
| `void kfree(void *ptr);` | 40 |
| `void *kmalloc_max(size_t *size, char const *arena_name);` (largest free block; gint arenas only; the name cannot be NULL) | 54 |
| `void kmalloc_init_arena(kmalloc_arena_t *a, bool enable_statistics);` (needs at least 256 bytes; sets `malloc`, `realloc`, `free`, `malloc_max`, `data`) | 103 |
| `bool kmalloc_add_arena(kmalloc_arena_t *arena);` | 109 |
| `void kmalloc_remove_arena(kmalloc_arena_t *arena);` | ~114 |
| `kmalloc_arena_t *kmalloc_get_arena(char const *name);` | 124 |
| `kmalloc_gint_stats_t *kmalloc_get_gint_stats(kmalloc_arena_t *arena);` (free, used and peak memory) | ~160 |

Allocator facts [V]:
- At most **8 arenas** (`KMALLOC_ARENA_MAX`). The OS heap `_os` always occupies the last slot.
- Default allocations try the arenas in insertion order.
- `malloc`/`free`/`realloc` in fxlibc map to `kmalloc(size, NULL)`/`kfree`/`krealloc` (`src/fxlibc_hal.c`).
- Blocks are **4-byte aligned**, with a 4-byte header and a maximum size of 2^24 bytes. Align manually for 32-byte DMA.
- The allocator only uses 32-bit accesses for its own metadata, so it can manage SPU PRAM0 (commit `d8005b5`).
- Arena names starting with `_` are reserved for gint.

### 5.2 Default arenas

| Arena | fx-CG50 | fx-CG100 (MPM) |
|---|---|---|
| `_uram` (default) | Static user RAM after `.data`/`.bss`/`.gint.bss`, up to `uram + uram_size - 16 KiB`. User RAM is 512 KiB mapped at `0x08100000`; the arena is addressed via P1 `mmu_uram()` (kernel.c:79-87, 113-123). | Same. MPM maps 8 x 64 KiB pages `0x08100000` -> `0x8c160000` (`CW_AppMemory`, `ref/mpm/casiowin.yaml:71`; `main.c:258-260`) [V] |
| `_ostk` (default) | 350 KiB at `0x8c0f0000` (`0x880f0000` on Prizm and the emulator), part of the OS stack. Disabled by building gint with `GINT_NO_OS_STACK` (kernel.c:126-139). **gint's VRAM (177,504 B) is allocated here.** | **Disabled** on the CG100 (kernel.c:127) |
| `_ld1`.. `_ld4` (default) | — | Built from MPM's load info (kernel.c:141-166). MPM 1.1 passes `{1,1, 0x10,0x8c200000, 0x11,loadAddress, 0}` (`ref/mpm/main.c:345-357`), where `loadAddress = (0x8c7f0000 - (g3a_size - 0x7000)) & ~0xffff` (main.c:211-229). Examples: a 1,000,000-byte binary gives `0x8c6f0000`, i.e. **4.94 MiB**; a 512 KiB binary gives **5.44 MiB**. The binary itself may be at most `0x370000` bytes (main.c:212) |
| `_os` (default, last) | OS heap via syscall `0x1f44` etc. | OS heap via direct calls; **crashes on OS 2.10** with gint dev |

How gint receives MPM's information [V]:
- `start(int load_type, u32 *load_info)` is the entry point, placed in `.text.entry` at `0x00300000`.
- It keeps `load_info` only if `load_type == 0x4d504d30` (`'MPM0'`) and the pointer is 4-aligned (`src/kernel/start.c:180-182`). It then calls `kinit(load_info)`.
- `kinit` stores the pointer in `gint_load_info` and scans pairs: `0x10` is an arena start, `0x11` an arena end. It builds up to four default arenas `_ld1`..`_ld4`; the arena structs themselves are `kmalloc`'d from `_uram`.

**Other CG100 memory facts from MPM** [V]:
- The add-in "ROM" is **RAM**: MPM copies the binary (without the 0x7000-byte g3a header) to `loadAddress` and maps `0x00300000` -> `loadAddress` with 55 UTLB entries of 64 KiB (entries 0..54).
- User RAM uses entries 55..62.
- **NULL is mapped to `0xa0000000`** with a 4 KiB page, user read/write (entry 63; `main.c:251-263`). NULL dereferences silently read flash instead of faulting.
- Code and rodata run at RAM speed.
- gint's linker script limits ROM to 2 MiB (`fxcg50.ld.c:22`) and static RAM to 491 KiB at `0x08101400` (`fxcg50.ld.c:26`).

**Stack** [V]/[G]:
- gint never sets `r15`. It assumes the stack is the last 16 KiB of user RAM (`kernel.c:84-86`) and places a canary there.
- MPM calls the add-in on its own stack and has a "TODO: Move the stack pointer!!" comment (`main.c:354`). The actual stack location on the CG100 is **unknown [G]**.
- Keep recursion and large locals modest, and test the stack depth on hardware.

### 5.3 Adding arenas, and how DOOM should take its zone

```c
#include <gint/kmalloc.h>
/* Take the biggest block of MPM's arena for DOOM's zone (CG100). */
size_t zsize = 0;
void *zone = kmalloc_max(&zsize, "_ld1");     /* then give some back with krealloc(zone, zsize - 256*1024) */

/* Custom arena example (e.g. CG50 RAM above 0x8c200000 after probing it, as
   CGDoom's FindZeroedMemory does in cgdoom/cgdoom.c:686): */
static kmalloc_arena_t extra = { 0 };
extra.name = "xram2mb"; extra.is_default = true;
extra.start = (void *)0x8c200000; extra.end = (void *)(0x8c200000 + probed_size);
kmalloc_init_arena(&extra, true);
kmalloc_add_arena(&extra);
```

- Leave at least about 64 KiB of `_uram` or `_ld1` free for gint and fxlibc: fugue fds, path normalization, FILE structs.
- On the CG50, CGDoom also uses SPU **PRAM0** (`0xfe200000`-`0xfe228000`, 160 KiB, 32-bit accesses only; `src-cg/platform.h:57-60`). Its existence on the CG100 is **unknown [G]**.

---

## 6. On-chip memory and sections

Linker script for fx-CG (`ref/gint/fxcg50.ld.c`, installed as `fxcg50.ld`) [V]:

```
rom   (rx):  o = 0x00300000 (or 0x8c200000 with FXCG50_FASTLOAD), l = 2M
ram   (rw):  o = 0x08101400, l = 491k      /* first 0x1400 bytes: VBR */
ilram (rwx): o = 0xe5200000, l = 4k
xyram (rwx): o = 0xe500e000, l = 16k        /* X 0xe500e000-0xe500ffff, Y 0xe5010000-0xe5011fff */
```

- Output section `.ilram` collects `*(.ilram)` and `*(.gint.mapped)`, and is loaded from ROM (`AT> rom`).
- `.xyram` collects `*(.xram .yram .xyram)`, also `AT> rom` (lines 137-159).
- Attributes (`include/gint/defs/attributes.h`):
  - `#define GILRAM __attribute__((section(".ilram")))`
  - `#define GXRAM __attribute__((section(".xyram")))`
  - `#define GYRAM __attribute__((section(".xyram")))`
  - `GXRAM` and `GYRAM` are the same section.
- fxconv assets can be placed in a section with a `section:` key in `fxconv-metadata.txt` (`ref/fxsdk/fxconv/fxconv-main.py:138-139`).

What gint itself uses [V]:
- ILRAM: `_gint_inth_callback_reloc` (about 150 B, `src/kernel/inth.S:183-299`) and a 32-byte `ILbuf` for `dma_memset` (`src/dma/memset.c:4`). That leaves about **3.8 KiB of ILRAM**.
- XYRAM: nothing; all **16 KiB** are free.

Validity on the CG100 under MPM:
- [V] `gint_load_onchip_sections()` (`start.c:97-111`) runs on all CG models. It does `memset(0xe5200000, 0, 4096)`, then a `regcpy` of `.ilram`, then `memset(0xe500e000, 0, 16384)`, then a `regcpy` of `.xyram`.
- [I] gint's interrupt callback code executes from ILRAM, and gint add-ins do run on the CG100. Both regions therefore exist and work on the CG100.

**Pitfall** [V]:
- The default `GINT_ONCHIP_REINITIALIZE` mode re-runs `gint_load_onchip_sections()` after **every** `gint_world_switch` (`world.c:186-187`). Runtime data in ILRAM or XYRAM is **wiped**: it is reset to its load image, zeros for unset parts.
- Code placed there, such as `GILRAM` hot loops like `R_DrawColumn` or `R_DrawSpan`, and constant tables are fine.
- For mutable state, call `gint_set_onchip_save_mode(GINT_ONCHIP_BACKUP, buf)` with a 20 KiB buffer (`GINT_ONCHIP_BUFSIZE = 20<<10`, gint.h:121-131).
- Never use on-chip memory as a `BFile_Read` destination across a world switch: it is wiped on return.

---

## 7. Overclocking (`include/gint/clock.h`, `src/cpg/overclock.c`, `src/cpg/cpg.c`)

API [V]:
- `int clock_get_speed(void);` (122) returns 1..5, or `CLOCK_SPEED_UNKNOWN` (0) if the registers match no table entry.
- `void clock_set_speed(int speed);` (138), with `CLOCK_SPEED_F1..F5 = 1..5` and `CLOCK_SPEED_DEFAULT = F1`.
- `void cpg_get_overclock_setting(struct cpg_overclock_setting *s)` and `void cpg_set_overclock_setting(struct cpg_overclock_setting const *s)`.
- `void cpg_set_overclock_permanent(bool permanent);` (163). By default the OS clock is restored on every world switch and on exit, because `drv_cpg` saves and restores state. The comment in `clock.h:136-137` saying otherwise is stale.

How the frequencies are computed (`cpg.c:69-105`):
- `base = 32768 * (FLF >> SELXM) * (STC + 1)`, then each clock is `base >> (div + 1)`.
- `FRQCR` fields: STC at bits 29:24, IFC at 23:20, SFC at 15:12, BFC at 11:8, P1FC at 3:0.
- Every level uses `FLLFRQ = 0x4000 + 900`, so the FLL multiplier is 450 and the PLL input is 14.7456 MHz.

`settings_fxcg100[5]` is the CG50 table with indices [F1, F2, F3, F4, F4] (overclock.c:448-512). The source comment for F5 reads: "*made identical to CLOCK_SPEED_F4 because clearly the Graph Math+ cannot handle the higher bus speed*". Computed values [I]:

| Level | FRQCR | Iφ (CPU) | Sφ | Bφ (bus, SDRAM and LCD) | Pφ (TMU/peripherals) | CS0WCR (flash) |
|---|---|---|---|---|---|---|
| F1 (OS default on CG50) | `0x0F011112` | 117.96 MHz | 58.98 | 58.98 | 29.49 | 0x3C0 |
| F2 | PLLx16, /4, /8, /8, /8 | 58.98 | 29.49 | 29.49 | 29.49 | 0x340 |
| F3 | PLLx26, /4, /8, /8, /8 | 95.85 | 47.92 | 47.92 | 47.92 | 0x240 |
| **F4 (CG100 max)** | PLLx32, /2, /4, /8, /16 | **235.93** | 117.96 | 58.98 | 29.49 | 0x2C0 |
| F5 on CG50 only | PLLx25, /2, /4, /4, /8 | 184.32 | 92.16 | **92.16** | 46.08 | 0x440 |
| F5 on CG100 | = F4 | | | | | |

Implications:
- On the CG100, F4 doubles CPU speed but **not** bus or LCD speed: `CS5aWCR` and `CS3WCR` are the same at every level, and Bφ stays at 59 MHz. The frame push therefore stays at about 8 ms.
- On the CG50, F5's 92 MHz bus would also speed up the LCD and SDRAM.
- `clock_set_speed()` waits for DMA (`gint_world_sync`), runs atomically and rescales timers.
- Whether the CG100 OS's own default equals F1 is unverified [G]. If `clock_get_speed()` returns 0 on the CG100, it does not.
- Safety: gint deliberately refuses anything beyond F4 on the CG100. Do not use `cpg_set_overclock_setting` with custom values.

---

## 8. Exiting

[V] (`src/kernel/start.c`, `src/kernel/osmenu.c`, `src/kernel/kernel.c`):
- Returning from `main()` or calling `exit(rc)` leads to `_Exit`, which `longjmp`s to `gint_exitbuf` in `start2()`. Destructors run, then `kquit()` performs `gint_world_switch_out` (driver states, display window, clock, DMA) and frees the world buffers. `start()` then returns to the loader (start.c:195-224).
- `gint_setrestart(int restart)` stores `restart && !GINT_OS_CP && gint[HWCALC] != HWCALC_FXCG100`, so it **never restarts on the CG100** (start.c:43-48).
- `void gint_osmenu(void)` is `gint_world_switch(GINT_CALL(gint_osmenu_native))`. `gint_osmenu_native()` returns immediately on the CG100 (osmenu.c:96-101). On the CG50 it finds `SaveAndOpenMainMenu` by disassembling syscall `0x1e58`, or falls back to a timer plus `GetKeyWait`.
- `gint_poweroff()`, `gint_set_quit_handler()`, and the panic screen's reset key call `PowerOff`, `SetQuitHandler` or `Reset`. All three are missing on the CG100 and panic with `0x10c0` "Missing syscall for this OS version" (`exch.c`, commit `f953efd`).
- On the CG100, MPM (`main.c:486-504`) ignores the return code, disables the MMU, redraws the frame with `CW_DrawFrameC3(0)` and rescans add-ins. It has a "TODO: Wait for all keys to be released!", so a key still held (for example EXE) can relaunch something. **Before returning, wait until the keyboard is idle**, for example until all six `KEYSC` words at `0xa44b0000` read 0 or `keydown_any(...)` returns false after `clearevents()`.
- Before exiting, close all files: fxlibc's `exit()` does not.

---

## 9. Building with the fxSDK

Toolchain file `fxsdk/cmake/FXCG50.cmake` [V]:
- `add_compile_options(-m4-nofpu -mb -fno-builtin -nostdlib -Wa,--dsp)`
- `add_link_options(-nostdlib -Wl,--no-warn-rwx-segments)`, `link_libraries(-lgcc)`
- `add_compile_definitions(TARGET_FXCG50)`; with `CMAKE_BUILD_TYPE=FastLoad`, also `TARGET_FXCG50_FASTLOAD`.
- The default is **`-m4-nofpu`, not `-m4a-nofpu`**.

`Gint::Gint` interface from `cmake/FindGint.cmake` [V]:
- Compile option `-fstrict-volatile-bitfields`, definition `FXCG50`.
- Link libraries `-lopenlibm -lgcc` and `Gint::Fxlibc` (`libc.a`).
- Linker script `-T fxcg50.ld`, or `fxcg50_fastload.ld` for FastLoad.

gint itself is built with `-Wall -Wextra -std=c11 -Os -g -fstrict-volatile-bitfields -mtas -flto` (`gint/CMakeLists.txt:273`). The installed compiler is `sh-elf-gcc 14.1.0` with multilibs `.` and `m4-nofpu`. `-m4a-nofpu` is accepted for compilation but maps to the default `.` multilib if passed at link time [V, tested locally]. If you want SH4A instructions, add it to compile options only [G].

Template (`fxsdk/assets/CMakeLists.txt`), adapted for DOOM:

```cmake
cmake_minimum_required(VERSION 3.15)
project(CGDoom100 C ASM)
include(GenerateG3A)
include(Fxconv)
find_package(Gint 2.11 REQUIRED)            # template asks for 2.9

set(SOURCES src/i_main.c src/i_video_cg.c ... )
set(ASSETS_cg assets-cg/font.png)            # fxconv: see fxconv-metadata.txt
fxconv_declare_assets(${ASSETS_cg} WITH_METADATA)

add_executable(cgdoom ${SOURCES} ${ASSETS_cg})
target_compile_options(cgdoom PRIVATE -Wall -O2 -flto=auto -fno-strict-aliasing)
target_link_options(cgdoom PRIVATE -O2 -flto=auto)
target_link_libraries(cgdoom Gint::Gint)
generate_g3a(TARGET cgdoom OUTPUT "CGDoom.g3a" NAME "CGDoom"
             ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
```

- `generate_g3a(TARGET OUTPUT NAME VERSION ICONS <uns> <sel>)` (`fxsdk/cmake/GenerateG3A.cmake`) runs `objcopy -O binary -R .bss -R .gint_bss` to produce `<target>.bin`, then `fxgxa --g3a -n NAME --icon-uns= --icon-sel= <bin> -o <out>`. In the FastLoad configuration it only produces the `.bin`.
- Icons are 92x64 PNGs. MPM reads them from g3a offsets `0x1000` (unselected) and `0x4000` (selected).
- fxconv metadata looks like `example.png:\n  type: bopti-image\n  name: img_example`. Other types: `type: binary` (raw bytes in `.rodata`; `fxconv.py:455-456, 1192`), `font`, and so on.
- The template's `-Os` should be replaced by `-O2` or `-O3` for hot DOOM code. `-fno-builtin` comes from the toolchain, so `memcpy`/`memset` go to fxlibc's assembly versions (`fxlibc/src/string/target/sh-generic/mem{cpy,set}.S`); use `__builtin_memcpy` for small fixed-size copies.

**Is there a CG100-specific target or g3a variant?** [V] **No.**
- fxsdk dev has `build-fx`, `build-cg`, `build-cg-push` (FastLoad, linked at `0x8c200000` for the CG50 "Add-In Push" app), `build-fxg3a` and `build-cp` (fx-CP400 `.hh3`).
- A search for `cg100`, `mathplus`, `fxcg100` and `mpm` in fxsdk finds nothing; `hh3` refers only to the fx-CP.
- On the CG100, MPM loads any non-folder `\\fls0\*.g3a` of at least 0x7002 bytes (`ref/mpm/main.c:31-68`). It skips the 0x7000-byte header and calls `0x00300000` with `(0x4d504d30, loadinfo)`.
- MPM also has a USB push mode: press **SETTINGS** in the MPM menu, then run `fxlink -pw <file>.bin` (same protocol as `fxlink/modes/push.c`; MPM `main.c:473-484`). MPM maps the pushed image at `0x00300000`, so push the **normal `build-cg/<target>.bin`**, not the `build-cg-push` output linked at `0x8c200000` [I].

---

## 10. Open questions to test on hardware

1. The CG100 display controller version: print `r61524_get_version()`, declared `extern uint16_t r61524_get_version(void);`. Is it 50 or 82 (new), or something else?
2. Measured cost of Option A versus Option B (§1.5) at F1 and F4, using `prof_exec`.
3. The return value of `BFile_Block`, the region of the returned pointer (P1/P2, flash or RAM), and how long pointers stay valid. Is the CG50 `0x1DAA` reachable through gint's world switch?
4. Does DMA from XYRAM (`0xe5007000` alias versus `0xe500e000`) work for stripe streaming?
5. Is the CG100 OS default clock equal to gint's F1? Does `clock_get_speed()` return 1?
6. Where does the stack actually live under MPM, and how much is safe to use?
7. Are BFile calls stable with the MMU on, as gint calls them? MPM turns the MMU off around every OS call. If they crash, wrap calls with `MMUCR.AT=0`. In that case only pass P1 pointers to the OS: `malloc` results are P1, but string literals and `.bss` are P0.
