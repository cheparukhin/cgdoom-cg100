/* Host unit test for src/gint/tlbplan.c (gint review round 1: pinning the
   image pages above MMUCR.URB, and the repair after world switches).

   A simulated UTLB starts in MPM's layout: slots 0..54 map the 64 KiB pages
   of 0x00300000.. (page i in slot i), 55..62 the user RAM at 0x08100000,
   63 the NULL page; URB = 54. The plans are executed by an independent
   model of cgd_tlb_apply() (tlbh.S), step by step, checking after every
   single ldtlb that no two valid entries map the same 64 KiB page (a UTLB
   multiple hit resets the CPU). Random "OS" activity (URC-based loads of
   foreign 4 KiB pages, explicit overwrites) and miss-handler reloads are
   then mixed with repairs.

   gint review round 2: the three mpm.bin layouts (v1.1 and dynload map 55
   ROM pages, Feb-2025 only 48 and leaves slots 48..54 alone), image-only
   repair tracking, and the exit normalisation (gtlb_plan_unpin) followed
   by a replay of MPM's next launch, which must find no duplicate VPN. */

#include "cgd_gint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

#define PHYS_ROM   0x0c400000u
#define PHYS_URAM  0x0c780000u
#define PTEL_64K   (0x100u | 0x80u | 0x60u | 0x08u | 0x04u)  /* V SZ=64K PR C D */
#define PTEL_4K    (0x100u | 0x10u | 0x60u | 0x08u | 0x04u)

static gtlb_view_t hw;      /* the "hardware" */
static int urb;             /* simulated MMUCR.URB */
static int urc;             /* simulated MMUCR.URC */

static void hw_load(int slot, uint32_t pteh, uint32_t ptel)
{
	hw.addr[slot] = (pteh & (GTLB_ADDR_VPN | 0xff)) | ((ptel & GTLB_DATA_V) ? GTLB_ADDR_V : 0);
	hw.data[slot] = ptel;
}

/* mpm.bin variants (ref/mpm history) */
enum { MPM_V11, MPM_DYNLOAD, MPM_FEB2025, MPM_KINDS };
static char const *mpm_name[MPM_KINDS] = { "v1.1", "dynload", "Feb-2025" };
static int mpm_rom_pages(int kind) { return kind == MPM_FEB2025 ? 48 : 55; }

/* MPM's load_addin(): MMU_Map of the ROM pages (page i in slot i), the
   user RAM (55..62) and the NULL page (63). v1.1 flushes the TLB first
   (invalidateTLB, MMUCR.TI); the others write over whatever is there. */
static void mpm_launch(int kind)
{
	if(kind == MPM_V11)
		memset(&hw, 0, sizeof hw);
	for(int i = 0; i < mpm_rom_pages(kind); i++)
		hw_load(i, GTLB_ROM_BASE + (i << 16), (PHYS_ROM + (i << 16)) | PTEL_64K);
	for(int i = 0; i < 8; i++)
		hw_load(55 + i, 0x08100000u + (i << 16), (PHYS_URAM + (i << 16)) | PTEL_64K);
	hw_load(63, 0, 0x00000000u | PTEL_4K);
	urb = 54;
	urc = 0;
}

static void mpm_layout_kind(int kind)
{
	memset(&hw, 0, sizeof hw);
	mpm_launch(kind);
}
static void mpm_layout(void) { mpm_layout_kind(MPM_V11); }

static uint32_t entry_size(uint32_t data)
{
	static uint32_t const sizes[4] = { 0x400, 0x1000, 0x10000, 0x100000 };
	return sizes[(((data >> 7) & 1) << 1) | ((data >> 4) & 1)];
}

/* No two valid entries whose address ranges overlap (multiple hit) */
static bool no_duplicates(gtlb_view_t const *v)
{
	for(int i = 0; i < GTLB_SLOTS; i++) {
		if(!(v->addr[i] & GTLB_ADDR_V)) continue;
		uint32_t a0 = v->addr[i] & GTLB_ADDR_VPN, a1 = a0 + entry_size(v->data[i]);
		for(int j = i + 1; j < GTLB_SLOTS; j++) {
			if(!(v->addr[j] & GTLB_ADDR_V)) continue;
			uint32_t b0 = v->addr[j] & GTLB_ADDR_VPN, b1 = b0 + entry_size(v->data[j]);
			if(a0 < b1 && b0 < a1)
				return false;
		}
	}
	return true;
}

/* Step-by-step model of cgd_tlb_apply() (independent of gtlb_view_apply) */
static int exec_ldtlb_count;
static void exec_ops(gtlb_op_t const *ops, int n, int final_urb)
{
	for(int k = 0; k < n; k++) {
		gtlb_op_t const *op = &ops[k];
		if(op->ptel & GTLB_DATA_V) {
			uint32_t vpn = op->pteh & GTLB_VPN64;
			for(int i = 63; i >= 0; i--) {
				if((int)op->slot == i) continue;
				uint32_t a = hw.addr[i];
				if(!(a & GTLB_ADDR_V) || (a & GTLB_VPN64) != vpn) continue;
				hw_load(i, vpn, 0);     /* LOADTLB i, vpn, 0 */
				exec_ldtlb_count++;
				CHECK(no_duplicates(&hw), "duplicate after invalidation");
			}
		}
		hw_load((int)op->slot, op->pteh, op->ptel);
		exec_ldtlb_count++;
		CHECK(no_duplicates(&hw), "duplicate after loading slot %u", op->slot);
	}
	urb = final_urb;
	urc = 0;
}

static gtlb_entry_t table[GTLB_PAGES];
static int8_t slot[GTLB_PAGES];
static int8_t slot_orig[GTLB_PAGES];

/* gtlb_init()'s snapshot, then tracking restricted to image pages 0..n-1
   (n <= 0: all) */
static int snapshot_img(int n)
{
	int pages = 0;
	memset(table, 0, sizeof table);
	memset(slot, -1, sizeof slot);
	for(int i = 0; i < GTLB_SLOTS; i++) {
		uint32_t a = hw.addr[i], d = hw.data[i];
		if(!(a & GTLB_ADDR_V) || !(d & GTLB_DATA_V)) continue;
		uint32_t vpn = a & GTLB_ADDR_VPN;
		if(vpn < GTLB_ROM_BASE || vpn >= GTLB_ROM_BASE + (GTLB_PAGES << 16)) continue;
		int p = (vpn - GTLB_ROM_BASE) >> 16;
		if(slot[p] >= 0) continue;
		table[p].pteh = vpn | (a & 0xff);
		table[p].ptel = d & 0x1ffffdff;
		slot[p] = (int8_t)i;
		pages++;
	}
	memcpy(slot_orig, slot, sizeof slot);
	gtlb_track_image(slot, n);
	return pages;
}
static int snapshot(void) { return snapshot_img(0); }

/* Slot holding page p (valid, right PPN), -1 if none, -2 if several */
static int where(int p)
{
	int found = -1;
	for(int i = 0; i < GTLB_SLOTS; i++) {
		if(!(hw.addr[i] & GTLB_ADDR_V)) continue;
		if((hw.addr[i] & GTLB_VPN64) != GTLB_ROM_BASE + ((uint32_t)p << 16)) continue;
		if((hw.data[i] & GTLB_DATA_PPN) != (table[p].ptel & GTLB_DATA_PPN)) return -3;
		found = (found == -1) ? i : -2;
	}
	return found;
}

static gtlb_op_t ops[GTLB_MAX_OPS];

static int do_pin(int n)
{
	gtlb_view_t v = hw;
	int new_urb;
	int count = gtlb_plan_pin(&v, table, slot, n, urb, ops, GTLB_MAX_OPS, &new_urb);
	if(count > 0) {
		exec_ops(ops, count, new_urb);
		CHECK(!memcmp(&v, &hw, sizeof hw), "planner view == executed state (pin)");
	}
	return count;
}

static int do_repair(int pin_lo)
{
	gtlb_view_t v = hw;
	int count = gtlb_plan_repair(&v, table, slot, pin_lo, ops, GTLB_MAX_OPS);
	exec_ops(ops, count, urb);
	CHECK(!memcmp(&v, &hw, sizeof hw), "planner view == executed state (repair)");
	return count;
}

static void test_pin_basic(void)
{
	mpm_layout();
	int n = 5;   /* ~314 KB image */
	CHECK(snapshot_img(n) == 55, "55 pages in the snapshot");
	int count = do_pin(n);
	CHECK(count == n, "pin ops %d", count);
	CHECK(urb == 49, "URB lowered to %d", urb);
	for(int p = 0; p < n; p++) {
		CHECK(where(p) == 49 + p, "page %d in slot %d", p, where(p));
		CHECK(slot[p] == 49 + p, "slot[%d] = %d", p, slot[p]);
		CHECK(!(hw.addr[p] & GTLB_ADDR_V), "old slot %d invalid", p);
	}
	for(int q = 49; q <= 53; q++)
		CHECK(slot[q] == -1 && where(q) == -1, "displaced page %d", q);
	for(int q = 5; q < 49; q++)
		CHECK(where(q) == q && slot[q] == -1, "page %d untouched, not tracked", q);
	CHECK(where(54) == 54 && slot[54] == -1, "slot 54 (MPM-protected) untouched");
	for(int i = 55; i < 64; i++)
		CHECK(hw.addr[i] & GTLB_ADDR_V, "MPM slot %d kept", i);
	/* Nothing to repair right after pinning */
	CHECK(do_repair(urb) == 0, "repair after pin is empty");
}

static void test_pin_refusals(void)
{
	int new_urb;
	gtlb_view_t before;

	mpm_layout(); snapshot();
	before = hw;
	gtlb_view_t v = hw;
	int8_t s0[GTLB_PAGES]; memcpy(s0, slot, sizeof s0);
	CHECK(gtlb_plan_pin(&v, table, slot, 0, urb, ops, GTLB_MAX_OPS, &new_urb) == 0, "n=0");
	CHECK(gtlb_plan_pin(&v, table, slot, 39, urb, ops, GTLB_MAX_OPS, &new_urb) == 0
		&& new_urb == urb, "too many pages (leaves < %d replaceable)", GTLB_MIN_REPLACEABLE);
	CHECK(gtlb_plan_pin(&v, table, slot, 38, urb, ops, GTLB_MAX_OPS, &new_urb) == 0,
		"38 pages: targets 16..53 overlap the sources");
	CHECK(!memcmp(&v, &before, sizeof v) && !memcmp(s0, slot, sizeof s0), "refusal changes nothing");

	/* An image page missing from the snapshot */
	table[2].ptel = 0;
	CHECK(gtlb_plan_pin(&v, table, slot, 5, urb, ops, GTLB_MAX_OPS, &new_urb) == 0, "missing page");
	snapshot();

	/* A target slot holding an image page (unexpected layout) */
	hw_load(2, GTLB_ROM_BASE + (52 << 16), (PHYS_ROM + (52 << 16)) | PTEL_64K);
	hw_load(52, GTLB_ROM_BASE + (2 << 16), (PHYS_ROM + (2 << 16)) | PTEL_64K);
	snapshot();
	v = hw;
	CHECK(gtlb_plan_pin(&v, table, slot, 5, urb, ops, GTLB_MAX_OPS, &new_urb) == 0, "source in target range");

	/* Foreign OS entries in the target range are simply replaced */
	mpm_layout(); snapshot_img(5);
	hw_load(50, 0x00112000u, 0x0c112000u | PTEL_4K);
	CHECK(do_pin(5) == 5 && where(1) == 50, "OS entry in target slot replaced");
}

/* The OS evicts an image page from the replaceable range (impossible once
   pinned, but our handler and the OS still use slots < URB) */
static void test_repair_cases(void)
{
	mpm_layout(); snapshot_img(5);
	do_pin(5);
	int lo = urb;

	/* OS overwrote a pinned slot explicitly; the miss handler then
	   reloaded the page at URC */
	hw_load(51, 0x00112000u, 0x0c112000u | PTEL_4K);
	CHECK(where(2) == -1, "page 2 evicted");
	hw_load(7, table[2].pteh, table[2].ptel);   /* handler at URC=7 */
	CHECK(do_repair(lo) >= 1, "repair moves page 2 back");
	CHECK(where(2) == 51, "page 2 back in its pinned slot (%d)", where(2));

	/* Round 2: the OS reusing the slot of a page beyond the image is not
	   a repair: its entry is left alone (page 30 reloads on demand) */
	hw_load(30, 0x00114000u, 0x0c114000u | PTEL_4K);
	CHECK(do_repair(lo) == 0, "OS entry over non-image page 30: no repair");
	CHECK((hw.addr[30] & GTLB_ADDR_VPN) == 0x00114000u && where(30) == -1,
		"OS entry kept in slot 30");
	/* ... nor is the miss handler reloading it elsewhere */
	hw_load(7, table[30].pteh, table[30].ptel);
	CHECK(do_repair(lo) == 0 && where(30) == 7, "handler reload of page 30 left");

	/* A foreign 4 KiB entry overlapping a pinned page is dropped when the
	   page is reloaded (no multiple hit) */
	hw_load(52, 0x00115000u, 0x0c115000u | PTEL_4K);   /* evict page 3 */
	hw_load(12, GTLB_ROM_BASE + (3 << 16) + 0x2000, 0x0c999000u | PTEL_4K);
	do_repair(lo);
	CHECK(where(3) == 52, "page 3 back (%d)", where(3));
	CHECK(no_duplicates(&hw), "no overlap left");

	/* Unpinned image (pinning refused or disabled): a 45-page image keeps
	   the pre-pinning behaviour for its pages, pin_lo = 64 */
	mpm_layout(); snapshot_img(45);
	lo = GTLB_SLOTS;

	/* Unpinned page evicted and reloaded elsewhere by the handler: left
	   there */
	hw_load(10, 0x00113000u, 0x0c113000u | PTEL_4K);
	hw_load(50, table[10].pteh, table[10].ptel);
	CHECK(do_repair(lo) == 0 && where(10) == 50, "unpinned reload left in place");

	/* ... but if the handler put it in another page's slot and that page
	   is missing, restoring that page drops it: it comes back in its own
	   slot within the same repair */
	hw_load(21, 0x00117000u, 0x0c117000u | PTEL_4K);   /* evict page 21 */
	hw_load(22, table[21].pteh, table[21].ptel);       /* reload in 22 */
	hw_load(11, 0x00118000u, 0x0c118000u | PTEL_4K);   /* evict page 11 */
	hw_load(21, table[11].pteh, table[11].ptel);       /* reload in 21 */
	CHECK(where(22) == -1, "page 22 evicted by the reload");
	do_repair(lo);
	CHECK(where(11) >= 0 && where(21) >= 0 && where(22) == 22,
		"cascade repaired: 11@%d 21@%d 22@%d", where(11), where(21), where(22));
	CHECK(do_repair(lo) == 0, "cascade: second repair is empty");

	/* Unpinned image page missing: written back to its slot */
	hw_load(30, 0x00114000u, 0x0c114000u | PTEL_4K);
	CHECK(do_repair(lo) == 1 && where(30) == 30, "unpinned page restored");
	/* Beyond the image (page 48): not restored */
	hw_load(48, 0x0011a000u, 0x0c11a000u | PTEL_4K);
	CHECK(do_repair(lo) == 0 && where(48) == -1, "non-image page 48 not restored");

	/* A foreign entry inside an unpinned page is not a copy */
	hw_load(40, 0x00119000u, 0x0c119000u | PTEL_4K);   /* evict page 40 */
	hw_load(2, GTLB_ROM_BASE + (40 << 16) + 0x1000, 0x0c998000u | PTEL_4K);
	do_repair(lo);
	CHECK(where(40) == 40 && no_duplicates(&hw), "page 40 back, overlap dropped");

	/* Without pinning (pin_lo = 64), small image */
	mpm_layout(); snapshot_img(5);
	hw_load(4, 0x00116000u, 0x0c116000u | PTEL_4K);
	CHECK(do_repair(GTLB_SLOTS) == 1 && where(4) == 4, "unpinned repair");

	/* Image size unknown (n = 0): every snapshot page tracked */
	mpm_layout(); snapshot();
	hw_load(30, 0x00114000u, 0x0c114000u | PTEL_4K);
	CHECK(do_repair(GTLB_SLOTS) == 1 && where(30) == 30, "untracked size: all repaired");
}

/* Exit: gtlb_quit() (plan_unpin + URB restore), as tlb.c runs it */
static int do_unpin(int urb_orig)
{
	gtlb_view_t v = hw;
	int count = gtlb_plan_unpin(&v, table, slot_orig, ops, GTLB_MAX_OPS);
	exec_ops(ops, count, urb_orig);
	CHECK(!memcmp(&v, &hw, sizeof hw), "planner view == executed state (unpin)");
	memcpy(slot, slot_orig, sizeof slot);
	return count;
}

/* After the exit normalisation: every snapshot page alone in its original
   slot, nothing else in the ROM range */
static void check_normalised(char const *what)
{
	for(int p = 0; p < GTLB_PAGES; p++) {
		if(slot_orig[p] >= 0)
			CHECK(where(p) == slot_orig[p], "%s: page %d at %d, not %d",
				what, p, where(p), slot_orig[p]);
		else
			CHECK(where(p) == -1, "%s: stray page %d at %d", what, p, where(p));
	}
	for(int i = 0; i < GTLB_SLOTS; i++) {
		if(!(hw.addr[i] & GTLB_ADDR_V)) continue;
		uint32_t a = hw.addr[i] & GTLB_ADDR_VPN;
		if(a >= GTLB_ROM_BASE && a < GTLB_ROM_BASE + (GTLB_PAGES << 16)) {
			int p = (int)((a - GTLB_ROM_BASE) >> 16);
			CHECK(slot_orig[p] == i, "%s: slot %d maps page %d", what, i, p);
		}
	}
}

/* Round 2: CGDoom pins, exits, and MPM launches the next add-in */
static void test_relaunch(void)
{
	for(int kind = 0; kind < MPM_KINDS; kind++) {
		/* What round 1 did (URB restore only) breaks Feb-2025 */
		mpm_layout_kind(kind); snapshot_img(5);
		CHECK(do_pin(5) == 5 && urb == 49, "%s: pinned", mpm_name[kind]);
		exec_ops(ops, 0, 54);
		mpm_launch(kind);
		bool dup = !no_duplicates(&hw);
		CHECK(dup == (kind == MPM_FEB2025), "%s: URB-only exit %s",
			mpm_name[kind], dup ? "duplicates (reset)" : "clean");

		/* Round 2: unpin at exit, then the next launch is clean */
		mpm_layout_kind(kind); snapshot_img(5);
		do_pin(5);
		/* plus an OS entry over an untracked page and a 4 KiB foreign
		   entry inside a ROM page, and a handler reload of page 20 */
		hw_load(20, 0x00114000u, 0x0c114000u | PTEL_4K);
		hw_load(9, GTLB_ROM_BASE + (40 << 16) + 0x3000, 0x0c997000u | PTEL_4K);
		hw_load(40, 0x00121000u, 0x0c121000u | PTEL_4K);
		hw_load(48, table[20].pteh, table[20].ptel);
		CHECK(no_duplicates(&hw), "%s: setup", mpm_name[kind]);
		int n = do_unpin(54);
		CHECK(n > 0 && urb == 54, "%s: unpin %d ops, URB %d", mpm_name[kind], n, urb);
		check_normalised(mpm_name[kind]);
		CHECK(do_unpin(54) == 0, "%s: unpin is idempotent", mpm_name[kind]);
		mpm_launch(kind);
		CHECK(no_duplicates(&hw), "%s: next launch after unpin has a duplicate",
			mpm_name[kind]);

		/* Unpinned run: handler reloads into 48..53 (Feb-2025: survive) */
		mpm_layout_kind(kind); snapshot_img(5);
		hw_load(2, 0x00122000u, 0x0c122000u | PTEL_4K);
		hw_load(50, table[2].pteh, table[2].ptel);
		CHECK(do_repair(GTLB_SLOTS) == 0, "%s: handler copy left", mpm_name[kind]);
		do_unpin(54);
		check_normalised(mpm_name[kind]);
		mpm_launch(kind);
		CHECK(no_duplicates(&hw), "%s: unpinned run, next launch", mpm_name[kind]);
	}
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* Random OS activity, handler reloads and repairs */
static void test_fuzz(void)
{
	int total_ops = 0;
	for(int round = 0; round < 200; round++) {
		int kind = round % MPM_KINDS;
		mpm_layout_kind(kind);
		int n = 1 + (int)(rnd() % 20);
		snapshot_img(n);
		bool pinned = (rnd() % 4) != 0;
		int lo = GTLB_SLOTS;
		if(pinned) {
			CHECK(do_pin(n) == n, "pin %d", n);
			lo = urb;
		}
		for(int step = 0; step < 300; step++) {
			int ev = (int)(rnd() % 10);
			if(ev < 5) {
				/* OS: URC-based load of a foreign 4 KiB page */
				uint32_t vpn = 0x00100000u + (rnd() % 0x100) * 0x1000;
				/* the OS only maps what is not mapped (no multiple hit) */
				bool clash = false;
				for(int i = 0; i < 64; i++)
					if((hw.addr[i] & GTLB_ADDR_V) && (hw.addr[i] & GTLB_ADDR_VPN) == vpn) clash = true;
				if(!clash) hw_load(urc, vpn, (0x0c000000u | vpn) | PTEL_4K);
				urc = (urc + 1) % urb;
			}
			else if(ev < 8) {
				/* Our miss handler: a missing page we know, loaded at URC */
				int p = (int)(rnd() % GTLB_PAGES);
				if(table[p].ptel && where(p) == -1) {
					hw_load(urc, table[p].pteh, table[p].ptel);
					urc = (urc + 1) % urb;
				}
			}
			else if(ev < 9) {
				/* Rare: the OS writes a slot explicitly (even pinned ones) */
				int s = (int)(rnd() % 55);
				uint32_t vpn = 0x00200000u + (rnd() % 0x100) * 0x1000;
				bool clash = false;
				for(int i = 0; i < 64; i++)
					if((hw.addr[i] & GTLB_ADDR_V) && (hw.addr[i] & GTLB_ADDR_VPN) == vpn) clash = true;
				if(!clash) hw_load(s, vpn, (0x0c000000u | vpn) | PTEL_4K);
			}
			else {
				total_ops += do_repair(lo);
				CHECK(no_duplicates(&hw), "fuzz: duplicates");
				for(int p = 0; p < n && pinned; p++)
					CHECK(where(p) == lo + p, "fuzz: pinned page %d at %d", p, where(p));
				for(int p = 0; p < GTLB_PAGES; p++)
					if(slot[p] >= 0) CHECK(where(p) >= 0, "fuzz: page %d mapped", p);
				CHECK(do_repair(lo) == 0, "fuzz: repair is idempotent");
			}
			if(pinned) {
				/* Replacement never selects pinned slots */
				CHECK(urc < lo, "URC %d below URB %d", urc, lo);
			}
		}
		/* Exit and the next launch (round 2) */
		do_unpin(54);
		CHECK(no_duplicates(&hw), "fuzz: duplicates after unpin");
		check_normalised("fuzz");
		mpm_launch(kind);
		CHECK(no_duplicates(&hw), "fuzz: %s next launch duplicates", mpm_name[kind]);
	}
	printf("fuzz: %d repair ops, %d ldtlb executed\n", total_ops, exec_ldtlb_count);
}

static void test_mmucr(void)
{
	/* AT=1, SV=1, URB=54, URC=17, SQMD=1, TI bit set in the "read" */
	uint32_t m = 1u | (1u << 8) | (54u << 18) | (17u << 10) | (1u << 9) | (1u << 2);
	uint32_t r = gtlb_mmucr_with_urb(m, 49);
	CHECK(gtlb_mmucr_urb(r) == 49, "URB field");
	CHECK(((r >> 10) & 63) == 0, "URC cleared");
	CHECK(!(r & 4), "TI never written");
	CHECK((r & ~((63u << 18) | (63u << 10) | 4u)) == (m & ~((63u << 18) | (63u << 10) | 4u)),
		"other bits kept");
}

int main(void)
{
	test_pin_basic();
	test_pin_refusals();
	test_repair_cases();
	test_relaunch();
	test_fuzz();
	test_mmucr();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
