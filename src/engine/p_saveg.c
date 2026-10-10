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
//	Archiving: SaveGame I/O.
//
//-----------------------------------------------------------------------------

//static const char 

#include "i_system.h"
#include "z_zone.h"
#include "p_local.h"

// State.
#include "doomstat.h"
#include "r_state.h"

byte*		save_p;

// CGDoom: the end of the savegame buffer. Vanilla archived into a buffer
// of fixed size and only compared the length with it once everything was
// written (a big level wrote past it), and unarchived whatever followed a
// truncated savegame, trusting every index in it. Archiving now stops
// before save_end (save_overflow is set and nothing more is written; the
// caller drops the savegame), and unarchiving fails (I_Error) before it
// reads past save_end or uses an index outside its array.
byte*		save_end;
boolean		save_overflow;


// Pads save_p to a 4-byte boundary
//  so that the load/save works on SGI&Gecko.
// CGDoom: to the pointer size when it is larger (64-bit host builds: the
// archived structs hold pointers, and UBSan rejects the misaligned stores
// of a 4-byte boundary). Identical on the calculator (4-byte pointers); the
// host save format already differed (struct sizes).
#define SAVE_ALIGN	(sizeof(void *) > 4 ? sizeof(void *) : 4)
#define SAVE_PAD(p)	((SAVE_ALIGN - ((uintptr_t) (p) & (SAVE_ALIGN - 1))) & (SAVE_ALIGN - 1))
#define PADSAVEP()	save_p += SAVE_PAD (save_p)


//
// CGDoom: savegame bounds and checks
//

// Bytes from save_p to the end of a record: head bytes (its class byte),
// the padding of PADSAVEP if pad is set, then size bytes
static size_t P_RecordSize (size_t head, boolean pad, size_t size)
{
    if (pad)
        head += SAVE_PAD ((uintptr_t) save_p + head);
    return head + size;
}

// Archiving: true if the record fits before save_end. Otherwise
// save_overflow is set, and stays set: nothing more is written.
boolean P_SaveRoom (size_t head, boolean pad, size_t size)
{
    if (!save_overflow
        && P_RecordSize (head, pad, size) > (size_t) (save_end - save_p))
        save_overflow = true;
    return !save_overflow;
}

// Unarchiving: the record must be in the savegame
void P_LoadCheck (size_t head, boolean pad, size_t size)
{
    if (P_RecordSize (head, pad, size) > (size_t) (save_end - save_p))
        I_Error ("Bad savegame: truncated");
}

static void P_BadSave (const char* what, intptr_t value)
{
    I_Error ("Bad savegame: %s %ld", what, (long) value);
}

// A state index of the savegame (archived states are indices)
static state_t* P_LoadState (intptr_t index)
{
    if (index < 0 || index >= NUMSTATES)
        P_BadSave ("state", index);
    return (state_t *) &states[index];
}

// The renderer finds sprite frames through sprite and frame without any
// check (only with RANGECHECK): the frame must exist in the WAD
static void P_CheckSprite (spritenum_t sprite, int frame)
{
    if ((unsigned) sprite >= (unsigned) numsprites)
        P_BadSave ("sprite", sprite);
    if ((frame & FF_FRAMEMASK) >= sprites[sprite].numframes)
        P_BadSave ("sprite frame", frame);
}

// A sector index of the savegame (specials)
static sector_t* P_LoadSector (intptr_t index)
{
    if (index < 0 || index >= numsectors)
        P_BadSave ("sector", index);
    return &sectors[index];
}

static void P_CheckFlat (int pic)
{
    if (pic < 0 || pic >= numflats)
        P_BadSave ("flat", pic);
}

static void P_CheckTexture (int texture)
{
    extern int numtextures;   // r_data.c

    if (texture < 0 || texture >= numtextures)
        P_BadSave ("texture", texture);
}

// Size of the world block (P_ArchiveWorld, P_UnArchiveWorld)
static size_t P_WorldSize (void)
{
    size_t	shorts;
    int		i;
    int		j;
    line_t*	li;

    shorts = (size_t) numsectors * 7;
    for (i=0, li = lines ; i<numlines ; i++,li++)
    {
        shorts += 3;
        for (j=0 ; j<2 ; j++)
            if (li->sidenum[j] != -1)
                shorts += 5;
    }
    return shorts * sizeof(short);
}



//
// P_ArchivePlayers
//
void P_ArchivePlayers (void)
{
  int		i;
  int		j;
  player_t*	dest;

  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (!playeringame[i])
      continue;

    if (!P_SaveRoom (0, true, sizeof(player_t)))
      return;
    PADSAVEP();

    dest = (player_t *)save_p;
    memcpy (dest,&players[i],sizeof(player_t));
    save_p += sizeof(player_t);
    for (j=0 ; j<NUMPSPRITES ; j++)
    {
      if (dest->psprites[j].state)
      {
        dest->psprites[j].state 
          = (state_t *)(dest->psprites[j].state-states);
      }
    }
  }
}


//
// P_UnArchivePlayers
//
void P_UnArchivePlayers (void)
{
    int   i;
    int   j;

    for (i=0 ; i<MAXPLAYERS ; i++)
    {
        if (!playeringame[i])
            continue;

        P_LoadCheck (0, true, sizeof(player_t));
        PADSAVEP();

        memcpy (&players[i],save_p, sizeof(player_t));
        save_p += sizeof(player_t);

        // will be set when unarc thinker
        players[i].mo = NULL;
        players[i].message = NULL;
        players[i].attacker = NULL;

        for (j=0 ; j<NUMPSPRITES ; j++)
        {
            if (players[i]. psprites[j].state)
            {
                players[i]. psprites[j].state
                    = P_LoadState ((intptr_t)players[i].psprites[j].state);
                P_CheckSprite (players[i].psprites[j].state->sprite,
                               players[i].psprites[j].state->frame);
            }
        }

        // CGDoom: the fields that index arrays: weaponinfo[]; the status
        // bar's faces[] (health, which the game keeps >= 0) and palettes
        // (damagecount, bonuscount, never negative); colormaps (the 32
        // light maps and INVERSECOLORMAP, p_user.c)
        if ((unsigned) players[i].readyweapon >= NUMWEAPONS)
            P_BadSave ("weapon", players[i].readyweapon);
        if ((unsigned) players[i].pendingweapon >= NUMWEAPONS
            && players[i].pendingweapon != wp_nochange)
            P_BadSave ("weapon", players[i].pendingweapon);
        if (players[i].health < 0)
            P_BadSave ("player health", players[i].health);
        if (players[i].damagecount < 0)
            P_BadSave ("damage count", players[i].damagecount);
        if (players[i].bonuscount < 0)
            P_BadSave ("bonus count", players[i].bonuscount);
        if ((unsigned) players[i].fixedcolormap > NUMCOLORMAPS)
            P_BadSave ("colormap", players[i].fixedcolormap);
    }
}

//
// P_ArchiveWorld
//
void P_ArchiveWorld (void)
{
    int     i;
    int     j;
    sector_t*   sec;
    line_t*   li;
    side_t*   si;
    short*    put;

    if (!P_SaveRoom (0, false, P_WorldSize ()))
        return;
    put = (short *)save_p;

    // do sectors
    for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
        *put++ = sec->floorheight >> FRACBITS;
        *put++ = sec->ceilingheight >> FRACBITS;
        *put++ = sec->floorpic;
        *put++ = sec->ceilingpic;
        *put++ = sec->lightlevel;
        *put++ = sec->special;    // needed?
        *put++ = sec->tag;    // needed?
    }


    // do lines
    for (i=0, li = lines ; i<numlines ; i++,li++)
    {
        *put++ = li->flags;
        *put++ = li->special;
        *put++ = li->tag;
        for (j=0 ; j<2 ; j++)
        {
            if (li->sidenum[j] == -1)
                continue;

            si = &sides[li->sidenum[j]];

            *put++ = si->textureoffset >> FRACBITS;
            *put++ = si->rowoffset >> FRACBITS;
            *put++ = si->toptexture;
            *put++ = si->bottomtexture;
            *put++ = si->midtexture;
        }
    }

    save_p = (byte *)put;
}



//
// P_UnArchiveWorld
//
void P_UnArchiveWorld (void)
{
    int     i;
    int     j;
    sector_t*   sec;
    line_t*   li;
    side_t*   si;
    short*    get;

    // CGDoom: the block is sized by the current level (a savegame of
    // another level layout fails here or at a check below)
    P_LoadCheck (0, false, P_WorldSize ());
    get = (short *)save_p;

    // do sectors
    for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
        sec->floorheight = *get++ << FRACBITS;
        sec->ceilingheight = *get++ << FRACBITS;
        sec->floorpic = *get++;
        sec->ceilingpic = *get++;
        P_CheckFlat (sec->floorpic);
        P_CheckFlat (sec->ceilingpic);
        sec->lightlevel = *get++;
        sec->special = *get++;    // needed?
        sec->tag = *get++;    // needed?
        sec->specialdata = 0;
        sec->soundtarget = 0;
    }

    // do lines
    for (i=0, li = lines ; i<numlines ; i++,li++)
    {
        li->flags = *get++;
        li->special = *get++;
        li->tag = *get++;
        for (j=0 ; j<2 ; j++)
        {
            if (li->sidenum[j] == -1)
              continue;
            si = &sides[li->sidenum[j]];
            si->textureoffset = *get++ << FRACBITS;
            si->rowoffset = *get++ << FRACBITS;
            si->toptexture = *get++;
            si->bottomtexture = *get++;
            si->midtexture = *get++;
            P_CheckTexture (si->toptexture);
            P_CheckTexture (si->bottomtexture);
            P_CheckTexture (si->midtexture);
        }
    }
    save_p = (byte *)get;
}



//
// Thinkers
//
typedef enum
{
    tc_end,
    tc_mobj

} thinkerclass_t;



//
// P_ArchiveThinkers
//
void P_ArchiveThinkers (void)
{
    thinker_t*    th;
    mobj_t*   mobj;

    // save off the current thinkers
    for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
        if (th->function == TP_MobjThinker)
        {
            if (!P_SaveRoom (1, true, sizeof(*mobj)))
                return;
            *save_p++ = tc_mobj;
            PADSAVEP();
            mobj = (mobj_t *)save_p;
            memcpy (mobj, th, sizeof(*mobj));
            save_p += sizeof(*mobj);
            mobj->state = (state_t *)(mobj->state - states);

            if (mobj->player)
                mobj->player = (player_t *)((mobj->player-players) + 1);
            continue;
        }

        // I_Error ("P_ArchiveThinkers: Unknown thinker function");
    }

    // add a terminating marker
    if (P_SaveRoom (1, false, 0))
        *save_p++ = tc_end;
}



//
// P_UnArchiveThinkers
//
void P_UnArchiveThinkers (void)
{
    byte    tclass;
    thinker_t*    currentthinker;
    thinker_t*    next;
    mobj_t*   mobj;
    intptr_t	pnum;
    int		i;

    // remove all the current thinkers
    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
        next = currentthinker->next;

        if (currentthinker->function == TP_MobjThinker)
            P_RemoveMobj ((mobj_t *)currentthinker);
        else
            Z_Free (currentthinker);

        currentthinker = next;
    }

    P_InitThinkers ();

    // read in saved thinkers
    while (1)
    {
        P_LoadCheck (1, false, 0);
        tclass = *save_p++;
        switch (tclass)
        {
          case tc_end:
            // CGDoom: every player in the game needs a body (the
            // engine runs, renders and steers them through mo)
            for (i=0 ; i<MAXPLAYERS ; i++)
                if (playeringame[i] && !players[i].mo)
                    P_BadSave ("no body for player", i+1);
            return;   // end of list

          case tc_mobj:
            P_LoadCheck (0, true, sizeof(*mobj));
            PADSAVEP();
            mobj = Z_Malloc (sizeof(*mobj), PU_LEVEL, NULL);
            memcpy (mobj, save_p, sizeof(*mobj));
            save_p += sizeof(*mobj);
            mobj->state = P_LoadState ((intptr_t)mobj->state);
            mobj->target = NULL;
            // CGDoom: like target (vanilla kept tracer, a pointer of the
            // session that saved; every user of tracer accepts NULL)
            mobj->tracer = NULL;
            if (mobj->player)
            {
                pnum = (intptr_t)mobj->player;
                if (pnum < 1 || pnum > MAXPLAYERS || !playeringame[pnum-1])
                    P_BadSave ("player", pnum);
                mobj->player = &players[pnum-1];
                mobj->player->mo = mobj;
            }
            // CGDoom: the other fields that index arrays: mobjinfo[], the
            // sprite frame (unless the renderer never sees the thing),
            // playeringame[] (P_LookForPlayers), the direction tables
            // (0-7 or DI_NODIR = 8, p_enemy.c)
            if ((unsigned) mobj->type >= NUMMOBJTYPES)
                P_BadSave ("thing type", mobj->type);
            if (!(mobj->flags & MF_NOSECTOR))
                P_CheckSprite (mobj->sprite, mobj->frame);
            if ((unsigned) mobj->lastlook >= MAXPLAYERS)
                P_BadSave ("lastlook", mobj->lastlook);
            if ((unsigned) mobj->movedir > 8)
                P_BadSave ("movedir", mobj->movedir);
            // CGDoom: the links are set by P_SetThingPosition, except those
            // of MF_NOSECTOR / MF_NOBLOCKMAP things: never pointers of the
            // session that saved, should those flags change
            mobj->snext = mobj->sprev = NULL;
            mobj->bnext = mobj->bprev = NULL;
            P_SetThingPosition (mobj);
            mobj->info = (mobjinfo_t *)&mobjinfo[mobj->type];
            mobj->floorz = mobj->subsector->sector->floorheight;
            mobj->ceilingz = mobj->subsector->sector->ceilingheight;
            mobj->thinker.function = TP_MobjThinker;
            P_AddThinker (&mobj->thinker, mobj);
            break;

          default:
            I_Error ("Unknown tclass %i in savegame",tclass);
        }
    }
}


//
// P_ArchiveSpecials
//
enum
{
  tc_ceiling,
  tc_door,
  tc_floor,
  tc_plat,
  tc_flash,
  tc_strobe,
  tc_glow,
  tc_endspecials

} specials_e;	



//
// Things to handle:
//
// T_MoveCeiling, (ceiling_t: sector_t * swizzle), - active list
// T_VerticalDoor, (vldoor_t: sector_t * swizzle),
// T_MoveFloor, (floormove_t: sector_t * swizzle),
// T_LightFlash, (lightflash_t: sector_t * swizzle),
// T_StrobeFlash, (strobe_t: sector_t *),
// T_Glow, (glow_t: sector_t *),
// T_PlatRaise, (plat_t: sector_t *), - active list
//
void P_ArchiveSpecials (void)
{
  thinker_t*		th;
  ceiling_t*		ceiling;
  vldoor_t*		door;
  floormove_t*	floor;
  plat_t*		plat;
  lightflash_t*	flash;
  strobe_t*		strobe;
  glow_t*		glow;
  int			i;

  // save off the current thinkers
  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
  {
    if (th->function == 0)
    {
      for (i = 0; i < MAXCEILINGS;i++)
        if (activeceilings[i] == (ceiling_t *)th)
          break;

      if (i<MAXCEILINGS)
      {
        if (!P_SaveRoom (1, true, sizeof(*ceiling)))
          return;
        *save_p++ = tc_ceiling;
        PADSAVEP();
        ceiling = (ceiling_t *)save_p;
        memcpy (ceiling, th, sizeof(*ceiling));
        save_p += sizeof(*ceiling);
        ceiling->sector = (sector_t *)(ceiling->sector - sectors);
      }
      continue;
    }

    if (th->function == TT_MoveCeiling)
    {
      if (!P_SaveRoom (1, true, sizeof(*ceiling)))
        return;
      *save_p++ = tc_ceiling;
      PADSAVEP();
      ceiling = (ceiling_t *)save_p;
      memcpy (ceiling, th, sizeof(*ceiling));
      save_p += sizeof(*ceiling);
      ceiling->sector = (sector_t *)(ceiling->sector - sectors);
      continue;
    }

    if (th->function == TT_VerticalDoor)
    {
      if (!P_SaveRoom (1, true, sizeof(*door)))
        return;
      *save_p++ = tc_door;
      PADSAVEP();
      door = (vldoor_t *)save_p;
      memcpy (door, th, sizeof(*door));
      save_p += sizeof(*door);
      door->sector = (sector_t *)(door->sector - sectors);
      continue;
    }

    if (th->function == TT_MoveFloor)
    {
      if (!P_SaveRoom (1, true, sizeof(*floor)))
        return;
      *save_p++ = tc_floor;
      PADSAVEP();
      floor = (floormove_t *)save_p;
      memcpy (floor, th, sizeof(*floor));
      save_p += sizeof(*floor);
      floor->sector = (sector_t *)(floor->sector - sectors);
      continue;
    }

    if (th->function == TT_PlatRaise)
    {
      if (!P_SaveRoom (1, true, sizeof(*plat)))
        return;
      *save_p++ = tc_plat;
      PADSAVEP();
      plat = (plat_t *)save_p;
      memcpy (plat, th, sizeof(*plat));
      save_p += sizeof(*plat);
      plat->sector = (sector_t *)(plat->sector - sectors);
      continue;
    }

    if (th->function == TT_LightFlash)
    {
      if (!P_SaveRoom (1, true, sizeof(*flash)))
        return;
      *save_p++ = tc_flash;
      PADSAVEP();
      flash = (lightflash_t *)save_p;
      memcpy (flash, th, sizeof(*flash));
      save_p += sizeof(*flash);
      flash->sector = (sector_t *)(flash->sector - sectors);
      continue;
    }

    if (th->function == TT_StrobeFlash)
    {
      if (!P_SaveRoom (1, true, sizeof(*strobe)))
        return;
      *save_p++ = tc_strobe;
      PADSAVEP();
      strobe = (strobe_t *)save_p;
      memcpy (strobe, th, sizeof(*strobe));
      save_p += sizeof(*strobe);
      strobe->sector = (sector_t *)(strobe->sector - sectors);
      continue;
    }

    if (th->function == TT_Glow)
    {
      if (!P_SaveRoom (1, true, sizeof(*glow)))
        return;
      *save_p++ = tc_glow;
      PADSAVEP();
      glow = (glow_t *)save_p;
      memcpy (glow, th, sizeof(*glow));
      save_p += sizeof(*glow);
      glow->sector = (sector_t *)(glow->sector - sectors);
      continue;
    }
  }

  // add a terminating marker
  if (P_SaveRoom (1, false, 0))
    *save_p++ = tc_endspecials;

}


//
// P_UnArchiveSpecials
//
void P_UnArchiveSpecials (void)
{
  byte		tclass;
  ceiling_t*		ceiling;
  vldoor_t*		door;
  floormove_t*	floor;
  plat_t*		plat;
  lightflash_t*	flash;
  strobe_t*		strobe;
  glow_t*		glow;


  // read in saved thinkers
  for(;;)
  {
    P_LoadCheck (1, false, 0);
    tclass = *save_p++;
    switch (tclass)
    {
    case tc_endspecials:
      return;	// end of list

    case tc_ceiling:
      P_LoadCheck (0, true, sizeof(*ceiling));
      PADSAVEP();
      ceiling = (ceiling_t*)Z_Malloc (sizeof(*ceiling), PU_LEVEL, NULL);
      memcpy (ceiling, save_p, sizeof(*ceiling));
      save_p += sizeof(*ceiling);
      ceiling->sector = P_LoadSector ((intptr_t)ceiling->sector);
      ceiling->sector->specialdata = ceiling;

      if (ceiling->thinker.function)
        ceiling->thinker.function = TT_MoveCeiling;

      P_AddThinker (&ceiling->thinker,ceiling);
      P_AddActiveCeiling(ceiling);
      break;

    case tc_door:
      P_LoadCheck (0, true, sizeof(*door));
      PADSAVEP();
      door = (vldoor_t*)Z_Malloc (sizeof(*door), PU_LEVEL, NULL);
      memcpy (door, save_p, sizeof(*door));
      save_p += sizeof(*door);
      door->sector = P_LoadSector ((intptr_t)door->sector);
      door->sector->specialdata = door;
      door->thinker.function = TT_VerticalDoor;
      P_AddThinker (&door->thinker,door);
      break;

    case tc_floor:
      P_LoadCheck (0, true, sizeof(*floor));
      PADSAVEP();
      floor = (floormove_t*)Z_Malloc (sizeof(*floor), PU_LEVEL, NULL);
      memcpy (floor, save_p, sizeof(*floor));
      save_p += sizeof(*floor);
      floor->sector = P_LoadSector ((intptr_t)floor->sector);
      floor->sector->specialdata = floor;
      // CGDoom: the flat T_MoveFloor puts on the sector when the floor
      // stops: a donut raised up or a floor lowered and changed. Only then:
      // EV_BuildStairs leaves type and texture uninitialized (vanilla), so
      // a valid save of moving stairs can hold any values there.
      if ((floor->type == donutRaise && floor->direction == 1)
          || (floor->type == lowerAndChange && floor->direction == -1))
        P_CheckFlat (floor->texture);
      floor->thinker.function = TT_MoveFloor;
      P_AddThinker (&floor->thinker,floor);
      break;

    case tc_plat:
      P_LoadCheck (0, true, sizeof(*plat));
      PADSAVEP();
      plat = (plat_t*)Z_Malloc (sizeof(*plat), PU_LEVEL, NULL);
      memcpy (plat, save_p, sizeof(*plat));
      save_p += sizeof(*plat);
      plat->sector = P_LoadSector ((intptr_t)plat->sector);
      plat->sector->specialdata = plat;

      if (plat->thinker.function)
        plat->thinker.function = TT_PlatRaise;

      P_AddThinker (&plat->thinker,plat);
      P_AddActivePlat(plat);
      break;

    case tc_flash:
      P_LoadCheck (0, true, sizeof(*flash));
      PADSAVEP();
      flash = (lightflash_t*)Z_Malloc (sizeof(*flash), PU_LEVEL, NULL);
      memcpy (flash, save_p, sizeof(*flash));
      save_p += sizeof(*flash);
      flash->sector = P_LoadSector ((intptr_t)flash->sector);
      flash->thinker.function = TT_LightFlash;
      P_AddThinker (&flash->thinker,flash);
      break;

    case tc_strobe:
      P_LoadCheck (0, true, sizeof(*strobe));
      PADSAVEP();
      strobe = (strobe_t*)Z_Malloc (sizeof(*strobe), PU_LEVEL, NULL);
      memcpy (strobe, save_p, sizeof(*strobe));
      save_p += sizeof(*strobe);
      strobe->sector = P_LoadSector ((intptr_t)strobe->sector);
      strobe->thinker.function = TT_StrobeFlash;
      P_AddThinker (&strobe->thinker,strobe);
      break;

    case tc_glow:
      P_LoadCheck (0, true, sizeof(*glow));
      PADSAVEP();
      glow = (glow_t*)Z_Malloc (sizeof(*glow), PU_LEVEL, NULL);
      memcpy (glow, save_p, sizeof(*glow));
      save_p += sizeof(*glow);
      glow->sector = P_LoadSector ((intptr_t)glow->sector);
      glow->thinker.function = TT_Glow;
      P_AddThinker (&glow->thinker,glow);
      break;

    default:
      I_Error ("P_UnarchiveSpecials:Unknown tclass %i in savegame",tclass);
    }

  }
}

