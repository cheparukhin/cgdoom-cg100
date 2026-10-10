/* 8.3 file-name mapping test (make test-names).

   Checks src/engine/cgdoom-names.c: fixed expectations for common WAD
   names, 8.3 validity of every generated name, determinism, and that the
   usual WAD names give pairwise distinct prefixes. Exit status 0 = PASS. */

#include <stdio.h>
#include <string.h>
#include "cgdoom.h"

#undef printf   /* os.h compiles the engine's printf() out */

const char *CGD_WADFileName;

static int fails;

static void expect_prefix(const char *wad, const char *want)
{
    char p[CGD_PREFIX_MAX + 1];
    CGD_FilePrefix(p, wad);
    if(strcmp(p, want)) {
        printf("FAIL prefix(%s) = %s, expected %s\n", wad ? wad : "(null)", p,
            want);
        fails++;
    }
}

static void expect_valid(const char *name, int want)
{
    if(CGD_ValidName83(name) != want) {
        printf("FAIL valid83(\"%s\") = %d, expected %d\n", name, !want, want);
        fails++;
    }
}

int main(void)
{
    /* the mapping (cgdoom-names.c rule 2 and 3) */
    expect_prefix("doom1", "DOOM1");
    expect_prefix("DOOM1", "DOOM1");
    expect_prefix("doom1.wad", "DOOM1");
    expect_prefix("/path/to/Doom2.WAD", "DOOM2");
    expect_prefix("doom", "DOOM");
    expect_prefix("tnt", "TNT");
    expect_prefix("doomu1", "DOO9I");
    expect_prefix("freedoom1", "FRE0P");
    expect_prefix("freedoom2", "FREXS");
    expect_prefix("freedm", "FREZ4");
    expect_prefix("plutonia", "PLUFN");
    expect_prefix("my wad", "MY_PR");
    expect_prefix("my_wad", "MY_P0");  /* 6 characters: hashed */
    expect_prefix("", "DOOM");
    expect_prefix(NULL, "DOOM");
    expect_prefix(".wad", "DOOM");

    /* validity checker */
    expect_valid("CGDOOM.CFG", 1);
    expect_valid("cgdoom.cfg", 1);
    expect_valid("DOOM1_0.DSG", 1);
    expect_valid("DOOM1D07.LMP", 1);
    expect_valid("ABCDEFGH", 1);
    expect_valid("A B$(1).X", 1);
    expect_valid("ABCDEFGHI.DSG", 0);
    expect_valid("DOOM1.DSGX", 0);
    expect_valid("DOOM1.", 0);
    expect_valid(".DSG", 0);
    expect_valid("A.B.C", 0);
    expect_valid("A~1.DSG", 0);
    expect_valid("A*.DSG", 0);
    expect_valid("doom1_demo00.lmp", 0);   /* the old CGDoom names */
    expect_valid("freedoom1_0.dsg", 0);
    expect_valid("", 0);

    /* settings, saves and demos for many WAD names */
    static const char *wads[] = {
        "doom1", "doom", "doom2", "doomu", "doomu1", "doomu2", "doomu3",
        "doomu4", "tnt", "plutonia", "freedoom1", "freedoom2", "freedm",
        "heretic", "hexen", "chex", "sigil", "nerve", "a", "verylongwadname",
        "x.y.z", "with space", "Mixed_Case-9", "123456789",
    };
    const int nw = sizeof wads / sizeof wads[0];
    char prefixes[sizeof wads / sizeof wads[0]][CGD_PREFIX_MAX + 1];

    expect_valid(CGDOOM_SETTINGS_FILE, 1);
    for(int i = 0; i < nw; i++) {
        char name[CGD_NAME_MAX], again[CGD_NAME_MAX];
        CGD_WADFileName = wads[i];
        CGD_FilePrefix(prefixes[i], wads[i]);
        for(int s = 0; s < 10; s++) {
            CGD_SaveFileName(name, s);
            CGD_SaveFileName(again, s);
            if(!CGD_ValidName83(name) || strcmp(name, again) ||
               strlen(name) != strlen(prefixes[i]) + 6) {
                printf("FAIL save name %s for %s\n", name, wads[i]);
                fails++;
            }
        }
        for(int s = 0; s < 100; s++) {
            CGD_DemoFileName(name, s);
            if(!CGD_ValidName83(name) ||
               strlen(name) != strlen(prefixes[i]) + 7) {
                printf("FAIL demo name %s for %s\n", name, wads[i]);
                fails++;
            }
        }
        CGD_SaveFileName(name, 0);
        printf("  %-16s -> %-5s  %-12s", wads[i], prefixes[i], name);
        CGD_DemoFileName(name, 7);
        printf(" %s\n", name);
    }
    for(int i = 0; i < nw; i++)
        for(int j = i + 1; j < nw; j++)
            if(!strcmp(prefixes[i], prefixes[j])) {
                printf("FAIL %s and %s share the prefix %s\n", wads[i],
                    wads[j], prefixes[i]);
                fails++;
            }

    printf("test-names: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
