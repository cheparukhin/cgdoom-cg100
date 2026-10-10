/* CGDoom gint platform: the OS heap arena ("_os"), replaced at link time.

   gint registers kmalloc_arena_osheap (src/kmalloc/arena_osheap.c) as the
   last default arena from the very first allocation in kinit(), and its
   functions call the OS heap syscalls __malloc/__realloc/__free. On the
   fx-CG100 that is wrong before main() can do anything about it: kinit()
   allocates the world buffers, the driver flags and the 177.5 KB VRAM, and
   the fxlibc constructors allocate too. Once _uram is exhausted those
   requests would fall through to the OS heap: OS code on API 2, and on any
   OS string gint maps to API 0 a jump to the fx-CG50 syscall gate
   0x80020070, which does not exist on the fx-CG100 (gint review round 1).

   Defining kmalloc_arena_osheap here makes the linker take this definition
   and never pull gint's arena_osheap.o (nothing else is in that object).
   The same g3a runs on both models, so the arena still forwards to the OS
   heap on the fx-CG50, where it is legitimate (~1 MB more heap). The model
   comes from gint[HWCALC], which hw_detect() sets before kinit() runs, so
   it is valid for every allocation (g_is_cg100 is only set in main()).
   On the fx-CG100 every request fails (NULL), and free/realloc of pointers
   that no other arena owns are ignored: gint's arena_owning() hands such
   pointers to this arena (start == end == NULL matches anything).

   gmem_init() still removes "_os" on the fx-CG100 (diagnostics show it). */

#include <gint/kmalloc.h>
#include <gint/hardware.h>
#include <stddef.h>

/* gint's OS heap syscall wrappers (kernel/syscalls.S) */
extern void *__malloc(size_t size);
extern void *__realloc(void *ptr, size_t newsize);
extern void __free(void *ptr);

static int os_heap_usable(void)
{
	return gint[HWCALC] != HWCALC_FXCG100;
}

static void *cgd_osheap_malloc(size_t size, void *data)
{
	(void)data;
	return os_heap_usable() ? __malloc(size) : NULL;
}

static void *cgd_osheap_realloc(void *ptr, size_t newsize, void *data)
{
	(void)data;
	return os_heap_usable() ? __realloc(ptr, newsize) : NULL;
}

static void cgd_osheap_free(void *ptr, void *data)
{
	(void)data;
	if(os_heap_usable())
		__free(ptr);
}

kmalloc_arena_t kmalloc_arena_osheap = {
	.malloc      = cgd_osheap_malloc,
	.realloc     = cgd_osheap_realloc,
	.free        = cgd_osheap_free,
	.malloc_max  = NULL,
	.name        = "_os",
	.start       = NULL,
	.end         = NULL,
	.data        = NULL,
	.is_default  = 1,
	.stats       = { 0 },
};
