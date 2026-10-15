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
   bar, SCREENWIDTH x ST_HEIGHT) */
extern	byte*	screens[SCREEN_COUNT];

#if CGD_EXTRAS
/* CGDoom: the translation V_DrawPatch draws through, NULL for none: one of
   v_crmap's (status-bar numbers, CGD_FEAT_COLORNUMS: the digits' reds
   turned into the colour CR_*, the other entries kept) or v_blackmap
   (every entry black: shadows). V_BuildColorMaps builds v_crmap from the
   palette (PLAYPAL 0); the flashes and gamma then tint them as everything
   else. */
enum { CR_GREEN, CR_GOLD, CR_BLUE, CR_GRAY, CR_COUNT };
extern	const byte	*v_patchmap;
extern	byte	v_crmap[CR_COUNT][256];
extern	const byte	v_blackmap[256];
void V_BuildColorMaps (const byte *pal);
#endif

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
void V_DrawBlock( int x, int y,int scrn,int width,int height,const byte* src );

// Reads a linear block of pixels into the view buffer.
void V_GetBlock(int x,int y,int scrn,int width,int height,byte* dest );


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
