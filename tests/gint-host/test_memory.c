/* Host unit test for src/gint/memory.c (mpm.bin variant detection and the
   private arenas) and src/gint/no_osheap.c (the "_os" arena replacement).

   gint review round 1:
   - pre-loadinfo mpm.bin builds (<= a1a2cc3: code at a fixed 0x8c400000,
     user RAM at 0x8c780000, no 'MPM0') get both free ranges:
     [0x8c200000, 0x8c400000) and [code end, 0x8c700000);
   - the OS heap arena returns NULL on the fx-CG100 from the very first
     allocation, and forwards to the OS heap on the fx-CG50. */

#include "cgd_gint.h"
#include <gint/kmalloc.h>
#include <gint/hardware.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

/* Globals memory.c needs */
bool g_is_cg100 = true;
char g_os_version[11] = "02.00.0000";
static bool extra_allowed = false;
bool gsys_cg50_extra_ram_allowed(void) { return extra_allowed; }
extern uint32_t cgd_host_image_size;
uint32_t gint[16];

/* Simulated arenas and MMU */
static kmalloc_arena_t *arenas[8];
static uint32_t ROM, URAM;
uint32_t mmu_translate(uint32_t page, uint32_t *size)
{
	(void)size;
	return page == 0x00300000 ? ROM : page == 0x08100000 ? URAM : (uint32_t)-1;
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
void *kmalloc_max(size_t *s, char const *n) { (void)s; (void)n; return NULL; }
void kfree(void *p) { (void)p; }
void *krealloc(void *p, size_t s) { (void)s; return p; }
kmalloc_gint_stats_t *kmalloc_get_gint_stats(kmalloc_arena_t *a) { return &a->sim; }

static kmalloc_arena_t ld1 = { .name = "_ld1" }, ur = { .name = "_uram" };
extern kmalloc_arena_t kmalloc_arena_osheap;

#define BIN 0x4c7e8u   /* image size of a ~314 KB build */
#define IMG64 0x50000u

static void run(uint32_t rom, uint32_t uram, uint32_t ld1end)
{
	memset(arenas, 0, sizeof arenas);
	memset(&gmem_stats, 0, sizeof gmem_stats);
	ROM = rom; URAM = uram;
	arenas[0] = &ur;
	arenas[7] = &kmalloc_arena_osheap;
	if(ld1end) {
		ld1.start = (void *)(uintptr_t)0x8c200000;
		ld1.end = (void *)(uintptr_t)ld1end;
		arenas[1] = &ld1;
	}
	cgd_host_image_size = BIN;
	gmem_init();
}

static void show(char const *label)
{
	printf("%-22s -> %-22s priv=[%08lx,%08lx) priv2=[%08lx,%08lx) _os removed=%d\n",
		label, gmem_variant_name(),
		(unsigned long)gmem_stats.priv_start, (unsigned long)gmem_stats.priv_end,
		(unsigned long)gmem_stats.priv2_start, (unsigned long)gmem_stats.priv2_end,
		gmem_stats.os_heap_removed);
}

static void test_variants(void)
{
	uint32_t v11 = (0x8c7f0000 - BIN) & ~0xffffu;
	uint32_t dyn = (0x8c700000 - BIN) & ~0xffffu;

	run(v11 & 0x1fffffff, 0x0c160000, v11);
	show("v1.1 (cbd1a41)");
	CHECK(gmem_stats.mpm_variant == GMPM_V11 && !gmem_stats.priv_end, "v1.1");
	CHECK(gmem_stats.os_heap_removed && !kmalloc_get_arena("_os"), "_os removed");

	run(0x0c400000, 0x0c780000, 0x8c400000);
	show("Feb-2025 (68e7f6e)");
	CHECK(gmem_stats.mpm_variant == GMPM_FEB2025, "feb");
	CHECK(gmem_stats.priv_start == 0x8c400000 + IMG64 && gmem_stats.priv_end == 0x8c700000, "feb range");
	CHECK(!gmem_stats.priv2_end && !kmalloc_get_arena("cgd_ram2"), "feb: one range");

	run(dyn & 0x1fffffff, 0x0c780000, 0);
	show("dynload (1dd901e)");
	CHECK(gmem_stats.mpm_variant == GMPM_DYNLOAD, "dynload");
	CHECK(gmem_stats.priv_start == 0x8c200000 && gmem_stats.priv_end == dyn, "dynload range");
	CHECK(!gmem_stats.priv2_end, "dynload: one range");

	/* Regression: a1a2cc3 and older */
	run(0x0c400000, 0x0c780000, 0);
	show("pre-loadinfo a1a2cc3");
	CHECK(gmem_stats.mpm_variant == GMPM_PRELOADINFO, "pre-loadinfo variant (%d)", gmem_stats.mpm_variant);
	CHECK(!strcmp(gmem_variant_name(), "mpm.bin pre-Feb-2025"), "name");
	CHECK(gmem_stats.priv_start == 0x8c200000 && gmem_stats.priv_end == 0x8c400000, "low range");
	CHECK(gmem_stats.priv2_start == 0x8c400000 + IMG64 && gmem_stats.priv2_end == 0x8c700000, "high range");
	kmalloc_arena_t *a2 = kmalloc_get_arena("cgd_ram2");
	CHECK(a2 && a2->is_default && (uintptr_t)a2->start == 0x8c400000 + IMG64, "cgd_ram2 arena");
	size_t total = gmem_total_free();
	printf("  total free %zu KiB\n", total >> 10);
	CHECK(total >= (4u << 20), "pre-loadinfo now offers > 4 MiB (%zu KiB)", total >> 10);

	/* An image so large that nothing is left above it */
	memset(arenas, 0, sizeof arenas);
	memset(&gmem_stats, 0, sizeof gmem_stats);
	ROM = 0x0c400000; URAM = 0x0c780000; arenas[0] = &ur;
	cgd_host_image_size = 0x2f8000;
	gmem_init();
	CHECK(gmem_stats.mpm_variant == GMPM_PRELOADINFO && !gmem_stats.priv2_end, "huge image: no 2nd range");

	run(0x0c400000, 0x0c160000, 0x8c400000);
	show("e99a7d1");
	CHECK(gmem_stats.mpm_variant == GMPM_FEB2025, "e99a7d1");
}

/* no_osheap.c */
static int os_calls = 0;
void *__malloc(size_t s) { os_calls++; return (void *)(uintptr_t)(0x88000000u + s); }
void *__realloc(void *p, size_t s) { (void)p; os_calls++; return (void *)(uintptr_t)(0x88000000u + s); }
void __free(void *p) { (void)p; os_calls++; }

static void test_osheap(void)
{
	kmalloc_arena_t *a = &kmalloc_arena_osheap;
	CHECK(!strcmp(a->name, "_os") && a->is_default && !a->start && !a->end, "arena shape");

	gint[HWCALC] = HWCALC_FXCG100;
	os_calls = 0;
	CHECK(a->malloc(100, a->data) == NULL, "CG100 malloc fails");
	CHECK(a->realloc((void *)0x1000, 100, a->data) == NULL, "CG100 realloc fails");
	a->free((void *)0x1000, a->data);
	CHECK(os_calls == 0, "CG100: no OS call at all");

	gint[HWCALC] = HWCALC_FXCG50;
	CHECK(a->malloc(100, a->data) == (void *)(uintptr_t)0x88000064u, "CG50 forwards malloc");
	a->free((void *)0x1000, a->data);
	CHECK(os_calls == 2, "CG50 forwards free");
}

/* Final review: the fx-CG50 experimental RAM handed to the engine is
   zeroed again at exit, so the next launch's probe finds it */
extern uint8_t *cgd_host_extra_p2;
static void test_extra_ram(void)
{
	enum { SZ = 6 << 20, FREE = 512 << 10 };
	uint8_t *ram = calloc(1, SZ);
	cgd_host_extra_p2 = ram;
	ram[FREE] = 1;                 /* the OS's data after 512 KiB */
	g_is_cg100 = false;
	extra_allowed = true;
	memset(arenas, 0, sizeof arenas);
	memset(&gmem_stats, 0, sizeof gmem_stats);

	gmem_probe_extra_ram();
	CHECK(gmem_stats.extra_ram == FREE, "probe %d KiB", gmem_stats.extra_ram >> 10);
	gmem_release_extra_ram();
	CHECK(ram[0] == 0 && ram[FREE] == 1, "nothing zeroed before it was handed out");
	void *base[8]; int size[8];
	int n = PL_GetZones(base, size, 8);
	CHECK(n == 1 && (uintptr_t)base[0] == 0x8c200000u && size[0] == FREE, "extra RAM zone");
	/* The engine's zone header and data (Z_AddZone writes size at 0):
	   without the release, the next launch's probe finds nothing (the
	   bug). exit_path(): the zone handed out is zeroed, and only it. */
	memset(ram, 0x6b, FREE);
	gmem_release_extra_ram();
	CHECK(ram[0] == 0 && ram[FREE - 1] == 0 && ram[FREE] == 1, "zone zeroed, OS data kept");
	gmem_probe_extra_ram();
	CHECK(gmem_stats.extra_ram == FREE, "next launch finds %d KiB again", gmem_stats.extra_ram >> 10);
	/* Idempotent (exit_path() then the destructor) */
	ram[0] = 9;
	gmem_release_extra_ram();
	CHECK(ram[0] == 9, "released once");
	/* The old behaviour, for reference: dirty RAM is not found */
	memset(ram, 0x6b, 16);
	gmem_probe_extra_ram();
	CHECK(gmem_stats.extra_ram == 0, "dirty RAM is not free");
	/* Not on the fx-CG100 / when not allowed */
	extra_allowed = false;
	gmem_probe_extra_ram();
	CHECK(gmem_stats.extra_ram == 0, "not allowed: no probe");
	g_is_cg100 = true;
	cgd_host_extra_p2 = NULL;
	free(ram);
}

int main(void)
{
	test_variants();
	test_extra_ram();
	test_osheap();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
