/* FixedMul/FixedDiv exactness test (make test-fixed, make test-fixed-sim).

   Checks the engine's FixedMul (m_fixed.h) and FixedDiv (m_fixed.c; on SH a
   div0u/div1 inline-asm 64/32 division) against independent references of
   the vanilla linuxdoom-1.10 definitions:

     FixedMul(a,b)  = ((long long)a * b) >> 16      (rounds toward -infinity)
     FixedDiv(a,b)  = (abs(a)>>14 >= abs(b)) ? ((a^b)<0 ? MININT : MAXINT)
                                             : FixedDiv2(a,b)
     FixedDiv2(a,b) = (fixed_t)((double)a / (double)b * FRACUNIT),
                      I_Error if the double is outside [-2^31, 2^31)
     with abs(MININT) == MININT (two's complement, as on the original x86).

   Where vanilla would I_Error (only possible for a == MININT), the engine
   saturates with the guard's sign rule; the test checks that too.

   Inputs: every pair of a list of edge values, values around the guard
   boundary, random values of every magnitude (MININT included). The double
   reference (soft-float on SH, slow) is evaluated for every edge pair and for
   one random pair out of DOUBLE_EVERY; an int64 reference for all of them.

   usage: fixed-exact [random_pairs [double_every]]     exit status 0 = PASS */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "doomtype.h"
#include "m_fixed.h"

/* abs() with abs(MININT) == MININT, without C undefined behaviour */
static int wabs(int x)
{
    return x < 0 ? (int)(0u - (unsigned)x) : x;
}

/* FixedMul reference by 16-bit partial products (does not use the 64-bit
   multiply the engine is compiled to) */
static fixed_t ref_mul(fixed_t a, fixed_t b)
{
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    uint32_t al = ua & 0xffff, ah = ua >> 16, bl = ub & 0xffff, bh = ub >> 16;
    /* unsigned 64-bit product of the bit patterns */
    uint64_t p = (uint64_t)(al * bl) + ((uint64_t)(ah * bl) << 16)
               + ((uint64_t)(al * bh) << 16) + ((uint64_t)(ah * bh) << 32);
    /* signed correction: x = ux - 2^32 [x<0] */
    if(a < 0) p -= (uint64_t)ub << 32;
    if(b < 0) p -= (uint64_t)ua << 32;
    /* arithmetic shift right by 16 of the 64-bit two's complement value,
       keep the low 32 bits */
    return (fixed_t)(uint32_t)(p >> 16);
}

/* Vanilla FixedDiv with FixedDiv2's doubles. *err = 1 where vanilla would
   I_Error; the value returned then is the engine's documented saturation. */
static fixed_t ref_div_double(fixed_t a, fixed_t b, int *err)
{
    double c;
    *err = 0;
    if((wabs(a) >> 14) >= wabs(b))
        return (a ^ b) < 0 ? MININT : MAXINT;
    c = ((double)a) / ((double)b) * FRACUNIT;   /* b == 0 gives +-inf */
    if(c >= 2147483648.0 || c < -2147483648.0 || c != c) {
        *err = 1;
        return (a ^ b) < 0 ? MININT : MAXINT;
    }
    return (fixed_t)c;
}

/* Same with a 64-bit integer quotient */
static fixed_t ref_div_int(fixed_t a, fixed_t b, int *err)
{
    int64_t q;
    *err = 0;
    if((wabs(a) >> 14) >= wabs(b))
        return (a ^ b) < 0 ? MININT : MAXINT;
    if(b == 0) {
        *err = 1;
        return (a ^ b) < 0 ? MININT : MAXINT;
    }
    q = ((int64_t)a * 65536) / b;
    if(q > MAXINT || q < MININT) {
        *err = 1;
        return (a ^ b) < 0 ? MININT : MAXINT;
    }
    return (fixed_t)q;
}

static uint32_t rng = 2463534242u;
static uint32_t xorshift(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
/* Random value with a random magnitude (0..32 bits) and sign, so MININT and
   MAXINT show up too */
static int rv(void)
{
    uint32_t s = xorshift() % 33, v;
    if(s == 0) v = 0;
    else if(s == 32) v = xorshift();
    else v = xorshift() >> (32 - s);
    return (xorshift() & 1) ? (int)(0u - v) : (int)v;
}

static long n_mul, n_div, n_dbl, n_err, bad_mul, bad_div, bad_dbl;

static void report(const char *what, int a, int b, int got, int want)
{
    static int shown;
    if(shown++ < 20)
        printf("MISMATCH %s(%d, %d) = %d, expected %d\n", what, a, b, got,
            want);
}

static void check(int a, int b, int with_double)
{
    fixed_t m = FixedMul(a, b), rm = ref_mul(a, b);
    fixed_t d = FixedDiv(a, b), ri, rd;
    int ei, ed;

    n_mul++;
    if(m != rm) { bad_mul++; report("FixedMul", a, b, m, rm); }

    n_div++;
    ri = ref_div_int(a, b, &ei);
    if(ei) n_err++;
    if(d != ri) { bad_div++; report("FixedDiv/int64", a, b, d, ri); }

    if(with_double) {
        n_dbl++;
        rd = ref_div_double(a, b, &ed);
        if(rd != d || ed != ei) {
            bad_dbl++;
            report("FixedDiv/double", a, b, d, rd);
        }
    }
}

int main(int argc, char **argv)
{
    static const int base[] = {
        0, 1, 2, 3, 7, 0x3fff, 0x4000, 0x4001, 0x7fff, 0x8000, 0x8001,
        0xffff, 0x10000, 0x10001, 0x18000, 0x1ffff, 0x20000, 0x12345,
        0x3ffff, 0x40000, 0x7ffff, 0x800000, 0xffffff, 0x1000000,
        0x12345678, 0x3fffffff, 0x40000000, 0x40000001, 0x7ffffffe,
        0x7fffffff, 100 << 16, 65535 * 3, 0x00ff00ff,
    };
    enum { NB = sizeof base / sizeof base[0] };
    int edges[2 * NB + 1];
    int ne = 0;
    long pairs = argc > 1 ? atol(argv[1]) : 50000000;
    long dbl_every = argc > 2 ? atol(argv[2]) : 1;

    for(int i = 0; i < NB; i++) {
        edges[ne++] = base[i];
        if(base[i]) edges[ne++] = -base[i];
    }
    edges[ne++] = MININT;

    /* all edge pairs */
    for(int i = 0; i < ne; i++)
        for(int j = 0; j < ne; j++)
            check(edges[i], edges[j], 1);

    /* a == MININT against many divisors (the vanilla abs() quirk) and every
       b around 1.0 */
    for(int b = -0x30000; b <= 0x30000; b += 7)
        check(MININT, b, 1);
    for(int b = 0xff00; b <= 0x10100; b++) {
        check(MININT, b, 1);
        check(MININT, -b, 1);
    }

    /* around the overflow guard: |a| = |b| * 2^14 + delta */
    for(long k = 0; k < pairs / 50 + 1000; k++) {
        int b = (int)(xorshift() >> (17 + xorshift() % 15));  /* < 2^17 */
        int delta = (int)(xorshift() % 33) - 16;
        int a = (int)(((uint32_t)b << 14) + (uint32_t)delta);
        if(xorshift() & 1) a = -a;
        if(xorshift() & 1) b = -b;
        check(a, b, (k % dbl_every) == 0);
    }

    /* random values of every magnitude */
    for(long k = 0; k < pairs; k++) {
        int a = rv(), b = rv();
        check(a, b, (k % dbl_every) == 0);
    }

    printf("FixedMul: %ld pairs, %ld mismatches vs ((int64)a*b)>>16\n",
        n_mul, bad_mul);
    printf("FixedDiv: %ld pairs, %ld mismatches vs int64 vanilla "
        "(%ld vanilla I_Error cases, saturated)\n", n_div, bad_div, n_err);
    printf("FixedDiv: %ld pairs, %ld mismatches vs vanilla double "
        "FixedDiv2\n", n_dbl, bad_dbl);
    if(bad_mul || bad_div || bad_dbl) {
        printf("test-fixed: FAIL\n");
        return 1;
    }
    printf("test-fixed: PASS\n");
    return 0;
}
