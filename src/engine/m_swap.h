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
//	Endianess handling, swapping 16bit and 32bit.
//
//-----------------------------------------------------------------------------


#ifndef __M_SWAP__
#define __M_SWAP__


#ifdef __GNUG__
#pragma interface
#endif

// Endianess handling.
// WAD files are stored little endian.
#include <stdint.h>

unsigned short	SwapSHORT(const unsigned short);
unsigned long	SwapLONG(const unsigned long);

/* CGDoom port (step 2, notes/engine-changes.md): the little-endian readers
   are static inline (they were out-of-line calls on every patch column).

   SHORTFromBytes / INTFromBytes read a little-endian value byte by byte and
   accept any alignment: WAD data can be misaligned (mapped lumps with
   TrustUnalignedLumps, fields inside maptexture_t), and SH4 faults on
   misaligned 16/32-bit loads. */
static inline unsigned short SHORTFromBytes(const unsigned char *pData)
{
  unsigned short w = pData[1];
  w <<= 8;
  w |= pData[0];
  return w;
}

static inline unsigned int INTFromBytes(const unsigned char *pData)
{
  unsigned int i = pData[3];
  i <<= 8;
  i |= pData[2];
  i <<= 8;
  i |= pData[1];
  i <<= 8;
  i |= pData[0];
  return i;
}

/* INTFromAligned: the same value as INTFromBytes(p), for a 4-byte aligned p
   only (one 32-bit load; on big-endian SH plus swap.b/swap.w/swap.b). Callers
   test the alignment at run time (see PatchColumnOfs). */
static inline unsigned int INTFromAligned(const unsigned char *p)
{
  unsigned int v = *(const uint32_t *)(const void *)p;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

/* columnofs[col] of a patch_t given as bytes (columnofs starts at byte 8):
   one aligned load when the patch is 4-byte aligned, bytewise otherwise.
   'aligned' is ((uintptr_t)patch & 3) == 0, computed once per patch. */
static inline unsigned int PatchColumnOfs(const unsigned char *patch,
                                          int aligned, int col)
{
  const unsigned char *p = patch + 8 + 4 * col;
  return aligned ? INTFromAligned(p) : INTFromBytes(p);
}

#define SHORT_NORM(x)	((short)SwapSHORT((unsigned short) (x)))
#define LONG(x)         ((long)SwapLONG((unsigned long) (x)))

#define SHORT(x) ((short)SHORTFromBytes((const unsigned char*)&(x##LE)))

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
