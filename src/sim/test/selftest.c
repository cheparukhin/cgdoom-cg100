/* Self test of the sh-elf-run harness: stdio on host files, malloc, 64-bit math, libm, SH4A insns.

   usage: selftest [wad-path [tmp-dir [expected-wad-crc32-hex]]]
   Prints "SELFTEST PASS" and exits 0 when everything checks out, else lists failures and exits 1. */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fxlibc/printf.h>
#include "sim.h"

#ifndef SELFTEST_WAD
#define SELFTEST_WAD "/Users/cheparukhin/casio_doom/test-wads/fd/freedoom-0.13.0/freedoom1.wad"
#endif

static int failures, checks;

#define CHECK(cond) do { checks++; if(!(cond)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)

/* ---- small helpers ------------------------------------------------------------------------- */

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	if(!crc_table[1]) {
		for(uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for(int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
			crc_table[i] = c;
		}
	}
	crc = ~crc;
	while(n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
	return ~crc;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- 1. printf family ------------------------------------------------------------------------ */

static void test_printf(void)
{
	char b[128];
	__printf_enable_fp();      /* fxlibc: %f/%e/%g are off unless asked for (same as on the calculator) */
	snprintf(b, sizeof b, "%d|%5d|%-5d|%05d|%u|%x|%X|%o", -42, 42, 42, 42, 4000000000u, 0xbeef, 0xbeef, 8);
	CHECK(!strcmp(b, "-42|   42|42   |00042|4000000000|beef|BEEF|10"));
	snprintf(b, sizeof b, "%s|%10s|%-10s|%c|%%|%.3s", "abc", "right", "left", 'z', "truncate");
	CHECK(!strcmp(b, "abc|     right|left      |z|%|tru"));
	snprintf(b, sizeof b, "%lld|%llu|%llx|%ld", -9000000000000ll, 18000000000000000000ull, 0x123456789abcdefull, -7l);
	CHECK(!strcmp(b, "-9000000000000|18000000000000000000|123456789abcdef|-7"));
	snprintf(b, sizeof b, "%.2f|%8.3f|%e|%g", 3.14159, -2.5, 12345.678, 0.0001);
	CHECK(!strcmp(b, "3.14|  -2.500|1.234568e+04|0.0001"));
	snprintf(b, sizeof b, "%p", (void *)0x1234);
	CHECK(strstr(b, "1234") != NULL);
	CHECK(snprintf(NULL, 0, "%d%s", 12345, "xy") == 7);
	int a, c; char w[16];
	CHECK(sscanf("77 0x1f word", "%d %i %15s", &a, &c, w) == 3 && a == 77 && c == 31 && !strcmp(w, "word"));
	CHECK(strtol("  -123abc", NULL, 10) == -123 && strtol("0x7fffffff", NULL, 0) == 0x7fffffff);
	CHECK(atoi("987") == 987 && fabs(atof("2.5e3") - 2500.0) < 1e-9);
}

/* ---- 2. reading the WAD through stdio ----------------------------------------------------------- */

static void test_wad(const char *path, uint32_t expect_crc, int have_crc)
{
	FILE *f = fopen(path, "rb");
	if(!f) { printf("FAIL cannot open %s (errno %d)\n", path, errno); failures++; return; }

	uint8_t hdr[12];
	CHECK(fread(hdr, 1, 12, f) == 12);
	CHECK(!memcmp(hdr, "IWAD", 4) || !memcmp(hdr, "PWAD", 4));
	uint32_t numlumps = le32(hdr + 4), infotableofs = le32(hdr + 8);
	printf("WAD %.4s: numlumps=%u infotableofs=%u\n", hdr, (unsigned)numlumps, (unsigned)infotableofs);

	CHECK(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	printf("WAD size=%ld bytes\n", size);
	CHECK(size > 12 && infotableofs + 16ul * numlumps <= (unsigned long)size);

	/* directory: filepos(4) size(4) name(8), all little endian */
	CHECK(fseek(f, infotableofs, SEEK_SET) == 0);
	uint32_t playpal_pos = 0, playpal_size = 0, bad = 0;
	for(uint32_t i = 0; i < numlumps; i++) {
		uint8_t e[16];
		if(fread(e, 1, 16, f) != 16) { bad++; break; }
		uint32_t pos = le32(e), sz = le32(e + 4);
		char name[9]; memcpy(name, e + 8, 8); name[8] = 0;
		if((unsigned long)pos + sz > (unsigned long)size) bad++;
		if(i < 6) printf("  lump %3u: %-8s pos=%-8u size=%u\n", (unsigned)i, name, (unsigned)pos, (unsigned)sz);
		if(!strcmp(name, "PLAYPAL")) { playpal_pos = pos; playpal_size = sz; }
	}
	CHECK(bad == 0);
	printf("PLAYPAL: pos=%u size=%u\n", (unsigned)playpal_pos, (unsigned)playpal_size);
	CHECK(playpal_size == 768 * 14);

	/* random access: the first palette entry's colours are bytes, any value is fine; check that
	   fseek+fread agrees with sequential reads via a second stream */
	uint8_t p1[48], p2[48];
	CHECK(fseek(f, playpal_pos + 100, SEEK_SET) == 0 && fread(p1, 1, 48, f) == 48);
	CHECK(ftell(f) == (long)playpal_pos + 148);
	FILE *g = fopen(path, "rb");
	CHECK(g != NULL);
	if(g) {
		CHECK(fseek(g, playpal_pos, SEEK_SET) == 0);
		CHECK(fseek(g, 100, SEEK_CUR) == 0);
		CHECK(fread(p2, 1, 48, g) == 48);
		CHECK(!memcmp(p1, p2, 48));
		fclose(g);
	}

	/* stream the whole file in odd-sized chunks and CRC it */
	rewind(f);
	static uint8_t chunk[7001];
	uint32_t crc = 0; long total = 0; size_t n;
	while((n = fread(chunk, 1, sizeof chunk, f)) > 0) { crc = crc32_update(crc, chunk, n); total += n; }
	CHECK(total == size);
	CHECK(feof(f) && !ferror(f));
	printf("WAD crc32=%08x\n", (unsigned)crc);
	if(have_crc) CHECK(crc == expect_crc);
	fclose(f);

	/* unbuffered-sized big read straight into the heap (like W_ReadLump on a big lump) */
	f = fopen(path, "rb");
	size_t big = (size_t)size;
	uint8_t *all = malloc(big);
	CHECK(all != NULL);
	if(all) {
		CHECK(fread(all, 1, big, f) == big);
		/* whole-file CRC again would cost 250M instructions: compare with the streamed read instead */
		FILE *g2 = fopen(path, "rb");
		static uint8_t one[4096];
		int same = 1;
		for(size_t off = 0; g2 && off < big; off += 997 * 4096 + 13) {
			size_t want = big - off < sizeof one ? big - off : sizeof one;
			same &= fseek(g2, off, SEEK_SET) == 0 && fread(one, 1, want, g2) == want && !memcmp(one, all + off, want);
		}
		CHECK(same);
		if(g2) fclose(g2);
		free(all);
	}
	fclose(f);

	struct stat st;
	CHECK(stat(path, &st) == 0 && st.st_size == size);
	CHECK(fopen("/nonexistent/file.wad", "rb") == NULL);
}

/* ---- 3. writing, appending, updating a file -------------------------------------------------------- */

static void test_file_write(const char *dir)
{
	char path[512];
	snprintf(path, sizeof path, "%s/selftest.out", dir);

	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if(!f) return;
	static uint8_t pat[30011];
	for(size_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + (i >> 8));
	CHECK(fwrite(pat, 1, sizeof pat, f) == sizeof pat);
	CHECK(fprintf(f, "tail:%d:%s\n", 12345, "end") == 15);
	CHECK(ftell(f) == (long)sizeof pat + 15);
	CHECK(fclose(f) == 0);

	f = fopen(path, "rb");
	CHECK(f != NULL);
	static uint8_t back[sizeof pat];
	CHECK(fread(back, 1, sizeof back, f) == sizeof back && !memcmp(back, pat, sizeof pat));
	char line[64];
	CHECK(fgets(line, sizeof line, f) && !strcmp(line, "tail:12345:end\n"));
	CHECK(fgetc(f) == EOF);
	fclose(f);

	f = fopen(path, "ab");             /* append */
	CHECK(f && fputs("more\n", f) >= 0);
	if(f) fclose(f);
	f = fopen(path, "r+b");            /* update in place */
	CHECK(f != NULL);
	if(f) {
		CHECK(fseek(f, 10, SEEK_SET) == 0);
		CHECK(fputc(0xAB, f) == 0xAB);
		CHECK(fseek(f, -5, SEEK_END) == 0);
		CHECK(fgetc(f) == 'm');
		CHECK(ungetc('Q', f) == 'Q' && fgetc(f) == 'Q');
		fclose(f);
	}
	f = fopen(path, "rb");
	if(f) {
		uint8_t b[2];
		CHECK(fseek(f, 10, SEEK_SET) == 0 && fread(b, 1, 1, f) == 1 && b[0] == 0xAB);
		CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)sizeof pat + 15 + 5);
		fclose(f);
	}
	struct stat st;
	CHECK(stat(path, &st) == 0 && st.st_size == (off_t)(sizeof pat + 20));

	/* low-level descriptors */
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 3);
	char tmp[4];
	CHECK(lseek(fd, 14, SEEK_SET) == 14 && read(fd, tmp, 1) == 1 && lseek(fd, 0, SEEK_CUR) == 15);
	CHECK(close(fd) == 0);
}

/* ---- 4. allocator ---------------------------------------------------------------------------------- */

static void test_malloc(void)
{
	struct sim_heap_info h0, h1;
	sim_heap_info(&h0);               /* baseline: stdio buffers etc. are legitimately allocated */
	CHECK(sim_heap_check() == 0);
	printf("heap: arena=%u free=%u largest=%u\n", (unsigned)h0.arena, (unsigned)h0.free, (unsigned)h0.largest);
	CHECK(h0.arena > 16u * 1024 * 1024);

	/* basic properties */
	void *a = malloc(1), *b = malloc(100), *c = calloc(33, 7);
	CHECK(a && b && c);
	CHECK(((uintptr_t)a & 7) == 0 && ((uintptr_t)b & 7) == 0 && ((uintptr_t)c & 7) == 0);
	for(int i = 0; i < 33 * 7; i++) CHECK(((uint8_t *)c)[i] == 0);
	free(b); free(a); free(c);
	CHECK(sim_heap_check() == 0);
	/* absurd request fails cleanly (volatile p: GCC elides a malloc whose result is only compared with NULL) */
	{ volatile size_t huge = (size_t)-8; void *volatile p = malloc(huge); CHECK(p == NULL); }

	/* randomized stress with content verification */
	enum { N = 400 };
	struct { uint8_t *p; uint32_t size, seed; } blk[N];
	memset(blk, 0, sizeof blk);
	for(int iter = 0; iter < 12000; iter++) {
		int i = rnd() % N;
		if(blk[i].p) {
			uint8_t *p = blk[i].p; uint32_t s = blk[i].seed;
			for(uint32_t k = 0; k < blk[i].size; k += 37) if(p[k] != (uint8_t)(s + k)) { CHECK(!"heap corruption"); break; }
			if(blk[i].size && p[blk[i].size - 1] != (uint8_t)(s + blk[i].size - 1)) CHECK(!"heap corruption (tail)");
		}
		uint32_t op = rnd() % 8;
		uint32_t sz = (rnd() & 1) ? rnd() % 200 : rnd() % 6000;
		if(op < 3 || !blk[i].p) {                    /* (re)allocate */
			free(blk[i].p);
			blk[i].p = malloc(sz + 1); blk[i].size = sz + 1;
			if(!blk[i].p) { CHECK(!"malloc failed"); continue; }
			blk[i].seed = rnd();
			for(uint32_t k = 0; k < blk[i].size; k++) blk[i].p[k] = (uint8_t)(blk[i].seed + k);
		} else if(op < 6) {                          /* realloc, content must survive */
			uint32_t ns = sz + 1, keep = ns < blk[i].size ? ns : blk[i].size;
			uint8_t *q = realloc(blk[i].p, ns);
			if(!q) { CHECK(!"realloc failed"); continue; }
			for(uint32_t k = 0; k < keep; k += 13) if(q[k] != (uint8_t)(blk[i].seed + k)) { CHECK(!"realloc lost data"); break; }
			blk[i].p = q; blk[i].size = ns;
			for(uint32_t k = 0; k < ns; k++) q[k] = (uint8_t)(blk[i].seed + k);
		} else {                                     /* free */
			free(blk[i].p); blk[i].p = NULL;
		}
		if(iter % 2500 == 0) CHECK(sim_heap_check() == 0);
	}
	CHECK(sim_heap_check() == 0);
	for(int i = 0; i < N; i++) { free(blk[i].p); blk[i].p = NULL; }
	sim_heap_info(&h1);
	CHECK(sim_heap_check() == 0);
	CHECK(h1.used == h0.used);                       /* nothing leaked */
	CHECK(h1.nchunks_free == h0.nchunks_free && h1.free == h0.free);   /* fully coalesced again */
	printf("heap stress: peak=%u KiB, %u allocs, %u frees\n", (unsigned)(h1.peak >> 10), h1.nalloc, h1.nfree_calls);

	/* big blocks and aligned blocks */
	uint8_t *big = malloc(24u << 20);
	CHECK(big != NULL);
	if(big) { memset(big, 0x5a, 24u << 20); CHECK(big[(24u << 20) - 1] == 0x5a); free(big); }
	static const size_t aligns[] = { 16, 32, 64, 256, 4096, 65536 };
	for(unsigned i = 0; i < sizeof aligns / sizeof *aligns; i++) {
		void *x = memalign(aligns[i], 1000 + i), *y = malloc(50);
		void *z = NULL;
		CHECK(x && ((uintptr_t)x % aligns[i]) == 0);
		CHECK(posix_memalign(&z, aligns[i], 77) == 0 && ((uintptr_t)z % aligns[i]) == 0);
		if(x) memset(x, 0xcc, 1000 + i);
		free(y); free(x); free(z);
		CHECK(sim_heap_check() == 0);
	}
	sim_heap_info(&h1);
	CHECK(h1.used == h0.used && h1.nchunks_free == h0.nchunks_free);

	/* realloc corner cases */
	char *s = realloc(NULL, 10);
	CHECK(s != NULL);
	strcpy(s, "123456789");
	s = realloc(s, 5000); CHECK(s && !strcmp(s, "123456789"));
	s = realloc(s, 12);   CHECK(s && !strcmp(s, "123456789"));
	CHECK(realloc(s, 0) == NULL);
	sim_heap_info(&h1);
	CHECK(h1.used == h0.used && sim_heap_check() == 0);
}

/* ---- 5. 64-bit arithmetic -------------------------------------------------------------------------- */

static int64_t fixmul_ref(int32_t a, int32_t b) { return ((int64_t)a * b) >> 16; }

static void test_int64(void)
{
	volatile int64_t x = 0x123456789abcdef0ll, y = -0x0fedcba987654321ll;
	CHECK(x + y == 0x02468acf13579bcfll);
	CHECK(x * 3 == 0x369d0369d0369cd0ll);
	CHECK(x * y == (int64_t)0xddc927701a9e7310ull);
	CHECK((x >> 12) == 0x123456789abcdll && (y >> 12) == -280223976814165ll);
	CHECK(((uint64_t)x << 8) == 0x3456789abcdef000ull);
	CHECK(x / 1000003 == 1311764532170ll && x % 1000003 == 193810);
	CHECK(x / -17 == -77162851027281783ll && y % 65537 == -30584);
	CHECK((uint64_t)(-y) / 10u == 114779740903081654ull && (uint64_t)y / 10u == 1729894666467873507ull);
	CHECK((uint64_t)y % 1000u == 71);
	CHECK(0xffffffffull * 0xffffffffull == 0xfffffffe00000001ull);
	volatile uint32_t u1 = 0xdeadbeefu, u2 = 0xcafebabeu;
	CHECK((uint64_t)u1 * u2 == 0xb092ab7b88cf5b62ull);
	volatile int32_t s1 = -123456789, s2 = 987654321;
	CHECK((int64_t)s1 * s2 == -121932631112635269ll);

	/* FixedMul/FixedDiv style identities used by DOOM */
	uint32_t seed = 99;
	for(int i = 0; i < 2000; i++) {
		seed = seed * 1664525u + 1013904223u;
		int32_t a = (int32_t)seed;
		seed = seed * 1664525u + 1013904223u;
		int32_t b = (int32_t)seed >> (i & 15);
		int64_t p = (int64_t)a * b;
		CHECK(fixmul_ref(a, b) == p / 65536 - ((p < 0 && (p & 0xffff)) ? 1 : 0));
		if(b != 0) {
			int64_t q = ((int64_t)a << 16) / b;
			CHECK(q * b + (((int64_t)a << 16) % b) == ((int64_t)a << 16));
		}
	}
	CHECK(__builtin_clz(0x00ff0000u) == 8 && __builtin_ctz(0x00ff0000u) == 16 && __builtin_popcount(0xf0f0u) == 8);
	CHECK(-7 / 2 == -3 && -7 % 2 == -1 && (-7 >> 1) == -4 && 7u / 3u == 2u);
}

/* ---- 6. libc / libm odds and ends ------------------------------------------------------------------- */

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_libc(void)
{
	enum { N = 1000 };
	static int v[N];
	for(int i = 0; i < N; i++) v[i] = (int)(rnd() % 100000);
	qsort(v, N, sizeof v[0], cmp_int);
	int sorted = 1;
	for(int i = 1; i < N; i++) sorted &= v[i - 1] <= v[i];
	CHECK(sorted);
	int key = v[345];
	int *r = bsearch(&key, v, N, sizeof v[0], cmp_int);
	CHECK(r && *r == key);

	CHECK(strcasecmp("Freedoom", "FREEDOOM") == 0 && strcasecmp("abc", "abd") < 0);
	CHECK(strncasecmp("PLAYPAL!", "playpal?", 7) == 0);
	CHECK(toupper('q') == 'Q' && tolower('Q') == 'q' && isdigit('7') && isspace('\t') && !isalpha('1'));

	char buf[64];
	memset(buf, 'x', sizeof buf);
	memcpy(buf + 1, "hello world", 12);
	memmove(buf + 3, buf + 1, 12);
	CHECK(!strcmp(buf + 3, "hello world"));
	CHECK(strlen(buf + 3) == 11 && strchr(buf + 3, 'w') && strstr(buf + 3, "o w") && strrchr(buf + 3, 'o') == strchr(buf + 3, 'w') + 1);
	CHECK(memcmp("abc", "abd", 3) < 0 && memchr("abcdef", 'd', 6) != NULL);
	/* memcpy over sizes/alignments straddling the 64-byte fast path */
	static uint8_t src[300], dst[300];
	for(int i = 0; i < 300; i++) src[i] = (uint8_t)(i * 3 + 1);
	int ok = 1;
	for(int so = 0; so < 4; so++) for(int d0 = 0; d0 < 4; d0++) for(int len = 0; len < 130; len += 7) {
		memset(dst, 0xee, sizeof dst);
		memcpy(dst + d0, src + so, len);
		for(int i = 0; i < len; i++) ok &= dst[d0 + i] == src[so + i];
		ok &= dst[d0 + len] == 0xee && (d0 == 0 || dst[d0 - 1] == 0xee);
	}
	CHECK(ok);

	CHECK(fabs(sqrt(2.0) - 1.41421356237) < 1e-9);
	CHECK(fabs(sin(1.0) - 0.8414709848) < 1e-9 && fabs(cos(1.0) - 0.5403023059) < 1e-9);
	CHECK(fabs(atan2(1.0, 1.0) - 0.78539816339) < 1e-9 && fabs(pow(2.0, 10.0) - 1024.0) < 1e-9);
	CHECK(fabs(exp(1.0) - 2.718281828) < 1e-8 && fabs(log(10.0) - 2.302585093) < 1e-8);
	CHECK((int)floor(-1.5) == -2 && (int)ceil(-1.5) == -1);
	float fv = 1.5f; fv = fv * fv + 0.25f;
	CHECK(fv == 2.5f);
	double dd = 1.0; for(int i = 0; i < 10; i++) dd *= 1.5;
	CHECK(fabs(dd - 57.6650390625) < 1e-9);

	CHECK(time(NULL) > 1700000000);
	clock_t c = clock(); (void)c;
	CHECK(getenv("NOPE") == NULL);
}

/* ---- 7. SH4A / SH4AL-DSP instructions (test/insns.s) ------------------------------------------------- */

extern uint32_t t_movua(const void *p);
extern uint32_t t_llsc(volatile uint32_t *p);
extern void t_cacheops(void *line);
extern uint32_t t_dsp_loop(uint32_t count);
extern uint32_t t_dsp_alu(void);

static void test_insns(void)
{
	static uint8_t bytes[16] __attribute__((aligned(4))) = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	CHECK(t_movua(bytes + 1) == 0x02030405u);
	CHECK(t_movua(bytes + 6) == 0x0708090au);

	static volatile uint32_t cell = 41;
	CHECK(t_llsc(&cell) == 42 && cell == 42);

	static char line[64] __attribute__((aligned(32)));
	t_cacheops(line);

	CHECK(t_dsp_loop(10) == 10);
	CHECK(t_dsp_loop(1000) == 1000);
	CHECK(t_dsp_alu() == 0x00070000u);

	/* dmuls.l / dmulu.l / xtrct / swap.w / shld through inline asm (plain SH4) */
	uint32_t hi, lo;
	__asm__ volatile ("dmulu.l %2, %3\n\tsts mach, %0\n\tsts macl, %1" : "=r"(hi), "=r"(lo) : "r"(0xdeadbeefu), "r"(0xcafebabeu) : "mach", "macl");
	CHECK((((uint64_t)hi << 32) | lo) == 0xb092ab7b88cf5b62ull);
	__asm__ volatile ("dmuls.l %2, %3\n\tsts mach, %0\n\tsts macl, %1" : "=r"(hi), "=r"(lo) : "r"(-100000), "r"(300000) : "mach", "macl");
	CHECK((int64_t)(((uint64_t)hi << 32) | lo) == -30000000000ll);
	uint32_t x = 0x11223344u, y = 0x55667788u, t;
	__asm__ volatile ("xtrct %1, %0" : "+r"(y) : "r"(x));
	CHECK(y == 0x33445566u);
	__asm__ volatile ("swap.w %1, %0" : "=r"(t) : "r"(x));
	CHECK(t == 0x33441122u);
	int sh = -4;
	__asm__ volatile ("shld %1, %0" : "+r"(x) : "r"(sh));
	CHECK(x == 0x01122334u);
	static volatile uint8_t lock;
	uint32_t was;
	__asm__ volatile ("tas.b @%1\n\tmovt %0" : "=r"(was) : "r"(&lock) : "t", "memory");
	CHECK(was == 1 && lock == 0x80);
}

/* ---- 8. simulator extension (only with build-sim/tools/sh-elf-run64) ------------------------------------ */

static void test_counters(void)
{
	struct sim_counters a, b;
	if(sim_counters(&a) != 0) {
		printf("sim64 extension: not available (stock sh-elf-run: 32-bit counters, no in-program counters)\n");
		return;
	}
	volatile int x = 0;
	for(int i = 0; i < 1000; i++) x += i;
	sim_counters(&b);
	(void)x;
	printf("sim64 extension: available; 1000-iteration loop = %llu insts, %llu cycles\n",
		(unsigned long long)(b.insts - a.insts), (unsigned long long)(b.cycles - a.cycles));
	CHECK(b.insts - a.insts >= 5000 && b.insts - a.insts < 20000);
	CHECK(b.cycles >= b.insts && a.cycles >= a.insts);       /* cycles = insts + branch penalties */
	CHECK(sim_pcprof(1) == 0);
	{ uint32_t m0 = sim_micros(); volatile int y = 0; for(int i = 0; i < 30000; i++) y += i; (void)y; CHECK(sim_micros() - m0 >= 3); }
}

/* ---- main -------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	printf("selftest: argc=%d\n", argc);
	for(int i = 0; i < argc; i++) printf("  argv[%d]=\"%s\"\n", i, argv[i]);
	const char *wad = argc > 1 ? argv[1] : SELFTEST_WAD;
	const char *dir = argc > 2 ? argv[2] : ".";
	uint32_t crc = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0;

	test_printf();
	test_wad(wad, crc, argc > 3 && strlen(argv[3]) == 8);
	test_file_write(dir);
	test_malloc();
	test_int64();
	test_libc();
	test_insns();
	test_counters();

	fprintf(stderr, "(stderr works)\n");
	printf("%d checks, %d failures\n", checks, failures);
	if(failures) { printf("SELFTEST FAIL\n"); return 1; }
	printf("SELFTEST PASS\n");
	return 0;
}
