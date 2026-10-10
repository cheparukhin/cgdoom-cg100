/* Host unit test for src/gint/wadnames.c (gint review round 1), linked with
   the engine's cgdoom-names.c:
   - the WAD base name given to the engine is whole (it used to be cut to 6
     characters, so freedoom1/freedoom2 and plutonia/plutonia2 shared saves);
   - demos map to the WAD they were recorded with (DOOM1D03.LMP used to go
     to doom.wad when both were present; hashed prefixes never matched). */

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

static char base[NW][GFS_LIST_NAME];

static void test_base(void)
{
	char b[GFS_LIST_NAME], small[4];
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

static void test_demos(void)
{
	/* Every WAD's own recorded demos map back to it */
	for(int i = 0; i < NW; i++) {
		CGD_WADFileName = base[i];
		for(int slot = 0; slot < 100; slot += 33) {
			char d[CGD_NAME_MAX];
			CGD_DemoFileName(d, slot);
			int w = gnames_demo_wad(wads, NW, d);
			CHECK(w == i, "%s -> %d (%s), want %s", d, w, w >= 0 ? wads[w].name : "-",
				wads[i].name);
			/* Case-insensitive, as listed by the file system */
			for(char *c = d; *c; c++) if(*c >= 'A' && *c <= 'Z') *c += 32;
			CHECK(gnames_demo_wad(wads, NW, d) == i, "lower-case %s", d);
		}
	}
	CHECK(gnames_demo_wad(wads, NW, "DOOM1D03.LMP") == 1, "DOOM1D03 -> doom1.wad, not doom.wad");
	CHECK(gnames_demo_wad(wads, NW, "DOOMD03.LMP") == 0, "DOOMD03 -> doom.wad");

	/* Other names: longest base name followed by a separator */
	CHECK(gnames_demo_wad(wads, NW, "doom1_demo00.lmp") == 1, "legacy doom1_");
	CHECK(gnames_demo_wad(wads, NW, "doom_run.lmp") == 0, "legacy doom_");
	CHECK(gnames_demo_wad(wads, NW, "plutonia2-speed.lmp") == 5, "legacy plutonia2-");
	CHECK(gnames_demo_wad(wads, NW, "doomed.lmp") == -1, "no separator: unknown");
	CHECK(gnames_demo_wad(wads, NW, "MYDEMO.LMP") == -1, "unknown");
	CHECK(gnames_demo_wad(wads, NW, "DOOM1D3.LMP") == -1, "one digit: not an engine name");
	CHECK(gnames_demo_wad(wads, 0, "DOOM1D03.LMP") == -1, "no WADs");
	CHECK(gnames_demo_wad(wads, NW, NULL) == -1, "NULL");
}

int main(void)
{
	test_base();
	test_demos();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
