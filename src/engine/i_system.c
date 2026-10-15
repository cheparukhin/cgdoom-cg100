// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------



#include "os.h"
#include "cgdoom.h"
#include "cgdoom-kbd.h"

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"

#include "g_game.h"
#include "d_main.h"
#include "doomstat.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"

int 			contrast_change= 0;
int 			map_mode= 0;
unsigned short 	timer_def_one;
byte			timer_def_two;


ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}

/* CGDoom: Screens are ordinary cached heap buffers, 32-byte aligned (cache
   lines). screens[2] and [3] are the wipe buffers, screens[4] the status
   bar's (32 rows). Buffers are allocated on first request, zeroed, and never
   freed. */
pixel_t *I_ScreenBase(int screen)
{
    static pixel_t *buffers[5];
    static const int sizes[5] = {
        SCREENWIDTH*SCREENHEIGHT*PIXELBYTES, SCREENWIDTH*SCREENHEIGHT*PIXELBYTES,
        SCREENWIDTH*SCREENHEIGHT*PIXELBYTES, SCREENWIDTH*SCREENHEIGHT*PIXELBYTES,
        SCREENWIDTH*32*PIXELBYTES,
    };

    if (screen < 0 || screen >= 5)
        return NULL;
    if (!buffers[screen])
    {
        byte *raw = CGD_calloc(sizes[screen] + 31);
        buffers[screen] = (pixel_t *)(((uintptr_t)raw + 31) & ~(uintptr_t)31);
    }
    return buffers[screen];
}

//
// I_Init
// Initialize machine state
//
void I_Init (void)
{
}


//
// I_Restore
// Restore machine state (if not done, the calculator will be killed once it enters sleep mode after the game quit)
//
void I_Restore (void)
{
}


//
// I_GetTime
// returns time in 1/35th second tics
int  I_GetTime (void)
{
    return PL_GetTics();
}

//
// I_Quit
//
void I_Quit (void)
{
    //M_SaveDefaults ();
    fuck= true;
}


//
// toupper_int
//
int toupper_int(int i) 
{
    if(('a' <= i) && (i <= 'z'))
        return 'A' + (i - 'a');
    else
        return i;
}

//
// I_Error
// CGDoom: fatal; the platform shows the message and exits (never returns).
//
extern boolean demorecording;

void I_Error (const char *error, ...)
{
    static char msg[256];
    static boolean in_error = false;
    va_list args;

    va_start(args, error);
    vsnprintf(msg, sizeof msg, error, args);
    va_end(args);

    /* Keep a demo being recorded (G_CheckDemoStatus may itself fail) */
    if (demorecording && !in_error)
    {
        in_error = true;
        G_CheckDemoStatus();
    }

    PL_Fatal(msg);
}

//
// I_StartTic
// Processes events
// Is a bit strange because a PC handles events synchronously, while the Nspire asynchronously
//
/* CGDoom: view size keys (SKEY_DECVP / SKEY_INCVP): one screenblocks step,
   3..11 like vanilla's screen size menu (CGDoom stopped at 1..10); a step
   past either end does nothing. */
extern int screenblocks;

static void R_SetViewSizeChange(int iDiff)
{
  int blocks = screenblocks + iDiff;
  if(blocks >= 3 && blocks <= 11)
    CGD_SetViewSize(blocks);
}

//
// Frame pacing (CGDoom: automatic frameskip, cgdoom.h)
//
// Fixed frameskip (CGD_AutoFrameskip == 0, CGDoom's semantics): a frame is
// rendered every CGD_Frameskip+1 tics, and the tic that starts such a group
// waits until CGD_Frameskip+1 tics have passed since the previous group.
//
// Auto (CGD_AutoFrameskip = N > 0): tic g is due at real time
// auto_base + g. I_StartTic waits while the tic is early; after the tic,
// I_DisplayDue skips the frame when the next tic is already due (the game is
// behind real time), at most N times in a row. The lag is capped to N tics
// (auto_base moves forward): after a long stall (level load, wipe) or when
// even N skips are not enough, the game slows down instead of fast-
// forwarding to catch up.
//
static boolean	auto_started = false;
static int	auto_base;		/* real tic at which gametic 0 was due */
static int	auto_skipped;		/* frames skipped in a row */

static void I_AutoPace (void)
{
    int now = PL_GetTics();
    int lag;

    if(!auto_started)
    {
        auto_base = now - gametic;
        auto_skipped = 0;
        auto_started = true;
    }

    lag = now - (auto_base + gametic);
    if(lag < 0)
        PL_WaitTics(auto_base + gametic);
    else if(lag > CGD_AutoFrameskip)
        auto_base += lag - CGD_AutoFrameskip;
}

boolean I_DisplayDue (void)
{
    if(timingdemo)
        return true;
    if(CGD_AutoFrameskip <= 0)
        return !(gametic % (CGD_Frameskip + 1));

    if(auto_skipped < CGD_AutoFrameskip
        && PL_GetTics() >= auto_base + gametic)
    {
        auto_skipped++;
        return false;
    }
    auto_skipped = 0;
    return true;
}

//
// I_FrameDisplayed
// FPS counter: called after every rendered frame (D_DoomLoop). CGDoom
// counted tics and divided by the fixed frameskip; counting the frames
// themselves also works for the automatic frameskip.
//
void I_FrameDisplayed (void)
{
    extern int fpscounter_data;

    /* Number of frames since FPS count started */
    static int fps_frames = 0;
    /* Time when the FPS count started */
    static uint32_t fps_start = 0;
    static boolean fps_started = false;

    if(!fps_started)
    {
        fps_start = PL_GetMicros();
        fps_started = true;
        return;
    }

    if(++fps_frames >= 16)
    {
        uint32_t now = PL_GetMicros();
        uint32_t elapsed = now - fps_start;
        if (elapsed == 0)
            fpscounter_data = -1;
        else
            fpscounter_data = (int)((uint64_t)1000000 * fps_frames /
                (uint64_t)elapsed);

        fps_start = now;
        fps_frames = 0;
    }
}

void I_StartTic (void)
{
    //---
    // Rate limiter
    //---

    static boolean ratelimit_started = false;
    static int ratelimit_last = 0;

    /* CGDoom: a timedemo runs uncapped */
    if(timingdemo)
    {
        auto_started = false;
        ratelimit_started = false;
    }
    else if(CGD_AutoFrameskip > 0)
    {
        I_AutoPace();
    }
    /* Rate limit the game at 35 tics per second, but only extend tics where we
       display frames, because that's when all the work is done */
    else
    {
        auto_started = false;
        if(!(gametic % (CGD_Frameskip + 1))) {
            if(ratelimit_started)
                PL_WaitTics(ratelimit_last + CGD_Frameskip + 1);
            ratelimit_last = PL_GetTics();
            ratelimit_started = true;
        }
    }

    extern boolean fpscounteractive;

    //---
    // Special keys
    //---

    /* This variable is set in m_menu.c while typing save file names */
    extern int saveStringEnter;

    CGD_UpdateKeyboardState();

    if(!saveStringEnter)
    {
        /* Capture events for special keys */
        if(CGD_KeyWasJustPressed(SKEY_CHEAT))
            CGD_Cheat();
        if(CGD_KeyWasJustPressed(SKEY_DECVP))
            R_SetViewSizeChange(-1);
        if(CGD_KeyWasJustPressed(SKEY_INCVP))
            R_SetViewSizeChange(1);
        if(CGD_KeyWasJustPressed(SKEY_NOCLIP))
            CGD_SwitchClip();
        if(CGD_KeyWasJustPressed(SKEY_GAMMA))
            CGD_CycleGamma();
        if(CGD_KeyWasJustPressed(SKEY_FPSCOUNTER))
            fpscounteractive = !fpscounteractive;
    }

    //---
    // Normal keys
    //---

    /* Sets of keys depending on input mode. Each set must only consist of keys
       that don't overlap */
    static const int base_keys[] = {
        KEY_LEFTARROW, KEY_RIGHTARROW, KEY_UPARROW, KEY_DOWNARROW,
        KEY_RCTRL, KEY_TAB, KEY_PAUSE, KEY_SLEFTARROW, KEY_SRIGHTARROW,
        KEY_RSHIFT, KEY_ESCAPE, KEY_ENTER,
        ' ', '1', '2', '3', '4', '5', '6', '7',
        0
    };
    static const int input_keys[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
        'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        -'0', -'1', -'2', -'3', -'4', -'5', -'6', -'7', -'8', -'9', -' ',
        KEY_BACKSPACE, KEY_ESCAPE, KEY_ENTER,
        0
    };

    const int *keys = base_keys;
    if(saveStringEnter)
        keys = input_keys;

    /* Emit events for all changes. The keys are passed as they are: digits
       and space are negative in input_keys (-'0'..-'9', -' ': typed with or
       without ALPHA, see cgdoom-kbd.c); a (uint8_t) cast, as CGDoom had
       here, would turn them into unknown keys. Every base key
       is positive and below 256, so its events are unchanged. The event
       carries the Doom key itself ('0'..'9', ' '). */
    const int entering = saveStringEnter;
    for(int i = 0; keys[i]; i++)
    {
        event_t e;
        if(CGD_KeyWasJustPressed(keys[i]))
            e.type = ev_keydown;
        else if(CGD_KeyWasJustReleased(keys[i]))
            e.type = ev_keyup;
        else continue;

        /* Events are handled as they are posted, so a key press that
           switches save-name entry on or off (EXE on a save slot; EXE or
           MENU while typing) applies to the presses after it in this scan,
           which belong to the other set. With Interact on EXE, picking an
           empty slot would type a space after the pre-filled "E1M1" (a
           Weapon key on EXE would type a digit).
           Drop those presses; releases still reach G_Responder. */
        if(e.type == ev_keydown && saveStringEnter != entering)
            continue;

        e.data1 = (keys[i] < 0) ? -keys[i] : keys[i];
        D_PostEvent(&e);
    }

    /* The gameplay keys released during save-name entry still reach
       G_Responder as key-ups (as vanilla's events do; the menu ignores
       key-ups). Otherwise an action held when the entry began (fire, a
       move) would stay down in gamekeydown afterwards. ESCAPE and
       ENTER are in both sets and were handled above. */
    if(entering)
    {
        for(int i = 0; base_keys[i]; i++)
        {
            if(base_keys[i] == KEY_ESCAPE || base_keys[i] == KEY_ENTER
                || !CGD_KeyWasJustReleased(base_keys[i]))
                continue;
            event_t e = { .type = ev_keyup, .data1 = base_keys[i] };
            D_PostEvent(&e);
        }
    }
}
