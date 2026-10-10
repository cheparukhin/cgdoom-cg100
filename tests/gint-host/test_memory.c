/* Host unit test for src/gint/memory.c (heap arenas, PL_GetZones) and
   src/gint/no_osheap.c (the "_os" arena replacement).

   - gmem_init() removes the OS heap arena;
   - the OS heap arena returns NULL from the very first allocation and
     never calls the OS (gint review round 1);
   - PL_GetZones() keeps the malloc reserve and hands out the rest. */

#include "cgd_gint.h"
#include <gint/kmalloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

/* Globals memory.c needs */
char g_os_version[11] = "02.00.0000";
extern uint32_t cgd_host_image_size;

/* Simulated arenas and MMU */
static kmalloc_arena_t *arenas[8];
uint32_t mmu_translate(uint32_t page, uint32_t *size)
{
	(void)size;
	return page == 0x00300000 ? 0x0c780000 : page == 0x08100000 ? 0x0c160000
		: (uint32_t)-1;
}
kmalloc_arena_t *kmalloc_get_arena(char const *n)
{
	for(int i = 0; i < 8; i++)
		if(arenas[i] && !strcmp(arenas[i]->name, n)) return arenas[i];
	return NULL;
}
void kmalloc_remove_arena(kmalloc_arena_t *a)
{
	for(int i = 0; i < 8; i++) if(arenas[i] == a) arenas[i] = NULL;
}
bool kmalloc_add_arena(kmalloc_arena_t *a)
{
	for(int i = 0; i < 8; i++) if(!arenas[i]) { arenas[i] = a; return true; }
	return false;
}
void kmalloc_init_arena(kmalloc_arena_t *a, bool s)
{
	(void)s;
	a->sim.free_memory = (uint32_t)((char *)a->end - (char *)a->start);
}
/* kmalloc_max: the whole free space of the arena, at its start */
void *kmalloc_max(size_t *s, char const *n)
{
	kmalloc_arena_t *a = kmalloc_get_arena(n);
	if(!a || !a->sim.free_memory)
		return NULL;
	*s = a->sim.free_memory;
	a->sim.free_memory = 0;
	return a->start;
}
void kfree(void *p) { (void)p; }
void *krealloc(void *p, size_t s) { (void)s; return p; }
kmalloc_gint_stats_t *kmalloc_get_gint_stats(kmalloc_arena_t *a) { return &a->sim; }

static kmalloc_arena_t ld1 = { .name = "_ld1" }, ur = { .name = "_uram" };
extern kmalloc_arena_t kmalloc_arena_osheap;

static void setup(void)
{
	memset(arenas, 0, sizeof arenas);
	memset(&gmem_stats, 0, sizeof gmem_stats);
	ur.start = (void *)(uintptr_t)0x8c160000; ur.end = (void *)(uintptr_t)0x8c1a0000;
	ld1.start = (void *)(uintptr_t)0x8c200000; ld1.end = (void *)(uintptr_t)0x8c780000;
	kmalloc_init_arena(&ur, true);
	kmalloc_init_arena(&ld1, true);
	arenas[0] = &ur;
	arenas[1] = &ld1;
	arenas[7] = &kmalloc_arena_osheap;
	cgd_host_image_size = 0x4c7e8;
	gmem_init();
}

static void test_init(void)
{
	setup();
	CHECK(gmem_stats.os_heap_removed && !kmalloc_get_arena("_os"), "_os removed");
	CHECK(gmem_stats.rom_phys == 0x0c780000 && gmem_stats.image_size == 0x4c7e8, "stats");
	CHECK(gmem_total_free() == 0x40000 + 0x580000, "total free %zu", gmem_total_free());
}

static void test_zones(void)
{
	setup();
	void *base[8]; int size[8];
	int n = PL_GetZones(base, size, 8);
	/* _ld1 (5.5 MiB) less the malloc reserve, then _uram (256 KiB) */
	CHECK(n == 2, "zones %d", n);
	CHECK(base[0] == ld1.start && size[0] == 0x580000 - CGD_MALLOC_RESERVE,
		"zone 0: %p %d", base[0], size[0]);
	CHECK(base[1] == ur.start && size[1] == 0x40000, "zone 1: %p %d", base[1], size[1]);
	CHECK(gmem_stats.reserve == CGD_MALLOC_RESERVE, "reserve %d", gmem_stats.reserve);
	setup();
	CHECK(PL_GetZones(base, size, 1) == 1 && base[0] == ld1.start, "max 1 zone");
}

/* no_osheap.c */
static void test_osheap(void)
{
	kmalloc_arena_t *a = &kmalloc_arena_osheap;
	CHECK(!strcmp(a->name, "_os") && a->is_default && !a->start && !a->end, "arena shape");
	CHECK(a->malloc(100, a->data) == NULL, "malloc fails");
	CHECK(a->realloc((void *)0x1000, 100, a->data) == NULL, "realloc fails");
	a->free((void *)0x1000, a->data);
}

int main(void)
{
	test_init();
	test_zones();
	test_osheap();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
