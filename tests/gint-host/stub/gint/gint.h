/* tests/gint-host: stub of <gint/gint.h>; a world switch is a plain call */
#ifndef STUB_GINT
#define STUB_GINT
typedef struct { int (*fn)(void *); void *arg; } gint_call_t;
/* Like gint's, with zero or one argument */
#define GINT_CALL_1(f, a, ...) ((gint_call_t){ (int (*)(void *))(void (*)(void))(f), (void *)(a) })
#define GINT_CALL(...) GINT_CALL_1(__VA_ARGS__, 0, 0)
static inline int gint_world_switch(gint_call_t c) { return c.fn(c.arg); }
#endif
