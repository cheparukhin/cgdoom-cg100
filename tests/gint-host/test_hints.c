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
	char const *zm = gfatal_hint("Z_Malloc(42776) failure", true);
	CHECK(zm && strstr(zm, "Copy WAD to RAM"), "CG100 memory hint: %s", zm ? zm : "-");
	zm = gfatal_hint("Z_Malloc(42776) failure", false);
	CHECK(zm && strstr(zm, "experimental RAM"), "CG50 memory hint: %s", zm ? zm : "-");
	char const *d = gfatal_hint("Demo is from a different game version (110)", true);
	CHECK(d && strstr(d, ".LMP"), "demo version hint");
	CHECK(gfatal_hint("M_ReadFile: cannot open FRE0D03.LMP (-1)", true) == d, "demo read hint");
	CHECK(gfatal_hint("M_ReadFile: cannot open FOO.CFG (-1)", true) == NULL, "non-demo read: no hint");
	char const *w = gfatal_hint("W_GetNumForName: map40 not found!", true);
	CHECK(w && strstr(w, "WAD"), "missing lump hint");
	CHECK(gfatal_hint("Savegame buffer overrun", true) == NULL, "other errors: no hint");
	CHECK(gfatal_hint(NULL, true) == NULL, "no message");
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
