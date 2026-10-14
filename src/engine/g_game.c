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
// DESCRIPTION:  none
//
//-----------------------------------------------------------------------------


//static const char 

#include "os.h"
#include "cgdoom.h"

#include "doomdef.h" 
#include "doomstat.h"

#include "z_zone.h"
#include "f_finale.h"
//#include "m_argv.h"
#include "m_misc.h"
#include "m_menu.h"
#include "m_random.h"
#include "i_system.h"

#include "p_setup.h"
#include "p_saveg.h"
#include "p_tick.h"

#include "d_main.h"

#include "wi_stuff.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "am_map.h"

// Needs access to LFB.
#include "v_video.h"

#include "w_wad.h"

#include "p_local.h" 

// Data.
#include "dstrings.h"

// SKY handling - still the wrong place.
#include "r_data.h"
#include "r_sky.h"



#include "g_game.h"


/* CGDoom: Size of the temporary savegame buffer (vanilla value). It used to
   be the screens area, which an error screen during a save would destroy. */
#define SAVEGAMESIZE 0x2c000
#define SAVESTRINGSIZE 24



boolean     G_CheckDemoStatus (void); 
void     G_ReadDemoTiccmd (ticcmd_t* cmd); 
void     G_WriteDemoTiccmd (ticcmd_t* cmd); 
void     G_PlayerReborn (int player); 
void     G_InitNew (skill_t skill, int episode, int map); 

void     G_DoReborn (void); 

void     G_DoLoadLevel (void); 
void     G_DoNewGame (void); 
void     G_DoLoadGame (void); 
void     G_DoPlayDemo (void);
void     G_DoCompleted (void); 
void     G_DoVictory (void); 
void     G_DoWorldDone (void); 
void     G_DoSaveGame (void);


gameaction_t   gameaction; 
gamestate_t    gamestate; 
skill_t     gameskill; 
boolean     respawnmonsters;
int      gameepisode; 
int      gamemap; 

boolean     paused; 
boolean     sendpause;              // send a pause event next tic 
boolean     sendsave;              // send a save event next tic 
boolean     usergame;               // ok to save / end game 

boolean     timingdemo;             // if true, exit with report on completion 
boolean     nodrawers;              // for comparative timing purposes 
boolean     noblit;                 // for comparative timing purposes 
int      starttime;           // for comparative timing purposes    

boolean     viewactive; 

boolean     deathmatch;            // only if started as net death 
boolean     netgame = 0;                // only true if packets are broadcast
boolean     playeringame[MAXPLAYERS]; 
player_t    players[MAXPLAYERS]; 

int consoleplayer = 0;          // player taking events and displaying
int      displayplayer;          // view being displayed 
int                gametic; 
int               levelstarttic;          // gametic at level start 
int               totalkills, totalitems, totalsecret;    // for intermission 

char      demoname[32];
boolean   demorecording;
boolean   demoplayback;
boolean   netdemo = 0;
byte*     demobuffer;
byte*     demo_p;
byte*     demoend; 
/* CGDoom: a recording is armed by G_RecordDemo (demorecording = true) but
   only begins in G_BeginRecording, after the first level load; before that
   demo_p is NULL (or points into a title demo that G_DoPlayDemo loaded into
   demobuffer). Nothing is written to the demo, and G_CheckDemoStatus drops
   an armed recording, until it has begun. G_RecordDemo's own buffer is kept
   apart so that a title demo cannot replace it. */
static boolean demorecbegun;
static byte*   demorecbuffer;
#define DEMO_MAXSIZE 0x20000
/* CGDoom: the end of the demo being played (demoend is the recording's) */
static byte*   demoplayend;
boolean   singledemo;             // quit after playing a demo from cmdline

boolean          precache = false;        // if true, load all graphics at start

wbstartstruct_t   wminfo;                // parms for world map / intermission 

byte*     savebuffer;


// 
// controls (have defaults) 
// 
int key_right        = KEY_RIGHTARROW;
int key_left         = KEY_LEFTARROW;

int key_up           = KEY_UPARROW;
int key_down         = KEY_DOWNARROW;
int key_strafeleft   = KEY_SRIGHTARROW;
int key_straferight  = KEY_SLEFTARROW;
int key_fire         = KEY_RCTRL;
int key_use          = 32;
int key_strafe;
int key_speed        = KEY_RSHIFT;

#define MAXPLMOVE  (forwardmove[1]) 

#define TURBOTHRESHOLD 0x32

const fixed_t     forwardmove[2] = {0x19, 0x32}; 
const fixed_t     sidemove[2] = {0x18, 0x28}; 
const fixed_t     angleturn[3] = {720, 720, 320}; // + slow turn

#define SLOWTURNTICS 6 

#define NUMKEYS   256 

boolean           gamekeydown[NUMKEYS]; 
int               turnheld;    // for accelerative turning 
int               speedtoggle = 0;    // for run toggle

int      savegameslot; 
char     savedescription[32]; 


#define BODYQUESIZE  32

mobj_t*     bodyque[BODYQUESIZE]; 
int      bodyqueslot; 

void*     statcopy;    // for statistics driver


/* I HOPE THIS ISN'T NEEDED, BECAUSE IT GIVES A STRANGE COMPILER WARNING THAT I HAVE NO IDEA WHAT TO DO WITH
int G_CmdChecksum (ticcmd_t* cmd) 
{ 
int  i;
int  sum = 0; 

for (i=0 ; i< sizeof(*cmd)/4 - 1 ; i++) 
sum += ((int *)cmd)[i]; 

return sum;
} 
*/
//
// G_BuildTiccmd
// Builds a ticcmd from all of the available inputs
// or reads it from the demo buffer. 
// If recording a demo, write it out 
// 
void G_BuildTiccmd (ticcmd_t* cmd) 
{ 
  int   i; 
  //boolean  strafe;
  int   speed;
  int   tspeed; 
  int   forward;
  int   side;

  ticcmd_t* base;

  base = I_BaseTiccmd ();  // empty, or external driver
  memcpy (cmd,base,sizeof(*cmd)); 

  //cmd->consistancy = consistancy[consoleplayer][maketic%BACKUPTICS];

  /* Bit 0 is the speed status, bit 1 is set until key is released */
  speed = (speedtoggle != 0);
  if (gamekeydown[key_speed] && !(speedtoggle & 2))
    speedtoggle = (speedtoggle ^ 1) | 2;
  if (!gamekeydown[key_speed])
    speedtoggle &= ~2;

  forward = side = 0;

  // use two stage accelerative turning on the keyboard
  if (gamekeydown[key_right] || gamekeydown[key_left]) 
    turnheld++;
  else 
    turnheld = 0;

  if (turnheld < SLOWTURNTICS)
    tspeed = 2;             // slow turn
  else
    tspeed = speed;

  if (gamekeydown[key_right])
    cmd->angleturn -= angleturn[tspeed];
  if (gamekeydown[key_left])
    cmd->angleturn += angleturn[tspeed];

  if (gamekeydown[key_up])
    forward= +forwardmove[speed];

  if (gamekeydown[key_down])
    forward= -forwardmove[speed];

  if (gamekeydown[key_strafeleft])
    side= +sidemove[speed];
  if (gamekeydown[key_straferight])
    side= -sidemove[speed];

  // buttons
  //cmd->chatchar = HU_dequeueChatChar(); 

  if (gamekeydown[key_fire])
    cmd->buttons |= BT_ATTACK;

  if (gamekeydown[key_use] ) 
    cmd->buttons |= BT_USE;

  // chainsaw overrides 
  for (i=0 ; i<NUMWEAPONS-1 ; i++)
    if (gamekeydown['1'+i]) 
    { 
      cmd->buttons |= BT_CHANGE; 
      cmd->buttons |= i<<BT_WEAPONSHIFT; 
      break; 
    }

  if (forward > MAXPLMOVE)
  {
    forward = MAXPLMOVE;
  }
  else if (forward < -MAXPLMOVE)
  {
    forward = -MAXPLMOVE;
  }

  if (side > MAXPLMOVE)
  {
    side = MAXPLMOVE;
  }
  else if (side < -MAXPLMOVE)
  {
    side = -MAXPLMOVE;
  }

  cmd->forwardmove= forward;
  cmd->sidemove= side;

  // special buttons
  if (sendpause)
  {
    sendpause = false;
    cmd->buttons = BT_SPECIAL | BTS_PAUSE;
  }

  if (sendsave)
  {
    sendsave = false;
    cmd->buttons = (byte)(BT_SPECIAL | BTS_SAVEGAME | (savegameslot<<BTS_SAVESHIFT));
  }
} 


//
// G_DoLoadLevel 
//
extern  gamestate_t     wipegamestate; 

void G_DoLoadLevel (void) 
{ 
  //int             i; 

  // Set the sky map.
  // First thing, we have a dummy sky texture name,
  //  a flat. The data is in the WAD only because
  //  we look for an actual index, instead of simply
  //  setting one.
  skyflatnum = R_FlatNumForName ( SKYFLATNAME );

  // DOOM determines the sky texture to be used
  // depending on the current episode, and the game version.
  if ( (gamemode == commercial) || ( gamemode == pack_tnt ) || ( gamemode == pack_plut ) )
  {
    skytexture = R_TextureNumForName ("SKY3");
    if (gamemap < 12)
      skytexture = R_TextureNumForName ("SKY1");
    else
      if (gamemap < 21)
        skytexture = R_TextureNumForName ("SKY2");
  }

  levelstarttic = gametic;        // for time calculation

  if (wipegamestate == GS_LEVEL) 
    wipegamestate = (gamestate_t)-1;             // force a wipe 

  gamestate = GS_LEVEL;

  /*for (i=0 ; i<MAXPLAYERS ; i++) 
  { 
  if (playeringame[i] && players[i].playerstate == PST_DEAD) 
  players[i].playerstate = PST_REBORN; 
  memset (players[i].frags,0,sizeof(players[i].frags)); 
  } */
  // (vanilla: a player still dead when a level loads is reborn)
  if (playeringame[0] && players[0].playerstate == PST_DEAD) 
    players[0].playerstate = PST_REBORN; 
  memset (players[0].frags,0,sizeof(players[0].frags)); 

  P_SetupLevel (gameepisode, gamemap, 0);    
  displayplayer = consoleplayer;  // view the guy you are playing    
  starttime = I_GetTime (); 
  gameaction = ga_nothing; 
  //Z_CheckHeap ();


  // clear cmd building stuff
  memset (gamekeydown, 0, sizeof(gamekeydown)); 
  sendpause = sendsave = paused = false; 
} 


//
// G_Responder  
// Get info needed to make ticcmd_ts for the players.
// 
boolean G_Responder (event_t* ev) 
{
  /*// allow spy mode changes even during the demo
  if (gamestate == GS_LEVEL && ev->type == ev_keydown && ev->data1 == KEY_F12 && (singledemo || !deathmatch) )
  {
  // spy mode 
  do 
  { 
  displayplayer++; 
  if (displayplayer == MAXPLAYERS) 
  displayplayer = 0; 
  } while (!playeringame[displayplayer] && displayplayer != consoleplayer); 
  return true; 
  }*/

  // any other key pops up menu if in demos
  /* CGDoom: vanilla's demoplayback is back in the condition, now that the
     title loop plays its demos: CGDoom (demos off by default) only opened
     the menu on a key press over a title page */
  if (gameaction == ga_nothing && !singledemo &&
      (demoplayback || gamestate == GS_DEMOSCREEN) ) 
  { 
    if (ev->type == ev_keydown ) 
    { 
      M_StartControlPanel (); 
      return true; 
    }
    return false; 
  } 

  //DEBUG DEBUG DEBUG
  //ONE of those three keeps crashing when there's an event, can't be bothered to find which one
  if (gamestate == GS_LEVEL) 
  { 
    if (HU_Responder (ev)) 
      return true; // chat ate the event 
    //if (ST_Responder (ev)) 
    // return true; // status window ate it 
    if (AM_Responder (ev)) 
      return true; // automap ate it
  } 

  if (gamestate == GS_FINALE) 
  { 
    if (F_Responder (ev)) 
      return true; // finale ate the event 
  } 

  switch (ev->type) 
  { 
  case ev_keydown: 
    if (ev->data1 == KEY_PAUSE) 
    { 
      sendpause = true; 
      return true; 
    } 
    if (ev->data1 <NUMKEYS)
    {
      gamekeydown[ev->data1] = true;
    }
    return true;    // eat key down events 

  case ev_keyup: 
    if (ev->data1 <NUMKEYS) 
      gamekeydown[ev->data1] = false; 
    return false;   // always let key up events filter down

  default: 
    break; 
  } 

  return false; 
} 



//
// G_Ticker
// Make ticcmd_ts for the players.
//
// CGDoom: called at the end of every G_Ticker when set (host -demotrace
// writes a per-tic game-state trace); it must not change game state
void (*G_TicEndHook) (void) = NULL;

void G_Ticker (void) 
{ 
  //int   i;
  ticcmd_t* cmd;

  // do player reborns if needed
  //for (i=0 ; i<MAXPLAYERS ; i++) 
  if (playeringame[0] && players[0].playerstate == PST_REBORN) 
    G_DoReborn ();

  // do things to change the game state
  while (gameaction != ga_nothing) 
  { 
    switch (gameaction) 
    { 
    case ga_loadlevel: 
      G_DoLoadLevel (); 
      break; 
    case ga_newgame: 
      G_DoNewGame (); 
      break; 
    case ga_loadgame: 
      G_DoLoadGame (); 
      break; 
    case ga_savegame: 
      G_DoSaveGame ();
      break;
    case ga_playdemo:
      G_DoPlayDemo ();
      break;
    case ga_completed: 
      G_DoCompleted (); 
      break; 
    case ga_victory: 
      F_StartFinale (); 
      break; 
    case ga_worlddone: 
      G_DoWorldDone (); 
      break; 
    case ga_nothing: 
      break; 
    } 
  }

  //I_Error("loopdaloop2"); //fine as far as this...

  // get commands
  // check for special buttons
  // CGDoom (external review): while the menu pauses the level (P_Ticker),
  // a recording takes no ticcmd at all. Playback, where the menu never
  // pauses, must run exactly the tics the recording ran; vanilla recorded
  // these and the demo desynced. A pause or save request made meanwhile
  // waits in sendpause/sendsave for the first tic after the menu.
  if (playeringame[0]
    && !(demorecording && gamestate == GS_LEVEL && P_MenuPaused ()))
  { 
    cmd = &players[0].cmd;

    if (demoplayback)
    {
      G_ReadDemoTiccmd (cmd);
      // -playdemo only (CGD_PlayDemoOnly): at the demo marker vanilla's
      // singledemo quits inside G_CheckDemoStatus (I_Quit), so this tic
      // never runs: stop here too, the final state is the demo's
      if (fuck)
        return;
    }
    else
      G_BuildTiccmd (cmd);

    if (demorecording && demorecbegun)
      G_WriteDemoTiccmd (cmd);

    if (players[0].cmd.buttons & BT_SPECIAL) 
    { 
      switch (players[0].cmd.buttons & BT_SPECIALMASK) 
      { 
      case BTS_PAUSE: 
        paused ^= 1; 
        break; 

      case BTS_SAVEGAME: 
        /* CGDoom: a demo's ticcmds come here too; its slot must be one of
           the menu's (6 is the recorded demo's delayed write, 7 was past
           CGD_DelayedSaves) */
        if (((players[0].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT)
          >= CGD_DELAYEDSAVES_DEMO - CGD_DELAYEDSAVES_SAVES)
          break;

        if (!savedescription[0]) 
          strcpy(savedescription, "NET GAME");

        savegameslot =  (players[0].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT; 
        gameaction = ga_savegame;
        break; 
      } 
    } 
  }

  //I_Error("loopdaloop3"); 

  // do main actions
  switch (gamestate) 
  { 
  case GS_LEVEL: 
    P_Ticker ();
    ST_Ticker ();
    AM_Ticker ();
    HU_Ticker ();
    break; 

  case GS_INTERMISSION: 
    WI_Ticker (); 
    break; 

  case GS_FINALE: 
    F_Ticker (); 
    break; 

  case GS_DEMOSCREEN: 
    D_PageTicker (); 
    break; 
  }        

  // CGDoom: per-tic observer (host -demotrace); NULL on the calculator
  if (G_TicEndHook)
    G_TicEndHook ();
}


//
// PLAYER STRUCTURE FUNCTIONS
// also see P_SpawnPlayer in P_Things
//

//
// G_InitPlayer 
// Called at the start.
// Called by the game initialization functions.
//
void G_InitPlayer (int player) 
{ 
  player_t*  p CGD_UNUSED; 

  // set up the saved info         
  p = &players[player]; 

  // clear everything else to defaults 
  G_PlayerReborn (player); 
} 



//
// G_PlayerFinishLevel
// Can when a player completes a level.
//
void G_PlayerFinishLevel (int player) 
{ 
  player_t* p; 

  p = &players[player]; 

  memset (p->powers, 0, sizeof (p->powers)); 
  memset (p->cards, 0, sizeof (p->cards)); 

  p->mo->flags &= ~MF_SHADOW;  // cancel invisibility 
  p->extralight = 0;   // cancel gun flashes 
  p->fixedcolormap = 0;  // cancel ir gogles 
  p->damagecount = 0;   // no palette changes 
  p->bonuscount = 0; 
} 


//
// G_PlayerReborn
// Called after a player dies 
// almost everything is cleared and initialized 
//
void G_PlayerReborn (int player) 
{ 
  player_t* p; 
  int  i; 
  int  frags[MAXPLAYERS]; 
  int  killcount;
  int  itemcount;
  int  secretcount; 

  memcpy (frags,players[player].frags,sizeof(frags)); 
  killcount = players[player].killcount; 
  itemcount = players[player].itemcount; 
  secretcount = players[player].secretcount; 

  p = &players[player]; 
  memset (p, 0, sizeof(*p)); 

  memcpy (players[player].frags, frags, sizeof(players[player].frags)); 
  players[player].killcount = killcount; 
  players[player].itemcount = itemcount; 
  players[player].secretcount = secretcount; 

  p->usedown = p->attackdown = true; // don't do anything immediately 
  p->playerstate = PST_LIVE;       
  p->health = MAXHEALTH; 
  p->readyweapon = p->pendingweapon = wp_pistol; 
  p->weaponowned[wp_fist] = true; 
  p->weaponowned[wp_pistol] = true; 
  p->ammo[am_clip] = 50; 

  for (i=0 ; i<NUMAMMO ; i++) 
    p->maxammo[i] = maxammo[i]; 
}

//
// G_CheckSpot  
// Returns false if the player cannot be respawned
// at the given mapthing_t spot  
// because something is occupying it 
//
void P_SpawnPlayer (mapthing_t* mthing); 

boolean G_CheckSpot ( int playernum, mapthing_t* mthing ) 
{ 
  fixed_t  x;
  fixed_t  y; 
  subsector_t* ss; 
  unsigned  an; 
  mobj_t*  mo CGD_UNUSED; 
  int   i;

  if (!players[playernum].mo)
  {
    // first spawn of level, before corpses
    for (i=0 ; i<playernum ; i++)
      if (players[i].mo->x == mthing->x << FRACBITS && players[i].mo->y == mthing->y << FRACBITS)
        return false; 
    return true;
  }

  x = mthing->x << FRACBITS; 
  y = mthing->y << FRACBITS; 

  if (!P_CheckPosition (players[playernum].mo, x, y) ) 
    return false; 

  // flush an old corpse if needed 
  if (bodyqueslot >= BODYQUESIZE) 
    P_RemoveMobj (bodyque[bodyqueslot%BODYQUESIZE]); 
  bodyque[bodyqueslot%BODYQUESIZE] = players[playernum].mo; 
  bodyqueslot++; 

  // spawn a teleport fog 
  ss = R_PointInSubsector (x,y); 
  an = (int)( (angle_t)ANG45 * (mthing->angle/45) ) >> ANGLETOFINESHIFT; 

  mo = P_SpawnMobj (x+20*finecosine(an), y+20*finesine[an], ss->sector->floorheight, MT_TFOG); 

  return true; 
} 


//
// G_DeathMatchSpawnPlayer 
// Spawns a player at one of the random death match spots 
// called at level load and each death 
//
/*void G_DeathMatchSpawnPlayer (int playernum) 
{ 
int             i,j; 
int    selections; 

selections = deathmatch_p - deathmatchstarts; 
if (selections < 4) 
I_Error ("Only %i deathmatch spots, 4 required", selections); 

for (j=0 ; j<20 ; j++) 
{ 
i = P_Random() % selections; 
if (G_CheckSpot (playernum, &deathmatchstarts[i]) ) 
{ 
deathmatchstarts[i].type = playernum+1; 
P_SpawnPlayer (&deathmatchstarts[i]); 
return; 
} 
} 

// no good spot, so the player will probably get stuck 
P_SpawnPlayer (&playerstarts[playernum]); 
} 
*/

//
// G_DoReborn 
// 
void G_DoReborn (void) 
{ 
  // reload the level from scratch
  gameaction = ga_loadlevel;
}

// DOOM Par Times
const int pars[4][10] = 
{ 
  {0}, 
  {0,30,75,120,90,165,180,180,30,165}, 
  {0,90,90,90,120,90,360,240,30,170}, 
  {0,90,45,90,150,90,90,165,30,135} 
}; 

// DOOM II Par Times
const int cpars[32] =
{
  30,90,120,120,90,150,120,120,270,90, //  1-10
    210,150,150,150,210,150,420,150,210,150, // 11-20
    240,150,180,150,150,300,330,420,300,180, // 21-30
    120,30     // 31-32
};


//
// G_DoCompleted 
//
boolean   secretexit; 
extern char* pagename; 

void G_ExitLevel (void) 
{ 
  secretexit = false; 
  gameaction = ga_completed; 
} 

// Here's for the german edition.
void G_SecretExitLevel (void) 
{ 
  // IF NO WOLF3D LEVELS, NO SECRET EXIT!
  if ( (gamemode == commercial) && (W_CheckNumForName("map31")<0))
    secretexit = false;
  else
    secretexit = true; 
  gameaction = ga_completed; 
} 

void G_DoCompleted (void) 
{ 
  //int             i; 

  gameaction = ga_nothing; 

  //for (i=0 ; i<MAXPLAYERS ; i++) 
  if (playeringame[0]) 
    G_PlayerFinishLevel (0);        // take away cards and stuff 

  if (automapactive) 
    AM_Stop ();

  if ( gamemode != commercial)
    switch(gamemap)
  {
    case 8:
      gameaction = ga_victory;
      return;
    case 9: 
      //for (i=0 ; i<MAXPLAYERS ; i++) 
      players[0].didsecret = true; 
      break;
  }

  if ( (gamemap == 8) && (gamemode != commercial) ) 
  {
    // victory 
    gameaction = ga_victory; 
    return; 
  } 

  if ( (gamemap == 9) && (gamemode != commercial) ) 
  {
    // exit secret level 
    //for (i=0 ; i<MAXPLAYERS ; i++) 
    players[0].didsecret = true; 
  }


  wminfo.didsecret = players[consoleplayer].didsecret; 
  wminfo.epsd = gameepisode -1; 
  wminfo.last = gamemap -1;

  if (CGD_SingleEpisodeUltimate)
    wminfo.epsd = CGD_SingleEpisodeUltimate - 1;

  // wminfo.next is 0 biased, unlike gamemap
  if ( gamemode == commercial)
  {
    if (secretexit)
      switch(gamemap)
    {
      case 15: wminfo.next = 30; break;
      case 31: wminfo.next = 31; break;
    }
    else
      switch(gamemap)
    {
      case 31:
      case 32: wminfo.next = 15; break;
      default: wminfo.next = gamemap;
    }
  }
  else
  {
    if (secretexit) 
      wminfo.next = 8;  // go to secret level 
    else if (gamemap == 9) 
    {
      // returning from secret level 
      switch (gameepisode) 
      { 
      case 1: 
        wminfo.next = 3; 
        break; 
      case 2: 
        wminfo.next = 5; 
        break; 
      case 3: 
        wminfo.next = 6; 
        break; 
      case 4:
        wminfo.next = 2;
        break;
      }                
    } 
    else 
      wminfo.next = gamemap;          // go to next level 
  }

  wminfo.maxkills = totalkills; 
  wminfo.maxitems = totalitems; 
  wminfo.maxsecret = totalsecret; 
  wminfo.maxfrags = 0; 

  if ( gamemode == commercial )
    wminfo.partime = 35*cpars[gamemap-1]; 
  else if ( gameepisode < 4 )
    wminfo.partime = 35*pars[gameepisode][gamemap]; 
  else
    /* CGDoom: vanilla reads past pars[] into cpars[] for episode 4 */
    wminfo.partime = 35*cpars[gamemap];
  wminfo.pnum = consoleplayer; 

  /*for (i=0 ; i<MAXPLAYERS ; i++) 
  { */
  wminfo.plyr[0].in = playeringame[0]; 
  wminfo.plyr[0].skills = players[0].killcount; 
  wminfo.plyr[0].sitems = players[0].itemcount; 
  wminfo.plyr[0].ssecret = players[0].secretcount; 
  wminfo.plyr[0].stime = leveltime; 
  memcpy (wminfo.plyr[0].frags, players[0].frags, sizeof(wminfo.plyr[0].frags)); 
  //} 

  gamestate = GS_INTERMISSION; 
  viewactive = false; 
  automapactive = false; 

  if (statcopy)
    memcpy (statcopy, &wminfo, sizeof(wminfo));

  WI_Start (&wminfo);
} 


//
// G_WorldDone 
//
void G_WorldDone (void) 
{ 
  gameaction = ga_worlddone; 

  if (secretexit) 
    players[consoleplayer].didsecret = true; 

  if ( gamemode == commercial )
  {
    switch (gamemap)
    {
    case 15:
    case 31:
      if (!secretexit)
        break;
    case 6:
    case 11:
    case 20:
    case 30:
      F_StartFinale ();
      break;
    }
  }
} 

void G_DoWorldDone (void) 
{        
  gamestate = GS_LEVEL; 
  gamemap = wminfo.next+1; 
  G_DoLoadLevel (); 
  gameaction = ga_nothing; 
  viewactive = true; 
} 



//
// G_InitFromSavegame
// Can be called by the startup code or the menu task. 
//
extern boolean setsizeneeded;
void R_ExecuteSetViewSize (void);

int   saveslotnumber;

void G_LoadGame (int slot)
{
    saveslotnumber = slot;
    gameaction = ga_loadgame;
}

#define VERSIONSIZE  16
/* CGDoom: description, version, skill, episode, map, players, leveltime */
#define SAVEHEADERSIZE (SAVESTRINGSIZE + VERSIONSIZE + 3 + MAXPLAYERS + 3)

/* CGDoom: G_DoLoadGame checks the header before G_InitNew, which destroys
   the current game: a skill, episode or map that G_InitNew would change (a
   MAP33 save would load MAP32's level with MAP33's archive) or a map the
   WAD does not have (a save of another WAD); player flags that are not 0
   or 1, and player 0 (the only one this engine runs, renders and reads
   commands for) and consoleplayer must be in the game. */
static boolean G_ValidSaveHeader (int skill, int episode, int map,
  const byte *ingame)
{
    char  lumpname[16];
    int   lastepisode;
    int   i;

    if (skill < sk_baby || skill > sk_nightmare)
        return false;

    /* G_InitNew's limits */
    if (gamemode == retail)
        lastepisode = 4;
    else if (gamemode == shareware)
        lastepisode = 1;
    else
        lastepisode = 3;
    if (episode < 1 || episode > lastepisode)
        return false;
    if (map < 1 || map > (gamemode == commercial ? 32 : 9))
        return false;

    /* P_SetupLevel's map name */
    if (gamemode == commercial)
        sprintf (lumpname, "map%02d", map);
    else
        sprintf (lumpname, "E%dM%d", episode, map);
    if (W_CheckNumForName (lumpname) < 0)
        return false;

    for (i=0 ; i<MAXPLAYERS ; i++)
        if (ingame[i] > 1)
            return false;
    return ingame[0] && ingame[consoleplayer];
}

void G_DoLoadGame (void) 
{
    int   length;
    int   i;
    int   a,b,c;
    char  vcheck[VERSIONSIZE];
    char  savename[64];
    int   skill, episode, map;
    byte  ingame[MAXPLAYERS];

    gameaction = ga_nothing;

    /* Read data from yet-unsaved saves */
    int n = CGD_DELAYEDSAVES_SAVES + saveslotnumber;
    if (CGD_DelayedSaves[n].data != NULL) {
      savebuffer = (void *)CGD_DelayedSaves[n].data;
      length = CGD_DelayedSaves[n].size;
    }
    else {
      CGD_SaveFileName(savename, saveslotnumber);
      savebuffer = NULL;
      length = M_ReadFile (savename, &savebuffer, 0);
      I_ReinitAfterError();
    }

    /* CGDoom: the whole header (vanilla only checked the description and
       the version, then read on) */
    if (length < SAVEHEADERSIZE)
    {
      if (length >= 0 && CGD_DelayedSaves[n].data == NULL)
        Z_Free (savebuffer);
      PL_Log ("G_DoLoadGame: slot %d: no valid savegame\n", saveslotnumber);
      return;
    }

    save_p = savebuffer + SAVESTRINGSIZE;
    save_end = savebuffer + length;

    // skip the description field
    memset (vcheck,0,sizeof(vcheck));
    sprintf (vcheck,"version %i",VERSION);
    if (strncmp ((const char *)save_p, vcheck, VERSIONSIZE))
    {
        if (CGD_DelayedSaves[n].data == NULL)
            Z_Free (savebuffer);
        PL_Log ("G_DoLoadGame: slot %d: no valid savegame\n", saveslotnumber);
        return;       // bad version
    }
    save_p += VERSIONSIZE;

    /* CGDoom: checked before G_InitNew (G_ValidSaveHeader) */
    skill = *save_p++;
    episode = *save_p++;
    map = *save_p++;
    for (i=0 ; i<MAXPLAYERS ; i++)
        ingame[i] = *save_p++;
    if (!G_ValidSaveHeader (skill, episode, map, ingame))
    {
        if (CGD_DelayedSaves[n].data == NULL)
            Z_Free (savebuffer);
        PL_Log ("G_DoLoadGame: slot %d: bad header (skill %d, episode %d, "
            "map %d, players %d%d%d%d)\n", saveslotnumber, skill, episode, map,
            ingame[0], ingame[1], ingame[2], ingame[3]);
        return;
    }

    /* CGDoom: gameskill keeps the current skill until G_InitNew, which
       switches the fast monsters by comparing the two (external review:
       vanilla set it first, so a Nightmare save loaded from the title of a
       fresh session had normal-speed monsters, and a normal save loaded
       during a Nightmare game kept fast ones). Demos never load saves. */
    gameepisode = episode;
    gamemap = map;
    for (i=0 ; i<MAXPLAYERS ; i++)
        playeringame[i] = ingame[i];

    // load a base level
    G_InitNew (skill, gameepisode, gamemap);

    // get the times
    a = *save_p++;
    b = *save_p++;
    c = *save_p++;
    leveltime = (a<<16) + (b<<8) + c;

    // dearchive all the modifications
    P_UnArchivePlayers ();
    P_UnArchiveWorld ();
    P_UnArchiveThinkers ();
    P_UnArchiveSpecials ();

    if (save_p >= save_end || *save_p != 0x1d)
        I_Error ("Bad savegame");

    // done
    if (CGD_DelayedSaves[n].data == NULL)
      Z_Free (savebuffer);

    if (setsizeneeded)
        R_ExecuteSetViewSize ();
}


//
// G_SaveGame
// Called by the menu task.
// Description is a 24 byte text string 
//
void G_SaveGame( int slot, char* description ) 
{ 
    savegameslot = slot;
    strcpy (savedescription, description);
    sendsave = true;
} 

void G_DoSaveGame (void)
{ 
    char  name[50];
    char  name2[VERSIONSIZE];
    char* description;
    int   length;
    int   i;

    CGD_SaveFileName(name, savegameslot);
    description = savedescription;

    /* CGDoom: dedicated buffer (was screens[0]); the archive stops at its
       end (save_overflow) instead of writing past it. The header always
       fits. */
    save_p = savebuffer = Z_Malloc (SAVEGAMESIZE, PU_STATIC, 0);
    save_end = savebuffer + SAVEGAMESIZE;
    save_overflow = false;

    memcpy (save_p, description, SAVESTRINGSIZE);
    save_p += SAVESTRINGSIZE;
    memset (name2,0,sizeof(name2));
    sprintf (name2,"version %i",VERSION);
    memcpy (save_p, name2, VERSIONSIZE);
    save_p += VERSIONSIZE;

    *save_p++ = gameskill;
    *save_p++ = gameepisode;
    *save_p++ = gamemap;
    for (i=0 ; i<MAXPLAYERS ; i++)
        *save_p++ = playeringame[i];
    *save_p++ = leveltime>>16;
    *save_p++ = leveltime>>8;
    *save_p++ = leveltime;

    P_ArchivePlayers ();
    P_ArchiveWorld ();
    P_ArchiveThinkers ();
    P_ArchiveSpecials ();

    if (P_SaveRoom (1, false, 0))
      *save_p++ = 0x1d;   // consistancy marker

    length = save_p - savebuffer;

    int n = CGD_DELAYEDSAVES_SAVES + savegameslot;
    CGDoom_DelayedFileWrite *dfw = &CGD_DelayedSaves[n];
    boolean save_failed = false;

    /* CGDoom: a level too large for the buffer (vanilla wrote past it,
       then failed with "Savegame buffer overrun"): nothing is written, and
       the slot keeps its previous save, its file and any delayed copy */
    if (!save_overflow) {
      if (dfw->data)
        Z_Free(dfw->data);
      dfw->data = NULL;
    }

    if (save_overflow) {
      PL_Log("G_DoSaveGame: %s not written (savegame larger than %d bytes)\n",
        name, SAVEGAMESIZE);
    }
    else if (CGD_Options.DelaySaves) {
      /* Keep an exact-size copy until CGD_FlushDelayedWrites() */
      void *data = Z_Malloc(length, PU_STATIC, 0);
      memcpy(data, savebuffer, length);
      dfw->data = data;
      dfw->size = length;
      strcpy(dfw->filename, name);
    }
    else {
      /* CGDoom (final review): the platform may refuse the write (gint:
         no memory to delay it while the WAD block map is live); say so
         instead of "game saved." */
      save_failed = PL_FileWrite(name, savebuffer, length) < 0;
      I_ReinitAfterError();
    }

    Z_Free (savebuffer);
    savebuffer = NULL;

    gameaction = ga_nothing;
    savedescription[0] = 0;

    static char msg[64];
    if (save_overflow)
      sprintf(msg, "%s", GGSAVETOOBIG);
    else if (save_failed) {
      PL_Log("G_DoSaveGame: %s not written\n", name);
      sprintf(msg, "%s", GGSAVEFAILED);
    }
    else
      sprintf(msg, "%s (%d kB)", CGD_Options.DelaySaves ? GGSAVED : "game saved.",
        length >> 10);
    players[consoleplayer].message = msg;

    /* Redraw status bar next frame; large save files override it */
    extern boolean st_firsttime;
    st_firsttime = true;
}

//
// G_InitNew
// Can be called by the startup code or the menu task,
// consoleplayer, displayplayer, playeringame[] should be set. 
//
skill_t d_skill; 
int     d_episode; 
int     d_map; 

void G_DeferedInitNew( skill_t skill, int  episode, int  map) 
{ 
  d_skill = skill; 
  d_episode = episode; 
  d_map = map; 
  gameaction = ga_newgame; 
} 


void G_DoNewGame (void) 
{
  demoplayback = false;
  netdemo = false;
  netgame = false;
  deathmatch = false;
  playeringame[0] = true;
  playeringame[1] = playeringame[2] = playeringame[3] = 0;
  respawnparm = false;
  fastparm = false;
  nomonsters = false;
  consoleplayer = 0;
  G_InitNew (d_skill, d_episode, d_map);
  gameaction = ga_nothing; 

  if (demorecording)
    G_BeginRecording ();
} 

// The sky texture to be used instead of the F_SKY1 dummy.
extern  int skytexture; 


void G_InitNew ( skill_t skill, int episode, int map ) 
{ 
  //int             i; 

  if (paused) 
  { 
    paused = false; 
  }


  if (skill > sk_nightmare) 
    skill = sk_nightmare;


  // This was quite messy with SPECIAL and commented parts.
  // Supposedly hacks to make the latest edition work.
  // It might not work properly.
  if (episode < 1)
    episode = 1; 

  if ( gamemode == retail )
  {
    if (episode > 4)
      episode = 4;
  }
  else if ( gamemode == shareware )
  {
    if (episode > 1) 
      episode = 1; // only start episode 1 on shareware
  }  
  else
  {
    if (episode > 3)
      episode = 3;
  }



  if (map < 1) 
    map = 1;

  if ( (map > 9) && ( gamemode != commercial) )
    map = 9; 

  /* CGDoom: like vanilla, the engine knows the 32 Doom II maps (HUD titles,
     par times, intermission names, and a level exit never leads past MAP32).
     A MAP33 in the WAD (the BFG/Unity doom2.wad has one) is not played: the
     map is limited as above, for a launcher or -warp start, a save or a demo */
  if ( (map > 32) && ( gamemode == commercial) )
    map = 32;

  M_ClearRandom (); 

  if (skill == sk_nightmare || respawnparm )
    respawnmonsters = true;
  else
    respawnmonsters = false;

  // CGDoom: states[] and mobjinfo[] are const; the nightmare/-fast changes
  // are kept in RAM by p_mobj.c (same conditions as vanilla)
  if (fastparm || (skill == sk_nightmare && gameskill != sk_nightmare) )
    P_SetFastMonsters (true);
  else if (skill != sk_nightmare && gameskill == sk_nightmare)
    P_SetFastMonsters (false);

  // force players to be initialized upon first level load         
  /*for (i=0 ; i<MAXPLAYERS ; i++) 
  players[i].playerstate = PST_REBORN; */
  players[0].playerstate = PST_REBORN;

  usergame = true;                // will be set false if a demo 
  paused = false; 
  demoplayback = false;
  automapactive = false; 
  viewactive = true; 
  gameepisode = episode; 
  gamemap = map; 
  gameskill = skill; 

  viewactive = true;

  // set the sky map for the episode
  if ( gamemode == commercial)
  {
    skytexture = R_TextureNumForName ("SKY3");
    if (gamemap < 12)
      skytexture = R_TextureNumForName ("SKY1");
    else
      if (gamemap < 21)
        skytexture = R_TextureNumForName ("SKY2");
  }
  else
    switch (CGD_SingleEpisodeUltimate ? CGD_SingleEpisodeUltimate : episode)
  { 
    case 1: 
      skytexture = R_TextureNumForName ("SKY1"); 
      break; 
    case 2: 
      skytexture = R_TextureNumForName ("SKY2"); 
      break; 
    case 3: 
      skytexture = R_TextureNumForName ("SKY3"); 
      break; 
    case 4: // Special Edition sky
      skytexture = R_TextureNumForName ("SKY4");
      break;
  } 

  G_DoLoadLevel (); 
} 


//
// DEMO RECORDING 
// 
#define DEMOMARKER  0x80


void G_ReadDemoTiccmd (ticcmd_t* cmd) 
{ 
  /* CGDoom: the end of the demo data counts as its marker (vanilla read on
     past the end of a demo without one, or with a partial ticcmd) */
  if (demoplayend - demo_p < 4 || *demo_p == DEMOMARKER) 
  {
    // end of demo data stream; the rest of this tic runs with the
    // locally built command in vanilla (none during playback), not with
    // the last demo command
    memset (cmd, 0, sizeof (*cmd));
    G_CheckDemoStatus (); 
    return; 
  } 
  cmd->forwardmove = ((signed char)*demo_p++); 
  cmd->sidemove = ((signed char)*demo_p++); 
  cmd->angleturn = ((unsigned char)*demo_p++)<<8; 
  cmd->buttons = (unsigned char)*demo_p++; 
} 


void G_WriteDemoTiccmd (ticcmd_t* cmd) 
{ 
  byte angle = (byte)((cmd->angleturn+128)>>8);

  /* CGDoom: unreachable in practice, M_Responder eats every KEY_ESCAPE
     press (it opens/closes the menu), so gamekeydown[KEY_ESCAPE] is never
     set; recordings end at a played demo (G_DoPlayDemo), a full buffer or
     the quit (wiring review) */
  if (gamekeydown[KEY_ESCAPE])           // press MENU to end demo recording
  {
    G_CheckDemoStatus (); 
    /* CGDoom: vanilla still stores this tic after the marker. The buffer
       now belongs to the delayed save, which may already be written and
       freed (DelaySaves off): store nothing, keep the same quantized turn */
    if (!demorecording)
    {
      cmd->angleturn = angle << 8;
      return;
    }
  }
  *demo_p++ = (byte)cmd->forwardmove; 
  *demo_p++ = (byte)cmd->sidemove; 
  *demo_p++ = angle; 
  *demo_p++ = (byte)cmd->buttons; 
//  demo_p -= 4;
  if (demo_p > demoend - 12)
  {
    // no more space 
    G_CheckDemoStatus (); 
    return; 
  } 

//  G_ReadDemoTiccmd (cmd);         // make SURE it is exactly the same
  cmd->angleturn = demo_p[-2] << 8;
} 



//
// G_RecordDemo 
// 
void G_RecordDemo (const char* name)
{
    int maxsize;

    usergame = false;
    /* CGDoom: name is a complete file name (CGD_DemoFileName); vanilla's
       ".lmp" is only appended to names without an extension */
    strncpy (demoname, name, sizeof(demoname) - 5);
    demoname[sizeof(demoname) - 5] = 0;
    if (!strchr (demoname, '.'))
        strcat (demoname, ".lmp");
    maxsize = DEMO_MAXSIZE;
    demobuffer = demorecbuffer = Z_Malloc (maxsize,PU_STATIC,NULL);
    demoend = demobuffer + maxsize;
    demo_p = NULL;
    demorecording = true;
    demorecbegun = false;
}


void G_BeginRecording (void)
{
    int             i;

    if (!demorecbuffer)
      I_Error ("G_BeginRecording: no G_RecordDemo");
    demobuffer = demo_p = demorecbuffer;
    demoend = demorecbuffer + DEMO_MAXSIZE;
    demorecbegun = true;

    *demo_p++ = VERSION;
    *demo_p++ = gameskill;
    *demo_p++ = gameepisode;
    *demo_p++ = gamemap;
    *demo_p++ = deathmatch;
    *demo_p++ = respawnparm;
    *demo_p++ = fastparm;
    *demo_p++ = nomonsters;
    *demo_p++ = consoleplayer;

    for (i=0 ; i<MAXPLAYERS ; i++)
        *demo_p++ = playeringame[i];
}


//
// G_PlayDemo 
//

char* defdemoname; 
char* defdemofilename;

void G_DeferedPlayDemo (char* name) 
{ 
  defdemoname = name; 
  defdemofilename = NULL;
  gameaction = ga_playdemo; 
} 

void G_DeferedPlayDemoFile (char* name)
{
  defdemoname = NULL;
  defdemofilename = name;
  gameaction = ga_playdemo;
}

/* CGDoom: version, skill, episode, map, deathmatch, respawn, fast,
   nomonsters, consoleplayer, players (versions 108-110) */
#define DEMOHEADERSIZE (9 + MAXPLAYERS)

/* CGDoom: G_DoPlayDemo rejects a demo it cannot play like one of another
   version (before anything changes): fatal in demo-only mode (nothing else
   to show), otherwise the title loop goes on. The buffer is released as
   G_CheckDemoStatus releases it at the end of a demo: a lump becomes
   purgeable again (a lump mapped in place is no zone block: Z_ChangeTag
   ignores it), the buffer of a file (demo-only mode) is freed. */
static void G_RejectDemo (const char *why, int value)
{
  if (CGD_PlayDemoOnly)
    I_Error ("Demo is %s (%d)", why, value);
  if (defdemoname)
    Z_ChangeTag (demobuffer, PU_CACHE);
  else
    Z_Free (demobuffer);
  demobuffer = demo_p = demoplayend = NULL;
  gameaction = ga_nothing;
}

void G_DoPlayDemo (void)
{
  skill_t skill;
  int i, episode, map, length;
  int dm, respawn, fast, nomon, console;
  byte ingame[MAXPLAYERS];

  gameaction = ga_nothing;
  /* CGDoom: a demo played while a recording runs (End Game with title
     demos on; MENU never reaches G_WriteDemoTiccmd, M_Responder eats it)
     ends the recording here, before demobuffer/demo_p are replaced. It used
     to go on writing ticcmds through demo_p into the played lump (4 bytes
     ahead of the reader: the demo desynced, ran over its marker and the
     writes went on past the lump), and to save from the lump at the end. */
  if (demorecording && demorecbegun && !demoplayback && !timingdemo)
    G_CheckDemoStatus ();
  /* CGDoom: with its length (vanilla read the header and the ticcmds
     whatever the length) */
  demobuffer = demo_p = NULL;
  length = 0;
  if (defdemoname)
  {
    i = W_GetNumForName (defdemoname);
    demobuffer = demo_p = (byte *)W_CacheLumpNumConst(i, PU_STATIC);
    length = W_LumpLength (i);
  }
  else if (defdemofilename)
  {
    demobuffer = NULL;
    length = M_ReadFile(defdemofilename, &demobuffer, 1);
    demo_p = demobuffer;
  }

  if (length < DEMOHEADERSIZE)
  {
    G_RejectDemo ("damaged: too short", length);
    return;
  }
  demoplayend = demobuffer + length;

  /* CGDoom: We have old WADs with demos at versions 108 and 109. The decoding
     work similarly, it's probably enemy position/AI that mess up */
  byte version = *demo_p++;
  if (version != VERSION && version != 109 && version != 108)
  {
    // fprintf( stderr, "Demo is from a different game version!\n");
    /* CGDoom: in demo-only mode there is nothing else to show */
    G_RejectDemo ("from a different game version", version);
    return;
  }

  skill = *demo_p++;
  episode = *demo_p++;
  map = *demo_p++;
  dm = *demo_p++;
  respawn = *demo_p++;
  fast = *demo_p++;
  nomon = *demo_p++;
  console = *demo_p++;
  for (i=0 ; i<MAXPLAYERS ; i++)
    ingame[i] = *demo_p++;

  /* CGDoom: players: flags 0 or 1, consoleplayer (vanilla indexed
     players[] with any byte) in the game, and player 0 (this engine runs,
     renders and reads ticcmds for player 0 only; without a player in the
     game, monsters looked for one forever) */
  for (i=0 ; i<MAXPLAYERS ; i++)
    if (ingame[i] > 1)
      break;
  if (i < MAXPLAYERS || console >= MAXPLAYERS || !ingame[console]
    || !ingame[0])
  {
    G_RejectDemo ("damaged: bad players, console player", console);
    return;
  }

  deathmatch = dm;
  respawnparm = respawn;
  fastparm = fast;
  nomonsters = nomon;
  consoleplayer = console;

  for (i=0 ; i<MAXPLAYERS ; i++)
    playeringame[i] = ingame[i];
  if (playeringame[1])
  {
    netgame = true;
    netdemo = true;
  }

  /* CGDoom: P_SpawnPlayer sets the bodies of the players in the game; the
     others keep none, not one of the previous level */
  for (i=0 ; i<MAXPLAYERS ; i++)
    players[i].mo = NULL;

  // don't spend a lot of time in loadlevel
  precache = false;
  G_InitNew (skill, episode, map);
  precache = true;

  /* CGDoom: a player whose start the map lacks would have no body */
  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i] && !players[i].mo)
      I_Error ("Demo is damaged: no start for player %d", i+1);

  usergame = false;
  demoplayback = true;
}

//
// G_TimeDemo 
//
void G_TimeDemo (char* name) 
{   
  nodrawers = 0; 
  noblit = 0; 
  timingdemo = true; 
  singletics = true; 

  defdemoname = name; 
  gameaction = ga_playdemo; 
} 


/* 
=================== 
= 
= G_CheckDemoStatus 
= 
= Called after a death or level completion to allow demos to be cleaned up 
= Returns true if a new demo loop action will take place 
=================== 
*/ 

boolean G_CheckDemoStatus (void) 
{ 
  int             endtime; 

  if (timingdemo) 
  { 
    endtime = I_GetTime (); 
    I_Error ("timed %i gametics in %i realtics",gametic 
      , endtime-starttime); 
  } 

  if (demoplayback)
  {
    if(CGD_PlayDemoOnly)
    {
      fuck = true;
      return true;
    }

    Z_ChangeTag (demobuffer, PU_CACHE);
    demoplayback = false;
    netdemo = false;
    netgame = false;
    deathmatch = false;
    playeringame[1] = playeringame[2] = playeringame[3] = 0;
    respawnparm = false;
    fastparm = false;
    nomonsters = false;
    consoleplayer = 0;
    D_AdvanceDemo ();
    return true;
  }

  if (demorecording && !demorecbegun)
  {
    /* CGDoom: armed, never begun (the game ends or fails on the title or
       during the first level load): nothing to save, and demo_p is not a
       position in the recording buffer. The buffer stays allocated. */
    demorecording = false;
    return false;
  }

  if (demorecording)
  {
    *demo_p++ = DEMOMARKER;
    CGDoom_DelayedFileWrite *dfw = &CGD_DelayedSaves[CGD_DELAYEDSAVES_DEMO];
    if (dfw->data)
      Z_Free(dfw->data);
    strcpy(dfw->filename, demoname);
    dfw->data = demobuffer;
    dfw->size = demo_p - demobuffer;
    demorecording = false;
    demorecbegun = false;
    demorecbuffer = NULL;           /* owned by the delayed save now */
    printf ("Demo %s recorded", demoname);
    if (!CGD_Options.DelaySaves
        && CGD_WriteDelayedFile(CGD_DELAYEDSAVES_DEMO) < 0)
      PL_Log("G_CheckDemoStatus: %s not written\n", demoname);
  }

  return false;
} 





/* CGDoom: paused/idle signal for the platform's idle timeout (cgdoom.h) */
int CGD_IsPaused(void)
{
    int idle = 0;

    if (paused)
        idle |= CGD_IDLE_PAUSED;
    if (menuactive)
        idle |= CGD_IDLE_MENU;
    /* gamemap is 0 until the first level is set up (gamestate starts as
       GS_LEVEL, i.e. 0, before the first tic) */
    if (gamestate != GS_LEVEL || gamemap == 0)
        idle |= CGD_IDLE_NOLEVEL;
    if (demoplayback)
        idle |= CGD_IDLE_DEMO;
    return idle;
}
