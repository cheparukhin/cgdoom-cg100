/* test_sh4.c - bit-exactness tests and benchmarks for the SH4 drawers (runs under sh-elf-run).

   usage: test_sh4 [test [N]]        randomized comparison against ref_draw.c (default N=3000 per
                                     drawer); exit status = number of failing checks (0 = pass)
          test_sh4 bench             instruction / simulator-cycle counts, C (-O2) vs asm
          test_sh4 one <name> <n>    run one benchmark kernel n times (for simbench)

   Every drawer is run twice from identical framebuffers (A for the C reference, B for the
   assembly) with identical globals; after each case the touched area plus a margin is compared,
   and every 16 cases (and at the end) the whole framebuffer including 4 KiB guards on both sides.
   Global side effects (fuzzpos, dc_yl/dc_yh for fuzz, ds_x1/ds_x2 for span low) are compared.

   Stray writes: every 128th case (and every case of the "long" pass, which draws full-height
   columns and full-width spans) snapshots all of the program's static memory - .text, .rodata,
   .data and .bss, i.e. everything the linker placed - before the asm call and checks afterwards
   that nothing changed except framebuffer B and the globals the drawer is allowed to write.
   (The framebuffer guards alone cannot see e.g. a store pointer walking a row per pixel
   through .bss; this check reports it from the first sampled case.)

   Calling convention: every asm drawer call goes through cgd_test_abi_call()
   (abi_call.S), which checks that r8-r14 and r15 come back unchanged.

   1-pixel columns: R_DrawColumn_SH4 / R_DrawColumnLow_SH4 take a separate
   path for dc_yl == dc_yh that saves no registers and reads dc_colormap directly. The "1px"
   passes draw only such columns (every colormap kind, unregistered table, non-contiguous
   columnofs) with the stray-write check after every call.

   Screen size: the test is built once per size of src/platform/cgd_config.h (Makefile TV=...:
   320x200 or 396x224), against the drawers assembled for it. The views follow
   R_ExecuteSetViewSize's sizes for that screen. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sim.h>
#include <fxlibc/printf.h>
#include "test_globals.h"
#include "../cgd_sh4.h"

/* ---- engine globals ---- */
const lighttable_t *dc_colormap;
int dc_x, dc_yl, dc_yh;
fixed_t dc_iscale, dc_texturemid;
const byte *dc_source;
byte *dc_translation;
int ds_y, ds_x1, ds_x2;
const lighttable_t *ds_colormap;
fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
const byte *ds_source;
byte *ylookup[MAXHEIGHT];
int columnofs[MAXWIDTH];
int centery, viewheight;
const lighttable_t *colormaps;
int fuzzpos;

/* ---- memory ---- */
#define GUARD 4096
#define FBSIZE (SCREENWIDTH * SCREENHEIGHT)
static uint8_t fbA_mem[GUARD + FBSIZE + GUARD] __attribute__((aligned(32)));
static uint8_t fbB_mem[GUARD + FBSIZE + GUARD] __attribute__((aligned(32)));
#define FBA (fbA_mem + GUARD)
#define FBB (fbB_mem + GUARD)

#define NMAPS 34
static uint8_t lumpbuf[(NMAPS + 2) * 256 + 64] __attribute__((aligned(32)));
static uint8_t *lump;                    /* registered COLORMAP lump (deliberately misaligned) */
static uint8_t rot[NMAPS * 256] __attribute__((aligned(32)));
static uint8_t othercm[8 * 256 + 64];    /* an unregistered colormap area */
static uint8_t texbuf[1 << 16];          /* column textures */
static uint8_t bigbuf[1 << 18];          /* translated columns: indices +-2^17 around the middle */
static uint8_t flatbuf[4 * 4096 + 64];
static uint8_t transbuf[4 * 256 + 16];

static uint32_t seed = 0x12345678;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t rndn(uint32_t n) { return n ? (uint32_t)(((uint64_t)rnd() * n) >> 32) : 0; }
static int rndr(int lo, int hi) { return lo + (int)rndn((uint32_t)(hi - lo + 1)); }   /* [lo, hi] */

typedef void (*drawfn)(void);
/* C fallbacks of r_draw_sh4_c.c (not in cgd_sh4.h: only the assembly calls them) */
void cgd_sh4_slow_fuzz(void);
void cgd_sh4_slow_fuzz_low(void);
void cgd_test_abi_call(drawfn fn, uint32_t out[9], uint32_t p);

/* The view sizes of R_ExecuteSetViewSize (320x200: 320x200, 320x168, 224x112, 96x48) */
#define SBARHEIGHT 32
#define VIEWW(b) (((b) * SCREENWIDTH / 10) & ~7)
#define VIEWH(b) (((b) * (SCREENHEIGHT - SBARHEIGHT) / 10) & ~7)
#define VIEW(b) { VIEWW(b), VIEWH(b), (SCREENWIDTH - VIEWW(b)) / 2, (SCREENHEIGHT - SBARHEIGHT - VIEWH(b)) / 2 }
struct view { int width, height, wx, wy; };
static const struct view views[] = {
	{ SCREENWIDTH, SCREENHEIGHT, 0, 0 },                /* full screen (setblocks 11) */
	{ SCREENWIDTH, SCREENHEIGHT - SBARHEIGHT, 0, 0 },   /* CGDoom default (setblocks 10) */
	VIEW(7),
	VIEW(3),
};
static const struct view *cur_view;
static int odd_columnofs;   /* columnofs not contiguous (exercises the low-detail C path) */

static void set_view(const struct view *v)
{
	cur_view = v;
	viewheight = v->height;
	centery = v->height / 2;
	for(int i = 0; i < v->width; i++)
		columnofs[i] = v->wx + i;
	if(odd_columnofs)
		for(int i = 0; i < v->width; i += 2) {
			int t = columnofs[i]; columnofs[i] = columnofs[i + 1]; columnofs[i + 1] = t;
		}
}
static void set_ylookup(uint8_t *fb)
{
	for(int i = 0; i < cur_view->height; i++)
		ylookup[i] = fb + (i + cur_view->wy) * SCREENWIDTH;
}

static int failures = 0;
static long cases = 0;
static int long_mode;            /* full-height columns / full-width spans, stray check each case */
static int one_px;               /* columns: dc_yl == dc_yh only, stray check each case */
#define STRAY_MASK 127           /* random passes: stray check when (n & STRAY_MASK) == STRAY_MASK */

static void report(const char *name, long n, const char *what, int extra)
{
	if(failures < 40)
		printf("FAIL %s case %ld: %s (%d) x=%d yl=%d yh=%d iscale=%08x mid=%08x | y=%d x1=%d x2=%d\n",
			name, n, what, extra, dc_x, dc_yl, dc_yh, (unsigned)dc_iscale, (unsigned)dc_texturemid,
			ds_y, ds_x1, ds_x2);
	failures++;
}

/* compare [lo, hi) of both framebuffers (offsets relative to FB start, may be negative) */
static int cmp_range(int lo, int hi)
{
	if(lo < -GUARD) lo = -GUARD;
	if(hi > FBSIZE + GUARD) hi = FBSIZE + GUARD;
	for(int i = lo; i < hi; i++)
		if(FBA[i] != FBB[i]) return i;
	return INT32_MIN;
}
static int cmp_full(void)
{
	const uint32_t *a = (const uint32_t *)fbA_mem, *b = (const uint32_t *)fbB_mem;
	for(unsigned i = 0; i < sizeof fbA_mem / 4; i++)
		if(a[i] != b[i]) return (int)(i * 4) - GUARD;
	return INT32_MIN;
}
static void resync(void) { memcpy(fbB_mem, fbA_mem, sizeof fbA_mem); }

/* ---- stray-write detection ----
   sim.ld puts .text at 0x1000 and ends .bss at __bss_end; the snapshot lives on the heap (after
   __bss_end), so it is not part of the compared range. */
extern char __bss_end[];
#define MEM_LO ((uint32_t *)0x1000)
static uint32_t *snap;
static size_t snap_words;
static int *const side_effects[] = { &dc_yl, &dc_yh, &fuzzpos, &ds_x1, &ds_x2 };

static void stray_init(void)
{
	snap_words = (size_t)((uint32_t *)__bss_end - MEM_LO) & ~(size_t)7;   /* __bss_end is 32-aligned */
	snap = malloc(snap_words * 4);
	if(!snap) { printf("stray_init: no memory\n"); exit(1); }
}
static void copy_words(uint32_t *d, const uint32_t *s, size_t n)   /* n % 8 == 0 */
{
	for(; n; n -= 8, d += 8, s += 8) {
		uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
		d[0] = a; d[1] = b; d[2] = c; d[3] = e;
		a = s[4]; b = s[5]; c = s[6]; e = s[7];
		d[4] = a; d[5] = b; d[6] = c; d[7] = e;
	}
}
/* first index in [lo, hi) where m and snap differ, or hi */
static size_t diff_words(size_t lo, size_t hi)
{
	const uint32_t *m = MEM_LO;
	while(lo < hi && m[lo] == snap[lo]) lo++;
	return lo;
}
static void stray_before(void) { copy_words(snap, MEM_LO, snap_words); }
/* checks that nothing changed outside [out, out + outsize) (32-byte aligned: framebuffer B or
   the expansion's destination) and the globals the drawers may write */
static void stray_after(const char *name, long n, const void *out, size_t outsize)
{
	size_t b0 = (size_t)((const uint32_t *)out - MEM_LO), b1 = b0 + outsize / 4;
	for(unsigned i = 0; i < sizeof side_effects / sizeof *side_effects; i++)
		snap[(uint32_t *)side_effects[i] - MEM_LO] = *(uint32_t *)side_effects[i];
	const size_t seg[2][2] = { { 0, b0 }, { b1, snap_words } };
	int bad = 0;
	size_t first = 0;
	uint32_t was = 0, now = 0;
	for(int k = 0; k < 2; k++)
		for(size_t i = diff_words(seg[k][0], seg[k][1]); i < seg[k][1]; i = diff_words(i + 1, seg[k][1])) {
			if(!bad) { first = i; was = snap[i]; now = MEM_LO[i]; }
			bad++;
		}
	if(!bad) return;
	/* undo first (framebuffer B excepted) so later cases start clean - this also restores the
	   test's own .bss (counters, stdio state) - then report */
	copy_words(MEM_LO, snap, b0 & ~(size_t)7);
	memcpy(MEM_LO + (b0 & ~(size_t)7), snap + (b0 & ~(size_t)7), (b0 & 7) * 4);
	memcpy(MEM_LO + b1, snap + b1, (snap_words - b1) * 4);
	if(failures < 40)
		printf("FAIL %s case %ld: %d stray word(s), first at %p: %08x -> %08x\n", name, n, bad,
			(void *)(MEM_LO + first), (unsigned)was, (unsigned)now);
	failures++;
}
/* the callee-saved registers as abi_call.S loads them from p */
static void abi_expect(uint32_t p, uint32_t e[7])
{
	e[0] = p; e[1] = ~p; e[2] = (p << 16) | (p >> 16);
	e[3] = (p & 0xffff0000u) | ((p & 0xff) << 8) | ((p >> 8) & 0xff);
	e[4] = 0u - p; e[5] = p << 1; e[6] = p >> 1;
}
static void call_asm(drawfn f, const char *name, long n)
{
	int chk = long_mode || one_px || (n & STRAY_MASK) == STRAY_MASK;
	uint32_t out[9], e[7], p = rnd();
	if(chk) stray_before();
	cgd_test_abi_call(f, out, p);
	if(chk) stray_after(name, n, fbB_mem, sizeof fbB_mem);
	abi_expect(p, e);
	for(int i = 0; i < 7; i++)
		if(out[i] != e[i]) { report(name, n, "callee-saved register clobbered: r", 8 + i); break; }
	if(out[7] != out[8]) report(name, n, "stack pointer moved by", (int)(out[7] - out[8]));
}

/* ---- random parameter generation ---- */
static const uint8_t *pick_colormap(int allow_bad)
{
	int r = rndn(100);
	if(!allow_bad || r < 85) return lump + 256 * rndn(NMAPS);
	if(r < 90) return othercm + rndn(7 * 256);                      /* unregistered */
	if(r < 95) return lump + 256 * rndn(NMAPS) + 1 + rndn(255);      /* misaligned in lump */
	if(r < 98) return lump + 256 * NMAPS;                           /* just past the end */
	return lump - 256;                                              /* just before */
}
static int pick_step(void)
{
	switch(rndn(6)) {
	case 0: return (int)rnd();                             /* anything */
	case 1: return (int)rndn(256);                         /* tiny */
	case 2: return -(int)rndr(0x100, 0x40000);             /* negative */
	default: return rndr(0x800, 0x40000);                  /* realistic */
	}
}
static int pick_mid(void)
{
	return rndn(3) ? rndr(-0x2000000, 0x2000000) : (int)rnd();
}
static void pick_rows(int *yl, int *yh)
{
	int h = cur_view->height;
	int r = rndn(10);
	if(long_mode) { *yl = 0; *yh = h - 1; return; }
	*yl = rndr(0, h - 1);
	if(one_px) { *yh = *yl; return; }
	if(r < 3) *yh = *yl + rndr(-1, 3);             /* short: 0..4 pixels */
	else if(r < 4) *yh = *yl - rndr(1, 5);         /* negative count */
	else *yh = rndr(*yl, h - 1);
	if(*yh > h - 1) *yh = h - 1;
}

/* ---- per-drawer test drivers ---- */

static void test_columns(const char *name, drawfn ref, drawfn asmf, int low, int translated, int N)
{
	for(int n = 0; n < N; n++, cases++) {
		set_view(&views[long_mode ? 0 : rndn(4)]);
		int w = cur_view->width >> low;
		dc_x = rndr(0, w - 1);
		pick_rows(&dc_yl, &dc_yh);
		if(rndn(8) == 0) centery = rndr(-300, 300);
		dc_iscale = pick_step();
		dc_texturemid = pick_mid();
		dc_colormap = pick_colormap(1);
		if(translated) {
			/* indices are (frac >> 16) without masking: keep them inside bigbuf */
			if(rndn(2)) { dc_iscale = rndr(0x800, 0x40000); dc_texturemid = rndr(0, 0x800000); }
			dc_source = bigbuf + (1 << 17) + rndn(256);
			dc_translation = transbuf + rndn(3 * 256 + 16);
		} else
			dc_source = texbuf + rndn(sizeof texbuf - 128);
		int yl = dc_yl, yh = dc_yh;
		set_ylookup(FBA); ref();
		set_ylookup(FBB); call_asm(asmf, name, n);
		if(dc_yl != yl || dc_yh != yh) report(name, n, "globals modified", 0);
		int y0 = yl - 2, y1 = (yh > yl ? yh : yl) + 2;
		int x0 = columnofs[low ? 2 * dc_x : dc_x] - 4;
		int lo = (y0 + cur_view->wy) * SCREENWIDTH + x0;
		int hi = (y1 + cur_view->wy) * SCREENWIDTH + x0 + 8;
		int bad = cmp_range(lo, hi);
		if(bad == INT32_MIN && (n & 15) == 15) bad = cmp_full();
		if(bad != INT32_MIN) { report(name, n, "pixel mismatch at offset", bad); resync(); }
	}
	int bad = cmp_full();
	if(bad != INT32_MIN) { report(name, N, "final full compare", bad); resync(); }
}

static void test_fuzz(const char *name, drawfn ref, drawfn asmf, int low, int N)
{
	for(int n = 0; n < N; n++, cases++) {
		set_view(&views[long_mode ? 0 : rndn(4)]);
		int w = cur_view->width >> low;
		int h = cur_view->height;
		dc_x = rndr(0, w - 1);
		pick_rows(&dc_yl, &dc_yh);
		if(rndn(4) == 0) dc_yl = 0;
		if(rndn(4) == 0) dc_yh = h - 1;
		dc_iscale = pick_step(); dc_texturemid = pick_mid();
		colormaps = rndn(8) ? lump : othercm;
		int fp = rndr(0, 49);
		int yl = dc_yl, yh = dc_yh;
		fuzzpos = fp;
		set_ylookup(FBA); ref();
		int r_yl = dc_yl, r_yh = dc_yh, r_fp = fuzzpos;
		dc_yl = yl; dc_yh = yh; fuzzpos = fp;
		set_ylookup(FBB); call_asm(asmf, name, n);
		if(dc_yl != r_yl || dc_yh != r_yh) report(name, n, "dc_yl/dc_yh side effect", dc_yl);
		if(fuzzpos != r_fp) report(name, n, "fuzzpos", fuzzpos);
		int bad = cmp_range((yl - 2 + cur_view->wy) * SCREENWIDTH, (yh + 3 + cur_view->wy) * SCREENWIDTH);
		if(bad == INT32_MIN && (n & 15) == 15) bad = cmp_full();
		if(bad != INT32_MIN) { report(name, n, "pixel mismatch at offset", bad); resync(); }
	}
	colormaps = lump;
	int bad = cmp_full();
	if(bad != INT32_MIN) { report(name, N, "final full compare", bad); resync(); }
}

static void test_spans(const char *name, drawfn ref, drawfn asmf, int low, int N)
{
	for(int n = 0; n < N; n++, cases++) {
		set_view(&views[long_mode ? 0 : rndn(4)]);
		int w = cur_view->width >> low;
		ds_y = rndr(0, cur_view->height - 1);
		int r = rndn(10);
		ds_x1 = rndr(0, w - 1);
		if(r < 3) ds_x2 = ds_x1 + rndr(0, 3);
		else ds_x2 = rndr(ds_x1, w - 1);
		if(long_mode) { ds_x1 = 0; ds_x2 = w - 1; }
		if(ds_x2 > w - 1) ds_x2 = w - 1;
		if(rndn(3)) {
			ds_xfrac = (int)rnd(); ds_yfrac = (int)rnd();
			ds_xstep = pick_step(); ds_ystep = pick_step();
		} else {
			ds_xfrac = rndr(-0x400000, 0x400000); ds_yfrac = rndr(-0x400000, 0x400000);
			ds_xstep = rndr(-0x30000, 0x30000); ds_ystep = rndr(-0x30000, 0x30000);
		}
		ds_colormap = pick_colormap(1);
		ds_source = flatbuf + rndn(sizeof flatbuf - 4096);
		int x1 = ds_x1, x2 = ds_x2;
		set_ylookup(FBA); ref();
		int r1 = ds_x1, r2 = ds_x2;
		ds_x1 = x1; ds_x2 = x2;
		set_ylookup(FBB); call_asm(asmf, name, n);
		if(ds_x1 != r1 || ds_x2 != r2) report(name, n, "ds_x1/ds_x2 side effect", ds_x1);
		int row = (ds_y + cur_view->wy) * SCREENWIDTH;
		int bad = cmp_range(row - SCREENWIDTH, row + 2 * SCREENWIDTH);
		if(bad == INT32_MIN && (n & 15) == 15) bad = cmp_full();
		if(bad != INT32_MIN) { report(name, n, "pixel mismatch at offset", bad); resync(); }
	}
	int bad = cmp_full();
	if(bad != INT32_MIN) { report(name, N, "final full compare", bad); resync(); }
}

/* ---- presenter conversion ---- */
static uint16_t pal[256];
static uint32_t pal32[256];
static uint8_t xsrc[6400 + 64] __attribute__((aligned(32)));
static uint16_t xdstA[6400 + 64] __attribute__((aligned(32)));
static uint16_t xdstB[6400 + 64] __attribute__((aligned(32)));

/* Plain/p32 expansion with a stack canary above the callee's frame (the plain entry builds a
   1 KiB table on its stack) and, every 8th case, the static-memory stray-write check. */
static void __attribute__((noinline)) expand_checked(int which, uint16_t *dst, const uint8_t *src,
	int np, long n)
{
	volatile uint32_t canary[32];
	for(int i = 0; i < 32; i++) canary[i] = 0xc0de0000u + i;
	int chk = (n & 7) == 7;
	if(chk) stray_before();
	if(which) cgd_expand_p8_rgb565_p32(dst, src, pal32, np);
	else cgd_expand_p8_rgb565(dst, src, pal, np);
	if(chk) stray_after(which ? "expand_p32" : "expand", n, xdstB, sizeof xdstB);
	for(int i = 0; i < 32; i++)
		if(canary[i] != 0xc0de0000u + i) { report(which ? "expand_p32" : "expand", n, "stack canary", i); break; }
}

static void test_expand(int N)
{
	for(int n = 0; n < N; n++, cases++) {
		for(int i = 0; i < 256; i++) pal[i] = (uint16_t)rnd();
		cgd_expand_build_pal32(pal32, pal);
		/* the first cases use fixed sizes: 3,200 px and the 6,400 px maximum of this buffer,
		   then the plain entry's table threshold (256) and its neighbours; then random sizes */
		static const int fixed[] = { 3200, 3200, 6400, 6400, 16, 16, 240, 240, 256, 256, 272, 272 };
		int np = n < (int)(sizeof fixed / sizeof *fixed) ? fixed[n] : 16 * rndr(1, 400);
		int so = 4 * rndn(8);
		for(int i = 0; i < np; i++) xsrc[so + i] = (uint8_t)rnd();
		for(int i = 0; i < 6400 + 64; i++) xdstA[i] = xdstB[i] = (uint16_t)rnd();
		ref_expand(xdstA, xsrc + so, pal, np);
		int which = n & 1;
		expand_checked(which, xdstB, xsrc + so, np, n);
		if(memcmp(xdstA, xdstB, sizeof xdstA)) {
			int i = 0; while(xdstA[i] == xdstB[i]) i++;
			report(which ? "expand_p32" : "expand", n, "mismatch at pixel", i);
		}
	}
}

static void test_fill(int N)
{
	for(int n = 0; n < N; n++, cases++) {
		uint32_t v = rnd();
		int sz = 32 * rndr(0, 200);
		uint8_t *a = (uint8_t *)xdstA, *b = (uint8_t *)xdstB;
		for(int i = 0; i < (int)sizeof xdstA; i++) a[i] = b[i] = (uint8_t)rnd();
		for(int i = 0; i < sz / 4; i++) ((uint32_t *)(a + 32))[i] = v;
		cgd_fill32(b + 32, v, sz);
		if(memcmp(a, b, sizeof xdstA)) report("fill32", n, "mismatch", sz);
	}
}

/* expected cgd_fixed_div(a, b): vanilla FixedDiv where vanilla returns; for a == INT_MIN and
   |b| <= 65536 (vanilla I_Error()s, except b == 65536 which is the exact INT_MIN) the contract
   is saturation by the sign of the true quotient. */
static int32_t exp_div(int32_t a, int32_t b)
{
	int32_t aa = a < 0 ? (int32_t)(0u - (uint32_t)a) : a;
	int32_t ab = b < 0 ? (int32_t)(0u - (uint32_t)b) : b;
	if((aa >> 14) >= ab || (a == INT32_MIN && ab <= 65536))
		return ((a ^ b) < 0) ? INT32_MIN : INT32_MAX;
	return (int32_t)(((int64_t)a << 16) / b);
}

static void test_div(int N)
{
	/* a == INT_MIN against boundary divisors: a >> 16 second guard (instead of >> 15) would
	   return wrong-signed values for 32768 < |b| < 65536 */
	static const int32_t bs[] = { 1, -1, 2, -2, 32767, -32767, 32768, -32768, 32769, -32769,
		40000, -40000, 65535, -65535, 65536, -65536, 65537, -65537, 131072, -131072,
		INT32_MAX, INT32_MIN };
	for(unsigned k = 0; k < sizeof bs / sizeof *bs; k++, cases++) {
		int32_t got = cgd_fixed_div(INT32_MIN, bs[k]), exp = exp_div(INT32_MIN, bs[k]);
		if(got != exp) { report("fixed_div(INT_MIN)", k, "mismatch", got); printf("  b=%d exp=%d\n", (int)bs[k], (int)exp); }
	}
	if(cgd_fixed_div(INT32_MIN, 65536) != INT32_MIN) report("fixed_div(INT_MIN,65536)", 0, "not exact", 0);
	for(int n = 0; n < N / 10; n++, cases++) {
		int32_t b = rndr(1, 65536 + 64);
		if(rnd() & 1) b = -b;
		int32_t got = cgd_fixed_div(INT32_MIN, b), exp = exp_div(INT32_MIN, b);
		if(got != exp) report("fixed_div(INT_MIN)", n, "mismatch", got);
	}
	for(int n = 0; n < N; n++, cases++) {
		int32_t a = (int32_t)rnd() >> rndn(24), b = (int32_t)rnd() >> rndn(31);
		if(rndn(16) == 0) b = INT32_MIN;
		if(b == 0) b = 1;
		int32_t got = cgd_fixed_div(a, b), exp = exp_div(a, b);
		if(got != exp) report("fixed_div", n, "mismatch", got);
	}
}

static void init_data(void)
{
	lump = lumpbuf + 256 + 7;    /* registered lump at an odd address, one map of slack before */
	for(unsigned i = 0; i < sizeof lumpbuf; i++) lumpbuf[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof othercm; i++) othercm[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof texbuf; i++) texbuf[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof bigbuf; i++) bigbuf[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof flatbuf; i++) flatbuf[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof transbuf; i++) transbuf[i] = (uint8_t)rnd();
	for(unsigned i = 0; i < sizeof fbA_mem; i++) fbA_mem[i] = (uint8_t)rnd();
	resync();
	colormaps = lump;
	cgd_sh4_set_colormaps(lump, NMAPS, rot);
	set_view(&views[0]);
}

/*==== benchmarks =================================================================================*/

struct colcase { int x, yl, yh, iscale, mid; const uint8_t *cm, *src; };
struct spancase { int y, x1, x2, xf, yf, xs, ys; const uint8_t *cm, *src; };
#define BENCH_H (SCREENHEIGHT - SBARHEIGHT)     /* views[1] */
#define NB 400
static struct colcase cc[NB];
static struct spancase sc[NB];
static long bench_pixels;

static void gen_cols(int lmin, int lmax, int low)
{
	bench_pixels = 0;
	for(int i = 0; i < NB; i++) {
		int len = rndr(lmin, lmax);
		cc[i].x = rndr(0, (SCREENWIDTH >> low) - 1);
		cc[i].yl = rndr(0, BENCH_H - len);
		cc[i].yh = cc[i].yl + len - 1;
		cc[i].iscale = rndr(0x2000, 0x30000);
		cc[i].mid = rndr(0, 0x800000);
		cc[i].cm = lump + 256 * rndn(32);
		cc[i].src = texbuf + rndn(sizeof texbuf - 512);
		bench_pixels += len << low;
	}
}
static void gen_spans(int lmin, int lmax, int low)
{
	bench_pixels = 0;
	for(int i = 0; i < NB; i++) {
		int w = SCREENWIDTH >> low;
		int len = rndr(lmin >> low, lmax >> low);
		if(len > w) len = w;
		if(len < 1) len = 1;
		sc[i].y = rndr(0, BENCH_H - 1);
		sc[i].x1 = rndr(0, w - len);
		sc[i].x2 = sc[i].x1 + len - 1;
		sc[i].xf = (int)rnd(); sc[i].yf = (int)rnd();
		sc[i].xs = rndr(-0x20000, 0x20000); sc[i].ys = rndr(-0x20000, 0x20000);
		sc[i].cm = lump + 256 * rndn(32);
		sc[i].src = flatbuf + 64 * rndn(16);
		bench_pixels += len << low;
	}
}
static void run_cols(drawfn f)
{
	for(int i = 0; i < NB; i++) {
		dc_x = cc[i].x; dc_yl = cc[i].yl; dc_yh = cc[i].yh; dc_iscale = cc[i].iscale;
		dc_texturemid = cc[i].mid; dc_colormap = cc[i].cm; dc_source = cc[i].src;
		f();
	}
}
static void run_spans(drawfn f)
{
	for(int i = 0; i < NB; i++) {
		ds_y = sc[i].y; ds_x1 = sc[i].x1; ds_x2 = sc[i].x2; ds_xfrac = sc[i].xf; ds_yfrac = sc[i].yf;
		ds_xstep = sc[i].xs; ds_ystep = sc[i].ys; ds_colormap = sc[i].cm; ds_source = sc[i].src;
		f();
	}
}
static void nop_draw(void) { }

static int measure(void (*runner)(drawfn), drawfn f, double *ipp, double *cpp)
{
	struct sim_counters a, b, c, d;
	if(sim_counters(&a)) return -1;
	runner(nop_draw);
	sim_counters(&b);
	runner(f);
	sim_counters(&c);
	(void)d;
	uint64_t base_i = b.insts - a.insts, base_c = b.cycles - a.cycles;
	*ipp = (double)((c.insts - b.insts) - base_i) / bench_pixels;
	*cpp = (double)((c.cycles - b.cycles) - base_c) / bench_pixels;
	return 0;
}

static void bench_line(const char *what, void (*runner)(drawfn), drawfn ref, drawfn asmf)
{
	double ri, rc, ai, ac;
	if(measure(runner, ref, &ri, &rc) || measure(runner, asmf, &ai, &ac)) {
		printf("  (sim_counters unavailable: build the sim64 simulator with make -C src/sim sim-tool)\n");
		return;
	}
	printf("  %-34s C: %6.2f insts/px %6.2f cyc/px   asm: %6.2f insts/px %6.2f cyc/px   (%5.2fx insts)\n",
		what, ri, rc, ai, ac, ri / ai);
}

static void bench(void)
{
	char buf[96];
	set_view(&views[1]);
	set_ylookup(FBA);
	colormaps = lump;
	printf("benchmarks (%dx%d): per output pixel, call overhead included (sim64 counters; 'cyc' is\n"
	       "the simulator's SH-1-style cycle model, NOT SH-4A timing - see README for pipeline\n"
	       "estimates)\n", SCREENWIDTH, SCREENHEIGHT);
	static const int lens[][2] = { {10, BENCH_H}, {1, 1}, {2, 2}, {3, 3}, {8, 8}, {32, 32}, {100, 100}, {BENCH_H, BENCH_H} };
	for(unsigned k = 0; k < sizeof lens / sizeof *lens; k++) {
		gen_cols(lens[k][0], lens[k][1], 0);
		sprintf(buf, "R_DrawColumn len %d..%d", lens[k][0], lens[k][1]);
		bench_line(buf, run_cols, ref_R_DrawColumn, R_DrawColumn_SH4);
	}
	for(unsigned k = 0; k < sizeof lens / sizeof *lens; k++) {
		gen_cols(lens[k][0], lens[k][1], 1);
		sprintf(buf, "R_DrawColumnLow len %d..%d", lens[k][0], lens[k][1]);
		bench_line(buf, run_cols, ref_R_DrawColumnLow, R_DrawColumnLow_SH4);
	}
	gen_cols(10, BENCH_H, 0);
	dc_translation = transbuf;
	sprintf(buf, "R_DrawTranslatedColumn len 10..%d", BENCH_H);
	bench_line(buf, run_cols, ref_R_DrawTranslatedColumn, R_DrawTranslatedColumn_SH4);
	{
		/* fuzz: keep inside the view (rows 1 .. BENCH_H - 2) */
		for(int i = 0; i < NB; i++) { if(cc[i].yl < 1) cc[i].yl = 1; if(cc[i].yh > BENCH_H - 2) cc[i].yh = BENCH_H - 2; }
		bench_pixels = 0;
		for(int i = 0; i < NB; i++) bench_pixels += cc[i].yh - cc[i].yl + 1;
		sprintf(buf, "R_DrawFuzzColumn len ~10..%d", BENCH_H - 2);
		bench_line(buf, run_cols, ref_R_DrawFuzzColumn, R_DrawFuzzColumn_SH4);
	}
	static const int slens[][2] = { {20, SCREENWIDTH}, {1, 1}, {4, 4}, {16, 16}, {64, 64}, {SCREENWIDTH, SCREENWIDTH} };
	for(unsigned k = 0; k < sizeof slens / sizeof *slens; k++) {
		gen_spans(slens[k][0], slens[k][1], 0);
		sprintf(buf, "R_DrawSpan len %d..%d", slens[k][0], slens[k][1]);
		bench_line(buf, run_spans, ref_R_DrawSpan, R_DrawSpan_SH4);
	}
	for(unsigned k = 0; k < sizeof slens / sizeof *slens; k++) {
		if(slens[k][1] < 2) continue;
		gen_spans(slens[k][0], slens[k][1], 1);
		sprintf(buf, "R_DrawSpanLow len %d..%d", slens[k][0], slens[k][1]);
		bench_line(buf, run_spans, ref_R_DrawSpanLow, R_DrawSpanLow_SH4);
	}

	/* presenter conversion: one 320x200 frame */
	{
		static uint8_t frame[64000] __attribute__((aligned(32)));
		static uint16_t out[64000] __attribute__((aligned(32)));
		struct sim_counters a, b, c, d, e;
		for(int i = 0; i < 64000; i++) frame[i] = (uint8_t)rnd();
		for(int i = 0; i < 256; i++) pal[i] = (uint16_t)rnd();
		cgd_expand_build_pal32(pal32, pal);
		sim_counters(&a);
		ref_expand(out, frame, pal, 64000);
		sim_counters(&b);
		cgd_expand_p8_rgb565(out, frame, pal, 64000);
		sim_counters(&c);
		cgd_expand_p8_rgb565_p32(out, frame, pal32, 64000);
		sim_counters(&d);
		cgd_expand_build_pal32(pal32, pal);
		sim_counters(&e);
		printf("  expand 64000 px: C %.2f insts/px %.2f cyc/px | asm %.2f / %.2f | asm p32 %.2f / %.2f | build_pal32 %llu insts\n",
			(double)(b.insts - a.insts) / 64000, (double)(b.cycles - a.cycles) / 64000,
			(double)(c.insts - b.insts) / 64000, (double)(c.cycles - b.cycles) / 64000,
			(double)(d.insts - c.insts) / 64000, (double)(d.cycles - c.cycles) / 64000,
			(unsigned long long)(e.insts - d.insts));
	}
}

/* single kernel for simbench (stock simulator): run_cols/run_spans of a realistic set, n times */
static int one(const char *name, int n)
{
	static const struct { const char *name; void (*runner)(drawfn); drawfn f; int span, low; } k[] = {
		{ "col_c", run_cols, ref_R_DrawColumn, 0, 0 }, { "col_asm", run_cols, R_DrawColumn_SH4, 0, 0 },
		{ "span_c", run_spans, ref_R_DrawSpan, 1, 0 }, { "span_asm", run_spans, R_DrawSpan_SH4, 1, 0 },
		{ "empty_col", run_cols, nop_draw, 0, 0 }, { "empty_span", run_spans, nop_draw, 1, 0 },
	};
	set_view(&views[1]); set_ylookup(FBA);
	for(unsigned i = 0; i < sizeof k / sizeof *k; i++) {
		if(strcmp(name, k[i].name)) continue;
		if(k[i].span) gen_spans(20, SCREENWIDTH, 0); else gen_cols(10, BENCH_H, 0);
		for(int j = 0; j < n; j++) k[i].runner(k[i].f);
		printf("%s: %ld pixels per run\n", name, bench_pixels);
		return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	__printf_enable_fp();
	init_data();
	stray_init();
	if(argc > 1 && !strcmp(argv[1], "expand")) {   /* expansion/fill only (P1-forced build) */
		test_expand(argc > 2 ? atoi(argv[2]) : 300);
		test_fill(argc > 2 ? atoi(argv[2]) : 300);
		printf("%ld cases, %d failures: %s\n", cases, failures, failures ? "FAIL" : "PASS");
		return failures;
	}
	if(argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
	if(argc > 3 && !strcmp(argv[1], "one")) return one(argv[2], atoi(argv[3]));
	int N = (argc > 2) ? atoi(argv[2]) : 3000;

	test_columns("R_DrawColumn", ref_R_DrawColumn, R_DrawColumn_SH4, 0, 0, N);
	test_columns("R_DrawColumnLow", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N);
	test_columns("R_DrawTranslatedColumn", ref_R_DrawTranslatedColumn, R_DrawTranslatedColumn_SH4, 0, 1, N);
	test_columns("R_DrawTranslatedColumnLow", ref_R_DrawTranslatedColumnLow, R_DrawTranslatedColumnLow_SH4, 1, 1, N);
	test_fuzz("R_DrawFuzzColumn", ref_R_DrawFuzzColumn, R_DrawFuzzColumn_SH4, 0, N);
	test_fuzz("R_DrawFuzzColumnLow", ref_R_DrawFuzzColumnLow, R_DrawFuzzColumnLow_SH4, 1, N);
	test_spans("R_DrawSpan", ref_R_DrawSpan, R_DrawSpan_SH4, 0, N);
	test_spans("R_DrawSpanLow", ref_R_DrawSpanLow, R_DrawSpanLow_SH4, 1, N);
	/* low detail with non-contiguous columnofs: C fallback path */
	odd_columnofs = 1;
	test_columns("R_DrawColumnLow(oddcols)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N / 4);
	test_fuzz("R_DrawFuzzColumnLow(oddcols)", ref_R_DrawFuzzColumnLow, R_DrawFuzzColumnLow_SH4, 1, N / 4);
	test_columns("R_DrawTranslatedColumnLow(oddcols)", ref_R_DrawTranslatedColumnLow, R_DrawTranslatedColumnLow_SH4, 1, 1, N / 4);
	odd_columnofs = 0;
	/* full-height columns / full-width spans on the full screen, stray-write check after every asm call */
	long_mode = 1;
	int NL = N / 100 + 8;
	test_columns("R_DrawColumn(long)", ref_R_DrawColumn, R_DrawColumn_SH4, 0, 0, NL);
	test_columns("R_DrawColumnLow(long)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, NL);
	test_columns("R_DrawTranslatedColumn(long)", ref_R_DrawTranslatedColumn, R_DrawTranslatedColumn_SH4, 0, 1, NL);
	test_columns("R_DrawTranslatedColumnLow(long)", ref_R_DrawTranslatedColumnLow, R_DrawTranslatedColumnLow_SH4, 1, 1, NL);
	test_fuzz("R_DrawFuzzColumn(long)", ref_R_DrawFuzzColumn, R_DrawFuzzColumn_SH4, 0, NL);
	test_fuzz("R_DrawFuzzColumnLow(long)", ref_R_DrawFuzzColumnLow, R_DrawFuzzColumnLow_SH4, 1, NL);
	test_spans("R_DrawSpan(long)", ref_R_DrawSpan, R_DrawSpan_SH4, 0, NL);
	test_spans("R_DrawSpanLow(long)", ref_R_DrawSpanLow, R_DrawSpanLow_SH4, 1, NL);
	long_mode = 0;
	/* 1-pixel columns (a separate asm path) */
	one_px = 1;
	test_columns("R_DrawColumn(1px)", ref_R_DrawColumn, R_DrawColumn_SH4, 0, 0, N / 4);
	test_columns("R_DrawColumnLow(1px)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N / 4);
	odd_columnofs = 1;
	test_columns("R_DrawColumnLow(1px,oddcols)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N / 8);
	odd_columnofs = 0;
	one_px = 0;
	/* nothing registered: everything through C (1-pixel columns: still asm, direct lookup) */
	cgd_sh4_set_colormaps(NULL, 0, NULL);
	test_columns("R_DrawColumn(unreg)", ref_R_DrawColumn, R_DrawColumn_SH4, 0, 0, N / 8);
	test_columns("R_DrawColumnLow(unreg)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N / 8);
	test_spans("R_DrawSpan(unreg)", ref_R_DrawSpan, R_DrawSpan_SH4, 0, N / 8);
	one_px = 1;
	test_columns("R_DrawColumn(1px,unreg)", ref_R_DrawColumn, R_DrawColumn_SH4, 0, 0, N / 8);
	test_columns("R_DrawColumnLow(1px,unreg)", ref_R_DrawColumnLow, R_DrawColumnLow_SH4, 1, 0, N / 8);
	one_px = 0;
	cgd_sh4_set_colormaps(lump, NMAPS, rot);
	/* the fuzz drawers' C fallbacks, called directly: only counts the engine never produces
	   would reach them */
	test_fuzz("cgd_sh4_slow_fuzz", ref_R_DrawFuzzColumn, cgd_sh4_slow_fuzz, 0, N / 8);
	test_fuzz("cgd_sh4_slow_fuzz_low", ref_R_DrawFuzzColumnLow, cgd_sh4_slow_fuzz_low, 1, N / 8);
	test_expand(N / 10 + 20);
	test_fill(N / 10 + 20);
	test_div(N * 10);

	printf("%dx%d: %ld cases, %d failures: %s\n", SCREENWIDTH, SCREENHEIGHT,
		cases, failures, failures ? "FAIL" : "PASS");
	return failures > 255 ? 255 : failures;
}
