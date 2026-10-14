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
// DESCRIPTION:
//	System specific interface stuff.
//
//-----------------------------------------------------------------------------


#ifndef __I_SYSTEM__
#define __I_SYSTEM__

#include "os.h"
#include "d_ticcmd.h"
#include "d_event.h"

#ifdef __GNUG__
#pragma interface
#endif

/* CGDoom: zones come from PL_GetZones() (see D_DoomMain). */

/* Extra CGDoom function supplying screen buffers (heap, 32-byte aligned) */
byte *I_ScreenBase(int screen);

// Called by D_DoomLoop,
// returns current time in tics (35 Hz, PL_GetTics).
int I_GetTime (void);

// Initializes the timer used by the function above
void I_Init (void);

// Resets the timer to avoid calckill
void I_Restore (void);

//
// Called by D_DoomLoop,
// called before processing any tics in a frame
// (just after displaying a frame).
// Time consuming syncronous operations
// are performed here (joystick reading).
// Can call D_PostEvent.
//
//void I_StartFrame (void);

//
// Called by D_DoomLoop,
// called before processing each tic in a frame.
// Quick syncronous operations are performed here.
// Can call D_PostEvent.
void I_StartTic (void);

// CGDoom: called by D_DoomLoop after each tic (gametic already
// incremented): whether to render a frame now (fixed or automatic
// frameskip, cgdoom.h CGD_AutoFrameskip), and after each rendered frame
// (FPS counter).
boolean I_DisplayDue (void);
void I_FrameDisplayed (void);

// Asynchronous interrupt functions should maintain private queues
// that are read by the synchronous functions
// to be converted into events.

// Either returns a null ticcmd,
// or calls a loadable driver to build it.
// This ticcmd will then be modified by the gameloop
// for normal input.
ticcmd_t* I_BaseTiccmd (void);


// Called by M_Responder when quit is selected.
// Clean exit, displays sell blurb.
void I_Quit (void);

// Allocates from low memory under dos,
// just mallocs under unix
byte* I_AllocLow (int length);

//Helper functions
void 	I_ShowDBox(const char* title, const char* msg);
int  	toupper_int(int i);
//boolean	access_ok(const char *file);
//void	I_GetMainDir(char* dir);
/* I_Error is declared in os.h (fatal, never returns) */

//Set by I_Error()
extern boolean fuck;

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
