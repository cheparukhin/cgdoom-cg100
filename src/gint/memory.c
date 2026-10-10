/* CGDoom gint platform: heap and zones (PL_GetZones).

   mpm.bin 1.1 announces its free RAM to gint through the loadinfo block
   ('MPM0'): gint turns it into the "_ld1" arena, which ends where our code
   was loaded (about 5.5 MiB with the bundled build). Older mpm.bin builds
   (the public Feb-2025 zip, the "dynload" build shipped by gpSP and KhiCAS)
   announce at most about 2 MiB, and CGDoom does not look for the rest: main()
   refuses them (CGD_MIN_TOTAL_HEAP) and asks for mpm.bin 1.1.

   The OS heap arena "_os" is removed so that exhausted mallocs return NULL
   instead of calling OS functions (no_osheap.c already refuses everything).

   PL_GetZones() is called once by D_DoomMain(): the platform's own buffers
   (frame buffer, WAD image, tables, private stack) have been allocated by
   then. It keeps a malloc reserve for the engine and gives the largest
   blocks of every arena to the zone allocator. */

#include "cgd_gint.h"

#include <gint/kmalloc.h>
#include <gint/mmu.h>
#include <stdlib.h>
#include <string.h>

gmem_stats_t gmem_stats;

/* Arena names considered for zones / statistics. "_ld1" is used for every
   loader arena by gint (kernel.c never increments its counter). */
static char const * const g_zone_arenas[] = {
	"_ld1", "_uram",
};
#define ZONE_ARENA_COUNT (int)(sizeof g_zone_arenas / sizeof *g_zone_arenas)

static char const * const g_diag_arenas[] = {
	"_uram", "_ld1", "_os",
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

void gmem_init(void)
{
	gmem_stats.image_size = image_size();
	gmem_stats.rom_phys = mmu_translate(0x00300000, NULL);
	gmem_stats.uram_phys = mmu_translate(0x08100000, NULL);

	/* No OS heap: gint's fallback would call OS malloc */
	kmalloc_arena_t *os = kmalloc_get_arena("_os");
	if(os) {
		kmalloc_remove_arena(os);
		gmem_stats.os_heap_removed = true;
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

typedef struct {
	void *p;
	size_t s;
	void *orig;     /* pointer returned by kmalloc */
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
	block_t b[ZONE_ARENA_COUNT];
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

	size_t reserve = CGD_MALLOC_RESERVE;
	size_t reserved = 0;
	block_t z[ZONE_ARENA_COUNT];
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

	/* Largest first, as many as the engine accepts (the rest stays in
	   the heap: kmalloc_max blocks that do not fit are freed) */
	sort_blocks(z, count);
	int out = 0;
	for(int i = 0; i < count; i++) {
		if(out >= max || out >= 8) {
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
