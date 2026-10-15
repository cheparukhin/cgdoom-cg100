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
//	Rendering of moving objects, sprites.
//
//-----------------------------------------------------------------------------


#ifndef __R_THINGS__
#define __R_THINGS__


#ifdef __GNUG__
#pragma interface
#endif

/* CGDoom: vissprites grow on demand from vanilla's 128
   up to MAXVISSPRITES; past it sprites are dropped as vanilla did at 128 */
#define MAXVISSPRITES_INITIAL	128
#define MAXVISSPRITES  	1024

extern vissprite_t*	vissprites;
extern vissprite_t*	vissprite_p;
extern vissprite_t	vsprsortedhead;

// Constant arrays used for psprite clipping
//  and initializing clipping.
extern const short		negonearray[SCREENWIDTH];
extern short		screenheightarray[SCREENWIDTH];

// vars for R_DrawMaskedColumn
extern short*		mfloorclip;
extern short*		mceilingclip;
extern fixed_t		spryscale;
extern fixed_t		sprtopscreen;

extern fixed_t		pspritescale;
extern fixed_t		pspriteiscale;
/* CGDoom: their vertical versions (aspect-ratio correction, r_main.h) */
extern fixed_t		pspritescaley;
extern fixed_t		pspriteiscaley;
/* CGDoom: added to the weapon's texturemid so that it shows the rows it
   shows in vanilla's view of the same size (0 in the 320x200 build) */
extern fixed_t		pspriteshift;


void R_DrawMaskedColumn (const column_t* column);


void R_SortVisSprites (void);

void R_AddSprites (sector_t* sec);
void R_AddPSprites (void);
void R_DrawSprites (void);
void R_InitSprites (void);
void R_ClearSprites (void);
void R_DrawMasked (void);

void
R_ClipVisSprite
( vissprite_t*		vis,
  int			xl,
  int			xh );


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
