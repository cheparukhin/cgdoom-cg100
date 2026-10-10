/* Host platform (src/host): headless, deterministic PL_* implementation.

   Built with clang for macOS (make host) and with sh-elf-gcc -DPL_SIM for the
   sh-elf-run simulator (make sim, src/sim). Only ISO C stdio/stdlib is used;
   host-only bits (real-time clock) are behind #ifndef PL_SIM. */

#ifndef HOST_H
#define HOST_H

#include <stdint.h>
#include <stdio.h>

/* Configuration, filled by main() before the engine starts */
typedef struct {
    const char *data_dir;   /* directory for config/saves/demos ("." default) */
    int fragment;           /* -fragment N: PL_WadMap fails across N-byte
                               block boundaries (0 = never) */
    int zone_kib[8];        /* zone sizes in KiB */
    int zone_count;
    int max_tics;           /* quit after this many game tics (0 = no limit) */
    FILE *crc_file;         /* per-frame CRC output (NULL = none) */
    const char *shot_prefix;/* PPM screenshots (NULL = none) */
    int shot_every;         /* every N frames (0 = last frame only) */
    /* Virtual clock (step 3): each presented frame advances it by
       frame_mtics, each game tic (PL_ScanKeyboard) by tic_mtics, in 1/1000
       tic (defaults 1000 and 0: one tic per frame, singletics). Lets tests
       model a slow renderer deterministically (-rendercost/-ticcost). */
    int frame_mtics;
    int tic_mtics;
    int realtime;           /* -realtime: wall-clock 35 Hz (host only, not
                               deterministic) */
    int slow_wad;           /* -slowwad: PL_WadIsSlow() returns 1 */
} HostConfig;

extern HostConfig host;

/* Counters maintained by the platform */
extern int host_frames;     /* PL_FinishUpdate calls */
extern int host_tics;       /* PL_ScanKeyboard calls (= game tics started) */

/* Called by PL_ScanKeyboard at the start of every game tic (host_main.c) */
void Host_TicHook(int tic);

/* Write the current screen as a PPM file (palette-expanded) */
int Host_WriteScreenshot(const char *path);
/* Write the final screenshot if one is pending (-shot without -shotevery) */
void Host_Finish(void);

/* Frame timing (host only, real time; zero on PL_SIM) */
extern uint64_t host_frame_us_total;
extern uint32_t host_frame_us_max;

/* Input scripts (host_input.c) */
int  Input_Load(const char *spec);   /* "builtin:<name>" or a file; <0 error */
void Input_GetKeys(int tic, uint8_t state[12]);
void Input_ListBuiltins(FILE *fp);

#endif /* HOST_H */
