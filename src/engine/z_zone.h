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
// DESCRIPTION:
//      Zone Memory Allocation, perhaps NeXT ObjectiveC inspired.
//	Remark: this was the only stuff that, according
//	 to John Carmack, might have been useful for
//	 Quake.
//
//---------------------------------------------------------------------



#ifndef __Z_ZONE__
#define __Z_ZONE__

#include "os.h"

//
// ZONE MEMORY
// PU - purge tags.
// Tags < 100 are not overwritten until freed.
#define PU_STATIC		1	// static entire execution time
#define PU_SOUND		2	// static while playing
#define PU_MUSIC		3	// static while playing
#define PU_DAVE		4	// anything else Dave wants static
#define PU_LEVEL		50	// static until level exited
#define PU_LEVSPEC		51      // a special thinker in a level
// Tags >= 100 are purgable whenever needed.
#define PU_PURGELEVEL	100
#define PU_CACHE		101


void	Z_Init (void);
void*	Z_Malloc (int size, int tag, void *ptr);
void    Z_Free (const void *ptr);
void    Z_FreeTags (int lowtag, int hightag);
void    Z_DumpHeap (int lowtag, int hightag);
//void    Z_FileDumpHeap (FILE *f);
void    Z_CheckHeap (void);
void    Z_ChangeTag2 (const void *ptr, int tag);

/* CGDoom: Returns free memory in specified zone, or all zones if zone < 0 */
int     Z_FreeMemory (int zone);
/* CGDoom: Returns largest free block in specified zone, or all if zone < 0 */
int     Z_LargestFreeBlock (int zone);

/* CGDoom: Support for multiple zones is added; Z_Init() now does nothing,
   zones need to be added manually (see D_DoomMain()). */
void    Z_AddZone (void *zone, int size);
/* CGDoom: Number of zones, and size of a zone (bytes) */
int     Z_ZoneCount (void);
int     Z_ZoneSize (int zone);

#ifdef RANGECHECK
/* CGDoom debug: fill new blocks with this byte if >= 0 (default -1) */
extern int Z_PoisonValue;
#endif

/* Maximum number of zones. */
#define ZONE_MAX 16


typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;	// NULL if a free block
    int			tag;	// purgelevel
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
} memblock_t;

/* CGDoom: tag of a zone block from its user pointer (no checks; ptr must be
   a live Z_Malloc block) */
static inline int Z_BlockTag(const void *ptr)
{
    return ((const memblock_t *)((const unsigned char *)ptr - sizeof(memblock_t)))->tag;
}

//
// This is used to get the local FILE:LINE info from CPP
// prior to really call the function in question.
//
#define Z_ChangeTagX(p,t) \
{ \
      if (( (memblock_t *)( (byte *)(p) - sizeof(memblock_t)))->id!=0x1d4a11) \
	  I_Error("Z_CT at "__FILE__":%i",__LINE__); \
	  Z_ChangeTag2(p,t); \
};
#define Z_ChangeTag(p,t) Z_ChangeTag2(p,t)


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
