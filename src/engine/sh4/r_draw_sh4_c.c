/* r_draw_sh4_c.c - C side of the SH4 drawers: rotated colormap registration, the C fallback paths
   the assembly tail-calls when its preconditions do not hold, and the palette table builder.

   The fallbacks are plain transcriptions of vanilla r_draw.c (CGDoom variant) and read the same
   engine globals, in the build's screen format (src/platform/cgd_config.h). They run when:
     - the colormap is not a 256-entry map of the registered light tables (or nothing registered),
     - the count is negative (nothing to draw) or larger than the repeat counter allows,
     - (low detail columns) columnofs[x+1] != columnofs[x] + 1.
   Vanilla code paths never trigger them except for count < 0, so their speed does not matter.

   The engine globals are declared here with plain C types that are compatible with the engine's
   (byte == unsigned char, fixed_t == int, pixel_t == lighttable_t == cgd_sh4_cm_t: a palette
   index, or RGB565 with CGD_TRUECOLOR). */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "cgd_sh4.h"

#define FUZZTABLE   50

typedef cgd_sh4_cm_t pix_t;

/* ---- engine globals (defined in r_draw.c / r_main.c / r_data.c) ---- */
extern const pix_t *dc_colormap;
extern int dc_x, dc_yl, dc_yh;
extern int dc_iscale, dc_texturemid;
extern const uint8_t *dc_source;
extern uint8_t *dc_translation;
extern int ds_y, ds_x1, ds_x2;
extern const pix_t *ds_colormap;
extern int ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern const uint8_t *ds_source;
extern pix_t *ylookup[];
extern int columnofs[];
extern int centery;
extern int viewheight;
extern const pix_t *colormaps;
extern int fuzzpos;

/* The spectre's darkening of the pixel behind it, as r_draw.c FUZZDARK: in RGB565, each channel
   scaled by 13/16 with rounding, on the pixel spread over 32 bits */
#if CGD_TRUECOLOR
static inline pix_t FuzzDark(pix_t p)
{
	uint32_t s = ((uint32_t)p | ((uint32_t)p << 16)) & 0x07e0f81f;
	s = ((s * 13 + 0x01004008) >> 4) & 0x07e0f81f;
	return (pix_t)(s | (s >> 16));
}
#define FUZZDARK(p) FuzzDark(p)
#else
#define FUZZDARK(p) (colormaps[6 * 256 + (p)])
#endif

struct cgd_sh4_cmaps cgd_sh4_cmaps = { 0, 0, 0, 0 };

static void rotate_maps(pix_t *rot, const pix_t *cm, int nmaps)
{
	for(int k = 0; k < nmaps; k++, rot += 256, cm += 256) {
		memcpy(rot, cm + 128, 128 * sizeof(pix_t));
		memcpy(rot + 128, cm, 128 * sizeof(pix_t));
	}
}

void cgd_sh4_set_colormaps(const pix_t *cm, int nmaps, pix_t *rot)
{
	/* Disable first: the drawers fall back to C while the table is incomplete */
	cgd_sh4_cmaps.nmaps = 0;
	cgd_sh4_cmaps.rot = 0;
	if(!cm || !rot || nmaps <= 0)
		return;
	if(nmaps > CGD_SH4_MAXMAPS)
		nmaps = CGD_SH4_MAXMAPS;
	rotate_maps(rot, cm, nmaps);
	cgd_sh4_cmaps.src = cm;
	cgd_sh4_cmaps.delta = (intptr_t)(rot + 128) - (intptr_t)cm;
	cgd_sh4_cmaps.rot = rot;
	cgd_sh4_cmaps.nmaps = (uint32_t)nmaps;
}

void cgd_sh4_colormaps_changed(void)
{
	if(cgd_sh4_cmaps.nmaps)
		rotate_maps(cgd_sh4_cmaps.rot, cgd_sh4_cmaps.src, (int)cgd_sh4_cmaps.nmaps);
}

/* Same values as the static fuzzoffset[] of r_draw.c (FUZZOFF = SCREENWIDTH) */
#define FO SCREENWIDTH
static const int16_t cgd_sh4_fuzzoffset[FUZZTABLE] = {
	FO,-FO,FO,-FO,FO,FO,-FO,FO,FO,-FO,FO,FO,FO,-FO,FO,FO,FO,-FO,-FO,-FO,-FO,FO,-FO,-FO,FO,
	FO,FO,FO,-FO,FO,-FO,FO,FO,-FO,-FO,FO,FO,-FO,-FO,-FO,-FO,FO,FO,FO,FO,-FO,FO,FO,-FO,FO,
};

/* ---- C fallbacks (same semantics as the assembly) ---- */

void cgd_sh4_slow_column(void)
{
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	pix_t *dest = ylookup[dc_yl] + columnofs[dc_x];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	const pix_t *cm = dc_colormap;
	const uint8_t *src = dc_source;
	do {
		*dest = cm[src[(frac >> 16) & 127]];
		dest += SCREENWIDTH;
		frac = (int)((unsigned)frac + (unsigned)fracstep);
	} while(count--);
}

void cgd_sh4_slow_column_low(void)
{
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	int x = dc_x << 1;
	pix_t *dest = ylookup[dc_yl] + columnofs[x];
	pix_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	const pix_t *cm = dc_colormap;
	const uint8_t *src = dc_source;
	do {
		*dest2 = *dest = cm[src[(frac >> 16) & 127]];
		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;
		frac = (int)((unsigned)frac + (unsigned)fracstep);
	} while(count--);
}

void cgd_sh4_slow_fuzz(void)
{
	if(!dc_yl) dc_yl = 1;
	if(dc_yh == viewheight - 1) dc_yh = viewheight - 2;
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	pix_t *dest = ylookup[dc_yl] + columnofs[dc_x];
	do {
		*dest = FUZZDARK(dest[cgd_sh4_fuzzoffset[fuzzpos]]);
		if(++fuzzpos == FUZZTABLE) fuzzpos = 0;
		dest += SCREENWIDTH;
	} while(count--);
}

void cgd_sh4_slow_fuzz_low(void)
{
	if(!dc_yl) dc_yl = 1;
	if(dc_yh == viewheight - 1) dc_yh = viewheight - 2;
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	int x = dc_x << 1;
	pix_t *dest = ylookup[dc_yl] + columnofs[x];
	pix_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
	do {
		*dest = FUZZDARK(dest[cgd_sh4_fuzzoffset[fuzzpos]]);
		*dest2 = FUZZDARK(dest2[cgd_sh4_fuzzoffset[fuzzpos]]);
		if(++fuzzpos == FUZZTABLE) fuzzpos = 0;
		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;
	} while(count--);
}

void cgd_sh4_slow_translated(void)
{
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	pix_t *dest = ylookup[dc_yl] + columnofs[dc_x];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	do {
		*dest = dc_colormap[dc_translation[dc_source[frac >> 16]]];
		dest += SCREENWIDTH;
		frac = (int)((unsigned)frac + (unsigned)fracstep);
	} while(count--);
}

void cgd_sh4_slow_translated_low(void)
{
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	int x = dc_x << 1;
	pix_t *dest = ylookup[dc_yl] + columnofs[x];
	pix_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	do {
		*dest2 = *dest = dc_colormap[dc_translation[dc_source[frac >> 16]]];
		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;
		frac = (int)((unsigned)frac + (unsigned)fracstep);
	} while(count--);
}

void cgd_sh4_slow_span(void)
{
	int xfrac = ds_xfrac, yfrac = ds_yfrac;
	pix_t *dest = ylookup[ds_y] + columnofs[ds_x1];
	int count = ds_x2 - ds_x1;
	if(count < 0) return;   /* vanilla would run ~2^32 iterations */
	do {
		int spot = ((yfrac >> (16 - 6)) & (63 * 64)) + ((xfrac >> 16) & 63);
		*dest++ = ds_colormap[ds_source[spot]];
		xfrac = (int)((unsigned)xfrac + (unsigned)ds_xstep);
		yfrac = (int)((unsigned)yfrac + (unsigned)ds_ystep);
	} while(count--);
}

void cgd_sh4_slow_span_low(void)
{
	int xfrac = ds_xfrac, yfrac = ds_yfrac;
	int count = ds_x2 - ds_x1;
	ds_x1 <<= 1;
	ds_x2 <<= 1;
	if(count < 0) return;
	pix_t *dest = ylookup[ds_y] + columnofs[ds_x1];
	do {
		int spot = ((yfrac >> (16 - 6)) & (63 * 64)) + ((xfrac >> 16) & 63);
		pix_t c = ds_colormap[ds_source[spot]];
		*dest++ = c;
		*dest++ = c;
		xfrac = (int)((unsigned)xfrac + (unsigned)ds_xstep);
		yfrac = (int)((unsigned)yfrac + (unsigned)ds_ystep);
	} while(count--);
}

/* ---- palette table for cgd_expand_p8_rgb565_p32 ---- */
void cgd_expand_build_pal32(uint32_t *pal32, uint16_t const *pal)
{
	for(int k = 0; k < 256; k++)
		pal32[k] = (uint32_t)pal[k ^ 0x80] * 0x00010001u;
}
