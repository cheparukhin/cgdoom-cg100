/* CGDoom: tiny portable profiler over PL_GetMicros().

   Same macro names as gint's libprof (prof_make/prof_enter/prof_leave/
   prof_time) so that the call sites from CGDoom stay unchanged. Contexts are
   recursion-aware: only the outermost enter/leave pair counts. Times are in
   microseconds; PL_GetMicros() wraps, which unsigned arithmetic absorbs.

   Define CGD_NO_PROFILING to compile all measurements out. */

#ifndef CGD_PROF_H
#define CGD_PROF_H

#include "platform.h"
#include <stdint.h>

typedef struct prof_t
{
    uint32_t rec;
    uint32_t elapsed;
} prof_t;

/* prof_make(): Create a new (cleared) context */
#define prof_make() ((prof_t){ 0, 0 })

#ifndef CGD_NO_PROFILING

/* prof_enter(): Start counting time for a context */
#define prof_enter(prof) do { \
    if(!(prof).rec++) (prof).elapsed -= PL_GetMicros(); \
} while(0)

/* prof_leave(): Stop counting time for a context */
#define prof_leave(prof) do { \
    if(!--(prof).rec) (prof).elapsed += PL_GetMicros(); \
} while(0)

#else

#define prof_enter(prof) do { (void)(prof); } while(0)
#define prof_leave(prof) do { (void)(prof); } while(0)

#endif /* CGD_NO_PROFILING */

/* prof_time(): Time spent in a context, in microseconds. Only valid when the
   context is not currently executing. */
#define prof_time(prof) ((uint32_t)(prof).elapsed)

#endif /* CGD_PROF_H */
