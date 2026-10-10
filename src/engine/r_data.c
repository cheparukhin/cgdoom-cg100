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
//	Preparation of data for rendering,
//	generation of lookups, caching, retrieval by name.
//
//-----------------------------------------------------------------------------


//static const char 


#include "i_system.h"
#include "z_zone.h"

#include "m_swap.h"

#include "w_wad.h"

#include "doomdef.h"
#include "r_local.h"
#include "p_local.h"

#include "doomstat.h"

#ifdef CGD_SH4
#include <stdint.h>
#include "sh4/cgd_sh4.h"
#endif
#include "r_sky.h"

#include "os.h"
#include "cgdoom.h"

#include "r_data.h"

//
// Graphics.
// DOOM graphics for walls and sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
// 



//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//
typedef struct
{
  short			originxLE;
  short			originyLE;
  short			patchLE;
  short			stepdirLE;
  short			colormapLE;
} mappatch_t;


//
// Texture definition.
// A DOOM wall texture is a list of patches
// which are to be combined in a predefined order.
//
typedef struct
{
  char			name[8];
  //boolean			masked;
  int				masked;			//Carmack, you stupid fucknut, int and boolean DO NOT have the same length!!!!
  short			widthLE;
  short			heightLE;
  int				obsolete;
  short			patchcountLE;
  mappatch_t		patches[1];
} maptexture_t;

/* CGDoom: read a little-endian short field from a possibly misaligned
   maptexture_t / mappatch_t given as a byte pointer */
#define MT_SHORT(p, f) \
  ((short)SHORTFromBytes((const byte *)(p) + offsetof(maptexture_t, f##LE)))
#define MP_SHORT(p, f) \
  ((short)SHORTFromBytes((const byte *)(p) + offsetof(mappatch_t, f##LE)))


// A single patch from a texture definition,
//  basically a rectangular area within
//  the texture rectangle.
typedef struct
{
  // Block origin (allways UL),
  // which has allready accounted
  // for the internal origin of the patch.
  short			originx;	
  short			originy;
  int				patch;
} texpatch_t;


// A maptexturedef_t describes a rectangular texture,
//  which is composed of one or more mappatch_t structures
//  that arrange graphic patches.
typedef struct
{
  // Keep name for switch changing, etc.
  char			name[8];		
  short			width;
  short			height;

  // All the patches[patchcount]
  //  are drawn back to front into the cached texture.
  short			patchcount;
  texpatch_t		patches[1];		

} texture_t;



int					firstflat;
int					lastflat;
int					numflats;

int					firstpatch;
int					lastpatch;
int					numpatches;

int					firstspritelump;
int					lastspritelump;
int					numspritelumps;

int					numtextures;
texture_t**			textures;


int*				texturewidthmask;
// needed for texture pegging
fixed_t*			textureheight;		
int*				texturecompositesize;
short**				texturecolumnlump;
unsigned short**	texturecolumnofs;
byte**				texturecomposite;

// for global animation
int*				flattranslation;
int*				texturetranslation;

// needed for pre rendering
fixed_t*			spritewidth;	
fixed_t*			spriteoffset;
fixed_t*			spritetopoffset;

const lighttable_t		*colormaps;


//
// MAPTEXTURE_T CACHING
// When a texture is first needed,
//  it counts the number of composite columns
//  required in the texture and allocates space
//  for a column directory and any new columns.
// The directory will simply point inside other patches
//  if there is only one patch in a given column,
//  but any columns with multiple patches
//  will have new column_ts generated.
//
//
// R_DrawColumnInCache
// Clip and draw a column
//  from a patch into a cached post.
//
static void R_DrawColumnInCache(const column_t* patch, byte* cache, int originy, int cacheheight )
{
  int		count;
  int		position;
  byte*	source;
  byte*	dest CGD_UNUSED;

  dest = (byte *)cache + 3;

  while (patch->topdelta != 0xff)
  {
    source = (byte *)patch + 3;
    count = patch->length;
    position = originy + patch->topdelta;

    if (position < 0)
    {
      count += position;
      position = 0;
    }

    if (position + count > cacheheight)
      count = cacheheight - position;

    if (count > 0)
      memcpy (cache + position, source, count);

    patch = (column_t *)(  (byte *)patch + patch->length + 4);
  }
}

// R_GenerateComposite
// Using the texture definition,
//  the composite texture is created from the patches,
//  and each column is cached.
//
static void R_GenerateComposite (int texnum)
{
  byte*			block;
  texture_t*		texture;
  texpatch_t*		patch;	
  const patch_t*		realpatch;
  int				x;
  int				x1;
  int				x2;
  int				i;
  const column_t*		patchcol;
  short*			collump;
  unsigned short*	colofs;

  texture = textures[texnum];

  /* CGDoom: the renderer reads up to 127 bytes past a column start (&127
     wrap), and columns not covered by any patch are never written; clear the
     block plus a pad so that such reads give the same result on every
     platform instead of depending on old heap contents */
  block = (byte*)Z_Malloc (texturecompositesize[texnum] + W_LUMP_PAD,PU_STATIC, &texturecomposite[texnum]);
  memset (block, 0, texturecompositesize[texnum] + W_LUMP_PAD);

  collump = texturecolumnlump[texnum];
  colofs = texturecolumnofs[texnum];

  // Composite the columns together.
  patch = texture->patches;

  for (i=0 , patch = texture->patches;i<texture->patchcount; i++, patch++)
  {
    realpatch = W_CacheLumpNumPatch(patch->patch, PU_CACHE);
    x1 = patch->originx;
    x2 = x1 + SHORT(realpatch->width);

    if (x1<0)
      x = 0;
    else
      x = x1;

    if (x2 > texture->width)
      x2 = texture->width;

    for ( ; x<x2 ; x++)
    {
      // Column does not have multiple patches?
      if (collump[x] >= 0)
      {
        continue;
      }

      patchcol = (const column_t *)((const byte *)realpatch + INTFromBytes((const byte*)&(realpatch->columnofsLE[x-x1])));
      R_DrawColumnInCache(patchcol,block + colofs[x],patch->originy,texture->height);
    }

  }

  // Now that the texture has been built in column cache,
  //  it is purgable from zone memory.
  Z_ChangeTag (block, PU_CACHE);
}




//
// R_GenerateLookup
//
void R_GenerateLookup (int texnum)
{
  texture_t*		texture;
  texpatch_t*		patch;	
  const patch_t*	realpatch;
  int				x;
  int				x1;
  int				x2;
  int				i;
  short*			collump;
  unsigned short*	colofs;
  /* CGDoom: patchcount[texture->width]; was a fixed byte[320] which
     overflowed for wider textures (Freedoom, PWAD skies) */
  static byte      *patchcount = NULL;
  static int        patchcount_size = 0;

  texture = textures[texnum];

  // Composited texture not created yet.
  texturecomposite[texnum] = 0;

  texturecompositesize[texnum] = 0;
  collump = texturecolumnlump[texnum];
  colofs = texturecolumnofs[texnum];

  // Now count the number of columns
  //  that are covered by more than one patch.
  // Fill in the lump / offset, so columns
  //  with only a single patch are all done.
  //patchcount = (byte *)alloca (texture->width);
  if (texture->width > patchcount_size)
  {
    patchcount_size = texture->width;
    patchcount = CGD_realloc(patchcount, patchcount_size);
  }
  memset (patchcount, 0, texture->width);
  patch = texture->patches;

  for (i=0 , patch = texture->patches;     i<texture->patchcount;    i++, patch++)
  {
    realpatch = W_CacheLumpNumPatch(patch->patch, PU_CACHE);
    x1 = patch->originx;
    x2 = x1 + SHORT(realpatch->width);

    if (x1 < 0)
      x = 0;
    else
      x = x1;

    if (x2 > texture->width)
      x2 = texture->width;

    for ( ; x<x2 ; x++)
    {
      patchcount[x]++;
      collump[x] = (short)patch->patch;
      colofs[x] = (unsigned short)(3 + INTFromBytes((const byte*)&(realpatch->columnofsLE[x-x1])));
    }
  }

  for (x=0 ; x<texture->width ; x++)
  {
    if (!patchcount[x])
    {
      /* CGDoom: a warning in vanilla, not an error (I_Error is fatal) */
      printf ("R_GenerateLookup: column without a patch (%.8s)\n",texture->name);
      return;
    }

    if (patchcount[x] > 1)
    {
      // Use the cached block.
      collump[x] = -1;	
      colofs[x] = (unsigned short)texturecompositesize[texnum];

      if (texturecompositesize[texnum] > 0x10000-texture->height)
      {
        I_Error ("R_GenerateLookup: texture %i is >64k",texnum);
      }

      texturecompositesize[texnum] += texture->height;
    }
  }

}




//
// R_GetColumn
// CGDoom (step 2): static inline in r_data.h; this is its composite path.
// (CGDoom's disabled "composite size limit" hack was dropped with it.)
//
const byte* R_GetColumnComposite ( int tex )
{
  R_GenerateComposite (tex);
  return texturecomposite[tex];
}





//
// R_InitTextures
// Initializes the texture list
//  with the textures from the world map.
//
void R_InitTextures (void)
{
  const byte*	mtexture;
  texture_t*		texture;
  const byte*		mpatch;
  texpatch_t*		patch;

  int				i;
  int				j;

  const char*			maptex;
  const char*			maptex2;
  const char*			maptex1;


  const char*			names;
  const char*			name_p;

  int*			patchlookup;

  int				totalwidth;
  int				nummappatches;
  int				offset;
  int				maxoff;
  int				maxoff2;
  int				numtextures1;
  int				numtextures2;

  const char*			directory;

  /*int				temp1;
  int				temp2;
  int				temp3;*/



  // Load the patch names from pnames.lmp.

  names = (const char *)W_CacheLumpNameConst ("PNAMES", PU_STATIC);
  nummappatches = INTFromBytes((const byte*)names );
  name_p = names+4;
  patchlookup = (int*)CGD_malloc(nummappatches*sizeof(*patchlookup));

  for (i=0 ; i<nummappatches ; i++)
  {
    patchlookup[i] = W_CheckNumForName (name_p+i*8);
  }

  Z_Free (names);

  // Load the map texture definitions from textures.lmp.
  // The data is contained in one or two lumps,
  //  TEXTURE1 for shareware, plus TEXTURE2 for commercial.
  maptex = maptex1 = (const char *)W_CacheLumpNameConst("TEXTURE1", PU_STATIC);
  numtextures1 = INTFromBytes((const byte*)maptex1);
  maxoff = W_LumpLength (W_GetNumForName ("TEXTURE1"));
  directory = maptex+4;

  if (W_CheckNumForName ("TEXTURE2") != -1)
  {
    maptex2 = (const char*)W_CacheLumpNameConst("TEXTURE2", PU_STATIC);
    numtextures2 = INTFromBytes((const byte*)maptex2);
    maxoff2 = W_LumpLength (W_GetNumForName ("TEXTURE2"));
  }
  else
  {
    maptex2 = NULL;
    numtextures2 = 0;
    maxoff2 = 0;
  }

  numtextures = numtextures1 + numtextures2;

  /* CGDoom: These used to be in PRAM; they are ordinary zone blocks now */
  textures = (texture_t **)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  texturecolumnlump = (short**)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  texturecolumnofs = (unsigned short**)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  texturecomposite = (byte**)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  texturecompositesize = (int*)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  texturewidthmask = (int*)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);
  textureheight = (fixed_t*)Z_Malloc(numtextures*sizeof(void *), PU_STATIC, 0);

  totalwidth = 0;

  /*//	Really complex printing shit...
  temp1 = W_GetNumForName ("S_START");  // P_???????
  temp2 = W_GetNumForName ("S_END") - 1;
  temp3 = ((temp2-temp1+63)/64) + ((numtextures+63)/64);
  printf("[");
  for (i = 0; i < temp3; i++)
  printf(" ");
  printf("         ]");
  for (i = 0; i < temp3; i++)
  printf("\x8");
  printf("\x8\x8\x8\x8\x8\x8\x8\x8\x8\x8");*/

  for (i=0 ; i<numtextures ; i++, directory+=4)
  {
    //if (!(i&63))printf (".");

    if (i == numtextures1)
    {
      // Start looking in second texture file.
      maptex = maptex2;
      maxoff = maxoff2;
      directory = maptex+4;
    }

    offset = INTFromBytes((const byte*)directory);

    if (offset > maxoff)
      I_Error ("R_InitTextures: bad texture directory");

    /* CGDoom: texture entries are only 2-aligned in the lump; access them
       through byte pointers only (no typed member access, which lets the
       compiler assume 4-byte alignment and emit faulting loads on SH4) */
    mtexture = (const byte *)maptex + offset;

    texture = textures[i] = (texture_t*)Z_Malloc (sizeof(texture_t)+ sizeof(texpatch_t)*(MT_SHORT(mtexture, patchcount)-1),PU_STATIC, 0);

    texture->width = MT_SHORT(mtexture, width);
    texture->height = MT_SHORT(mtexture, height);
    texture->patchcount = MT_SHORT(mtexture, patchcount);

    memcpy (texture->name, mtexture + offsetof(maptexture_t, name), sizeof(texture->name));
    mpatch = mtexture + offsetof(maptexture_t, patches);
    patch = &texture->patches[0];

    for (j=0 ; j<texture->patchcount ; j++, mpatch += sizeof(mappatch_t), patch++)
    {
      patch->originx = MP_SHORT(mpatch, originx);
      patch->originy = MP_SHORT(mpatch, originy);
      patch->patch = patchlookup[MP_SHORT(mpatch, patch)];
      if (patch->patch == -1)
        I_Error ("R_InitTextures: Missing patch in texture %s", texture->name);
    }		
    texturecolumnlump[i] = (short*)Z_Malloc (texture->width*2, PU_STATIC,0);
    texturecolumnofs[i] = (unsigned short*)Z_Malloc (texture->width*2, PU_STATIC,0);

    j = 1;
    while (j*2 <= texture->width)
      j<<=1;

    texturewidthmask[i] = j-1;
    textureheight[i] = texture->height<<FRACBITS;

    totalwidth += texture->width;
  }

  Z_Free (maptex1);
  if (maptex2)
    Z_Free (maptex2);

  // Precalculate whatever possible.	
  for (i=0 ; i<numtextures ; i++)
    R_GenerateLookup (i);

  // Create translation table for global animation.
  texturetranslation = (int *)Z_Malloc((numtextures+1)*sizeof(void *), PU_STATIC, 0);

  for (i=0 ; i<numtextures ; i++)
    texturetranslation[i] = i;

  //Nspire has no alloca() function for temporary memory, so we have to use malloc() and free() to compensate
  //CGD: Prizm has malloc / free, but only 128 KB heap
  free(patchlookup);
}


//
// R_InitFlats
//
void R_InitFlats (void)
{
  int		i;

  firstflat = W_GetNumForName ("F_START") + 1;
  lastflat = W_GetNumForName ("F_END") - 1;
  numflats = lastflat - firstflat + 1;

  // Create translation table for global animation.
  flattranslation = (int *)Z_Malloc((numflats+1)*4, PU_STATIC, 0);

  for (i=0 ; i<numflats ; i++)
    flattranslation[i] = i;
}


//
// R_InitSpriteLumps
// Finds the width and hoffset of all sprites in the wad,
//  so the sprite does not need to be cached completely
//  just for having the header info ready during rendering.
//
void R_InitSpriteLumps (void)
{
  int		i;
  const patch_t	*patch;

  firstspritelump = W_GetNumForName ("S_START") + 1;
  lastspritelump = W_GetNumForName ("S_END") - 1;

  numspritelumps = lastspritelump - firstspritelump + 1;
  spritewidth = (fixed_t *)Z_Malloc (numspritelumps*4, PU_STATIC, 0);
  spriteoffset = (fixed_t *)Z_Malloc (numspritelumps*4, PU_STATIC, 0);
  spritetopoffset = (fixed_t *)Z_Malloc (numspritelumps*4, PU_STATIC, 0);

  for (i=0 ; i< numspritelumps ; i++)
  {
    //if (!(i&63))printf (".");

    patch = W_CacheLumpNumPatch(firstspritelump+i, PU_CACHE);
    spritewidth[i] = SHORT(patch->width)<<FRACBITS;
    spriteoffset[i] = SHORT(patch->leftoffset)<<FRACBITS;
    spritetopoffset[i] = SHORT(patch->topoffset)<<FRACBITS;
  }
}



//
// R_InitColormaps
//
void R_InitColormaps (void)
{
  //int	lump, length;

  // Load in the light tables, 
  //  256 byte align tables.
  //lump = W_GetNumForName("COLORMAP"); 
  /*length = W_LumpLength (lump) + 255; 
  colormaps = (lighttable_t *)Z_Malloc (length, PU_STATIC, 0); 
  colormaps = (lighttable_t *)( ((int)colormaps + 255)&~0xff); 
  W_ReadLump (lump,colormaps); 
*/
  colormaps = (lighttable_t *)W_CacheLumpNameConst ("COLORMAP", PU_STATIC);

#ifdef CGD_SH4
  /* The SH4 drawers index a "rotated" copy of the whole lump with the
     sign-extended texel (src/engine/sh4/README.md §2, §7.2): one copy of
     nmaps*256 bytes (34 maps = 8704 for DOOM), 32-byte aligned, kept for the
     session like the lump itself. Colormap pointers outside the registered
     lump (none in vanilla) make the drawers fall back to C. */
  {
    int nmaps = W_LumpLength (W_GetNumForName ("COLORMAP")) / 256;
    byte *rot = NULL;

    if (nmaps > 0)
    {
      if (nmaps > 256)
        nmaps = 256;
      rot = (byte *)Z_Malloc (nmaps * 256 + 31, PU_STATIC, NULL);
      rot = (byte *)(((uintptr_t)rot + 31) & ~(uintptr_t)31);
    }
    cgd_sh4_set_colormaps (colormaps, nmaps, rot);
  }
#endif
}



//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//
void R_InitData (void)
{
  printf ("InitTextures\n");
  R_InitTextures ();
  printf ("InitFlats\n");
  R_InitFlats ();
  printf ("InitSprites\n");
  R_InitSpriteLumps ();
  printf ("InitColormaps\n");
  R_InitColormaps ();
}



//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
int R_FlatNumForName (const char* name)
{
  int		i;

  //Note: This copy may seem strange, but we really really really -don't- want to corrupt the value at the original pointer
  //CGD: problem solved: int R_FlatNumForName (char* name) -> int R_FlatNumForName (const char* name)
  i = W_CheckNumForName (name);
  if (i == -1)
  {
    I_Error ("R_FlatNumForName: %s not found",name);
  }

  return i - firstflat;
}




//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
int	R_CheckTextureNumForName (const char *name)
{
  int     i;
  // "NoTexture" marker.
  if(name[0] == '-')
  {
    return 0;
  }

  for(i=0 ; i<numtextures ; i++)
  {
    if(!strnicmp(textures[i]->name,name,8))
    {
      return i;
    }
  }
  return -1;
}



//
// R_TextureNumForName
// Calls R_CheckTextureNumForName,
//  aborts with error message.
//
int	R_TextureNumForName (const char* name)
{
  int		i;

  i = R_CheckTextureNumForName (name);

  if (i==-1)
  {
    I_Error ("R_TextureNumForName: %s not found", name);
  }
  return i;
}



//
// R_PrecacheLevel
// Preloads all relevant graphics for the level.
//
int		flatmemory;
int		texturememory;
int		spritememory;

/* CGDoom step 3: enabled again for slow WAD mappings (W_WadSlow, called by
   P_SetupLevel): every lump the level's flats, wall textures and things'
   sprites use is cached (PU_CACHE) up front, which copies it into the zone
   while the zone has room (W_CopySlowLump). Vanilla skipped this during
   demo playback; here it is done for demos too (it only moves lumps into
   RAM, the output is the same). Step 3 review: each lump goes through
   W_SlowPrecacheWanted (w_wad.c), so precaching stops at the zone reserve
   for unmappable lumps too and leaves already cached lumps alone. Flats are skipped with potato floors (only
   their average colour is used). */
void R_PrecacheLevel (void)
{
  char*			flatpresent;
  char*			texturepresent;
  char*			spritepresent;

  int				i;
  int				j;
  int				k;
  int				lump;

  texture_t*		texture;
  thinker_t*		th;
  spriteframe_t*	sf;

  // Precache flats.
  //flatpresent = alloca(numflats);
  flatpresent = (char*)CGD_malloc(numflats);
  memset (flatpresent,0,numflats);	

  for (i=0 ; i<numsectors ; i++)
  {
    flatpresent[sectors[i].floorpic] = 1;
    flatpresent[sectors[i].ceilingpic] = 1;
  }

  flatmemory = 0;

  for (i=0 ; i<numflats ; i++)
  {
    if (flatpresent[i])
    {
      lump = firstflat + i;
      flatmemory += lumpinfo[lump].size;
      if (!CGD_Options.FlatFloors && W_SlowPrecacheWanted(lump))
        W_CacheLumpNumConst(lump, PU_CACHE);
    }
  }

  // Precache textures.
  //texturepresent = alloca(numtextures);
  texturepresent = (char*)CGD_malloc(numtextures);
  memset (texturepresent,0, numtextures);

  for (i=0 ; i<numsides ; i++)
  {
    texturepresent[sides[i].toptexture] = 1;
    texturepresent[sides[i].midtexture] = 1;
    texturepresent[sides[i].bottomtexture] = 1;
  }

  // Sky texture is always present.
  // Note that F_SKY1 is the name used to
  //  indicate a sky floor/ceiling as a flat,
  //  while the sky texture is stored like
  //  a wall texture, with an episode dependend
  //  name.
  texturepresent[skytexture] = 1;

  texturememory = 0;
  for (i=0 ; i<numtextures ; i++)
  {
    if (!texturepresent[i])
      continue;

    texture = textures[i];

    for (j=0 ; j<texture->patchcount ; j++)
    {
      lump = texture->patches[j].patch;
      texturememory += lumpinfo[lump].size;
      if (W_SlowPrecacheWanted(lump))
        W_CacheLumpNumConst(lump , PU_CACHE);
    }
  }

  // Precache sprites.
  //spritepresent = alloca(numsprites);
  spritepresent = (char*)CGD_malloc (numsprites);
  memset (spritepresent,0, numsprites);

  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
  {
    if (th->function == TP_MobjThinker)
      spritepresent[((mobj_t *)th->target)->sprite] = 1;
  }

  spritememory = 0;
  for (i=0 ; i<numsprites ; i++)
  {
    if (!spritepresent[i])
      continue;

    for (j=0 ; j<sprites[i].numframes ; j++)
    {
      sf = &sprites[i].spriteframes[j];
      for (k=0 ; k<8 ; k++)
      {
        lump = firstspritelump + sf->lump[k];
        spritememory += lumpinfo[lump].size;
        if (W_SlowPrecacheWanted(lump))
          W_CacheLumpNumConst(lump , PU_CACHE);
      }
    }
  }

  //Nspire has no alloca() function for temporary memory, so we have to use malloc() and free() to compensate
  free(flatpresent);
  free(texturepresent);
  free(spritepresent);
}
 
