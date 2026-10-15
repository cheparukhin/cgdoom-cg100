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
// Status bar code.
// Does the face/direction indicator animatin.
// Does palette indicators as well (red pain/berserk, bright pickup)
//
//-----------------------------------------------------------------------------

//static const char 


#include "os.h"
#include "cgdoom.h"

#include "i_system.h"
#include "i_video.h"
#include "z_zone.h"
#include "m_random.h"
#include "w_wad.h"

#include "doomdef.h"

#include "g_game.h"

#include "st_stuff.h"
#include "st_lib.h"
#include "m_swap.h"
#include "r_local.h"

#include "p_local.h"
#include "p_inter.h"

#include "am_map.h"
//#include "m_cheat.h"

//#include "s_sound.h"

// Needs access to LFB.
#include "v_video.h"

// State.
#include "doomstat.h"

// Data.
#include "dstrings.h"
//#include "sounds.h"

//
// STATUS BAR DATA
//


// Palette indices.
// For damage/bonus red-/gold-shifts
#define STARTREDPALS   1
#define STARTBONUSPALS   9
#define NUMREDPALS    8
#define NUMBONUSPALS   4
// Radiation suit, green shift.
#define RADIATIONPAL   13

// N/256*100% probability
//  that the normal face state will change
#define ST_FACEPROBABILITY  96

// For Responder
#define ST_TOGGLECHAT   KEY_ENTER

// Location of status bar
#define ST_X     0
#define ST_X2     104

/* CGDoom: the status bar's positions are vanilla's 320x200 layout, where
   the bar starts at y 168. On a larger screen the bar's 320 columns are
   centred (ST_BARX) and its rows are the last 32 (ST_Y); the side strips are
   filled in ST_refreshBackground. */
#define ST_BARX   WIDESCREENDELTA
#define ST_LX(x)  ((x) + ST_BARX)
#define ST_LY(y)  ((y) - 168 + ST_Y)

#define ST_FX       ST_LX(143)
#define ST_FY       ST_LY(169)

// Should be set to patch width
//  for tall numbers later on
#define ST_TALLNUMWIDTH   (tallnum[0]->width)

// Number of status faces.
#define ST_NUMPAINFACES   5
#define ST_NUMSTRAIGHTFACES  3
#define ST_NUMTURNFACES   2
#define ST_NUMSPECIALFACES  3

#define ST_FACESTRIDE \
  (ST_NUMSTRAIGHTFACES+ST_NUMTURNFACES+ST_NUMSPECIALFACES)

#define ST_NUMEXTRAFACES  2

#define ST_NUMFACES \
  (ST_FACESTRIDE*ST_NUMPAINFACES+ST_NUMEXTRAFACES)

#define ST_TURNOFFSET   (ST_NUMSTRAIGHTFACES)
#define ST_OUCHOFFSET   (ST_TURNOFFSET + ST_NUMTURNFACES)
#define ST_EVILGRINOFFSET  (ST_OUCHOFFSET + 1)
#define ST_RAMPAGEOFFSET  (ST_EVILGRINOFFSET + 1)
#define ST_GODFACE    (ST_NUMPAINFACES*ST_FACESTRIDE)
#define ST_DEADFACE    (ST_GODFACE+1)

#define ST_FACESX    ST_LX(143)
#define ST_FACESY    ST_LY(168)

#define ST_EVILGRINCOUNT  (2*TICRATE)
#define ST_STRAIGHTFACECOUNT (TICRATE/2)
#define ST_TURNCOUNT   (1*TICRATE)
#define ST_OUCHCOUNT   (1*TICRATE)
#define ST_RAMPAGEDELAY   (2*TICRATE)

#define ST_MUCHPAIN    20


// Location and size of statistics,
//  justified according to widget type.
// Problem is, within which space? STbar? Screen?
// Note: this could be read in by a lump.
//       Problem is, is the stuff rendered
//       into a buffer,
//       or into the frame buffer?

// AMMO number pos.
#define ST_AMMOWIDTH  3 
#define ST_AMMOX   ST_LX(44)
#define ST_AMMOY   ST_LY(171)

// HEALTH number pos.
#define ST_HEALTHWIDTH  3 
#define ST_HEALTHX   ST_LX(90)
#define ST_HEALTHY   ST_LY(171)

// Weapon pos.
#define ST_ARMSX   ST_LX(111)
#define ST_ARMSY   ST_LY(172)
#define ST_ARMSBGX   ST_LX(104)
#define ST_ARMSBGY   ST_LY(168)
#define ST_ARMSXSPACE  12
#define ST_ARMSYSPACE  10

// Frags pos.
#define ST_FRAGSX   ST_LX(138)
#define ST_FRAGSY   ST_LY(171)
#define ST_FRAGSWIDTH  2

// ARMOR number pos.
#define ST_ARMORWIDTH  3
#define ST_ARMORX   ST_LX(221)
#define ST_ARMORY   ST_LY(171)

// Key icon positions.
#define ST_KEY0WIDTH  8
#define ST_KEY0HEIGHT  5
#define ST_KEY0X   ST_LX(239)
#define ST_KEY0Y   ST_LY(171)
#define ST_KEY1WIDTH  ST_KEY0WIDTH
#define ST_KEY1X   ST_LX(239)
#define ST_KEY1Y   ST_LY(181)
#define ST_KEY2WIDTH  ST_KEY0WIDTH
#define ST_KEY2X   ST_LX(239)
#define ST_KEY2Y   ST_LY(191)

// Ammunition counter.
#define ST_AMMO0WIDTH  3
#define ST_AMMO0HEIGHT  6
#define ST_AMMO0X   ST_LX(288)
#define ST_AMMO0Y   ST_LY(173)
#define ST_AMMO1WIDTH  ST_AMMO0WIDTH
#define ST_AMMO1X   ST_LX(288)
#define ST_AMMO1Y   ST_LY(179)
#define ST_AMMO2WIDTH  ST_AMMO0WIDTH
#define ST_AMMO2X   ST_LX(288)
#define ST_AMMO2Y   ST_LY(191)
#define ST_AMMO3WIDTH  ST_AMMO0WIDTH
#define ST_AMMO3X   ST_LX(288)
#define ST_AMMO3Y   ST_LY(185)

// Indicate maximum ammunition.
// Only needed because backpack exists.
#define ST_MAXAMMO0WIDTH 3
#define ST_MAXAMMO0HEIGHT 5
#define ST_MAXAMMO0X  ST_LX(314)
#define ST_MAXAMMO0Y  ST_LY(173)
#define ST_MAXAMMO1WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO1X  ST_LX(314)
#define ST_MAXAMMO1Y  ST_LY(179)
#define ST_MAXAMMO2WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO2X  ST_LX(314)
#define ST_MAXAMMO2Y  ST_LY(191)
#define ST_MAXAMMO3WIDTH ST_MAXAMMO0WIDTH
#define ST_MAXAMMO3X  ST_LX(314)
#define ST_MAXAMMO3Y  ST_LY(185)

// pistol
#define ST_WEAPON0X   ST_LX(110)
#define ST_WEAPON0Y   ST_LY(172)

// shotgun
#define ST_WEAPON1X   ST_LX(122)
#define ST_WEAPON1Y   ST_LY(172)

// chain gun
#define ST_WEAPON2X   ST_LX(134)
#define ST_WEAPON2Y   ST_LY(172)

// missile launcher
#define ST_WEAPON3X   ST_LX(110)
#define ST_WEAPON3Y   ST_LY(181)

// plasma gun
#define ST_WEAPON4X   ST_LX(122)
#define ST_WEAPON4Y   ST_LY(181)

// bfg
#define ST_WEAPON5X   ST_LX(134)
#define ST_WEAPON5Y   ST_LY(181)

// WPNS title
#define ST_WPNSX   ST_LX(109)
#define ST_WPNSY   ST_LY(191)

// DETH title
#define ST_DETHX   ST_LX(109)
#define ST_DETHY   ST_LY(191)

//Incoming messages window location
//UNUSED
// #define ST_MSGTEXTX    (viewwindowx)
// #define ST_MSGTEXTY    (viewwindowy+viewheight-18)
#define ST_MSGTEXTX   0
#define ST_MSGTEXTY   0
// Dimensions given in characters.
#define ST_MSGWIDTH   52
// Or shall I say, in lines?
#define ST_MSGHEIGHT  1

#define ST_OUTTEXTX   0
#define ST_OUTTEXTY   6

// Width, in characters again.
#define ST_OUTWIDTH   52 
// Height, in lines. 
#define ST_OUTHEIGHT  1

#define ST_MAPWIDTH  (strlen(mapnames[(gameepisode-1)*9+(gamemap-1)]))

#define ST_MAPTITLEX  (SCREENWIDTH - ST_MAPWIDTH * ST_CHATFONTWIDTH)

#define ST_MAPTITLEY  0
#define ST_MAPHEIGHT  1


// main player in game
static player_t*   plyr; 

// ST_Start() has just been called
boolean    st_firsttime;

// used to execute ST_Init() only once
static int     veryfirsttime = 1;

// lump number for PLAYPAL
static int     lu_palette;

// used for timing
static unsigned int   st_clock;

// used for making messages go away
static int     st_msgcounter=0;

// used when in chat 
static st_chatstateenum_t st_chatstate;

// whether in automap or first-person
static st_stateenum_t  st_gamestate;

// whether left-side main status bar is active
static boolean    st_statusbaron;

// whether status bar chat is active
static boolean    st_chat;

// value of st_chat before message popped up
static boolean    st_oldchat;

// whether chat window has the cursor on
static boolean    st_cursoron;

// !deathmatch
static boolean    st_notdeathmatch; 

// !deathmatch && st_statusbaron
static boolean    st_armson;

// !deathmatch
static boolean    st_fragson; 

// main bar left
static const patch_t*    sbar;

// 0-9, tall numbers
static const patch_t*    tallnum[10];

// tall % sign
static const patch_t*    tallpercent;

// 0-9, short, yellow (,different!) numbers
static const patch_t*    shortnum[10];

// 3 key-cards, 3 skulls
static const patch_t*    keys[6];

// face status patches
static const patch_t*    faces[ST_NUMFACES];

// face background
static const patch_t*    faceback;

// main bar right
static const patch_t*    armsbg;

// weapon ownership patches
static const patch_t*    arms_0;
static const patch_t*    arms_1;
static const patch_t*    arms_2;
static const patch_t*    arms_3;
static const patch_t*    arms_4;
static const patch_t*    arms_5;

// ready-weapon widget
static st_number_t   w_ready;

// in deathmatch only, summary of frags stats
static st_number_t   w_frags;

// health widget
static st_percent_t   w_health;

// arms background
static st_binicon_t   w_armsbg; 


// weapon ownership widgets
static st_binicon_t   w_arms_0;
static st_binicon_t   w_arms_1;
static st_binicon_t   w_arms_2;
static st_binicon_t   w_arms_3;
static st_binicon_t   w_arms_4;
static st_binicon_t   w_arms_5;

// face status widget
static st_multicon_t  w_faces; 

// keycard widgets
static st_multicon_t  w_keyboxes_0;
static st_multicon_t  w_keyboxes_1;
static st_multicon_t  w_keyboxes_2;

// armor widget
static st_percent_t   w_armor;

// ammo widgets
static st_number_t   w_ammo[4];

// max ammo widgets
static st_number_t   w_maxammo[4]; 



// number of frags so far in deathmatch
static int st_fragscount;

// used to use appopriately pained face
static int st_oldhealth = -1;

// used for evil grin
static boolean oldweaponsowned[NUMWEAPONS]; 

// count until face changes
static int st_facecount = 0;

// current face index, used by w_faces
static int st_faceindex = 0;

// holds key-type for each key box on bar
static int keyboxes_0;
static int keyboxes_1;
static int keyboxes_2;

// a random number per tick
static int st_randomnumber;  


// 
extern const char mapnames[45][35];


//
// STATUS BAR CODE
//
void ST_Stop(void);

void ST_refreshBackground(void)
{

  if (st_statusbaron)
  {
#if SCREENWIDTH != ORIGWIDTH
    /* CGDoom: the strips beside the bar get the view border's flat (as
       Crispy Doom does), with the border's bottom edge along their top */
    {
      const byte *flat = (const byte *)W_CacheLumpNameConst (
        gamemode == commercial ? "GRNROCK" : "FLOOR7_2", PU_CACHE);
      const patch_t *edge;
      byte *dest = screens[BG];
      int x, y;

      for (y = 0; y < ST_HEIGHT; y++)
        for (x = 0; x < SCREENWIDTH; x++)
          *dest++ = flat[((y + ST_Y) & 63)*64 + (x & 63)];
      /* (only now: this lookup may purge the flat) */
      edge = W_CacheLumpNamePatch ("BRDR_B", PU_CACHE);
      for (x = 0; x < ST_BARX; x += 8)
      {
        V_DrawPatch(x, 0, BG, edge);
        V_DrawPatch(SCREENWIDTH - 8 - x, 0, BG, edge);
      }
    }
#endif
    V_DrawPatch(ST_BARX, 0, BG, sbar);

    if (netgame)
    {
      /* CGDoom: only loaded for netgames at startup; multiplayer demos set
         netgame later */
      if (!faceback)
      {
        char namebuf[9];
        sprintf(namebuf, "STFB%d", consoleplayer);
        faceback = W_CacheLumpNamePatch(namebuf, PU_STATIC);
      }
      V_DrawPatch(ST_FX, 0, BG, faceback);
    }

    V_CopyRect(ST_X, 0, BG, ST_WIDTH, ST_HEIGHT, ST_X, ST_Y, FG);
  }
}


/* CGDoom special keys (i_system.c). plyr is set by ST_Start(), when the
   first level starts: on the title screen before that there is no player.
   The setting keys still apply there, without a message; cheat and noclip
   do nothing (otherwise a NULL write, a crash on the calculator). */
static void ST_SpecialMessage(char *message)
{
  if (plyr)
    plyr->message = message;
}

void CGD_Cheat(void)
{
  int i;
  if (!plyr)
    return;
  plyr->cheats ^= CF_GODMODE;
  if (plyr->cheats & CF_GODMODE)
  {
    //if (plyr->mo)plyr->mo->health = 100;

    plyr->health = 200;
    //plyr->message = STSTR_DQDON;
  }



  plyr->armorpoints = 200;
  plyr->armortype = 2;

  for (i=0;i<NUMWEAPONS;i++)
    plyr->weaponowned[i] = true;

  for (i=0;i<NUMAMMO;i++)
    plyr->ammo[i] = plyr->maxammo[i];

  for (i=0;i<NUMCARDS;i++)
    plyr->cards[i] = true;
  
  plyr->message = STSTR_KFAADDED;

}

void CGD_SwitchClip(void)
{
  if (!plyr)
    return;
  plyr->cheats ^= CF_NOCLIP;
  if(plyr->cheats & CF_NOCLIP)
  {
    plyr->message = STSTR_NCON;
  }
  else
  {
    plyr->message = STSTR_NCOFF;
  }
}

void CGD_CycleGamma(void)
{
  extern int usegamma;
  usegamma = (usegamma + 1) % 5;
  I_SetPalette(NULL);

  extern char gammamsg[5][26];
  ST_SpecialMessage(gammamsg[usegamma]);
}

// Respond to keyboard input events,
//  intercept cheats.
boolean ST_Responder (event_t* ev)
{
  // Filter automap on/off.
  if (ev->type == ev_keyup && ((ev->data1 & 0xffff0000) == AM_MSGHEADER))
  {
    switch(ev->data1)
    {
    case AM_MSGENTERED:
      st_gamestate = AutomapState;
      st_firsttime = true;
      break;

    case AM_MSGEXITED:
      // fprintf(stderr, "AM exited\n");
      st_gamestate = FirstPersonState;
      break;
    }
  }

  return false;
}



int ST_calcPainOffset(void)
{
  int  health;
  static int lastcalc;
  static int oldhealth = -1;

  health = plyr->health > 100 ? 100 : plyr->health;

  if (health != oldhealth)
  {
    lastcalc = ST_FACESTRIDE * (((100 - health) * ST_NUMPAINFACES) / 101);
    oldhealth = health;
  }
  return lastcalc;
}


//
// This is a not-very-pretty routine which handles
//  the face states and their timing.
// the precedence of expressions is:
//  dead > evil grin > turned head > straight ahead
//
void ST_updateFaceWidget(void)
{
  int  i;
  angle_t badguyangle;
  angle_t diffang;
  static int lastattackdown = -1;
  static int priority = 0;
  boolean doevilgrin;

  if (priority < 10)
  {
    // dead
    if (!plyr->health)
    {
      priority = 9;
      st_faceindex = ST_DEADFACE;
      st_facecount = 1;
    }
  }

  if (priority < 9)
  {
    if (plyr->bonuscount)
    {
      // picking up bonus
      doevilgrin = false;

      for (i=0;i<NUMWEAPONS;i++)
      {
        if (oldweaponsowned[i] != plyr->weaponowned[i])
        {
          doevilgrin = true;
          oldweaponsowned[i] = plyr->weaponowned[i];
        }
      }
      if (doevilgrin) 
      {
        // evil grin if just picked up weapon
        priority = 8;
        st_facecount = ST_EVILGRINCOUNT;
        st_faceindex = ST_calcPainOffset() + ST_EVILGRINOFFSET;
      }
    }

  }

  if (priority < 8)
  {
    if (plyr->damagecount && plyr->attacker && plyr->attacker != plyr->mo)
    {
      // being attacked
      priority = 7;

      if (st_oldhealth - plyr->health > ST_MUCHPAIN)
      {
        st_facecount = ST_TURNCOUNT;
        st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
      }
      else
      {
        badguyangle = R_PointToAngle2(plyr->mo->x,plyr->mo->y,plyr->attacker->x,plyr->attacker->y);

        if (badguyangle > plyr->mo->angle)
        {
          // whether right or left
          diffang = badguyangle - plyr->mo->angle;
          i = diffang > ANG180; 
        }
        else
        {
          // whether left or right
          diffang = plyr->mo->angle - badguyangle;
          i = diffang <= ANG180; 
        } // confusing, aint it?


        st_facecount = ST_TURNCOUNT;
        st_faceindex = ST_calcPainOffset();

        if (diffang < ANG45)
        {
          // head-on    
          st_faceindex += ST_RAMPAGEOFFSET;
        }
        else if (i)
        {
          // turn face right
          st_faceindex += ST_TURNOFFSET;
        }
        else
        {
          // turn face left
          st_faceindex += ST_TURNOFFSET+1;
        }
      }
    }
  }

  if (priority < 7)
  {
    // getting hurt because of your own damn stupidity
    if (plyr->damagecount)
    {
      if (st_oldhealth - plyr->health > ST_MUCHPAIN)
      {
        priority = 7;
        st_facecount = ST_TURNCOUNT;
        st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
      }
      else
      {
        priority = 6;
        st_facecount = ST_TURNCOUNT;
        st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
      }

    }

  }

  if (priority < 6)
  {
    // rapid firing
    if (plyr->attackdown)
    {
      if (lastattackdown==-1)
        lastattackdown = ST_RAMPAGEDELAY;
      else if (!--lastattackdown)
      {
        priority = 5;
        st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
        st_facecount = 1;
        lastattackdown = 1;
      }
    }
    else
      lastattackdown = -1;

  }

  if (priority < 5)
  {
    // invulnerability
    if ((plyr->cheats & CF_GODMODE) || plyr->powers[pw_invulnerability])
    {
      priority = 4;

      st_faceindex = ST_GODFACE;
      st_facecount = 1;

    }

  }

  // look left or look right if the facecount has timed out
  if (!st_facecount)
  {
    st_faceindex = ST_calcPainOffset() + (st_randomnumber % 3);
    st_facecount = ST_STRAIGHTFACECOUNT;
    priority = 0;
  }
  st_facecount--;
}

void ST_updateWidgets(void)
{
  static int largeammo = 1994; // means "n/a"
  int  i;

  // must redirect the pointer if the ready weapon has changed.
  //  if (w_ready.data != plyr->readyweapon)
  //  {
  if (weaponinfo[plyr->readyweapon].ammo == am_noammo)
    w_ready.num = &largeammo;
  else
    w_ready.num = &plyr->ammo[weaponinfo[plyr->readyweapon].ammo];

  w_ready.data = plyr->readyweapon;

  // update keycard multiple widgets
  keyboxes_0 = plyr->cards[0] ? 0 : -1;
  if (plyr->cards[3]) keyboxes_0 = 3;
  keyboxes_1 = plyr->cards[1] ? 1 : -1;
  if (plyr->cards[4]) keyboxes_1 = 4;
  keyboxes_2 = plyr->cards[2] ? 2 : -1;
  if (plyr->cards[5]) keyboxes_2 = 5;

  // refresh everything if this is him coming back to life
  ST_updateFaceWidget();

  // used by the w_armsbg widget
  st_notdeathmatch = 1;//!deathmatch;

  // used by w_arms[] widgets
  st_armson = st_statusbaron && !deathmatch; 

  // used by w_frags widget
  st_fragson = deathmatch && st_statusbaron; 
  st_fragscount = 0;

  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (i != consoleplayer)
      st_fragscount += plyr->frags[i];
    else
      st_fragscount -= plyr->frags[i];
  }

  // get rid of chat window if up because of message
  if (!--st_msgcounter)
    st_chat = st_oldchat;

}

void ST_Ticker (void)
{

  st_clock++;
  st_randomnumber = M_Random();
  ST_updateWidgets();
  st_oldhealth = plyr->health;

}

static int st_palette = 0;

void ST_doPaletteStuff(void)
{
  int   palette;
  byte* pal;
  int   cnt;
  int   bzc;

  cnt = plyr->damagecount;

  if (plyr->powers[pw_strength])
  {
    // slowly fade the berzerk out
    bzc = 12 - (plyr->powers[pw_strength]>>6);

    if (bzc > cnt)
      cnt = bzc;
  }

  if (cnt)
  {
    palette = (cnt+7)>>3;

    if (palette >= NUMREDPALS)
      palette = NUMREDPALS-1;

    palette += STARTREDPALS;
  }

  else if (plyr->bonuscount)
  {
    palette = (plyr->bonuscount+7)>>3;

    if (palette >= NUMBONUSPALS)
      palette = NUMBONUSPALS-1;

    palette += STARTBONUSPALS;
  }

  else if ( plyr->powers[pw_ironfeet] > 4*32
      || plyr->powers[pw_ironfeet]&8)
    palette = RADIATIONPAL;
  else
    palette = 0;

  if (palette != st_palette)
  {
    st_palette = palette;
    pal = (byte *) W_CacheLumpNumConst (lu_palette, PU_CACHE)+palette*768;
    I_SetPalette (pal);
  }
}


#if CGD_EXTRAS
//
// CGDoom: the colours of the ready ammo, health and armour numbers
// (CGD_FEAT_COLORNUMS, as Crispy Doom's coloured HUD): NULL (the numbers'
// own red) or one of v_crmap's translations
//
static const byte *ST_AmmoColor (void)
{
  ammotype_t type = weaponinfo[plyr->readyweapon].ammo;
  int ammo, full;

  if (type == am_noammo)
    return NULL;
  ammo = plyr->ammo[type];
  full = maxammo[type];
  if (ammo < full/4)
    return NULL;
  if (ammo < full/2)
    return v_crmap[CR_GOLD];
  if (ammo <= full)
    return v_crmap[CR_GREEN];
  return v_crmap[CR_BLUE];
}

static const byte *ST_HealthColor (void)
{
  if ((plyr->cheats & CF_GODMODE) || plyr->powers[pw_invulnerability])
    return v_crmap[CR_GRAY];
  if (plyr->health < 25)
    return NULL;
  if (plyr->health < 50)
    return v_crmap[CR_GOLD];
  if (plyr->health <= 100)
    return v_crmap[CR_GREEN];
  return v_crmap[CR_BLUE];
}

static const byte *ST_ArmorColor (void)
{
  if ((plyr->cheats & CF_GODMODE) || plyr->powers[pw_invulnerability])
    return v_crmap[CR_GRAY];
  if (plyr->armortype >= 2)
    return v_crmap[CR_BLUE];
  if (plyr->armortype == 1)
    return v_crmap[CR_GREEN];
  return NULL;
}

/* A number in its colour; redrawn when only the colour changed */
static void ST_UpdateColored (st_number_t *num, st_percent_t *per,
  const byte *map, const byte **oldmap, boolean refresh)
{
  if (!(CGD_Features & CGD_FEAT_COLORNUMS))
    map = NULL;
  if (map != *oldmap)
    refresh = true;
  *oldmap = map;
  v_patchmap = map;
  if (per)
    STlib_updatePercent(per, refresh);
  else
    STlib_updateNum(num, refresh);
  v_patchmap = NULL;
}

//
// CGDoom: the full-screen HUD (CGD_FEAT_FULLHUD, as Crispy Doom's): at the
// largest view size, where the status bar is hidden, the ready ammo and
// health are drawn over the view at the bottom left, the armour and keys
// at the bottom right. No background: the view under them is drawn again
// every frame. Each patch has a black shadow, so that it stays readable on
// any wall; colours as on the bar (CGD_FEAT_COLORNUMS).
//

#define HUD_Y     (SCREENHEIGHT - 21)   /* the numbers' top, 5 px up */
#define HUD_KEYW  9                     /* a key and the gap after it */

/* A patch with its shadow */
static void ST_HudPatch (int x, int y, const patch_t *patch)
{
  const byte *map = v_patchmap;

  v_patchmap = v_blackmap;
  V_DrawPatch (x + 1, y + 1, FG, patch);
  v_patchmap = map;
  V_DrawPatch (x, y, FG, patch);
}

/* A tall number right-aligned at x, as STlib_drawNum (negative: 0) */
static void ST_HudNum (int x, int y, int num)
{
  int w = SHORT(tallnum[0]->width);

  if (num < 0)
    num = 0;
  if (!num)
    ST_HudPatch (x - w, y, tallnum[0]);
  while (num)
  {
    x -= w;
    ST_HudPatch (x, y, tallnum[num % 10]);
    num /= 10;
  }
}

void ST_DrawFullscreenHUD (void)
{
  ammotype_t type = weaponinfo[plyr->readyweapon].ammo;
  int w = SHORT(tallnum[0]->width);
  /* Three digits each: the ammo from x = 2, the health 18 px after it;
     the key slots (blue, yellow, red, as on the bar) at the right edge,
     centred on the numbers, the armour 5 px before them */
  int ammox = 2 + 3*w;
  int healthx = ammox + 18 + 3*w;
  int keyx = SCREENWIDTH - 2 - 3*HUD_KEYW + 2;
  int armorx = keyx - 5 - SHORT(tallpercent->width);
  boolean colored = (CGD_Features & CGD_FEAT_COLORNUMS) != 0;

  v_patchmap = colored ? ST_AmmoColor () : NULL;
  if (type != am_noammo)
    ST_HudNum (ammox, HUD_Y, plyr->ammo[type]);
  v_patchmap = colored ? ST_HealthColor () : NULL;
  ST_HudNum (healthx, HUD_Y, plyr->health);
  ST_HudPatch (healthx, HUD_Y, tallpercent);
  v_patchmap = colored ? ST_ArmorColor () : NULL;
  ST_HudNum (armorx, HUD_Y, plyr->armorpoints);
  ST_HudPatch (armorx, HUD_Y, tallpercent);
  v_patchmap = NULL;

  if (keyboxes_0 >= 0)
    ST_HudPatch (keyx, HUD_Y + 5, keys[keyboxes_0]);
  if (keyboxes_1 >= 0)
    ST_HudPatch (keyx + HUD_KEYW, HUD_Y + 5, keys[keyboxes_1]);
  if (keyboxes_2 >= 0)
    ST_HudPatch (keyx + 2*HUD_KEYW, HUD_Y + 5, keys[keyboxes_2]);
}
#endif

void ST_drawWidgets(boolean refresh)
{
  int  i;

  //used by w_arms[] widgets
  st_armson = st_statusbaron && 1;// && !deathmatch;

  // used by w_frags widget
  //st_fragson = deathmatch && st_statusbaron; 

#if CGD_EXTRAS
  static const byte *oldready, *oldhealth, *oldarmor;
  ST_UpdateColored(&w_ready, NULL, ST_AmmoColor(), &oldready, refresh);
#else
  STlib_updateNum(&w_ready, refresh);
#endif

  for (i=0;i<4;i++)
  {
    STlib_updateNum(&w_ammo[i], refresh);
    STlib_updateNum(&w_maxammo[i], refresh);
  }

#if CGD_EXTRAS
  ST_UpdateColored(NULL, &w_health, ST_HealthColor(), &oldhealth, refresh);
  ST_UpdateColored(NULL, &w_armor, ST_ArmorColor(), &oldarmor, refresh);
#else
  STlib_updatePercent(&w_health, refresh);
  STlib_updatePercent(&w_armor, refresh);
#endif

  STlib_updateBinIcon(&w_armsbg, refresh);


  STlib_updateBinIcon(&w_arms_0, refresh);
  STlib_updateBinIcon(&w_arms_1, refresh);
  STlib_updateBinIcon(&w_arms_2, refresh);
  STlib_updateBinIcon(&w_arms_3, refresh);
  STlib_updateBinIcon(&w_arms_4, refresh);
  STlib_updateBinIcon(&w_arms_5, refresh);

  //I_Error("STlib4...");

  STlib_updateMultIcon(&w_faces, refresh);

  STlib_updateMultIcon(&w_keyboxes_0, refresh);
  STlib_updateMultIcon(&w_keyboxes_1, refresh);
  STlib_updateMultIcon(&w_keyboxes_2, refresh);

}

void ST_doRefresh(void)
{

  st_firsttime = false;

  // draw status bar background to off-screen buff
  ST_refreshBackground();
  // and refresh all widgets
  ST_drawWidgets(true);

}

void ST_diffDraw(void)
{
  // update all widgets
  ST_drawWidgets(false);
}

void ST_Drawer (boolean fullscreen, boolean refresh)
{

  st_statusbaron = (!fullscreen) || AM_FULLMAP;
  st_firsttime = st_firsttime || refresh;

  // Do red-/gold-shifts from damage/items
  ST_doPaletteStuff();


  // If just after ST_Start(), refresh all
  if (st_firsttime)
    ST_doRefresh();
  // Otherwise, update as little as possible
  else ST_diffDraw();

}

void ST_loadGraphics(void)
{

  int  i;
  int  j;
  int  facenum;

  char namebuf[9];

  // Load the numbers, tall and short
  for (i=0;i<10;i++)
  {
    sprintf(namebuf, "STTNUM%d", i);
    tallnum[i] = W_CacheLumpNamePatch(namebuf, PU_STATIC);

    sprintf(namebuf, "STYSNUM%d", i);
    shortnum[i] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
  }

  // Load percent key.
  //Note: why not load STMINUS here, too?
  tallpercent = W_CacheLumpNamePatch("STTPRCNT", PU_STATIC);

  keys[0] = W_CacheLumpNamePatch("STKEYS0", PU_STATIC);
  keys[1] = W_CacheLumpNamePatch("STKEYS1", PU_STATIC);
  keys[2] = W_CacheLumpNamePatch("STKEYS2", PU_STATIC);

  if(W_CheckNumForName("STKEYS3") >= 0)
    keys[3] = W_CacheLumpNamePatch("STKEYS3", PU_STATIC);
  if(W_CheckNumForName("STKEYS4") >= 0)
    keys[4] = W_CacheLumpNamePatch("STKEYS4", PU_STATIC);
  if(W_CheckNumForName("STKEYS5") >= 0)
    keys[5] = W_CacheLumpNamePatch("STKEYS5", PU_STATIC);


  // arms background
  armsbg = W_CacheLumpNamePatch("STARMS", PU_STATIC);

  // Arms ownership widgets
  arms_0 = shortnum[2];
  arms_1 = shortnum[3];
  arms_2 = shortnum[4];
  arms_3 = shortnum[5];
  arms_4 = shortnum[6];
  arms_5 = shortnum[7];

  // face backgrounds for different color players
  sprintf(namebuf, "STFB%d", consoleplayer);
  if (netgame)
  {
    faceback = W_CacheLumpNamePatch(namebuf, PU_STATIC);
  }

  // status bar background bits
  sbar = (const patch_t *) W_CacheLumpNameConst("STBAR", PU_STATIC);

  // face states
  facenum = 0;
  for (i=0;i<ST_NUMPAINFACES;i++)
  {
    for (j=0;j<ST_NUMSTRAIGHTFACES;j++)
    {
      sprintf(namebuf, "STFST%d%d", i, j);
      faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
    }
    sprintf(namebuf, "STFTR%d0", i); // turn right
    faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
    sprintf(namebuf, "STFTL%d0", i); // turn left
    faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
    sprintf(namebuf, "STFOUCH%d", i); // ouch!
    faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
    sprintf(namebuf, "STFEVL%d", i); // evil grin ;)
    faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
    sprintf(namebuf, "STFKILL%d", i); // pissed off
    faces[facenum++] = W_CacheLumpNamePatch(namebuf, PU_STATIC);
  }
  faces[facenum++] = W_CacheLumpNamePatch("STFGOD0", PU_STATIC);
  faces[facenum++] = W_CacheLumpNamePatch("STFDEAD0", PU_STATIC);

}

void ST_loadData(void)
{
  lu_palette = W_GetNumForName ("PLAYPAL");
  ST_loadGraphics();
}

void ST_unloadGraphics(void)
{

  int i;

  // unload the numbers, tall and short
  for (i=0;i<10;i++)
  {
    Z_ChangeTag(tallnum[i], PU_CACHE);
    Z_ChangeTag(shortnum[i], PU_CACHE);
  }
  // unload tall percent
  Z_ChangeTag(tallpercent, PU_CACHE); 

  // unload arms background
  Z_ChangeTag(armsbg, PU_CACHE); 

  // unload the key cards
  for (i=0;i<NUMCARDS;i++)
    Z_ChangeTag(keys[i], PU_CACHE);

  Z_ChangeTag(sbar, PU_CACHE);
  if (netgame)
  {
    Z_ChangeTag(faceback, PU_CACHE);
  }

  for (i=0;i<ST_NUMFACES;i++)
    Z_ChangeTag(faces[i], PU_CACHE);

  // Note: nobody ain't seen no unloading
  //   of stminus yet. Dude.


}

void ST_unloadData(void)
{
  ST_unloadGraphics();
}

void ST_initData(void)
{

  int  i;

  st_firsttime = true;
  plyr = &players[consoleplayer];

  st_clock = 0;
  st_chatstate = StartChatState;
  st_gamestate = FirstPersonState;

  st_statusbaron = true;
  st_oldchat = st_chat = false;
  st_cursoron = false;

  st_faceindex = 0;
  st_palette = -1;

  st_oldhealth = -1;

  for (i=0;i<NUMWEAPONS;i++)
    oldweaponsowned[i] = plyr->weaponowned[i];

  //for (i=0;i<3;i++)
  //keyboxes[i] = -1;
  keyboxes_0 = -1;
  keyboxes_1 = -1;
  keyboxes_2 = -1;

  STlib_init();

}



void ST_createWidgets(void)
{
  // ready weapon ammo
  /* CGDoom: no &ammo[am_noammo] (out of bounds); ST_updateWidgets() sets the
     real pointer before drawing */
  STlib_initNum(&w_ready,ST_AMMOX,ST_AMMOY,tallnum,
    weaponinfo[plyr->readyweapon].ammo == am_noammo ? &plyr->ammo[0] : &plyr->ammo[weaponinfo[plyr->readyweapon].ammo],
    &st_statusbaron,ST_AMMOWIDTH );

  // the last weapon type
  w_ready.data = plyr->readyweapon;

  // health percentage
  STlib_initPercent(&w_health,ST_HEALTHX,ST_HEALTHY,tallnum,&plyr->health,&st_statusbaron,tallpercent);

  // arms background
  STlib_initBinIcon(&w_armsbg,ST_ARMSBGX,ST_ARMSBGY,armsbg,&st_notdeathmatch,&st_statusbaron);

  // weapons owned
  
  STlib_initBinIcon(&w_arms_0,ST_ARMSX+(0*ST_ARMSXSPACE),ST_ARMSY+(0*ST_ARMSYSPACE),arms_0, &plyr->weaponowned[1],&st_armson);
  STlib_initBinIcon(&w_arms_1,ST_ARMSX+(1*ST_ARMSXSPACE),ST_ARMSY+(0*ST_ARMSYSPACE),arms_1, &plyr->weaponowned[2],&st_armson);
  STlib_initBinIcon(&w_arms_2,ST_ARMSX+(2*ST_ARMSXSPACE),ST_ARMSY+(0*ST_ARMSYSPACE),arms_2, &plyr->weaponowned[3],&st_armson);
  STlib_initBinIcon(&w_arms_3,ST_ARMSX+(0*ST_ARMSXSPACE),ST_ARMSY+(1*ST_ARMSYSPACE),arms_3, &plyr->weaponowned[4],&st_armson);
  STlib_initBinIcon(&w_arms_4,ST_ARMSX+(1*ST_ARMSXSPACE),ST_ARMSY+(1*ST_ARMSYSPACE),arms_4, &plyr->weaponowned[5],&st_armson);
  STlib_initBinIcon(&w_arms_5,ST_ARMSX+(2*ST_ARMSXSPACE),ST_ARMSY+(1*ST_ARMSYSPACE),arms_5, &plyr->weaponowned[6],&st_armson);

  // frags sum
  STlib_initNum(&w_frags,ST_FRAGSX,ST_FRAGSY,tallnum,&st_fragscount,&st_fragson,ST_FRAGSWIDTH);

  // faces
  STlib_initMultIcon(&w_faces,ST_FACESX,ST_FACESY,faces,&st_faceindex,&st_statusbaron);

  // armor percentage - should be colored later
  STlib_initPercent(&w_armor,ST_ARMORX,ST_ARMORY,tallnum,&plyr->armorpoints,&st_statusbaron, tallpercent);

  // keyboxes 0-2
  STlib_initMultIcon(&w_keyboxes_0,ST_KEY0X,ST_KEY0Y,keys,&keyboxes_0,&st_statusbaron);

  STlib_initMultIcon(&w_keyboxes_1,ST_KEY1X,ST_KEY1Y,keys,&keyboxes_1,&st_statusbaron);

  STlib_initMultIcon(&w_keyboxes_2,ST_KEY2X,ST_KEY2Y,keys,&keyboxes_2,&st_statusbaron);

  // ammo count (all four kinds)
  STlib_initNum(&w_ammo[0],ST_AMMO0X,ST_AMMO0Y,shortnum,&plyr->ammo[0],&st_statusbaron,ST_AMMO0WIDTH);

  STlib_initNum(&w_ammo[1],ST_AMMO1X,ST_AMMO1Y,shortnum,&plyr->ammo[1],&st_statusbaron,ST_AMMO1WIDTH);

  STlib_initNum(&w_ammo[2],ST_AMMO2X,ST_AMMO2Y,shortnum,&plyr->ammo[2],&st_statusbaron,ST_AMMO2WIDTH);

  STlib_initNum(&w_ammo[3],ST_AMMO3X,ST_AMMO3Y,shortnum,&plyr->ammo[3],&st_statusbaron,ST_AMMO3WIDTH);

  // max ammo count (all four kinds)
  STlib_initNum(&w_maxammo[0],ST_MAXAMMO0X,ST_MAXAMMO0Y,shortnum,&plyr->maxammo[0],&st_statusbaron,ST_MAXAMMO0WIDTH);

  STlib_initNum(&w_maxammo[1],ST_MAXAMMO1X,ST_MAXAMMO1Y,shortnum,&plyr->maxammo[1],&st_statusbaron,ST_MAXAMMO1WIDTH);

  STlib_initNum(&w_maxammo[2],ST_MAXAMMO2X,ST_MAXAMMO2Y,shortnum,&plyr->maxammo[2],&st_statusbaron,ST_MAXAMMO2WIDTH);

  STlib_initNum(&w_maxammo[3],ST_MAXAMMO3X,ST_MAXAMMO3Y,shortnum,&plyr->maxammo[3],&st_statusbaron,ST_MAXAMMO3WIDTH);

}

static boolean st_stopped = true;


void ST_Start (void)
{

  if (!st_stopped)
    ST_Stop();

  ST_initData();
  ST_createWidgets();
  st_stopped = false;

}

void ST_Stop (void)
{
  if (st_stopped)
    return;

  st_stopped = true;
}

void ST_Init (void)
{
  veryfirsttime = 0;
  ST_loadData();
#if CGD_EXTRAS
  V_BuildColorMaps ((const byte *)W_CacheLumpNameConst ("PLAYPAL", PU_CACHE));
#endif
  screens[4] = I_ScreenBase(4);
}
