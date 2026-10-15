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
//	Gamma correction LUT.
//	Functions to draw patches (by post) directly to screen.
//	Functions to blit a block to the screen.
//
//-----------------------------------------------------------------------------


#ifndef __V_VIDEO__
#define __V_VIDEO__

#include "doomtype.h"

#include "doomdef.h"

// Needed because we are refering to patches.
#include "r_data.h"

// Needed for I_PrepScreen()
#include "i_video.h"

//
// VIDEO
//

#define CENTERY			(SCREENHEIGHT/2)

// Screen numbers
#define BG 4
#define FG 0


#define SCREEN_COUNT	5
/* CGDoom: SCREENWIDTH x SCREENHEIGHT pixels each (screens[4]: the status
   bar, SCREENWIDTH x ST_HEIGHT). With CGD_TRANSIENT_SCREENS, screens[1] is
   NULL and screens[2] and [3] exist only during a wipe (f_wipe.c). */
extern	pixel_t*	screens[SCREEN_COUNT];

/* CGDoom: the builds with larger screens (widescreen or true colour) save
   their memory: the intermission redraws its background every frame instead
   of keeping it in screens[1], and the wipe allocates its two screens for
   its duration (it is skipped when they do not fit). */
#define CGD_TRANSIENT_SCREENS	(CGD_WIDESCREEN || CGD_TRUECOLOR)

/* CGDoom: the pixel of palette index c. 2D art (patches, the automap,
   fills) is drawn through it: in the true-colour build, v_palette holds
   the RGB565 colours of the current palette (I_SetPalette). */
#if CGD_TRUECOLOR
extern	uint16_t	v_palette[256];
/* Counts the palette changes (I_SetPalette): what a true-colour screen keeps
   from earlier frames must be redrawn when it changes */
extern	int	v_palette_serial;
#define V_PIXEL(c)	(v_palette[(byte)(c)])
#else
#define V_PIXEL(c)	((pixel_t)(c))
#endif

#if CGD_EXTRAS && CGD_TRUECOLOR
/* The colours V_DrawPatch draws with: v_palette, or for a moment one of
   v_crpalette's recoloured palettes (status-bar numbers, CGD_FEAT_COLORNUMS):
   the saturated reds of the palette turned into the colour CR_*, the other
   colours kept. Rebuilt with v_palette. */
enum { CR_GREEN, CR_GOLD, CR_BLUE, CR_GRAY, CR_COUNT };
extern	const uint16_t	*v_patchpal;
extern	uint16_t	v_crpalette[CR_COUNT][256];
void V_BuildColorPalettes (void);
#endif

/* CGDoom: fills count pixels with c */
void V_FillPixels (pixel_t *dest, int count, pixel_t c);

extern  int	dirtybox[4];

extern	const byte	gammatable[5][256];
extern	int	usegamma;



// Allocates buffer screens, call before R_Init.
void V_Init (void);


void
V_CopyRect
( int		srcx,
  int		srcy,
  int		srcscrn,
  int		width,
  int		height,
  int		destx,
  int		desty,
  int		destscrn );

void V_DrawPatch(int x,int y, int scrn,const patch_t*	patch);
#define V_DrawPatchDirect(x,y,scrn,patch ) V_DrawPatch(x,y,scrn,patch)

/* CGDoom: draws a 320x200 full-screen picture (title, help, intermission,
   finale), centred on a larger screen whose borders it clears */
void V_DrawFullscreenPatch(int scrn, const char *lumpname);

/* CGDoom: draws a patch at (x, y) of the 320x200 layout of the 2D screens
   (menus, intermission, finale), which is centred on a larger screen like
   the full-screen pictures */
#define V_DrawLayoutPatch(x,y,scrn,patch) \
	V_DrawPatch((x) + WIDESCREENDELTA, (y) + HEIGHTDELTA, scrn, patch)

// Draw a linear block of pixels into the view buffer.
void V_DrawBlock( int x, int y,int scrn,int width,int height,const pixel_t* src );

// Reads a linear block of pixels into the view buffer.
void V_GetBlock(int x,int y,int scrn,int width,int height,pixel_t* dest );


void
V_MarkRect
( int		x,
  int		y,
  int		width,
  int		height );

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
