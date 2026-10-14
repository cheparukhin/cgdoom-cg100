/* Host implementation of the PL_* platform API (see src/platform/platform.h).

   Deterministic time: the 35 Hz clock is virtual and advances by one tic per
   presented frame (PL_FinishUpdate) or when the engine sleeps (PL_WaitTics),
   so every run of a scenario renders exactly the same frames. */

#include "platform.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef PL_SIM
#include <time.h>
#endif

/* Engine entry points used by the platform */
extern void I_Quit(void);

HostConfig host = {
    .data_dir = ".",
    .fragment = 0,
    .zone_kib = { 16384 },
    .zone_count = 1,
    .max_tics = 0,
    .crc_file = NULL,
    .shot_prefix = NULL,
    .shot_every = 0,
    .frame_mtics = 1000,
    .tic_mtics = 0,
    .realtime = 0,
    .slow_wad = 0,
};

int host_frames = 0;
int host_tics = 0;
uint64_t host_frame_us_total = 0;
uint32_t host_frame_us_max = 0;

//---
// Errors and logging
//---

void PL_Fatal(const char *msg)
{
    fflush(stdout);
    fprintf(stderr, "FATAL: %s\n", msg);
    if(host.crc_file)
        fflush(host.crc_file);
    exit(2);
}

void PL_Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

//---
// Time
//---

/* Virtual clock: whole tics plus a fraction in 1/1000 tic (only used with
   -rendercost / -ticcost; by default the clock moves in whole tics) */
static int virtual_tics = 0;
static int virtual_mtics = 0;

static void advance_clock(int mtics)
{
    if(mtics == 1000) {
        virtual_tics++;
        return;
    }
    virtual_mtics += mtics;
    virtual_tics += virtual_mtics / 1000;
    virtual_mtics %= 1000;
}

#ifndef PL_SIM
/* -realtime: microseconds of the monotonic clock at the first query */
static uint64_t realtime_start = 0;

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int realtime_tics(void)
{
    uint64_t now = monotonic_us();
    if(!realtime_start)
        realtime_start = now;
    return (int)((now - realtime_start) * 35 / 1000000);
}
#endif

int PL_GetTics(void)
{
#ifndef PL_SIM
    if(host.realtime)
        return realtime_tics();
#endif
    return virtual_tics;
}

void PL_WaitTics(int until)
{
#ifndef PL_SIM
    if(host.realtime) {
        while(realtime_tics() < until) {
            struct timespec ts = { 0, 1000000 };    /* 1 ms */
            nanosleep(&ts, NULL);
        }
        return;
    }
#endif
    if(virtual_tics < until) {
        virtual_tics = until;
        virtual_mtics = 0;
    }
}

uint32_t PL_GetMicros(void)
{
#ifndef PL_SIM
    return (uint32_t)monotonic_us();
#else
    /* Simulator: no clock; nominal 1/35 s per virtual tic */
    return (uint32_t)virtual_tics * 28571u + (uint32_t)virtual_mtics * 28u;
#endif
}

//---
// Memory
//---

int PL_GetZones(void **base, int *size, int max)
{
    int n = 0;
    for(int i = 0; i < host.zone_count && n < max; i++) {
        int bytes = host.zone_kib[i] * 1024;
        void *p = malloc(bytes);
        if(!p) {
            fprintf(stderr, "cannot allocate %d KiB zone\n", host.zone_kib[i]);
            continue;
        }
        /* Deterministic content (the engine must not depend on it anyway) */
        memset(p, 0, bytes);
        base[n] = p;
        size[n] = bytes;
        n++;
    }
    return n;
}

//---
// Video
//---

static uint16_t palette[256];
static const uint8_t *last_fb = NULL;

/* CRC-32 (IEEE 802.3, reflected), table-driven */
static uint32_t crc_table[256];

static void crc_init(void)
{
    for(uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for(int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while(n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void PL_SetPalette(const uint16_t pal565[256])
{
    memcpy(palette, pal565, sizeof palette);
}

void PL_VideoReset(void)
{
}

static void expand565(uint16_t c, uint8_t *rgb)
{
    int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

int Host_WriteScreenshot(const char *path)
{
    if(!last_fb)
        return -1;
    FILE *fp = fopen(path, "wb");
    if(!fp)
        return -1;
    fprintf(fp, "P6\n320 200\n255\n");
    uint8_t row[320 * 3];
    for(int y = 0; y < 200; y++) {
        for(int x = 0; x < 320; x++)
            expand565(palette[last_fb[320 * y + x]], row + 3 * x);
        fwrite(row, 1, sizeof row, fp);
    }
    fclose(fp);
    return 0;
}

static void write_shot(int frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s%05d.ppm", host.shot_prefix, frame);
    if(Host_WriteScreenshot(path) < 0)
        fprintf(stderr, "cannot write %s\n", path);
}

void Host_Finish(void)
{
    if(host.shot_prefix && host.shot_every <= 0 && host_frames > 0)
        write_shot(host_frames - 1);
    if(host.crc_file)
        fflush(host.crc_file);
}

void PL_FinishUpdate(const uint8_t *fb)
{
    static int crc_ready = 0;
#ifndef PL_SIM
    static uint32_t last_us = 0;
#endif

    if(!crc_ready) {
        crc_init();
        crc_ready = 1;
    }
    last_fb = fb;

    if(host.crc_file) {
        /* Palette as big-endian bytes so host and SH sim CRCs agree */
        uint8_t palbytes[512];
        for(int i = 0; i < 256; i++) {
            palbytes[2*i] = palette[i] >> 8;
            palbytes[2*i+1] = palette[i] & 0xff;
        }
        uint32_t c_fb = crc32_update(0, fb, 320 * 200);
        uint32_t c_pal = crc32_update(0, palbytes, sizeof palbytes);
        fprintf(host.crc_file, "%d %d %08x %08x\n", host_frames, host_tics,
            (unsigned)c_fb, (unsigned)c_pal);
    }

    if(host.shot_prefix && host.shot_every > 0
        && host_frames % host.shot_every == 0)
        write_shot(host_frames);

#ifndef PL_SIM
    uint32_t now = PL_GetMicros();
    if(host_frames > 0) {
        uint32_t dt = now - last_us;
        host_frame_us_total += dt;
        if(dt > host_frame_us_max)
            host_frame_us_max = dt;
    }
    last_us = now;
#endif

    host_frames++;
    advance_clock(host.frame_mtics);
}

//---
// Input
//---

void PL_ScanKeyboard(uint8_t state[12])
{
    memset(state, 0, 12);
    host_tics++;

    /* Quit cleanly after max_tics full tics (the engine checks the quit flag
       after running the tic during which it is set) */
    if(host.max_tics > 0 && host_tics > host.max_tics) {
        I_Quit();
        return;
    }
    Host_TicHook(host_tics - 1);
    Input_GetKeys(host_tics - 1, state);
    if(host.tic_mtics)
        advance_clock(host.tic_mtics);
}

//---
// WAD access: whole file in RAM
//---

static uint8_t *wad_data = NULL;
static int wad_size = 0;

int PL_WadOpen(const char *name)
{
    PL_WadClose();

    FILE *fp = fopen(name, "rb");
    if(!fp)
        return -1;
    if(fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if(size <= 0 || size > 0x7fffffffL) {
        fclose(fp);
        return -1;
    }

    wad_data = malloc(size);
    if(!wad_data) {
        fclose(fp);
        return -1;
    }
    if(fread(wad_data, 1, size, fp) != (size_t)size) {
        fclose(fp);
        free(wad_data);
        wad_data = NULL;
        return -1;
    }
    fclose(fp);
    wad_size = (int)size;
    return wad_size;
}

void PL_WadClose(void)
{
    free(wad_data);
    wad_data = NULL;
    wad_size = 0;
}

int PL_WadRead(void *dst, int offset, int size)
{
    if(!wad_data || offset < 0 || size < 0 || offset > wad_size)
        return -1;
    if(size > wad_size - offset)
        size = wad_size - offset;
    memcpy(dst, wad_data + offset, size);
    return size;
}

const void *PL_WadMap(int offset, int size)
{
    if(!wad_data || offset < 0 || size < 0 || offset > wad_size
        || size > wad_size - offset)
        return NULL;

    /* Simulated fragmentation: ranges that cross a block boundary are not
       contiguous, so the engine must copy them (exercises the copy path) */
    if(host.fragment > 0 && size > 0
        && offset / host.fragment != (offset + size - 1) / host.fragment)
        return NULL;

    return wad_data + offset;
}

int PL_WadIsSlow(void)
{
    return host.slow_wad;
}

int PL_IsWadPointer(const void *p)
{
    const uint8_t *b = p;
    return wad_data && b >= wad_data && b < wad_data + wad_size;
}

//---
// Small files, relative to host.data_dir
//---

static void make_path(char *path, size_t n, const char *name)
{
    snprintf(path, n, "%s/%s", host.data_dir, name);
}

int PL_FileSize(const char *name)
{
    char path[512];
    make_path(path, sizeof path, name);
    FILE *fp = fopen(path, "rb");
    if(!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    return (size < 0 || size > 0x7fffffffL) ? -1 : (int)size;
}

int PL_FileRead(const char *name, void *buf, int size)
{
    char path[512];
    make_path(path, sizeof path, name);
    FILE *fp = fopen(path, "rb");
    if(!fp)
        return -1;
    size_t rc = fread(buf, 1, size, fp);
    fclose(fp);
    return (int)rc;
}

/* The engine must only create strict 8.3 names (enforced by the calculator
   platform too): on the host a violation is fatal so that tests catch it. */
int CGD_ValidName83(const char *name);
static void check_83(const char *op, const char *name)
{
    if(!CGD_ValidName83(name)) {
        char msg[160];
        snprintf(msg, sizeof msg, "%s: '%s' is not a strict 8.3 name", op,
            name ? name : "(null)");
        PL_Fatal(msg);
    }
}

int PL_FileWrite(const char *name, const void *buf, int size)
{
    char path[512];
    check_83("PL_FileWrite", name);
    make_path(path, sizeof path, name);
    FILE *fp = fopen(path, "wb");
    if(!fp)
        return -1;
    size_t rc = fwrite(buf, 1, size, fp);
    if(fclose(fp) != 0 || rc != (size_t)size)
        return -1;
    return size;
}

int PL_FileDelete(const char *name)
{
    char path[512];
    check_83("PL_FileDelete", name);
    make_path(path, sizeof path, name);
    return remove(path) == 0 ? 0 : -1;
}
