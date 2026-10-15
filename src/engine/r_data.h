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
// DESCRIPTION:
//  Refresh module, data I/O, caching, retrieval of graphics
//  by name.
//
//-----------------------------------------------------------------------------


#ifndef __R_DATA__
#define __R_DATA__

#include "r_defs.h"
#include "r_state.h"

#ifdef __GNUG__
#pragma interface
#endif

#include "w_wad.h"

/* Texture column directory (r_data.c) */
extern int*			texturewidthmask;
extern short**			texturecolumnlump;
extern unsigned short**		texturecolumnofs;
extern byte**			texturecomposite;

/* Out-of-line part of R_GetColumn: builds a composite texture */
const byte* R_GetColumnComposite( int tex );

// Retrieve column data for span blitting.
/* CGDoom: inline, with the lump lookup through
   W_CacheLumpNumConstFast, so a wall column whose patch is already cached
   costs no call (instead of R_GetColumn -> W_CacheLumpNumConst ->
   Z_ChangeTag2 -> PL_IsWadPointer). Same result and same zone state as
   vanilla's. */
static inline const byte* R_GetColumn( int tex, int col )
{
  int lump, ofs;

  col &= texturewidthmask[tex];
  lump = texturecolumnlump[tex][col];
  ofs = texturecolumnofs[tex][col];

  if (lump > 0)
    return (const byte *)W_CacheLumpNumConstFast(lump, PU_CACHE) + ofs;

  if (texturecomposite[tex])
    return texturecomposite[tex] + ofs;
  return R_GetColumnComposite(tex) + ofs;
}


#if CGD_TRUECOLOR
/* CGDoom: true colour: the COLORMAP lump (palette indices; colormaps holds
   its maps in RGB565) */
extern const byte*		colormaps8;
/* Rebuilds the RGB565 light tables (colormaps) for the base palette, the
   current palette's tint (v_video.h) and a gamma table (I_SetPalette) */
typedef struct vtint_s vtint_t;
void R_BuildLightTables (const byte *base, const vtint_t *tint,
  const byte *gamma);
#endif

// I/O, setting up the stuff.
void R_InitData (void);
void R_PrecacheLevel (void);


// Retrieval.
// Floor/ceiling opaque texture tiles,
// lookup by name. For animation?
int R_FlatNumForName (const char* name);


// Called by P_Ticker for switches and animations,
// returns the texture number for the texture name.
int R_TextureNumForName (const char *name);
int R_CheckTextureNumForName (const char *name);

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
