/* CGDoom gint platform: heap discovery and zones (PL_GetZones).

   fx-CG100: three mpm.bin variants exist in the wild and they give gint very
   different heaps (notes/research-critique.md §B3):

   | variant          | detection                               | free RAM      |
   |------------------|-----------------------------------------|---------------|
   | v1.1 (git main)  | _ld1 exists, _ld1.end == phys(0x300000) | _ld1 ~5.5 MiB |
   | Feb-2025 zip     | _ld1.end == 0x8c400000 == phys(0x300000)| _ld1 2 MiB +  |
   |                  |                                         | [code end, 0x8c700000) unannounced |
   | dynload          | no _ld1 (no 'MPM0' loadinfo), user RAM  | [0x8c200000, phys(0x300000)) |
   |  (gpSP, KhiCAS)  | at 0x8c780000                           | unannounced   |
   | pre-Feb-2025     | as dynload, but code at a fixed         | [0x8c200000, 0x8c400000) and  |
   |  (<= a1a2cc3)    | 0x8c400000 (48 pages mapped)            | [code end, 0x8c700000)        |

   Unannounced regions become private kmalloc arenas ("cgd_ram", and
   "cgd_ram2" for the second range of pre-Feb-2025 builds). Nothing
   is ever placed above 0x8c700000 (old mpm.bin is resident there) and
   nothing touches 0x8c800000+ (cold boot reported by gpSP). The OS heap
   arena "_os" is removed so that exhausted mallocs return NULL instead of
   calling OS functions. No RAM address is used without one of these
   detections; RAM is not assumed to be zeroed.

   fx-CG50: gint's _uram + _ostk (+ the OS heap, which works there through
   syscalls), plus CGDoom's opt-in "experimental RAM" past the 2 MB line on
   OS <= 3.60 (CG50 only).

   PL_GetZones() is called once by D_DoomMain(): the platform's own buffers
   (frame buffer, WAD image, tables, private stack) have been allocated by
   then. It keeps a malloc reserve for the engine and gives the largest
   blocks of every arena to the zone allocator. */

#include "cgd_gint.h"

#include <gint/kmalloc.h>
#include <gint/mmu.h>
#include <gint/hardware.h>
#include <gint/defs/util.h>
#include <stdlib.h>
#include <string.h>

gmem_stats_t gmem_stats;

static kmalloc_arena_t g_priv_arena, g_priv_arena2;
static uintptr_t g_extra_base = 0;
static int g_extra_size = 0;
/* The extra RAM was handed to the engine as a zone (PL_GetZones) and must
   be zeroed again before we exit (gmem_release_extra_ram) */
static bool g_extra_used = false;

#ifdef CGD_HOST_TEST
/* tests/gint-host: the uncached view of the experimental RAM */
uint8_t *cgd_host_extra_p2;
#define EXTRA_P2 ((uintptr_t)cgd_host_extra_p2)
#else
#define EXTRA_P2 0xac200000u
#endif

/* Arena names considered for zones / statistics. "_ld1" is used for every
   loader arena by gint (kernel.c never increments its counter). */
static char const * const g_zone_arenas[] = {
	"_ld1", "cgd_ram", "cgd_ram2", "_ostk", "_uram",
};
#define ZONE_ARENA_COUNT (int)(sizeof g_zone_arenas / sizeof *g_zone_arenas)

static char const * const g_diag_arenas[] = {
	"_uram", "_ostk", "_ld1", "cgd_ram", "cgd_ram2", "_os",
};

#ifdef CGD_HOST_TEST
/* tests/gint-host: the image size is set by the test */
uint32_t cgd_host_image_size;
static uint32_t image_size(void)
{
	return cgd_host_image_size;
}
#else
/* Linker symbols (gint fxcg50.ld): load addresses and sizes of the sections
   copied from the image. Their maximum end is the end of the binary. */
extern char srom[], ldata[], sdata[], lilram[], silram[], lxyram[], sxyram[];

static uint32_t image_size(void)
{
	uintptr_t end = 0x00300000u + (uintptr_t)srom;
	uintptr_t e;

	e = (uintptr_t)ldata + (uintptr_t)sdata;
	if(e > end) end = e;
	e = (uintptr_t)lilram + (uintptr_t)silram;
	if(e > end) end = e;
	e = (uintptr_t)lxyram + (uintptr_t)sxyram;
	if(e > end) end = e;

	return end - 0x00300000u;
}
#endif

static uintptr_t p1(uint32_t phys)
{
	return (phys & 0x1fffffffu) | 0x80000000u;
}

static bool add_private_arena(kmalloc_arena_t *a, char const *name,
	uintptr_t start, uintptr_t end)
{
	start = (start + 31) & ~(uintptr_t)31;
	end &= ~(uintptr_t)31;

	/* Hard limits, whatever the detection said */
	if(start < CGD_RAM_LOW_LIMIT || end > CGD_RAM_OLD_MPM_LIMIT)
		return false;
	if(end <= start || end - start < (64 << 10))
		return false;

	memset(a, 0, sizeof *a);
	a->name = name;
	a->is_default = true;
	a->start = (void *)start;
	a->end = (void *)end;
	kmalloc_init_arena(a, true);
	return kmalloc_add_arena(a);
}

void gmem_init(void)
{
	gmem_stats.image_size = image_size();
	gmem_stats.rom_phys = mmu_translate(0x00300000, NULL);
	gmem_stats.uram_phys = mmu_translate(0x08100000, NULL);

	if(!g_is_cg100) {
		gmem_stats.mpm_variant = GMPM_NONE;
		return;
	}

	/* No OS heap on the fx-CG100: gint's fallback would call OS malloc
	   (and on unsupported OS versions jump to the CG50 syscall gate). */
	kmalloc_arena_t *os = kmalloc_get_arena("_os");
	if(os) {
		kmalloc_remove_arena(os);
		gmem_stats.os_heap_removed = true;
	}

	if(gmem_stats.rom_phys == (uint32_t)-1) {
		gmem_stats.mpm_variant = GMPM_UNKNOWN;
		return;
	}

	uintptr_t rom = p1(gmem_stats.rom_phys);
	uintptr_t uram = (gmem_stats.uram_phys == (uint32_t)-1) ? 0 :
		p1(gmem_stats.uram_phys);
	kmalloc_arena_t *ld1 = kmalloc_get_arena("_ld1");
	uint32_t img64 = (gmem_stats.image_size + 0xffff) & ~0xffffu;

	if(ld1 && (uintptr_t)ld1->end == 0x8c400000u && rom == 0x8c400000u) {
		/* Public Feb-2025 mpm.bin: code at a fixed 0x8c400000, 48 pages
		   mapped up to 0x8c700000 where mpm.bin lives */
		gmem_stats.mpm_variant = GMPM_FEB2025;
		if(add_private_arena(&g_priv_arena, "cgd_ram", rom + img64,
			CGD_RAM_OLD_MPM_LIMIT)) {
			gmem_stats.priv_start = rom + img64;
			gmem_stats.priv_end = CGD_RAM_OLD_MPM_LIMIT;
		}
	}
	else if(ld1 && (uintptr_t)ld1->end == rom) {
		gmem_stats.mpm_variant = GMPM_V11;
	}
	else if(!ld1 && uram == 0x8c780000u && rom > CGD_RAM_LOW_LIMIT
		&& rom <= CGD_RAM_OLD_MPM_LIMIT) {
		/* dynload: code at (0x8c700000 - size) & -64k, nothing announced */
		gmem_stats.mpm_variant = GMPM_DYNLOAD;
		if(add_private_arena(&g_priv_arena, "cgd_ram", CGD_RAM_LOW_LIMIT,
			rom)) {
			gmem_stats.priv_start = CGD_RAM_LOW_LIMIT;
			gmem_stats.priv_end = rom;
		}

		/* mpm.bin up to a1a2cc3 (2025-02-10, the base of both the Feb-2025
		   build and dynload): code at a fixed 0x8c400000, 48 pages mapped
		   up to 0x8c700000, user RAM at 0x8c780000, no loadinfo. The
		   range after the image is free exactly as in the Feb-2025 layout.
		   A dynload build of our (< 2 MiB) binary never loads at
		   0x8c400000, and for dynload [rom + img64, 0x8c700000) is empty
		   anyway (the code ends at 0x8c700000). */
		if(rom == 0x8c400000u) {
			gmem_stats.mpm_variant = GMPM_PRELOADINFO;
			if(add_private_arena(&g_priv_arena2, "cgd_ram2", rom + img64,
				CGD_RAM_OLD_MPM_LIMIT)) {
				gmem_stats.priv2_start = rom + img64;
				gmem_stats.priv2_end = CGD_RAM_OLD_MPM_LIMIT;
			}
		}
	}
	else {
		gmem_stats.mpm_variant = GMPM_UNKNOWN;
	}
}

char const *gmem_variant_name(void)
{
	switch(gmem_stats.mpm_variant) {
	case GMPM_NONE:    return "no MPM";
	case GMPM_V11:     return "mpm.bin >= 1.1";
	case GMPM_FEB2025: return "mpm.bin Feb-2025";
	case GMPM_DYNLOAD: return "mpm.bin dynload";
	case GMPM_PRELOADINFO: return "mpm.bin pre-Feb-2025";
	default:           return "unknown loader";
	}
}

size_t gmem_total_free(void)
{
	size_t total = 0;
	for(int i = 0; i < ZONE_ARENA_COUNT; i++) {
		kmalloc_arena_t *a = kmalloc_get_arena(g_zone_arenas[i]);
		if(!a)
			continue;
		kmalloc_gint_stats_t *s = kmalloc_get_gint_stats(a);
		if(s)
			total += s->free_memory;
	}
	return total;
}

static size_t arena_largest(char const *name)
{
	size_t size = 0;
	void *p = kmalloc_max(&size, name);
	if(!p)
		return 0;
	kfree(p);
	return size;
}

size_t gmem_largest_free(void)
{
	size_t best = 0;
	for(int i = 0; i < ZONE_ARENA_COUNT; i++) {
		if(!kmalloc_get_arena(g_zone_arenas[i]))
			continue;
		size_t s = arena_largest(g_zone_arenas[i]);
		if(s > best)
			best = s;
	}
	return best;
}

bool gmem_arena_info(int i, char const **name, uintptr_t *start,
	uintptr_t *end, size_t *free_bytes, size_t *largest)
{
	if(i < 0 || i >= (int)(sizeof g_diag_arenas / sizeof *g_diag_arenas))
		return false;

	*name = g_diag_arenas[i];
	*start = *end = 0;
	*free_bytes = *largest = 0;

	kmalloc_arena_t *a = kmalloc_get_arena(*name);
	if(!a) {
		*name = NULL;
		return true;
	}
	*start = (uintptr_t)a->start;
	*end = (uintptr_t)a->end;
	kmalloc_gint_stats_t *s = kmalloc_get_gint_stats(a);
	if(s)
		*free_bytes = s->free_memory;
	if(a->malloc_max)
		*largest = arena_largest(*name);
	return true;
}

void gmem_probe_extra_ram(void)
{
	g_extra_size = 0;
	g_extra_base = 0;
	if(g_is_cg100 || !gsys_cg50_extra_ram_allowed())
		return;

	/* CGDoom's FindZeroedMemory (cgdoom.c): zero longwords every 16 bytes
	   from 0xac200000 (uncached view), at most 6 MiB, 4 KiB granularity.
	   fx-CG50 with OS <= 3.60 only, and only when the user enabled it. */
	volatile uint32_t const *probe = (void *)EXTRA_P2;
	int size = 0;
	while(size < (6 << 20) && probe[size >> 2] == 0)
		size += 16;
	size &= ~0xfff;

	if(size >= CGD_MIN_ZONE) {
		g_extra_base = 0x8c200000u;
		g_extra_size = size;
	}
	gmem_stats.extra_ram = g_extra_size;
}

/* Final review: the probe takes zeroed memory as free, and the zone header
   (Z_AddZone) and the game's data are not zero, so without this the next
   launch (and any other add-in using the same trick) found no extra RAM
   until the calculator was reset. Called once the engine cannot touch the
   zone any more: exit_path() after the flush, and the destructor (abort()).
   The cached P1 lines of the range are dropped first (a later write-back
   would put old data over the zeros), then the range is zeroed through the
   uncached P2 alias, which is what the probe reads. */
void gmem_release_extra_ram(void)
{
	if(!g_extra_used || g_extra_size <= 0)
		return;
	g_extra_used = false;
#ifdef __sh__
	for(uintptr_t a = g_extra_base; a < g_extra_base + g_extra_size; a += 32)
		__asm__ volatile("ocbi @%0" :: "r"(a) : "memory");
#endif
	synco();
	volatile uint32_t *p = (void *)EXTRA_P2;
	for(int i = 0; i < g_extra_size / 4; i++)
		p[i] = 0;
}

typedef struct {
	void *p;
	size_t s;
	void *orig;     /* pointer returned by kmalloc (NULL: not from kmalloc) */
} block_t;

static void sort_blocks(block_t *b, int n)
{
	for(int i = 1; i < n; i++) {
		block_t x = b[i];
		int j = i;
		while(j > 0 && b[j-1].s < x.s) {
			b[j] = b[j-1];
			j--;
		}
		b[j] = x;
	}
}

int PL_GetZones(void **base, int *size, int max)
{
	block_t b[ZONE_ARENA_COUNT + 1];
	int n = 0;

	for(int i = 0; i < ZONE_ARENA_COUNT; i++) {
		if(!kmalloc_get_arena(g_zone_arenas[i]))
			continue;
		size_t s = 0;
		void *p = kmalloc_max(&s, g_zone_arenas[i]);
		if(p) {
			b[n].p = p;
			b[n].s = s;
			b[n].orig = p;
			n++;
		}
	}
	sort_blocks(b, n);

	size_t reserve = g_is_cg100 ? CGD_MALLOC_RESERVE
		: CGD_MALLOC_RESERVE_CG50;
	size_t reserved = 0;
	block_t z[ZONE_ARENA_COUNT + 2];
	int count = 0;

	for(int i = 0; i < n; i++) {
		void *p = b[i].p;
		size_t s = b[i].s;

		/* Leave the malloc reserve, taken from the largest blocks */
		if(reserved < reserve) {
			size_t want = reserve - reserved;
			if(s >= want + CGD_MIN_ZONE) {
				size_t keep = (s - want) & ~(size_t)31;
				void *q = krealloc(p, keep);
				if(q) {
					p = q;
					s = keep;
					reserved = reserve;
				}
			}
			else {
				kfree(p);
				reserved += s;
				continue;
			}
		}

		if(s < CGD_MIN_ZONE) {
			kfree(p);
			continue;
		}

		/* 32-byte (cache line) alignment */
		uintptr_t a = ((uintptr_t)p + 31) & ~(uintptr_t)31;
		s = (s - (a - (uintptr_t)p)) & ~(size_t)31;
		z[count].p = (void *)a;
		z[count].s = s;
		z[count].orig = p;
		count++;
	}

	if(g_extra_size >= CGD_MIN_ZONE) {
		z[count].p = (void *)g_extra_base;
		z[count].s = g_extra_size;
		z[count].orig = NULL;
		count++;
		/* Zeroed again at exit, whether the engine takes it or not */
		g_extra_used = true;
	}

	/* Largest first, as many as the engine accepts (the rest stays in
	   the heap: kmalloc_max blocks that do not fit are freed) */
	sort_blocks(z, count);
	int out = 0;
	for(int i = 0; i < count; i++) {
		if(out >= max || out >= 8) {
			if(z[i].orig)
				kfree(z[i].orig);
			continue;
		}
		base[out] = z[i].p;
		size[out] = (int)z[i].s;
		gmem_stats.zone_base[out] = z[i].p;
		gmem_stats.zone_size[out] = (int)z[i].s;
		out++;
	}
	gmem_stats.zone_count = out;
	gmem_stats.reserve = (int)reserved;
	return out;
}
