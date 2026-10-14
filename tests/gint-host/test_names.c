/* Host unit test for src/gint/wadnames.c, linked with the engine's
   cgdoom-names.c: the WAD base name given to the engine is whole (cut to 6
   characters, freedoom1/freedoom2 and plutonia/plutonia2 would share
   saves). */

#include "cgd_gint.h"
#include "cgdoom.h"
#include <stdio.h>
#include <string.h>

/* The engine headers redefine printf (on-calculator logging) */
#undef printf

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

const char *CGD_WADFileName = NULL;

static gfs_entry_t wads[] = {
	{ "doom.wad", 0 }, { "doom1.wad", 0 }, { "freedoom1.wad", 0 },
	{ "freedoom2.wad", 0 }, { "plutonia.wad", 0 }, { "plutonia2.wad", 0 },
	{ "doomu1.wad", 0 }, { "MyLongWadName.WAD", 0 },
};
#define NW (int)(sizeof wads / sizeof *wads)

static void test_base(void)
{
	char b[GFS_LIST_NAME], small[4];
	static char base[NW][GFS_LIST_NAME];
	gnames_wad_base(b, sizeof b, "freedoom1.wad");
	CHECK(!strcmp(b, "freedoom1"), "base %s", b);
	gnames_wad_base(b, sizeof b, "noext");
	CHECK(!strcmp(b, "noext"), "no extension");
	gnames_wad_base(b, sizeof b, "a.b.wad");
	CHECK(!strcmp(b, "a"), "first dot");
	gnames_wad_base(small, sizeof small, "freedoom1.wad");
	CHECK(!strcmp(small, "fre"), "bounded by the buffer");

	/* Distinct saves for every WAD (the old 6-character cut collided) */
	char saves[NW][CGD_NAME_MAX];
	for(int i = 0; i < NW; i++) {
		gnames_wad_base(base[i], sizeof base[i], wads[i].name);
		CGD_WADFileName = base[i];
		CGD_SaveFileName(saves[i], 0);
		CHECK(CGD_ValidName83(saves[i]), "8.3 save name %s", saves[i]);
	}
	for(int i = 0; i < NW; i++)
		for(int j = i + 1; j < NW; j++)
			CHECK(strcmp(saves[i], saves[j]), "%s and %s share %s",
				wads[i].name, wads[j].name, saves[i]);
	printf("saves: freedoom1 %s, freedoom2 %s, plutonia %s, plutonia2 %s\n",
		saves[2], saves[3], saves[4], saves[5]);

	/* What the old code computed: both freedoom WADs -> "freedo" */
	char p1[CGD_PREFIX_MAX + 1], p2[CGD_PREFIX_MAX + 1];
	CGD_FilePrefix(p1, "freedo");
	CGD_FilePrefix(p2, "freedoom2");
	CHECK(strcmp(p1, p2), "truncation would have changed the prefix");
}

int main(void)
{
	test_base();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
