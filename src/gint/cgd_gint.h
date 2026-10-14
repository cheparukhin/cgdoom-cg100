/* CGDoom gint platform (fx-CG100 / Graph Math+ under MPM 1.1): internal
   interfaces shared by the files of src/gint.

   The public side of this platform is src/platform/platform.h (PL_*). This
   header only declares what the gint files need from each other:

     sys.c       model / OS check, OS call table (BFile, block map)
     tlb.c       UTLB snapshot, repair after world switches, TLB-miss handler
     memory.c    heap arenas, PL_GetZones
     files.c     world-switched BFile helpers, PL_File*, delayed writes
     wad.c       PL_Wad* (RAM image / 4 KiB block map / BFile)
     video.c     PL_SetPalette / PL_FinishUpdate / PL_VideoReset
     input.c     PL_ScanKeyboard, raw-scan UI keys
     keymaps.c   fx-CG100 keymaps, key names, presets
     time.c      PL_GetTics / PL_GetMicros / PL_WaitTics
     clock.c     OS clock / F4 overclock
     ui.c        launcher drawing helpers
     diag.c      diagnostics screen and microbenchmarks
     stack.S     run a function on a private stack
     main.c      entry point, launcher, game start, exit path, PL_Fatal

   Nothing here is visible to the engine. */

#ifndef CGD_GINT_H
#define CGD_GINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <gint/bfile.h>

#include "platform.h"

/* ---- Tunables -------------------------------------------------------------- */

/* Heap left free for the engine's malloc() after the zones are carved out
   (screens ~266 KB, lumpinfo, lumpcache, delayed-write copies...). */
#ifndef CGD_MALLOC_RESERVE
#define CGD_MALLOC_RESERVE        (768 << 10)
#endif
/* Smallest block worth turning into a zone. */
#define CGD_MIN_ZONE              (32 << 10)
/* The WAD is copied to RAM (the primary path) when the image fits in the
   largest free block and at least this much heap remains afterwards for
   the zone and malloc (host runs show ~1-1.7 MB zone peaks on 64-bit). */
#ifndef CGD_MIN_GAME_ZONE
#define CGD_MIN_GAME_ZONE         (1 << 20)
#endif
#ifndef CGD_WAD_RAM_MARGIN
#define CGD_WAD_RAM_MARGIN        (CGD_MALLOC_RESERVE + CGD_MIN_GAME_ZONE)
#endif
/* Refuse to start with less heap than this in total: mpm.bin 1.1 leaves
   about 5.5 MiB (by calculation), the older builds at most about 2 MiB, so
   they are refused rather than run untested */
#ifndef CGD_MIN_TOTAL_HEAP
#define CGD_MIN_TOTAL_HEAP        (3 << 20)
#endif
/* Private stack for the game; 0 disables it. */
#ifndef CGD_PRIVATE_STACK_SIZE
#define CGD_PRIVATE_STACK_SIZE    (128 << 10)
#endif

//---
// sys.c: model and OS check, OS call table
//---

/* Table of the OS functions used by the platform. Every function must be
   called inside gint_world_switch() (see gfs_os_call()). */
typedef struct {
	int (*open)(uint16_t const *path, int mode, int zero);
	int (*close)(int fd);
	int (*size)(int fd);
	int (*read)(int fd, void *buf, int size, int whence);
	int (*write)(int fd, void const *buf, int size);
	int (*create)(uint16_t const *path, int type, int *size);
	int (*remove)(uint16_t const *path);
	int (*find_first)(uint16_t const *pattern, int *shandle,
		uint16_t *found, struct BFile_FileInfo *info);
	int (*find_next)(int shandle, uint16_t *found,
		struct BFile_FileInfo *info);
	int (*find_close)(int shandle);
	/* Bfile_GetBlockAddress (NULL: no block map, tests only) */
	int (*block)(int fd, int offset, void **ptr);
} gos_api_t;

extern char        g_os_version[11]; /* string at 0x80020020 */
extern gos_api_t   g_os;            /* valid if gsys_supported() */
extern uint32_t    g_entry_r15;     /* stack pointer at main() entry */

void gsys_init(void);
/* fx-CG100 with exact OS 02.00.0000 (API 2) or 02.20.0000 (API 3).
   Anything else: no platform OS call, MMU or heap change. */
bool gsys_supported(void);
char const *gsys_model_name(void);

//---
// tlb.c: protect our code pages against UTLB replacement by the OS
//---

/* safe: safe start (DEL held at launch): the miss handler
   only, no P2 executor (no pinning, no ldtlb), as when the executor cannot
   be set up */
void gtlb_init(bool safe);
/* Verify the snapshot after a world switch; rewrite missing pages. */
void gtlb_repair(void);
/* Exit normalisation (unpin, URB restore). Called from main.c's destructor
   on every exit path, after gwad_quit(). */
void gtlb_quit(void);
typedef struct {
	int pages;          /* our 64 KiB pages found in the UTLB snapshot */
	bool handler;       /* own TLB-miss handler installed at VBR+0x400 */
	int checks;         /* gtlb_repair() calls */
	int repairs;        /* entries rewritten by gtlb_repair() */
	uint32_t *misses;   /* counter incremented by the miss handler */
	int pinned;         /* image pages pinned above MMUCR.URB (0: none) */
	int urb_orig;       /* MMUCR.URB found at startup (MPM: 54) */
	int urb;            /* MMUCR.URB while pinned */
	int urb_fixes;      /* times gtlb_repair() found URB changed */
	int last_repairs;   /* entries rewritten by the last gtlb_repair() */
	bool executor;      /* P2 UTLB executor available (pin/repair/unpin) */
	bool safe;          /* safe start: executor not used */
} gtlb_stats_t;
extern gtlb_stats_t gtlb_stats;

/* tlbplan.c: UTLB layout planning, pure logic (host-tested in
   tests/gint-host). Pages are the 64 KiB pages of [0x00300000,
   0x00700000); slots are the 64 UTLB entries. */
#define GTLB_SLOTS      64
#define GTLB_PAGES      64
#define GTLB_ROM_BASE   0x00300000u
#define GTLB_ADDR_V     0x00000100u   /* address array: valid bit */
#define GTLB_DATA_V     0x00000100u   /* data array / PTEL: valid bit */
#define GTLB_ADDR_VPN   0xfffffc00u
#define GTLB_DATA_PPN   0x1ffffc00u
#define GTLB_VPN64      0xffff0000u   /* 64 KiB page of an address */
/* Keep at least this many replaceable slots for the OS when pinning */
#define GTLB_MIN_REPLACEABLE 16
/* Capacity of the operation buffer handed to cgd_tlb_apply() */
#define GTLB_MAX_OPS    (GTLB_PAGES + 8)

typedef struct {
	uint32_t pteh;      /* VPN | ASID (0 for "not one of our pages") */
	uint32_t ptel;      /* PTEL value; 0 = page not in the snapshot */
} gtlb_entry_t;

/* Copy of the UTLB address and data arrays (memory-mapped reads) */
typedef struct {
	uint32_t addr[GTLB_SLOTS];
	uint32_t data[GTLB_SLOTS];
} gtlb_view_t;
/* Read the UTLB, for the Diagnostics probe */
void gtlb_read_view(gtlb_view_t *v);

/* One UTLB write, performed by cgd_tlb_apply() (tlbh.S) as
   MMUCR.URC = slot; PTEH = pteh; PTEL = ptel; ldtlb. Before a valid load,
   the executor invalidates every OTHER valid slot that maps the same
   64 KiB page, so the UTLB never holds two entries for one address (a
   multiple hit resets the CPU). Layout known to tlbh.S: 12 bytes. */
typedef struct {
	uint32_t slot;
	uint32_t pteh;
	uint32_t ptel;
} gtlb_op_t;

/* Reference semantics of cgd_tlb_apply() for one op (planner, tests) */
void gtlb_view_apply(gtlb_view_t *v, gtlb_op_t const *op);
/* Plan pinning of image pages 0..n-1 into the n slots just below urb
   (slots urb-n .. urb-1); the caller then sets MMUCR.URB = urb-n. Updates
   v and slot[] (displaced pages get slot -1: reloaded on demand by the
   miss handler). Returns the number of ops, 0 if pinning was refused
   (nothing changed), and sets *new_urb. */
int gtlb_plan_pin(gtlb_view_t *v, gtlb_entry_t const *table, int8_t *slot,
	int n, int urb, gtlb_op_t *ops, int max_ops, int *new_urb);
/* Plan the repair after a world switch: every page with slot[p] >= 0 that
   is not in its slot is rewritten there, except unpinned pages
   (slot[p] < pin_lo) that the miss handler reloaded elsewhere. Pinned
   pages found elsewhere are moved back. Updates v; returns the op count. */
int gtlb_plan_repair(gtlb_view_t *v, gtlb_entry_t const *table,
	int8_t const *slot, int pin_lo, gtlb_op_t *ops, int max_ops);
/* Restrict slot[] to the image pages 0..n-1 (n <= 0: keep all): only
   they are repaired after world switches */
void gtlb_track_image(int8_t *slot, int n);
/* Plan the exit normalisation: every snapshot page p (slot_orig[p] >= 0)
   back in slot_orig[p] and in no other slot (the executor drops the other
   copies). Pages already alone in their slot are skipped. Updates v;
   returns the op count. */
int gtlb_plan_unpin(gtlb_view_t *v, gtlb_entry_t const *table,
	int8_t const *slot_orig, gtlb_op_t *ops, int max_ops);
/* MMUCR value with URB = urb and URC = 0 (other fields kept, TI clear) */
uint32_t gtlb_mmucr_with_urb(uint32_t mmucr, int urb);
static inline int gtlb_mmucr_urb(uint32_t mmucr) { return (mmucr >> 18) & 63; }

//---
// memory.c
//---

typedef struct {
	int zone_count;
	void *zone_base[8];
	int zone_size[8];
	int reserve;           /* bytes left for malloc */
	bool os_heap_removed;
	uint32_t rom_phys;     /* mmu_translate(0x00300000) */
	uint32_t uram_phys;    /* mmu_translate(0x08100000) */
	uint32_t image_size;   /* loaded binary size (from linker symbols) */
} gmem_stats_t;

extern gmem_stats_t gmem_stats;
void gmem_init(void);
/* Total free bytes in the default gint arenas (excluding the OS heap) */
size_t gmem_total_free(void);
/* Largest block currently available in the default gint arenas */
size_t gmem_largest_free(void);
/* Iterate known arenas for diagnostics: returns false past the end */
bool gmem_arena_info(int i, char const **name, uintptr_t *start,
	uintptr_t *end, size_t *free_bytes, size_t *largest);

//---
// files.c: world-switched BFile helpers, delayed writes
//---

#define GFS_NAME_MAX  13          /* strict 8.3 name, including NUL */
#define GFS_LIST_NAME 40          /* names found on storage (read/list) */
#define GFS_PATH_MAX  (7 + GFS_LIST_NAME)
/* PL_FileWrite() refuses larger sizes before touching the file (defence in
   depth): every legitimate write (settings, saves) comes from RAM, and no
   RAM of this model is that large. A bogus size (such as a pointer
   difference with a NULL pointer, about 1.9e9) must never reach
   os_flush(), which removes the existing file before create() fails. */
#define GFS_WRITE_MAX (16 << 20)

typedef struct {
	char name[GFS_LIST_NAME];
	int size;
} gfs_entry_t;

/* Validate a strict 8.3 name (letters, digits, _ - ! # $ % ( ) @ ^) */
bool gfs_valid_83(char const *name);
/* Build u"\\\\fls0\\<name>" (any listed name; caller validates) */
bool gfs_make_path(uint16_t *path, int max, char const *name);

/* Run fn(arg) in the OS world (gint_world_switch). Waits for the LCD DMA,
   marks the LCD as dirty (the OS may draw its busy indicator), and repairs
   the UTLB afterwards. Every OS call of the platform goes through this. */
int gfs_os_call(int (*fn)(void *), void *arg);
extern int gfs_world_switches;

/* BFile_Read result check: Fugue returns the bytes read (MPM relies on it);
   the CASIOWIN convention returns the bytes remaining (gpSP accepts 0). */
bool gfs_read_ok(int rc, int size, int remaining_after);

/* P1 scratch area used for every path / small buffer given to the OS */
typedef struct {
	uint16_t path[GFS_PATH_MAX + 1];
	uint16_t found[512];
	struct BFile_FileInfo info;
	uint8_t small[64];
} gfs_scratch_t;
gfs_scratch_t *gfs_scratch(void);
/* True if p..p+size is a P1 address the OS can safely use */
bool gfs_is_p1(void const *p, size_t size);

/* List "*.wad" in the storage root in one world switch. gfs_list_more is
   set when the list was cut at its maximum. */
int gfs_list(gfs_entry_t *wads, int wad_max, int *wad_count);
extern bool gfs_list_more;
/* Failure result recorded when PL_FileWrite() cannot keep a copy for later
   and may not write at once (WAD block map live) */
#define GFS_ERR_NO_MEMORY  (-1000)

/* Write all pending writes/deletes in a single world switch. Returns the
   number of failed operations. Refuses while the WAD block map is live. */
int gfs_flush(void);
int gfs_pending_count(void);
int gfs_flush_failures(void);
/* How many of those failed writes left the previous version of their file
   in storage (a replacement that does not fit puts it back) */
int gfs_flush_kept(void);
/* Text for the exit screen if any write failed during the session (or
   flush_result, the exit gfs_flush()'s result, is non-zero); false if
   there is nothing to report */
bool gfs_failure_text(char *text, size_t size, int flush_result);
void gfs_drop_read_cache(void);

/* Name and result of the first failed operation of the last gfs_flush()
   with failures (false if there was none) */
bool gfs_last_failure(char name[GFS_NAME_MAX], int *result);
/* Count a file the caller could not even hand over (shown at exit) */
void gfs_note_failure(char const *name, int result);

//---
// wadnames.c: the WAD name given to the engine
//---

/* WAD base name (up to the first '.') for CGD_WADFileName: whole, never
   truncated (the engine hashes it into an 8.3 prefix) */
void gnames_wad_base(char *out, size_t size, char const *wadname);

//---
// wad.c
//---

typedef enum { GWAD_NONE = 0, GWAD_RAM, GWAD_MAP, GWAD_BFILE } gwad_mode_t;

/* The engine reads lumps with W_LUMP_PAD trailing WAD bytes (w_wad.h):
   PL_WadMap(position, size + pad), clamped to the WAD end. Every RAM-image
   segment is followed by this many true file bytes (zeros past the end).
   main.c checks GWAD_LUMP_PAD >= W_LUMP_PAD at compile time. */
#define GWAD_LUMP_PAD 136

typedef struct {
	gwad_mode_t mode;
	int size;
	/* RAM image */
	int ram_bytes;         /* RAM used by the image */
	int ram_segments;      /* lumps/segments loaded */
	int ram_skipped;       /* sound/music lumps left in storage */
	int ram_skipped_bytes;
	bool ram_failed;       /* RAM image attempted and failed */
	/* Block map */
	int blocks;            /* 4 KiB blocks in the file */
	int mapped;            /* blocks with a usable direct pointer */
	int raw_p1, raw_p2, raw_p0, raw_other, raw_error;
	int rej_unaligned;
	bool map_rejected;     /* map did not match the file: BFile reads */
	int runs, longest_run; /* physically contiguous runs (blocks) */
	uint32_t phys_min, phys_max;
	/* Common */
	int load_ms;           /* RAM load / block query time (RTC) */
	int map_hits, map_misses;   /* PL_WadMap results */
	int fallback_reads;    /* PL_WadRead served by world-switched BFile */
	int read_errors;
} gwad_stats_t;

extern gwad_stats_t gwad_stats;
/* PL_WadOpen() tries the RAM image first (default true); false makes it
   use the block map (Diagnostics storage probe, tests) */
extern bool gwad_ram_allowed;
bool gwad_map_live(void);
/* PL_WadOpen() failures, one per cause so the user can act on it, and
   their launcher text */
enum {
	GWAD_ERR_NAME = -1,        /* name too long / unusable characters */
	GWAD_ERR_OPEN = -2,        /* BFile open failed */
	GWAD_ERR_SHORT = -3,       /* < 12 bytes, header unreadable */
	GWAD_ERR_MAGIC = -4,       /* no IWAD/PWAD magic */
	GWAD_ERR_TRUNCATED = -5,   /* directory past the end of the file */
};
char const *gwad_error_text(int rc);
/* Re-query block addresses after a storage write */
void gwad_remap(void);
char const *gwad_mode_name(void);
/* Close the WAD file if it is still open (main.c's destructor: the abort()
   path skips PL_WadClose). Nothing else is freed. */
void gwad_quit(void);

//---
// video.c
//---

/* Allocate the RGB565 frame buffer (call early, before the zones) */
bool gvid_init(void);
/* False if gvid_init() failed: PL_FinishUpdate then writes every pixel
   with the CPU instead of the async DMA */
bool gvid_dma_available(void);
/* Wait for the frame DMA and give the full 396x224 window back (always
   before gint drawing + dupdate()). */
void gvid_release(void);
/* The OS may have drawn into GRAM (world switch): clear the borders before
   the next frame. */
void gvid_note_os_activity(void);
/* Clear the whole LCD to black (CPU writes) */
void gvid_clear_lcd(void);
/* Push one frame and wait for its DMA (diagnostics benchmark) */
void gvid_push_test(uint8_t const *fb);

/* Replaceable 8-bit -> RGB565 expansion. The presenter uses the _p32
   entry with a persistent 1 KiB table built by cgd_expand_build_pal32()
   in PL_SetPalette() (layout: pal32[k] = pal[k ^ 0x80] * 0x00010001). Weak
   C defaults in video.c; the SH4 versions (src/engine/sh4: expand_sh4.S,
   r_draw_sh4_c.c, same prototypes as cgd_sh4.h) replace them when linked.
   Contract (also of the plain cgd_expand_p8_rgb565 in expand_sh4.S):
   - dst[i] = pal[src[i]]; npixels % 16 == 0 (the _p32 entry writes nothing
     otherwise); src is 4-byte aligned (the asm accepts any alignment, the
     weak C reads words), big-endian: src[0] is the leftmost pixel and
     dst[0] its RGB565 value;
   - dst is 32-byte aligned and is EITHER cached SDRAM (P1, 0x8xxxxxxx) OR
     on-chip X/Y memory (0xe5xxxxxx, not cacheable; supported by the
     routines, but no caller uses it).
     For P1 dst every 32-byte line is allocated with movca.l and purged
     with ocbp once written (asm and weak C alike), so the data is in RAM
     on return and the caller only issues synco before a DMA reads it. For
     on-chip dst, plain stores only (no movca.l / ocb*);
   - no DSP repeat loops (ldrs/ldre), no DSP or MAC registers: safe in
     interrupt context (it runs in the foreground today). */
void cgd_expand_build_pal32(uint32_t *pal32, uint16_t const *pal);
void cgd_expand_p8_rgb565_p32(uint16_t *dst, uint8_t const *src,
	uint32_t const *pal32, int npixels);

//---
// input.c / keymaps.c
//---

/* Raw KEYSC scan (12 bytes, PL_ScanKeyboard layout) */
void ginp_scan(uint8_t state[12]);
bool ginp_any_key(void);
/* True if the emergency abort chord SHIFT+7+3+AC/ON is held in a scan
   (checked by time.c's tic interrupt; gint's own chord cannot fire, see
   input.c) */
bool ginp_abort_chord(uint8_t const st[12]);
/* Safe start: DEL held when the add-in starts. Read from a
   raw scan before any OS call or UTLB change. */
#define GINP_SAFE_START_KEY 0x44
bool ginp_safe_start(void);
/* Wait until every key is released (all KEYSC words read 0), at most
   GINP_RELEASE_TIMEOUT_US (or GINP_RELEASE_MAX_POLLS polls) */
#define GINP_RELEASE_TIMEOUT_US  3000000u
#define GINP_RELEASE_MAX_POLLS   2000
void ginp_wait_release(void);
/* True on a new key press: a key that was not down at entry, or one
   released and pressed again (a key held or stuck at entry does not
   count); false after timeout_ms */
bool ginp_wait_any_key(int timeout_ms);
/* Wait for a new key press and return its matrix code (AC/ON is 0x07);
   -1 after timeout_ms */
int ginp_wait_raw_key(int timeout_ms);

/* UI key events (from raw scans, by physical position) */
enum {
	GK_NONE = 0, GK_UP, GK_DOWN, GK_LEFT, GK_RIGHT,
	GK_OK,       /* OK or EXE */
	GK_BACK,     /* BACK */
	GK_DEL,
	GK_QUIT,     /* HOME */
	GK_DIGIT0,   /* GK_DIGIT0 + n for digit n */
};
/* Wait for a UI key. If ginp_idle_hook is set, once the keyboard has
   not changed for GINP_IDLE_MS (time.c's tic scan: an auto-repeating held
   arrow is no activity) the hook is called and GK_NONE
   is returned (callers redraw and wait again). Without a tic timer, each
   call times out after GINP_IDLE_MS without a UI key instead. */
int ginp_getkey(void);
#define GINP_IDLE_MS  (10 * 60 * 1000)
extern void (*ginp_idle_hook)(void);
/* Like ginp_getkey() but returns GK_NONE after timeout_ms */
int ginp_getkey_timeout(int timeout_ms);

/* Display name of a physical key (fx-CG100 legends) */
char const *gkm_key_name(int code);

typedef struct {
	char const *name;
	int16_t const *map;    /* CGDOOM_KEYMAP_SIZE entries */
} gkm_preset_t;

/* Keymap presets (index 0 = default) */
gkm_preset_t const *gkm_presets(int *count);
/* Matrix code that opens the Doom menu (KEY_ESCAPE), and whether the
   engine let us choose it (weak hook, see keymaps.c). */
int gkm_menu_key(void);
bool gkm_menu_key_configurable(void);
void gkm_apply_engine_hooks(void);

/* Check a key captured by the Controls screen: OK to bind, cancel (BACK /
   HOME: keep the old binding), or reserved (*why is set). */
enum { GKM_BIND_OK = 0, GKM_BIND_CANCEL, GKM_BIND_RESERVED };
int gkm_rebind_check(int code, char const **why);
/* Index of the keymap entry (CGDOOM_KEYMAP_SIZE entries) other than
   `except` that is bound to `code`, or -1 (a rebind clears the older
   binding of the key and says so) */
int gkm_find_binding(int16_t const *map, int code, int except);
/* Second Enter key of the Doom menu (CGD_EnterKeycode2): OK when BACK is
   the menu key, else -1 */
int gkm_enter_key2(void);

//---
// time.c
//---

void gtime_init(void);
void gtime_quit(void);
void gtime_resync(void);
/* Quit chord: the tic interrupt latches a request;
   gtime_take_quit() returns it once and disarms the interrupt-context
   abort() fallback, which only fires when the chord stays held this long
   without the request being taken (hang). */
#define GTIME_ABORT_HOLD_TICS  70          /* 2 s */
bool gtime_take_quit(void);
/* No abort() from the tic any more (PL_Fatal, exit path: bounded, and
   their file writes must not be cut) */
void gtime_disarm_abort(void);
/* Tics since the keyboard state last changed (0 without a tic timer) */
int gtime_idle_tics(void);
void gtime_idle_reset(void);
/* True if the 35 Hz tic timer runs (gtime_idle_tics() is meaningful) */
bool gtime_ticking(void);
/* TMU of the 35 Hz tic (-1: none, polled clock) */
int gtime_tic_timer(void);

//---
// main.c
//---

/* Foreground safe point (PL_GetTics, every input.c wait): takes a quit
   chord request (exit path, keeping the session's saves) and, in a game,
   runs the idle exit. Does nothing on the exit and fatal paths. */
void gquit_poll(void);
/* Safe point of long engine phases that never call PL_GetTics (startup
   R_Init, level setup and precache, game loads): wad.c calls it
   on entry of PL_WadRead()/PL_WadMap(). In a game, takes a pending quit
   chord request like gquit_poll(); never draws (no idle exit). */
void gquit_poll_load(void);
/* In-game idle exit: demo playback / title loop (nothing to lose) after
   GINP_IDLE_MS, the player's own game after this long; 60 s warning
   either way */
#define GINP_GAME_IDLE_MS  (30 * 60 * 1000)
/* In-game idle policy (time.c, pure; main.c's game_idle_check). paused is
   the engine's CGD_IsPaused() mask (bits below, equal to cgdoom.h's
   CGD_IDLE_*, checked in main.c), usergame the engine's flag (a game the
   player started: false during demos), title whether the title loop is
   showing a page (gamestate == GS_DEMOSCREEN). Returns the idle limit in
   ms: GINP_IDLE_MS when nothing can be lost (a demo, a title page, no
   level outside a user game), else GINP_GAME_IDLE_MS (a level being played,
   paused, behind the menu or save-name entry, with the automap, or the
   intermission/finale of the player's game). *lose_progress says whether
   quitting loses unsaved progress (the warning says so). */
#define GIDLE_PAUSED   0x01
#define GIDLE_MENU     0x02
#define GIDLE_NOLEVEL  0x04
#define GIDLE_DEMO     0x08
int gtime_game_idle_limit_ms(int paused, bool usergame, bool title,
	bool *lose_progress);
/* False if gint's profiler found no free TMU (prof_tcnt unusable: no
   microsecond clock, benchmarks impossible) */
bool gtime_prof_ok(void);

//---
// clock.c
//---

enum { GCLK_OFF = 0, GCLK_F4 = 1 };
void gclk_init(void);
void gclk_apply(int level);
char const *gclk_name(int level);
int gclk_current(void);

//---
// diag.c
//---

/* hints.c: one recovery line for common fatal errors (NULL: none) */
char const *gfatal_hint(char const *msg);

/* wads: the launcher's WAD list (the Storage page can probe one) */
void gdiag_screen(gfs_entry_t const *wads, int count);

//---
// stack.S
//---

/* Call fn() with r15 = stack_top (8-byte aligned top of a P1 buffer) */
void cgd_call_on_stack(void (*fn)(void), void *stack_top);

//---
// ui.c: launcher drawing
//---

#define UI_BG      0xffff
#define UI_FG      0x0000
#define UI_DIM     0x7bef
#define UI_TITLE   0x9800
#define UI_WARN    0xb800
#define UI_HEAD    0x18e3

#define UI_MAX_ITEMS 48

typedef struct {
	int y, count;             /* reset each frame */
	int focus, scroll;        /* persistent */
	int item_y[UI_MAX_ITEMS];
} ui_layout_t;

void ui_init(void);
int  ui_line_height(void);
void ui_layout_init(ui_layout_t *l);
void ui_frame_start(ui_layout_t *l, char const *title);
void ui_frame_end(ui_layout_t *l, char const *footer);
int  ui_item(ui_layout_t *l, char const *label, char const *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void ui_note(ui_layout_t *l, int color, char const *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void ui_space(ui_layout_t *l, int pixels);
bool ui_nav(ui_layout_t *l, int key);

void ui_begin(void);        /* gvid_release() + clear */
void ui_end(void);          /* dupdate() */
void ui_message(char const *title, char const *text, bool wait);
/* ui_message(wait = true) that also returns after timeout_ms */
void ui_message_timeout(char const *title, char const *text, int timeout_ms);
bool ui_confirm(char const *title, char const *text);
void ui_status(char const *title, char const *line1, char const *line2);
void ui_text(int x, int y, int color, int halign, char const *str);
void ui_printf(int x, int y, int color, int halign, char const *fmt, ...)
	__attribute__((format(printf, 5, 6)));
int  ui_wrap(int x, int y, int w, int color, char const *text);

#endif /* CGD_GINT_H */
