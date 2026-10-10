/* tests/gint-host: stub of <gint/kmalloc.h>; arenas are simulated by the
   tests (sim = statistics returned by kmalloc_get_gint_stats) */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t free_memory; } kmalloc_gint_stats_t;
struct kmalloc_stats { int live_blocks; };
typedef struct kmalloc_arena_t {
  void *(*malloc)(size_t, void *);
  void *(*realloc)(void *, size_t, void *);
  void (*free)(void *, void *);
  void *(*malloc_max)(size_t *, void *);
  char const *name;
  void *start, *end;
  void *data;
  bool is_default;
  struct kmalloc_stats stats;
  kmalloc_gint_stats_t sim;
} kmalloc_arena_t;
kmalloc_arena_t *kmalloc_get_arena(char const *name);
void kmalloc_remove_arena(kmalloc_arena_t *a);
bool kmalloc_add_arena(kmalloc_arena_t *a);
void kmalloc_init_arena(kmalloc_arena_t *a, bool s);
void *kmalloc_max(size_t *size, char const *name);
void kfree(void *p);
void *krealloc(void *p, size_t s);
kmalloc_gint_stats_t *kmalloc_get_gint_stats(kmalloc_arena_t *a);
