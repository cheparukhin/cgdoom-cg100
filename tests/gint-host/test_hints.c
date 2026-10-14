/* Host unit test for src/gint/hints.c (final review): the fatal-error
   screen adds a recovery line for the common engine errors. */
#include "cgd_gint.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

int main(void)
{
	/* The engine's exact texts (z_zone.c, g_game.c, m_misc.c, w_wad.c) */
	char const *zm = gfatal_hint("Z_Malloc(42776) failure");
	CHECK(zm && strstr(zm, "smaller WAD"), "memory hint: %s", zm ? zm : "-");
	/* No demo hint since the demo replay was removed */
	CHECK(gfatal_hint("Demo is from a different game version (110)") == NULL, "no demo version hint");
	CHECK(gfatal_hint("M_ReadFile: cannot open FRE0D03.LMP (-1)") == NULL, "no demo read hint");
	char const *s = gfatal_hint("Bad savegame: state 5000");
	CHECK(s && strstr(s, ".DSG"), "savegame hint");
	CHECK(gfatal_hint("Bad savegame") == s, "savegame marker hint");
	CHECK(gfatal_hint("M_ReadFile: cannot open FOO.CFG (-1)") == NULL, "non-demo read: no hint");
	char const *w = gfatal_hint("W_GetNumForName: map40 not found!");
	CHECK(w && strstr(w, "WAD"), "missing lump hint");
	CHECK(gfatal_hint("Savegame buffer overrun") == NULL, "other errors: no hint");
	CHECK(gfatal_hint(NULL) == NULL, "no message");
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
