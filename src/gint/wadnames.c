/* CGDoom gint platform: WAD names for the engine and the launcher.

   - gnames_wad_base(): the WAD base name handed to the engine as
     CGD_WADFileName. The engine derives its 8.3 save/demo prefix from the
     WHOLE base name (CGD_FilePrefix, src/engine/cgdoom-names.c); the
     launcher used to cut it to 6 characters, so freedoom1/freedoom2 and
     plutonia/plutonia2 shared their saves and demos (gint review round 1).
   - gnames_demo_wad(): which WAD a demo in storage belongs to. Demos are
     recorded as "<prefix>D<nn>.LMP" with the engine's prefix; the launcher
     used to take the first WAD whose base name prefixes the demo name, so
     DOOM1D03.LMP went to doom.wad when both were present, and hashed
     prefixes (FRE0P for freedoom1) never matched at all.

   Pure logic (host-tested in tests/gint-host with the engine's
   cgdoom-names.c). */

#include "cgd_gint.h"
#include "cgdoom.h"

#include <string.h>

static char upper(char c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static bool prefix_eq(char const *s, char const *p, int len)
{
	for(int i = 0; i < len; i++) {
		if(!s[i] || upper(s[i]) != upper(p[i]))
			return false;
	}
	return true;
}

static int base_len(char const *name)
{
	char const *dot = strchr(name, '.');
	return dot ? (int)(dot - name) : (int)strlen(name);
}

void gnames_wad_base(char *out, size_t size, char const *wadname)
{
	size_t i = 0;
	if(size == 0)
		return;
	/* Everything up to the first '.' (CGD_FilePrefix stops there too) */
	for(; wadname && wadname[i] && wadname[i] != '.' && i < size - 1; i++)
		out[i] = wadname[i];
	out[i] = 0;
}

/* "<p>D<nn>.LMP", case-insensitive, nothing else */
static bool is_engine_demo(char const *demo, char const *p)
{
	int l = (int)strlen(p);
	if((int)strlen(demo) != l + 7 || !prefix_eq(demo, p, l))
		return false;
	char const *d = demo + l;
	return upper(d[0]) == 'D'
		&& d[1] >= '0' && d[1] <= '9' && d[2] >= '0' && d[2] <= '9'
		&& prefix_eq(d + 3, ".LMP", 4);
}

int gnames_demo_wad(gfs_entry_t const *wads, int count, char const *demo)
{
	if(!demo)
		return -1;

	/* 1. Recorded by this engine: exact "<prefix>D<nn>.LMP" */
	for(int i = 0; i < count; i++) {
		char base[GFS_LIST_NAME];
		char p[CGD_PREFIX_MAX + 1];
		gnames_wad_base(base, sizeof base, wads[i].name);
		CGD_FilePrefix(p, base);
		if(is_engine_demo(demo, p))
			return i;
	}

	/* 2. Other names ("doom1_demo.lmp"...): the LONGEST WAD base name that
	   starts the demo name and is followed by a separator, so "doom1_x"
	   goes to doom1.wad even when doom.wad is present */
	int best = -1, best_len = 0;
	for(int i = 0; i < count; i++) {
		int len = base_len(wads[i].name);
		if(len <= best_len || !prefix_eq(demo, wads[i].name, len))
			continue;
		char c = demo[len];
		if(c == '_' || c == '-' || c == '.' || c == ' ') {
			best = i;
			best_len = len;
		}
	}
	return best;
}
