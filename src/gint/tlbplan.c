/* CGDoom gint platform: UTLB layout planning (fx-CG100 / MPM).

   Pure logic, no hardware access: tlb.c reads the UTLB into a gtlb_view_t,
   asks for a list of gtlb_op_t, and runs them with cgd_tlb_apply()
   (tlbh.S) from its P2 alias. The host tests (tests/gint-host) run the
   same functions against a simulated UTLB and check that no step ever
   leaves two valid entries for one page.

   Why pinning: gint switches VBR back to the OS in
   gint_world_switch_out() and to ours only in gint_world_switch_in(), so
   the P0 code that runs in between (our os_* callbacks between OS calls,
   gint_call, the on-chip memory restore, the canary check, the funbind
   loop) takes TLB misses through the OS's handler, not ours. If the OS
   replaces one of our entries with LDTLB while we are there, the add-in
   dies. LDTLB with MMUCR.URC only ever selects slots below MMUCR.URB, so
   once our image pages sit at or above URB, the OS cannot evict them. */

#include "cgd_gint.h"

static bool slot_valid(gtlb_view_t const *v, int s)
{
	return (v->addr[s] & GTLB_ADDR_V) && (v->data[s] & GTLB_DATA_V);
}

/* 64 KiB page index of the valid entry in slot s, or -1 */
static int slot_page(gtlb_view_t const *v, int s)
{
	if(!slot_valid(v, s))
		return -1;
	uint32_t vpn = v->addr[s] & GTLB_VPN64;
	if(vpn < GTLB_ROM_BASE || vpn >= GTLB_ROM_BASE + (GTLB_PAGES << 16))
		return -1;
	return (vpn - GTLB_ROM_BASE) >> 16;
}

static bool slot_holds(gtlb_view_t const *v, int s, gtlb_entry_t const *e)
{
	return slot_valid(v, s)
		&& (v->addr[s] & GTLB_ADDR_VPN) == (e->pteh & GTLB_ADDR_VPN)
		&& (v->data[s] & GTLB_DATA_PPN) == (e->ptel & GTLB_DATA_PPN);
}

void gtlb_view_apply(gtlb_view_t *v, gtlb_op_t const *op)
{
	int t = (int)(op->slot & 63);

	/* Executor: drop every other valid entry of the same 64 KiB page */
	if(op->ptel & GTLB_DATA_V) {
		uint32_t vpn = op->pteh & GTLB_VPN64;
		for(int s = GTLB_SLOTS - 1; s >= 0; s--) {
			if(s != t && (v->addr[s] & GTLB_ADDR_V)
				&& (v->addr[s] & GTLB_VPN64) == vpn) {
				v->addr[s] = vpn;
				v->data[s] = 0;
			}
		}
	}

	/* ldtlb: VPN/ASID from PTEH, everything else from PTEL. The V bit is
	   shared by both arrays. */
	v->addr[t] = (op->pteh & (GTLB_ADDR_VPN | 0xffu))
		| ((op->ptel & GTLB_DATA_V) ? GTLB_ADDR_V : 0);
	v->data[t] = op->ptel;
}

static void emit(gtlb_view_t *v, gtlb_op_t *ops, int *n, int slot,
	gtlb_entry_t const *e)
{
	ops[*n] = (gtlb_op_t){ (uint32_t)slot, e->pteh, e->ptel };
	gtlb_view_apply(v, &ops[*n]);
	(*n)++;
}

int gtlb_plan_pin(gtlb_view_t *v, gtlb_entry_t const *table, int8_t *slot,
	int n, int urb, gtlb_op_t *ops, int max_ops, int *new_urb)
{
	*new_urb = urb;
	int lo = urb - n;
	if(n <= 0 || n > GTLB_PAGES || urb > GTLB_SLOTS || n > max_ops)
		return 0;
	/* Leave the OS enough replaceable slots */
	if(lo < GTLB_MIN_REPLACEABLE)
		return 0;
	/* Every image page must be known (we cannot pin what we cannot load) */
	for(int p = 0; p < n; p++) {
		if(!(table[p].ptel & GTLB_DATA_V))
			return 0;
	}
	/* The target slots must not hold an image page: with MPM's layout
	   (page i in slot i) they hold pages beyond the image, which nothing
	   uses after startup, or whatever the OS loaded there. Anything else
	   (e.g. a source slot inside the target range) is unexpected: give
	   up rather than risk a half-moved layout. */
	for(int t = lo; t < urb; t++) {
		int q = slot_page(v, t);
		if(q >= 0 && q < n)
			return 0;
	}

	int count = 0;
	for(int p = 0; p < n; p++) {
		int t = lo + p;
		/* A snapshot page displaced from t is reloaded on demand */
		for(int q = 0; q < GTLB_PAGES; q++) {
			if(slot[q] == t)
				slot[q] = -1;
		}
		/* The executor invalidates the old slot of page p first */
		emit(v, ops, &count, t, &table[p]);
		slot[p] = (int8_t)t;
	}
	*new_urb = lo;
	return count;
}

int gtlb_plan_repair(gtlb_view_t *v, gtlb_entry_t const *table,
	int8_t const *slot, int pin_lo, gtlb_op_t *ops, int max_ops)
{
	int count = 0;

	/* A load into page p's slot drops whatever the slot held, which can
	   be a copy of an earlier page that the handler had put there: run
	   the pass again until nothing changes. Every page has its own slot,
	   so the second pass only restores such pages and the third finds
	   nothing (the bound is a safety net). */
	for(int pass = 0; pass < 4; pass++) {
		int before = count;
		for(int p = 0; p < GTLB_PAGES && count < max_ops; p++) {
			int s = slot[p];
			if(s < 0)
				continue;
			gtlb_entry_t const *e = &table[p];
			if(slot_holds(v, s, e))
				continue;

			/* A real copy of the page elsewhere (same VPN and PPN; a
			   foreign entry overlapping the page does not count, the
			   load below drops it) */
			bool elsewhere = false;
			for(int i = 0; i < GTLB_SLOTS && !elsewhere; i++)
				elsewhere = (i != s && slot_holds(v, i, e));

			/* Unpinned page reloaded elsewhere by the miss handler:
			   leave it there (moving it would only evict something) */
			if(elsewhere && s < pin_lo)
				continue;
			/* Missing, or a pinned page that is not in its slot */
			emit(v, ops, &count, s, e);
		}
		if(count == before)
			break;
	}
	return count;
}

void gtlb_track_image(int8_t *slot, int n)
{
	/* The snapshot also holds the pages MPM maps beyond the image (55
	   under mpm.bin 1.1). Nothing executes there, so keeping them in
	   slot[] would make every repair overwrite the OS's fresh entries in
	   those slots with useless mappings, and drown real image-page
	   repairs in the counter. Their
	   table[] entries stay: the miss handler still reloads them on demand,
	   and gtlb_plan_unpin() puts them back at exit. */
	if(n <= 0)
		return;	/* image size unknown: track everything */
	for(int p = n; p < GTLB_PAGES; p++)
		slot[p] = -1;
}

int gtlb_plan_unpin(gtlb_view_t *v, gtlb_entry_t const *table,
	int8_t const *slot_orig, gtlb_op_t *ops, int max_ops)
{
	/* At exit, put every snapshot page back in the slot MPM loaded it
	   into, and nowhere else (defence in depth: mpm.bin 1.1 flushes the
	   TLB and rewrites MMUCR before the next launch anyway). A loader
	   that rewrites only some slots and never sets MMUCR.TI would
	   otherwise find a copy of a ROM page left elsewhere (a pinned slot,
	   or a miss-handler reload) duplicating the VPN it maps again: UTLB
	   multiple hit, CPU reset.
	   Loading page p into its original slot makes the executor drop every
	   other entry of that 64 KiB page (pinned copy, handler copy, foreign
	   overlapping entry). Pages already alone in their slot are skipped. */
	int count = 0;
	for(int p = 0; p < GTLB_PAGES && count < max_ops; p++) {
		int s = slot_orig[p];
		gtlb_entry_t const *e = &table[p];
		if(s < 0 || !(e->ptel & GTLB_DATA_V))
			continue;
		bool other = false;
		for(int i = 0; i < GTLB_SLOTS && !other; i++) {
			other = (i != s && (v->addr[i] & GTLB_ADDR_V)
				&& (v->addr[i] & GTLB_VPN64) == (e->pteh & GTLB_VPN64));
		}
		if(!other && slot_holds(v, s, e))
			continue;
		emit(v, ops, &count, s, e);
	}
	return count;
}

uint32_t gtlb_mmucr_with_urb(uint32_t mmucr, int urb)
{
	/* URB = bits 23..18, URC = bits 15..10, TI = bit 2 (write 1 flushes
	   the whole TLB: never set it) */
	mmucr &= ~((63u << 18) | (63u << 10) | (1u << 2));
	return mmucr | ((uint32_t)(urb & 63) << 18);
}
