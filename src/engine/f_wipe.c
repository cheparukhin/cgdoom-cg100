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
// Mission begin melt/wipe screen special effect.
//
//-----------------------------------------------------------------------------



#include "z_zone.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"
#include "m_random.h"

#include "doomdef.h"

#ifdef ENABLE_F_WIPE
#include "f_wipe.h"

//
//                       SCREEN WIPE PACKAGE
//

// when zero, stop the wipe
static boolean go = 0;

static pixel_t* wipe_scr_start;
static pixel_t* wipe_scr_end;
static pixel_t* wipe_scr;

/* CGDoom: the melt moves columns of two pixels */
#if CGD_TRUECOLOR
typedef uint32_t wipe_pair_t;
#else
typedef uint16_t wipe_pair_t;
#endif

#define WIPE_BYTES	(SCREENWIDTH*SCREENHEIGHT*PIXELBYTES)


int wipe_initColorXForm( int width, int height, int ticks )
{
  ticks= 0; //Shuts up GCC
  memcpy(wipe_scr, wipe_scr_start, width*height*sizeof(pixel_t));
  return 0;
}

int wipe_doColorXForm( int width, int height, int ticks )
{
  boolean changed;
  pixel_t* w;
  pixel_t* e;
  int  newval;

  changed = false;
  w = wipe_scr;
  e = wipe_scr_end;

  while (w!=wipe_scr+width*height)
  {
    if (*w != *e)
    {
      if (*w > *e)
      {
        newval = *w - ticks;
        if (newval < *e)
          *w = *e;
        else
          *w = newval;
        changed = true;
      }
      else if (*w < *e)
      {
        newval = *w + ticks;
        if (newval > *e)
          *w = *e;
        else
          *w = newval;
        changed = true;
      }
    }
    w++;
    e++;
  }

  return !changed;

}

int wipe_exitColorXForm( int width, int height, int ticks )
{
  ticks= 0; //Shuts up GCC
  width= 0;
  height= 0;

  return 0;
}


static int* y;

int wipe_initMelt( int width, int height, int ticks )
{
  int i, r;

  ticks= 0; //Shuts up GCC

  // copy start screen to main screen
  memcpy(wipe_scr, wipe_scr_start, width*height*sizeof(pixel_t));

  /* CGDoom: Normally the wipe effect transposes the buffers in color-major
     order to better use the cache, but uses a screen-sized temporary buffer in
     the process. We don't need the added speed, but we sure need every bit of
     memory we can scrape. So we keep the row-major order. */
  // makes this wipe faster (in theory)
  // to have stuff in column-major format
//  wipe_shittyColMajorXform((short*)wipe_scr_start, width/2, height);
//  wipe_shittyColMajorXform((short*)wipe_scr_end, width/2, height);

  // setup initial column positions
  // (y<0 => not ready to scroll yet)
  y = (int *) Z_Malloc(width*sizeof(int), PU_STATIC, 0);
  y[0] = -(M_Random()%16);
  for (i=1;i<width;i++)
  {
    r = (M_Random()%3) - 1;
    y[i] = y[i-1] + r;
    if (y[i] > 0) y[i] = 0;
    else if (y[i] == -16) y[i] = -15;
  }

  return 0;
}

int wipe_doMelt( int width, int height, int ticks )
{
  /* CGDoom: To spare memory, we have left both wipe_scr_start and wipe_scr_end
     in row-major order. This affects the computation of (short *s) as well as
     its stride (now a full row instead of 2 pixels). */

  int  i;
  int  j;
  int  dy;
  int  idx;

  wipe_pair_t* s;
  wipe_pair_t* d;
  boolean done = true;

  width/=2;

  while (ticks--)
  {
    for (i=0;i<width;i++)
    {
      if (y[i]<0)
      {
        y[i]++; done = false;
      }
      else if (y[i] < height)
      {
        dy = (y[i] < 16) ? y[i]+1 : 8;
        if (y[i]+dy >= height) dy = height - y[i];
        s = &((wipe_pair_t *)wipe_scr_end)[y[i]*width+i]; /* Row-major order */
        d = &((wipe_pair_t *)wipe_scr)[y[i]*width+i];
        idx = 0;
        for (j=dy;j;j--)
        {
          d[idx] = *s;
          s += width; /* Row-major order */
          idx += width;
        }
        y[i] += dy;
        s = &((wipe_pair_t *)wipe_scr_start)[i]; /* Row-major order */
        d = &((wipe_pair_t *)wipe_scr)[y[i]*width+i];
        idx = 0;
        for (j=height-y[i];j;j--)
        {
          d[idx] = *s;
          s += width; /* Row-major order */
          idx += width;
        }
        done = false;
      }
    }
  }

  return done;

}

/* CGDoom: CGD_TRANSIENT_SCREENS: screens[2] and [3] are allocated for the
   duration of a wipe (malloc, 32-byte aligned) */
#if CGD_TRANSIENT_SCREENS
static void *wipe_alloc[2];

static void wipe_FreeScreens (void)
{
  free (wipe_alloc[0]);
  free (wipe_alloc[1]);
  wipe_alloc[0] = wipe_alloc[1] = NULL;
  screens[2] = screens[3] = NULL;
}
#endif

int wipe_exitMelt( int width, int height, int ticks )
{
  ticks= 0; //Shuts up GCC
  width= 0;
  height= 0;

  Z_Free(y);
#if CGD_TRANSIENT_SCREENS
  wipe_FreeScreens ();
#endif
  return 0;
}

int wipe_StartScreen ()
{
#if CGD_TRANSIENT_SCREENS
  for (int i = 0; i < 2; i++)
  {
    wipe_alloc[i] = malloc (WIPE_BYTES + 31);
    if (!wipe_alloc[i])
    {
      wipe_FreeScreens ();
      return 0;
    }
    screens[2+i] = (pixel_t *)(((uintptr_t)wipe_alloc[i] + 31)
      & ~(uintptr_t)31);
  }
#endif
  wipe_scr_start = screens[2];
  I_ReadScreen(wipe_scr_start);
  return 1;
}

int wipe_EndScreen( int x, int y, int width, int height )
{
  wipe_scr_end = screens[3];
  I_ReadScreen(wipe_scr_end);

  V_DrawBlock(x, y, 0, width, height, wipe_scr_start); // restore start scr.

  return 0;
}

int wipe_ScreenWipe ( int wipeno, int width, int height, int ticks )
{
  int rc;
  static int (*wipes[])(int, int, int) =
  {
    wipe_initColorXForm, wipe_doColorXForm, wipe_exitColorXForm,
    wipe_initMelt, wipe_doMelt, wipe_exitMelt
  };

  if (wipeno < 0 || wipeno >= wipe_NUMWIPES)
    return 1;

  // initial stuff
  if (!go)
  {
    go = 1;
    //wipe_scr = (byte *) Z_Malloc(width*height, PU_STATIC, 0); // carmack debug, don't touch!
    wipe_scr = screens[0];
    (*wipes[wipeno*3])(width, height, ticks);
  }

  // do a piece of wipe-in
  V_MarkRect(0, 0, width, height);
  rc = (*wipes[wipeno*3+1])(width, height, ticks);
  //V_DrawBlock(x, y, 0, width, height, wipe_scr); // carmack debug, don't touch!

  // final stuff
  if (rc)
  {
    go = 0;
    (*wipes[wipeno*3+2])(width, height, ticks);
  }

  return !go;

}
#endif //#ifdef ENABLE_F_WIPE
