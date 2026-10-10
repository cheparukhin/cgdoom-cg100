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
//	Fixed point implementation.
//
//-----------------------------------------------------------------------------


//static const char 

#include "os.h"

#include "doomtype.h"
#include "i_system.h"

#ifdef __GNUG__
#pragma implementation "m_fixed.h"
#endif
#include "m_fixed.h"


/* FixedMul is static inline in m_fixed.h.

   FixedDiv (vanilla-exact; see m_fixed.h for the contract).

   Why 64-bit integer division equals vanilla's FixedDiv2, which computed
   c = (double)a / (double)b * FRACUNIT and truncated it: past the guard the
   exact quotient is q = a * 2^16 / b with |q| < 2^31. If q is not an integer,
   its distance to the nearest integer is at least 1/|b|. The double division
   has a relative error of at most 2^-53, i.e. an absolute error of at most
   |q| * 2^-53 = |a| * 2^-37 / |b| < 2^-6 / |b|, so it can never reach or
   cross an integer and the truncation is the same (the * 2^16 is exact).
   tests/fixed_exact.c also checks this against a double reference. */

/* abs() as the original compilers computed it: abs(MININT) == MININT
   (two's complement wrap-around, without the C undefined behaviour). */
static inline int vanilla_abs(int x)
{
  return x < 0 ? (int)(0u - (unsigned int)x) : x;
}

#ifdef __sh__
/* (hi:lo) / d for hi < d (so the quotient fits in 32 bits): unsigned 64/32
   restoring division, div0u then 32 x (rotcl; div1) and a final rotcl, as in
   the SH-4 programming manual. 66 single-cycle EX instructions. Clobbers T
   (and the M/Q bits, which GCC never keeps live across statements). */
static inline uint32_t udiv64_32(uint32_t hi, uint32_t lo, uint32_t d)
{
  __asm__("div0u\n\t"
          ".rept 32\n\t"
          "rotcl %1\n\t"
          "div1 %2,%0\n\t"
          ".endr\n\t"
          "rotcl %1"
          : "+r"(hi), "+r"(lo) : "r"(d) : "t");
  return lo;
}
#endif

/* a == MININT, b != MININT: vanilla's guard computes abs(MININT) >> 14 < 0
   and never fires, and FixedDiv2 then either succeeds (|b| > 1.0 or
   b == 1.0) or calls I_Error. Exact for the former; saturates for the latter
   (b == 0 included). Never seen in practice: kept out of line. */
static fixed_t FixedDiv_MinA(fixed_t b) __attribute__((noinline));
static fixed_t FixedDiv_MinA(fixed_t b)
{
  int64_t q;
  /* saturate with the guard's sign rule: (MININT ^ b) < 0 iff b >= 0 */
  if(b == 0)
    return MININT;
  q = ((int64_t)MININT * FRACUNIT) / b;
  if(q > MAXINT || q < MININT)
    return (b < 0) ? MAXINT : MININT;
  return (fixed_t)q;
}

fixed_t FixedDiv(fixed_t a, fixed_t b)
{
  if((vanilla_abs(a) >> 14) >= vanilla_abs(b))
    return (a ^ b) < 0 ? MININT : MAXINT;
  if(a == MININT)
    return FixedDiv_MinA(b);

  /* Here a != MININT, so 0 <= |a| < 2^31; b != MININT (the guard always
     fires for it) and b != 0, so 0 < |b| < 2^31; and the guard failed, so
     |a| <= |b| * 2^14 - 1 and the quotient magnitude is below 2^30. */
#ifdef __sh__
  {
    /* branch-free magnitudes, so that GCC keeps a single copy of the asm */
    uint32_t sa = (uint32_t)(a >> 31), sb = (uint32_t)(b >> 31);
    uint32_t ua = ((uint32_t)a ^ sa) - sa, ub = ((uint32_t)b ^ sb) - sb;
    /* ua >> 16 < ub follows from (ua >> 14) < ub */
    uint32_t q = udiv64_32(ua >> 16, ua << 16, ub);
    uint32_t s = sa ^ sb;  /* 0 or ~0: negate the quotient if signs differ */
    return (fixed_t)((q ^ s) - s);
  }
#else
  return (fixed_t)(((int64_t)a * FRACUNIT) / b);
#endif
}
