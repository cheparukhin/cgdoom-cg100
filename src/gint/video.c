/* CGDoom gint platform: video (PL_SetPalette / PL_FinishUpdate /
   PL_VideoReset).

   The 320x200 8-bit frame is centered on the 396x224 panel (window x 38-357,
   y 12-211, borders black). The window is always set with gint's
   r61524_start_frame(), which supports both the classic R61524 and the new
   2025 controllers; r61524_win_set() is never used because its register
   numbers are commands on the new controller.

   Display paths (option, default GDISP_DMA):
   - GDISP_DMA: expand into a 32-byte aligned RGB565 frame in cached SDRAM
     (cgd_expand_p8_rgb565_p32, replaceable; each line is allocated with
     movca.l and purged with ocbp, so only a synco is needed before the
     DMA), r61524_start_frame(), then dma_transfer_async(channel 0,
     DMA_32B, 4000 blocks, -> 0xb4000000 fixed) and return immediately: the
     ~8 ms bus-bound transfer overlaps the next frame. The next call waits
     for it first. Channel 0 is gint's display channel, so gint's dupdate()
     also waits for it.
   - GDISP_STRIPS: gpSP's hardware-proven presenter (gint_platform.c):
     10-row strips expanded alternately into the on-chip X bank (0xe5007000)
     and Y bank (0xe5017000), each sent by DMA while the next one is being
     expanded; the last DMA is left in flight. No SDRAM traffic for the
     DMA source. X and Y are reserved for this (nothing in GXRAM/GYRAM).
     Each strip's stores are drained with synco before its DMA starts.
   - GDISP_CPU: CPU writes of palette lookups to the LCD data register
     (CGDoom's approach), ~8 ms of CPU per frame.
   All three read the palette from one persistent 1 KiB pal32 table
   (cgd_expand_build_pal32 layout), rebuilt by PL_SetPalette(). The
   expansion runs in the foreground; the _p32 routine (asm and weak C) has
   no DSP repeat loops and uses no DSP/MAC registers, so it would also be
   safe in interrupt context.

   After any world switch the OS may have drawn its busy indicator into
   GRAM: gvid_note_os_activity() makes the next frame clear the borders. */

#include "cgd_gint.h"

#include <gint/dma.h>
#include <gint/drivers/r61524.h>
#include <gint/defs/util.h>
#include <stdlib.h>
#include <string.h>

#define LCD_DATA      ((volatile uint16_t *)0xb4000000)
#define LCD_W         396
#define LCD_H         224
#define FB_W          320
#define FB_H          200
#define FB_X0         ((LCD_W - FB_W) / 2)      /* 38 */
#define FB_Y0         ((LCD_H - FB_H) / 2)      /* 12 */
#define FB_PIXELS     (FB_W * FB_H)
#define STRIP_ROWS    10
#define STRIP_PIXELS  (FB_W * STRIP_ROWS)       /* 6400 bytes */
#define STRIP_COUNT   (FB_H / STRIP_ROWS)

_Static_assert(STRIP_PIXELS * 2 <= 8192, "strip must fit an 8 KiB bank");
_Static_assert(STRIP_PIXELS % 16 == 0, "expansion works on 16 pixels");
_Static_assert(FB_H % STRIP_ROWS == 0, "whole number of strips");

/* Palette as the expansion's pal32 table (cgd_expand_build_pal32 layout:
   g_pal32[k] = pal[k ^ 0x80] * 0x00010001), rebuilt by PL_SetPalette();
   1 KiB, replaces the 512-byte RGB565 copy (net +512 bytes of .bss) */
static uint32_t g_pal32[256] __attribute__((aligned(32)));
static void *g_fb16_alloc = NULL;
static uint16_t *g_fb16 = NULL;
static int g_mode = GDISP_DMA;
static bool g_window_narrow = false;
static bool g_borders_dirty = true;

/* RGB565 of palette index b */
#define PAL565(t, b)  ((uint16_t)(t)[(b) ^ 0x80])

//---
// Default 8-bit -> RGB565 expansion (weak; see cgd_gint.h for the contract).
// The calculator build links the strong SH4 versions (src/engine/sh4:
// expand_sh4.S, r_draw_sh4_c.c); these are the fallback and the host-tested
// reference.
//---

__attribute__((weak))
void cgd_expand_build_pal32(uint32_t *pal32, uint16_t const *pal)
{
	for(int k = 0; k < 256; k++)
		pal32[k] = (uint32_t)pal[k ^ 0x80] * 0x00010001u;
}

/* Two pixels (big-endian: the left one in the high half) */
#define PAIR(t, x, y) \
	(((t)[(x) ^ 0x80] & 0xffff0000u) | ((t)[(y) ^ 0x80] & 0x0000ffffu))

#define EXPAND_LINE(s, t, w)                                          \
	do {                                                          \
		uint32_t a_ = (s)[0], b_ = (s)[1], c_ = (s)[2], d_ = (s)[3]; \
		w[0] = PAIR(t, a_ >> 24, (a_ >> 16) & 0xff);          \
		w[1] = PAIR(t, (a_ >> 8) & 0xff, a_ & 0xff);          \
		w[2] = PAIR(t, b_ >> 24, (b_ >> 16) & 0xff);          \
		w[3] = PAIR(t, (b_ >> 8) & 0xff, b_ & 0xff);          \
		w[4] = PAIR(t, c_ >> 24, (c_ >> 16) & 0xff);          \
		w[5] = PAIR(t, (c_ >> 8) & 0xff, c_ & 0xff);          \
		w[6] = PAIR(t, d_ >> 24, (d_ >> 16) & 0xff);          \
		w[7] = PAIR(t, (d_ >> 8) & 0xff, d_ & 0xff);          \
	} while(0)

__attribute__((weak))
void cgd_expand_p8_rgb565_p32(uint16_t *dst, uint8_t const *src,
	uint32_t const *pal32, int npixels)
{
	uint32_t const *s = (uint32_t const *)src;
	uint32_t *d = (uint32_t *)dst;
	uint32_t w[8];

	/* As the asm: nothing written unless a positive multiple of 16 */
	if(npixels <= 0 || npixels % 16)
		return;

#ifdef __sh__
	if(((uintptr_t)dst >> 29) == 4) {
		/* Cached P1 SDRAM: allocate each 32-byte line without fetching it
		   (movca.l), fill it, then purge it (ocbp: write back +
		   invalidate) so the 128 KB frame does not evict the renderer's
		   working set. Every line is in RAM on return (only a synco is
		   left to the caller before a DMA reads it). */
		for(int i = 0; i < npixels; i += 16, s += 4, d += 8) {
			EXPAND_LINE(s, pal32, w);
			__asm__ volatile("movca.l %1, @%0"
				:: "r"(d), "z"(w[0]) : "memory");
			d[1] = w[1]; d[2] = w[2]; d[3] = w[3];
			d[4] = w[4]; d[5] = w[5]; d[6] = w[6]; d[7] = w[7];
			__asm__ volatile("ocbp @%0" :: "r"(d) : "memory");
		}
	}
	else
#endif
	{
		/* On-chip memory (or anything else): plain stores */
		for(int i = 0; i < npixels; i += 16, s += 4, d += 8) {
			EXPAND_LINE(s, pal32, w);
			d[0] = w[0]; d[1] = w[1]; d[2] = w[2]; d[3] = w[3];
			d[4] = w[4]; d[5] = w[5]; d[6] = w[6]; d[7] = w[7];
		}
	}
}

//---
// Helpers
//---

static void lcd_fill(int x0, int x1, int y0, int y1, uint16_t color)
{
	r61524_start_frame(x0, x1, y0, y1);
	int n = (x1 - x0 + 1) * (y1 - y0 + 1);
	while(n-- > 0)
		*LCD_DATA = color;
	g_window_narrow = true;
}

static void clear_borders(void)
{
	lcd_fill(0, LCD_W - 1, 0, FB_Y0 - 1, 0);
	lcd_fill(0, LCD_W - 1, FB_Y0 + FB_H, LCD_H - 1, 0);
	lcd_fill(0, FB_X0 - 1, FB_Y0, FB_Y0 + FB_H - 1, 0);
	lcd_fill(FB_X0 + FB_W, LCD_W - 1, FB_Y0, FB_Y0 + FB_H - 1, 0);
	g_borders_dirty = false;
}

static void cpu_push16(uint16_t const *buf, int n)
{
	for(int i = 0; i < n; i++)
		*LCD_DATA = buf[i];
}

static void start_window(void)
{
	r61524_start_frame(FB_X0, FB_X0 + FB_W - 1, FB_Y0, FB_Y0 + FB_H - 1);
	g_window_narrow = true;
}

//---
// Presentation paths
//---

static void present_cpu(uint8_t const *fb)
{
	dma_transfer_wait(0);
	if(g_borders_dirty)
		clear_borders();
	start_window();

	/* No expansion buffer here: each pixel is looked up in the same pal32
	   table and written to the LCD data register */
	uint32_t const *pal32 = g_pal32;
	if(((uintptr_t)fb & 3) == 0) {
		uint32_t const *s = (uint32_t const *)fb;
		for(int i = 0; i < FB_PIXELS / 4; i++) {
			uint32_t q = s[i];
			*LCD_DATA = PAL565(pal32, q >> 24);
			*LCD_DATA = PAL565(pal32, (q >> 16) & 0xff);
			*LCD_DATA = PAL565(pal32, (q >> 8) & 0xff);
			*LCD_DATA = PAL565(pal32, q & 0xff);
		}
	}
	else {
		for(int i = 0; i < FB_PIXELS; i++)
			*LCD_DATA = PAL565(pal32, fb[i]);
	}
}

static void present_dma(uint8_t const *fb)
{
	/* Previous frame done? (normally long done: rendering takes longer) */
	dma_transfer_wait(0);
	if(g_borders_dirty)
		clear_borders();

	/* g_fb16 is P1 (gvid_init): the expansion allocates every line with
	   movca.l and purges it with ocbp, so the frame is already in SDRAM
	   (the per-line ocbwb loop that followed was redundant: integration
	   step "gint video"). Only drain the stores before the DMA starts. */
	cgd_expand_p8_rgb565_p32(g_fb16, fb, g_pal32, FB_PIXELS);
	synco();

	start_window();
	if(!dma_transfer_async(0, DMA_32B, FB_PIXELS * 2 / 32, g_fb16, DMA_INC,
		(void *)LCD_DATA, DMA_FIXED, GINT_CALL_NULL))
		cpu_push16(g_fb16, FB_PIXELS);
}

static void present_strips(uint8_t const *fb)
{
	uint16_t *bank[2] = { GVID_XBANK, GVID_YBANK };

	dma_transfer_wait(0);
	if(g_borders_dirty)
		clear_borders();
	start_window();

	/* One window for the whole frame; consecutive DMA bursts keep
	   streaming into GRAM. Expand strip s into one bank while the other
	   bank's DMA (strip s-1) is running. */
	for(int s = 0; s < STRIP_COUNT; s++) {
		uint16_t *buf = bank[s & 1];
		cgd_expand_p8_rgb565_p32(buf, fb + s * STRIP_PIXELS, g_pal32,
			STRIP_PIXELS);
		/* Drain the CPU's stores into X/Y RAM before the DMA reads the
		   bank: nothing else on this path executes synco (dma_setup()
		   only writes DMAC registers; gpSP always went through
		   r61524_start_frame()'s synco here). gint review round 1. */
		synco();
		if(s > 0)
			dma_transfer_wait(0);
		if(!dma_transfer_async(0, DMA_32B, STRIP_PIXELS * 2 / 32, buf,
			DMA_INC, (void *)LCD_DATA, DMA_FIXED, GINT_CALL_NULL))
			cpu_push16(buf, STRIP_PIXELS);
	}
	/* The last strip's DMA stays in flight */
}

//---
// Platform-internal API
//---

bool gvid_init(void)
{
	if(g_fb16)
		return true;
	g_fb16_alloc = malloc(FB_PIXELS * 2 + 32);
	if(!g_fb16_alloc)
		return false;
	uintptr_t a = ((uintptr_t)g_fb16_alloc + 31) & ~(uintptr_t)31;
	if(!gfs_is_p1((void *)a, FB_PIXELS * 2)) {
		/* DMA needs a P1/P2 source (dma_translate) and movca.l needs a
		   cacheable one: fall back to the CPU path */
		free(g_fb16_alloc);
		g_fb16_alloc = NULL;
		return false;
	}
	g_fb16 = (uint16_t *)a;
	return true;
}

void gvid_set_mode(int mode)
{
	dma_transfer_wait(0);
	if(mode < 0 || mode >= GDISP_COUNT)
		mode = GDISP_DMA;
	g_mode = mode;
}

bool gvid_dma_available(void)
{
	return g_fb16 != NULL;
}

char const *gvid_mode_name(int mode)
{
	switch(mode) {
	/* Without the frame buffer PL_FinishUpdate() quietly uses the CPU
	   path: say so wherever the mode is shown (gint review round 2) */
	case GDISP_DMA:    return g_fb16 ? "DMA (async)" : "DMA n/a: CPU";
	case GDISP_STRIPS: return "DMA strips (X/Y RAM)";
	case GDISP_CPU:    return "CPU direct";
	default:           return "?";
	}
}

void gvid_release(void)
{
	dma_transfer_wait(0);
	if(g_window_narrow) {
		r61524_start_frame(0, LCD_W - 1, 0, LCD_H - 1);
		g_window_narrow = false;
	}
}

void gvid_note_os_activity(void)
{
	g_borders_dirty = true;
}

void gvid_clear_lcd(void)
{
	dma_transfer_wait(0);
	lcd_fill(0, LCD_W - 1, 0, LCD_H - 1, 0);
	/* lcd_fill() left the full-screen window */
	g_window_narrow = false;
	g_borders_dirty = false;
}

void gvid_push_test(int mode, uint8_t const *fb)
{
	int saved = g_mode;
	g_mode = mode;
	PL_FinishUpdate(fb);
	dma_transfer_wait(0);
	g_mode = saved;
}

//---
// PL_* API
//---

void PL_SetPalette(const uint16_t pal565[256])
{
	/* Persistent table (integration step "gint video"): built here once
	   per palette change (~256 iterations) instead of on the expansion's
	   stack at every call of the plain entry (1 KiB of stack, ~870
	   cycles, 20 times a frame in strip mode). Never in a DMA's way: the
	   DMA reads g_fb16 or the strip banks, not the table. */
	if(pal565)
		cgd_expand_build_pal32(g_pal32, pal565);
}

void PL_FinishUpdate(const uint8_t *fb)
{
	if(!fb)
		return;

	switch(g_mode) {
	case GDISP_STRIPS:
		if(((uintptr_t)fb & 3) == 0) {
			present_strips(fb);
			return;
		}
		break;
	case GDISP_DMA:
		if(g_fb16 && ((uintptr_t)fb & 3) == 0) {
			present_dma(fb);
			return;
		}
		break;
	default:
		break;
	}
	present_cpu(fb);
}

/* Called by the engine after errors and file I/O (I_ReinitAfterError:
   every save/load). Blanking the whole LCD here showed a black frame on
   each save/load (gint review round 1); what the world switch may have
   damaged is already handled: the borders are cleared by the next frame
   (dirty flag, also set by gfs_os_call()) and every present re-sets the
   window. The next PL_FinishUpdate repaints the game area. */
void PL_VideoReset(void)
{
	gvid_release();
	g_borders_dirty = true;
}
