/* CGDoom: strict 8.3 names for the files the engine creates.

   The fx-CG100 storage only takes DOS-style 8.3 names made of
   A-Z a-z 0-9 ! # $ % , ( ) + - ; = @ [ ] ^ _ ` and space, compared without
   case. The engine creates:

     CGDOOM.CFG          settings (CGDOOM_SETTINGS_FILE)
     <P>_<s>.DSG         save slot s (0..9)          e.g. DOOM1_0.DSG
     <P>D<nn>.LMP        recorded demo slot nn (0..99) e.g. DOOM1D03.LMP

   <P> is a prefix of at most 5 characters derived from CGD_WADFileName
   (CGD_FilePrefix), so both bases fit in 8 characters:

   1. Take the base name: drop any directory part and everything from the
      first '.' on (CGD_WADFileName is meant to be "doom1", but "doom1.wad"
      works too). Fold to upper case. Empty or NULL -> "DOOM".
   2. If it has at most 5 characters, all in [A-Z0-9_-], it is the prefix
      as is: DOOM1, DOOM2, DOOM, TNT, CHEX.
   3. Otherwise the prefix is its first 3 characters (each one outside
      [A-Z0-9_-] replaced by '_') followed by 2 base-36 digits [0-9A-Z] of
      the FNV-1a hash of the whole upper-cased base name:
      FREEDOOM1 -> FRE0P, FREEDOOM2 -> FREXS, PLUTONIA -> PLUFN,
      DOOMU1 -> DOO9I (tests/names_test.c, make test-names).

   Deterministic: a function of the WAD name only. Collisions: names from
   rule 2 are distinct per WAD name (up to case, as the file system), and
   the slot part is parsed from the end, so <P>_<s> and <P>D<nn> never
   collide across prefixes or slots. Two long names collide only if their
   first 3 characters and their hashes mod 1296 match (1/1296 per pair with
   the same 3-character start; FREEDOOM1/FREEDOOM2/FREEDM are distinct), and
   a hashed prefix equals a rule-2 prefix only if some WAD is literally named
   like it. A platform must pass the whole WAD base name ("freedoom1"), not a
   truncated one, or WADs that share a start share their saves. */

#include "cgdoom.h"

#include <string.h>

/* Characters the Casio file system accepts in names (besides '.') */
static int name_char_ok(char c)
{
    if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9'))
        return 1;
    return c != 0 && strchr("!#$%,()+-;=@[]^_` ", c) != NULL;
}

int CGD_ValidName83(const char *name)
{
    int base = 0, ext = -1;
    if(!name)
        return 0;
    for(const char *c = name; *c; c++) {
        if(*c == '.') {
            if(ext >= 0 || base == 0)
                return 0;
            ext = 0;
            continue;
        }
        if(!name_char_ok(*c))
            return 0;
        if(ext >= 0) {
            if(++ext > 3)
                return 0;
        }
        else if(++base > 8)
            return 0;
    }
    return base > 0 && ext != 0;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

/* Characters kept in prefixes */
static int prefix_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
        c == '-';
}

void CGD_FilePrefix(char out[CGD_PREFIX_MAX + 1], const char *wadname)
{
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char *b = wadname ? wadname : "";
    int len, clean = 1;
    uint32_t h = 2166136261u;

    /* 1. base name */
    for(const char *p = b; *p; p++) {
        if(*p == '/' || *p == '\\')
            b = p + 1;
    }
    for(len = 0; b[len] && b[len] != '.'; len++) {
        char c = upper(b[len]);
        if(!prefix_char_ok(c))
            clean = 0;
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    if(len == 0) {
        strcpy(out, "DOOM");
        return;
    }

    /* 2. short clean names are kept */
    if(clean && len <= CGD_PREFIX_MAX) {
        for(int i = 0; i < len; i++)
            out[i] = upper(b[i]);
        out[len] = 0;
        return;
    }

    /* 3. 3 characters + 2 base-36 digits of the hash */
    int i;
    for(i = 0; i < 3 && i < len; i++) {
        char c = upper(b[i]);
        out[i] = prefix_char_ok(c) ? c : '_';
    }
    h %= 36 * 36;
    out[i++] = digits[h / 36];
    out[i++] = digits[h % 36];
    out[i] = 0;
}

void CGD_SaveFileName(char out[CGD_NAME_MAX], int slot)
{
    char p[CGD_PREFIX_MAX + 1];
    int n;
    CGD_FilePrefix(p, CGD_WADFileName);
    n = strlen(p);
    memcpy(out, p, n);
    out[n++] = '_';
    out[n++] = '0' + ((slot < 0 ? 0 : slot) % 10);
    strcpy(out + n, ".DSG");
}

void CGD_DemoFileName(char out[CGD_NAME_MAX], int slot)
{
    char p[CGD_PREFIX_MAX + 1];
    int n;
    CGD_FilePrefix(p, CGD_WADFileName);
    n = strlen(p);
    if(slot < 0)
        slot = 0;
    memcpy(out, p, n);
    out[n++] = 'D';
    out[n++] = '0' + (slot / 10) % 10;
    out[n++] = '0' + slot % 10;
    strcpy(out + n, ".LMP");
}
