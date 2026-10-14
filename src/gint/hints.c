/* CGDoom gint platform: recovery hints for the fatal-error screen.

   PL_Fatal() shows the engine's I_Error() text, then exits to MPM. For the
   common errors, which give the player no next step, the screen adds one
   line. Pure logic (host-tested, tests/gint-host). */

#include "cgd_gint.h"

#include <string.h>

char const *gfatal_hint(char const *msg)
{
	if(!msg)
		return NULL;
	/* z_zone.c: "Z_Malloc(%d) failure" (the WAD or the level does not
	   fit in the zones) */
	if(strstr(msg, "Z_Malloc"))
		return "Out of memory for this WAD or level: play a smaller "
			"WAD.";
	/* (No demo hint: the title loop skips a demo it cannot play, and
	   demo files are not played.) */
	/* g_game.c, p_saveg.c: "Bad savegame..." (G_DoLoadGame) */
	if(strstr(msg, "Bad savegame"))
		return "This savegame is damaged, or was made with another WAD "
			"or version: delete its .DSG file (Memory app).";
	/* w_wad.c: "W_GetNumForName: %s not found!" */
	if(strstr(msg, "W_GetNumForName"))
		return "A lump is missing: the WAD may be incomplete (copy it "
			"again) or not one CGDoom can play.";
	return NULL;
}
