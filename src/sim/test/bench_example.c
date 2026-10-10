/* Micro-benchmark example for the sh-elf-run harness.

   Each benchmark function runs its kernel `n` times; the program is started twice with different n
   and the DIFFERENCE of the simulator statistics divided by the difference of n gives the exact cost
   of one kernel call, with start-up, libc init and exit cancelling out (see src/sim/simbench).

   Two ways to measure:
     1. any simulator:   src/sim/simbench bench_example.elf <name> [N1 N2]   (difference of two runs)
     2. sim64 simulator: "bench_example <name> <n> -i" prints the in-program numbers itself, taken
        with sim_counters() around the loop, minus an identical loop calling an empty function.

   usage: bench_example <name> <n> [-i]   (names: see table below; "list" prints them)

   To benchmark your own routine: copy this file, add an entry to `benchmarks[]` and keep the kernel
   a real function call (noinline) so the compiler cannot remove it. The kernel's result is
   accumulated into `sink` so it is not optimised away. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim.h"

volatile uint32_t sink;

/* ---- DOOM column drawer, vanilla C (R_DrawColumn inner loop: 'count' pixels) ---------------- */
static uint8_t fb[320 * 200] __attribute__((aligned(32)));
static uint8_t tex[128] __attribute__((aligned(32)));
static uint8_t cmap[256] __attribute__((aligned(32)));

#define COUNT 100        /* pixels per call */

__attribute__((noinline)) static void col_c(void)
{
	uint8_t *dest = fb + 17;
	int frac = 0x12340000, fracstep = 0x00005800, count = COUNT;
	do {
		*dest = cmap[tex[(frac >> 16) & 127]];
		dest += 320;
		frac += fracstep;
	} while(--count);
}

/* same, 4x unrolled with the frac masking folded in */
__attribute__((noinline)) static void col_unrolled(void)
{
	uint8_t *dest = fb + 17;
	uint32_t frac = 0x12340000, step = 0x00005800;
	int count = COUNT / 4;
	do {
		uint8_t a = cmap[tex[(frac >> 16) & 127]]; frac += step;
		uint8_t b = cmap[tex[(frac >> 16) & 127]]; frac += step;
		uint8_t c = cmap[tex[(frac >> 16) & 127]]; frac += step;
		uint8_t d = cmap[tex[(frac >> 16) & 127]]; frac += step;
		dest[0] = a; dest[320] = b; dest[640] = c; dest[960] = d;
		dest += 1280;
	} while(--count);
}

static uint8_t bufa[512] __attribute__((aligned(32))), bufb[512] __attribute__((aligned(32)));
__attribute__((noinline)) static void memcpy256(void) { memcpy(bufb, bufa, 256); }
__attribute__((noinline)) static void memcpy255_unaligned(void) { memcpy(bufb + 1, bufa + 2, 255); }

__attribute__((noinline)) static void mul64(void)
{
	volatile int32_t a = -123456789, b = 987654321;
	sink += (uint32_t)(((int64_t)a * b) >> 16);
}

__attribute__((noinline)) static void div32(void)
{
	volatile uint32_t a = 0xfffffff1u, b = 0x12345u;
	sink += a / b;
}

__attribute__((noinline)) static void empty_fn(void) { __asm__ volatile(""); }

static const struct { const char *name; void (*fn)(void); } benchmarks[] = {
	{ "col_c",              col_c },
	{ "col_unrolled",       col_unrolled },
	{ "memcpy256",          memcpy256 },
	{ "memcpy255_unaligned", memcpy255_unaligned },
	{ "mul64",              mul64 },
	{ "div32",              div32 },
	{ "empty",              NULL },
};

int main(int argc, char **argv)
{
	if(argc < 2 || !strcmp(argv[1], "list")) {
		for(unsigned i = 0; i < sizeof benchmarks / sizeof *benchmarks; i++) puts(benchmarks[i].name);
		return 0;
	}
	int n = argc > 2 ? atoi(argv[2]) : 100;
	for(unsigned i = 0; i < sizeof tex; i++) tex[i] = (uint8_t)(i * 5 + 3);
	for(unsigned i = 0; i < sizeof cmap; i++) cmap[i] = (uint8_t)(i ^ 0x5a);
	for(unsigned i = 0; i < sizeof bufa; i++) bufa[i] = (uint8_t)i;
	for(unsigned i = 0; i < sizeof benchmarks / sizeof *benchmarks; i++) {
		if(strcmp(argv[1], benchmarks[i].name)) continue;
		void (*fn)(void) = benchmarks[i].fn ? benchmarks[i].fn : empty_fn;
		if(argc <= 3 || strcmp(argv[3], "-i")) {          /* plain run, for simbench */
			for(int k = 0; k < n; k++) fn();
			return 0;
		}
		struct sim_counters c0, c1, c2;
		int have = sim_counters(&c0) == 0;
		for(int k = 0; k < n; k++) empty_fn();          /* baseline: loop + call overhead */
		sim_counters(&c1);
		for(int k = 0; k < n; k++) fn();
		sim_counters(&c2);
		if(!have) fprintf(stderr, "no in-program counters: run `make sim-tool` and use simrun\n");
		else {
			int64_t di = (int64_t)(c2.insts - c1.insts) - (int64_t)(c1.insts - c0.insts);
			int64_t dc = (int64_t)(c2.cycles - c1.cycles) - (int64_t)(c1.cycles - c0.cycles);
			if(di < 0) di = 0;
			if(dc < 0) dc = 0;
			printf("%s: %u calls, per call (loop overhead subtracted): %u.%02u insts, %u.%02u cycles\n",
				benchmarks[i].name, (unsigned)n, (unsigned)(di / n), (unsigned)(di * 100 / n % 100),
				(unsigned)(dc / n), (unsigned)(dc * 100 / n % 100));
		}
		return 0;
	}
	fprintf(stderr, "unknown benchmark '%s'\n", argv[1]);
	return 2;
}
