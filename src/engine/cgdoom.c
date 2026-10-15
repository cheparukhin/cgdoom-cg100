/* CGDoom: portable glue between the engine and the platform.

   main(), WAD selection, file mapping and the launcher UI live in the
   platform (src/host, src/gint). This file keeps the global options, the
   statistics, the settings file and the delayed file writes, all on top of
   the PL_* API. */

#include "os.h"
#include "cgdoom.h"

#include "doomtype.h"
#include "m_misc.h"
#include "z_zone.h"

void *CGD_malloc(int size)
{
	void *p = malloc(size);
	if (!p)
		I_Error ("CGD_malloc(%d) failure", size);
	return p;
}

void *CGD_calloc(int size)
{
	void *p = CGD_malloc(size);
	memset(p, 0, size);
	return p;
}

void *CGD_realloc(void *p, int size)
{
	p = (p == NULL) ? malloc(size) : realloc(p, size);
	if (!p)
		I_Error ("CGD_realloc(%d) failure", size);
	return p;
}

int strnicmp(const char *s1,const char *s2,int count)
{
	for(int i = 0; i < count; i++) {
		int c1 = (s1[i] >= 'a' && s1[i] <= 'z') ? s1[i] & ~0x20 : s1[i];
		int c2 = (s2[i] >= 'a' && s2[i] <= 'z') ? s2[i] & ~0x20 : s2[i];

		if(c1 != c2 || !c1 || !c2)
			return c1 - c2;
	}
	return 0;
}

/* Default for platforms that do not define PL_WadIsSlow() (gint and the
   host do: gint returns 1 in flash-map/BFile modes, 0 with the WAD in RAM).
   Weak, so a platform definition replaces it. */
__attribute__((weak)) int PL_WadIsSlow(void)
{
	return 0;
}

CGDoom_Options CGD_Options;
CGDoom_Stats CGD_Stats;

/* Global options */
int CGD_SingleEpisodeUltimate = 0;
int CGD_TitleDemos = 1;
int CGD_Frameskip = 1;
int CGD_AutoFrameskip = 0;
const char *CGD_WADFileName = NULL;
int CGD_RecordDemoSlot = -1;
int CGD_PlayDemoOnly = 0;
int CGD_Features = CGD_FEATURES_DEFAULT;
const char *const CGD_FeatureNames[CGD_FEATURE_COUNT + 1] = {
	"aspect",
	"colornums",
	"crosshair",
	"automap",
	"fullhud",
	NULL,
};
int CGD_TrueColorExact = 0;
int CGD_PaletteRedraw = 0;
int CGD_TimeDemo = 0;
int CGD_TimeDemoCount = 0;
CGD_TimeDemoRun CGD_TimeDemoRuns[CGD_TIMEDEMO_MAX];

/* Delayed file accesses */
CGDoom_DelayedFileWrite CGD_DelayedSaves[CGD_DELAYEDSAVES_COUNT] = { 0 };

//---
// Delayed file writes
//---

int CGD_WriteDelayedFile(int i)
{
	CGDoom_DelayedFileWrite *dfw = &CGD_DelayedSaves[i];
	if(dfw->data == NULL)
		return 0;

	int rc = PL_FileWrite(dfw->filename, dfw->data, dfw->size);
	Z_Free(dfw->data);
	dfw->data = NULL;
	dfw->size = 0;
	return rc;
}

int CGD_FlushDelayedWrites(void)
{
	int failed = 0;
	for(int i = 0; i < CGD_DELAYEDSAVES_COUNT; i++) {
		if(CGD_WriteDelayedFile(i) < 0)
			failed++;
	}
	return failed;
}

//---
// Setting save/load
//---

void CGD_DefaultOptions(CGDoom_Options *o)
{
	memset(o, 0, sizeof *o);
	o->DelaySaves          = 1;
	o->LowDetail           = 0;
	o->ViewSize            = 10;
	CGD_CopyKeymap(o->Keymap, CGD_Keymap_ThumbsOnly);
}

int CGD_SaveSettings(const CGDoom_Options *o)
{
	/* ~30 bytes per keymap entry plus a few lines of options. Not
	   CGD_malloc: without memory the settings are not saved, which is no
	   reason for a fatal error at the end of a game */
	char *out_start = malloc(2048);
	if(!out_start)
		return -1;
	char *out = out_start;

	/* Version identifcation (just in case something goes wrong) */
	out += sprintf(out, "Version=%d\n", CGDOOM_SETTINGS_FILE_VERSION);

	/* Settings */
	out += sprintf(out, "DelaySaves=%d\n",
		o->DelaySaves);
	out += sprintf(out, "LowDetail=%d\n",
		o->LowDetail);
	out += sprintf(out, "ViewSize=%d\n",
		o->ViewSize);

	/* Keyboard layout */
	for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
		out += sprintf(out, "Keymap.%s=%s\n",
			CGD_DoomKey_TechnicalName(CGD_KeymapEntries[i]),
			CGD_PhysicalKey_TechnicalName(o->Keymap[i]));
	}

	int rc = PL_FileWrite(CGDOOM_SETTINGS_FILE, out_start, out - out_start);
	free(out_start);
	return rc;
}

/* Whether the key of a settings line (keylen characters) is exactly `name`
   (a plain strncmp(line, name, keylen) would accept any prefix, so "View="
   would set the first key starting with "View") */
static int SettingIs(const char *key, int keylen, const char *name)
{
	return (int)strlen(name) == keylen && !strncmp(key, name, keylen);
}

/* Integer value of a settings line: an optional '-' and 1..9 digits, nothing
   else (atoi() skips leading whitespace, newlines included, so an empty
   value such as "LowDetail=" would read the digits of the next line).
   Returns 0 and leaves *out alone for anything else. */
static int SettingInt(const char *value, int vlen, int *out)
{
	int i = 0, neg = 0, v = 0;
	if(vlen > 0 && value[0] == '-')
		neg = 1, i = 1;
	if(vlen - i < 1 || vlen - i > 9)
		return 0;
	for(; i < vlen; i++) {
		if(value[i] < '0' || value[i] > '9')
			return 0;
		v = 10 * v + (value[i] - '0');
	}
	*out = neg ? -v : v;
	return 1;
}

int CGD_LoadSettings(CGDoom_Options *o)
{
	int size = PL_FileSize(CGDOOM_SETTINGS_FILE);
	if(size <= 0 || size > 16384)
		return -1;

	char *buffer = CGD_malloc(size + 1);
	int length = PL_FileRead(CGDOOM_SETTINGS_FILE, buffer, size);
	if(length <= 0) {
		free(buffer);
		return -1;
	}
	if(length > size)
		length = size;
	buffer[length] = 0;

	CGDoom_Options n = *o;

	/* Parse every line on the form "<Key>=<Value>\n" (or "\r\n"). Keys
	   and values match exactly; unknown keys, unknown values (including
	   empty or non-numeric values of numeric options) and lines without
	   '=' are ignored, among them the options of earlier builds
	   (DeveloperInfo, EnableDemos, Autostart, FlatFloors, the gint
	   platform's CG.* lines and Keymap lines of removed actions). A NUL
	   byte ends the file. */
	const char *line, *nextline=NULL;
	const char *end = buffer + strlen(buffer);

	for(line = buffer; line < end; line = nextline) {
		/* Start of next line, or end of string */
		nextline = strchr(line, '\n');
		nextline = nextline ? nextline + 1 : end;

		if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;

		const char *eq = memchr(line, '=', nextline - line);
		if(!eq)
			continue;
		const char *key = line;
		int klen = eq - line;
		const char *value = eq+1;
		/* Value length, without the end-of-line */
		int vlen = nextline - value;
		while(vlen > 0 && (value[vlen-1] == '\n' || value[vlen-1] == '\r'))
			vlen--;

		if(SettingIs(key, klen, "Version")) {
			int version = -1;
			if(!SettingInt(value, vlen, &version)
				|| version != CGDOOM_SETTINGS_FILE_VERSION) {
				free(buffer);
				return -1;
			}
		}

		/* "Keymap.<action>=<key>"; <key> = NONE unbinds the action
		   (CGD_SaveSettings writes NONE for an action unbound in the
		   launcher; ignoring it here would bring the preset key back at
		   the next start) */
		else if(klen > 7 && !strncmp(key, "Keymap.", 7)) {
			int k1 = CGD_DoomKey_FromTechnicalName(key+7, klen-7);
			int k2 = SettingIs(value, vlen, "NONE") ? -1 :
				CGD_PhysicalKey_FromTechnicalName(value, vlen);
			int valid = k2 >= 0 || SettingIs(value, vlen, "NONE");

			if(k1 > 0 && valid) {
				for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
					if(CGD_KeymapEntries[i] == k1) {
						n.Keymap[i] = k2;
						break;
					}
				}
			}
		}

		else if(SettingIs(key, klen, "DelaySaves"))
			SettingInt(value, vlen, &n.DelaySaves);
		else if(SettingIs(key, klen, "LowDetail"))
			SettingInt(value, vlen, &n.LowDetail);
		else if(SettingIs(key, klen, "ViewSize"))
			SettingInt(value, vlen, &n.ViewSize);
	}

	free(buffer);
	*o = n;
	return 0;
}
