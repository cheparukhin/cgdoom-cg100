// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	The actual span/column drawing functions.
//	Here find the main potential for optimization,
//	 e.g. inline assembly, different algorithms.
//
//-----------------------------------------------------------------------------


//static const char 


#include "doomdef.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "r_local.h"

// Needs access to LFB (guess what).
#include "v_video.h"

// State.
#include "doomstat.h"


// ?
#define MAXWIDTH			1120
#define MAXHEIGHT			832


//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//


pixel_t*		viewimage; 
int				viewwidth;
int				scaledviewwidth;
int				viewheight;
int				viewwindowx;
int				viewwindowy; 
pixel_t*		ylookup[MAXHEIGHT]; 
int				columnofs[MAXWIDTH]; 

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//
byte			translations[3][256];	




//
// R_DrawColumn
// Source is the top of the column to scale.
//
const lighttable_t*	dc_colormap; 
int				dc_x; 
int				dc_yl; 
int				dc_yh; 
fixed_t			dc_iscale; 
fixed_t			dc_texturemid;

// first pixel in a column (possibly virtual) 
const byte*		dc_source;		

// just for profiling 
int				dccount;

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
// 
void R_DrawColumn (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  fixed_t		frac;
  fixed_t		fracstep;

  count = dc_yh - dc_yl; 

  // Zero length, column does not exceed a pixel.
  if (count < 0) 
    return; 

#ifdef RANGECHECK 
  if ((unsigned)dc_x >= SCREENWIDTH
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT) 
    I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x); 
#endif 

  // Framebuffer destination address.
  // Use ylookup LUT to avoid multiply with ScreenWidth.
  // Use columnofs LUT for subwindows? 
  dest = ylookup[dc_yl] + columnofs[dc_x];  

  // Determine scaling,
  //  which is the only mapping to be done.
  fracstep = dc_iscale; 
  frac = dc_texturemid + (dc_yl-centery)*fracstep; 

  // Inner loop that does the actual texture mapping,
  //  e.g. a DDA-line scaling.
  // This is as fast as it gets.
  do 
  {
    // Re-map color indices from wall texture column
    //  using a lighting/special effects LUT.
    *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];

    dest += SCREENWIDTH; 
    frac += fracstep;

  } while (count--); 
}

//
// Low detail drawers (detailshift = 1).
// The view is viewwidth = scaledviewwidth/2 columns wide and every column
// covers two screen pixels. These follow Chocolate Doom, which matches the
// DOS executable (copied from src/engine/sh4/test/ref_draw.c, the reference
// the SH4 versions are bit-exact against; see src/engine/sh4/README.md):
//  - columns: x = dc_x << 1 is a LOCAL (dc_x is not modified; linuxdoom's
//    version did dc_x <<= 1, which breaks multi-post sprites because
//    R_DrawVisSprite loops on dc_x);
//  - spans: count = ds_x2 - ds_x1 is taken BEFORE ds_x1/ds_x2 are doubled
//    (on the globals, like vanilla); linuxdoom took it after and overdrew.
//
void R_DrawColumnLow (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  pixel_t*		dest2;
  fixed_t		frac;
  fixed_t		fracstep;	 
  int			x;

  count = dc_yh - dc_yl; 

  // Zero length.
  if (count < 0) 
    return; 

#ifdef RANGECHECK 
  if ((unsigned)dc_x >= SCREENWIDTH/2
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT)
  {
    I_Error ("R_DrawColumnLow: %i to %i at %i", dc_yl, dc_yh, dc_x);
  }
#endif 

  // Blocky mode, need to multiply by 2.
  x = dc_x << 1;

  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];

  fracstep = dc_iscale; 
  frac = dc_texturemid + (dc_yl-centery)*fracstep;

  do 
  {
    // Hack. Does not work corretly.
    *dest2 = *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
    frac += fracstep; 

  } while (count--);
}

//
// Spectre/Invisibility.
//
#define FUZZTABLE		50 
#define FUZZOFF	(SCREENWIDTH)


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

int	fuzzpos = 0; 

/* CGDoom: the spectre's darkening of the pixel behind it. Vanilla maps its
   palette index through colormap 6; in true colour, each RGB565 channel is
   scaled by 13/16 (colormap 6's brightness, 26/32). */
#if CGD_TRUECOLOR
#define FUZZDARK(p)	((pixel_t)((((p) >> 1) & 0x7bef) + (((p) >> 2) & 0x39e7) \
			  + (((p) >> 4) & 0x0861)))
#else
#define FUZZDARK(p)	(colormaps[6*256+(p)])
#endif


//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//
void R_DrawFuzzColumn (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  fixed_t		frac;
  fixed_t		fracstep;	 

  // Adjust borders. Low... 
  if (!dc_yl) 
    dc_yl = 1;

  // .. and high.
  if (dc_yh == viewheight-1) 
    dc_yh = viewheight - 2; 

  count = dc_yh - dc_yl; 

  // Zero length.
  if (count < 0) 
    return; 


#ifdef RANGECHECK 
  if ((unsigned)dc_x >= SCREENWIDTH
    || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
  {
    I_Error ("R_DrawFuzzColumn: %i to %i at %i",
      dc_yl, dc_yh, dc_x);
  }
#endif


  // Keep till detailshift bug in blocky mode fixed,
  //  or blocky mode removed.
  /* WATCOM code 
  if (detailshift)
  {
  if (dc_x & 1)
  {
  outpw (GC_INDEX,GC_READMAP+(2<<8) ); 
  outp (SC_INDEX+1,12); 
  }
  else
  {
  outpw (GC_INDEX,GC_READMAP); 
  outp (SC_INDEX+1,3); 
  }
  dest = destview + dc_yl*80 + (dc_x>>1); 
  }
  else
  {
  outpw (GC_INDEX,GC_READMAP+((dc_x&3)<<8) ); 
  outp (SC_INDEX+1,1<<(dc_x&3)); 
  dest = destview + dc_yl*80 + (dc_x>>2); 
  }*/


  // Does not work with blocky mode.
  dest = ylookup[dc_yl] + columnofs[dc_x];

  // Looks familiar.
  fracstep = dc_iscale; 
  frac = dc_texturemid + (dc_yl-centery)*fracstep; 

  // Looks like an attempt at dithering,
  //  using the colormap #6 (of 0-31, a bit
  //  brighter than average).
  do 
  {
    // Lookup framebuffer, and retrieve
    //  a pixel that is either one column
    //  left or right of the current one.
    // Add index from colormap to index.
    *dest = FUZZDARK(dest[fuzzoffset[fuzzpos]]); 

    // Clamp table lookup index.
    if (++fuzzpos == FUZZTABLE) 
      fuzzpos = 0;

    dest += SCREENWIDTH;

    frac += fracstep; 
  } while (count--); 
} 

//
// R_DrawFuzzColumnLow (low detail, Chocolate Doom semantics): both pixels
// of a column are fuzzed separately, one fuzzpos step per row.
//
void R_DrawFuzzColumnLow (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  pixel_t*		dest2; 
  int			x;

  // Adjust borders. Low... 
  if (!dc_yl) 
    dc_yl = 1;

  // .. and high.
  if (dc_yh == viewheight-1) 
    dc_yh = viewheight - 2; 

  count = dc_yh - dc_yl; 

  // Zero length.
  if (count < 0) 
    return; 

  // low detail mode, need to multiply by 2
  x = dc_x << 1;

#ifdef RANGECHECK 
  if ((unsigned)x >= SCREENWIDTH
    || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
  {
    I_Error ("R_DrawFuzzColumnLow: %i to %i at %i",
      dc_yl, dc_yh, dc_x);
  }
#endif

  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];

  do 
  {
    *dest = FUZZDARK(dest[fuzzoffset[fuzzpos]]); 
    *dest2 = FUZZDARK(dest2[fuzzoffset[fuzzpos]]); 

    // Clamp table lookup index.
    if (++fuzzpos == FUZZTABLE) 
      fuzzpos = 0;

    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
  } while (count--); 
} 




//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//
byte*	dc_translation;
byte*	translationtables;

void R_DrawTranslatedColumn (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  fixed_t		frac;
  fixed_t		fracstep;	 

  count = dc_yh - dc_yl; 
  if (count < 0) 
    return; 

#ifdef RANGECHECK 
  if ((unsigned)dc_x >= SCREENWIDTH
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT)
  {
    I_Error ( "R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
  }

#endif 

  // WATCOM VGA specific.
  /* Keep for fixing.
  if (detailshift)
  {
  if (dc_x & 1)
  outp (SC_INDEX+1,12); 
  else
  outp (SC_INDEX+1,3);

  dest = destview + dc_yl*80 + (dc_x>>1); 
  }
  else
  {
  outp (SC_INDEX+1,1<<(dc_x&3)); 

  dest = destview + dc_yl*80 + (dc_x>>2); 
  }*/


  // FIXME. As above.
  dest = ylookup[dc_yl] + columnofs[dc_x]; 

  // Looks familiar.
  fracstep = dc_iscale; 
  frac = dc_texturemid + (dc_yl-centery)*fracstep; 

  // Here we do an additional index re-mapping.
  do 
  {
    // Translation tables are used
    //  to map certain colorramps to other ones,
    //  used with PLAY sprites.
    // Thus the "green" ramp of the player 0 sprite
    //  is mapped to gray, red, black/indigo. 
    *dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
    dest += SCREENWIDTH;

    frac += fracstep; 
  } while (count--); 
} 

//
// R_DrawTranslatedColumnLow (low detail, Chocolate Doom semantics)
//
void R_DrawTranslatedColumnLow (void) 
{ 
  int			count; 
  pixel_t*		dest; 
  pixel_t*		dest2; 
  fixed_t		frac;
  fixed_t		fracstep;	 
  int			x;

  count = dc_yh - dc_yl; 
  if (count < 0) 
    return; 

  // low detail, need to scale by 2
  x = dc_x << 1;

#ifdef RANGECHECK 
  if ((unsigned)x >= SCREENWIDTH
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT)
  {
    I_Error ( "R_DrawTranslatedColumnLow: %i to %i at %i", dc_yl, dc_yh, x);
  }
#endif 

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




#if CGD_EXTRAS && CGD_TRUECOLOR
//
// CGDoom: translucent columns (r_draw.h). An RGB565 pixel is spread over 32
// bits with a gap above each channel (0x07e0f81f: green in bits 21-26, red
// in 11-15, blue in 0-4), so that the three channels are added or scaled
// at once without carrying into each other.
//
static inline uint32_t TL_Spread (pixel_t p)
{
  return ((uint32_t)p | ((uint32_t)p << 16)) & 0x07e0f81f;
}

static inline pixel_t TL_Fold (uint32_t s)
{
  s &= 0x07e0f81f;
  return (pixel_t)(s | (s >> 16));
}

/* 21/32 of fg over bg (Boom's TRANMAP and Crispy Doom: 66%) */
static inline pixel_t TL_Over (pixel_t bg, pixel_t fg)
{
  return TL_Fold ((TL_Spread (fg) * 21 + TL_Spread (bg) * 11) >> 5);
}

/* fg added to bg, each channel saturated */
static inline pixel_t TL_Add (pixel_t bg, pixel_t fg)
{
  uint32_t s = TL_Spread (fg) + TL_Spread (bg);
  uint32_t o = s & 0x00010020, og = s & 0x08000000;	/* carries */
  return TL_Fold (s | (o - (o >> 5)) | (og - (og >> 6)));
}

#define TL_DRAWER(name, blend)						\
void name (void)							\
{									\
  int		count = dc_yh - dc_yl;					\
  pixel_t*	dest;							\
  fixed_t	frac, fracstep;						\
									\
  if (count < 0)							\
    return;								\
  dest = ylookup[dc_yl] + columnofs[dc_x];				\
  fracstep = dc_iscale;							\
  frac = dc_texturemid + (dc_yl-centery)*fracstep;			\
  do									\
  {									\
    *dest = blend (*dest, dc_colormap[dc_source[(frac>>FRACBITS)&127]]); \
    dest += SCREENWIDTH;						\
    frac += fracstep;							\
  } while (count--);							\
}									\
									\
void name##Low (void)							\
{									\
  int		count = dc_yh - dc_yl;					\
  pixel_t*	dest;							\
  pixel_t*	dest2;							\
  fixed_t	frac, fracstep;						\
  int		x = dc_x << 1;						\
									\
  if (count < 0)							\
    return;								\
  dest = ylookup[dc_yl] + columnofs[x];					\
  dest2 = ylookup[dc_yl] + columnofs[x+1];				\
  fracstep = dc_iscale;							\
  frac = dc_texturemid + (dc_yl-centery)*fracstep;			\
  do									\
  {									\
    pixel_t c = dc_colormap[dc_source[(frac>>FRACBITS)&127]];		\
    *dest = blend (*dest, c);						\
    *dest2 = blend (*dest2, c);						\
    dest += SCREENWIDTH;						\
    dest2 += SCREENWIDTH;						\
    frac += fracstep;							\
  } while (count--);							\
}

TL_DRAWER (R_DrawTLOverColumn, TL_Over)
TL_DRAWER (R_DrawTLAddColumn, TL_Add)
#endif


//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//
void R_InitTranslationTables (void)
{
  int		i;

  translationtables = (byte *)Z_Malloc (256*3+255, PU_STATIC, 0);
  translationtables = (byte *)(( (uintptr_t)translationtables + 255 )& ~255);

  // translate just the 16 green colors
  for (i=0 ; i<256 ; i++)
  {
    if (i >= 0x70 && i<= 0x7f)
    {
      // map green ramp to gray, brown, red
      translationtables[i]      =(byte)( 0x60 + (i&0xf));
      translationtables [i+256] =(byte)( 0x40 + (i&0xf));
      translationtables [i+512] =(byte)( 0x20 + (i&0xf));
    }
    else
    {
      // Keep all other colors as is.
      translationtables[i] = translationtables[i+256] = translationtables[i+512] = (byte)i;
    }
  }
}




//
// R_DrawSpan 
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//
int			ds_y; 
int			ds_x1; 
int			ds_x2;

const lighttable_t*	ds_colormap; 

fixed_t			ds_xfrac; 
fixed_t			ds_yfrac; 
fixed_t			ds_xstep; 
fixed_t			ds_ystep;

// start of a 64*64 tile image 
const byte*			ds_source;	

// just for profiling
int			dscount;


//
// Draws the actual span.
void R_DrawSpan (void) 
{ 
  fixed_t		xfrac;
  fixed_t		yfrac; 
  pixel_t*		dest; 
  int			count;
  int			spot; 

#ifdef RANGECHECK 
  if (ds_x2 < ds_x1
    || ds_x1<0
    || ds_x2>=SCREENWIDTH  
    || (unsigned)ds_y>SCREENHEIGHT)
  {
    I_Error( "R_DrawSpan: %i to %i at %i",
      ds_x1,ds_x2,ds_y);
  }
  //	dscount++; 
#endif 

  xfrac = ds_xfrac; 
  yfrac = ds_yfrac; 

  dest = ylookup[ds_y] + columnofs[ds_x1];

  // We do not check for zero spans here?
  count = ds_x2 - ds_x1; 

  do 
  {
    // Current texture index in u,v.
    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);

    // Lookup pixel from flat texture tile,
    //  re-index using light/colormap.
    *dest++ = ds_colormap[ds_source[spot]];

    // Next step in u,v.
    xfrac += ds_xstep; 
    yfrac += ds_ystep;

  } while (count--); 
}

//
// R_DrawSpanLow (low detail, Chocolate Doom semantics): the count is taken
// before ds_x1/ds_x2 are doubled; every texel covers two pixels.
//
void R_DrawSpanLow (void) 
{ 
  fixed_t		xfrac;
  fixed_t		yfrac; 
  pixel_t*		dest; 
  int			count;
  int			spot; 

#ifdef RANGECHECK 
  if (ds_x2 < ds_x1
    || ds_x1<0
    || ds_x2>=SCREENWIDTH/2
    || (unsigned)ds_y>SCREENHEIGHT)
  {
    I_Error( "R_DrawSpanLow: %i to %i at %i",
      ds_x1,ds_x2,ds_y);
  }
#endif 

  xfrac = ds_xfrac; 
  yfrac = ds_yfrac; 

  count = ds_x2 - ds_x1; 

  // Blocky mode, need to multiply by 2.
  ds_x1 <<= 1;
  ds_x2 <<= 1;

  dest = ylookup[ds_y] + columnofs[ds_x1];

  do 
  {
    spot = ((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63);

    // Lowres/blocky mode does it twice,
    //  while scale is adjusted appropriately.
    *dest++ = ds_colormap[ds_source[spot]];
    *dest++ = ds_colormap[ds_source[spot]];

    xfrac += ds_xstep; 
    yfrac += ds_ystep;

  } while (count--); 
}


#if CGD_EXTRAS
//
// CGDoom: brightmapped columns and spans (CGD_FEAT_BRIGHTMAPS, r_bmaps.h).
// The drawers above, except that each texel picks its light table by its
// brightmap flag: colormaps (full brightness) for a lit texel, else
// dc_colormap / ds_colormap. In C for every pixel format; the renderer uses
// them only for the images that have a brightmap, outside fixedcolormap.
//
const byte*		dc_brightmap;
const byte*		ds_brightmap;

void R_DrawBrightColumn (void)
{
  int			count = dc_yh - dc_yl;
  pixel_t*		dest;
  fixed_t		frac;
  fixed_t		fracstep;
  const byte* const	src = dc_source;
  const byte* const	bm = dc_brightmap;
  const lighttable_t*	maps[2];

  if (count < 0)
    return;

#ifdef RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT)
    I_Error ("R_DrawBrightColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif

  maps[0] = dc_colormap;
  maps[1] = colormaps;
  dest = ylookup[dc_yl] + columnofs[dc_x];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;

  do
  {
    const byte	t = src[(frac>>FRACBITS)&127];

    *dest = maps[bm[t]][t];
    dest += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void R_DrawBrightColumnLow (void)
{
  int			count = dc_yh - dc_yl;
  pixel_t*		dest;
  pixel_t*		dest2;
  fixed_t		frac;
  fixed_t		fracstep;
  int			x;
  const byte* const	src = dc_source;
  const byte* const	bm = dc_brightmap;
  const lighttable_t*	maps[2];

  if (count < 0)
    return;

#ifdef RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH/2
    || dc_yl < 0
    || dc_yh >= SCREENHEIGHT)
    I_Error ("R_DrawBrightColumnLow: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif

  maps[0] = dc_colormap;
  maps[1] = colormaps;
  x = dc_x << 1;
  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x+1];
  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl-centery)*fracstep;

  do
  {
    const byte	t = src[(frac>>FRACBITS)&127];

    *dest2 = *dest = maps[bm[t]][t];
    dest += SCREENWIDTH;
    dest2 += SCREENWIDTH;
    frac += fracstep;
  } while (count--);
}

void R_DrawBrightSpan (void)
{
  fixed_t		xfrac = ds_xfrac;
  fixed_t		yfrac = ds_yfrac;
  pixel_t*		dest;
  int			count;
  const byte* const	src = ds_source;
  const byte* const	bm = ds_brightmap;
  const lighttable_t*	maps[2];

#ifdef RANGECHECK
  if (ds_x2 < ds_x1
    || ds_x1<0
    || ds_x2>=SCREENWIDTH
    || (unsigned)ds_y>SCREENHEIGHT)
    I_Error ("R_DrawBrightSpan: %i to %i at %i", ds_x1, ds_x2, ds_y);
#endif

  maps[0] = ds_colormap;
  maps[1] = colormaps;
  dest = ylookup[ds_y] + columnofs[ds_x1];
  count = ds_x2 - ds_x1;

  do
  {
    const byte	t = src[((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63)];

    *dest++ = maps[bm[t]][t];
    xfrac += ds_xstep;
    yfrac += ds_ystep;
  } while (count--);
}

/* Chocolate Doom's low detail semantics, as R_DrawSpanLow */
void R_DrawBrightSpanLow (void)
{
  fixed_t		xfrac = ds_xfrac;
  fixed_t		yfrac = ds_yfrac;
  pixel_t*		dest;
  int			count;
  const byte* const	src = ds_source;
  const byte* const	bm = ds_brightmap;
  const lighttable_t*	maps[2];

#ifdef RANGECHECK
  if (ds_x2 < ds_x1
    || ds_x1<0
    || ds_x2>=SCREENWIDTH/2
    || (unsigned)ds_y>SCREENHEIGHT)
    I_Error ("R_DrawBrightSpanLow: %i to %i at %i", ds_x1, ds_x2, ds_y);
#endif

  maps[0] = ds_colormap;
  maps[1] = colormaps;
  count = ds_x2 - ds_x1;
  ds_x1 <<= 1;
  ds_x2 <<= 1;
  dest = ylookup[ds_y] + columnofs[ds_x1];

  do
  {
    const byte	t = src[((yfrac>>(16-6))&(63*64)) + ((xfrac>>16)&63)];
    const pixel_t c = maps[bm[t]][t];

    *dest++ = c;
    *dest++ = c;
    xfrac += ds_xstep;
    yfrac += ds_ystep;
  } while (count--);
}
#endif


//
// R_InitBuffer 
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//
void R_InitBuffer( int	width,  int	height ) 
{ 
  int		i; 

  // Handle resize,
  //  e.g. smaller view windows
  //  with border and/or status bar.
  viewwindowx = (SCREENWIDTH-width) >> 1; 

  // Column offset. For windows.
  for (i=0 ; i<width ; i++) 
    columnofs[i] = viewwindowx + i;

  // Samw with base row offset.
  if (width == SCREENWIDTH) 
    viewwindowy = 0; 
  else 
    viewwindowy = (SCREENHEIGHT-SBARHEIGHT-height) >> 1; 

  // Preclaculate all row offsets.
  for (i=0 ; i<height ; i++) 
    ylookup[i] = screens[0] + (i+viewwindowy)*SCREENWIDTH; 
} 



//
// R_VideoErase
// Erases part of the border of a reduced view (R_DrawViewBorder, HUD text
// erase in hu_lib.c).
// CGDoom: the background is black (V_Clear clears the view area when the
// view size changes) instead of vanilla's flat pattern; vanilla copied it
// from screens[1], which CGDoom uses for the intermission background, so
// the border is cleared to 0 directly (a copy would paste pieces of the
// last intermission screen into the border).
//
void R_VideoErase ( unsigned ofs, int count ) 
{ 
  if(count <0)return;//hack
  V_FillPixels (screens[0]+ofs, count, V_PIXEL(0)); 
} 


//
// R_DrawViewBorder
// Draws the border around the view
//  for different size windows?
//
void V_MarkRect( int x,  int y, int	width,
                int		height ); 

void R_DrawViewBorder (void) 
{ 
  int		top;
  int		side;
  int		ofs;
  int		i; 

  if (scaledviewwidth == SCREENWIDTH) 
    return; 

  top = ((SCREENHEIGHT-SBARHEIGHT)-viewheight)/2; 
  side = (SCREENWIDTH-scaledviewwidth)/2; 

  // copy top and one line of left side 
  R_VideoErase (0, top*SCREENWIDTH+side); 

  // copy one line of right side and bottom 
  ofs = (viewheight+top)*SCREENWIDTH-side; 
  R_VideoErase (ofs, top*SCREENWIDTH+side); 

  // copy sides using wraparound 
  ofs = top*SCREENWIDTH + SCREENWIDTH-side; 
  side <<= 1;

  for (i=1 ; i<viewheight ; i++) 
  { 
    R_VideoErase (ofs, side); 
    ofs += SCREENWIDTH; 
  } 

  // ? 
  V_MarkRect (0,0,SCREENWIDTH, SCREENHEIGHT-SBARHEIGHT); 
} 


