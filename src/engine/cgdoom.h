#ifndef CGDOOM_H
#define CGDOOM_H

/* CGDoom glue between the portable engine and the platform.

   Startup sequence expected from a platform main():

     CGD_DefaultOptions(&CGD_Options);     // defaults (keymap, flags)
     CGD_LoadSettings(&CGD_Options);       // optional, reads CGDOOM.CFG
     ... platform UI: pick WAD, options, keymap ...
     PL_WadOpen(path);                     // the engine reads lumps via PL_Wad*
     CGD_WADFileName = "doom1";            // WAD base name: saves DOOM1_<n>.DSG,
                                           // demos DOOM1D<nn>.LMP (8.3 names)
     startepisode/startmap/skill/autostart (d_main.h), CGD_Frameskip, ...
     D_DoomMain();                         // returns when the game quits
     PL_WadClose();                        // first: see below
     CGD_FlushDelayedWrites();             // write pending saves/demos

   The WAD is closed BEFORE the delayed writes: on the calculator a storage
   write moves flash records and invalidates a block-mapped WAD, and while
   such a mapping is live the platform may have to refuse a write it cannot
   copy for later (no memory), losing the file (gint files.c).

   Fatal errors go through I_Error() -> PL_Fatal() and never return. */

#include "cgdoom-kbd.h"
#include "os.h"
#include "cgd-prof.h"
#include <stdint.h>

//---
// Delayed file writes
//---

/* Description of a file write that has been delayed. */
typedef struct
{
   char filename[32];
   const void *data;
   int size;

} CGDoom_DelayedFileWrite;

/* IDs for delayed saves */
enum {
   CGD_DELAYEDSAVES_SAVES = 0, /* 6 entries */
   CGD_DELAYEDSAVES_DEMO = 6,
   CGD_DELAYEDSAVES_COUNT,
};
/* Save Game operations delayed until closing the add-in (one per save).
   Writing files on the calculator reshuffles the storage memory, which would
   invalidate the WAD mapping; so by default saves and recorded demos are kept
   in the zone heap (PU_STATIC) and written by CGD_FlushDelayedWrites() when
   the platform is done with the WAD. */
extern CGDoom_DelayedFileWrite CGD_DelayedSaves[CGD_DELAYEDSAVES_COUNT];

/* Write delayed file #i with PL_FileWrite() and release it. Returns the
   PL_FileWrite() result (<0 on error), 0 if there was nothing to write. */
int CGD_WriteDelayedFile(int i);
/* Write all pending delayed files. Returns the number of failed writes. */
int CGD_FlushDelayedWrites(void);

/* WAD access method (platform-interpreted; kept for the settings file) */
enum {
    /* Read lumps through the file API every time */
    CGDOOM_WAD_BFILE = 0,
    /* Map the file into memory and reference lumps in place when possible */
    CGDOOM_WAD_MMAP = 1,
};

//---
// CGDoom options
//---

/* The settings file is text-based and probably doesn't ever need to change
   even if settings are added or removed. This is just in case. */
#define CGDOOM_SETTINGS_FILE_VERSION 1
/* Strict 8.3 name (notes/design.md §7; the file system ignores case) */
#define CGDOOM_SETTINGS_FILE "CGDOOM.CFG"

typedef struct {
    /* Enable technical detail screens (platform). */
    int DeveloperInfo;
    /* File access method (see above; platform). */
    int WADMethod;
    /* If set, mapped lumps that are not 4-aligned are also referenced in
       place instead of being copied to the heap. Safe for the C engine (all
       WAD fields are read bytewise) but not for code doing aligned loads. */
    int TrustUnalignedLumps;
    /* Skip title screen and get straight to gameplay. */
    int AutoStart;
    /* Enable demos in the title screen (most are for previous versions of
       the engine and just look bad). */
    int EnableDemos;
    /* Keep saves/demos in memory until CGD_FlushDelayedWrites() (see above).
       If 0, files are written as soon as they are produced. */
    int DelaySaves;
    /* Custom controls. */
    CGDoom_Keymap Keymap;

    /* Renderer settings (step 3), applied by R_Init and kept up to date when
       they change in the game (menu detail item, view size keys), so that a
       platform saving the options after the game keeps them:
       - LowDetail: vanilla's low detail (detailshift 1): 160-column view,
         every column/texel drawn as two pixels. 0 = high (default).
       - FlatFloors: "potato" floors and ceilings, each span one solid colour
         (the flat's average colour, still light-mapped). 0 = off (default).
       - ViewSize: screenblocks 3..11 (10 = 320x168 + status bar, CGDoom's
         default; 11 = 320x200 without status bar; smaller sizes are
         centered with a black border). Other values mean 10. */
    int LowDetail;
    int FlatFloors;
    int ViewSize;

} CGDoom_Options;

extern CGDoom_Options CGD_Options;

/* Default options (ThumbsOnly keymap, delayed saves). */
void CGD_DefaultOptions(CGDoom_Options *o);
/* Load/save options from/to CGDOOM_SETTINGS_FILE. Load returns <0 if there
   is no valid settings file (o is left untouched then); save returns the
   PL_FileWrite() result. */
int CGD_LoadSettings(CGDoom_Options *o);
int CGD_SaveSettings(const CGDoom_Options *o);

// CGDoom settings that affect the game

/* The selected WAD is a split Ultimate Doom WAD with a single episode (this
   exists on the fx-CG 50 and is incorrectly detected by normal logic) */
extern int CGD_SingleEpisodeUltimate;
/* Skip this amount of frames after every rendered frame (default 1); used
   when CGD_AutoFrameskip is 0 (fixed frameskip, CGDoom's semantics) */
extern int CGD_Frameskip;
/* Automatic frameskip (step 3, design §8). 0 = fixed CGD_Frameskip. N > 0 =
   auto: game tics stay at 35 Hz real time (PL_GetTics) and a rendered frame
   (D_Display) is skipped while the game is behind real time, with at most
   N consecutive skipped frames (at least one frame every N+1 tics). When
   rendering is too slow even then, the game slows down instead of piling up
   lag (catch-up is limited to N tics). CGD_AUTOFRAMESKIP_DEFAULT is the
   suggested N. Changes take effect at the next tic. */
extern int CGD_AutoFrameskip;
#define CGD_AUTOFRAMESKIP_DEFAULT 3

/* Change the renderer settings at run time: these update CGD_Options and
   take effect at the next rendered frame (r_main.c). */
void CGD_SetLowDetail(int on);
void CGD_SetFlatFloors(int on);
/* blocks is clamped to 3..11 */
void CGD_SetViewSize(int blocks);
/* WAD file name (without .wad), used to name save and demo files. Pass the
   whole base name (e.g. "freedoom1"): the engine derives the 8.3 file names
   from it (CGD_FilePrefix below). */
extern const char *CGD_WADFileName;
/* Slot of demo file to record */
extern int CGD_RecordDemoSlot;
/* Whether we're only playing a demo and leaving */
extern int CGD_PlayDemoOnly;

/* Paused/idle signal for the platform's idle timeout (g_game.c). Nonzero
   when the game is not simulating a level the player is playing, as a mask
   of the reasons below:
   - CGD_IDLE_PAUSED: the game is paused (Pause key);
   - CGD_IDLE_MENU: the menu, a menu message or a help screen is shown (in
     a single-player game P_Ticker freezes the world behind it, as in
     vanilla; this includes typing a save-game name);
   - CGD_IDLE_NOLEVEL: no level: title loop/demo screen, intermission or
     finale (waiting for a key, or counting);
   - CGD_IDLE_DEMO: a demo is playing (title loop or -playdemo).
   0 means a level is being played (also with the automap open). Cheap and
   side-effect free: it only reads engine globals, so it may be called from
   PL_GetTics or a timer callback between tics. A paused or idle game still
   has unsaved progress whenever a level was being played (usergame), so a
   platform quitting on idle should keep its long timeout for those cases,
   or save first. */
#define CGD_IDLE_PAUSED   0x01
#define CGD_IDLE_MENU     0x02
#define CGD_IDLE_NOLEVEL  0x04
#define CGD_IDLE_DEMO     0x08
int CGD_IsPaused(void);

//---
// 8.3 file names (cgdoom-names.c; the mapping is documented there)
//---

/* Longest file-name prefix derived from a WAD name, and the buffer size for
   a complete 8.3 name ("XXXXXXXX.XXX" + NUL) */
#define CGD_PREFIX_MAX 5
#define CGD_NAME_MAX   13

/* Whether name is a strict 8.3 name that the Casio file system accepts:
   1-8 characters, optionally '.' and 1-3 characters, each one of
   A-Z a-z 0-9 ! # $ % , ( ) + - ; = @ [ ] ^ _ ` and space. */
int CGD_ValidName83(const char *name);
/* The upper-case prefix (1 to CGD_PREFIX_MAX characters) for a WAD name */
void CGD_FilePrefix(char out[CGD_PREFIX_MAX + 1], const char *wadname);
/* "<prefix>_<slot>.DSG" (slot 0..9) for CGD_WADFileName */
void CGD_SaveFileName(char out[CGD_NAME_MAX], int slot);
/* "<prefix>D<NN>.LMP" (slot 0..99) for CGD_WADFileName */
void CGD_DemoFileName(char out[CGD_NAME_MAX], int slot);

//---
// Control functions (mapped to special keys)
//---

void CGD_Cheat(void);
void CGD_SwitchClip(void);
void CGD_FreeMem(void);
void CGD_CycleFrameskip(void);
void CGD_CycleGamma(void);
void CGD_ProfilerResults(void);

//---
// Performance metrics
//---

typedef struct
{
   prof_t DynamicAllocation;
   prof_t GraphicsRendering;
   prof_t DisplayInterface;
   prof_t LumpLoading;
   prof_t UnalignedLumpLoading;

} CGDoom_Perf;

extern CGDoom_Perf CGD_Perf;

//---
// Game and debugging statistics
//---

typedef struct
{
   /* Total memory allocated with Z_Malloc (bytes, cumulative) */
   uint32_t MemoryAllocated;

   /* Zone usage in bytes, block headers included: all allocated blocks, and
      non-purgeable blocks (tag < PU_PURGELEVEL) only; with peaks. The
      non-purgeable peak is the minimum zone size a run needs. */
   int ZoneUsed;
   int ZoneUsedPeak;
   int ZoneStatic;
   int ZoneStaticPeak;

   /* Number of lumps loaded, number of lumps not loaded (addressed directly in
      the WAD mapping), number of mapped but unaligned lumps loaded */
   int LumpsLoaded;
   int LumpsReferenced;
   int UnalignedLumpsLoaded;

   /* The total size for these three catefories (bytes) */
   uint64_t LumpsLoadedTotal;
   uint64_t LumpsReferencedTotal;
   uint64_t UnalignedLumpsLoadedTotal;

   /* Slow WAD mapping (step 3, PL_WadIsSlow): mappable lumps copied into the
      zone (count, bytes; also counted in LumpsLoaded), and mappable lumps
      referenced in place because the zone was too full (also counted in
      LumpsReferenced) */
   int SlowLumpsCopied;
   uint64_t SlowLumpsCopiedTotal;
   int SlowLumpsInPlace;

   /* Renderer limits (step 2): per-frame peaks of visplanes, drawsegs,
      vissprites and openings (shorts) in use, the number of frames that went
      over vanilla's static limits (128, 256, 128, 320*64; those frames
      would have dropped walls/sprites or crashed before), and the walls and
      sprites dropped at the raised limits. */
   int VisplanesPeak;
   int DrawsegsPeak;
   int VisspritesPeak;
   int OpeningsPeak;
   int FramesOverVanillaLimits;
   int DrawsegsDropped;
   int VisspritesDropped;
   /* Zone bytes held by these renderer arrays (they were static before
      step 2), current and peak */
   int RenderArrayBytes;
   int RenderArrayBytesPeak;

} CGDoom_Stats;

extern CGDoom_Stats CGD_Stats;

/* Test knob (r_plane.c): if set before D_DoomMain, the growable renderer
   arrays (visplanes, drawsegs, vissprites, openings) start tiny so that every
   run exercises their growth; the output must not change (host -rlimits). */
extern int CGD_SmallRenderLimits;

#endif /* CGDOOM_H */
