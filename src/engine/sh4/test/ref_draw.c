/* ref_draw.c - reference C drawers for the SH4 tests.

   R_DrawColumn / R_DrawFuzzColumn / R_DrawTranslatedColumn / R_DrawSpan are transcribed from
   CGDoom's cgdoom/r_draw.c (identical to linuxdoom-1.10 apart from RANGECHECK), with the same
   types and the same expressions. The low-detail drawers follow Chocolate Doom (== DOS vanilla
   behaviour; linuxdoom-1.10's C versions mutate dc_x and overdraw spans, see ../README.md):
     R_DrawColumnLow:  x = dc_x << 1 (local), writes columnofs[x] and columnofs[x+1]
     R_DrawSpanLow:    count = ds_x2 - ds_x1 before ds_x1 <<= 1; ds_x2 <<= 1
     R_DrawFuzzColumnLow / R_DrawTranslatedColumnLow: as Chocolate Doom.
   Compiled with the test's -O2 these are also the "C" side of the benchmarks. */

#include "test_globals.h"

#define FRACBITS 16
#define FUZZTABLE 50
#define FUZZOFF (SCREENWIDTH)

static const int fuzzoffset[FUZZTABLE] =
{
  FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};

/* Note: the engine is compiled with -fwrapv semantics in practice (GCC on SH wraps); the
   reference uses the same int expressions as r_draw.c and is built with -fwrapv. */

void ref_R_DrawColumn (void)
{
  int			count;
  byte*		dest;
  fixed_t		frac;
  fixed_t		fracstep;

  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  dest = ylookup[dc_yl] + columnofs[dc_x];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;
  do
  {
    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
    dest += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void ref_R_DrawColumnLow (void)
{
  int			count;
  byte*		dest;
  byte*		dest2;
  fixed_t		frac;
  fixed_t		fracstep;
  int x;

  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  x = dc_x << 1;
  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;
  do
  {
    *dest2 = *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void ref_R_DrawFuzzColumn (void)
{
  int			count;
  byte*		dest;

  if (!dc_yl)
    dc_yl = 1;
  if (dc_yh == viewheight-1)
    dc_yh = viewheight - 2;
  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  dest = ylookup[dc_yl] + columnofs[dc_x];
  do
  {
    *dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
    if (++fuzzpos == FUZZTABLE)
      fuzzpos = 0;
    dest += SCREENWIDTH;
  } while (count--);
}

void ref_R_DrawFuzzColumnLow (void)
{
  int			count;
  byte*		dest;
  byte*		dest2;
  int x;

  if (!dc_yl)
    dc_yl = 1;
  if (dc_yh == viewheight-1)
    dc_yh = viewheight - 2;
  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  x = dc_x << 1;
  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];
  do
  {
    *dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
    *dest2 = colormaps[6*256+dest2[fuzzoffset[fuzzpos]]];
    if (++fuzzpos == FUZZTABLE)
      fuzzpos = 0;
    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
  } while (count--);
}

void ref_R_DrawTranslatedColumn (void)
{
  int			count;
  byte*		dest;
  fixed_t		frac;
  fixed_t		fracstep;

  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  dest = ylookup[dc_yl] + columnofs[dc_x];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;
  do
  {
    *dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
    dest += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void ref_R_DrawTranslatedColumnLow (void)
{
  int			count;
  byte*		dest;
  byte*		dest2;
  fixed_t		frac;
  fixed_t		fracstep;
  int x;

  count = dc_yh - dc_yl;
  if (count < 0)
    return;
  x = dc_x << 1;
  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;
  do
  {
    *dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
    *dest2 = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void ref_R_DrawSpan (void)
{
  fixed_t		xfrac;
  fixed_t		yfrac;
  byte*		dest;
  int			count;
  int			spot;

  xfrac = ds_xfrac;
  yfrac = ds_yfrac;
  dest = ylookup[ds_y] + columnofs[ds_x1];
  count = ds_x2 - ds_x1;
  do
  {
    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
    *dest++ = ds_colormap[ds_source[spot]];
    xfrac += ds_xstep;
    yfrac += ds_ystep;
  } while (count--);
}

void ref_R_DrawSpanLow (void)
{
  fixed_t		xfrac;
  fixed_t		yfrac;
  byte*		dest;
  int			count;
  int			spot;

  xfrac = ds_xfrac;
  yfrac = ds_yfrac;
  count = ds_x2 - ds_x1;
  ds_x1 <<= 1;
  ds_x2 <<= 1;
  dest = ylookup[ds_y] + columnofs[ds_x1];
  do
  {
    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);
    *dest++ = ds_colormap[ds_source[spot]];
    *dest++ = ds_colormap[ds_source[spot]];
    xfrac += ds_xstep;
    yfrac += ds_ystep;
  } while (count--);
}

/* Presenter conversion reference */
void ref_expand(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n)
{
  for (int i = 0; i < n; i++)
    dst[i] = pal[src[i]];
}
