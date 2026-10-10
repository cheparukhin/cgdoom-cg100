/* Public interface of the sh-elf-run harness runtime (src/sim).
   Program code normally only needs standard C (fxlibc); these are the extras. */
#ifndef SIM_H
#define SIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- simulator syscall ABI (newlib/libgloss for SH: "trapa #34") -------------------------------
   r4 = syscall number, r5/r6/r7 = arguments; result in r0, errno (newlib numbering) in r1. */
enum {
	SIM_SYS_exit = 1,  SIM_SYS_read = 3,  SIM_SYS_write = 4,  SIM_SYS_open = 5,
	SIM_SYS_close = 6, SIM_SYS_unlink = 10 /* unsupported by the simulator */,
	SIM_SYS_lseek = 19, SIM_SYS_time = 23, SIM_SYS_stat = 38,
	SIM_SYS_argc = 172, SIM_SYS_argnlen = 173, SIM_SYS_argn = 174,
	SIM_SYS_counters = 0x7f00, SIM_SYS_pcprof = 0x7f01, SIM_SYS_mark = 0x7f02   /* sim64 extension */
};
int sim_syscall(int nr, int a, int b, int c);   /* raw; returns r0 (errno is set when r0 < 0) */

/* ---- statistics (need the sim64 simulator, see README; on a stock sh-elf-run they fail with -1) -----
   Counters are cumulative since the start of the program. */
struct sim_counters { uint64_t insts, cycles, stalls, memstalls; };
int  sim_counters(struct sim_counters *out);   /* 0 on success, -1 if the simulator lacks the extension */
int  sim_mark(int id);                         /* prints "SIMMARK <id> <insts> <cycles>" on stderr */
int  sim_pcprof(int on);                       /* pause (0) / resume (1) the SIM_PCPROF profile */

/* Virtual microsecond clock for PL_GetMicros() in sim builds: cycles / SIM_CYCLES_PER_US (default 118,
   an SH7305 at its stock clock), so it is deterministic. Falls back to the host's 1 s time() without
   the sim64 extension. Wraps like a uint32_t. */
#ifndef SIM_CYCLES_PER_US
#define SIM_CYCLES_PER_US 118
#endif
uint32_t sim_micros(void);

/* ---- process ---------------------------------------------------------------------------------- */
void sim_exit(int status) __attribute__((noreturn));  /* flush stdout/stderr, then exit(status) */

/* ---- heap (libc-glue.c) -------------------------------------------------------------------------
   The heap is the region [__heap_start, __heap_end) of sim.ld. */
struct sim_heap_info {
	size_t arena;        /* total bytes managed by the allocator */
	size_t used;         /* bytes in allocated chunks (including 4 bytes of header per chunk) */
	size_t peak;         /* high-water mark of 'used' */
	size_t free;         /* bytes in free chunks */
	size_t largest;      /* largest free chunk (the biggest malloc() that can succeed is ~4 less) */
	unsigned nalloc, nfree_calls, nchunks_free;
};
void sim_heap_info(struct sim_heap_info *out);
int  sim_heap_check(void);        /* walk and verify the whole heap; returns 0 if consistent */

void  *memalign(size_t alignment, size_t size);          /* alignment: power of two */
void  *aligned_alloc(size_t alignment, size_t size);
int    posix_memalign(void **out, size_t alignment, size_t size);
size_t malloc_usable_size(void *ptr);

/* ---- memory map symbols (sim.ld) ----------------------------------------------------------------- */
extern char __heap_start[], __heap_end[], __stack_limit[], __sim_stack_top[], __bss_start[], __bss_end[];

/* ---- program arguments (also passed to main) -------------------------------------------------- */
extern int    sim_argc;
extern char **sim_argv;

#ifdef __cplusplus
}
#endif
#endif
