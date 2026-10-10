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
//	All the clipping: columns, horizontal spans, sky columns.
//
//-----------------------------------------------------------------------------


//static const char 


#include "os.h"
#include "cgdoom.h"

#include "i_system.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"


// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
boolean		segtextured;	

// False if the back side is the same plane.
boolean		markfloor;	
boolean		markceiling;

boolean		maskedtexture;
int			toptexture;
int			bottomtexture;
int			midtexture;


angle_t		rw_normalangle;
// angle to line origin
int			rw_angle1;	

//
// regular wall
//
int				rw_x;
int				rw_stopx;
angle_t			rw_centerangle;
fixed_t			rw_offset;
fixed_t			rw_distance;
fixed_t			rw_scale;
fixed_t			rw_scalestep;
fixed_t			rw_midtexturemid;
fixed_t			rw_toptexturemid;
fixed_t			rw_bottomtexturemid;

int				worldtop;
int				worldbottom;
int				worldhigh;
int				worldlow;

fixed_t			pixhigh;
fixed_t			pixlow;
fixed_t			pixhighstep;
fixed_t			pixlowstep;

fixed_t			topfrac;
fixed_t			topstep;

fixed_t			bottomfrac;
fixed_t			bottomstep;


const lighttable_t**	walllights;

short*			maskedtexturecol;

//
// R_RenderMaskedSegRange
//
void R_RenderMaskedSegRange( drawseg_t*	ds, int		x1, int		x2 )
{
  unsigned	index;
  const column_t*	col;
  int		lightnum;
  int		texnum;

  // Calculate light table.
  // Use different light tables
  //   for horizontal / vertical / diagonal. Diagonal?
  // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
  curline = ds->curline;
  frontsector = curline->frontsector;
  backsector = curline->backsector;
  texnum = texturetranslation[curline->sidedef->midtexture];

  lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

  if (curline->v1->y == curline->v2->y)
    lightnum--;
  else if (curline->v1->x == curline->v2->x)
    lightnum++;

  if (lightnum < 0)		
    walllights = scalelight[0];
  else if (lightnum >= LIGHTLEVELS)
    walllights = scalelight[LIGHTLEVELS-1];
  else
    walllights = scalelight[lightnum];

  maskedtexturecol = ds->maskedtexturecol;

  rw_scalestep = ds->scalestep;		
  spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
  mfloorclip = ds->sprbottomclip;
  mceilingclip = ds->sprtopclip;

  // find positioning
  if (curline->linedef->flags & ML_DONTPEGBOTTOM)
  {
    dc_texturemid = frontsector->floorheight > backsector->floorheight
      ? frontsector->floorheight : backsector->floorheight;
    dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
  }
  else
  {
    dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight
      ? frontsector->ceilingheight : backsector->ceilingheight;
    dc_texturemid = dc_texturemid - viewz;
  }
  dc_texturemid += curline->sidedef->rowoffset;

  if (fixedcolormap)
    dc_colormap = fixedcolormap;

  // draw the columns
  for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
  {
    // calculate lighting
    if (maskedtexturecol[dc_x] != MAXSHORT)
    {
      if (!fixedcolormap)
      {
        index = spryscale>>LIGHTSCALESHIFT;

        if (index >=  MAXLIGHTSCALE )
          index = MAXLIGHTSCALE-1;

        dc_colormap = walllights[index];
      }

      sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
      dc_iscale = 0xffffffffu / (unsigned)spryscale;

      // draw the texture
      col = (const column_t *)( (const byte *)R_GetColumn(texnum,maskedtexturecol[dc_x]) -3);

      R_DrawMaskedColumn (col);
      maskedtexturecol[dc_x] = MAXSHORT;
    }
    spryscale += rw_scalestep;
  }

}




//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
#define HEIGHTBITS		12
#define HEIGHTUNIT		(1<<HEIGHTBITS)

void R_RenderSegLoop (void)
{
  angle_t		angle;
  unsigned	index;
  int			yl;
  int			yh;
  int			mid;
  fixed_t		texturecolumn;
  int			top;
  int			bottom;

  /* CGDoom (step 2): the loop state is kept in locals. As globals, the
     compiler had to reload and store all of them around every R_DrawColumn
     call (about a third of this loop's instructions). Nothing called from the
     loop (the column drawers, R_GetColumn and the zone/WAD code below it)
     reads or writes these globals, so this is the same computation; the
     globals the loop advances are written back at the end. */
  int			x = rw_x;
  const int		stopx = rw_stopx;
  fixed_t		scale = rw_scale;
  const fixed_t		scalestep = rw_scalestep;
  fixed_t		tfrac = topfrac;
  const fixed_t		tstep = topstep;
  fixed_t		bfrac = bottomfrac;
  const fixed_t		bstep = bottomstep;
  fixed_t		phigh = pixhigh;
  const fixed_t		phighstep = pixhighstep;
  fixed_t		plow = pixlow;
  const fixed_t		plowstep = pixlowstep;
  const boolean		mceiling = markceiling;
  const boolean		mfloor = markfloor;
  const boolean		textured = segtextured;
  const boolean		masked = maskedtexture;
  const int		midtex = midtexture;
  const int		toptex = toptexture;
  const int		bottomtex = bottomtexture;
  visplane_t* const	cplane = ceilingplane;
  visplane_t* const	fplane = floorplane;
  const lighttable_t** const lights = walllights;
  const angle_t		centerangle = rw_centerangle;
  const fixed_t		offset = rw_offset;
  const fixed_t		distance = rw_distance;
  const fixed_t		midtexturemid = rw_midtexturemid;
  const fixed_t		toptexturemid = rw_toptexturemid;
  const fixed_t		bottomtexturemid = rw_bottomtexturemid;
  short* const		mtcol = maskedtexturecol;
  const short		vheight = (short)viewheight;
  boolean		lit;

  /* Lighting and per-column drawer state, once per column before the first
     tier it draws (textured is always true when a tier is drawn). */
#define SEG_COLUMN_SETUP()						\
  do {									\
    if (!lit)								\
    {									\
      index = scale>>LIGHTSCALESHIFT;					\
      if (index >=  MAXLIGHTSCALE )					\
        index = MAXLIGHTSCALE-1;					\
      dc_colormap = lights[index];					\
      dc_x = x;								\
      dc_iscale = 0xffffffffu / (unsigned)scale;			\
      lit = true;							\
    }									\
  } while (0)

  for ( ; x < stopx ; x++)
  {
    // mark floor / ceiling areas
    yl = (tfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

    // no space above wall?
    if (yl < ceilingclip[x]+1)
      yl = ceilingclip[x]+1;

    if (mceiling)
    {
      top = ceilingclip[x]+1;
      bottom = yl-1;

      if (bottom >= floorclip[x])
        bottom = floorclip[x]-1;

      if (top <= bottom)
      {
        cplane->top[x] = (byte)top;
        cplane->bottom[x] = (byte)bottom;
      }
    }

    yh = bfrac>>HEIGHTBITS;

    if (yh >= floorclip[x])
      yh = floorclip[x]-1;

    if (mfloor)
    {
      top = yh+1;
      bottom = floorclip[x]-1;
      if (top <= ceilingclip[x])
        top = ceilingclip[x]+1;
      if (top <= bottom)
      {
        fplane->top[x] = (byte)top;
        fplane->bottom[x] = (byte)bottom;
      }
    }

    // texturecolumn is independent of wall tiers
    if (textured)
    {
      // calculate texture offset
      angle = (centerangle + xtoviewangle[x])>>ANGLETOFINESHIFT;
      texturecolumn = offset-FixedMul(finetangent[angle],distance);
      texturecolumn >>= FRACBITS;
    }
    else
    {
      // purely to shut up the compiler

      texturecolumn = 0;
    }
    /* CGDoom (polish): the lighting and dc_x/dc_iscale are set up only
       before the first tier this column actually draws (SEG_COLUMN_SETUP).
       Every other reader of dc_colormap, dc_x and dc_iscale (masked segs,
       sprites, sky) sets them itself first, and the values at each colfunc
       call are vanilla's, so the frames are the same; a column that draws
       nothing skips the light lookup and the division. */
    lit = false;

    // draw the wall tiers
    if (midtex)
    {
      // single sided line
      /* CGDoom (polish): vanilla calls R_GetColumn and colfunc even when
         yh < yl (26% of the column calls in e1m1); every column drawer
         returns at once for dc_yh < dc_yl (count < 0), so skipping both
         draws the same frame (only the texture cache traffic differs).
         yl and yh are within +-2^20, so yh >= yl is exactly count >= 0.
         The top and bottom tiers below were already guarded (mid >= yl,
         mid <= yh). */
      if (yh >= yl)
      {
        SEG_COLUMN_SETUP();
        dc_yl = yl;
        dc_yh = yh;
        dc_texturemid = midtexturemid;
        dc_source = R_GetColumn(midtex,texturecolumn);
        colfunc ();
      }
      ceilingclip[x] = vheight;
      floorclip[x] = -1;
    }
    else
    {
      // two sided line
      if (toptex)
      {
        // top wall
        mid = phigh>>HEIGHTBITS;
        phigh += phighstep;

        if (mid >= floorclip[x])
          mid = floorclip[x]-1;

        if (mid >= yl)
        {
          SEG_COLUMN_SETUP();
          dc_yl = yl;
          dc_yh = mid;
          dc_texturemid = toptexturemid;
          dc_source = R_GetColumn(toptex,texturecolumn);
          colfunc ();
          ceilingclip[x] = (short)mid;
        }
        else
          ceilingclip[x] = (short)(yl-1);
      }
      else
      {
        // no top wall
        if (mceiling)
          ceilingclip[x] = (short)(yl-1);
      }

      if (bottomtex)
      {
        // bottom wall
        mid = (plow+HEIGHTUNIT-1)>>HEIGHTBITS;
        plow += plowstep;

        // no space above wall?
        if (mid <= ceilingclip[x])
          mid = ceilingclip[x]+1;

        if (mid <= yh)
        {
          SEG_COLUMN_SETUP();
          dc_yl = mid;
          dc_yh = yh;
          dc_texturemid = bottomtexturemid;
          dc_source = R_GetColumn(bottomtex, texturecolumn);
          colfunc ();
          floorclip[x] = (short)mid;
        }
        else
          floorclip[x] = (short)(yh+1);
      }
      else
      {
        // no bottom wall
        if (mfloor)
          floorclip[x] = (short)(yh+1);
      }

      if (masked)
      {
        // save texturecol
        //  for backdrawing of masked mid texture
        mtcol[x] = (short)texturecolumn;
      }
    }

    scale += scalestep;
    tfrac += tstep;
    bfrac += bstep;
  }

#undef SEG_COLUMN_SETUP

  rw_x = x;
  rw_scale = scale;
  topfrac = tfrac;
  bottomfrac = bfrac;
  pixhigh = phigh;
  pixlow = plow;
}




/* CGDoom (step 2): copy n clip shorts (vanilla used memcpy). The copies are
   short (often a few columns); fxlibc's memcpy does sizes below 64 bytes
   byte by byte, and lastopening is only 2-byte aligned. */
static inline void R_CopyClip (short *dest, const short *src, int n)
{
  for ( ; n > 0; n--)
    *dest++ = *src++;
}

//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void R_StoreWallRange ( int start, int stop )
{
  fixed_t		hyp;
  fixed_t		sineval;
  angle_t		distangle, offsetangle;
  fixed_t		vtop;
  int			lightnum;

  // don't overflow and crash
  /* CGDoom: drawsegs and openings grow on demand (B10); at their raised
     limits the wall is dropped as vanilla did at 256 drawsegs */
  if (ds_p == drawsegs_end && !R_GrowDrawSegs ())
  {
    CGD_Stats.DrawsegsDropped++;
    return;
  }
  if (!R_ReserveOpenings (3 * (stop - start + 1)))
  {
    CGD_Stats.DrawsegsDropped++;
    return;
  }

#ifdef RANGECHECK
  if (start >=viewwidth || start > stop)
    I_Error ("Bad R_RenderWallRange: %i to %i", start , stop);
#endif

  sidedef = curline->sidedef;
  linedef = curline->linedef;

  // mark the segment as visible for auto map
  linedef->flags |= ML_MAPPED;

  // calculate rw_distance for scale calculation
  rw_normalangle = curline->angle + ANG90;
  offsetangle = rw_normalangle-rw_angle1;
  //HAX HAX HAX
  //offsetangle = abs(rw_normalangle-rw_angle1); OFFSETANGLE IS UNSIGNED
  /*if (offsetangle > MAXINT)
  offsetangle= rw_angle1-rw_normalangle;*/

  /*if (offsetangle > ANG90)
  printf("offsetangle_precorrect= %i \n",offsetangle>>ANGLETOFINESHIFT);
  offsetangle = ANG90;*/

  if (offsetangle > ANG180)
    offsetangle = -offsetangle;	

  if (offsetangle > ANG90)
    offsetangle = ANG90;

  distangle = ANG90 - offsetangle;
  hyp = R_PointToDist (curline->v1->x, curline->v1->y);
  sineval = finesine[distangle>>ANGLETOFINESHIFT];
  rw_distance = FixedMul (hyp, sineval);

  //if (start==141 && stop==194)
  /*if (start==252 && stop==274)
  {
  printf("rw_angle1= %i \n",rw_angle1>>ANGLETOFINESHIFT);
  printf("curline->angle= %i \n",curline->angle>>ANGLETOFINESHIFT);
  printf("rw_normalangle= %i \n",rw_normalangle>>ANGLETOFINESHIFT);
  printf("offsetangle= %i \n",offsetangle>>ANGLETOFINESHIFT);
  printf("distangle= %i \n",distangle>>ANGLETOFINESHIFT);
  printf("hyp= %i, sineval= %i \n",hyp>>16,sineval);
  printf("rw_dist= %i, \n",rw_distance>>16);
  }*/
  ds_p->x1 = rw_x = start;
  ds_p->x2 = stop;
  ds_p->curline = curline;
  rw_stopx = stop+1;

  // calculate scale at both ends and step
  ds_p->scale1 = rw_scale = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

  if (stop > start )
  {
    ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
    /* CGDoom: (scale2 - rw_scale) / (stop-start) with stop > start, through
       the unsigned divider (IntDivPos, m_fixed.h; same result) */
    ds_p->scalestep = rw_scalestep = IntDivPos(ds_p->scale2 - rw_scale, stop-start);
  }
  else
  {
    ds_p->scale2 = ds_p->scale1;
  }

  // calculate texture boundaries
  //  and decide if floor / ceiling marks are needed
  worldtop = frontsector->ceilingheight - viewz;
  worldbottom = frontsector->floorheight - viewz;

  midtexture = toptexture = bottomtexture = maskedtexture = 0;
  ds_p->maskedtexturecol = NULL;

  if (!backsector)
  {
    // single sided line
    midtexture = texturetranslation[sidedef->midtexture];
    // a single sided line is terminal, so it must mark ends
    markfloor = markceiling = true;
    if (linedef->flags & ML_DONTPEGBOTTOM)
    {
      vtop = frontsector->floorheight +
        textureheight[sidedef->midtexture];
      // bottom of texture at bottom
      rw_midtexturemid = vtop - viewz;
    }
    else
    {
      // top of texture at top
      rw_midtexturemid = worldtop;
    }
    rw_midtexturemid += sidedef->rowoffset;

    ds_p->silhouette = SIL_BOTH;
    ds_p->sprtopclip = screenheightarray;
    ds_p->sprbottomclip = (short *)negonearray;
    ds_p->bsilheight = MAXINT;
    ds_p->tsilheight = MININT;
  }
  else
  {
    // two sided line
    ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
    ds_p->silhouette = 0;

    if (frontsector->floorheight > backsector->floorheight)
    {
      ds_p->silhouette = SIL_BOTTOM;
      ds_p->bsilheight = frontsector->floorheight;
    }
    else if (backsector->floorheight > viewz)
    {
      ds_p->silhouette = SIL_BOTTOM;
      ds_p->bsilheight = MAXINT;
      // ds_p->sprbottomclip = negonearray;
    }

    if (frontsector->ceilingheight < backsector->ceilingheight)
    {
      ds_p->silhouette |= SIL_TOP;
      ds_p->tsilheight = frontsector->ceilingheight;
    }
    else if (backsector->ceilingheight < viewz)
    {
      ds_p->silhouette |= SIL_TOP;
      ds_p->tsilheight = MININT;
      // ds_p->sprtopclip = screenheightarray;
    }

    if (backsector->ceilingheight <= frontsector->floorheight)
    {
      ds_p->sprbottomclip = (short *)negonearray;
      ds_p->bsilheight = MAXINT;
      ds_p->silhouette |= SIL_BOTTOM;
    }

    if (backsector->floorheight >= frontsector->ceilingheight)
    {
      ds_p->sprtopclip = screenheightarray;
      ds_p->tsilheight = MININT;
      ds_p->silhouette |= SIL_TOP;
    }

    worldhigh = backsector->ceilingheight - viewz;
    worldlow = backsector->floorheight - viewz;

    // hack to allow height changes in outdoor areas
    if (frontsector->ceilingpic == skyflatnum && backsector->ceilingpic == skyflatnum)
    {
      worldtop = worldhigh;
    }


    if (worldlow != worldbottom 
      || backsector->floorpic != frontsector->floorpic
      || backsector->lightlevel != frontsector->lightlevel)
    {
      markfloor = true;
    }
    else
    {
      // same plane on both sides
      markfloor = false;
    }


    if (worldhigh != worldtop 
      || backsector->ceilingpic != frontsector->ceilingpic
      || backsector->lightlevel != frontsector->lightlevel)
    {
      markceiling = true;
    }
    else
    {
      // same plane on both sides
      markceiling = false;
    }

    if (backsector->ceilingheight <= frontsector->floorheight
      || backsector->floorheight >= frontsector->ceilingheight)
    {
      // closed door
      markceiling = markfloor = true;
    }


    if (worldhigh < worldtop)
    {
      // top texture
      toptexture = texturetranslation[sidedef->toptexture];
      if (linedef->flags & ML_DONTPEGTOP)
      {
        // top of texture at top
        rw_toptexturemid = worldtop;
      }
      else
      {
        vtop = backsector->ceilingheight + textureheight[sidedef->toptexture];

        // bottom of texture
        rw_toptexturemid = vtop - viewz;
      }
    }
    if (worldlow > worldbottom)
    {
      // bottom texture
      bottomtexture = texturetranslation[sidedef->bottomtexture];

      if (linedef->flags & ML_DONTPEGBOTTOM )
      {
        // bottom of texture at bottom
        // top of texture at top
        rw_bottomtexturemid = worldtop;
      }
      else	// top of texture at top
        rw_bottomtexturemid = worldlow;
    }
    rw_toptexturemid += sidedef->rowoffset;
    rw_bottomtexturemid += sidedef->rowoffset;

    // allocate space for masked texture tables
    if (sidedef->midtexture)
    {
      // masked midtexture
      maskedtexture = true;
      ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
      lastopening += rw_stopx - rw_x;
    }
  }

  // calculate rw_offset (only needed for textured lines)
  segtextured = (midtexture | toptexture | bottomtexture | maskedtexture) != 0;

  if (segtextured)
  {
    offsetangle = rw_normalangle-rw_angle1;

    if (offsetangle > ANG180)
      offsetangle = -offsetangle;

    if (offsetangle > ANG90)
      offsetangle = ANG90;

    sineval = finesine[offsetangle >>ANGLETOFINESHIFT];
    rw_offset = FixedMul (hyp, sineval);

    if (rw_normalangle-rw_angle1 < ANG180)
      rw_offset = -rw_offset;

    rw_offset += sidedef->textureoffset + curline->offset;
    rw_centerangle = ANG90 + viewangle - rw_normalangle;

    // calculate light table
    //  use different light tables
    //  for horizontal / vertical / diagonal
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    if (!fixedcolormap)
    {
      lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

      if (curline->v1->y == curline->v2->y)
        lightnum--;
      else if (curline->v1->x == curline->v2->x)
        lightnum++;

      if (lightnum < 0)		
        walllights = scalelight[0];
      else if (lightnum >= LIGHTLEVELS)
        walllights = scalelight[LIGHTLEVELS-1];
      else
        walllights = scalelight[lightnum];
    }
  }

  // if a floor / ceiling plane is on the wrong side
  //  of the view plane, it is definitely invisible
  //  and doesn't need to be marked.


  if (frontsector->floorheight >= viewz)
  {
    // above view plane
    markfloor = false;
  }

  if ((frontsector->ceilingheight <= viewz) && (frontsector->ceilingpic != skyflatnum))
  {
    // below view plane
    markceiling = false;
  }


  // calculate incremental stepping values for texture edges
  worldtop >>= 4;
  worldbottom >>= 4;

  topstep = -FixedMul (rw_scalestep, worldtop);
  topfrac = (centeryfrac>>4) - FixedMul (worldtop, rw_scale);

  bottomstep = -FixedMul (rw_scalestep,worldbottom);
  bottomfrac = (centeryfrac>>4) - FixedMul (worldbottom, rw_scale);

  if (backsector)
  {	
    worldhigh >>= 4;
    worldlow >>= 4;

    if (worldhigh < worldtop)
    {
      pixhigh = (centeryfrac>>4) - FixedMul (worldhigh, rw_scale);
      pixhighstep = -FixedMul (rw_scalestep,worldhigh);
    }

    if (worldlow > worldbottom)
    {
      pixlow = (centeryfrac>>4) - FixedMul (worldlow, rw_scale);
      pixlowstep = -FixedMul (rw_scalestep,worldlow);
    }
  }

  // render it
  if (markceiling)
    ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);

  if (markfloor)
    floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

  R_RenderSegLoop ();


  // save sprite clipping info
  if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
    && !ds_p->sprtopclip)
  {
    R_CopyClip (lastopening, ceilingclip+start, rw_stopx-start);
    ds_p->sprtopclip = lastopening - start;
    lastopening += rw_stopx - start;
  }

  if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)  && (!ds_p->sprbottomclip))
  {
    R_CopyClip (lastopening, floorclip+start, rw_stopx-start);
    ds_p->sprbottomclip = lastopening - start;
    lastopening += rw_stopx - start;
  }

  if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
  {
    ds_p->silhouette |= SIL_TOP;
    ds_p->tsilheight = MININT;
  }
  if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
  {
    ds_p->silhouette |= SIL_BOTTOM;
    ds_p->bsilheight = MAXINT;
  }
  ds_p++;
}

