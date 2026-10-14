/* cgd_sh4.h - SH4 (SH4AL-DSP, fx-CG100) assembly renderer routines for CGDoom.

   See README.md in this directory for the full contract, register usage, timings and the exact
   integration steps. Summary:

   - Drawers: drop-in replacements for the vanilla r_draw.c drawers (void(void), reading the same
     globals: dc_* / ds_* / ylookup[] / columnofs[] / centery / viewheight / colormaps / fuzzpos).
     They are bit-exact with the C versions (see test/ref_draw.c for the reference semantics).
   - The fast paths need a "rotated" copy of the COLORMAP lump, registered with
     cgd_sh4_set_colormaps(). When dc_colormap/ds_colormap does not point at a 256-byte map of the
     registered lump (or nothing is registered), the drawers transparently fall back to C code.
   - The column/span drawers use SH4AL-DSP zero-overhead repeat loops (ldrs/ldre/ldrc). They must
     only be called from foreground code, and NO code running in interrupt context may execute
     ldrs/ldre while they can run (gint does not save RS/RE on interrupts). Known offenders:
     gint's USB FIFO read path (usb_pipe_read4, reached from the BRDY interrupt when an
     asynchronous USB read is pending), fxlibc memchr() (and str* built on it), gint/Azur image
     rendering. The palette expansion routines below never use repeat loops.
   - Section: hot code goes to ".ilram" (gint copies it to the on-chip IL memory at start-up and
     after world switches) unless CGD_SH4_NO_ILRAM is defined when assembling.

   This header only uses <stdint.h>/<stddef.h>; it can be included from engine and platform code. */
#ifndef CGD_SH4_H
#define CGD_SH4_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Drawers (r_draw_sh4.S) ------------------------------------------------------------------ */

/* Same semantics as vanilla/CGDoom r_draw.c: */
void R_DrawColumn_SH4(void);            /* R_DrawColumn */
void R_DrawFuzzColumn_SH4(void);        /* R_DrawFuzzColumn (updates dc_yl, dc_yh, fuzzpos like C) */
void R_DrawTranslatedColumn_SH4(void);  /* R_DrawTranslatedColumn (no &127 on the texel index) */
void R_DrawSpan_SH4(void);              /* R_DrawSpan (requires ds_x2 >= ds_x1, as the C loop does) */
/* Low detail (detailshift = 1), Chocolate Doom / DOS semantics (see README "Low detail"):
   x = dc_x << 1 (dc_x is NOT modified), pixels at columnofs[x] and columnofs[x+1];
   span: count = ds_x2 - ds_x1 before doubling, ds_x1 <<= 1 and ds_x2 <<= 1 are applied to the
   globals (as both vanilla and Chocolate do), 2*(count+1) pixels from columnofs[ds_x1]. */
void R_DrawColumnLow_SH4(void);
void R_DrawSpanLow_SH4(void);
void R_DrawFuzzColumnLow_SH4(void);
void R_DrawTranslatedColumnLow_SH4(void);

/* ---- Rotated colormaps (r_draw_sh4_c.c) ---------------------------------------------------------
   rot must hold nmaps*256 bytes (any alignment; 32-byte alignment recommended). Builds
   rot[k*256 + j] = colormaps[k*256 + (j ^ 0x80)] so the drawers can index a colormap with the
   sign-extended texel byte (SH mov.b sign-extends). Call it after R_InitColormaps (and again
   whenever the colormap lump moves or its contents change; vanilla never changes it). nmaps is
   the number of 256-byte maps in the lump (W_LumpLength(COLORMAP) / 256 = 34 for DOOM), max 256.
   colormaps == NULL or nmaps <= 0 unregisters (all drawers then use their C fallbacks, except
   1-pixel columns of R_DrawColumn_SH4 / R_DrawColumnLow_SH4, which read dc_colormap directly). */
void cgd_sh4_set_colormaps(const uint8_t *colormaps, int nmaps, uint8_t *rot);

/* State read by the assembly (do not modify directly). */
struct cgd_sh4_cmaps {
	const uint8_t *src;   /* registered COLORMAP lump */
	uint32_t nmaps;       /* number of maps covered (0 = disabled) */
	intptr_t delta;       /* (rot + 128) - src: rotated base = colormap pointer + delta */
};
extern struct cgd_sh4_cmaps cgd_sh4_cmaps;

/* ---- Palette expansion for the presenter (expand_sh4.S) ------------------------------------------
   dst[i] = pal[src[i]] for 0 <= i < npixels. Contract (src/gint/cgd_gint.h): src 4-byte aligned,
   npixels % 16 == 0, dst 32-byte aligned and either cached P1 SDRAM (0x80000000-0x9fffffff) or
   any other address, e.g. on-chip X/Y memory (supported; the presenter does not use it). For P1 dst each 32-byte line is allocated with movca.l
   (no line fill) and purged with ocbp after being written (data is in RAM when the function
   returns: the caller needs only a synco before starting a DMA, as src/gint/video.c
   present_dma() does). Other dst: plain stores only.
   No repeat loops, no DSP registers, no MACH/MACL: safe in interrupt context.
   For npixels >= 256 it builds the pal32 table below on its stack (1052 bytes of stack at the
   peak, ~870 modelled cycles per call); smaller counts use a byte loop (8-10 cycles/pixel). */
void cgd_expand_p8_rgb565(uint16_t *dst, uint8_t const *src, uint16_t const *pal, int npixels);

/* Same loop without the per-call table build (modelled 2.81 cycles/pixel against ~2.83 for the
   plain entry at 64000 pixels, the presenter's one call per frame, and ~3.09 at 3200), using a
   1 KiB table built from the palette by cgd_expand_build_pal32() (rebuild it whenever pal
   changes, e.g. in PL_SetPalette).
   pal32 must be 4-byte aligned; layout: pal32[k] = pal[k^0x80] * 0x00010001. */
void cgd_expand_build_pal32(uint32_t *pal32, uint16_t const *pal);
void cgd_expand_p8_rgb565_p32(uint16_t *dst, uint8_t const *src, uint32_t const *pal32, int npixels);

/* ---- Cache-line fill (expand_sh4.S) ---------------------------------------------------------------
   Fills size bytes at dst with the 32-bit pattern value. dst must be 32-byte aligned and size a
   multiple of 32. P1 (cached SDRAM) destinations use movca.l (no line fills); any other address
   uses plain stores. No repeat loops. */
void cgd_fill32(void *dst, uint32_t value, size_t size);

/* ---- Inline helpers --------------------------------------------------------------------------- */

#if defined(__sh__) || defined(__SH4__)
/* Prefetch the cache line containing p (no fault on any address). */
static inline void cgd_pref(const void *p)
{
	__asm__ volatile("pref @%0" :: "r"(p));
}

/* Unsigned 64/32 -> 32 division ((hi:lo) / d), exact (truncating). Requires hi < d (else the
   quotient does not fit in 32 bits and the result is garbage). 66 EX-group instructions. */
/* (The 32 steps are spelled out instead of .rept: GCC sizes inline asm by counting its lines,
   and an underestimate makes it place constant pools out of range - "pcrel too far".) */
#define CGD_DIV1_STEP "rotcl %1\n\tdiv1 %2, %0\n\t"
#define CGD_DIV1_STEP4 CGD_DIV1_STEP CGD_DIV1_STEP CGD_DIV1_STEP CGD_DIV1_STEP
static inline uint32_t cgd_udiv64_32(uint32_t hi, uint32_t lo, uint32_t d)
{
	__asm__("div0u\n\t"
		CGD_DIV1_STEP4 CGD_DIV1_STEP4 CGD_DIV1_STEP4 CGD_DIV1_STEP4
		CGD_DIV1_STEP4 CGD_DIV1_STEP4 CGD_DIV1_STEP4 CGD_DIV1_STEP4
		"rotcl %1"
		: "+r"(hi), "+r"(lo) : "r"(d) : "t");
	return lo;
}
#else
static inline void cgd_pref(const void *p) { (void)p; }
static inline uint32_t cgd_udiv64_32(uint32_t hi, uint32_t lo, uint32_t d)
{
	return (uint32_t)((((uint64_t)hi << 32) | lo) / d);
}
#endif

/* Vanilla-exact FixedDiv: vanilla's guard ((abs(a) >> 14) >= abs(b), signed int compare, so
   b == INT_MIN always saturates) then ((int64)a << 16) / b truncated toward zero, which equals
   vanilla's FixedDiv2 double computation. Only difference: a == INT_MIN with |b| < 65536 or
   b == -65536, where vanilla calls I_Error ("FixedDiv: divide by zero"); this returns INT_MIN /
   INT_MAX by the sign of the true quotient (b == 65536 returns INT_MIN, the exact result).
   The second guard only fires for a == INT_MIN (for any other a the first guard already gives
   (|a| >> 15) < |b|) and keeps the unsigned quotient below 2^31. */
static inline int32_t cgd_fixed_div(int32_t a, int32_t b)
{
	int32_t aa = a < 0 ? (int32_t)(0u - (uint32_t)a) : a;
	int32_t ab = b < 0 ? (int32_t)(0u - (uint32_t)b) : b;
	if((aa >> 14) >= ab)
		return ((a ^ b) < 0) ? INT32_MIN : INT32_MAX;
	uint32_t ua = (uint32_t)aa, ub = (uint32_t)ab;
	if((ua >> 15) >= ub)   /* only a == INT_MIN, |b| <= 65536: quotient >= 2^31 */
		return ((a ^ b) < 0) ? INT32_MIN : INT32_MAX;
	uint32_t q = cgd_udiv64_32(ua >> 16, ua << 16, ub);
	return ((a ^ b) < 0) ? (int32_t)(0u - q) : (int32_t)q;
}

#ifdef __cplusplus
}
#endif
#endif /* CGD_SH4_H */
