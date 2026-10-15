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
// DOOM main program (D_DoomMain) and game loop (D_DoomLoop),
// plus functions to determine game mode (shareware, registered),
// parse command line parameters, configure game parameters (turbo),
// and call the startup functions.
//
//-----------------------------------------------------------------------------


#define BGCOLOR  7
#define FGCOLOR  8


#include "os.h"
#include "cgdoom.h"

#include "doomdef.h"
#include "doomstat.h"

#include "dstrings.h"


#include "z_zone.h"
#include "w_wad.h"
#include "v_video.h"

#include "f_finale.h"
#include "f_wipe.h"

#include "m_misc.h"
#include "m_menu.h"

#include "i_system.h"
#include "i_video.h"

#include "g_game.h"

#include "hu_stuff.h"
#include "wi_stuff.h"
#include "st_stuff.h"
#include "am_map.h"

#include "p_setup.h"
#include "r_local.h"


#include "d_main.h"


//
// D-DoomLoop()
// Not a globally visible function,
//  just included for source reference,
//  called by D_DoomMain, never exits.
// Manages timing and IO,
//  calls all ?_Responder, ?_Ticker, and ?_Drawer,
//  calls I_GetTime, I_StartFrame, and I_StartTic
//
void D_DoomLoop (void);


boolean   devparm; // started game with -devparm
boolean         nomonsters; // checkparm of -nomonsters
boolean         respawnparm; // checkparm of -respawn
boolean         fastparm; // checkparm of -fast

boolean         drone;

boolean   singletics = true;

boolean   fuck = false;


extern boolean inhelpscreens;

skill_t   startskill;
int    startepisode = 1;
int    startmap = 1;
boolean   autostart;

boolean   advancedemo;
void D_ProcessEvents (void);
void G_BuildTiccmd (ticcmd_t* cmd);
void D_DoAdvanceDemo (void);

//
// D_PostEvent
// Called by the I/O functions when input is detected
//
void D_PostEvent (event_t* ev)
{
  if (M_Responder (ev))
    return;               // menu ate the event
  G_Responder (ev);
}

//
// D_Display
//  draw current display, possibly wiping it from the previous
//

// wipegamestate can be set to -1 to force a wipe on the next draw
gamestate_t     wipegamestate = GS_DEMOSCREEN;
extern boolean setsizeneeded;
extern int  showMessages;
void    R_ExecuteSetViewSize (void);

void D_Display (void)
{
  static  boolean   viewactivestate CGD_UNUSED = false;
  static  boolean   menuactivestate = false;
  static  boolean   automapactivestate = false;
  static  boolean   inhelpscreensstate = false;
  static  boolean   fullscreen = false;
  static  gamestate_t  oldgamestate = (gamestate_t)-1;
  static  int    borderdrawcount CGD_UNUSED;
  int      nowtime;
  int      tics;
  int      wipestart;
  int      y;
  boolean     done;
  boolean     wipe;
  boolean     redrawsbar;
  int         wipepalettes = 0;   /* CGDoom: CGD_Stats at the wipe's start */

  redrawsbar = false;

  // change the view size if needed
  if (setsizeneeded)
  {
    R_ExecuteSetViewSize ();
    oldgamestate = (gamestate_t)-1;                      // force background redraw
    borderdrawcount = 3;
  }

  // save the current screen if about to wipe
  /* CGDoom: no wipe in a timedemo (vanilla timed its wipes too), or
     without memory for it (wipe_StartScreen) */
  if (gamestate != wipegamestate && !timingdemo && wipe_StartScreen())
  {
    wipe = true;
    wipepalettes = CGD_Stats.PaletteChanges;
  }
  else
  {
    wipe = false;
    wipegamestate = gamestate;
  }

  if (gamestate == GS_LEVEL && gametic)
    HU_Erase();

  /* CGDoom: the palette of a new non-level state (vanilla set it after
     drawing, which the 8-bit palette recolours at presentation, but a
     true-colour frame keeps the colours it was drawn with: the wipe would
     melt into a tinted intermission) */
  if (gamestate != oldgamestate && gamestate != GS_LEVEL)
    I_SetPalette ((byte *)W_CacheLumpNameConst ("PLAYPAL", PU_CACHE));

  // do buffered drawing
  switch (gamestate)
  {
  case GS_LEVEL:
    if (!gametic)
      break;
#if CGD_TRUECOLOR
    /* CGDoom: the damage/bonus palette before the automap is drawn
       (ST_Drawer below sets it for the rest) */
    ST_doPaletteStuff ();
#endif
    /* CGDoom: AM_FULLMAP is automapactive without the overlay (am_map.h),
       which is drawn after the view */
    if (AM_FULLMAP)
      AM_Drawer ();
    if (wipe || (viewheight != SCREENHEIGHT && fullscreen) )
      redrawsbar = true;
    if (inhelpscreensstate && !inhelpscreens)
      redrawsbar = true;              // just put away the help screen
    /* CGDoom: with the full-screen view (screenblocks 11) the status
       bar is only drawn with the automap, so it is redrawn in full when the
       automap opens (vanilla did this through ST_Responder's AM_MSGENTERED,
       which CGDoom's automap does not send) */
    if (AM_FULLMAP && !automapactivestate && viewheight == SCREENHEIGHT)
      redrawsbar = true;
    ST_Drawer ((boolean)(viewheight == SCREENHEIGHT), redrawsbar );
    fullscreen = (boolean)(viewheight == SCREENHEIGHT);
    break;

  case GS_INTERMISSION:
    WI_Drawer ();
    break;

  case GS_FINALE:
    F_Drawer ();
    break;

  case GS_DEMOSCREEN:
    D_PageDrawer ();
    break;
  }

  /* CGDoom: border of a reduced view (screenblocks < 10: the view
     is narrower than the screen). The border is black (CGDoom's
     background, R_VideoErase); it is cleared again whenever something may
     have drawn over it: the menu (active now or in the previous frame), the
     automap (previous frame), a help screen, or a new game state or view
     size (oldgamestate is reset by R_ExecuteSetViewSize above). Vanilla did
     this after HU_Drawer; doing it before keeps the HUD messages that are
     drawn in the border. */
  /* (CGDoom true colour: also after a palette change, which recolours
     everything only in the 8-bit build) */
#if CGD_TRUECOLOR
  static int border_serial;
  boolean newpalette = border_serial != v_palette_serial;
  border_serial = v_palette_serial;
#else
  const boolean newpalette = false;
#endif
  if (gamestate == GS_LEVEL && !AM_FULLMAP && gametic
    && scaledviewwidth != SCREENWIDTH
    && (menuactive || menuactivestate || automapactivestate
      || inhelpscreensstate || oldgamestate != GS_LEVEL || newpalette))
    R_DrawViewBorder ();

  // draw the view directly
  if (gamestate == GS_LEVEL && !AM_FULLMAP && gametic)
    R_RenderPlayerView (&players[0]);

  if (gamestate == GS_LEVEL && automapoverlay && gametic)
    AM_Drawer ();

  if (gamestate == GS_LEVEL && gametic)
    HU_Drawer ();

  menuactivestate = menuactive;
  automapactivestate = AM_FULLMAP;
  viewactivestate = viewactive;
  inhelpscreensstate = inhelpscreens;
  oldgamestate = gamestate;

  // draw pause pic
  if (paused)
  {
    if (AM_FULLMAP)
      y = 4;
    else
      y = viewwindowy+4;
    V_DrawPatchDirect(viewwindowx+(scaledviewwidth-68)/2, y ,0 ,W_CacheLumpNamePatch("M_PAUSE", PU_CACHE) );
  }

  // menus go directly to the screen
  M_Drawer ();          // menu is drawn even on top of everything

  // normal update
  if (!wipe)
  {
    I_Flip ();              // page flip or blit buffer
    return;
  }

  // wipe update
  wipe_EndScreen(0, 0, SCREENWIDTH, SCREENHEIGHT);
  CGD_Stats.WipeStalePalette = CGD_Stats.PaletteChanges != wipepalettes;

  wipestart = I_GetTime () - 1;

  do
  {
    do
    {
      nowtime = I_GetTime ();
      tics = nowtime - wipestart;
      /* CGDoom: sleep instead of spinning (host: advances its clock) */
      if (tics <= 0)
        PL_WaitTics (wipestart + 1);
    } while (tics<=0);
    wipestart = nowtime;
    done = wipe_ScreenWipe(wipe_Melt, SCREENWIDTH, SCREENHEIGHT, tics);
    M_Drawer ();       // menu is drawn even on top of wipes
    I_Flip ();        // page flip or blit buffer
  } while (!done);
  CGD_Stats.WipeStalePalette = 0;
  gamestate = oldgamestate;
  wipegamestate= oldgamestate;
}



//
//  D_DoomLoop
//
extern  boolean  demorecording;

void D_DoomLoop (void)
{
  I_InitGraphics ();

  while (!fuck)
  {
    I_StartTic ();

    if (advancedemo)
      D_DoAdvanceDemo ();

    M_Ticker (); //Menu ticker

    G_Ticker (); //Game ticker
    if (fuck) break; //Quick exit after save game

    gametic++;

    // Update display, next frame, with current state
    // (CGDoom: fixed or automatic frameskip, i_system.c)
    if (I_DisplayDue ())
    {
      D_Display ();
      I_FrameDisplayed ();
      G_TimeDemoFrame ();
    }
  }

  G_CheckDemoStatus();
  Z_Free(lumpinfo);
  Z_Free(lumpcache);
  Z_Free(W_LumpMapped);
  lumpcache = NULL;
  W_LumpMapped = NULL;
  I_ShutdownGraphics();
  return;
}



//
//  DEMO LOOP
//
int             demosequence;
int             pagetic;
char           *pagename;


//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker (void)
{
  if (--pagetic < 0)
    D_AdvanceDemo ();
}



//
// D_PageDrawer
//
void D_PageDrawer (void)
{
  V_DrawFullscreenPatch(0, pagename);
}


//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//
void D_AdvanceDemo (void)
{
  advancedemo = true;
}

int episode = 1;
int map = 1;    //HACK
/* CGDoom: start skill 0..4 (sk_baby..sk_nightmare), default sk_hard; may be
   set by the platform before D_DoomMain() */
int skill = 3;

/* The demo lump G_DeferedPlayDemo() was given (g_game.c) */
extern char* defdemoname;

//
// This cycles through the demo sequences.
// FIXME - version dependend demo numbers?
//
void D_DoAdvanceDemo (void)
{
    players[consoleplayer].playerstate = PST_LIVE;  // not reborn
    advancedemo = false;
    usergame = false;               // no save / end game here
    paused = false;
    gameaction = ga_nothing;

    if ( gamemode == retail )
      demosequence = (demosequence+1)%7;
    else
      demosequence = (demosequence+1)%6;

    switch (demosequence)
    {
      case 0:
        if ( gamemode == commercial )
            pagetic = 35 * 11;
        else
            pagetic = 170;
        gamestate = GS_DEMOSCREEN;
        pagename = "TITLEPIC";
        break;
      case 1:
        G_DeferedPlayDemo ("demo1");
        break;
      case 2:
        pagetic = 200;
        gamestate = GS_DEMOSCREEN;
        pagename = "CREDIT";
        break;
      case 3:
        G_DeferedPlayDemo ("demo2");
        break;
      case 4:
        gamestate = GS_DEMOSCREEN;
        if ( gamemode == commercial)
        {
            pagetic = 35 * 11;
            pagename = "TITLEPIC";
        }
        else
        {
            pagetic = 200;

            if ( gamemode == retail )
              pagename = "CREDIT";
            else
              pagename = "HELP2";
        }
        break;
      case 5:
        G_DeferedPlayDemo ("demo3");
        break;
      case 6:
        G_DeferedPlayDemo ("demo4");
        break;
    }

    /* CGDoom: skip a demo when CGD_TitleDemos is clear (host tests), or
       when the WAD has no such lump (tools/wadtool.py pack --drop-demos):
       G_DoPlayDemo would stop with "W_GetNumForName: demo1 not found!" */
    if (gameaction == ga_playdemo
        && (!CGD_TitleDemos || W_CheckNumForName (defdemoname) < 0)) {
      D_DoAdvanceDemo();
    }
}


//
// D_StartTitle
//
void D_StartTitle (void)
{
  gameaction = ga_nothing;
  demosequence = -1;
  D_AdvanceDemo ();
}




//      print title for every printed line
//char            title[128];

//
// D_DoomMain
//
void D_DoomMain()
{
  int ok_wad=0;
//  int selected=1;
//  int rskill=1;
  int maxepisode=1;

//  int repisode=1;
//  int rmap=1;

  /* CGDoom: Z_Init is executed early because we rely on the larger heap to
     allocate the lump cache and lump info; normal heap isn't enough for WADs
     larger than the shareware WAD */
  Z_Init ();

  {
    void *zone_base[ZONE_MAX];
    int zone_size[ZONE_MAX];
    int zone_count = PL_GetZones(zone_base, zone_size, ZONE_MAX);
    for (int i = 0; i < zone_count; i++)
      Z_AddZone (zone_base[i], zone_size[i]);
    if (Z_ZoneCount() == 0)
      I_Error ("D_DoomMain: no memory zones");
  }

  /* CGDoom: the WAD has been opened by the platform (PL_WadOpen); its
     directory is read only once (it was read twice, leaking lumpinfo) */
  ok_wad=W_InitMultipleFiles();
  if (!ok_wad)
    I_Error ("W_InitMultipleFiles: not a valid WAD file");

  if(W_CheckNumForName("MAP01")!=-1)
    gamemode=commercial;
  else if(W_CheckNumForName("E1M1")==-1)
    gamemode=indetermined;
  else if (W_CheckNumForName("E2M1")==-1)
    gamemode=shareware;
  else if (W_CheckNumForName("E4M1")==-1)
    gamemode=registered;
  else
    gamemode=retail;  

  if(gamemode == indetermined) // CX port
    I_Error ("D_DoomMain: unknown game (no E1M1 or MAP01)");

  if (W_CheckNumForName("E4M1")!=-1)
    maxepisode = 4;
  else if (W_CheckNumForName("E3M1")!=-1)
    maxepisode = 3;
  else if (W_CheckNumForName("E2M1")!=-1)
    maxepisode = 2;

  /* CGDoom: Override game mode for split WADs; we have single-episode WADs for
     all episodes of Ultimate Doom, and they should identify as retail.Note
     that CGD_SingleEpisodeUltimate might have been set before based on the WAD
     file name. */
  /* (Not for Doom II IWADs: Freedoom phase 2 has WIMAP0 too) */
  if (!CGD_SingleEpisodeUltimate && maxepisode == 1 && gamemode != commercial) {
    /* Shareware Doom */
    if (W_CheckNumForName("DEMO4") == -1)
      {}
    /* Each individual Ultimate Doom episode */
    else if (W_CheckNumForName("WIMAP0") != -1)
      CGD_SingleEpisodeUltimate = 1;
    else if (W_CheckNumForName("WIMAP1") != -1)
      CGD_SingleEpisodeUltimate = 2;
    else if (W_CheckNumForName("WIMAP2") != -1)
      CGD_SingleEpisodeUltimate = 3;
    else if (W_CheckNumForName("INTERPIC") != -1)
      CGD_SingleEpisodeUltimate = 4;
  }
  if (CGD_SingleEpisodeUltimate > 0) {
    gamemode=retail;
    maxepisode = CGD_SingleEpisodeUltimate;
  }


    nomonsters = 0;
    respawnparm = 0;
    fastparm = 0;
    devparm = 0;

    // get skill / episode / map from parms // DON'T, LEFT IN UNTIL OPCHECK
    if(skill==0)  startskill = sk_baby;
    else if(skill==1) startskill = sk_easy;
    else if(skill==2) startskill = sk_medium;
    else if(skill==3) startskill = sk_hard;
    else if(skill==4) startskill = sk_nightmare;

    // init subsystems
    printf ("V_Init: Allocate screens\n");
    V_Init ();
    printf ("Z_Init: Init zone memory allocation daemon \n");
    //Z_Init ();
    /* CGDoom: W_InitMultipleFiles() was already called above */

    // Check and print which version is executed.
    switch ( gamemode )
    {
    case shareware:
    case indetermined:
      printf (
        "=====================================\n"
        "             Shareware!\n"
        "=====================================\n"
        );
      break;
    case registered:
    case retail:
    case commercial:
      printf (
        "=====================================\n"
        "          Commercial product\n"
        "          do not distribute!\n"
        "=====================================\n"
        );
      break;

    default:
      // Ouch.
      break;
    }

    printf ("I_Init: Setting up machine state\n");
    I_Init ();
    printf ("M_Init: Init miscellaneous info\n");
    M_Init ();
    printf ("R_Init: Init DOOM refresh daemon \n");
    R_Init ();
    printf ("P_Init: Init Playloop state\n");
    P_Init ();
    printf ("HU_Init: Setup heads up display\n");
    HU_Init ();
    printf ("ST_Init: Init status bar\n");
    ST_Init ();
    printf ("Engage... \n");

    if (CGD_RecordDemoSlot >= 0) {
      static char filename[CGD_NAME_MAX];
      CGD_DemoFileName(filename, CGD_RecordDemoSlot);
      G_RecordDemo(filename);
    }

    /* CGDoom: timedemo of the WAD's demos (cgdoom.h); none: quit */
    if (CGD_TimeDemo && !G_TimeDemo ())
      fuck = true;

    /* CGDoom: Demo file was specified from main menu */
    if (gameaction == ga_playdemo || fuck)
      {}
    else if (autostart)
      G_DeferedInitNew (startskill, startepisode, startmap);
    else
      D_StartTitle ();

    D_DoomLoop ();  // never returns, except when it does
    I_Restore (); //calckill = bad

    return;
}
