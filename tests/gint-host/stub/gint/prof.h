/* tests/gint-host: stub of <gint/prof.h> (time.c; provided by test_time.c) */
#ifndef STUB_PROF
#define STUB_PROF
#include <stdint.h>
int prof_init(void);
void prof_quit(void);
extern uint32_t volatile *prof_tcnt;
#endif
