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
//	Fixed point arithemtics, implementation.
//
//-----------------------------------------------------------------------------


#ifndef __M_FIXED__
#define __M_FIXED__

#include <stdint.h>


#ifdef __GNUG__
#pragma interface
#endif


//
// Fixed point, 32bit as 16.16.
//
#define FRACBITS		16
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

/* CGDoom port: vanilla-exact fixed point.
   CGDoom's bit-serial FixedMul rounded toward zero and its FixedDiv dropped
   the last quotient bit, which broke vanilla demo sync; these follow
   linuxdoom-1.10 exactly (tests/fixed_exact.c: make test-fixed and
   make test-fixed-sim check them on the host and in the SH simulator).

   FixedMul: the 64-bit product shifted right arithmetically, i.e. rounded
   toward minus infinity. GCC -m4 compiles it to dmuls.l + xtrct. */
static inline fixed_t FixedMul(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * b) >> FRACBITS);
}

/* FixedDiv: vanilla's overflow guard
       if((abs(a) >> 14) >= abs(b)) return (a^b) < 0 ? MININT : MAXINT;
   with abs(MININT) == MININT as on the original compilers, then the quotient
   (a << 16) / b truncated toward zero (vanilla FixedDiv2 used doubles; the
   int64 quotient is provably identical, see m_fixed.c). The only deviation:
   for a == MININT and a quotient that does not fit (|b| < 1.0, b == -1.0 or
   b == 0), where vanilla calls I_Error("FixedDiv: divide by zero"), this
   saturates with the guard's sign rule instead of aborting. On SH the quotient is a div0u/div1 64/32
   division (m_fixed.c), otherwise a 64-bit C division. */
fixed_t FixedDiv	(fixed_t a, fixed_t b);


/* CGDoom port: exact replacements for signed C divisions on hot paths:
   signed divisions whose divisor is known to be positive go through libgcc's
   unsigned divider instead of the signed __sdivsi3_i4i. The gain is small: on
   SH4 the two cost the same (about 30 to 60 instructions per call depending
   on operand size); IntCycleFill wins by doing fewer divisions.
   tests/div_exact.c (make test-div, test-div-sim) checks both helpers
   against the C operators.

   IntDivPos(n, d) == n / d for every int n and every d > 0 (C truncates
   toward zero): with s = n >> 31 (0 or -1), (n ^ s) - s is |n| as an
   unsigned value (2^31 for INT_MIN), the unsigned quotient |n| / d is the
   truncated magnitude, and (q ^ s) - s negates it back when n < 0. The only
   quotient above INT_MAX is 2^31 (n == INT_MIN, d == 1), which converts to
   INT_MIN like the C result. */
static inline int IntDivPos(int n, int d)
{
    uint32_t s = (uint32_t)(n >> 31);
    uint32_t q = (((uint32_t)n ^ s) - s) / (uint32_t)d;
    return (int)((q ^ s) - s);
}

/* IntCycleFill(dest, base, count, speed, time) stores, for i = base ..
   base+count-1, dest[i] = base + (time / speed + i) % count: P_UpdateSpecials'
   flat/texture animation step. Requires time >= 0, speed > 0, base >= 0 and
   count > 0 (the caller checks); then every operand is non-negative, the
   signed and unsigned operators agree, time / speed is computed once, and
   the remainder of consecutive i advances by one and wraps at count:
   (t + base + k) % count == ((t + base) % count + k) % count. */
static inline void IntCycleFill(int *dest, int base, int count, int speed,
                                int time)
{
    uint32_t n = (uint32_t)count;
    uint32_t r = ((uint32_t)time / (uint32_t)speed + (uint32_t)base) % n;
    int *d = dest + base;
    for (uint32_t k = 0; k < n; k++)
    {
        d[k] = base + (int)r;
        if (++r == n)
            r = 0;
    }
}



#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
