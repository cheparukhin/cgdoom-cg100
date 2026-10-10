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
//	WAD I/O functions.
//
//-----------------------------------------------------------------------------


#ifndef __W_WAD__
#define __W_WAD__


#ifdef __GNUG__
#pragma interface
#endif


#include "r_defs.h"
#include "z_zone.h"
//
// TYPES
//
typedef struct
{
    // Should be "IWAD" or "PWAD".
    char		identification[4];		
    int			numlumps;
    int			infotableofs;
    
} wadinfo_t;


typedef struct
{
    int			filepos;
    int			size;
    char		name[8];
    
} filelump_t;

//
// WADFILE I/O related stuff.
//
typedef struct
{
    char	name[8];
//    int		handle;
    int		position;
    int		size;
} lumpinfo_t;


extern	void**		lumpcache;
extern	lumpinfo_t*	lumpinfo;

/* CGDoom (step 2): W_LumpMapped[lump] != 0 when lumpcache[lump] is a
   PL_WadMap pointer. Such pointers are not zone blocks: they are never
   purged, Z_ChangeTag ignores them, and they stay valid until PL_WadClose. */
extern	byte*		W_LumpMapped;

/* CGDoom step 3: PL_WadIsSlow() at W_InitMultipleFiles: mapped lumps are
   copied into the zone while it has room (w_wad.c), and P_SetupLevel
   precaches the level's graphics (R_PrecacheLevel). */
extern	boolean		W_WadSlow;
/* Before loading a level (slow mapping only, else nothing): purge the
   purgeable lumps and forget the lumps referenced in place (w_wad.c) */
void	W_SlowLevelStart (void);
/* Step 3 review: R_PrecacheLevel's test for one lump: true if the lump is not
   cached yet and a copy of it keeps the zone above the slow-mode reserve
   (w_wad.c, W_CopySlowLump) */
boolean	W_SlowPrecacheWanted (int lump);

/* CGDoom: The renderer may read a little past the end of a lump (column
   index wraps at 128, sprite rounding). Cached lumps are therefore accessed
   with W_LUMP_PAD extra bytes: a lump is referenced in place only if the
   platform can map it with the whole pad, inside the WAD (a lump within
   W_LUMP_PAD bytes of the end of the WAD is always copied), and heap copies
   carry the same trailing WAD bytes (zero-filled past the end). This
   makes the output independent of whether lumps are mapped or copied. */
#define W_LUMP_PAD 136

int    W_InitMultipleFiles (void);
//void    W_Reload (void);

int		W_CheckNumForName (const char* name);
int		W_GetNumForName (const char* name);

int		W_LumpLength (int lump);
void    W_ReadLump (int lump, void *dest);
void*   W_ReadLumpWithZ_Malloc(int lump,int tag,int iEnableFlash);

//void* W_CacheLumpNum (int lump, int tag);
void *W_CacheLumpNumVolatile(int lump, int tag);
const void* W_CacheLumpNumConst (int lump, int tag);

const patch_t *W_CacheLumpNumPatch(int lump, int tag);

/* CGDoom (step 2): W_CacheLumpNumConst for hot paths (R_GetColumn). When the
   lump is already cached and W_CacheLumpNumConst's Z_ChangeTag would change
   nothing (a mapped lump, or a zone block that already has this tag), the
   pointer is returned without any call; otherwise this is
   W_CacheLumpNumConst. Same result and same zone state in every case. The
   lump number must be valid (no numlumps check on the fast path). */
static inline const void *W_CacheLumpNumConstFast(int lump, int tag)
{
  const void *p = lumpcache[lump];
  if (p && (W_LumpMapped[lump] || Z_BlockTag(p) == tag))
    return p;
  return W_CacheLumpNumConst(lump, tag);
}

//void* W_CacheLumpName (const char* name, int tag);
void* W_CacheLumpNameVolatile (const char* name, int tag);
const void* W_CacheLumpNameConst (const char* name, int tag);
const patch_t *W_CacheLumpNamePatch(const char* name, int tag);



//void strupr (char* s);


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
