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
//	Rendering main loop and setup functions,
//	 utility functions (BSP, geometry, trigonometry).
//	See tables.c, too.
//
//-----------------------------------------------------------------------------



#include "os.h"
#include "cgdoom.h"


#include "doomdef.h"
//#include "d_net.h"

#include "m_bbox.h"

#include "r_local.h"
#include "r_sky.h"
#include "v_video.h"
#ifdef CGD_SH4
#include "sh4/cgd_sh4.h"
#endif





// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW			2048	

int							viewangleoffset;

// increment every time a check is made
int							validcount = 1;		


const lighttable_t*				fixedcolormap;
extern const lighttable_t**		walllights;

int							centerx;
int							centery;

fixed_t						centerxfrac;
fixed_t						centeryfrac;
fixed_t						projection;

/* CGDoom: widescreen ("Hor+", as Crispy Doom): the view width that the
   field of view and the scales are defined for, 320 at full size (blocks*32
   below, >>detailshift). The view is wider (viewwidth); its centre is
   centerx. projection, the focal length, is half this width, so a wider
   screen shows more of the scene to the sides at the same scale. In the
   320-wide build it equals viewwidth. */
int						viewwidth_nonwide;
fixed_t						projectiony;

// just for profiling purposes
int							framecount;	

int							sscount;
int							linecount;
int							loopcount;

fixed_t						viewx;
fixed_t						viewy;
fixed_t						viewz;

angle_t						viewangle;

fixed_t						viewcos;
fixed_t						viewsin;

player_t*					viewplayer;

// 0 = high, 1 = low
int							detailshift;	

//
// precalculated math tables
//
angle_t						clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X. 
int							viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t						xtoviewangle[SCREENWIDTH+1];


// UNUSED.
// The finetangentgent[angle+FINEANGLES/4] table
// holds the fixed_t tangent values for view angles,
// ranging from MININT to 0 to MAXINT.
// fixed_t		finetangent[FINEANGLES/2];

// fixed_t		finesine[5*FINEANGLES/4];
//fixed_t*					finecosine = &finesine[FINEANGLES/4];


const lighttable_t*				scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
const lighttable_t*				scalelightfixed[MAXLIGHTSCALE];
const lighttable_t*				zlight[LIGHTLEVELS][MAXLIGHTZ];

// bumped light from gun blasts
int							extralight;			



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);


/* finecosine() is static inline in tables.h */


//
// R_AddPointToBox
// Expand a given bbox
// so that it encloses a given point.
//
void R_AddPointToBox( int x, int y,fixed_t*	box )
{
  if (x< box[BOXLEFT])
    box[BOXLEFT] = x;
  if (x> box[BOXRIGHT])
    box[BOXRIGHT] = x;
  if (y< box[BOXBOTTOM])
    box[BOXBOTTOM] = y;
  if (y> box[BOXTOP])
    box[BOXTOP] = y;
}


//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int R_PointOnSide( fixed_t	x, fixed_t	y, node_t*	node )
{
  fixed_t	dx;
  fixed_t	dy;
  fixed_t	left;
  fixed_t	right;

  if (!node->dx)
  {
    if (x <= node->x)
      return node->dy > 0;

    return node->dy < 0;
  }
  if (!node->dy)
  {
    if (y <= node->y)
      return node->dx < 0;

    return node->dx > 0;
  }

  dx = (x - node->x);
  dy = (y - node->y);

  // Try to quickly decide by looking at sign bits.
  if ( (node->dy ^ node->dx ^ dx ^ dy)&0x80000000 )
  {
    if  ( (node->dy ^ dx) & 0x80000000 )
    {
      // (left is negative)
      return 1;
    }
    return 0;
  }

  left = FixedMul ( node->dy>>FRACBITS , dx );
  right = FixedMul ( dy , node->dx>>FRACBITS );

  if (right < left)
  {
    // front side
    return 0;
  }
  // back side
  return 1;			
}


int R_PointOnSegSide( fixed_t	x,  fixed_t	y, seg_t*	line )
{
  fixed_t	lx;
  fixed_t	ly;
  fixed_t	ldx;
  fixed_t	ldy;
  fixed_t	dx;
  fixed_t	dy;
  fixed_t	left;
  fixed_t	right;

  lx = line->v1->x;
  ly = line->v1->y;

  ldx = line->v2->x - lx;
  ldy = line->v2->y - ly;

  if (!ldx)
  {
    if (x <= lx)
      return ldy > 0;

    return ldy < 0;
  }
  if (!ldy)
  {
    if (y <= ly)
      return ldx < 0;

    return ldx > 0;
  }

  dx = (x - lx);
  dy = (y - ly);

  // Try to quickly decide by looking at sign bits.
  if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
  {
    if  ( (ldy ^ dx) & 0x80000000 )
    {
      // (left is negative)
      return 1;
    }
    return 0;
  }

  left = FixedMul ( ldy>>FRACBITS , dx );
  right = FixedMul ( dy , ldx>>FRACBITS );

  if (right < left)
  {
    // front side
    return 0;
  }
  // back side
  return 1;			
}


//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.

//




angle_t R_PointToAngle( fixed_t	x,  fixed_t	y )
{	
  x -= viewx;
  y -= viewy;

  if ( (!x) && (!y) )
    return 0;

  if (x>= 0)
  {
    // x >=0
    if (y>= 0)
    {
      // y>= 0

      if (x>y)
      {
        // octant 0
        return tantoangle[ SlopeDiv(y,x)];
      }
      else
      {
        // octant 1
        return ANG90-1-tantoangle[ SlopeDiv(x,y)];
      }
    }
    else
    {
      // y<0
      y = -y;

      if (x>y)
      {
        // octant 8
        return -tantoangle[SlopeDiv(y,x)];
      }
      else
      {
        // octant 7
        return ANG270+tantoangle[ SlopeDiv(x,y)];
      }
    }
  }
  else
  {
    // x<0
    x = -x;

    if (y>= 0)
    {
      // y>= 0
      if (x>y)
      {
        // octant 3
        return ANG180-1-tantoangle[ SlopeDiv(y,x)];
      }
      else
      {
        // octant 2
        return ANG90+ tantoangle[ SlopeDiv(x,y)];
      }
    }
    else
    {
      // y<0
      y = -y;

      if (x>y)
      {
        // octant 4
        return ANG180+tantoangle[ SlopeDiv(y,x)];
      }
      else
      {
        // octant 5
        return ANG270-1-tantoangle[ SlopeDiv(x,y)];
      }
    }
  }
}


angle_t R_PointToAngle2( fixed_t	x1, fixed_t	y1, fixed_t	x2, fixed_t	y2 )
{	
  viewx = x1;
  viewy = y1;

  return R_PointToAngle (x2, y2);
}


fixed_t R_PointToDist( fixed_t	x, fixed_t	y )
{
  int		angle;
  fixed_t	dx;
  fixed_t	dy;
  fixed_t	temp;
  fixed_t	dist;

  dx = abs(x - viewx);
  dy = abs(y - viewy);

  if (dy>dx)
  {
    temp = dx;
    dx = dy;
    dy = temp;
  }

  /* CGDoom: clamp; dx == dy == 0 (view exactly on a vertex) made FixedDiv
     return MAXINT and read far out of tantoangle[] (vanilla bug) */
  {
    int slope = FixedDiv(dy,dx)>>DBITS;
    if (slope < 0 || slope > SLOPERANGE)
      slope = (slope < 0) ? 0 : SLOPERANGE;
    angle = (tantoangle[ slope ]+ANG90) >> ANGLETOFINESHIFT;
  }

  // use as cosine
  dist = FixedDiv (dx, finesine[angle] );	

  return dist;
}


//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
  fixed_t		scale;
  int			anglea;
  int			angleb;
  int			sinea;
  int			sineb;
  fixed_t		num;
  int			den;


  anglea = ANG90 + (visangle-viewangle);
  angleb = ANG90 + (visangle-rw_normalangle);

  // both sines are allways positive
  sinea = finesine[anglea>>ANGLETOFINESHIFT];	
  sineb = finesine[angleb>>ANGLETOFINESHIFT];
  num = FixedMul(projectiony,sineb)<<detailshift;
  den = FixedMul(rw_distance,sinea);

  if (den > num>>16)
  {
    scale = FixedDiv (num, den);

    if (scale > 64*FRACUNIT)
      scale = 64*FRACUNIT;
    else if (scale < 256)
      scale = 256;
  }
  else
    scale = 64*FRACUNIT;

  return scale;
}


//
// R_InitTextureMapping
//
void R_InitTextureMapping (void)
{
  int			i;
  int			x;
  int			t;
  fixed_t		focallength;

  // Use tangent table to generate viewangletox:
  //  viewangletox will give the next greatest x
  //  after the view angle.
  //
  // Calc focallength
  //  so FIELDOFVIEW angles covers SCREENWIDTH.
  // CGDoom: covers the non-widescreen width (projection); the angle range
  // grows with the wider view.
  focallength = FixedDiv (projection,
    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );

  for (i=0 ; i<FINEANGLES/2 ; i++)
  {
    if (finetangent[i] > FRACUNIT*2)
      t = -1;
    else if (finetangent[i] < -FRACUNIT*2)
      t = viewwidth+1;
    else
    {
      t = FixedMul (finetangent[i], focallength);
      t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

      if (t < -1)
        t = -1;
      else if (t>viewwidth+1)
        t = viewwidth+1;
    }
    viewangletox[i] = t;
  }

  // Scan viewangletox[] to generate xtoviewangle[]:
  //  xtoviewangle will give the smallest view angle
  //  that maps to x.	
  for (x=0;x<=viewwidth;x++)
  {
    i = 0;
    while (viewangletox[i]>x)
      i++;
    xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
  }

  // Take out the fencepost cases from viewangletox.
  for (i=0 ; i<FINEANGLES/2 ; i++)
  {
    t = FixedMul (finetangent[i], focallength);
    t = centerx - t;

    if (viewangletox[i] == -1)
      viewangletox[i] = 0;
    else if (viewangletox[i] == viewwidth+1)
      viewangletox[i]  = viewwidth;
  }

  clipangle = xtoviewangle[0];
}



//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP		2

void R_InitLightTables (void)
{
  int		i;
  int		j;
  int		level;
  int		startmap; 	
  int		scale;

  // Calculate the light levels to use
  //  for each level / distance combination.
  for (i=0 ; i< LIGHTLEVELS ; i++)
  {
    startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
    for (j=0 ; j<MAXLIGHTZ ; j++)
    {
      scale = FixedDiv ((ORIGWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
      scale >>= LIGHTSCALESHIFT;
      level = startmap - scale/DISTMAP;

      if (level < 0)
        level = 0;

      if (level >= NUMCOLORMAPS)
        level = NUMCOLORMAPS-1;

      zlight[i][j] = colormaps + level*256;
    }
  }
}



//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean		setsizeneeded;
int		setblocks;
int		setdetail;


void R_SetViewSize( int		blocks, int detail )
{
  setsizeneeded = true;
  setblocks = blocks;
  setdetail = detail;
}

extern int	detailLevel;
extern int	screenblocks;

/* CGDoom: run-time renderer settings (cgdoom.h). They update
   CGD_Options and request a view size change, so that the new drawers and
   tables are set up between two frames. */
void CGD_SetLowDetail (int on)
{
  CGD_Options.LowDetail = detailLevel = on ? 1 : 0;
  R_SetViewSize (screenblocks, detailLevel);
}

void CGD_SetViewSize (int blocks)
{
  if (blocks < 3)
    blocks = 3;
  if (blocks > 11)
    blocks = 11;
  CGD_Options.ViewSize = screenblocks = blocks;
  R_SetViewSize (screenblocks, detailLevel);
}

void V_Clear();

//
// R_ExecuteSetViewSize
//
void R_ExecuteSetViewSize (void)
{
  fixed_t	cosadj;
  fixed_t	dy;
  int		i;
  int		j;
  int		level;
  int		startmap; 	

  setsizeneeded = false;
  /* CGDoom-specific function */
  V_Clear();

  /* CGDoom: SBARHEIGHT rows for the status bar; the smaller sizes keep
     vanilla's proportions of the screen (blocks*32 x (blocks*168/10)&~7 on
     a 320x200 screen) and its field of view (viewwidth_nonwide) */
  if (setblocks >= 10)
  {
    scaledviewwidth = SCREENWIDTH;
    viewheight = setblocks == 11 ? SCREENHEIGHT : SCREENHEIGHT-SBARHEIGHT;
    viewwidth_nonwide = ORIGWIDTH;
  }
  else
  {
    scaledviewwidth = (setblocks*SCREENWIDTH/10)&~7;
    viewheight = (setblocks*(SCREENHEIGHT-SBARHEIGHT)/10)&~7;
    viewwidth_nonwide = setblocks*ORIGWIDTH/10;
  }

  /* CGDoom: low detail through vanilla's detailshift path */
  detailshift = setdetail ? 1 : 0;
  viewwidth = scaledviewwidth>>detailshift;
  viewwidth_nonwide >>= detailshift;

  centery = viewheight/2;
  centerx = viewwidth/2;
  centerxfrac = centerx<<FRACBITS;
  centeryfrac = centery<<FRACBITS;
  projection = (viewwidth_nonwide/2)<<FRACBITS;
  projectiony = (CGD_Features & CGD_FEAT_ASPECT)
    ? projection*ASPECT_NUM/ASPECT_DEN : projection;

  /* Drawer selection: the ONE place that picks the column/span drawers.
     Low detail uses Chocolate Doom's drawers (fuzz and translated included,
     where vanilla kept the full-detail ones), see r_draw.c.
     SH4 builds (CGD_SH4, r_draw.h: __sh__ and not CGD_NO_SH4) use the
     bit-exact *_SH4 assembly versions of src/engine/sh4 (README.md §7),
     assembled for the build's screen width; they need cgd_sh4_set_colormaps() (R_InitColormaps), else they fall
     back to C. */
#ifdef CGD_SH4
  if (!detailshift)
  {
    colfunc = basecolfunc = R_DrawColumn_SH4;
    fuzzcolfunc = R_DrawFuzzColumn_SH4;
    transcolfunc = R_DrawTranslatedColumn_SH4;
    spanfunc = R_DrawSpan_SH4;
  }
  else
  {
    colfunc = basecolfunc = R_DrawColumnLow_SH4;
    fuzzcolfunc = R_DrawFuzzColumnLow_SH4;
    transcolfunc = R_DrawTranslatedColumnLow_SH4;
    spanfunc = R_DrawSpanLow_SH4;
  }
#else
  if (!detailshift)
  {
    colfunc = basecolfunc = R_DrawColumn;
    fuzzcolfunc = R_DrawFuzzColumn;
    transcolfunc = R_DrawTranslatedColumn;
    spanfunc = R_DrawSpan;
  }
  else
  {
    colfunc = basecolfunc = R_DrawColumnLow;
    fuzzcolfunc = R_DrawFuzzColumnLow;
    transcolfunc = R_DrawTranslatedColumnLow;
    spanfunc = R_DrawSpanLow;
  }
#endif

  R_InitBuffer (scaledviewwidth, viewheight);

  R_InitTextureMapping ();

  // psprite scales
  pspritescale = FRACUNIT*viewwidth_nonwide/ORIGWIDTH;
  pspriteiscale = FRACUNIT*ORIGWIDTH/viewwidth_nonwide;
  pspritescaley = projectiony == projection ? pspritescale
    : FRACUNIT*viewwidth_nonwide*ASPECT_NUM/(ORIGWIDTH*ASPECT_DEN);
  pspriteiscaley = projectiony == projection ? pspriteiscale
    : FRACUNIT*ORIGWIDTH*ASPECT_DEN/(viewwidth_nonwide*ASPECT_NUM);

  /* CGDoom: the weapon is drawn around centery; it shows the same rows
     (down to the bottom of the view) as in vanilla's view of this size,
     whose height is vheight, with the vanilla scale: in vanilla, the weapon
     rows below BASEYCENTER that fit are (vheight/2) / scale; here
     (viewheight-centery) / scaley. Moved by the difference (0 when the
     screen and scales are vanilla's). */
  {
    int vheight = setblocks >= 11 ? ORIGHEIGHT
      : (setblocks*(ORIGHEIGHT-SBARHEIGHT)/10)&~7;
    pspriteshift = FixedDiv ((vheight/2)<<FRACBITS, pspritescale<<detailshift)
      - FixedDiv ((viewheight-centery)<<FRACBITS,
        pspritescaley<<detailshift);
  }

  // thing clipping
  for (i=0 ; i<viewwidth ; i++)
    screenheightarray[i] = (short)viewheight;

  // planes
  for (i=0 ; i<viewheight ; i++)
  {
    dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;
    dy = abs(dy);
    yslope[i] = FixedDiv ( projectiony<<detailshift, dy);
  }

  for (i=0 ; i<viewwidth ; i++)
  {
    cosadj = abs(finecosine(xtoviewangle[i]>>ANGLETOFINESHIFT));
    distscale[i] = FixedDiv (FRACUNIT,cosadj);
  }

  // Calculate the light levels to use
  //  for each level / scale combination.
  for (i=0 ; i< LIGHTLEVELS ; i++)
  {
    startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
    for (j=0 ; j<MAXLIGHTSCALE ; j++)
    {
      /* CGDoom: j is a vertical scale, larger by ASPECT_NUM/ASPECT_DEN
         with the aspect-ratio correction: the same light at the same
         distance */
      if (projectiony == projection)
        level = startmap - j*ORIGWIDTH/(viewwidth_nonwide<<detailshift)/DISTMAP;
      else
        level = startmap - j*ORIGWIDTH*ASPECT_DEN
          /((viewwidth_nonwide<<detailshift)*ASPECT_NUM)/DISTMAP;

      if (level < 0)
        level = 0;

      if (level >= NUMCOLORMAPS)
        level = NUMCOLORMAPS-1;

      scalelight[i][j] = colormaps + level*256;
    }
  }
}



//
// R_Init
//



void R_Init (void)
{
  printf ("R_InitData\n");
  R_InitData ();
  /* CGDoom: view size and detail from the options (vanilla read
     them from the config file defaults). screenblocks is 3..11 as in
     vanilla (11 = full screen, no status bar; CGDoom's default is 10). */
  screenblocks = CGD_Options.ViewSize;
  if (screenblocks < 3 || screenblocks > 11)
    screenblocks = 10;
  detailLevel = CGD_Options.LowDetail ? 1 : 0;
  R_SetViewSize (screenblocks, detailLevel);
  printf ("R_InitPlanes\n");
  R_InitPlanes ();
  printf ("R_InitLightTables\n");
  R_InitLightTables ();
  printf ("R_InitSkyMap\n");
  R_InitSkyMap ();
  printf ("R_InitTranslationsTables\n");
  R_InitTranslationTables ();

  framecount = 0;
}


//
// R_PointInSubsector
//
subsector_t* R_PointInSubsector( fixed_t	x,  fixed_t	y )
{
  node_t*	node;
  int		side;
  int		nodenum;

  // single subsector is a special case
  if (!numnodes)				
    return subsectors;

  nodenum = numnodes-1;

  while (! (nodenum & NF_SUBSECTOR) )
  {
    node = &nodes[nodenum];
    side = R_PointOnSide (x, y, node);
    nodenum = node->children[side];
  }

  return &subsectors[nodenum & ~NF_SUBSECTOR];
}



//
// R_SetupFrame
//
void R_SetupFrame (player_t* player)
{		
  int		i;

  viewplayer = player;
  viewx = player->mo->x;
  viewy = player->mo->y;
  viewangle = player->mo->angle + viewangleoffset;
  extralight = player->extralight;

  viewz = player->viewz;

  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine(viewangle>>ANGLETOFINESHIFT);//finecosine[viewangle>>ANGLETOFINESHIFT];

  sscount = 0;

  if (player->fixedcolormap)
  {
    fixedcolormap = colormaps + player->fixedcolormap*256;

    walllights = scalelightfixed;

    for (i=0 ; i<MAXLIGHTSCALE ; i++)
      scalelightfixed[i] = fixedcolormap;
  }
  else
    fixedcolormap = 0;

  framecount++;
  validcount++;
}



#if CGD_EXTRAS
//
// R_DrawCrosshair
// CGDoom: CGD_FEAT_CROSSHAIR. Four arms of two pixels around an open centre,
// at the centre of the view (where a shot goes when autoaim finds nothing
// above or below), in the palette's pure yellow: much brighter than red,
// which vanishes on demons, blood, red walls, the pain tint and the
// pistol's flash. Drawn over the weapon.
//
static void R_DrawCrosshair (void)
{
  static const signed char arm[8][2] = {
    {-3,0}, {-2,0}, {2,0}, {3,0}, {0,-3}, {0,-2}, {0,2}, {0,3},
  };
  int cx = viewwindowx + scaledviewwidth/2;
  int cy = viewwindowy + viewheight/2;
  byte yellow = 231;

  for (int i = 0; i < 8; i++)
    screens[0][(cy + arm[i][1])*SCREENWIDTH + cx + arm[i][0]] = yellow;
}
#endif

//
// R_RenderView
// Main 3D render function, where all the accursed gypsy magic happens
//
void R_RenderPlayerView (player_t* player)
{
  R_SetupFrame (player);
  // Clear buffers.
  R_ClearClipSegs ();
  R_ClearDrawSegs ();
  R_ClearPlanes ();
  R_ClearSprites ();

  // The head node is the last node output.
  R_RenderBSPNode (numnodes-1);

  R_DrawPlanes ();

  R_DrawMasked ();
  //I_Error("poof");

#if CGD_EXTRAS
  if (CGD_Features & CGD_FEAT_CROSSHAIR)
    R_DrawCrosshair ();
#endif
}
