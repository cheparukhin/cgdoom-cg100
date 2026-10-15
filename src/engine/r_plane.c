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
//	Here is a core component: drawing the floors and ceilings,
//	 while maintaining a per column clipping list only.
//	Moreover, the sky areas have to be determined.
//
//-----------------------------------------------------------------------------


//static const char 


#include "os.h"
#include "cgdoom.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"
#include "r_bmaps.h"



planefunction_t			floorfunc;
planefunction_t			ceilingfunc;

//
// opening
//

// Here comes the obnoxious "visplane".
/* CGDoom: renderer limits.

   Visplanes are allocated from the zone (PU_STATIC) in blocks of
   VISPLANE_BLOCK as a frame needs them, up to MAXVISPLANES (vanilla: a static
   array of 128 and "R_FindPlane: no more visplanes"). Blocks are kept for
   the next frames and visplanes never move, so floorplane/ceilingplane stay
   valid. VISPLANE(i) is the i-th visplane created this frame: R_DrawPlanes
   draws them in vanilla's order.

   R_FindPlane looks a plane up in a hash table instead of scanning every
   visplane. Each bucket chain is kept in creation order (planes are
   appended), and every plane of a given (height, picnum, lightlevel) is in
   the same bucket, so the first match in the chain is the first match of
   vanilla's linear scan: the same plane is returned.

   Openings (sprite clip and masked-column arrays) come in blocks of
   OPENINGS_BLOCK shorts (vanilla's MAXOPENINGS, which had no overflow check
   at all). R_StoreWallRange reserves the most it can use before drawing a
   wall (R_ReserveOpenings); when the current block cannot hold it the next
   block is used. Drawsegs keep pointers into the blocks, so blocks never
   move either.

   Growth never changes the output: a visplane, drawseg or vissprite slot
   keeps its storage once allocated and starts zeroed, exactly like the slot
   of vanilla's zero-initialized static array (the stale contents matter for
   visplanes, see R_AllocZeroed), and openings are always written before they
   are read. CGD_SmallRenderLimits (host -rlimits small) starts every array
   tiny so that the tests run the growth paths and check exactly this. */
#define VISPLANE_BLOCK		64
#define MAXVISPLANES		1024
#define VISPLANE(i)		(&visplane_blocks[(i) / VISPLANE_BLOCK][(i) % VISPLANE_BLOCK])
#define VISPLANE_HASH		64	/* power of 2 */

static visplane_t*			visplane_blocks[MAXVISPLANES / VISPLANE_BLOCK];
static int				visplanes_allocated;	/* in the blocks */
static int				numvisplanes;		/* used this frame */
static visplane_t*			visplane_head[VISPLANE_HASH];
static visplane_t*			visplane_tail[VISPLANE_HASH];
visplane_t*				floorplane;
visplane_t*				ceilingplane;

#define OPENINGS_BLOCK		(SCREENWIDTH*64)
#define OPENINGS_BLOCK_SMALL	(3*SCREENWIDTH)	/* one wall's worth */
#define MAXOPENINGS_BLOCKS	16
static int				openings_block_size = OPENINGS_BLOCK;
static short*				openings_blocks[MAXOPENINGS_BLOCKS];
static int				openings_allocated;	/* blocks */
static int				openings_block;		/* current block */
static int				openings_used;		/* in the previous blocks */
short*					openings_end;
short*					lastopening;


//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
short					floorclip[SCREENWIDTH];
short					ceilingclip[SCREENWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
int						spanstart[SCREENHEIGHT];
int						spanstop[SCREENHEIGHT];

//
// texture mapping
//
const zlight_t*				planezlight;
fixed_t					planeheight;

fixed_t					yslope[SCREENHEIGHT];
fixed_t					distscale[SCREENWIDTH];
fixed_t					basexscale;
fixed_t					baseyscale;

fixed_t					cachedheight[SCREENHEIGHT];
fixed_t					cacheddistance[SCREENHEIGHT];
fixed_t					cachedxstep[SCREENHEIGHT];
fixed_t					cachedystep[SCREENHEIGHT];



//
// R_AllocZeroed
// CGDoom: a zero-filled PU_STATIC zone block, for the renderer arrays that
// were static (zero-initialized) in vanilla. The zeros matter: R_DrawPlanes
// reads bottom[] of columns a visplane does not cover (and the pads), which
// vanilla left as zeros or as earlier frames' valid rows.
//
void* R_AllocZeroed (int size)
{
  void *p = Z_Malloc (size, PU_STATIC, NULL);
  memset (p, 0, size);
  CGD_Stats.RenderArrayBytes += size;
  if (CGD_Stats.RenderArrayBytes > CGD_Stats.RenderArrayBytesPeak)
    CGD_Stats.RenderArrayBytesPeak = CGD_Stats.RenderArrayBytes;
  return p;
}

/* CGDoom: frees an R_AllocZeroed block of the given size */
void R_FreeArray (void *p, int size)
{
  Z_Free (p);
  CGD_Stats.RenderArrayBytes -= size;
}

/* CGDoom test knob: see the comment at the top of this file */
int CGD_SmallRenderLimits = 0;


//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
  /* CGDoom: the first visplane and openings blocks (vanilla's static
     limits); more are allocated on demand */
  int i, n = CGD_SmallRenderLimits ? 1 : 128 / VISPLANE_BLOCK;

  for (i = 0; i < n; i++)
    visplane_blocks[i] = R_AllocZeroed (VISPLANE_BLOCK * sizeof(visplane_t));
  visplanes_allocated = n * VISPLANE_BLOCK;

  if (CGD_SmallRenderLimits)
    openings_block_size = OPENINGS_BLOCK_SMALL;
  openings_blocks[0] = R_AllocZeroed (openings_block_size * sizeof(short));
  openings_allocated = 1;
}


//
// R_NextOpeningsBlock
// CGDoom: R_ReserveOpenings' slow path (r_plane.h): moves lastopening to the
// next block (allocated if needed). Returns false when MAXOPENINGS_BLOCKS are
// full (the caller then skips the wall).
//
boolean R_NextOpeningsBlock (int n)
{
  if (openings_block + 1 >= MAXOPENINGS_BLOCKS || n > openings_block_size)
    return false;

  openings_used += lastopening - openings_blocks[openings_block];
  openings_block++;
  if (openings_block == openings_allocated)
  {
    openings_blocks[openings_block] = R_AllocZeroed (openings_block_size *
      sizeof(short));
    openings_allocated++;
  }
  lastopening = openings_blocks[openings_block];
  openings_end = lastopening + openings_block_size;
  return true;
}


//
// R_NewVisplane
// CGDoom: the next visplane of this frame, with its key set and appended to
// its hash chain.
//
static unsigned R_VisplaneHash (fixed_t height, int picnum, int lightlevel)
{
  return ((unsigned)picnum * 3 + (unsigned)lightlevel
    + ((unsigned)height >> FRACBITS) * 7) & (VISPLANE_HASH - 1);
}

static visplane_t* R_NewVisplane (fixed_t height, int picnum, int lightlevel)
{
  visplane_t*	pl;
  unsigned	h;

  if (numvisplanes == visplanes_allocated)
  {
    if (visplanes_allocated == MAXVISPLANES)
      I_Error ("R_FindPlane: no more visplanes");
    visplane_blocks[visplanes_allocated / VISPLANE_BLOCK] = R_AllocZeroed (
      VISPLANE_BLOCK * sizeof(visplane_t));
    visplanes_allocated += VISPLANE_BLOCK;
  }

  pl = VISPLANE(numvisplanes);
  numvisplanes++;

  pl->height = height;
  pl->picnum = picnum;
  pl->lightlevel = lightlevel;
  pl->next = NULL;

  h = R_VisplaneHash (height, picnum, lightlevel);
  if (visplane_head[h])
    visplane_tail[h]->next = pl;
  else
    visplane_head[h] = pl;
  visplane_tail[h] = pl;

  return pl;
}


//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
void R_MapPlane( int y, int	x1,  int x2 )
{
  angle_t	angle;
  fixed_t	distance;
  fixed_t	length;
  unsigned	index;

#ifdef RANGECHECK
  if (x2 < x1
    || x1<0
    || x2>=viewwidth
    || (unsigned)y>(unsigned)viewheight)
  {
    I_Error ("R_MapPlane: %i, %i at %i",x1,x2,y);
  }
#endif

  if (planeheight != cachedheight[y])
  {
    cachedheight[y] = planeheight;
    distance = cacheddistance[y] = FixedMul (planeheight, yslope[y]);
    ds_xstep = cachedxstep[y] = FixedMul (distance,basexscale);
    ds_ystep = cachedystep[y] = FixedMul (distance,baseyscale);
  }
  else
  {
    distance = cacheddistance[y];
    ds_xstep = cachedxstep[y];
    ds_ystep = cachedystep[y];
  }

  length = FixedMul (distance,distscale[x1]);
  angle = (viewangle + xtoviewangle[x1])>>ANGLETOFINESHIFT;
  ds_xfrac = viewx + FixedMul(finecosine(angle), length);
  ds_yfrac = -viewy - FixedMul(finesine[angle], length);

  if (fixedcolormap)
    ds_colormap = fixedcolormap;
  else
  {
    index = distance >> LIGHTZSHIFT;

    if (index >= MAXLIGHTZ )
      index = MAXLIGHTZ-1;

    ds_colormap = ZLIGHT_MAP(planezlight[index]);
  }

  ds_y = y;
  ds_x1 = x1;
  ds_x2 = x2;

  // high or low detail
  spanfunc ();	
}


//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes (void)
{
  int		i;
  angle_t	angle;

  // opening / clipping determination
  for (i=0 ; i<viewwidth ; i++)
  {
    floorclip[i] = (short)viewheight;
    ceilingclip[i] = -1;
  }

  numvisplanes = 0;
  memset (visplane_head, 0, sizeof(visplane_head));

  openings_block = 0;
  openings_used = 0;
  lastopening = openings_blocks[0];
  openings_end = lastopening + openings_block_size;

  // texture calculation
  memset (cachedheight, 0, sizeof(cachedheight));

  // left to right mapping
  angle = (viewangle-ANG90)>>ANGLETOFINESHIFT;

  // scale will be unit scale at SCREENWIDTH/2 distance
  // CGDoom: at the focal length (projection; centerxfrac in vanilla, which
  // is the same but in widescreen)
  basexscale = FixedDiv (finecosine(angle),projection);
  baseyscale = -FixedDiv (finesine[angle],projection);
}




//
// R_FindPlane
//
visplane_t* R_FindPlane( fixed_t	height, int	picnum, int	lightlevel )
{
  visplane_t*	check;

  if (picnum == skyflatnum)
  {
    height = 0;			// all skys map together
    lightlevel = 0;
  }

  /* CGDoom: hash chain in creation order = vanilla's linear scan */
  for (check = visplane_head[R_VisplaneHash (height, picnum, lightlevel)];
    check; check = check->next)
  {
    if (height == check->height
      && picnum == check->picnum
      && lightlevel == check->lightlevel)
    {
      return check;
    }
  }

  check = R_NewVisplane (height, picnum, lightlevel);
  check->minx = SCREENWIDTH;
  check->maxx = -1;

  memset (check->top,0xff,sizeof(check->top));

  return check;
}


//
// R_CheckPlane
//
visplane_t* R_CheckPlane( visplane_t*	pl, int	start, int	stop )
{
  int		intrl;
  int		intrh;
  int		unionl;
  int		unionh;
  int		x;

  if (start < pl->minx)
  {
    intrl = pl->minx;
    unionl = start;
  }
  else
  {
    unionl = pl->minx;
    intrl = start;
  }

  if (stop > pl->maxx)
  {
    intrh = pl->maxx;
    unionh = stop;
  }
  else
  {
    unionh = pl->maxx;
    intrh = stop;
  }

  for (x=intrl ; x<= intrh ; x++)
    if (pl->top[x] != 0xff)
      break;

  if (x > intrh)
  {
    pl->minx = unionl;
    pl->maxx = unionh;

    // use the same one
    return pl;		
  }

  // make a new visplane
  pl = R_NewVisplane (pl->height, pl->picnum, pl->lightlevel);
  pl->minx = start;
  pl->maxx = stop;

  memset (pl->top,0xff,sizeof(pl->top));

  return pl;
}


//
// R_MakeSpans
//
void R_MakeSpans( int	x, int	t1, int	b1, int	t2, int	b2 )
{
  while (t1 < t2 && t1<=b1)
  {
    R_MapPlane (t1,spanstart[t1],x-1);
    t1++;
  }
  while (b1 > b2 && b1>=t1)
  {
    R_MapPlane (b1,spanstart[b1],x-1);
    b1--;
  }

  while (t2 < t1 && t2<=b2)
  {
    spanstart[t2] = x;
    t2++;
  }
  while (b2 > b1 && b2>=t2)
  {
    spanstart[b2] = x;
    b2--;
  }
}



//
// R_DrawPlanes
// At the end of each frame.
//
/* CGDoom: reads the visplane pads top[minx-1] and top[maxx+1] by design */
CGD_NO_SANITIZE_BOUNDS void R_DrawPlanes (void)
{
  visplane_t*		pl;
  int				light;
  int				x;
  int				stop;
  int				angle;
#if CGD_EXTRAS
  /* CGDoom: brightmaps (CGD_FEAT_BRIGHTMAPS, r_bmaps.h): a flat that has
     one is drawn by R_DrawBrightSpan, the others by spanfunc */
  void			(* const basespanfunc) (void) = spanfunc;
  const boolean		bright = (CGD_Features & CGD_FEAT_BRIGHTMAPS)
			  && !fixedcolormap;
#endif

  /* CGDoom: renderer limit statistics (the BSP walk is done: drawsegs,
     visplanes, vissprites and openings are final for this frame) */
  {
    int used = openings_used + (int)(lastopening - openings_blocks[openings_block]);
    int nds = (int)(ds_p - drawsegs);
    int nvs = (int)(vissprite_p - vissprites);
    if (numvisplanes > CGD_Stats.VisplanesPeak)
      CGD_Stats.VisplanesPeak = numvisplanes;
    if (used > CGD_Stats.OpeningsPeak)
      CGD_Stats.OpeningsPeak = used;
    if (nds > CGD_Stats.DrawsegsPeak)
      CGD_Stats.DrawsegsPeak = nds;
    if (nvs > CGD_Stats.VisspritesPeak)
      CGD_Stats.VisspritesPeak = nvs;
    if (numvisplanes > 128 || nds > 256 || nvs > 128 || used > SCREENWIDTH*64)
      CGD_Stats.FramesOverVanillaLimits++;
  }

#ifdef RANGECHECK
  if (lastopening > openings_end)
    I_Error ("R_DrawPlanes: opening overflow (%i)",
    (int)(lastopening - openings_end));
#endif

  pl = NULL;
  for (int i = 0 ; i < numvisplanes ; i++)
  {
    /* CGDoom: VISPLANE(i), walking each block */
    pl = (i % VISPLANE_BLOCK) ? pl + 1 : visplane_blocks[i / VISPLANE_BLOCK];
    if (pl->minx > pl->maxx)
      continue;


    // sky flat
    if (pl->picnum == skyflatnum)
    {
      dc_iscale = pspriteiscaley>>detailshift;

      // Sky is allways drawn full bright,
      //  i.e. colormaps[0] is used.
      // Because of this hack, sky is not affected
      //  by INVUL inverse mapping.
      dc_colormap = colormaps;
      dc_texturemid = skytexturemid;
      for (x=pl->minx ; x <= pl->maxx ; x++)
      {
        dc_yl = pl->top[x];
        dc_yh = pl->bottom[x];

        if (dc_yl <= dc_yh)
        {
          angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
          dc_x = x;
          dc_source = R_GetColumn(skytexture, angle);
          colfunc ();
        }
      }
      continue;
    }

    // regular flat
    ds_source = (const byte*)W_CacheLumpNumConst(firstflat +flattranslation[pl->picnum],PU_STATIC);
#if CGD_EXTRAS
    if (bright
      && (ds_brightmap = R_FlatBrightmap (flattranslation[pl->picnum])))
      spanfunc = detailshift ? R_DrawBrightSpanLow : R_DrawBrightSpan;
    else
      spanfunc = basespanfunc;
#endif

    planeheight = abs(pl->height-viewz);
    light = (pl->lightlevel >> LIGHTSEGSHIFT)+extralight*LIGHTBRIGHT;

    if (light >= LIGHTLEVELS)
      light = LIGHTLEVELS-1;

    if (light < 0)
      light = 0;

    planezlight = zlight[light];

    pl->top[pl->maxx+1] = 0xff;
    pl->top[pl->minx-1] = 0xff;

    stop = pl->maxx + 1;

    for (x=pl->minx ; x<= stop ; x++)
    {
      R_MakeSpans(x,pl->top[x-1],pl->bottom[x-1],pl->top[x],pl->bottom[x]);
    }

    Z_ChangeTag (ds_source, PU_CACHE);
  }
#if CGD_EXTRAS
  spanfunc = basespanfunc;
#endif
}
