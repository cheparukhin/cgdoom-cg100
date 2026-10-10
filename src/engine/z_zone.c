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
// Zone Memory Allocation. Neat.
//
//-----------------------------------------------------------------------------

#include "os.h"
#include "z_zone.h"
#include "i_system.h"
#include "doomdef.h"

#include "cgdoom.h"


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
// 

#define ZONEID  0x1d4a11


typedef struct
{
  // total bytes malloced, including header
  int size;
  // start / end cap for linked list
  memblock_t  blocklist;
  memblock_t* rover;
} memzone_t;

/* CGDoom: In order to increase available memory, we need to support multiple
   zones. The code below is adapted to do that:

   * Z_ClearZone is already parameterized, nothing to do (it's never called)
   * Z_Init is mostly replaced by a new function Z_AddZone()
   * Z_Free is changed to detect which zone contains the pointer
   * Z_Malloc is changed to try all zones
   * Z_FreeTags is changed to iterate on all zones
   * Z_CheckHeap is changed to iterate on all zones
   * Z_ChangeTag2 operates on a single block and needs no change
   * Z_FreeMemory is changed to iterate on one or all zones

   In addition to that, the next-fit strategy with a rover also runs over zones
   since starting at the first zone when it's full loses *a lot* of time.

   However, the next-fit strategy tends to produce *a lot* of fragmentation,
   resulting in early allocation failures due to the lack of large spaces. For
   this reason, the last region is not part of the zone rover rotation. */

static memzone_t *zones[ZONE_MAX]; /* static memzone_t *mainzone; */

#ifdef RANGECHECK
/* Debug: if >= 0, every new block is filled with this byte, to check that the
   output never depends on uninitialized heap contents */
int Z_PoisonValue = -1;
#endif
static int zone_count;
static int zone_rover = 0;



//
// Z_ClearZone
//
void Z_ClearZone (memzone_t* zone)
{
  memblock_t* block;

  // set the entire zone to one free block
  zone->blocklist.next = zone->blocklist.prev = block = (memblock_t *)((byte *)zone + sizeof(memzone_t));

  zone->blocklist.user = (void **)zone;
  zone->blocklist.tag = PU_STATIC;
  zone->rover = block;

  block->prev = block->next = &zone->blocklist;
  // NULL indicates a free block.
  block->user = NULL;
  block->size = zone->size - sizeof(memzone_t);
}



//
// Z_Init
//
void Z_Init(void)
{
  /* All work is left to Z_AddZone() */
  zone_count = 0;
}

void Z_AddZone (void *buf, int size)
{
  memblock_t* block;

  if (zone_count >= ZONE_MAX)
    return;

  /* CGDoom: align the zone on pointer size (blocks are multiples of it) */
  uintptr_t align = (-(uintptr_t)buf) & (sizeof(void *) - 1);
  buf = (byte *)buf + align;
  size = (size - align) & ~(int)(sizeof(void *) - 1);
  if (size < (int)(sizeof(memzone_t) + sizeof(memblock_t) + 64))
    return;

  memzone_t* zone = (memzone_t *)buf;
  zones[zone_count++] = zone;
  zone->size = size;

  // set the entire zone to one free block
  zone->blocklist.next = zone->blocklist.prev = block = (memblock_t *)( (byte *)zone + sizeof(memzone_t) );

  zone->blocklist.user = (void **)zone;
  zone->blocklist.tag = PU_STATIC;
  zone->rover = block;

  block->prev = block->next = &zone->blocklist;

  // NULL indicates a free block.
  block->user = NULL;

  block->size = zone->size - sizeof(memzone_t);
}


//
// Z_Free
//
void Z_Free (const void* ptr)
{
  memblock_t* block;
  memblock_t* other;

  if (ptr == NULL || PL_IsWadPointer(ptr))
    return;

  memzone_t* zone = NULL;
  for (int i = 0; i < zone_count; i++)
  {
    if ((const byte *)ptr >= (const byte *)zones[i] &&
        (const byte *)ptr < (const byte *)zones[i] + zones[i]->size)
    {
      zone = zones[i];
      break;
    }
  }
  if (zone == NULL)
  {
    I_Error ("Z_Free: %p is out of zone", ptr);
  }

  prof_enter(CGD_Perf.DynamicAllocation);
  block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

  if (block->id != ZONEID)
  {
    I_Error ("Z_Free: freed a pointer without ZONEID: %p", ptr);
  }

  if (block->user > (void **)0x100)
  {
    // smaller values are not pointers
    // Note: OS-dependend?

    // clear the user's mark
    *block->user = 0;
  }

  /* CGDoom: usage statistics */
  CGD_Stats.ZoneUsed -= block->size;
  if (block->tag < PU_PURGELEVEL)
    CGD_Stats.ZoneStatic -= block->size;

  // mark as free
  block->user = NULL;
  block->tag = 0;
  block->id = 0;

  other = block->prev;

  if (!other->user)
  {
    // merge with previous free block
    other->size += block->size;
    other->next = block->next;
    other->next->prev = other;

    if (block == zone->rover)
    {
      zone->rover = other;
    }

    block = other;
  }

  other = block->next;
  if (!other->user)
  {
    // merge the next free block onto the end
    block->size += other->size;
    block->next = other->next;
    block->next->prev = block;

    if (other == zone->rover)
    {
      zone->rover = block;
    }
  }

  prof_leave(CGD_Perf.DynamicAllocation);
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT 64


void* Z_Malloc( int size,  int  tag,  void* user )
{
  prof_enter(CGD_Perf.DynamicAllocation);
  CGD_Stats.MemoryAllocated += size;

  int extra;
  memblock_t* start;
  memblock_t* rover;
  memblock_t* newblock;
  memblock_t* base;

  size = (size + (sizeof(void *)-1)) & ~(sizeof(void *)-1);

  // scan through the block list,
  // looking for the first free block
  // of sufficient size,
  // throwing out any purgable blocks along the way.

  // account for size of block header
  size += sizeof(memblock_t);

  int zone_start = zone_rover;
  int zone_no = zone_rover;
  memzone_t *zone = zones[zone_no];

  for (;;)
  {
    // if there is a free block behind the rover,
    //  back up over them
    base = zone->rover;

    if (!base->prev->user)
    {
      base = base->prev;
    }

    rover = base;
    start = base->prev;
    int failed = 0;

    do
    {
      if (rover == start)
      {
        // scanned all the way around the list
        failed = 1;
        break;
      }

      if (rover->user)
      {
        if (rover->tag < PU_PURGELEVEL)
        {
          // hit a block that can't be purged,
          //  so move base past it
          base = rover = rover->next;
        }
        else
        {
          // free the rover block (adding the size to base)

          // the rover can be the base block
          base = base->prev;
          Z_Free ((byte *)rover+sizeof(memblock_t));
          base = base->next;
          rover = base->next;
        }
      }
      else
        rover = rover->next;
    } while (base->user || base->size < size);

    if (failed)
    {
      if (zone_no == zone_count - 1)
      {
        I_Error ("Z_Malloc(%d) failure", size);
      }

      /* When rotating, perform a full cycle before defaulting to the last
         zone; this preserves the last zone to avoid fragmentation and absorb
         occasional large chunks */
      zone_no++;
      if(zone_no == zone_count - 1)
        zone_no = 0;
      if(zone_no == zone_start)
        zone_no = zone_count - 1;

      zone = zones[zone_no];
    }
    else break;
  }

  // found a block big enough
  extra = base->size - size;

  if (extra >  MINFRAGMENT)
  {
    // there will be a free fragment after the allocated block
    newblock = (memblock_t *) ((byte *)base + size );
    newblock->size = extra;

    // NULL indicates free block.
    newblock->user = NULL;
    newblock->tag = 0;
    newblock->prev = base;
    newblock->next = base->next;
    newblock->next->prev = newblock;

    base->next = newblock;
    base->size = size;
  }

  if (user)
  {
    // mark as an in use block
    base->user = (void **)user;
    *(void **)user = (void *) ((byte *)base + sizeof(memblock_t));
  }
  else
  {
    if (tag >= PU_PURGELEVEL)
    {
      I_Error ("Z_Malloc: an owner is required for purgable blocks");
    }

    // mark as in use, but unowned
    base->user = (void **)2;
  }
  base->tag = tag;

  /* CGDoom: usage statistics */
  CGD_Stats.ZoneUsed += base->size;
  if (CGD_Stats.ZoneUsed > CGD_Stats.ZoneUsedPeak)
    CGD_Stats.ZoneUsedPeak = CGD_Stats.ZoneUsed;
  if (tag < PU_PURGELEVEL)
  {
    CGD_Stats.ZoneStatic += base->size;
    if (CGD_Stats.ZoneStatic > CGD_Stats.ZoneStaticPeak)
      CGD_Stats.ZoneStaticPeak = CGD_Stats.ZoneStatic;
  }

  // next allocation will start looking here
  zone->rover = base->next;
  if (zone_no != zone_count - 1)
    zone_rover = zone_no;

  base->id = ZONEID;

#ifdef RANGECHECK
  if (Z_PoisonValue >= 0)
    memset((byte *)base + sizeof(memblock_t), Z_PoisonValue,
      base->size - sizeof(memblock_t));
#endif

  prof_leave(CGD_Perf.DynamicAllocation);
  return (void *) ((byte *)base + sizeof(memblock_t));
}



//
// Z_FreeTags
//
void Z_FreeTags(int lowtag,int  hightag )
{
  memblock_t* block;
  memblock_t* next;

  for (int i = 0; i < zone_count; i++)
  {
    memzone_t* mainzone = zones[i];

    for(block = mainzone->blocklist.next;block != &mainzone->blocklist;block = next)
    {
      // get link before freeing
      next = block->next;

      // free block?
      if (!block->user)
      {
        continue;
      }

      if (block->tag >= lowtag && block->tag <= hightag)
      {
        Z_Free ( (byte *)block+sizeof(memblock_t));
      }
    }
  }
}

//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
  memblock_t* block;

  for (int i = 0; i < zone_count; i++)
  {
    memzone_t* mainzone = zones[i];

    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
      if (block->next == &mainzone->blocklist)
      {
        // all blocks have been hit
        break;
      }

      if((byte *)block + block->size != (byte *)block->next)
      {
        I_Error("Z_CheckHeap: block size does not touch the next block\n");
      }

      if(block->next->prev != block)
      {
        I_Error("Z_CheckHeap: next block doesn't have proper back link\n");
      }

      if(!block->user && !block->next->user)
      {
        I_Error("Z_CheckHeap: two consecutive free blocks\n");
      }
    }
  }
}




//
// Z_ChangeTag
//
void Z_ChangeTag2(const void* ptr,int tag )
{
  memblock_t* block;

  if(ptr == NULL || PL_IsWadPointer(ptr))
    return;

  block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

  if (block->id != ZONEID)
  {
    I_Error ("Z_ChangeTag: freed a pointer without ZONEID: %p", ptr);
  }

  if (tag >= PU_PURGELEVEL && (uintptr_t)block->user < 0x100)
  {
    I_Error ("Z_ChangeTag: an owner is required for purgable blocks");
  }

  /* CGDoom: usage statistics */
  if (block->tag < PU_PURGELEVEL && tag >= PU_PURGELEVEL)
    CGD_Stats.ZoneStatic -= block->size;
  else if (block->tag >= PU_PURGELEVEL && tag < PU_PURGELEVEL)
  {
    CGD_Stats.ZoneStatic += block->size;
    if (CGD_Stats.ZoneStatic > CGD_Stats.ZoneStaticPeak)
      CGD_Stats.ZoneStaticPeak = CGD_Stats.ZoneStatic;
  }

  block->tag = tag;
}



//
// Z_FreeMemory
//
int Z_FreeMemory (int zone)
{
  if (zone < 0)
  {
    int free = 0;
    for (int i = 0; i < zone_count; i++)
      free += Z_FreeMemory (i);
    return free;
  }

  memblock_t* block;
  int free = 0;

  memzone_t* mainzone = zones[zone];

  for(block = mainzone->blocklist.next;block != &mainzone->blocklist;block = block->next)
  {
    if (!block->user || block->tag >= PU_PURGELEVEL)
    {
      free += block->size;
    }
  }

  return free;
}



//
// Z_LargestFreeBlock
//
int Z_LargestFreeBlock (int zone)
{
  if (zone < 0)
  {
    int maxsize = 0;
    for (int i = 0; i < zone_count; i++) {
      int n = Z_LargestFreeBlock(i);
      if (n > maxsize)
        maxsize = n;
    }
    return maxsize;
  }

  memblock_t* block;
  int maxsize = 0;

  memzone_t* mainzone = zones[zone];

  for(block = mainzone->blocklist.next;block != &mainzone->blocklist;block = block->next)
  {
    if (!block->user || block->tag >= PU_PURGELEVEL)
    {
      if (block->size > maxsize)
        maxsize = block->size;
    }
  }

  return maxsize;
}



//
// Z_ZoneCount, Z_ZoneSize (CGDoom)
//
int Z_ZoneCount (void)
{
  return zone_count;
}

int Z_ZoneSize (int zone)
{
  if (zone < 0 || zone >= zone_count)
    return 0;
  return zones[zone]->size;
}
