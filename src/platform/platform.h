/* CGDoom platform API (PL_*).

   The portable engine (src/engine) reaches the platform only through these
   functions. Implementations: src/gint (fx-CG100/CG50) and src/host (macOS
   host, also built for the SH simulator with -DPL_SIM). See notes/design.md §3.
   All sizes and offsets are in bytes.

   Additions to design.md §3 (documented here; all header-only except
   PL_WadIsSlow):
   - PL_KEY_BYTE/PL_KEY_BIT/PL_KeySet/PL_KeyDown: layout helpers for the
     12-byte keyboard state of PL_ScanKeyboard.
   - Precise semantics for PL_FileRead (partial reads) and PL_WadRead.
   - PL_WadIsSlow (engine step 3), see below. */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/* ---- Fatal errors / logging ---------------------------------------------- */

/* Show msg, wait for a key, clean exit. Never returns. */
void PL_Fatal(const char *msg) __attribute__((noreturn));
/* Debug log (host: stderr; gint: no-op or fxlink). */
void PL_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Time ------------------------------------------------------------------ */

/* 35 Hz monotonic counter since start (TICRATE). */
int      PL_GetTics(void);
/* Free-running microsecond counter (wraps) for FPS/profiling. */
uint32_t PL_GetMicros(void);
/* Sleep/idle until PL_GetTics() >= until (host: advances its virtual clock). */
void     PL_WaitTics(int until);

/* ---- Memory ----------------------------------------------------------------- */

/* Fill up to `max` zones for Z_Zone (largest first). Returns count. Each zone
   must be 4-byte aligned, cached RAM. Platform keeps enough heap for itself
   and for malloc(). */
int PL_GetZones(void **base, int *size, int max);

/* ---- Video ------------------------------------------------------------------ */

/* The engine renders 320x200 8-bit into screens[0] (cached RAM, 32-byte
   aligned). */
void PL_SetPalette(const uint16_t pal565[256]);  /* gamma already applied */
void PL_FinishUpdate(const uint8_t *fb);         /* present fb (may return
                                                    before the transfer ends) */
void PL_VideoReset(void);                        /* redraw borders etc. after
                                                    errors / file I/O */

/* ---- Input ------------------------------------------------------------------ */

/* Physical keys are matrix codes (row<<4 | (7-col)), the KEYCODE_* constants
   of cgdoom-kbd.h. The state is CGDoom's layout: one byte per row as the
   KEYSC registers are read as big-endian 16-bit words, i.e. key `code` is
   bit PL_KEY_BIT(code) of byte PL_KEY_BYTE(code). */
void PL_ScanKeyboard(uint8_t state[12]);

#define PL_KEY_BYTE(code) (((code) >> 4) ^ 1)
#define PL_KEY_BIT(code)  (0x80 >> ((code) & 7))

static inline void PL_KeySet(uint8_t state[12], int code)
{
    if(code >= 0 && PL_KEY_BYTE(code) < 12)
        state[PL_KEY_BYTE(code)] |= PL_KEY_BIT(code);
}
static inline int PL_KeyDown(const uint8_t state[12], int code)
{
    if(code < 0 || PL_KEY_BYTE(code) >= 12)
        return 0;
    return (state[PL_KEY_BYTE(code)] & PL_KEY_BIT(code)) != 0;
}

/* ---- WAD access -------------------------------------------------------------- */

/* Open the IWAD; returns its size in bytes, <0 on error. */
int         PL_WadOpen(const char *name);
void        PL_WadClose(void);
/* Read [offset, offset+size) into dst; returns bytes read (== size unless out
   of range or I/O error). Gathers fragments. */
int         PL_WadRead(void *dst, int offset, int size);
/* Zero-copy pointer to [offset, offset+size) if the range is contiguous in
   readable (cached) memory, else NULL. The engine only uses it for lumps;
   pointer stays valid until PL_WadClose. */
const void *PL_WadMap(int offset, int size);
/* True if p was returned by PL_WadMap (never freed by the engine). */
int         PL_IsWadPointer(const void *p);
/* (Step 3 addition.) 1 if PL_WadMap pointers are in slow memory such as NOR
   flash (calculator flash block-map or BFile modes), so the engine should
   prefer copying hot lumps into the zone (it then copies mappable lumps while
   the zone keeps a free reserve, and precaches each level's graphics); 0 if
   they are as fast as the zone (WAD image in RAM, host). Must not change
   between PL_WadOpen and PL_WadClose; the engine reads it once at start.
   The engine has a weak default returning 0 (cgdoom.c) until a platform
   defines it. */
int         PL_WadIsSlow(void);

/* ---- Small files (config, saves, demos) -------------------------------------- */

/* Names are plain ASCII file names in the storage root (e.g. "doom1.cfg",
   "doom1_0.dsg"). */
int  PL_FileSize(const char *name);                       /* <0 if missing */
/* Read min(size, file size) bytes from the start of the file into buf.
   Returns bytes read, <0 on error (missing file). */
int  PL_FileRead(const char *name, void *buf, int size);
int  PL_FileWrite(const char *name, const void *buf, int size); /* create or
                                                     overwrite; <0 on error */
int  PL_FileDelete(const char *name);

#endif /* PLATFORM_H */
