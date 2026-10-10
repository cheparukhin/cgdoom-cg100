/* tests/gint-host: stub of <gint/cpu.h> (input.c: sleep() is provided by
   the test and advances the simulated clock; time.c: the interrupt-
   cancellable sleep is provided by test_time.c) */
#ifndef STUB_CPU
#define STUB_CPU
#include <stdint.h>
void sleep(void);
typedef uint8_t cpu_csleep_t[20];
void cpu_csleep_init(cpu_csleep_t *ics);
void cpu_csleep(cpu_csleep_t *ics);
void cpu_csleep_cancel(cpu_csleep_t *ics);
#endif
