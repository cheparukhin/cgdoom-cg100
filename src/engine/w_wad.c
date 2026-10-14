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
// Handles WAD file header, directory, lump I/O.
//
//-----------------------------------------------------------------------------


//static const char 


#include "os.h"
#include "cgdoom.h"

#include "doomtype.h"
#include "m_swap.h"
#include "i_system.h"
#include "z_zone.h"

#ifdef __GNUG__
#pragma implementation "w_wad.h"
#endif
#include "w_wad.h"




//
// GLOBALS
//

// Location of each lump on disk.
lumpinfo_t*  lumpinfo;
static int  numlumps = 0;
/* End of the WAD data (directory or last lump), for lump padding */
static int  wad_end = 0;

void**   lumpcache = NULL;
byte*    W_LumpMapped = NULL;
boolean  W_WadSlow = false;


//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// CGDoom: reads the directory of the single WAD opened by the platform
// (PL_WadOpen) through PL_WadRead.
//

static int W_AddFile ()
{
  wadinfo_t  header;
  lumpinfo_t*  lump_p;
  int    i;
  int    length;
  int    startlump;
  filelump_t*  fileinfo;
  filelump_t*  fileinfo_mem;

  startlump = numlumps;

  if (PL_WadRead(&header, 0, sizeof(header)) != (int)sizeof(header))
    return 0;
  if (strncmp(header.identification,"IWAD",4))
  {
    // Homebrew levels?
    if (strncmp(header.identification,"PWAD",4))
      return 0; // CX port
  }
  header.numlumps = LONG(header.numlumps);
  header.infotableofs = LONG(header.infotableofs);
  if (header.numlumps <= 0 || header.numlumps > (1 << 20))
    return 0;
  length = header.numlumps * sizeof(filelump_t);
  fileinfo = fileinfo_mem = CGD_malloc(length);
  if (PL_WadRead(fileinfo, header.infotableofs, length) != length)
  {
    free(fileinfo_mem);
    return 0;
  }
  numlumps += header.numlumps;
  wad_end = header.infotableofs + length;

  // Fill in lumpinfo (when handling multiples files, should realloc)
  lumpinfo = (lumpinfo_t *)Z_Malloc (numlumps*sizeof(lumpinfo_t), PU_STATIC, 0);

  lump_p = &lumpinfo[startlump];

  for (i=startlump ; i<numlumps ; i++,lump_p++, fileinfo++)
  {
    lump_p->position = LONG(fileinfo->filepos);
    lump_p->size = LONG(fileinfo->size);
    if (lump_p->position + lump_p->size > wad_end)
      wad_end = lump_p->position + lump_p->size;
    memset(lump_p->name,0,8);//hack
    strncpy (lump_p->name, fileinfo->name, 8);
  }

  free(fileinfo_mem);
  return 1; // CX port
}

//
// W_InitMultipleFiles
// CGDoom: only one WAD (the one opened with PL_WadOpen) is supported. Must be
// called once, after the zones have been set up.
//
int W_InitMultipleFiles(void)
{
  // open all the files, load headers, and count lumps
  numlumps = 0;

  // will be realloced as lumps are added
  lumpinfo = NULL;
  if(!W_AddFile ())
  {
    return 0; // CX port
  }

  if (!numlumps)
  {
    I_Error ("W_InitFiles: no files found");
  }

  // set up caching
  printf ("numlumps = %i \n",numlumps);
  lumpcache = (void **)Z_Malloc(numlumps * sizeof(*lumpcache), PU_STATIC, 0);
  memset(lumpcache, 0, numlumps * sizeof(*lumpcache));
  W_LumpMapped = (byte *)Z_Malloc(numlumps, PU_STATIC, 0);
  memset(W_LumpMapped, 0, numlumps);

  /* CGDoom step 3: RAM residency policy (see W_CopySlowLump) */
  W_WadSlow = PL_WadIsSlow() ? true : false;

  return 1; // CX port
}


//
// W_NumLumps
//
int W_NumLumps (void)
{
  return numlumps;
}



//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (const char* name)
{
  lumpinfo_t* lump_p;
  // scan backwards so patch lump files take precedence
  lump_p = lumpinfo + numlumps;

  while(lump_p-- != lumpinfo)
  {
    if(strnicmp(name,lump_p->name,8)==0 )
    {
      return lump_p - lumpinfo;
    }
  }
  // TFB. Not found.
  return -1;
}




//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (const char* name)
{
  int i;
  i = W_CheckNumForName (name);

  if (i == -1)
  {
    I_Error ("W_GetNumForName: %s not found!", name);
  }

  return i;
}


//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (int lump)
{
  if (lump >= numlumps)
    I_Error ("W_LumpLength: %i >= numlumps",lump);

  return lumpinfo[lump].size;
}



//
// W_ReadLump
// Loads the lump into the given buffer,
//  which must be >= W_LumpLength().
//
void W_ReadLump(int lump, void* dest )
{
  int     c;
  lumpinfo_t* l;

  if (lump >= numlumps)
  {
    I_Error ("W_ReadLump: %i >= numlumps",lump);
  }

  l = lumpinfo+lump;
  c = PL_WadRead(dest, l->position, l->size);

  if (c < l->size)
  {
    I_Error ("W_ReadLump: only read %i of %i on lump %i", c,l->size,lump);
  }
}


/* CGDoom step 3: RAM residency of a slow WAD mapping (design §7, research-
   critique C1). When PL_WadIsSlow() (e.g. the WAD is mapped from NOR flash,
   whose line fills are several times slower than RAM), a lump that could be
   referenced in place is copied into the zone instead, with the tag the
   caller asked for (so PU_CACHE copies stay purgeable, exactly like copies of
   unmappable lumps), as long as the zone keeps at least W_SLOW_RESERVE free
   bytes after the copy; otherwise it is referenced in place as before. The
   copy carries the same W_LUMP_PAD trailing bytes as a mapped lump, so the
   output does not depend on the choice (host -slowwad must reproduce every
   golden). A lump referenced in place stays so until the end (mapped
   pointers are never purged); a purged copy is copied again (or referenced,
   if the zone is fuller by then) the next time it is needed.

   Reserve: max(1/4 of the zone, 256 KiB). It leaves room for level data, the
   renderer's growable arrays and saves without making Z_Malloc purge cached
   lumps all the time; the copies themselves are purgeable anyway. "Free" is
   the zone size minus CGD_Stats.ZoneUsed (blocks of any tag, headers
   included), so purgeable copies count as used and the copies never evict
   each other to stay above the reserve. */
#define W_SLOW_RESERVE_MIN (256 * 1024)

static boolean W_CopySlowLump(int bytes)
{
  int total = 0, reserve;

  for (int i = 0; i < Z_ZoneCount(); i++)
    total += Z_ZoneSize(i);
  reserve = total / 4;
  if (reserve < W_SLOW_RESERVE_MIN)
    reserve = W_SLOW_RESERVE_MIN;
  /* bytes plus a block header and the alignment slack of Z_Malloc */
  return total - CGD_Stats.ZoneUsed - (bytes + 64) >= reserve;
}

/* CGDoom step 3: called by P_SetupLevel before a level is loaded, with a
   slow WAD mapping only. Copies count as used zone memory, so without this
   the previous level's cached lumps would keep the zone at the reserve and
   the new level's lumps would all be referenced in place: purge every
   purgeable (PU_PURGELEVEL..PU_CACHE) block, and forget the lumps referenced
   in place, so that both are copied again while the zone has room. A
   pointer to a forgotten mapped lump that someone still holds stays valid
   (mapped pointers live until PL_WadClose; Z_Free/Z_ChangeTag ignore them). */
void W_SlowLevelStart(void)
{
  int i;

  if (!W_WadSlow)
    return;

  Z_FreeTags (PU_PURGELEVEL, PU_CACHE);
  for (i = 0; i < numlumps; i++)
  {
    if (W_LumpMapped[i])
    {
      W_LumpMapped[i] = 0;
      lumpcache[i] = NULL;
    }
  }
}

/* Step 3 review: R_PrecacheLevel caches a lump only through this test. The
   reserve check of W_ReadLumpWithZ_Malloc covers mappable lumps only:
   unmappable ones (BFile mode, lumps across flash blocks, misaligned) are
   always copied, so an unbounded precache read the level's whole graphics
   set from the slow storage even when it was larger than the zone (host
   -fragment 16 -slowwad -zone 2048, e1m1: about 1.7 MiB of extra reads, the
   zone full and most of them purged again before use). Lumps that are
   already cached are skipped as well: re-caching them with PU_CACHE only
   changes their tag, which would demote a lump someone holds PU_STATIC. */
boolean W_SlowPrecacheWanted(int lump)
{
  return lump >= 0 && lump < numlumps && !lumpcache[lump]
    && W_CopySlowLump(lumpinfo[lump].size + W_LUMP_PAD);
}

/* CGDoom: Lumps are referenced in place when the platform can map them
   (PL_WadMap) and the pointer is 4-aligned; otherwise they are copied to
   the zone heap. Volatile lumps (iEnableFlash=0)
   are modified by the caller and always copied. With a slow WAD mapping
   (W_WadSlow), mappable lumps are also copied while the zone has room (step
   3, W_CopySlowLump above). */
void * W_ReadLumpWithZ_Malloc(int lump,int tag,int iEnableFlash)
{
  const void *mapped = NULL;
  int position, size, padded;

  if (lump >= numlumps)
  {
    I_Error ("W_ReadLump: %i >= numlumps",lump);
  }

  /* Lump plus the trailing W_LUMP_PAD bytes that exist in the WAD */
  position = lumpinfo[lump].position;
  size = lumpinfo[lump].size;
  padded = size + W_LUMP_PAD;
  if (padded > wad_end - position)
    padded = (wad_end - position > size) ? wad_end - position : size;

  /* Referenced in place only if the whole padded range is in the WAD (gint
     review R2-7): a lump whose pad would cross the end of the WAD is copied
     with a zero-filled pad instead, on every platform, so the renderer never
     reads whatever follows the WAD in the mapping (flash bytes, another
     file, unmapped memory) and the output does not depend on it. */
  if(iEnableFlash && padded == size + W_LUMP_PAD)
  {
    mapped = PL_WadMap(position, padded);
  }

  int aligned = (((uintptr_t)mapped & 3) == 0);

  /* Record statistics on loading unaligned but mapped lumps */
  if(mapped && !aligned) {
    CGD_Stats.UnalignedLumpsLoaded++;
    CGD_Stats.UnalignedLumpsLoadedTotal += lumpinfo[lump].size;
  }

  /* Step 3: a slow mapping is copied to the zone while there is room */
  int slowcopy = 0;
  if(mapped && aligned && W_WadSlow) {
    if(W_CopySlowLump(size + W_LUMP_PAD)) {
      slowcopy = 1;
      CGD_Stats.SlowLumpsCopied++;
      CGD_Stats.SlowLumpsCopiedTotal += size;
    }
    else
      CGD_Stats.SlowLumpsInPlace++;
  }

  /* Allocate only if the lump cannot be mapped, or is misaligned; otherwise,
     return the mapped address, which is just as good as what the heap would
     provide (unless the mapping is slow, see above). */
  if(!mapped || !aligned || slowcopy)
  {
    CGD_Stats.LumpsLoaded++;
    CGD_Stats.LumpsLoadedTotal += lumpinfo[lump].size;

    byte *dest = Z_Malloc (size + W_LUMP_PAD, tag, &lumpcache[lump]);
    int c = PL_WadRead(dest, position, padded);
    if (c < size)
      I_Error ("W_ReadLump: only read %i of %i on lump %i", c, size, lump);
    memset(dest + c, 0, size + W_LUMP_PAD - c);
  }
  else
  {
    lumpcache[lump] = (void *)mapped;
    W_LumpMapped[lump] = 1;
    CGD_Stats.LumpsReferenced++;
    CGD_Stats.LumpsReferencedTotal += lumpinfo[lump].size;
  }

  return lumpcache[lump];
}


//
// W_CacheLumpNum
//
static void* W_CacheLumpNumCommon( int lump, int tag,int iEnableFlash)
{
  if (lump >= numlumps)
  {
    I_Error ("W_CacheLumpNum: %i >= numlumps (%i)",lump,numlumps);
  }

  if (!lumpcache[lump])
  {
    W_ReadLumpWithZ_Malloc(lump,tag,iEnableFlash);
  }
  else
  {
    Z_ChangeTag (lumpcache[lump],tag);
  }

  if(lumpcache[lump] == NULL)
    I_Error ("W_CacheLumpNumCommon: Loading lump %d failed (%p)", lump,
      lumpcache[lump]);

  return lumpcache[lump];
}

//#define USE_FLASH 0

void *W_CacheLumpNumVolatile (int lump, int tag)
{
  return W_CacheLumpNumCommon(lump,tag,0);
}

const void* W_CacheLumpNumConst (int lump, int tag)
{
  return W_CacheLumpNumCommon(lump,tag,1);
}

/*void* W_CacheLumpNum(int lump, int tag)
{
  return W_CacheLumpNumCommon(lump,tag,USE_FLASH);
}*/

const patch_t* W_CacheLumpNumPatch(int lump, int tag)
{
 return (patch_t*) W_CacheLumpNumCommon(lump,tag,1);
}

//
// W_CacheLumpName
//
void* W_CacheLumpNameVolatile (const char* name, int tag)
{
  return W_CacheLumpNumCommon(W_GetNumForName(name),tag,0);
}


/*void* W_CacheLumpName (const char* name, int tag)
{
  return W_CacheLumpNumCommon(W_GetNumForName(name),tag,USE_FLASH);
}*/

const void* W_CacheLumpNameConst (const char* name, int tag)
{
  return W_CacheLumpNumCommon(W_GetNumForName(name),tag,1);
}

const patch_t *W_CacheLumpNamePatch(const char* name, int tag)
{
  return (patch_t *)W_CacheLumpNumCommon(W_GetNumForName(name),tag,1);
}

