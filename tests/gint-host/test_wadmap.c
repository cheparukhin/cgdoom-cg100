/* Host test for wad.c block-map mode (GWAD_MAP) with adversarial
   Bfile_GetBlockAddress layouts. wad.c is #included
   so that memcpy/memcmp on fake P1 flash addresses (0x80000000..0x82000000)
   are redirected to a host buffer that simulates the flash (filled with
   0xEE outside the file's clusters).

   Also checks that a range ending at EOF is never mapped in
   place (the renderer may read W_LUMP_PAD bytes past it, which in flash
   are not the zeros of the engine's heap copy and of the RAM image). */
#include "cgd_gint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static uint8_t *g_flash;   /* 32 MiB simulated physical flash */
static void const *tr(void const *p)
{
	uintptr_t a = (uintptr_t)p;
	if(a >= 0x80000000u && a < 0x82000000u)
		return g_flash + (a - 0x80000000u);
	if(a >= 0xa0000000u && a < 0xa2000000u) {
		fprintf(stderr, "P2 dereference %lx\n", (unsigned long)a);
		abort();
	}
	if(a < 0x80000000u && a > 0x10000u && a < 0x10000000u) {
		fprintf(stderr, "P0 dereference %lx\n", (unsigned long)a);
		abort();
	}
	return p;
}
static void *t_memcpy(void *d, void const *s, size_t n) { return memcpy((void *)tr(d), tr(s), n); }
static int t_memcmp(void const *a, void const *b, size_t n) { return memcmp(tr(a), tr(b), n); }
#undef memcpy
#undef memcmp
#define memcpy t_memcpy
#define memcmp t_memcmp
#include "wad.c"
#undef memcpy
#undef memcmp

char g_os_version[11] = "02.00.0000";
gos_api_t g_os;
uint32_t g_entry_r15;
gtlb_stats_t gtlb_stats;
void gvid_release(void) {}
void gvid_note_os_activity(void) {}
void gtlb_repair(void) {}
void ui_status(char const *a, char const *b, char const *c) { (void)a; (void)b; (void)c; }
size_t gmem_total_free(void) { return 64u << 20; }
size_t gmem_largest_free(void) { return 64u << 20; }
void PL_Log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  [log] "); vprintf(fmt, a); va_end(a); }
/* main.c's load-phase quit poll; may longjmp (taken request) */
#include <setjmp.h>
static long load_polls = 0;
static jmp_buf *poll_jmp = NULL;
void gquit_poll_load(void)
{
	load_polls++;
	if(poll_jmp)
		longjmp(*poll_jmp, 1);
}

/* one simulated file */
static uint8_t *g_file; static int g_fsize; static int os_calls;
static int s_open(uint16_t const *p, int m, int z) { (void)p; (void)m; (void)z; os_calls++; return 3; }
static int s_close(int fd) { (void)fd; os_calls++; return 0; }
static int s_size(int fd) { (void)fd; os_calls++; return g_fsize; }
static int s_read(int fd, void *buf, int size, int whence)
{
	(void)fd; os_calls++;
	int n = size; if(whence + n > g_fsize) n = g_fsize - whence; if(n < 0) n = 0;
	memcpy(buf, g_file + whence, n); return n;
}
static int s_write(int fd, void const *b, int s) { (void)fd; (void)b; (void)s; return -1; }
static int s_create(uint16_t const *p, int t, int *s) { (void)p; (void)t; (void)s; return -1; }
static int s_remove(uint16_t const *p) { (void)p; return -1; }

/* Layout per block: raw pointer value returned by GetBlockAddress */
static uintptr_t *g_raw; static int g_nblk; static int g_block_rc_fail = -1;
static int s_block(int fd, int offset, void **ptr)
{
	(void)fd;
	int b = offset >> 12;
	if(offset & 4095) { fprintf(stderr, "unaligned block query\n"); abort(); }
	if(b >= g_nblk) { *ptr = NULL; return -1; }
	if(b == g_block_rc_fail) { *ptr = (void *)0x80001000; return -5; }
	*ptr = (void *)g_raw[b];
	return 0;
}

static int fails;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

enum { K_P2, K_P1, K_P0, K_NULL, K_UNAL };

/* Build a file of fsize bytes (WAD header) and a flash layout given by
   kinds[] and phys slot numbers (4 KiB each) */
static void setup(int fsize, int const *kind, int const *slot)
{
	free(g_file); free(g_raw);
	g_fsize = fsize;
	g_file = malloc(fsize);
	for(int i = 0; i < fsize; i++) g_file[i] = (uint8_t)(i * 7 + (i >> 12) * 13 + 1);
	memcpy(g_file, "IWAD", 4);
	/* An empty directory at offset 12 (PL_WadOpen rejects a directory
	   outside the file) */
	memset(g_file + 4, 0, 8);
	g_file[8] = 12;
	g_nblk = (fsize + 4095) >> 12;
	g_raw = calloc(g_nblk, sizeof *g_raw);
	memset(g_flash, 0xEE, 32 << 20);
	for(int b = 0; b < g_nblk; b++) {
		uint32_t phys = 0x00C80000u + slot[b] * 4096u;
		int n = fsize - b * 4096; if(n > 4096) n = 4096;
		switch(kind[b]) {
		case K_P2: g_raw[b] = 0xa0000000u + phys; memcpy(g_flash + phys, g_file + b*4096, n); break;
		case K_P1: g_raw[b] = 0x80000000u + phys; memcpy(g_flash + phys, g_file + b*4096, n); break;
		case K_P0: g_raw[b] = 0x00112900u + b * 0x1000; break;
		case K_NULL: g_raw[b] = 0; break;
		case K_UNAL: g_raw[b] = 0xa0000002u + phys; break;
		}
	}
}

static void check_all(char const *tag)
{
	gwad_ram_allowed = false;
	int rc = PL_WadOpen("TEST.WAD");
	CHECK(rc == g_fsize, "%s open %d", tag, rc);
	CHECK(gwad_stats.mode == GWAD_MAP, "%s mode %d", tag, gwad_stats.mode);
	uint8_t *buf = malloc(g_fsize + 64);
	/* every (offset, size) on and around block boundaries */
	int edges[] = { 0, 1, 2, 3, 4092, 4093, 4094, 4095, 4096, 4097, 8191, 8192, 8193 };
	int ne = sizeof edges / sizeof *edges;
	for(int b = 0; b <= g_nblk; b++)
	for(int e = 0; e < ne; e++) {
		int off = b * 4096 + edges[e] - 4096;
		if(off < 0 || off >= g_fsize) continue;
		int lens[] = { 1, 2, 3, 4, 136, 4095, 4096, 4097, 8192, 12289, 1 << 20 };
		for(unsigned l = 0; l < sizeof lens / sizeof *lens; l++) {
			int len = lens[l]; if(len > g_fsize - off) len = g_fsize - off;
			memset(buf, 0x55, len);
			int got = PL_WadRead(buf, off, len);
			CHECK(got == len, "%s read %d+%d got %d", tag, off, len, got);
			if(got == len) CHECK(!memcmp(buf, g_file + off, len), "%s read content %d+%d", tag, off, len);
			uint8_t const *m = PL_WadMap(off, len);
			if(m) {
				CHECK(!memcmp(tr(m), g_file + off, len), "%s map content %d+%d", tag, off, len);
				CHECK(PL_IsWadPointer(m), "%s map ptr not WAD", tag);
				CHECK(PL_IsWadPointer(m + len - 1), "%s map end not WAD", tag);
			}
		}
	}
	/* past end */
	CHECK(PL_WadRead(buf, g_fsize, 10) == 0, "%s read at EOF", tag);
	CHECK(PL_WadRead(buf, g_fsize - 3, 10) == 3, "%s clamp", tag);
	CHECK(PL_WadMap(g_fsize - 3, 10) == NULL, "%s map past end", tag);
	CHECK(PL_WadMap(g_fsize, 1) == NULL, "%s map at EOF", tag);
	CHECK(!PL_IsWadPointer(buf), "%s heap ptr", tag);
	printf("%s: blocks %d mapped %d runs %d longest %d p0 %d p2 %d p1 %d err %d unal %d fallback %d\n",
		tag, gwad_stats.blocks, gwad_stats.mapped, gwad_stats.runs, gwad_stats.longest_run,
		gwad_stats.raw_p0, gwad_stats.raw_p2, gwad_stats.raw_p1, gwad_stats.raw_error,
		gwad_stats.rej_unaligned, gwad_stats.fallback_reads);
	free(buf);
	PL_WadClose();
}

int main(void)
{
	g_flash = malloc(32 << 20);
	g_os = (gos_api_t){ s_open, s_close, s_size, s_read, s_write, s_create, s_remove, NULL, NULL, NULL, s_block };
	enum { N = 12 };
	int kind[64], slot[64];

	/* 1. contiguous P2, size not a multiple of 4096 */
	for(int i = 0; i < N; i++) { kind[i] = K_P2; slot[i] = i; }
	setup(N * 4096 - 1000, kind, slot); check_all("contig-partial");
	/* exact multiple */
	setup(N * 4096, kind, slot); check_all("contig-exact");
	/* 2. fragmented: reversed slots, plus a 2-block run */
	for(int i = 0; i < N; i++) slot[i] = 100 - 3 * i;
	slot[5] = slot[4] + 1;
	setup(N * 4096 - 5, kind, slot); check_all("fragmented");
	/* 3. P0, NULL, unaligned blocks mid-file and at both ends */
	for(int i = 0; i < N; i++) { kind[i] = K_P1; slot[i] = i; }
	kind[0] = K_P0; kind[3] = K_NULL; kind[6] = K_UNAL; kind[N-1] = K_P0;
	setup(N * 4096 - 77, kind, slot); check_all("p0-mixed");
	/* 4. one-block file, partial */
	kind[0] = K_P2; slot[0] = 7;
	setup(100, kind, slot); check_all("tiny");
	/* 5. a block query failing with a (bogus) pointer set */
	for(int i = 0; i < N; i++) { kind[i] = K_P2; slot[i] = i; }
	g_block_rc_fail = 4;
	setup(N * 4096 - 9, kind, slot); check_all("rc-fail");
	g_block_rc_fail = -1;
	/* 6. P1/P2 mixed contiguous (same physical run through both aliases) */
	for(int i = 0; i < N; i++) { kind[i] = (i & 1) ? K_P1 : K_P2; slot[i] = 50 + i; }
	setup(N * 4096 - 1, kind, slot); check_all("p1p2-mixed");


	/* 7. fuzz: random layouts and random ranges */
	srand(3);
	for(int round = 0; round < 300; round++) {
		int nb = 1 + rand() % 40;
		int base = rand() % 200;
		for(int i = 0; i < nb; i++) {
			int r = rand() % 10;
			kind[i] = r < 6 ? K_P2 : r < 8 ? K_P1 : r == 8 ? K_P0 : K_NULL;
			slot[i] = (rand() % 3) ? base + i : 1000 + 7 * i + rand() % 5;
		}
		int fsize = (nb - 1) * 4096 + 12 + rand() % 4085;
		setup(fsize, kind, slot);
		gwad_ram_allowed = false;
		CHECK(PL_WadOpen("T.WAD") == fsize, "fuzz open");
		uint8_t *buf = malloc(fsize);
		for(int k = 0; k < 300; k++) {
			int off = rand() % fsize, len = rand() % (fsize - off + 1);
			if(k & 1) len = len % 9000;
			int got = PL_WadRead(buf, off, len);
			CHECK(got == len && !memcmp(buf, g_file + off, len), "fuzz r%d read %d+%d got %d", round, off, len, got);
			if(len > 0) {
				uint8_t const *m = PL_WadMap(off, len);
				if(m) CHECK(!memcmp(tr(m), g_file + off, len) && PL_IsWadPointer(m), "fuzz map %d+%d", off, len);
				/* map must succeed whenever every block is mapped and contiguous */
				int b0 = off >> 12, b1 = (off + len - 1) >> 12; bool contig = true;
				for(int b = b0; b <= b1; b++) {
					if(kind[b] == K_P0 || kind[b] == K_NULL) contig = false;
					if(b > b0 && slot[b] != slot[b-1] + 1) contig = false;
				}
				if(off + len >= fsize) contig = false;   /* EOF: never mapped */
				CHECK(!!m == contig, "fuzz map availability %d+%d m=%p contig=%d", off, len, (void*)m, contig);
			}
		}
		free(buf);
		PL_WadClose();
	}

	/* 8. lump ending at EOF: engine maps exactly to EOF and reads up to
	   W_LUMP_PAD past it (w_wad.c: heap copies are zero-filled) */
	for(int i = 0; i < 2; i++) { kind[i] = K_P2; slot[i] = 20 + i; }
	setup(2 * 4096 - 50, kind, slot);
	gwad_ram_allowed = false; PL_WadOpen("T.WAD");
	CHECK(gwad_stats.mode == GWAD_MAP, "EOF: map mode");
	{ uint8_t const *m = PL_WadMap(2 * 4096 - 50 - 60, 60);
	  CHECK(m == NULL, "EOF lump mapped in place (flash bytes past EOF)");
	  /* the same lump with room for the pad before EOF still maps */
	  m = PL_WadMap(2 * 4096 - 50 - 60 - 136, 60 + 136);
	  CHECK(m == NULL, "range ending at EOF");
	  m = PL_WadMap(2 * 4096 - 50 - 60 - 137, 60 + 136);
	  CHECK(m != NULL && !memcmp(tr(m), g_file + 2 * 4096 - 50 - 60 - 137, 60 + 136),
		"range ending 1 byte before EOF maps"); }
	PL_WadClose();
	{ uint8_t *h = g_file; int fs = 2 * 4096 - 50; h[4]=1;h[5]=h[6]=h[7]=0; h[8]=16;h[9]=h[10]=h[11]=0;
	  uint32_t pos = fs - 60; h[16]=pos;h[17]=pos>>8;h[18]=h[19]=0; h[20]=60;h[21]=h[22]=h[23]=0; memcpy(h+24,"LUMP\0\0\0\0",8); }
	gwad_ram_allowed = true; PL_WadOpen("T.WAD");
	CHECK(gwad_stats.mode == GWAD_RAM, "EOF: RAM mode");
	{ uint8_t const *m = PL_WadMap(2 * 4096 - 50 - 60, 60); int nz = 0;
	  CHECK(m != NULL, "EOF lump in RAM image");
	  for(int k = 60; m && k < 60 + 136; k++) nz += m[k] != 0;
	  CHECK(nz == 0, "RAM: %d of 136 bytes past EOF are non-zero", nz); }
	PL_WadClose();

	/* 9. Every engine WAD read and map is a quit-chord safe
	   point, polled first; a taken request (longjmp) leaves the WAD state
	   and the statistics untouched; PL_WadOpen() never polls */
	for(int i = 0; i < N; i++) { kind[i] = K_P2; slot[i] = 30 + i; }
	setup(N * 4096 - 3, kind, slot);
	gwad_ram_allowed = false;
	load_polls = 0;
	CHECK(PL_WadOpen("T.WAD") == N * 4096 - 3 && gwad_stats.mode == GWAD_MAP, "poll: open");
	CHECK(load_polls == 0, "PL_WadOpen polled %ld times", load_polls);
	{ uint8_t b[64];
	  CHECK(PL_WadRead(b, 100, 64) == 64 && load_polls == 1, "read polls (%ld)", load_polls);
	  CHECK(PL_WadMap(100, 64) != NULL && load_polls == 2, "map polls (%ld)", load_polls);
	  CHECK(PL_WadMap(-1, 64) == NULL && load_polls == 3, "a bad map still polls (%ld)", load_polls);
	  int hits = gwad_stats.map_hits, misses = gwad_stats.map_misses;
	  jmp_buf jb; volatile int left = 0;
	  poll_jmp = &jb;
	  memset(b, 0x5a, sizeof b);
	  if(!setjmp(jb)) { PL_WadRead(b, 100, 64); left = 1; }
	  if(!setjmp(jb)) { PL_WadMap(100, 64); left = 1; }
	  poll_jmp = NULL;
	  CHECK(!left, "the poll left before the read/map returned");
	  CHECK(b[0] == 0x5a && b[63] == 0x5a, "nothing read before the poll");
	  CHECK(gwad_stats.map_hits == hits && gwad_stats.map_misses == misses, "stats untouched");
	  CHECK(W.mode == GWAD_MAP && PL_WadRead(b, 100, 64) == 64 && !memcmp(b, g_file + 100, 64),
		"WAD state intact after the poll left"); }
	PL_WadClose();

	/* 10. PL_WadIsSlow() is 1 for the flash block map and BFile
	   reads (the engine then keeps hot lumps in the zone), 0 for the RAM
	   image and with no WAD; it stays 1 when gwad_remap() falls back from
	   MAP to BFILE */
	for(int i = 0; i < N; i++) { kind[i] = K_P2; slot[i] = 40 + i; }
	setup(N * 4096 - 3, kind, slot);
	CHECK(PL_WadIsSlow() == 0, "slow: no WAD");
	gwad_ram_allowed = false; PL_WadOpen("T.WAD");
	CHECK(W.mode == GWAD_MAP && PL_WadIsSlow() == 1, "slow: MAP (mode %d)", W.mode);
	memset(g_flash + 0x00C80000u + 40 * 4096u, 0x11, 12);   /* header gone */
	gwad_remap();
	CHECK(W.mode == GWAD_BFILE && PL_WadIsSlow() == 1, "slow: MAP -> BFILE (mode %d)", W.mode);
	PL_WadClose();
	CHECK(PL_WadIsSlow() == 0, "slow: closed");
	setup(N * 4096 - 3, kind, slot);
	/* no block map: BFile reads */
	g_os.block = NULL; PL_WadOpen("T.WAD");
	CHECK(W.mode == GWAD_BFILE && PL_WadIsSlow() == 1, "slow: BFILE (mode %d)", W.mode);
	PL_WadClose();
	g_os.block = s_block;
	/* a directory with one lump, for the RAM image (as in 8.) */
	{ uint8_t *h = g_file; uint32_t pos = 4096; h[4]=1;h[5]=h[6]=h[7]=0; h[8]=16;h[9]=h[10]=h[11]=0;
	  h[16]=pos;h[17]=pos>>8;h[18]=h[19]=0; h[20]=60;h[21]=h[22]=h[23]=0; memcpy(h+24,"LUMP\0\0\0\0",8); }
	gwad_ram_allowed = true; PL_WadOpen("T.WAD");
	CHECK(W.mode == GWAD_RAM && PL_WadIsSlow() == 0, "slow: RAM (mode %d)", W.mode);
	PL_WadClose();
	gwad_ram_allowed = false;

	/* 11. The map is checked against the file at open: block 0 must hold
	   the header, and sampled blocks (middle, last, directory) must match
	   a BFile read. A mismatch means
	   BFile reads, never wrong data. */
	{ struct { char const *tag; int block; } bad[] = {
		{ "header block", 0 }, { "middle block", N / 2 }, { "last block", N - 1 } };
	  for(unsigned t = 0; t < sizeof bad / sizeof *bad; t++) {
		for(int i = 0; i < N; i++) { kind[i] = K_P2; slot[i] = 60 + i; }
		setup(N * 4096 - 21, kind, slot);
		/* the flash copy of one block disagrees with the file */
		g_flash[0x00C80000u + slot[bad[t].block] * 4096u + 2] ^= 0x5a;
		gwad_ram_allowed = false;
		CHECK(PL_WadOpen("T.WAD") == N * 4096 - 21, "verify %s: open", bad[t].tag);
		CHECK(W.mode == GWAD_BFILE && gwad_stats.map_rejected && !gwad_map_live(),
			"verify %s: BFile reads (mode %d)", bad[t].tag, W.mode);
		uint8_t *b = malloc(N * 4096);
		CHECK(PL_WadRead(b, 0, N * 4096 - 21) == N * 4096 - 21
			&& !memcmp(b, g_file, N * 4096 - 21), "verify %s: true bytes", bad[t].tag);
		CHECK(PL_WadMap(0, 64) == NULL, "verify %s: nothing mapped", bad[t].tag);
		free(b);
		PL_WadClose();
	  }
	  /* the same layout, intact: block map */
	  setup(N * 4096 - 21, kind, slot);
	  PL_WadOpen("T.WAD");
	  CHECK(W.mode == GWAD_MAP && !gwad_stats.map_rejected, "verify: intact map kept");
	  PL_WadClose();
	  /* a directory block that disagrees (header points into block 7) */
	  setup(N * 4096 - 21, kind, slot);
	  g_file[8] = 0; g_file[9] = 0x70; g_file[10] = 0; g_file[11] = 0; g_file[4] = 0;
	  memcpy(g_flash + 0x00C80000u + slot[0] * 4096u, g_file, 12);
	  g_flash[0x00C80000u + slot[7] * 4096u + 5] ^= 1;
	  PL_WadOpen("T.WAD");
	  CHECK(W.mode == GWAD_BFILE && gwad_stats.map_rejected, "verify: directory block");
	  PL_WadClose(); }
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
