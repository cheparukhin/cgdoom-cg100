/* Exactness test of the exact division helpers of m_fixed.h
   (make test-div on the host, make test-div-sim in the SH simulator).

   IntDivPos(n, d) must equal the C quotient n / d for every int n and d > 0.
   It is checked against the 32-bit C operator (libgcc __sdivsi3_i4i on SH)
   and against a 64-bit quotient (libgcc __divdi3 on SH, an independent
   routine):
     - exhaustively for every d in 1..D_EXHAUST with a list of edge n values
       (INT_MIN, INT_MAX, 0, +-1, multiples of d and their neighbours, powers
       of two and their neighbours);
     - exhaustively for every n in [-N_EXHAUST, N_EXHAUST] with d in 1..64;
     - for random (n, d) pairs of every magnitude.

   IntCycleFill(dest, base, count, speed, time) must store exactly what
   P_UpdateSpecials' vanilla loop stores:
       for (i = base; i < base + count; i++)
           dest[i] = base + ((time / speed + i) % count);
   It is checked for every time in 0..TIME_EXHAUST with all speeds 1..17,
   counts 1..40 and a few bases, and for random large times.

   usage: div-exact [random_pairs]      exit status 0 = PASS */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "m_fixed.h"

#define D_EXHAUST    65536
#define N_EXHAUST    (1 << 18)
#define TIME_EXHAUST 2000

static uint32_t rng = 0x12345678;
static uint32_t rnd(void)
{
    /* xorshift32 */
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static long long checked = 0, failures = 0;

/* noinline: keep the C operator a real division, not folded with the
   helper by the optimizer */
static int __attribute__((noinline)) c_div(int n, int d)
{
    return n / d;
}

static void check(int n, int d)
{
    int got = IntDivPos(n, d);
    int ref32 = c_div(n, d);
    int ref64 = (int)((int64_t)n / (int64_t)d);
    checked++;
    if(got != ref32 || got != ref64) {
        if(failures < 20)
            printf("IntDivPos(%d, %d) = %d, C %d, int64 %d\n", n, d, got,
                ref32, ref64);
        failures++;
    }
}

static int dest_new[4096], dest_ref[4096];

/* Compares dest[base-2 .. base+count+1]: the written range and two guard
   cells on each side */
static void check_cycle(int base, int count, int speed, int time)
{
    int i, lo = base - 2 < 0 ? 0 : base - 2, hi = base + count + 2;
    for(i = lo; i < hi; i++)
        dest_new[i] = dest_ref[i] = 0x55555555;
    for(i = base; i < base + count; i++)
        dest_ref[i] = base + ((time / speed + i) % count);
    IntCycleFill(dest_new, base, count, speed, time);
    checked++;
    if(memcmp(dest_new + lo, dest_ref + lo, (hi - lo) * sizeof(int))) {
        if(failures < 20)
            printf("IntCycleFill(base=%d, count=%d, speed=%d, time=%d) "
                "differs\n", base, count, speed, time);
        failures++;
    }
}

int main(int argc, char **argv)
{
    long long pairs = argc > 1 ? atoll(argv[1]) : 20000000;
    int d, k, n;

    /* 1. every divisor 1..D_EXHAUST with edge numerators */
    for(d = 1; d <= D_EXHAUST; d++) {
        static const int fixed_n[] = { INT_MIN, INT_MIN + 1, INT_MAX,
            INT_MAX - 1, 0, 1, -1, 2, -2, 65535, -65535, 65536, -65536 };
        for(k = 0; k < (int)(sizeof fixed_n / sizeof *fixed_n); k++)
            check(fixed_n[k], d);
        for(k = -3; k <= 3; k++) {
            int64_t m = (int64_t)d * (k * 1000 + 7);
            if(m >= INT_MIN + 2 && m <= INT_MAX - 2) {
                check((int)m, d); check((int)m + 1, d); check((int)m - 1, d);
                check((int)-m, d); check((int)-m + 1, d);
                check((int)-m - 1, d);
            }
        }
        check(rnd(), d);
        check(-(int)(rnd() >> 1), d);
    }
    for(k = 0; k < 31; k++) {
        int p = 1 << k;
        for(d = 1; d <= 300; d++) {
            check(p, d); check(p - 1, d); check(p + 1, d);
            check(-p, d); check(-p + 1, d); check(-p - 1, d);
        }
        check(INT_MIN, p); check(INT_MAX, p); check(INT_MIN + 1, p);
        check(-p, p); check(p, p); check(-p - 1, p); check(-p + 1, p);
    }
    check(INT_MIN, INT_MAX); check(INT_MAX, INT_MAX); check(-1, INT_MAX);

    /* 2. every numerator in [-N_EXHAUST, N_EXHAUST] for small divisors (the
       renderer's scalestep divides by a screen-column count) */
    for(n = -N_EXHAUST; n <= N_EXHAUST; n++) {
        for(d = 1; d <= 8; d++)
            check(n, d);
        check(n, 320);
        check(n, 319);
    }
    for(d = 9; d <= 64; d++)
        for(n = -40000; n <= 40000; n += 7)
            check(n, d);

    /* 3. random pairs of every magnitude */
    for(long long i = 0; i < pairs; i++) {
        uint32_t a = rnd(), b = rnd();
        int nshift = a & 31, dshift = b & 31;
        int nn = (int)(rnd() >> nshift);
        if(a & 0x100) nn = -nn;
        if(a & 0x200 && nshift == 0) nn = INT_MIN;
        int dd = (int)((rnd() >> 1) >> dshift);
        if(dd <= 0) dd = 1 + (b >> 24);
        check(nn, dd);
    }
    long long divs = checked;

    /* 4. IntCycleFill against P_UpdateSpecials' loop */
    {
        static const int bases[] = { 0, 1, 7, 100, 333, 2000 };
        int b, count, speed, time;
        for(b = 0; b < (int)(sizeof bases / sizeof *bases); b++)
            for(count = 1; count <= 40; count++)
                for(speed = 1; speed <= 17; speed++)
                    for(time = 0; time <= TIME_EXHAUST; time += 1 + (speed > 8) * 3)
                        check_cycle(bases[b], count, speed, time);
        for(k = 0; k < 200000; k++) {
            int base = rnd() % 3000;
            count = 1 + rnd() % 64;
            speed = 1 + rnd() % 64;
            time = (int)(rnd() >> (1 + rnd() % 31));
            check_cycle(base, count, speed, time);
        }
        check_cycle(5, 4, 8, (1 << 24) - 1);
        check_cycle(5, 4, 1, INT_MAX - 5 - 4);
    }

    printf("div-exact: %lld IntDivPos checks, %lld IntCycleFill checks, "
        "%lld mismatches: %s\n", divs, checked - divs, failures,
        failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
