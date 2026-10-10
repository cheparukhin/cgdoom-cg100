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
//	Savegame I/O, archiving, persistence.
//
//-----------------------------------------------------------------------------


#ifndef __P_SAVEG__
#define __P_SAVEG__


#ifdef __GNUG__
#pragma interface
#endif


// Persistent storage/archiving.
// These are the load / save game routines.
void P_ArchivePlayers (void);
void P_UnArchivePlayers (void);
void P_ArchiveWorld (void);
void P_UnArchiveWorld (void);
void P_ArchiveThinkers (void);
void P_UnArchiveThinkers (void);
void P_ArchiveSpecials (void);
void P_UnArchiveSpecials (void);

extern byte*		save_p; 

// CGDoom: the end of the savegame buffer, set by the caller with save_p.
// The P_Archive* functions stop writing before it (save_overflow is set,
// the savegame must be dropped); the P_UnArchive* functions fail (I_Error)
// instead of reading past it or using an index out of range.
extern byte*		save_end;
extern boolean		save_overflow;

// Room for a record of head bytes, the padding of the next PADSAVEP when
// pad is set, and size bytes: archiving (false and save_overflow set if
// it does not fit) and unarchiving (I_Error if it is not in the savegame)
boolean P_SaveRoom (size_t head, boolean pad, size_t size);
void P_LoadCheck (size_t head, boolean pad, size_t size);


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
