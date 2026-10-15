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

//---
// CGDoom options
//---

/* The settings file is text-based and probably doesn't ever need to change
   even if settings are added or removed. This is just in case. */
#define CGDOOM_SETTINGS_FILE_VERSION 1
/* Strict 8.3 name (the file system ignores case) */
#define CGDOOM_SETTINGS_FILE "CGDOOM.CFG"

typedef struct {
    /* Keep saves/demos in memory until CGD_FlushDelayedWrites() (see above).
       If 0, files are written as soon as they are produced. */
    int DelaySaves;
    /* Custom controls. */
    CGDoom_Keymap Keymap;

    /* Renderer settings, applied by R_Init and kept up to date when
       they change in the game (menu detail item, view size keys), so that a
       platform saving the options after the game keeps them:
       - LowDetail: vanilla's low detail (detailshift 1): 160-column view,
         every column/texel drawn as two pixels. 0 = high (default).
       - ViewSize: screenblocks 3..11 (10 = 320x168 + status bar, CGDoom's
         default; 11 = 320x200 without status bar; smaller sizes are
         centered with a black border). Other values mean 10. */
    int LowDetail;
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
/* Play the WAD's demos in the title loop, as vanilla does (default 1). The
   host tests clear it unless -demos is given. */
extern int CGD_TitleDemos;
/* Skip this amount of frames after every rendered frame (default 1); used
   when CGD_AutoFrameskip is 0 (fixed frameskip, CGDoom's semantics) */
extern int CGD_Frameskip;
/* Automatic frameskip. 0 = fixed CGD_Frameskip. N > 0 =
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

/* Rendering features beyond vanilla, as bits of CGD_Features. The builds
   with CGD_EXTRAS (cgd_config.h; the HD build) have them all, on by default;
   the others have none. None changes game state, so demos stay in sync
   either way. Set before D_DoomMain (host -features); the names, indexed by
   bit, are for the host's option. */
#define CGD_FEAT_ASPECT   0x01  /* aspect-ratio correction: the 3D view
                                   scaled 6/5 vertically, as DOOM's 320x200
                                   looked on a 4:3 monitor (r_main.c) */
#define CGD_FEAT_COLORNUMS 0x02 /* status-bar health, armour and ammo
                                   coloured by level; true colour only
                                   (st_stuff.c) */
#define CGD_FEAT_CROSSHAIR 0x04 /* a small yellow cross at the centre of
                                   the view (r_main.c) */
#define CGD_FEAT_AUTOMAP  0x08  /* automap: the map key also gives an
                                   overlay that turns with the player;
                                   kills, items, secrets and time shown
                                   (am_map.c) */
#define CGD_FEAT_FULLHUD  0x10  /* the largest view size shows health,
                                   armour, ammo and keys over the view
                                   (st_stuff.c) */
#define CGD_FEATURE_COUNT 5
extern int CGD_Features;
extern const char *const CGD_FeatureNames[];
#if CGD_EXTRAS && CGD_TRUECOLOR
#define CGD_FEATURES_ALL ((1 << CGD_FEATURE_COUNT) - 1)
#elif CGD_EXTRAS
#define CGD_FEATURES_ALL (((1 << CGD_FEATURE_COUNT) - 1) & ~CGD_FEAT_COLORNUMS)
#else
#define CGD_FEATURES_ALL 0
#endif
#define CGD_FEATURES_DEFAULT CGD_FEATURES_ALL

/* True-colour build (CGD_TRUECOLOR): 1 builds the light tables from the
   COLORMAP lump, so that frames equal the 8-bit build's through its palette
   (a test mode, host -tcexact); 0 (default) scales the palette in RGB
   (r_data.c R_BuildLightTables). Read when the tables are built. */
extern int CGD_TrueColorExact;
/* 8-bit build, test knob (host -palredraw): the status bar is redrawn at
   every palette change, as in the true-colour build (i_video.c) */
extern int CGD_PaletteRedraw;

/* Timedemo (vanilla's -timedemo, for every demo of the WAD). Set
   CGD_TimeDemo before D_DoomMain: the WAD's DEMO1..DEMO4 lumps (those
   present) play back to back as fast as the renderer goes, every tic
   rendered, without pacing or wipes; then D_DoomMain returns. Each run
   counts its rendered frames and the PL_GetMicros time from the end of its
   level load to the end of the demo. CGD_TimeDemoCount is 0 when the WAD
   has no demo. */
#define CGD_TIMEDEMO_MAX 4
typedef struct {
    char name[8];
    int frames;
    uint32_t micros;
} CGD_TimeDemoRun;
extern int CGD_TimeDemo;
extern int CGD_TimeDemoCount;
extern CGD_TimeDemoRun CGD_TimeDemoRuns[CGD_TIMEDEMO_MAX];

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
void CGD_CycleGamma(void);

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

   /* Slow WAD mapping (PL_WadIsSlow): mappable lumps copied into the
      zone (count, bytes; also counted in LumpsLoaded), and mappable lumps
      referenced in place because the zone was too full (also counted in
      LumpsReferenced) */
   int SlowLumpsCopied;
   uint64_t SlowLumpsCopiedTotal;
   int SlowLumpsInPlace;

   /* Renderer limits: per-frame peaks of visplanes, drawsegs,
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
   /* Zone bytes held by these renderer arrays (vanilla's are static),
      current and peak */
   int RenderArrayBytes;
   int RenderArrayBytesPeak;

   /* For the host's -crc565 flags, the frames where the 8-bit and
      true-colour builds differ on purpose: sprites drawn with the spectre's
      fuzz (its true-colour darkening differs), gamma changes (the 8-bit
      palette also recolours stale pixels, such as the hall-of-mirrors
      outside a map in noclip), palette changes (I_SetPalette calls) and a
      wipe whose start screen was drawn with another palette (in 8-bit it
      takes the new one). */
   int FuzzSprites;
   int GammaChanges;
   int PaletteChanges;
   int WipeStalePalette;

} CGDoom_Stats;

extern CGDoom_Stats CGD_Stats;

/* Test knob (r_plane.c): if set before D_DoomMain, the growable renderer
   arrays (visplanes, drawsegs, vissprites, openings) start tiny so that every
   run exercises their growth; the output must not change (host -rlimits). */
extern int CGD_SmallRenderLimits;

#endif /* CGDOOM_H */
