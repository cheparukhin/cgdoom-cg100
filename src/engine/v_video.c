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
// Gamma correction LUT stuff.
// Functions to draw patches (by post) directly to screen.
// Functions to blit a block to the screen.
//
//-----------------------------------------------------------------------------


//static const char 

#include "os.h"
#include "cgdoom.h"

#include "i_system.h"
#include "r_local.h"

#include "doomdef.h"
#include "doomdata.h"

#include "m_bbox.h"
#include "m_swap.h"

#include "v_video.h"

#include "z_zone.h"
#include "w_wad.h"


// Each screen is [SCREENWIDTH*SCREENHEIGHT]; 

byte* screens[SCREEN_COUNT]; 

int    dirtybox[4]; 



// Now where did these came from?
const byte gammatable[5][256] =
{
  {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,
    81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,
    113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255},

  {2,4,5,7,8,10,11,12,14,15,16,18,19,20,21,23,24,25,26,27,29,30,31,
  32,33,34,36,37,38,39,40,41,42,44,45,46,47,48,49,50,51,52,54,55,
  56,57,58,59,60,61,62,63,64,65,66,67,69,70,71,72,73,74,75,76,77,
  78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,
  99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,
  115,116,117,118,119,120,121,122,123,124,125,126,127,128,129,129,
  130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,
  146,147,148,148,149,150,151,152,153,154,155,156,157,158,159,160,
  161,162,163,163,164,165,166,167,168,169,170,171,172,173,174,175,
  175,176,177,178,179,180,181,182,183,184,185,186,186,187,188,189,
  190,191,192,193,194,195,196,196,197,198,199,200,201,202,203,204,
  205,205,206,207,208,209,210,211,212,213,214,214,215,216,217,218,
  219,220,221,222,222,223,224,225,226,227,228,229,230,230,231,232,
  233,234,235,236,237,237,238,239,240,241,242,243,244,245,245,246,
  247,248,249,250,251,252,252,253,254,255},

  {4,7,9,11,13,15,17,19,21,22,24,26,27,29,30,32,33,35,36,38,39,40,42,
  43,45,46,47,48,50,51,52,54,55,56,57,59,60,61,62,63,65,66,67,68,69,
  70,72,73,74,75,76,77,78,79,80,82,83,84,85,86,87,88,89,90,91,92,93,
  94,95,96,97,98,100,101,102,103,104,105,106,107,108,109,110,111,112,
  113,114,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,
  129,130,131,132,133,133,134,135,136,137,138,139,140,141,142,143,144,
  144,145,146,147,148,149,150,151,152,153,153,154,155,156,157,158,159,
  160,160,161,162,163,164,165,166,166,167,168,169,170,171,172,172,173,
  174,175,176,177,178,178,179,180,181,182,183,183,184,185,186,187,188,
  188,189,190,191,192,193,193,194,195,196,197,197,198,199,200,201,201,
  202,203,204,205,206,206,207,208,209,210,210,211,212,213,213,214,215,
  216,217,217,218,219,220,221,221,222,223,224,224,225,226,227,228,228,
  229,230,231,231,232,233,234,235,235,236,237,238,238,239,240,241,241,
  242,243,244,244,245,246,247,247,248,249,250,251,251,252,253,254,254,
  255},

  {8,12,16,19,22,24,27,29,31,34,36,38,40,41,43,45,47,49,50,52,53,55,
  57,58,60,61,63,64,65,67,68,70,71,72,74,75,76,77,79,80,81,82,84,85,
  86,87,88,90,91,92,93,94,95,96,98,99,100,101,102,103,104,105,106,107,
  108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,
  125,126,127,128,129,130,131,132,133,134,135,135,136,137,138,139,140,
  141,142,143,143,144,145,146,147,148,149,150,150,151,152,153,154,155,
  155,156,157,158,159,160,160,161,162,163,164,165,165,166,167,168,169,
  169,170,171,172,173,173,174,175,176,176,177,178,179,180,180,181,182,
  183,183,184,185,186,186,187,188,189,189,190,191,192,192,193,194,195,
  195,196,197,197,198,199,200,200,201,202,202,203,204,205,205,206,207,
  207,208,209,210,210,211,212,212,213,214,214,215,216,216,217,218,219,
  219,220,221,221,222,223,223,224,225,225,226,227,227,228,229,229,230,
  231,231,232,233,233,234,235,235,236,237,237,238,238,239,240,240,241,
  242,242,243,244,244,245,246,246,247,247,248,249,249,250,251,251,252,
  253,253,254,254,255},

  {16,23,28,32,36,39,42,45,48,50,53,55,57,60,62,64,66,68,69,71,73,75,76,
  78,80,81,83,84,86,87,89,90,92,93,94,96,97,98,100,101,102,103,105,106,
  107,108,109,110,112,113,114,115,116,117,118,119,120,121,122,123,124,
  125,126,128,128,129,130,131,132,133,134,135,136,137,138,139,140,141,
  142,143,143,144,145,146,147,148,149,150,150,151,152,153,154,155,155,
  156,157,158,159,159,160,161,162,163,163,164,165,166,166,167,168,169,
  169,170,171,172,172,173,174,175,175,176,177,177,178,179,180,180,181,
  182,182,183,184,184,185,186,187,187,188,189,189,190,191,191,192,193,
  193,194,195,195,196,196,197,198,198,199,200,200,201,202,202,203,203,
  204,205,205,206,207,207,208,208,209,210,210,211,211,212,213,213,214,
  214,215,216,216,217,217,218,219,219,220,220,221,221,222,223,223,224,
  224,225,225,226,227,227,228,228,229,229,230,230,231,232,232,233,233,
  234,234,235,235,236,236,237,237,238,239,239,240,240,241,241,242,242,
  243,243,244,244,245,245,246,246,247,247,248,248,249,249,250,250,251,
  251,252,252,253,254,254,255,255}
};



int usegamma;

//
// V_MarkRect 
// 
void V_MarkRect( int x,int y,int width,int height ) 
{ 
  M_AddToBox (dirtybox, x, y); 
  M_AddToBox (dirtybox, x+width-1, y+height-1); 
} 


/* CGDoom: one row of V_CopyRect. The status-bar widgets copy
   rows of a few tens of bytes every frame, which fxlibc's memcpy does byte
   by byte below 64 bytes. Source and destination have the same x and
   SCREENWIDTH-byte rows, so they are usually equally aligned: copy words. */
static inline void V_CopyRow (byte *dest, const byte *src, int n)
{
  if (n >= 64 || (((uintptr_t)dest ^ (uintptr_t)src) & 3))
  {
    memcpy (dest, src, n);
    return;
  }
  for ( ; n > 0 && ((uintptr_t)dest & 3); n--)
    *dest++ = *src++;
  for ( ; n >= 4; n -= 4, dest += 4, src += 4)
    *(uint32_t *)dest = *(const uint32_t *)src;
  for ( ; n > 0; n--)
    *dest++ = *src++;
}

//
// V_CopyRect 
// 
void V_CopyRect(int srcx,int srcy,int srcscrn,int width,int height,int destx,int desty,int destscrn ) 
{ 
  byte* src;
  byte* dest; 

#ifdef RANGECHECK 
  if (srcx<0  ||srcx+width >SCREENWIDTH  || srcy<0 || srcy+height>SCREENHEIGHT  ||destx<0||destx+width >SCREENWIDTH
    || desty<0  || desty+height>SCREENHEIGHT  || (unsigned)srcscrn>=SCREEN_COUNT || (unsigned)destscrn>=SCREEN_COUNT)
  {
    I_Error ("Bad V_CopyRect");
  }
#endif 

  V_MarkRect (destx, desty, width, height); 

  ASSERT(SCREEN_COUNT > srcscrn);
  src = screens[srcscrn]+SCREENWIDTH*srcy+srcx; 
  ASSERT(SCREEN_COUNT > destscrn);
  dest = screens[destscrn]+SCREENWIDTH*desty+destx; 
  for ( ; height>0 ; height--) 
  { 
    V_CopyRow (dest, src, width);
    src += SCREENWIDTH; 
    dest += SCREENWIDTH; 
  } 
} 

static void CopyColumn(byte *pTarget,const byte *pSource,int iLength)
{
  while(iLength) 
  { 
    pTarget[0] = pSource[0];
    pSource++; 
    pTarget += SCREENWIDTH; 
    iLength--;
  } 
}

#if CGD_EXTRAS
/* CGDoom: the translation V_DrawPatch draws through (v_video.h) */
const byte *v_patchmap;
byte v_crmap[CR_COUNT][256];
const byte v_blackmap[256];

static void CopyColumnMapped(byte *pTarget,const byte *pSource,int iLength,
  const byte *map)
{
  while(iLength--)
  {
    *pTarget = map[*pSource++];
    pTarget += SCREENWIDTH;
  }
}

/* The status-bar digits' reds: the red ramp and three darker reds */
static boolean V_IsDigitRed (int c)
{
  return (c >= 176 && c <= 191) || c == 44 || c == 45 || c == 47;
}

/* The palette entry nearest to (r, g, b) */
static int V_NearestColor (const byte *pal, int r, int g, int b)
{
  int best = 0, bestdist = 0x7fffffff;

  for (int c = 0; c < 256; c++)
  {
    int dr = pal[3*c] - r, dg = pal[3*c+1] - g, db = pal[3*c+2] - b;
    int dist = dr*dr + dg*dg + db*db;
    if (dist < bestdist)
    {
      bestdist = dist;
      best = c;
    }
  }
  return best;
}

void V_BuildColorMaps (const byte *pal)
{
  for (int k = 0; k < CR_COUNT; k++)
    for (int c = 0; c < 256; c++)
    {
      int r = pal[3*c], w = pal[3*c+1] > pal[3*c+2] ? pal[3*c+1] : pal[3*c+2];

      v_crmap[k][c] = c;
      if (!V_IsDigitRed (c))
        continue;
      switch (k)
      {
      case CR_GREEN:    /* a little light: the palette's darker greens are olive */
        v_crmap[k][c] = V_NearestColor (pal, w + (r - w)/4, r, w + (r - w)/4);
        break;
      case CR_GOLD:
        v_crmap[k][c] = V_NearestColor (pal, r, (r*13 + w*3)/16, w);
        break;
      case CR_BLUE:     /* a little light, but still a palette blue in every
                           shade (lighter ones fall on grays) */
        v_crmap[k][c] = V_NearestColor (pal, w + (r - w)/6, w + (r - w)/4, r);
        break;
      case CR_GRAY:     /* light: a dark gray vanished on the gray bar */
        v_crmap[k][c] = V_NearestColor (pal, (r*7 + w)/8, (r*7 + w)/8,
          (r*7 + w)/8);
        break;
      }
    }
}
#endif

//
// V_DrawPatch
// Masks a column based masked pic to the screen. 
//
void V_DrawPatch(int x,int y,int scrn,const patch_t* patch) 
{ 
  int  col; 
  //const column_t* column; 
  byte* desttop;
  int  w = SHORT(patch->width); 
  int h = SHORT(patch->height);
  if(w == 0)
  {
    return; //CGD: hack for some broken wad files
  }

  y -= SHORT(patch->topoffset); 
  x -= SHORT(patch->leftoffset); 
#ifdef RANGECHECK 
  if (x<0 ||x+w >SCREENWIDTH || y<0 || y+h>SCREENHEIGHT)
  {
    printf("Patch at %d,%d exceeds LFB\n", x,y );
    // No I_Error abort - what is up with TNT.WAD?
    printf("V_DrawPatch: bad patch (ignored)\n");
    return;
  }
#endif 

  if (!scrn)
    V_MarkRect (x, y, w, h); 

  col = 0; 
  ASSERT(SCREEN_COUNT > scrn);
  desttop = screens[scrn]+y*SCREENWIDTH+x; 

  /* CGDoom: columnofs with one aligned load when the lump is 4-aligned */
  int aligned = ((uintptr_t)patch & 3) == 0;
  for ( ; col<w ; x++, col++, desttop++)
  { 
    const byte * pData = (const byte *)patch + PatchColumnOfs((const byte *)patch, aligned, col);
    // step through the posts in a column 
    while (pData[0] != 0xff ) 
    { 
      const byte * pSource = pData+3; 
      byte* dest = desttop + ((int)pData[0])*SCREENWIDTH; 
      int count = pData[1]; 
      pData += count + 4 ; 

#if CGD_EXTRAS
      if (v_patchmap)
        CopyColumnMapped(dest,pSource,count,v_patchmap);
      else
#endif
      CopyColumn(dest,pSource,count);
    } 
  }    
} 

//
// V_DrawFullscreenPatch
// CGDoom: Draws a full-screen patch (title, help, intermission pictures).
// The whole lump is cached (the heap holds it) and drawn as a patch, centred
// on a larger screen; the borders around it are cleared to black (palette
// index 0, like the view border).
//
void V_DrawFullscreenPatch(int scrn, const char *lumpname)
{
#if SCREENWIDTH != ORIGWIDTH || SCREENHEIGHT != ORIGHEIGHT
  byte *p = screens[scrn];
  byte black = 0;
  int y;

  memset (p, black, HEIGHTDELTA*SCREENWIDTH);
  for (y = 0; y < ORIGHEIGHT; y++)
  {
    byte *row = p + (HEIGHTDELTA + y)*SCREENWIDTH;
    memset (row, black, WIDESCREENDELTA);
    memset (row + WIDESCREENDELTA + ORIGWIDTH, black,
      SCREENWIDTH - WIDESCREENDELTA - ORIGWIDTH);
  }
  memset (p + (HEIGHTDELTA + ORIGHEIGHT)*SCREENWIDTH, black,
    (SCREENHEIGHT - HEIGHTDELTA - ORIGHEIGHT)*SCREENWIDTH);
  if (!scrn)
    V_MarkRect (0, 0, SCREENWIDTH, SCREENHEIGHT);
#endif
  V_DrawPatch(WIDESCREENDELTA, HEIGHTDELTA, scrn,
    W_CacheLumpNamePatch(lumpname, PU_CACHE));
}

//
// V_DrawPatchFlipped 
// Masks a column based masked pic to the screen.
// Flips horizontally, e.g. to mirror face.
//
void V_DrawPatchFlipped(int x,int y,int scrn,const patch_t* patch) 
{ 
  int  col; 
  //const column_t* column; 
  byte* desttop;
  int  w = SHORT(patch->width); 
  int h = SHORT(patch->height);

  y -= SHORT(patch->topoffset); 
  x -= SHORT(patch->leftoffset); 
#ifdef RANGECHECK 
  if (x<0 ||x+w >SCREENWIDTH || y<0 || y+h>SCREENHEIGHT)
  {
    printf("Patch at %d,%d exceeds LFB\n", x,y );
    // No I_Error abort - what is up with TNT.WAD?
    printf("V_DrawPatch: bad patch (ignored)\n");
    return;
  }
#endif 

  if (!scrn)
    V_MarkRect (x, y, w, h); 

  col = 0; 
  ASSERT(SCREEN_COUNT > scrn);
  desttop = screens[scrn]+y*SCREENWIDTH+x; 
  w--;
  /* CGDoom: columnofs with one aligned load when the lump is 4-aligned */
  int aligned = ((uintptr_t)patch & 3) == 0;
  for ( ; col<=w ; x++, col++, desttop++) 
  { 
    const byte * pData = (const byte *)patch + PatchColumnOfs((const byte *)patch, aligned, w-col);
    // step through the posts in a column 
    while (pData[0] != 0xff ) 
    { 
      const byte * pSource = pData+3; 
      byte* dest = desttop + ((int)pData[0])*SCREENWIDTH; 
      int count = pData[1]; 
      pData += count + 4 ; 

      CopyColumn(dest,pSource,count);
    } 
  }    
} 



//
// V_DrawBlock
// Draw a linear block of pixels into the view buffer.
//
void V_DrawBlock ( int  x, int  y, int  scrn, int  width, int  height, const byte*  src ) 
{ 
  byte* dest; 

#ifdef RANGECHECK 
  if (x<0 ||x+width >SCREENWIDTH || y<0 || y+height>SCREENHEIGHT || (unsigned)scrn>=SCREEN_COUNT )
  {
    I_Error ("Bad V_DrawBlock");
  }
#endif 

  V_MarkRect (x, y, width, height); 

  ASSERT(SCREEN_COUNT > scrn);
  dest = screens[scrn] + y*SCREENWIDTH+x; 

  while (height--) 
  { 
    memcpy (dest, src, width); 
    src += width; 
    dest += SCREENWIDTH; 
  } 
} 



//
// V_GetBlock
// Gets a linear block of pixels from the view buffer.
//
void V_GetBlock( int  x, int  y, int  scrn, int  width, int  height, byte*  dest ) 
{ 
  byte* src; 

#ifdef RANGECHECK 
  if (x<0 ||x+width >SCREENWIDTH || y<0 || y+height>SCREENHEIGHT  || (unsigned)scrn>=SCREEN_COUNT )
  {
    I_Error ("Bad V_DrawBlock");
  }
#endif 

  ASSERT(SCREEN_COUNT > scrn);
  src = screens[scrn] + y*SCREENWIDTH+x; 

  while (height--) 
  { 
    memcpy (dest, src, width); 
    src += SCREENWIDTH; 
    dest += width; 
  } 
} 


//
// V_Init
// 
void V_Init (void) 
{ 
  /* screens[0] is the main screen */
  screens[0] = I_ScreenBase(0);
  /* screens[1] is used for the level end sequence (intermission) */
  screens[1] = I_ScreenBase(1);
  /* screens[2] and screens[3] are used for wipe effects */
  screens[2] = I_ScreenBase(2);
  screens[3] = I_ScreenBase(3);
  /* screens[4] is used for the status bar (allocated by st_stuff.c) */
  screens[4] = NULL;
}

/* CGDoom-specific function to clear screen when changing window size (the
   area above the status bar) */
void V_Clear (void)
{
  memset(screens[0], 0, SCREENWIDTH*(SCREENHEIGHT-32));
}
