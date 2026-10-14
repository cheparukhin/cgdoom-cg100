/* CGDOOM.CFG settings test (make test-settings).

   Links src/engine/cgdoom.c and cgdoom-kbd.c with an in-memory file system
   and checks CGD_SaveSettings/CGD_LoadSettings:
   - round trip of every option and of a keymap with unbound (-1) entries,
     which CGD_SaveSettings writes as "Keymap.<action>=NONE";
   - "Keymap.<action>=NONE" in a hand-written file unbinds the action;
   - keys and values match exactly: truncated or extended keys ("View=",
     "ViewSizeX=", "Keymap.Weapon=", "Keymap.Weapon1x=") and values ("F",
     "NONEX", "MMAPX") are ignored, as are unknown, garbled and binary lines,
     lines without '=' and an unterminated last line;
   - the options and keymap actions of earlier builds (DeveloperInfo,
     EnableDemos, Autostart, FlatFloors, the gint platform's CG.* lines,
     the heap statistics, frameskip and profiler keys) are ignored;
   - CRLF line ends;
   - a wrong Version, an empty or a missing file leave the options untouched.
   Exit status 0 = PASS. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cgdoom.h"
#include "cgdoom-kbd.h"

#undef printf   /* os.h compiles the engine's printf() out */

static int fails;
#define CHECK(c, ...) do { if(!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while(0)

//---
// Stubs: one in-memory file, keyboard, errors
//---

static char file_name[16];
static char file_data[16384];
static int file_size = -1;

int PL_FileSize(const char *name)
{
    return strcmp(name, file_name) ? -1 : file_size;
}

int PL_FileRead(const char *name, void *buf, int size)
{
    if(strcmp(name, file_name) || file_size < 0)
        return -1;
    int n = size < file_size ? size : file_size;
    memcpy(buf, file_data, n);
    return n;
}

int PL_FileWrite(const char *name, const void *buf, int size)
{
    if(size > (int)sizeof file_data)
        return -1;
    snprintf(file_name, sizeof file_name, "%s", name);
    memcpy(file_data, buf, size);
    file_size = size;
    return size;
}

static void set_file(const char *text)
{
    snprintf(file_name, sizeof file_name, "%s", CGDOOM_SETTINGS_FILE);
    file_size = (int)strlen(text);
    memcpy(file_data, text, file_size);
}

void PL_ScanKeyboard(uint8_t state[12]) { memset(state, 0, 12); }
void Z_Free(void *p) { (void)p; }

void I_Error(const char *error, ...)
{
    va_list args;
    va_start(args, error);
    vprintf(error, args);
    va_end(args);
    printf("\n");
    exit(2);
}

//---
// Tests
//---

static int keymap_index(int doomkey)
{
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++)
        if(CGD_KeymapEntries[i] == doomkey)
            return i;
    return -1;
}

static int same_options(const CGDoom_Options *a, const CGDoom_Options *b)
{
    return !memcmp(a, b, sizeof *a);
}

static void test_roundtrip(void)
{
    CGDoom_Options o, n;
    CGD_DefaultOptions(&o);
    o.DelaySaves = 0;
    o.LowDetail = 1;
    o.ViewSize = 7;
    CGD_CopyKeymap(o.Keymap, CGD_Keymap_FullHands);
    o.Keymap[0] = -1;
    o.Keymap[keymap_index('3')] = -1;
    o.Keymap[CGDOOM_KEYMAP_SIZE - 1] = -1;
    o.Keymap[1] = KEYCODE_ACON;

    CHECK(CGD_SaveSettings(&o) > 0, "save");
    file_data[file_size] = 0;
    CHECK(strstr(file_data, "=NONE\n") != NULL, "NONE written");

    CGD_DefaultOptions(&n);
    CHECK(CGD_LoadSettings(&n) == 0, "load");
    CHECK(same_options(&o, &n), "round trip differs");
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++)
        CHECK(o.Keymap[i] == n.Keymap[i], "keymap[%d]: %d != %d", i,
            o.Keymap[i], n.Keymap[i]);

    /* Same with CRLF line ends */
    char crlf[8192];
    int k = 0;
    for(int i = 0; i < file_size; i++) {
        if(file_data[i] == '\n')
            crlf[k++] = '\r';
        crlf[k++] = file_data[i];
    }
    crlf[k] = 0;
    set_file(crlf);
    CGD_DefaultOptions(&n);
    CHECK(CGD_LoadSettings(&n) == 0 && same_options(&o, &n), "CRLF round trip");
}

static void test_none_and_garbage(void)
{
    char text[4096];
    int n = 0;
    const char *a_left = CGD_DoomKey_TechnicalName(KEY_LEFTARROW);
    const char *a_w1 = CGD_DoomKey_TechnicalName('1');
    const char *a_w2 = CGD_DoomKey_TechnicalName('2');
    const char *a_map = CGD_DoomKey_TechnicalName(KEY_TAB);
    const char *a_run = CGD_DoomKey_TechnicalName(KEY_RSHIFT);

    n += sprintf(text + n, "# comment=1\n\n\r\n");
    n += sprintf(text + n, "Version=%d\n", CGDOOM_SETTINGS_FILE_VERSION);
    n += sprintf(text + n, "Keymap.%s=NONE\n", a_left);         /* unbind */
    n += sprintf(text + n, "Keymap.%s=NONE\r\n", a_w1);         /* CRLF */
    n += sprintf(text + n, "Keymap.%s=NONEX\n", a_w2);          /* bad value */
    n += sprintf(text + n, "Keymap.%s=F\n", a_map);             /* prefix of F1 */
    n += sprintf(text + n, "Keymap.Weapon=F2\n");               /* prefix of Weapon1 */
    n += sprintf(text + n, "Keymap.Weapon1x=F3\n");
    n += sprintf(text + n, "Keymap.=F4\n");
    n += sprintf(text + n, "Keymap.NoSuchAction=NONE\n");
    n += sprintf(text + n, "Keymap.%s=SQUARE\n", a_run);        /* valid */
    n += sprintf(text + n, "View=3\n");                         /* prefix */
    n += sprintf(text + n, "ViewSizeX=4\n");
    n += sprintf(text + n, "Low=1\nLowDetai=1\n");
    /* Options of older versions (WAD access, unaligned lumps) */
    n += sprintf(text + n, "WADMethod=BFILE\nTrustUnalignedLumps=1\n");
    n += sprintf(text + n, "=5\n=\nnoequals\n\x01\x02=\xff\xfe\n");
    /* Options and actions removed on 2026-10-14 */
    n += sprintf(text + n, "DeveloperInfo=1\nEnableDemos=1\nAutostart=1\n"
        "FlatFloors=1\n");
    n += sprintf(text + n, "CG.Overclock=2\nCG.Display=1\nCG.Frameskip=0\n"
        "CG.FPS=1\nCG.WadRam=0\nCG.Skill=4\n");
    n += sprintf(text + n, "Keymap.HeapStatistics=SIN\n"
        "Keymap.CycleFrameskip=COS\nKeymap.Profiler=MUL\n");
    n += sprintf(text + n, "ViewSize=8");                       /* no '\n' */
    set_file(text);

    CGDoom_Options d, o;
    CGD_DefaultOptions(&d);
    o = d;
    CHECK(CGD_LoadSettings(&o) == 0, "load");

    CGDoom_Options want = d;
    want.Keymap[keymap_index(KEY_LEFTARROW)] = -1;
    want.Keymap[keymap_index('1')] = -1;
    want.Keymap[keymap_index(KEY_RSHIFT)] = KEYCODE_SQUARE;
    want.ViewSize = 8;
    CHECK(same_options(&o, &want), "garbled file: options differ");
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++)
        CHECK(o.Keymap[i] == want.Keymap[i], "keymap[%d] (%s): %d, want %d",
            i, CGD_DoomKey_TechnicalName(CGD_KeymapEntries[i]), o.Keymap[i],
            want.Keymap[i]);
    CHECK(o.LowDetail == 0 && o.ViewSize == 8, "prefix keys");
    CHECK(CGD_DoomKey_FromTechnicalName("Profiler", -1) < 0
        && CGD_DoomKey_FromTechnicalName("HeapStatistics", -1) < 0
        && CGD_DoomKey_FromTechnicalName("CycleFrameskip", -1) < 0,
        "removed actions have no technical name");

    /* Exact technical-name lookups */
    CHECK(CGD_PhysicalKey_FromTechnicalName("F", 1) < 0, "F is not F1");
    CHECK(CGD_PhysicalKey_FromTechnicalName("F1", -1) == KEYCODE_F1, "F1");
    CHECK(CGD_PhysicalKey_FromTechnicalName("ACON", -1) == KEYCODE_ACON, "ACON");
    CHECK(CGD_PhysicalKey_FromTechnicalName("NONE", 4) < 0, "NONE -> -1");
    CHECK(CGD_DoomKey_FromTechnicalName("Weapon", -1) < 0, "Weapon is not Weapon1");
    CHECK(CGD_DoomKey_FromTechnicalName("Weapon7", -1) == '7', "Weapon7");
    CHECK(CGD_DoomKey_FromTechnicalName("", 0) < 0, "empty name");
}

static void test_rejected(void)
{
    CGDoom_Options d, o;
    CGD_DefaultOptions(&d);

    char text[256];
    sprintf(text, "ViewSize=5\nVersion=%d\nLowDetail=1\n",
        CGDOOM_SETTINGS_FILE_VERSION + 1);
    set_file(text);
    o = d;
    CHECK(CGD_LoadSettings(&o) < 0 && same_options(&o, &d), "wrong version");

    set_file("");
    o = d;
    CHECK(CGD_LoadSettings(&o) < 0 && same_options(&o, &d), "empty file");

    file_size = -1;
    o = d;
    CHECK(CGD_LoadSettings(&o) < 0 && same_options(&o, &d), "no file");

    /* A NUL byte ends the file */
    sprintf(text, "Version=%d\nViewSize=6\n", CGDOOM_SETTINGS_FILE_VERSION);
    int len = (int)strlen(text);
    set_file(text);
    memcpy(file_data + len, "\0ViewSize=4\n", 12);
    file_size = len + 12;
    o = d;
    CHECK(CGD_LoadSettings(&o) == 0 && o.ViewSize == 6, "NUL ends the file");
}

/* Follow-ups review: numeric values are parsed within their own line. An
   empty value used to be atoi()'d across the line end ("LowDetail=\n1\n"
   set LowDetail from the next line); empty and non-numeric values are now
   ignored, like unknown values of the other options. */
static void test_numeric_values(void)
{
    CGDoom_Options d, o;
    CGD_DefaultOptions(&d);
    char text[512];

    sprintf(text, "Version=%d\nLowDetail=\n1\nDelaySaves=\r\n0\r\n"
        "ViewSize= 5\nLowDetail=1x\nDelaySaves=-\nViewSize=99999999999\n",
        CGDOOM_SETTINGS_FILE_VERSION);
    set_file(text);
    o = d;
    CHECK(CGD_LoadSettings(&o) == 0, "load");
    CHECK(same_options(&o, &d), "empty/malformed numbers changed options "
        "(LowDetail=%d DelaySaves=%d ViewSize=%d)", o.LowDetail,
        o.DelaySaves, o.ViewSize);

    sprintf(text, "Version=%d\r\nViewSize=7\r\nLowDetail=1\nDelaySaves=-1",
        CGDOOM_SETTINGS_FILE_VERSION);
    set_file(text);
    o = d;
    CHECK(CGD_LoadSettings(&o) == 0 && o.ViewSize == 7 && o.LowDetail == 1
        && o.DelaySaves == -1, "valid numbers");

    /* An empty Version is not the current version */
    sprintf(text, "Version=\n%d\nViewSize=5\n", CGDOOM_SETTINGS_FILE_VERSION);
    set_file(text);
    o = d;
    CHECK(CGD_LoadSettings(&o) < 0 && same_options(&o, &d), "empty Version");

    /* A long line without '=' and a long value are ignored */
    static char big[16384];
    int n = sprintf(big, "Version=%d\n", CGDOOM_SETTINGS_FILE_VERSION);
    memset(big + n, 'x', 6000); n += 6000; big[n++] = '\n';
    n += sprintf(big + n, "Keymap.Left=");
    memset(big + n, 'F', 6000); n += 6000; big[n++] = '\n';
    n += sprintf(big + n, "ViewSize=6\n");
    big[n] = 0;
    set_file(big);
    o = d;
    CHECK(CGD_LoadSettings(&o) == 0 && o.ViewSize == 6
        && o.Keymap[0] == d.Keymap[0], "long lines");
}

int main(void)
{
    test_roundtrip();
    test_none_and_garbage();
    test_rejected();
    test_numeric_values();
    if(fails) {
        printf("FAIL settings (%d failures)\n", fails);
        return 1;
    }
    printf("PASS settings\n");
    return 0;
}
