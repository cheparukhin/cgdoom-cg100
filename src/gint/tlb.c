/* CGDoom gint platform: keep our code pages mapped (fx-CG100 / MPM).

   See tlbh.S for the problem. Three layers of protection:
   1. At startup, snapshot every UTLB entry that maps a 64 KiB page of our
      image ([0x00300000, 0x00700000)) and install our own TLB-miss handler
      at VBR+0x400, which reloads such pages from the snapshot.
      This only covers misses taken while gint's VBR is installed, i.e. in
      the gint world. It does NOT cover the OS-world window of a world
      switch: gint_world_switch_out() gives the OS its VBR back (drv_cpu
      hrestore) before gint_call(), and ours returns only in
      gint_world_switch_in(), after gint_call(), the on-chip memory
      restore, the canary check and the funbind loop. Our os_* callbacks
      (wad.c, files.c) and that part of gint all run from P0 in this window,
      so a miss there goes to the OS's handler. (An earlier version of this
      comment claimed layer 1 covered it; gint review round 1.)
   2. Pinning (gint review round 1): move the entries of the image pages
      into the slots just below MMUCR.URB and lower URB under them. LDTLB
      with MMUCR.URC (the OS's replacement, and our handler's) only selects
      slots below URB, so the OS can no longer evict them, in either world.
      The moves run in cgd_tlb_apply() (tlbh.S) from its P2 alias with
      interrupts masked, and never leave two valid entries for one page.
      gint itself never touches MMUCR, so the OS runs with our URB during
      world switches.
   3. After every world switch (gfs_os_call), verify the slots of the image
      pages and rewrite the ones that are missing (and the URB, if the OS
      changed it), using the same executor. Unpinned pages already
      re-loaded elsewhere by the handler are left alone; pinned ones are
      moved back. Pages MPM maps beyond the image are not repaired (round
      2): nothing runs there, the handler reloads them on demand.
   At exit (gtlb_quit, run from main.c's destructor; gint review rounds 2
   and 3.1), every snapshot page is put back in the slot MPM gave it, every
   other copy is dropped and the original URB is restored. Required: only mpm.bin v1.1 flushes
   the TLB (invalidateTLB) before the next launch and only dynload rewrites
   all of slots 0..54. The Feb-2025 public mpm.bin (and a1a2cc3, 0fb6cec)
   maps ROM pages into slots 0..47 only, never sets MMUCR.TI, and leaves
   48..54 alone, so a copy of one of our pages left in 49..53 would make
   the next add-in launch hit two UTLB entries (reset). It runs from a
   destructor, not an atexit handler: gint's start2 runs .dtors after
   exit() AND after abort() (raise -> _Exit longjmp), which skips atexit
   handlers; that covers the chord's hang fallback (time.c) and EXIT on
   gint's panic screen.
   Without the P2 executor (gint review round 3.1) nothing can drop the
   handler's reloads at exit, so gtlb_init() instead lowers URB to the
   leading slots MPM rewrites (48 under Feb-2025; nothing to do under v1.1
   and dynload) with a plain MMUCR write from P1 (cgd_tlbh_set_mmucr, in
   the handler's block) and keeps it there after world switches, up to and
   including kquit (MPM rewrites the whole MMUCR when the add-in returns).

   Planning is pure logic in tlbplan.c (host-tested, tests/gint-host).

   fx-CG50: nothing is done. The OS demand-pages the add-in from flash
   through gint's handler (syscall 12), which must stay. */

#include "cgd_gint.h"

#include <gint/cpu.h>
#include <gint/defs/util.h>
#include <stdlib.h>
#include <string.h>

#define UTLB_ADDR(i)  (*(volatile uint32_t *)(0xf6000000u | ((uint32_t)(i) << 8)))
#define UTLB_DATA(i)  (*(volatile uint32_t *)(0xf7000000u | ((uint32_t)(i) << 8)))
#define MMUCR         (*(volatile uint32_t *)0xff000010u)

#define PAGE_64K      0x10000u

/* Set to 0 to disable pinning (layers 1 and 3 still run). Without a
   rebuild: hold DEL while starting the add-in (safe start, main.c), which
   also leaves out the executor */
#ifndef CGD_TLB_PIN
#define CGD_TLB_PIN   1
#endif

/* GTLB_PAGES entries followed by the miss counter (table + 512, tlbh.S) */
static gtlb_entry_t *g_table = NULL;
static int8_t g_slot[GTLB_PAGES];
static uint32_t g_no_misses = 0;
/* Slot of every page in MPM's layout at startup (g_slot is the working
   copy, restricted to the image pages and changed by pinning) */
static int8_t g_slot_orig[GTLB_PAGES];
/* Image pages (0 if the image size is unknown) */
static int g_image_pages = 0;
/* Operation buffer (P1 heap) and the P2 alias of cgd_tlb_apply(). Without
   the executor nothing writes the UTLB except the miss handler. */
static gtlb_op_t *g_ops = NULL;
static void (*g_apply)(gtlb_op_t const *ops, int n, uint32_t mmucr) = NULL;
/* First pinned slot (= MMUCR.URB while pinned), GTLB_SLOTS if none */
static int g_pin_lo = GTLB_SLOTS;
/* MMUCR writer in the VBR area (P1 copy of cgd_tlbh_set_mmucr) and the URB
   it keeps without the executor (0: none), gint review round 3.1 */
static void (*g_set_mmucr)(uint32_t mmucr) = NULL;
static int g_urb_cap = 0;

gtlb_stats_t gtlb_stats = { .misses = &g_no_misses };

extern char cgd_tlbh_start[], cgd_tlbh_end[], cgd_tlbh_table[];
extern char cgd_tlbh_set_mmucr[];
extern void cgd_tlb_apply(gtlb_op_t const *ops, int n, uint32_t mmucr);

static uint32_t page_size(uint32_t data)
{
	int sz = (((data >> 7) & 1) << 1) | ((data >> 4) & 1);
	static uint32_t const sizes[4] = { 0x400, 0x1000, 0x10000, 0x100000 };
	return sizes[sz];
}

static void install_handler(void)
{
	uint32_t size = (uint32_t)(cgd_tlbh_end - cgd_tlbh_start);
	if(size == 0 || size > 0x200)
		return;

	uint8_t *dst = (uint8_t *)(cpu_getVBR() + 0x400);
	uint32_t table_off = (uint32_t)(cgd_tlbh_table - cgd_tlbh_start);

	cpu_atomic_start();
	memcpy(dst, cgd_tlbh_start, size);
	*(uint32_t volatile *)(dst + table_off) = (uint32_t)g_table;

	/* The VBR area is P1 (cached): write back, then drop stale I-cache */
	for(uint32_t off = 0; off < size; off += 32) {
		__asm__ volatile("ocbwb @%0" :: "r"(dst + off) : "memory");
		__asm__ volatile("icbi @%0" :: "r"(dst + off) : "memory");
	}
	synco();
	cpu_atomic_end();

	g_set_mmucr = (void *)(dst + (cgd_tlbh_set_mmucr - cgd_tlbh_start));
	gtlb_stats.handler = true;
}

static void read_view(gtlb_view_t *v)
{
	for(int i = 0; i < GTLB_SLOTS; i++) {
		v->addr[i] = UTLB_ADDR(i);
		v->data[i] = UTLB_DATA(i);
	}
}

void gtlb_read_view(gtlb_view_t *v)
{
	if(!g_is_cg100) {
		memset(v, 0, sizeof *v);
		return;
	}
	cpu_atomic_start();
	read_view(v);
	cpu_atomic_end();
}

/* Locate cgd_tlb_apply() through P2 (untranslated, uncached): its P0
   address, translated with our own snapshot. Checked by comparing the
   first instructions through both views. */
static bool setup_executor(void)
{
	uint32_t a = (uint32_t)&cgd_tlb_apply;
	if(a < GTLB_ROM_BASE || a >= GTLB_ROM_BASE + (GTLB_PAGES << 16))
		return false;
	gtlb_entry_t const *e = &g_table[(a - GTLB_ROM_BASE) >> 16];
	if(!(e->ptel & GTLB_DATA_V) || page_size(e->ptel) != PAGE_64K)
		return false;
	uint32_t phys = (e->ptel & GTLB_DATA_PPN & ~(PAGE_64K - 1))
		| (a & (PAGE_64K - 1));
	uint32_t p2 = 0xa0000000u | phys;
	for(int i = 0; i < 4; i++) {
		if(((uint32_t volatile *)a)[i] != ((uint32_t volatile *)p2)[i])
			return false;
	}

	gtlb_op_t *ops = malloc(GTLB_MAX_OPS * sizeof *ops);
	if(!ops)
		return false;
	if(((uintptr_t)ops >> 29) != 4) {	/* must be P1 */
		free(ops);
		return false;
	}
	g_ops = ops;
	g_apply = (void *)p2;
	return true;
}

/* Run ops, then set MMUCR = mmucr. Interrupts must be masked.

   Only through the executor (gint review round 2). The former direct
   fallback (MMUCR.URC + ldtlb from P0) did not drop the other entries of
   the page, which the planners assume (a foreign entry overlapping the
   page would have become a multiple hit), and ran from P0, where an ITLB
   refill between the URC write and ldtlb can move URC. Invalidating
   through the address array needs P1/P2 code, i.e. the executor; without
   it, nothing is written (callers check g_apply). */
static void apply(gtlb_op_t const *ops, int n, uint32_t mmucr)
{
	if(g_apply)
		g_apply(ops, n, mmucr);
}

/* Without the executor: keep MMUCR.URB at most g_urb_cap (gint review
   round 3.1). A plain MMUCR write from P1, no ldtlb. Interrupts masked. */
static bool urb_above(int urb, int cap)
{
	return (urb == 0 ? GTLB_SLOTS : urb) > cap;
}

static void contain_urb(void)
{
	if(!g_set_mmucr)
		return;
	cpu_atomic_start();
	uint32_t mmucr = MMUCR;
	int urb = gtlb_mmucr_urb(mmucr);
	int cap = gtlb_plan_contain_urb(g_slot_orig, urb);
	if(cap != urb) {
		g_set_mmucr(gtlb_mmucr_with_urb(mmucr, cap));
		g_urb_cap = cap;
		gtlb_stats.urb_cap = cap;
	}
	cpu_atomic_end();
}

/* Exit normalisation, see the header comment. Called from main.c's
   destructor (after the WAD file is closed) so that it also runs on the
   abort() path: gint start2 runs .dtors before kquit on both paths; the
   heap holding g_table/g_ops is still valid then. */
void gtlb_quit(void)
{
	if(!g_table || gtlb_stats.pages == 0)
		return;

	/* No executor (gint review round 3.1): nothing was pinned and nothing
	   can be unpinned; the handler's reloads stayed below g_urb_cap, in
	   slots MPM's next launch rewrites. The URB is deliberately NOT
	   restored: kquit still runs from our image with this handler
	   installed, and a reload there must not land in 48..53 either. The
	   cap is only ever set under the Feb-2025 family (k = 48), whose
	   MMU_SetEnabled(false) after the add-in returns writes the whole
	   MMUCR (URB 63), and MMU_SetEnabled(true) sets URB 54 again before
	   the next launch (loader.c in 68e7f6e, a1a2cc3, 0fb6cec, e99a7d1). */
	if(!g_apply)
		return;

	static gtlb_view_t v;
	cpu_atomic_start();
	uint32_t mmucr = MMUCR;
	int urb = gtlb_mmucr_urb(mmucr);
	int new_urb = (g_pin_lo < GTLB_SLOTS) ? gtlb_stats.urb_orig : urb;
	read_view(&v);
	int count = gtlb_plan_unpin(&v, g_table, g_slot_orig, g_ops,
		GTLB_MAX_OPS);
	if(count > 0 || new_urb != urb)
		apply(g_ops, count, gtlb_mmucr_with_urb(mmucr, new_urb));
	/* A later repair (none expected) keeps MPM's layout */
	memcpy(g_slot, g_slot_orig, sizeof g_slot);
	gtlb_track_image(g_slot, g_image_pages);
	g_pin_lo = GTLB_SLOTS;
	cpu_atomic_end();
	gtlb_stats.pinned = 0;
}

static void pin_image(void)
{
	/* Every page of the loaded image: text and rodata are used all the
	   time; the .data/.ilram/.xyram load images only at startup, but they
	   cost at most one extra slot */
	int n = g_image_pages;
	if(n <= 0 || !g_apply)
		return;

	static gtlb_view_t v;
	cpu_atomic_start();
	uint32_t mmucr = MMUCR;
	int urb = gtlb_mmucr_urb(mmucr);
	read_view(&v);
	int new_urb;
	int count = gtlb_plan_pin(&v, g_table, g_slot, n, urb, g_ops,
		GTLB_MAX_OPS, &new_urb);
	if(count > 0) {
		apply(g_ops, count, gtlb_mmucr_with_urb(mmucr, new_urb));
		g_pin_lo = new_urb;
		gtlb_stats.pinned = n;
		gtlb_stats.urb = new_urb;
	}
	cpu_atomic_end();
}

void gtlb_init(bool safe)
{
	if(!g_is_cg100)
		return;
	gtlb_stats.safe = safe;

	g_table = malloc(GTLB_PAGES * sizeof *g_table + 4);
	if(!g_table)
		return;
	memset(g_table, 0, GTLB_PAGES * sizeof *g_table + 4);
	memset(g_slot, -1, sizeof g_slot);
	gtlb_stats.misses = (uint32_t *)((uint8_t *)g_table + 512);
	gtlb_stats.urb_orig = gtlb_mmucr_urb(MMUCR);

	for(int i = 0; i < GTLB_SLOTS; i++) {
		uint32_t a = UTLB_ADDR(i);
		uint32_t d = UTLB_DATA(i);
		if(!(a & GTLB_ADDR_V) || !(d & GTLB_DATA_V))
			continue;

		uint32_t vpn = a & GTLB_ADDR_VPN;
		if(vpn < GTLB_ROM_BASE
			|| vpn >= GTLB_ROM_BASE + GTLB_PAGES * PAGE_64K)
			continue;
		if(page_size(d) != PAGE_64K || (vpn & (PAGE_64K - 1)))
			continue;

		int p = (vpn - GTLB_ROM_BASE) >> 16;
		if(g_slot[p] >= 0)
			continue;	/* duplicate: keep the first one */
		g_table[p].pteh = vpn | (a & 0xff);
		g_table[p].ptel = d & 0x1ffffdff;
		g_slot[p] = i;
		gtlb_stats.pages++;
	}
	memcpy(g_slot_orig, g_slot, sizeof g_slot);
	g_image_pages = (int)((gmem_stats.image_size + PAGE_64K - 1) >> 16);
	gtlb_track_image(g_slot, g_image_pages);

	if(gtlb_stats.pages > 0)
		install_handler();
	if(!gtlb_stats.handler)
		return;
	/* Safe start (final review): no executor, so no pinning and no
	   ldtlb from our code, a session-only escape from a pinning problem
	   on hardware without a rebuild (CGD_TLB_PIN=0). The handler stays:
	   gint's would jump to the fx-CG50 syscall gate. */
	gtlb_stats.executor = safe ? false : setup_executor();
	if(gtlb_stats.executor && CGD_TLB_PIN)
		pin_image();
	/* gint review round 3.1: keep the handler (gint's would jump to the
	   fx-CG50 syscall gate), but keep its reloads where MPM rewrites */
	if(!gtlb_stats.executor) {
		contain_urb();
		PL_Log("UTLB: no executor; URB %d%s\n", g_urb_cap ? g_urb_cap
			: gtlb_stats.urb_orig, g_urb_cap ? " (lowered)" : "");
	}
}

void gtlb_repair(void)
{
	if(!g_table || gtlb_stats.pages == 0)
		return;

	/* No executor: no safe way to write the UTLB (see apply()); the miss
	   handler still covers the gint world. Only put the URB back under
	   g_urb_cap if the OS raised it (gint review round 3.1). */
	if(!g_apply) {
		if(g_urb_cap <= 0 || !g_set_mmucr)
			return;
		gtlb_stats.checks++;
		cpu_atomic_start();
		uint32_t mmucr = MMUCR;
		bool fix = urb_above(gtlb_mmucr_urb(mmucr), g_urb_cap);
		if(fix)
			g_set_mmucr(gtlb_mmucr_with_urb(mmucr, g_urb_cap));
		cpu_atomic_end();
		if(fix) {
			gtlb_stats.urb_fixes++;
			PL_Log("UTLB: URB reset to %d\n", g_urb_cap);
		}
		return;
	}
	gtlb_stats.checks++;

	static gtlb_view_t v;
	cpu_atomic_start();
	uint32_t mmucr = MMUCR;
	int urb = gtlb_mmucr_urb(mmucr);
	read_view(&v);
	int count = gtlb_plan_repair(&v, g_table, g_slot, g_pin_lo, g_ops,
		GTLB_MAX_OPS);
	/* The OS may rewrite MMUCR; our boundary must hold while pinned */
	bool fix_urb = (g_pin_lo < GTLB_SLOTS && urb != g_pin_lo);
	if(count > 0 || fix_urb)
		apply(g_ops, count, gtlb_mmucr_with_urb(mmucr,
			fix_urb ? g_pin_lo : urb));
	cpu_atomic_end();

	/* Diagnostics (gint review round 1): repairs > 0 in the logs means the
	   OS does evict our image pages during world switches (since round 2
	   only image pages are tracked, so OS reuse of the slots of the pages
	   beyond the image no longer counts) */
	gtlb_stats.last_repairs = count;
	gtlb_stats.repairs += count;
	if(fix_urb)
		gtlb_stats.urb_fixes++;
	if(count > 0 || fix_urb)
		PL_Log("UTLB: %d repaired%s\n", count,
			fix_urb ? ", URB reset" : "");
}
