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
//	DOOM graphics stuff (palette and presentation go through PL_*)
//
//-----------------------------------------------------------------------------

#include "os.h"
#include "cgdoom.h"

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "d_main.h"

#include "doomdef.h"

/* Current palette data (after gamma transform) */
static uint16_t gPaletteData[256];
/* Copy of current palette (before gamma transform); a copy because the
   PLAYPAL lump may be a purgeable heap copy */
static byte gPalette[768];
static boolean gPaletteSet = false;

void I_SetPalette(const byte* pal)
{
  /* If pal=NULL, just regenerate the current palette to account for changes
     in the gamma level */
  if (pal == NULL)
  {
    if (!gPaletteSet)
      return;
    pal = gPalette;
  }
  else
  {
    memcpy(gPalette, pal, sizeof gPalette);
    gPaletteSet = true;
  }

  extern int usegamma;
  const byte *gamma = gammatable[usegamma % 5];

  for (int i = 0; i < 256; i++)
  {
    int r = pal[3*i];
    int g = pal[3*i+1];
    int b = pal[3*i+2];

    r = gamma[r] >> 3;
    g = gamma[g] >> 2;
    b = gamma[b] >> 3;
    gPaletteData[i] = (r << 11) | (g << 5) | b;
  }

  PL_SetPalette(gPaletteData);
}

void I_Flip (void)
{
  prof_enter(CGD_Perf.DisplayInterface);
  PL_FinishUpdate(screens[0]);
  prof_leave(CGD_Perf.DisplayInterface);
}

void I_InitGraphics(void)
{
  usegamma = 0;
}

void I_ShutdownGraphics(void)
{
}

void I_ReinitAfterError(void)
{
  /* Redraw status bar over help message */
  extern boolean st_firsttime;
  st_firsttime = true;

  PL_VideoReset();
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
  memcpy(scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}
