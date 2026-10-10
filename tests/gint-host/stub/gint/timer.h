/* tests/gint-host: stub of <gint/timer.h> (time.c; provided by test_time.c) */
#ifndef STUB_TIMER
#define STUB_TIMER
#include <stdint.h>
#include <gint/gint.h>
enum { TIMER_ANY = -1, TIMER_TMU = -2, TIMER_ETMU = -3 };
enum { TIMER_CONTINUE = 0, TIMER_STOP = 1 };
int timer_configure(int timer, uint64_t delay_us, gint_call_t callback);
void timer_start(int timer);
void timer_stop(int timer);
#endif
