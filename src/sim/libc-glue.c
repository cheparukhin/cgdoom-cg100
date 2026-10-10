/* Allocator and fxlibc HAL glue for the sh-elf-run harness.

   fxlibc's malloc/realloc/free forward to fxlibc_hal_malloc/realloc/free (on the calculator, gint's
   kmalloc); here they are implemented by a small boundary-tag allocator over the region
   [__heap_start, __heap_end) of sim.ld:
   - chunks are 8-byte aligned and so are the payloads; per-allocation overhead is 4 bytes
     (dlmalloc layout: "head" word at chunk+4, payload at chunk+8);
   - free chunks are kept in one circular doubly-linked list, allocation is first-fit with splitting,
     free() coalesces with both neighbours in O(1); realloc grows in place when the next chunk is free;
   - memalign()/aligned_alloc()/posix_memalign() are provided (alignment up to any power of two). */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fxlibc/hal.h>
#include "sim.h"

typedef struct chunk {
	size_t prev_size;        /* valid only when the previous chunk is free */
	size_t head;             /* size | PINUSE; size is a multiple of 8, 0 for the fence post */
	struct chunk *fd, *bk;   /* only when the chunk is free (they overlay the payload) */
} chunk_t;

#define PINUSE     1u
#define SIZE_MASK  (~7u)
#define MIN_CHUNK  16u
#define PAYLOAD    8u        /* offset of payload inside a chunk */

static chunk_t freelist;     /* sentinel of the circular list */
static chunk_t *arena_end;   /* fence post */
static int inited;
static struct sim_heap_info stats;

static inline size_t csize(const chunk_t *c) { return c->head & SIZE_MASK; }
static inline chunk_t *cnext(chunk_t *c) { return (chunk_t *)((char *)c + csize(c)); }
static inline chunk_t *mem2chunk(void *p) { return (chunk_t *)((char *)p - PAYLOAD); }
static inline void *chunk2mem(chunk_t *c) { return (char *)c + PAYLOAD; }
static inline int cinuse(chunk_t *c) { return cnext(c)->head & PINUSE; }

static void list_insert(chunk_t *c)
{
	c->fd = freelist.fd;
	c->bk = &freelist;
	freelist.fd->bk = c;
	freelist.fd = c;
}

static void list_remove(chunk_t *c)
{
	c->bk->fd = c->fd;
	c->fd->bk = c->bk;
}

/* Mark c (size already in head, PINUSE preserved) free: footer, next's PINUSE, list. */
static void make_free(chunk_t *c, size_t size)
{
	c->head = size | (c->head & PINUSE);
	chunk_t *n = (chunk_t *)((char *)c + size);
	n->prev_size = size;
	n->head &= ~PINUSE;
	list_insert(c);
}

static void heap_init(void)
{
	uintptr_t s = ((uintptr_t)__heap_start + 7) & ~7u;
	uintptr_t e = ((uintptr_t)__heap_end) & ~7u;
	freelist.fd = freelist.bk = &freelist;
	arena_end = (chunk_t *)(e - PAYLOAD);              /* fence post occupies the last 8 bytes */
	arena_end->head = 0;                               /* size 0, PINUSE set by make_free */
	chunk_t *first = (chunk_t *)s;
	first->head = PINUSE;
	make_free(first, (size_t)((char *)arena_end - (char *)first));
	stats.arena = (char *)arena_end - (char *)first;
	inited = 1;
}

static inline size_t request_to_chunk(size_t n)
{
	if(n > 0x7ffffff0u) return 0;
	size_t sz = (n + 4 + 7) & ~7u;
	return sz < MIN_CHUNK ? MIN_CHUNK : sz;
}

static void note_alloc(size_t size)
{
	stats.used += size;
	if(stats.used > stats.peak) stats.peak = stats.used;
}

/* Carve the first `need` bytes of in-use chunk c; the remainder (if usable) is freed.
   Returns the number of bytes released (the caller does the statistics). */
static size_t trim(chunk_t *c, size_t need)
{
	size_t size = csize(c);
	if(size - need < MIN_CHUNK) return 0;
	chunk_t *rest = (chunk_t *)((char *)c + need);
	c->head = need | (c->head & PINUSE);
	rest->head = PINUSE;            /* previous (c) is in use */
	/* coalesce rest with its next neighbour if free */
	size_t rsize = size - need;
	chunk_t *n = (chunk_t *)((char *)c + size);
	if(n->head & SIZE_MASK) {
		if(!cinuse(n)) {
			list_remove(n);
			rsize += csize(n);
		}
	}
	make_free(rest, rsize);
	return size - need;
}

void *fxlibc_hal_malloc(size_t n)
{
	if(!inited) heap_init();
	size_t need = request_to_chunk(n);
	if(!need) return NULL;

	for(chunk_t *c = freelist.fd; c != &freelist; c = c->fd) {
		size_t size = csize(c);
		if(size < need) continue;
		list_remove(c);
		chunk_t *nx = (chunk_t *)((char *)c + size);
		nx->head |= PINUSE;
		note_alloc(size - trim(c, need));
		stats.nalloc++;
		return chunk2mem(c);
	}
	return NULL;
}

void fxlibc_hal_free(void *p)
{
	if(!p) return;
	chunk_t *c = mem2chunk(p);
	size_t size = csize(c);
	stats.used -= size;
	stats.nfree_calls++;
	chunk_t *n = (chunk_t *)((char *)c + size);

	if(!(c->head & PINUSE)) {                    /* merge backwards */
		chunk_t *prev = (chunk_t *)((char *)c - c->prev_size);
		list_remove(prev);
		size += csize(prev);
		c = prev;
	}
	if((n->head & SIZE_MASK) && !cinuse(n)) {    /* merge forwards */
		list_remove(n);
		size += csize(n);
	}
	make_free(c, size);
}

void *fxlibc_hal_realloc(void *p, size_t n)
{
	if(!p) return fxlibc_hal_malloc(n);
	if(n == 0) { fxlibc_hal_free(p); return NULL; }

	chunk_t *c = mem2chunk(p);
	size_t size = csize(c);
	size_t need = request_to_chunk(n);
	if(!need) return NULL;

	if(need <= size) {                           /* shrink in place */
		stats.used -= trim(c, need);
		return p;
	}
	chunk_t *nx = (chunk_t *)((char *)c + size);
	if((nx->head & SIZE_MASK) && !cinuse(nx) && size + csize(nx) >= need) {   /* grow in place */
		size_t nsz = csize(nx);
		list_remove(nx);
		chunk_t *nn = (chunk_t *)((char *)nx + nsz);
		nn->head |= PINUSE;
		c->head = (size + nsz) | (c->head & PINUSE);
		note_alloc(nsz - trim(c, need));
		return p;
	}
	void *q = fxlibc_hal_malloc(n);
	if(!q) return NULL;
	memcpy(q, p, size - 4);                      /* usable bytes of the old block */
	fxlibc_hal_free(p);
	return q;
}

size_t malloc_usable_size(void *p)
{
	return p ? csize(mem2chunk(p)) - 4 : 0;
}

void *memalign(size_t align, size_t n)
{
	if(align <= 8) return malloc(n);   /* sets errno itself */
	if(align & (align - 1)) { errno = EINVAL; return NULL; }
	void *raw = fxlibc_hal_malloc(n + align + MIN_CHUNK);
	if(!raw) { errno = ENOMEM; return NULL; }
	if(((uintptr_t)raw & (align - 1)) == 0) {
		stats.used -= trim(mem2chunk(raw), request_to_chunk(n));
		return raw;
	}
	chunk_t *c = mem2chunk(raw);
	uintptr_t p = ((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1);
	if(p - (uintptr_t)raw < MIN_CHUNK) p += align;   /* leading slack must hold a free chunk */
	chunk_t *nc = mem2chunk((void *)p);
	size_t total = csize(c);
	size_t lead = (char *)nc - (char *)c;
	/* leading part stays an in-use chunk until freed; nc is its in-use successor */
	c->head = lead | (c->head & PINUSE);
	nc->head = (total - lead) | PINUSE;
	stats.used -= trim(nc, request_to_chunk(n));
	fxlibc_hal_free(chunk2mem(c));       /* releases the leading slack */
	return (void *)p;
}

void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }

int posix_memalign(void **out, size_t align, size_t n)
{
	if(align < sizeof(void *) || (align & (align - 1))) return EINVAL;
	void *p = memalign(align, n);
	if(!p) return ENOMEM;
	*out = p;
	return 0;
}

void sim_heap_info(struct sim_heap_info *out)
{
	if(!inited) heap_init();
	stats.free = stats.largest = stats.nchunks_free = 0;
	for(chunk_t *c = freelist.fd; c != &freelist; c = c->fd) {
		size_t s = csize(c);
		stats.free += s;
		stats.nchunks_free++;
		if(s > stats.largest) stats.largest = s;
	}
	*out = stats;
}

/* Walk every chunk by address, cross-check with the free list. Returns 0 if fine, else a code. */
int sim_heap_check(void)
{
	if(!inited) return 0;
	uintptr_t s = ((uintptr_t)__heap_start + 7) & ~7u;
	chunk_t *c = (chunk_t *)s;
	size_t nfree_seen = 0, used = 0;
	int prev_free = 0;
	while(c != arena_end) {
		size_t sz = csize(c);
		if(sz < MIN_CHUNK || (sz & 7) || (char *)c + sz > (char *)arena_end) return 1;
		if(!!(c->head & PINUSE) == prev_free) return 2;       /* PINUSE inconsistent */
		chunk_t *n = cnext(c);
		int is_used = (n->head & PINUSE) != 0;
		if(is_used) used += sz;
		else {
			if(prev_free) return 3;                               /* uncoalesced neighbours */
			if(n->prev_size != sz) return 4;                      /* bad footer */
			nfree_seen++;
		}
		prev_free = !is_used;
		c = n;
	}
	if(!!(arena_end->head & PINUSE) == prev_free) return 5;
	size_t nl = 0;
	for(chunk_t *f = freelist.fd; f != &freelist; f = f->fd) {
		if(f->fd->bk != f) return 6;
		nl++;
	}
	if(nl != nfree_seen) return 7;
	if(used != stats.used) return 8;
	return 0;
}

/* ---- standard functions that fxlibc declares but does not implement --------------------------- */

__attribute__((weak))
void *bsearch(const void *key, const void *base, size_t n, size_t size,
	int (*cmp)(const void *, const void *))
{
	const char *lo = base;
	while(n) {
		size_t half = n >> 1;
		const char *mid = lo + half * size;
		int r = cmp(key, mid);
		if(r == 0) return (void *)mid;
		if(r > 0) { lo = mid + size; n -= half + 1; }
		else n = half;
	}
	return NULL;
}

/* DOS-isms some DOOM sources expect; weak so that an engine-provided definition wins. */
__attribute__((weak)) char *strupr(char *s)
{
	for(char *p = s; *p; p++) if(*p >= 'a' && *p <= 'z') *p -= 32;
	return s;
}

__attribute__((weak)) char *strlwr(char *s)
{
	for(char *p = s; *p; p++) if(*p >= 'A' && *p <= 'Z') *p += 32;
	return s;
}
