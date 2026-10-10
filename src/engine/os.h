/* CGDoom engine: common platform-facing header.

   The engine includes only "platform.h" (src/platform) and standard C
   headers. This header gathers them and defines a few engine-wide helpers. */

#ifndef CGDOOM_OS_H
#define CGDOOM_OS_H

#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

void *CGD_malloc(int size);
void *CGD_calloc(int size);
void *CGD_realloc(void *p, int size);

int strnicmp(const char *s1, const char *s2, int iLen);

/* Formats the message and calls PL_Fatal(); never returns. */
void I_Error(const char *error, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

/* Debug output from the original code. Compiled out unless CGD_VERBOSE is
   defined, in which case it goes to PL_Log(). (stdio.h is included above, so
   its printf() prototype is not affected by this macro.) */
#ifdef CGD_VERBOSE
# define printf(...) PL_Log(__VA_ARGS__)
#else
# define printf(...) ((void)0)
#endif

/* Assertions follow RANGECHECK, which the build defines (-DRANGECHECK):
   ON for host debug builds, OFF for calculator/release builds. */
#ifdef RANGECHECK
# define ASSERT(x) ((x) ? (void)0 : \
    I_Error("Assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__))
#else
# define ASSERT(x) ((void)0)
#endif

/* Vanilla leftovers: variables that are set but never read */
#define CGD_UNUSED __attribute__((unused))

/* For code that intentionally indexes the pads around an array (visplane
   top[-1]/top[SCREENWIDTH]); only affects clang's UBSan bounds check */
#if defined(__clang__)
# define CGD_NO_SANITIZE_BOUNDS __attribute__((no_sanitize("bounds")))
#else
# define CGD_NO_SANITIZE_BOUNDS
#endif

#endif /* CGDOOM_OS_H */
