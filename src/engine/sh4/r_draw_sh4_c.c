/* r_draw_sh4_c.c - C side of the SH4 drawers: rotated colormap registration, the C fallback paths
   the assembly tail-calls when its preconditions do not hold, and the palette table builder.

   The fallbacks are plain transcriptions of vanilla r_draw.c (CGDoom variant) and read the same
   engine globals, at the build's SCREENWIDTH (src/platform/cgd_config.h). They run when:
     - the colormap is not a 256-byte map of the registered COLORMAP lump (or nothing registered),
     - the count is negative (nothing to draw) or larger than the repeat counter allows,
     - (low detail columns) columnofs[x+1] != columnofs[x] + 1.
   Vanilla code paths never trigger them except for count < 0, so their speed does not matter.

   The engine globals are declared here with plain C types that are compatible with the engine's
   (lighttable_t == byte == unsigned char, fixed_t == int). */

#include <stdint.h>
#include <stddef.h>
#include "cgd_sh4.h"
#include "cgd_config.h"

#define FUZZTABLE   50

/* ---- engine globals (defined in r_draw.c / r_main.c / r_data.c) ---- */
extern const uint8_t *dc_colormap;
extern int dc_x, dc_yl, dc_yh;
extern int dc_iscale, dc_texturemid;
extern const uint8_t *dc_source;
extern uint8_t *dc_translation;
extern int ds_y, ds_x1, ds_x2;
extern const uint8_t *ds_colormap;
extern int ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern const uint8_t *ds_source;
extern uint8_t *ylookup[];
extern int columnofs[];
extern int centery;
extern int viewheight;
extern const uint8_t *colormaps;
extern int fuzzpos;

struct cgd_sh4_cmaps cgd_sh4_cmaps = { 0, 0, 0 };

void cgd_sh4_set_colormaps(const uint8_t *cm, int nmaps, uint8_t *rot)
{
	/* Disable first: the drawers fall back to C while the table is incomplete */
	cgd_sh4_cmaps.nmaps = 0;
	if(!cm || !rot || nmaps <= 0)
		return;
	if(nmaps > 256)
		nmaps = 256;
	for(int k = 0; k < nmaps; k++) {
		const uint8_t *s = cm + 256 * k;
		uint8_t *d = rot + 256 * k;
		for(int j = 0; j < 128; j++) {
			d[j] = s[j + 128];
			d[j + 128] = s[j];
		}
	}
	cgd_sh4_cmaps.src = cm;
	cgd_sh4_cmaps.delta = (intptr_t)(rot + 128) - (intptr_t)cm;
	cgd_sh4_cmaps.nmaps = (uint32_t)nmaps;
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
	uint8_t *dest = ylookup[dc_yl] + columnofs[dc_x];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	const uint8_t *cm = dc_colormap, *src = dc_source;
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
	uint8_t *dest = ylookup[dc_yl] + columnofs[x];
	uint8_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
	int fracstep = dc_iscale;
	int frac = (int)((unsigned)dc_texturemid + (unsigned)(dc_yl - centery) * (unsigned)fracstep);
	const uint8_t *cm = dc_colormap, *src = dc_source;
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
	uint8_t *dest = ylookup[dc_yl] + columnofs[dc_x];
	do {
		*dest = colormaps[6 * 256 + dest[cgd_sh4_fuzzoffset[fuzzpos]]];
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
	uint8_t *dest = ylookup[dc_yl] + columnofs[x];
	uint8_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
	do {
		*dest = colormaps[6 * 256 + dest[cgd_sh4_fuzzoffset[fuzzpos]]];
		*dest2 = colormaps[6 * 256 + dest2[cgd_sh4_fuzzoffset[fuzzpos]]];
		if(++fuzzpos == FUZZTABLE) fuzzpos = 0;
		dest += SCREENWIDTH;
		dest2 += SCREENWIDTH;
	} while(count--);
}

void cgd_sh4_slow_translated(void)
{
	int count = dc_yh - dc_yl;
	if(count < 0) return;
	uint8_t *dest = ylookup[dc_yl] + columnofs[dc_x];
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
	uint8_t *dest = ylookup[dc_yl] + columnofs[x];
	uint8_t *dest2 = ylookup[dc_yl] + columnofs[x + 1];
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
	uint8_t *dest = ylookup[ds_y] + columnofs[ds_x1];
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
	uint8_t *dest = ylookup[ds_y] + columnofs[ds_x1];
	do {
		int spot = ((yfrac >> (16 - 6)) & (63 * 64)) + ((xfrac >> 16) & 63);
		uint8_t c = ds_colormap[ds_source[spot]];
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
