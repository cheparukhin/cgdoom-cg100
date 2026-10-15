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
//	System specific interface stuff.
//
//-----------------------------------------------------------------------------


#ifndef __R_MAIN__
#define __R_MAIN__

#include "d_player.h"
#include "r_data.h"


#ifdef __GNUG__
#pragma interface
#endif


//
// POV related.
//
extern fixed_t		viewcos;
extern fixed_t		viewsin;

extern int		viewwidth;
extern int		viewheight;
extern int		viewwindowx;
extern int		viewwindowy;



extern int		centerx;
extern int		centery;

extern fixed_t		centerxfrac;
extern fixed_t		centeryfrac;
extern fixed_t		projection;
extern int		viewwidth_nonwide;
/* CGDoom: the vertical focal length: projection, or with the aspect-ratio
   correction (CGD_FEAT_ASPECT) ASPECT_NUM/ASPECT_DEN of it. DOOM's 320x200
   was shown on 4:3 monitors, where its pixels were 6/5 taller than wide; on
   the calculator's square pixels the 3D view is scaled by 6/5 vertically
   instead (walls, sprites, flats, sky and weapon), so that it has the
   shapes it was drawn for. */
extern fixed_t		projectiony;
#define ASPECT_NUM		6
#define ASPECT_DEN		5

extern int		validcount;

extern int		linecount;
extern int		loopcount;


//
// Lighting LUT.
// Used for z-depth cuing per column/row,
//  and other lighting effects (sector ambient, flash).
//

// Lighting constants.
// Now why not 32 levels here?
/* CGDoom: the CGD_EXTRAS builds also have smooth diminishing lighting
   (CGD_FEAT_SMOOTHLIGHT, Crispy Doom's values for 32 colormaps): twice the
   sector light levels (LIGHTBRIGHT levels per step of extralight and of
   the walls' fake contrast) and distance steps 8 times finer. There these
   constants are variables (R_InitLightTables) and the tables are sized for
   the smooth values; zlight then holds colormap numbers instead of
   pointers (32 KiB instead of 128 KiB), read through ZLIGHT_MAP. */
#if CGD_EXTRAS
extern int		lightlevels, lightsegshift, lightbright;
extern int		maxlightz, lightzshift;
#define LIGHTLEVELS		lightlevels
#define LIGHTSEGSHIFT		lightsegshift
#define LIGHTBRIGHT		lightbright
#define MAXLIGHTZ		maxlightz
#define LIGHTZSHIFT		lightzshift
#define LIGHTLEVELS_MAX		32
#define MAXLIGHTZ_MAX		1024
typedef byte			zlight_t;
#define ZLIGHT_MAP(z)		(colormaps + (z)*256)
#else
#define LIGHTLEVELS	        16
#define LIGHTSEGSHIFT	         4
#define LIGHTBRIGHT		1
#define MAXLIGHTZ	       128
#define LIGHTZSHIFT		20
#define LIGHTLEVELS_MAX		LIGHTLEVELS
#define MAXLIGHTZ_MAX		MAXLIGHTZ
typedef const lighttable_t*	zlight_t;
#define ZLIGHT_MAP(z)		(z)
#endif

#define MAXLIGHTSCALE		48
#define LIGHTSCALESHIFT		12

extern const lighttable_t*	scalelight[LIGHTLEVELS_MAX][MAXLIGHTSCALE];
extern const lighttable_t*	scalelightfixed[MAXLIGHTSCALE];
extern zlight_t			zlight[LIGHTLEVELS_MAX][MAXLIGHTZ_MAX];

extern int		extralight;
extern const lighttable_t*	fixedcolormap;


// Number of diminishing brightness levels.
// There a 0-31, i.e. 32 LUT in the COLORMAP lump.
#define NUMCOLORMAPS		32


// Blocky/low detail mode.
//B remove this?
//  0 = high, 1 = low
extern	int		detailshift;	


//
// Function pointers to switch refresh/drawing functions.
// Used to select shadow mode etc.
//
extern void		(*colfunc) (void);
extern void		(*basecolfunc) (void);
extern void		(*fuzzcolfunc) (void);
extern void		(*transcolfunc) (void);
// No shadow effects on floors.
extern void		(*spanfunc) (void);


//
// Utility functions.
int
R_PointOnSide
( fixed_t	x,
  fixed_t	y,
  node_t*	node );

int
R_PointOnSegSide
( fixed_t	x,
  fixed_t	y,
  seg_t*	line );

angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y );

angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 );

fixed_t
R_PointToDist
( fixed_t	x,
  fixed_t	y );


fixed_t R_ScaleFromGlobalAngle (angle_t visangle);

subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y );

void
R_AddPointToBox
( int		x,
  int		y,
  fixed_t*	box );



//
// REFRESH - the actual rendering functions.
//

// Called by G_Drawer.
void R_RenderPlayerView (player_t *player);

// Called by startup code.
void R_Init (void);

// Called by M_Responder.
// CGDoom: vanilla's signature (blocks 3..11, detail 0 = high,
// 1 = low); takes effect at the next frame (R_ExecuteSetViewSize).
void R_SetViewSize (int blocks, int detail);
void R_ExecuteSetViewSize (void);

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
