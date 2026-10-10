/* CGDoom gint platform: entry point, launcher, game start, exit path.

   Startup (before any OS call):
     record r15 and check the model and OS string (sys.c): anything but the
     fx-CG100 with OS 02.00 -> message, exit, with nothing else set up.
     Then remove the OS heap arena (memory.c), snapshot the UTLB and install
     the TLB-miss handler (tlb.c), start the timers, allocate the frame
     buffer and the private stack. Too little heap -> message, exit.
   Launcher (port of CGDoom's cgdoom-ui.c UI_Main to gint on 396x224):
     WAD list, skill, start episode/map, title screen mode, demo recording
     and replay, controls editor, options (overclock, display path,
     frameskip, FPS counter, view size, detail, flat floors), advanced
     options, diagnostics.
   Game: PL_WadOpen() (RAM image or block map) before D_DoomMain(), which
     takes the zones (PL_GetZones) and returns when the player quits.
   Exit path (also reached from PL_Fatal through longjmp): wait for keys to
     be released, finish the LCD DMA, close the WAD, write the delayed files
     in one world switch, stop the timers, return from main(). Then, on this
     path and on the abort() path alike, the platform_quit() destructor
     closes the WAD file if it is still open (gwad_quit) and restores MPM's
     UTLB layout (gtlb_quit), before gint's kquit restores the OS clock, LCD
     window and drivers; MPM redraws.
   Safe start (final review): DEL held when the add-in starts skips the
     UTLB executor and pinning, the overclock and the private stack for
     that session (a way out of a startup problem on new hardware without
     a rebuild).

   The engine is not re-entrant (static state), so after a game we exit to
   MPM like CGDoom did, instead of returning to the launcher. */

#include "cgd_gint.h"

/* Engine */
#include "cgdoom.h"
#include "d_main.h"
#include "g_game.h"
#include "w_wad.h"
#include "doomstat.h"

_Static_assert(GWAD_LUMP_PAD >= W_LUMP_PAD,
	"RAM-image pads must cover the engine's lump pad");

/* gint (no keyboard header: the engine defines KEY_* macros) */
#include <gint/display.h>
#include <gint/gint.h>
#include <gint/hardware.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gopt_t g_opt;

/* Engine globals without a header declaration */
extern boolean fpscounteractive;
extern boolean demoplayback;
extern boolean demorecording;
extern boolean timingdemo;


static jmp_buf g_exit_jmp;
static volatile bool g_exit_ready = false;
static volatile bool g_in_fatal = false;
static volatile bool g_game_running = false;
/* exit_path() has started (gint review round 3.1: gquit_poll stays out) */
static volatile bool g_in_exit = false;

/* DEL held at startup: no UTLB pinning, no overclock, no private stack */
static bool g_safe_start = false;

/* Private stack */
static uint32_t *g_stack = NULL;
static size_t g_stack_words = 0;
static int g_stack_peak = -1;
#define STACK_FILL 0xa5a55a5au

//---
// Option defaults, load and save
//---

static void options_defaults(void)
{
	CGD_DefaultOptions(&CGD_Options);
	int n;
	gkm_preset_t const *p = gkm_presets(&n);
	CGD_CopyKeymap(CGD_Options.Keymap, p[0].map);

	g_opt.overclock = GCLK_F4;
	g_opt.display = GDISP_DMA;
	g_opt.frameskip = -1;
	g_opt.fps_counter = 0;
	g_opt.wad_in_ram = 1;
	g_opt.skill = 2;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void parse_platform_line(char const *key, int klen, char const *value)
{
	#define IS(k) (klen == (int)strlen(k) && !strncmp(key, k, klen))
	int v = atoi(value);
	if(IS("Overclock"))
		g_opt.overclock = clampi(v, 0, GCLK_COUNT - 1);
	else if(IS("Display"))
		g_opt.display = clampi(v, 0, GDISP_COUNT - 1);
	else if(IS("Frameskip"))
		g_opt.frameskip = clampi(v, -1, 3);
	else if(IS("FPS"))
		g_opt.fps_counter = (v != 0);
	else if(IS("WadRam"))
		g_opt.wad_in_ram = (v != 0);
	else if(IS("Skill"))
		g_opt.skill = clampi(v, 0, 4);
	#undef IS
}

static void load_platform_settings(void)
{
	int size = PL_FileSize(CGDOOM_SETTINGS_FILE);
	if(size <= 0 || size > 16384)
		return;
	char *buf = malloc(size + 1);
	if(!buf)
		return;
	int n = PL_FileRead(CGDOOM_SETTINGS_FILE, buf, size);
	if(n <= 0) {
		free(buf);
		return;
	}
	buf[n] = 0;

	/* Actions unbound with DEL ("Keymap.<action>=NONE") are handled by
	   the engine's CGD_LoadSettings() (gint review round 1, §13). */
	for(char *line = buf; *line; ) {
		char *end = strchr(line, '\n');
		if(end)
			*end = 0;
		if(!strncmp(line, "CG.", 3)) {
			char *eq = strchr(line, '=');
			if(eq)
				parse_platform_line(line + 3, eq - line - 3, eq + 1);
		}
		if(!end)
			break;
		line = end + 1;
	}
	free(buf);
}

static char g_append[256];

static char const *settings_appender(void)
{
	snprintf(g_append, sizeof g_append,
		"CG.Overclock=%d\nCG.Display=%d\nCG.Frameskip=%d\n"
		"CG.FPS=%d\nCG.WadRam=%d\nCG.Skill=%d\n",
		g_opt.overclock, g_opt.display, g_opt.frameskip, g_opt.fps_counter,
		g_opt.wad_in_ram, g_opt.skill);
	return g_append;
}

static CGDoom_Options g_saved_engine_opts;
static gopt_t g_saved_opts;
/* Set by load_settings(): before that, g_saved_* are not a baseline and
   saving would write default settings over the user's file (the idle hook
   runs from the startup error screens since gint review round 2) */
static bool g_settings_loaded = false;

static void load_settings(void)
{
	options_defaults();
	gfs_settings_name = CGDOOM_SETTINGS_FILE;
	CGD_LoadSettings(&CGD_Options);
	load_platform_settings();

	memcpy(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options);
	g_saved_opts = g_opt;
	g_settings_loaded = true;
}

/* Queue the settings file if anything changed (written at exit) */
static void save_settings_if_changed(void)
{
	if(!g_settings_loaded)
		return;
	if(!memcmp(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options)
		&& !memcmp(&g_saved_opts, &g_opt, sizeof g_opt))
		return;

	gfs_settings_appender = settings_appender;
	CGD_SaveSettings(&CGD_Options);
	gfs_settings_appender = NULL;

	memcpy(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options);
	g_saved_opts = g_opt;
}

//---
// Launcher screens
//---

static char const * const skill_names[5] = {
	"I'm too young to die", "Hey, not too rough", "Hurt me plenty",
	"Ultra-Violence", "Nightmare!",
};

static char const *footer_main(void)
{
	return "OK/EXE: select   </>: change   BACK: quit";
}

static char const *footer_sub(void)
{
	return "OK/EXE: select   </>: change   BACK: return";
}

/* Engine renderer settings (CGD_Options.ViewSize/LowDetail/FlatFloors,
   saved in CGDOOM.CFG by the engine; R_Init applies them and the in-game
   view size keys / detail menu item update them, see end_game_session) */
static int view_size(void)
{
	/* The engine reads values outside 3..11 as 10 (cgdoom.h) */
	int v = CGD_Options.ViewSize;
	return (v >= 3 && v <= 11) ? v : 10;
}

static char const *view_size_name(int v)
{
	static char buf[24];
	if(v == 11)
		return "Full screen";
	if(v == 10)
		return "Status bar";
	snprintf(buf, sizeof buf, "%d (smaller)", v);
	return buf;
}

static void screen_options(void)
{
	ui_layout_t l;
	ui_layout_init(&l);

	while(1) {
		ui_frame_start(&l, "Options");
		int i_clk = ui_item(&l, "CPU clock:", "< %s >",
			gclk_name(g_opt.overclock));
		if(g_opt.overclock == GCLK_PTUNE4_F5)
			ui_note(&l, UI_WARN, "Bus 118 MHz: may crash some units!");
		else if(g_opt.overclock == GCLK_F4)
			ui_note(&l, UI_DIM, "236 MHz CPU, bus unchanged (safe).");
		int i_disp = ui_item(&l, "Display:", "< %s >",
			gvid_mode_name(g_opt.display));
		int i_fs = ui_item(&l, "Frameskip:", g_opt.frameskip < 0 ?
			"< Auto (max %d) >" : "< %d >", g_opt.frameskip < 0 ?
			CGD_AUTOFRAMESKIP_DEFAULT : g_opt.frameskip);
		int i_fps = ui_item(&l, "FPS counter:", "%s",
			g_opt.fps_counter ? "[x]" : "[ ]");
		ui_space(&l, 4);
		int i_vs = ui_item(&l, "View size:", "< %s >",
			view_size_name(view_size()));
		int i_det = ui_item(&l, "Detail:", "< %s >",
			CGD_Options.LowDetail ? "Low" : "High");
		int i_flat = ui_item(&l, "Flat floors:", "%s",
			CGD_Options.FlatFloors ? "[x]" : "[ ]");
		ui_note(&l, UI_DIM, "Faster: smaller view, low detail, flat floors.");
		ui_frame_end(&l, footer_sub());

		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			return;

		int d = (k == GK_LEFT) ? -1 : (k == GK_RIGHT || k == GK_OK) ? 1 : 0;
		if(!d)
			continue;

		if(l.focus == i_clk) {
			int v = (g_opt.overclock + d + GCLK_COUNT) % GCLK_COUNT;
			if(v == GCLK_PTUNE4_F5 && !ui_confirm("Experimental overclock",
				"Ptune4 F5 runs the SDRAM bus at 118 MHz (CPU 236 MHz). "
				"gint does not offer it on the fx-CG100 because some "
				"units cannot handle the faster bus: crashes or "
				"corrupted data are possible. gpSP and KhiCAS use it on "
				"many fx-CG100 units. Enable it?"))
				continue;
			g_opt.overclock = v;
		}
		else if(l.focus == i_disp)
			g_opt.display = (g_opt.display + d + GDISP_COUNT) % GDISP_COUNT;
		else if(l.focus == i_fs)
			g_opt.frameskip = (g_opt.frameskip + 1 + d + 5) % 5 - 1;
		else if(l.focus == i_fps)
			g_opt.fps_counter ^= 1;
		else if(l.focus == i_vs)
			CGD_Options.ViewSize = (view_size() - 3 + d + 9) % 9 + 3;
		else if(l.focus == i_det)
			CGD_Options.LowDetail = !CGD_Options.LowDetail;
		else if(l.focus == i_flat)
			CGD_Options.FlatFloors = !CGD_Options.FlatFloors;
	}
}

static void screen_advanced(void)
{
	ui_layout_t l;
	ui_layout_init(&l);

	while(1) {
		ui_frame_start(&l, "Advanced options");
		int i_dev = ui_item(&l, "Developer info:", "%s",
			CGD_Options.DeveloperInfo ? "[x]" : "[ ]");
		ui_note(&l, UI_DIM, "Statistics before and after the game.");
		int i_ram = ui_item(&l, "Copy WAD to RAM if it fits:", "%s",
			g_opt.wad_in_ram ? "[x]" : "[ ]");
		ui_note(&l, UI_DIM, "Otherwise lumps are read from flash.");
		ui_space(&l, 4);
		ui_note(&l, UI_DIM, "%u KiB free", (unsigned)(gmem_total_free()
			>> 10));
		ui_frame_end(&l, footer_sub());

		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			return;
		if(k != GK_OK && k != GK_LEFT && k != GK_RIGHT)
			continue;

		if(l.focus == i_dev)
			CGD_Options.DeveloperInfo ^= 1;
		else if(l.focus == i_ram)
			g_opt.wad_in_ram ^= 1;
	}
}

static void screen_controls(void)
{
	ui_layout_t l;
	ui_layout_init(&l);
	int npresets;
	gkm_preset_t const *presets = gkm_presets(&npresets);
	int preset = 0;

	CGDoom_Keymap backup;
	CGD_CopyKeymap(backup, CGD_Options.Keymap);

	while(1) {
		ui_frame_start(&l, "Controls");
		int i_preset = ui_item(&l, "Load preset:", "< %s >",
			presets[preset].name);
		ui_note(&l, UI_DIM, "Doom menu: %s (fixed)   Enter: %s",
			gkm_key_name(gkm_menu_key()), gkm_enter_key2() >= 0
			? "EXE or OK (if unbound)" : "EXE");
		int first = -1;
		for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
			int idx = ui_item(&l, CGD_DoomKey_DisplayName(
				CGD_KeymapEntries[i]), "%s",
				gkm_key_name(CGD_Options.Keymap[i]));
			if(first < 0)
				first = idx;
		}
		int i_undo = ui_item(&l, "Discard changes", NULL);
		ui_frame_end(&l, "OK/EXE: rebind  DEL: unbind  BACK: done");

		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			return;

		if(l.focus == i_preset) {
			if(k == GK_LEFT)
				preset = (preset + npresets - 1) % npresets;
			else if(k == GK_RIGHT)
				preset = (preset + 1) % npresets;
			else if(k == GK_OK)
				CGD_CopyKeymap(CGD_Options.Keymap, presets[preset].map);
		}
		else if(l.focus == i_undo && k == GK_OK) {
			CGD_CopyKeymap(CGD_Options.Keymap, backup);
		}
		else if(first >= 0 && l.focus >= first
			&& l.focus < first + CGDOOM_KEYMAP_SIZE) {
			int e = l.focus - first;
			if(k == GK_DEL)
				CGD_Options.Keymap[e] = -1;
			else if(k == GK_OK) {
				char line[80];
				snprintf(line, sizeof line, "Press a key for: %s",
					CGD_DoomKey_DisplayName(CGD_KeymapEntries[e]));
				ui_status("Controls", line,
					"(release first; BACK/HOME: cancel)");
				/* -1 after the idle time: cancels the capture, then
				   the idle hook of the controls screen takes over */
				int code = ginp_wait_raw_key(GINP_IDLE_MS);
				ginp_wait_release();
				/* BACK/HOME cancel; AC/ON and the Doom menu key are
				   refused (gint review rounds 1, 2) */
				char const *why = NULL;
				int verdict = gkm_rebind_check(code, &why);
				if(verdict == GKM_BIND_OK) {
					/* One action per key (final review: the older
					   binding stayed, and both fired) */
					int other = gkm_find_binding(CGD_Options.Keymap,
						code, e);
					CGD_Options.Keymap[e] = code;
					if(other >= 0) {
						CGD_Options.Keymap[other] = -1;
						char t[120];
						snprintf(t, sizeof t, "%s was bound to %s, "
							"which is now unbound.", gkm_key_name(code),
							CGD_DoomKey_DisplayName(
							CGD_KeymapEntries[other]));
						ui_message("Controls", t, true);
					}
				}
				else if(verdict == GKM_BIND_RESERVED)
					ui_message("Controls", why, true);
			}
		}
	}
}

/* "512 B", "12 KiB", "3.4M" (final review: demos under 1 KiB showed
   "0 KiB" and WADs under 100 KB "0.0M") */
static char const *size_text(char *buf, size_t n, int bytes)
{
	if(bytes < 1024)
		snprintf(buf, n, "%d B", bytes);
	else if(bytes < 1000000)
		snprintf(buf, n, "%d KiB", bytes >> 10);
	else
		snprintf(buf, n, "%d.%dM", bytes / 1000000,
			(bytes % 1000000) / 100000);
	return buf;
}

/* Returns the index of the entry picked, or -1. With owners (the WAD
   list), each entry is a demo and shows the WAD it was recorded with
   (final review); more: the list was cut at its maximum. */
static int screen_pick(char const *title, gfs_entry_t *list, int count,
	gfs_entry_t const *owners, int owner_count, bool more)
{
	ui_layout_t l;
	ui_layout_init(&l);

	while(1) {
		ui_frame_start(&l, title);
		for(int i = 0; i < count; i++) {
			char sz[16];
			size_text(sz, sizeof sz, list[i].size);
			if(owners) {
				int w = gnames_demo_wad(owners, owner_count,
					list[i].name);
				ui_item(&l, list[i].name, "%s, %s", w >= 0
					? owners[w].name : "WAD unknown", sz);
			}
			else
				ui_item(&l, list[i].name, "%s", sz);
		}
		if(more)
			ui_note(&l, UI_WARN, "More files exist: only %d are listed.",
				count);
		ui_frame_end(&l, footer_sub());

		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			return -1;
		if(k == GK_OK)
			return l.focus;
	}
}

static char const *wad_action(char const *name)
{
	static struct { char const *file, *action; } const known[] = {
		{ "doom.wad",   "Play Doom (shareware)" },
		{ "doom1.wad",  "Play Doom (shareware)" },
		{ "doomu.wad",  "Play Ultimate Doom" },
		{ "doomu1.wad", "Play Ultimate Doom E1" },
		{ "doomu2.wad", "Play Ultimate Doom E2" },
		{ "doomu3.wad", "Play Ultimate Doom E3" },
		{ "doomu4.wad", "Play Ultimate Doom E4" },
		{ "doom2.wad",  "Play Doom II" },
		{ "freedoom1.wad", "Play Freedoom 1" },
		{ "freedoom2.wad", "Play Freedoom 2" },
	};
	for(size_t i = 0; i < sizeof known / sizeof *known; i++) {
		if(!strnicmp(name, known[i].file, strlen(known[i].file) + 1))
			return known[i].action;
	}
	return "Play other WAD";
}

typedef struct {
	int wad;          /* index in wads[], -1 = quit */
	int demo;         /* index in demos[], -1 = none */
	int episode, map;
	int title;        /* TS_* for this launch (shown) */
	int title_pref;   /* TS_* the user chose: the saved setting */
	bool start_set;   /* episode/map changed: this launch skips the title */
	int record_slot;  /* -1 = none */
} launch_t;

enum { TS_SKIP, TS_NODEMOS, TS_FULL };

/* The last start map. The engine is vanilla's: HUD titles, par times and
   intermission names exist for the 32 Doom II maps only, and no exit leads
   past MAP32 (G_InitNew limits a start to it as well). */
#define LAUNCH_MAP_MAX 32

/* Recording a demo implies a direct start (vanilla's -record sets
   autostart) and no title demos (gint review round 3.3). Before the first
   game G_Ticker would not record, and G_CheckDemoStatus() on a quit from
   the title stored through demo_p == NULL; after a title demo,
   G_DoPlayDemo() had replaced demobuffer and demo_p with the demo lump, so
   the "recording" wrote into it and saved it under the user's slot. The
   slot wins: choosing one selects Skip, and leaving Skip clears it.
   Final review: that Skip, and the one a changed start episode/map
   selects, is for this launch only. The saved "Title screen" setting is
   title_pref, which only the Title row changes; going back to no slot
   restores it (unless a start map was chosen). */
static void launcher_normalize(launch_t *L)
{
	if(L->record_slot >= 0)
		L->title = TS_SKIP;
}

/* The user's title-screen setting in the engine options (saved) */
static void launcher_commit(launch_t const *L)
{
	CGD_Options.AutoStart = (L->title_pref == TS_SKIP);
	CGD_Options.EnableDemos = (L->title_pref == TS_FULL);
}

/* The saved setting while a game runs with this launch's title choice
   (run_game -> end_game_session) */
static bool g_title_saved = false;
static int g_title_autostart, g_title_demos;

static void launcher(gfs_entry_t *wads, int wad_count, gfs_entry_t *demos,
	int demo_count, launch_t *out)
{
	ui_layout_t l;
	ui_layout_init(&l);

	out->wad = -1;
	out->demo = -1;
	if(out->episode < 1) out->episode = 1;
	out->map = clampi(out->map, 1, LAUNCH_MAP_MAX);
	out->title_pref = CGD_Options.AutoStart ? TS_SKIP
		: CGD_Options.EnableDemos ? TS_FULL : TS_NODEMOS;
	out->title = (out->start_set || out->record_slot >= 0) ? TS_SKIP
		: out->title_pref;

	while(1) {
		launcher_normalize(out);
		ui_frame_start(&l, "CGDoom for fx-CG100 / Graph Math+");

		if(wad_count == 0) {
			ui_note(&l, UI_WARN, "No *.wad file in the storage root.");
			ui_note(&l, UI_DIM, "Connect to a PC (USB) and copy a WAD such");
			ui_note(&l, UI_DIM, "as doom1.wad to the top folder. Smaller:");
			ui_note(&l, UI_DIM, "tools/wadtool.py pack (on the PC) first.");
		}
		for(int i = 0; i < wad_count; i++) {
			char sz[16];
			ui_item(&l, wad_action(wads[i].name), "%s (%s)",
				wads[i].name, size_text(sz, sizeof sz, wads[i].size));
		}
		if(gfs_list_more & GFS_MORE_WADS)
			ui_note(&l, UI_WARN, "More WAD files exist: only %d listed.",
				wad_count);
		ui_space(&l, 4);

		int i_skill = ui_item(&l, "Skill:", "< %s >",
			skill_names[g_opt.skill]);
		int i_ep = ui_item(&l, "Start at episode:", "< %d >", out->episode);
		int i_map = ui_item(&l, "Start at map:", "< %d >", out->map);
		int i_title = ui_item(&l, "Title screen:", "%s",
			out->title == TS_SKIP ? "Skip >" :
			out->title == TS_NODEMOS ? "< No demos >" : "< Full");
		/* Skill and the start map are used by a direct start only
		   (d_main.c autostart); the Doom menu asks otherwise */
		if(out->title != TS_SKIP)
			ui_note(&l, UI_DIM, "Skill: used only with Skip (the Doom "
				"menu asks).");
		else if(out->title_pref != TS_SKIP)
			ui_note(&l, UI_DIM, "Skip for this game only (saved: %s).",
				out->title_pref == TS_FULL ? "Full" : "No demos");
		int i_rec = ui_item(&l, "Record demo on slot:",
			out->record_slot < 0 ? "None >" : "< %d >", out->record_slot);
		int i_replay = -1;
		if(demo_count > 0 && wad_count > 0)
			i_replay = ui_item(&l, "Replay demo...", NULL);
		int i_ctrl = ui_item(&l, "Controls...", NULL);
		int i_opt = ui_item(&l, "Options...", "%s",
			gclk_name(g_opt.overclock));
		int i_adv = ui_item(&l, "Advanced...", NULL);
		int i_diag = ui_item(&l, "Diagnostics...", NULL);
		int i_quit = ui_item(&l, "Quit", NULL);
		ui_frame_end(&l, footer_main());

		/* Keep the engine options in step before every wait: the idle
		   exit and the quit chord leave from inside ginp_getkey() and
		   save them without passing the end of this function (gint
		   review round 3.1) */
		launcher_commit(out);
		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			break;

		int f = l.focus;
		int d = (k == GK_LEFT) ? -1 : (k == GK_RIGHT) ? 1 : 0;
		int digit = (k >= GK_DIGIT0 && k <= GK_DIGIT0 + 9) ? k - GK_DIGIT0
			: -1;

		if(f < wad_count && k == GK_OK) {
			out->wad = f;
			break;
		}
		else if(f == i_skill && d)
			g_opt.skill = (g_opt.skill + d + 5) % 5;
		/* Episodes 1..4 and maps 1..LAUNCH_MAP_MAX (the engine has no
		   more; a PWAD's MAP33 and up cannot be played). run_game() checks
		   the map is in the WAD. */
		else if(f == i_ep && (d || digit > 0)) {
			out->episode = clampi(digit > 0 ? digit : out->episode + d,
				1, 4);
			out->title = TS_SKIP;
			out->start_set = true;
		}
		else if(f == i_map && (d || digit > 0)) {
			out->map = clampi(digit > 0 ? digit : out->map + d, 1,
				LAUNCH_MAP_MAX);
			out->title = TS_SKIP;
			out->start_set = true;
		}
		else if(f == i_title && d) {
			out->title = clampi(out->title + d, TS_SKIP, TS_FULL);
			out->title_pref = out->title;
			out->start_set = false;
			if(out->title != TS_SKIP)
				out->record_slot = -1;
		}
		else if(f == i_rec && d) {
			out->record_slot = clampi(out->record_slot + d, -1, 9);
			if(out->record_slot < 0)
				out->title = out->start_set ? TS_SKIP : out->title_pref;
		}
		else if(f == i_replay && k == GK_OK) {
			int n = screen_pick("Replay a demo", demos, demo_count, wads,
				wad_count, (gfs_list_more & GFS_MORE_DEMOS) != 0);
			if(n >= 0) {
				/* The WAD the demo was recorded with (engine name
				   "<prefix>D<nn>.LMP"); ask when it is not known
				   rather than replaying against the wrong IWAD */
				int w = gnames_demo_wad(wads, wad_count, demos[n].name);
				if(w < 0 && wad_count == 1)
					w = 0;
				if(w < 0)
					w = screen_pick("Replay with which WAD?", wads,
						wad_count, NULL, 0, false);
				if(w >= 0) {
					out->demo = n;
					out->wad = w;
					break;
				}
			}
		}
		else if(f == i_ctrl && k == GK_OK)
			screen_controls();
		else if(f == i_opt && k == GK_OK)
			screen_options();
		else if(f == i_adv && k == GK_OK)
			screen_advanced();
		else if(f == i_diag && k == GK_OK)
			gdiag_screen(wads, wad_count);
		else if(f == i_quit && k == GK_OK)
			break;
	}

	/* Persisted with the other options (also when quitting) */
	launcher_commit(out);
}

//---
// Developer info screens
//---

static void dev_info_before(char const *wadname)
{
	char text[700];
	gwad_stats_t const *w = &gwad_stats;
	int n = snprintf(text, sizeof text,
		"WAD %s: %d bytes, %s.\n", wadname, w->size, gwad_mode_name());
	if(w->mode == GWAD_RAM)
		n += snprintf(text + n, sizeof text - n,
			"RAM image %d KiB in %d segments, %d sound lumps (%d KiB) "
			"left in storage, loaded in %d ms.\n", w->ram_bytes >> 10,
			w->ram_segments, w->ram_skipped, w->ram_skipped_bytes >> 10,
			w->load_ms);
	if(w->blocks)
		n += snprintf(text + n, sizeof text - n,
			"Blocks %d: mapped %d (P1 %d, P2 %d, P0 %d, other %d, error %d,"
			" unaligned %d), %d runs (longest %d), phys %08x-%08x, %d ms.\n",
			w->blocks, w->mapped, w->raw_p1, w->raw_p2, w->raw_p0,
			w->raw_other, w->raw_error, w->rej_unaligned, w->runs,
			w->longest_run, (unsigned)w->phys_min, (unsigned)w->phys_max,
			w->load_ms);
	n += snprintf(text + n, sizeof text - n,
		"Heap: %u KiB free, largest block %u KiB. Clock: %s. "
		"Display: %s. World switches so far: %d.",
		(unsigned)(gmem_total_free() >> 10),
		(unsigned)(gmem_largest_free() >> 10),
		gclk_name(g_opt.overclock), gvid_mode_name(g_opt.display),
		gfs_world_switches);
	ui_message("Developer info", text, true);
}

static void dev_info_after(void)
{
	char text[700];
	CGDoom_Stats const *s = &CGD_Stats;
	int n = snprintf(text, sizeof text,
		"Lumps loaded %d (%d KiB), unaligned %d (%d KiB), referenced %d "
		"(%d KiB).\n", s->LumpsLoaded, (int)(s->LumpsLoadedTotal >> 10),
		s->UnalignedLumpsLoaded, (int)(s->UnalignedLumpsLoadedTotal >> 10),
		s->LumpsReferenced, (int)(s->LumpsReferencedTotal >> 10));
	n += snprintf(text + n, sizeof text - n,
		"Zone peak %d KiB (static peak %d KiB) in %d zones; malloc "
		"reserve %d KiB.\n", s->ZoneUsedPeak >> 10, s->ZoneStaticPeak >> 10,
		gmem_stats.zone_count, gmem_stats.reserve >> 10);
	n += snprintf(text + n, sizeof text - n,
		"WAD map hits %d, misses %d, BFile fallbacks %d.\n",
		gwad_stats.map_hits, gwad_stats.map_misses,
		gwad_stats.fallback_reads);
	n += snprintf(text + n, sizeof text - n,
		"Stack peak %d bytes of %d. UTLB repairs %d, misses %u. World "
		"switches %d.", g_stack_peak, (int)(g_stack_words * 4),
		gtlb_stats.repairs, (unsigned)*gtlb_stats.misses,
		gfs_world_switches);
	ui_message("Developer info", text, true);
}

//---
// Game
//---

/* Every way out of a game but a crash (gint review round 3.2): the normal
   return, PL_Fatal(), the quit chord and the in-game idle exit.
   - The WAD block map goes first: PL_FileWrite() refuses its no-memory
     fallback while it is live, and its arrays give memory back for the
     copies (gint review round 1). exit_path()'s PL_WadClose() is then a
     no-op.
   - A demo being recorded is finished, as I_Error() and the normal quit
     (d_main.c, after D_DoomLoop) do: G_CheckDemoStatus() alone moves the
     buffer into the engine's delayed saves. Not for a timedemo (it ends in
     I_Error) nor during playback (it would start the next demo). Once:
     it may itself fail into I_Error() -> PL_Fatal() -> here.
     A recording armed by G_RecordDemo() but not begun yet (the first
     level load, where the chord is polled, demo_p is NULL) is dropped by
     G_CheckDemoStatus() itself: the engine's "begun" flag (g_game.c
     demorecbegun) makes it clear demorecording and save nothing, which
     also keeps it from I_Error() if the flush below fails. (gint review
     round 3.3 checked demo_p here, gdemo_begun(); the engine flag covers
     every such case, tests/demo-unbegun.sh.)
     The launcher makes recording imply a direct start without title demos
     (launcher()), so demobuffer is never replaced by a played demo lump.
   - The delayed saves (savegames, the demo) go to the file layer.
   - The settings are queued: the engine changes view size, low detail and
     flat floors in game (r_main.c CGD_Set*) and never saves them itself. */
static void end_game_session(void)
{
	static bool demo_done = false;
	g_game_running = false;
	PL_WadClose();
	if(demorecording && !demoplayback && !timingdemo && !demo_done) {
		demo_done = true;
		G_CheckDemoStatus();
	}
	CGD_FlushDelayedWrites();
	/* The user's title-screen setting, not this launch's (final review) */
	if(g_title_saved) {
		CGD_Options.AutoStart = g_title_autostart;
		CGD_Options.EnableDemos = g_title_demos;
		g_title_saved = false;
	}
	save_settings_if_changed();
}

static void game_entry(void)
{
	D_DoomMain();
}

/* Returns false if the game could not be started (back to the launcher) */
static bool run_game(gfs_entry_t *wads, gfs_entry_t *demos, launch_t *L)
{
	static char wad_base[GFS_LIST_NAME];
	static char demo_name[GFS_LIST_NAME];
	char const *wadname = wads[L->wad].name;

	save_settings_if_changed();
	gfs_drop_read_cache();

	/* Recording over an existing demo of this WAD and slot (final review:
	   it was replaced at exit without a word). Needs only the WAD's name:
	   checked before the (slow) WAD load. */
	gnames_wad_base(wad_base, sizeof wad_base, wadname);
	CGD_WADFileName = wad_base;
	if(L->demo < 0 && L->record_slot >= 0) {
		char demo[CGD_NAME_MAX];
		CGD_DemoFileName(demo, L->record_slot);
		int old = PL_FileSize(demo);
		if(old >= 0) {
			char t[160], sz[16];
			snprintf(t, sizeof t, "Slot %d of %s already holds a demo "
				"(%s, %s). Record over it?", L->record_slot, wadname, demo,
				size_text(sz, sizeof sz, old));
			if(!ui_confirm("Record demo", t))
				return false;
		}
	}

	ui_status("Loading", wadname, NULL);
	int wrc = PL_WadOpen(wadname);
	if(wrc < 0) {
		/* Final review: one message per cause (gwad_error_text) */
		char t[220];
		snprintf(t, sizeof t, "Cannot open %s. %s", wadname,
			gwad_error_text(wrc));
		ui_message("Error", t, true);
		return false;
	}

	/* A direct start at a map the WAD does not have: back to the launcher
	   (final review: Doom II MAP33+ ended in the fatal error screen and an
	   exit to MPM; other WADs were clamped silently by G_InitNew) */
	if(L->demo < 0 && L->title == TS_SKIP) {
		char lump[12];
		bool commercial = gwad_has_lump("MAP01");
		if(commercial)
			snprintf(lump, sizeof lump, "MAP%02d", L->map);
		else
			snprintf(lump, sizeof lump, "E%dM%d", L->episode, L->map);
		if(!gwad_has_lump(lump)) {
			PL_WadClose();
			char t[220];
			snprintf(t, sizeof t, "%s has no map %s. Change 'Start at %s' "
				"(%s).", wadname, lump, commercial ? "map" : "episode/map",
				commercial ? "Doom II style WAD: MAP01-MAP32, the episode "
				"is not used" : "maps 1-9 of the episodes this WAD has");
			ui_message("Start at map", t, true);
			return false;
		}
	}
	if(CGD_Options.DeveloperInfo)
		dev_info_before(wadname);

	/* Engine parameters (only once the game is sure to start).
	   CGD_WADFileName was set above, before the record-slot check: the
	   whole base name, from which the engine derives an 8.3-safe save/demo
	   prefix (CGD_FilePrefix). Truncating it made WADs with a common start
	   share their saves (gint review round 1). A copy, since wads[] may be
	   rescanned. */

	CGD_SingleEpisodeUltimate = 0;
	for(int e = 1; e <= 4; e++) {
		char n[12];
		snprintf(n, sizeof n, "doomu%d.wad", e);
		if(!strnicmp(wadname, n, 11))
			CGD_SingleEpisodeUltimate = e;
	}

	startepisode = L->episode;
	startmap = L->map;
	skill = g_opt.skill;
	/* Recording: from a direct start only, never with a replay (whose
	   G_DoPlayDemo() replaces demobuffer); see launcher_normalize() */
	CGD_RecordDemoSlot = (L->demo >= 0) ? -1 : L->record_slot;
	/* This launch's title choice; end_game_session() puts the saved
	   setting back before the settings are queued (final review) */
	g_title_autostart = CGD_Options.AutoStart;
	g_title_demos = CGD_Options.EnableDemos;
	g_title_saved = true;
	CGD_Options.AutoStart = (L->title == TS_SKIP);
	CGD_Options.EnableDemos = (L->title == TS_FULL);
	autostart = CGD_Options.AutoStart || CGD_RecordDemoSlot >= 0;
	CGD_PlayDemoOnly = 0;
	if(L->demo >= 0) {
		strcpy(demo_name, demos[L->demo].name);
		G_DeferedPlayDemoFile(demo_name);
		CGD_PlayDemoOnly = 1;
	}

	/* Auto: at most CGD_AUTOFRAMESKIP_DEFAULT skipped frames in a row,
	   which is also the catch-up limit (cgdoom.h). It was 1, which slowed
	   the game down as soon as a frame took more than 2 tics (gint review
	   round 3.3). CGD_Frameskip is then unused (the in-game frameskip key
	   leaves auto for fixed 0). */
	CGD_Frameskip = (g_opt.frameskip < 0) ? 1 : g_opt.frameskip;
	CGD_AutoFrameskip = (g_opt.frameskip < 0) ? CGD_AUTOFRAMESKIP_DEFAULT : 0;
	fpscounteractive = g_opt.fps_counter ? true : false;
	gkm_apply_engine_hooks();

	/* Safe start: the OS clock for this session (the setting is kept) */
	gclk_apply(g_safe_start ? GCLK_OFF : g_opt.overclock);
	gvid_set_mode(g_opt.display);
	ui_status("CGDoom", "Starting...", NULL);
	ginp_wait_release();
	gvid_clear_lcd();

	gtime_idle_reset();
	g_game_running = true;
	if(g_stack) {
		for(size_t w = 0; w < g_stack_words; w++)
			g_stack[w] = STACK_FILL;
		cgd_call_on_stack(game_entry, g_stack + g_stack_words);
		size_t w = 0;
		while(w < g_stack_words && g_stack[w] == STACK_FILL)
			w++;
		g_stack_peak = (int)((g_stack_words - w) * 4);
	}
	else
		game_entry();
	g_game_running = false;

	/* Back from the game: unmap, then hand the engine's delayed saves and
	   the in-game option changes to the file layer (written by the exit
	   path in one world switch) */
	gvid_release();
	end_game_session();
	if(CGD_Options.DeveloperInfo) {
		/* The key that quit the game may still be down: it would be read
		   as a new press and dismiss the message at once */
		ginp_wait_release();
		dev_info_after();
	}
	return true;
}

//---
// Exit path and fatal errors
//---

static void exit_path(void)
{
	g_in_exit = true;
	/* No abort() from the quit chord while the files are written (gint
	   review round 3.1); every wait below is bounded */
	gtime_disarm_abort();
	g_game_running = false;
	ginp_idle_hook = NULL;
	ginp_wait_release();
	gvid_release();

	PL_WadClose();
	int pending = gfs_pending_count();
	int failures = 0;
	if(pending > 0) {
		char line[48];
		snprintf(line, sizeof line, "Writing %d file%s...", pending,
			pending > 1 ? "s" : "");
		ui_status("Saving", line, "Do not turn off the calculator");
		failures = gfs_flush();
	}
	/* The log is a RAM ring buffer, gone once we exit: say what failed
	   right here (gint review round 1). Every failure of the session
	   counts, including the immediate writes of PL_FileWrite()'s
	   no-memory path, which leave nothing pending (gint review round 2;
	   nothing reports failures before this point). */
	char text[256];
	if(gfs_failure_text(text, sizeof text, failures)) {
		/* Bounded: the idle hook is off here (gint review round 2) */
		ui_message_timeout("Saving", text, GINP_IDLE_MS);
	}

	ginp_wait_release();
	gtime_quit();
	gvid_release();
	/* gint's kquit() restores the OS clock, LCD window and drivers */
}

void PL_Fatal(const char *msg)
{
	if(g_in_fatal) {
		/* Error while handling an error: leave right away */
		if(g_exit_ready)
			longjmp(g_exit_jmp, 2);
		exit(1);
	}
	g_in_fatal = true;
	ginp_idle_hook = NULL;
	gtime_disarm_abort();
	PL_Log("Fatal: %s", msg ? msg : "?");

	gvid_release();
	ui_begin();
	drect(0, 0, DWIDTH - 1, 17, UI_TITLE);
	ui_text(DWIDTH / 2, 4, UI_BG, DTEXT_CENTER, "CGDoom error");
	int y = ui_wrap(12, 28, DWIDTH - 24, UI_FG, msg ? msg : "(no message)");
	char const *hint = gfatal_hint(msg);
	if(hint)
		ui_wrap(12, y + 8, DWIDTH - 24, UI_DIM, hint);
	drect(0, DHEIGHT - 14, DWIDTH - 1, DHEIGHT - 1, UI_HEAD);
	ui_text(DWIDTH / 2, DHEIGHT - 12, UI_BG, DTEXT_CENTER,
		"Press any key to exit");
	ui_end();
	/* Bounded (gint review round 2): the idle hook is off here and the
	   add-in owns the interrupts (no OS auto power-off). After
	   GINP_IDLE_MS we leave as if a key had been pressed. */
	ginp_wait_release();
	if(ginp_wait_any_key(GINP_IDLE_MS))
		ginp_wait_release();

	/* Keep the saves made during this session (best effort; I_Error()
	   has already finished a demo being recorded). A failure in there
	   re-enters PL_Fatal(), which leaves at once (g_in_fatal). */
	if(g_game_running)
		end_game_session();

	if(g_exit_ready)
		longjmp(g_exit_jmp, 1);
	exit(1);
}

//---
// Idle timeout
//---

/* Called by ginp_getkey() after GINP_IDLE_MS without a key in a launcher
   screen. The add-in owns the interrupts, so the OS's auto power-off cannot
   run (critique §C7); no PowerOff syscall exists for the fx-CG100 OS in
   gint, and the backlight registers of r61525_brightness_set() are not
   known to be valid on the fx-CG100's new LCD controller, so instead of
   dimming we return to MPM, where the OS's own auto power-off works. */
static void idle_quit(void)
{
	if(g_game_running || g_in_fatal || !g_exit_ready)
		return;
	ui_status("Idle", "No key pressed for 10 minutes.",
		"Quitting to MPM in 60 s. Press any key to stay.");
	/* Edge-triggered: a pinned key is not "stay" (gint review round 3.1) */
	if(ginp_wait_any_key(60 * 1000)) {
		ginp_wait_release();
		return;
	}
	save_settings_if_changed();
	longjmp(g_exit_jmp, 3);
}

//---
// Quit chord and in-game idle exit (gint review round 3.1)
//---

/* Leave like PL_Fatal does after its message: in a game, end_game_session()
   (WAD map dropped, a demo being recorded finished, the delayed saves
   handed over); the settings are queued either way, and exit_path() writes
   everything. */
static void leave_to_exit_path(int code) __attribute__((noreturn));
static void leave_to_exit_path(int code)
{
	if(g_game_running)
		end_game_session();
	else
		save_settings_if_changed();
	longjmp(g_exit_jmp, code);
}

/* The engine never calls ginp_getkey(), so idle_quit() cannot run in a
   game, and the add-in owns the interrupts (no OS auto power-off). The tic
   interrupt records keyboard changes (time.c); a game left alone runs this
   from PL_GetTics, i.e. in the foreground (never from the interrupt). The
   engine keeps calling PL_GetTics while it is paused, in the menu, in the
   save-name entry, on the automap, in intermissions and in title demos, so
   all of these are covered.
   The limit depends on what a quit would lose (integration follow-up: the
   engine's CGD_IsPaused() mask and usergame, gtime_game_idle_limit_ms()):
   - demos (title loop, replays), title pages, no level outside a user game:
     nothing to lose, GINP_IDLE_MS;
   - the player's own game, also paused or behind the menu: unsaved
     progress, GINP_GAME_IDLE_MS, and the warning says so.
   Quitting goes through leave_to_exit_path(): the explicit saves, a demo
   being recorded and the settings (including in-game view size / detail /
   flat floors changes) are queued and exit_path() writes them, as with a
   normal quit. A game cannot be saved behind the player's back (no slot is
   ours), so a long-paused game offers to quit rather than saving. */
_Static_assert(GIDLE_PAUSED == CGD_IDLE_PAUSED && GIDLE_MENU == CGD_IDLE_MENU
	&& GIDLE_NOLEVEL == CGD_IDLE_NOLEVEL && GIDLE_DEMO == CGD_IDLE_DEMO,
	"time.c's idle policy bits are the engine's CGD_IDLE_* mask");

static bool g_poll_busy = false;

static void game_idle_check(void)
{
	/* Cheap first: no engine state read until the shorter limit */
	int idle = gtime_idle_tics();
	if(idle < GINP_IDLE_MS / 1000 * 35)
		return;
	bool lose_progress;
	int limit_ms = gtime_game_idle_limit_ms(CGD_IsPaused(), usergame,
		gamestate == GS_DEMOSCREEN, &lose_progress);
	if(idle < limit_ms / 1000 * 35)
		return;

	char line[64];
	snprintf(line, sizeof line, "No key pressed for %d minutes.%s",
		limit_ms / 60000, lose_progress ? " Game not saved." : "");
	ui_status(lose_progress ? "Idle: unsaved game" : "Idle", line,
		"Quitting to MPM in 60 s. Press any key to stay.");
	if(!ginp_wait_any_key(60 * 1000))
		leave_to_exit_path(3);
	gtime_idle_reset();
	/* The next frame repaints the game area and the borders */
	PL_VideoReset();
	/* The game never sees that key. Polls again: the key may be the quit
	   chord, still held (the abort() fallback must not fire here) */
	g_poll_busy = false;
	ginp_wait_release();
}

void gquit_poll(void)
{
	if(g_poll_busy || g_in_exit || g_in_fatal || !g_exit_ready)
		return;
	g_poll_busy = true;
	/* leave_to_exit_path() does not return; g_poll_busy then stays set,
	   and exit_path() sets g_in_exit anyway */
	if(gtime_take_quit()) {
		PL_Log("Quit chord\n");
		leave_to_exit_path(4);
	}
	if(g_game_running)
		game_idle_check();
	g_poll_busy = false;
}

/* From wad.c, on entry of every engine WAD read or map (gint review round
   3.2). Only in a game: PL_WadOpen() (before the game) does not come here,
   and the launcher has its own poll points. No idle exit (no drawing in
   the middle of a frame or a load); PL_GetTics runs it soon enough. */
void gquit_poll_load(void)
{
	if(!g_game_running || g_poll_busy || g_in_exit || g_in_fatal
		|| !g_exit_ready)
		return;
	if(gtime_take_quit()) {
		g_poll_busy = true;
		PL_Log("Quit chord (loading)\n");
		leave_to_exit_path(4);
	}
}

//---
// Destructor: every exit path, including abort() (gint review round 3.1)
//---

/* gint's start2 runs .dtors before kquit after exit() and after abort()
   (the quit chord's hang fallback, EXIT on gint's panic screen), which
   skip exit_path(). Order matters: closing the WAD is a world switch,
   which needs the UTLB protection that gtlb_quit() then removes. */
__attribute__((destructor))
static void platform_quit(void)
{
	gwad_quit();
	gtlb_quit();
}

//---
// Entry point
//---

/* Another model or OS (the g3a also starts on the fx-CG50, from its main
   MENU): say so and leave. Nothing else is set up; the timers only bound
   the wait, and the destructor finds nothing to undo. */
static int unsupported(void)
{
	gtime_init();
	ui_init();
	char t[300];
	snprintf(t, sizeof t, "CGDoom only runs on the fx-CG100 / Graph Math+ "
		"under MPM, with OS 02.00.0202 or 02.00.2202 (the versions MPM can "
		"be installed on). This calculator: %s, OS %s.", gsys_model_name(),
		g_os_version);
	ui_message_timeout("Unsupported calculator", t, GINP_IDLE_MS);
	gtime_quit();
	return 1;
}

int main(void)
{
	uint32_t r15;
	__asm__ volatile("mov r15, %0" : "=r"(r15));
	g_entry_r15 = r15;

	/* No OS call, UTLB or heap change before the model and the OS string
	   have been checked */
	gsys_init();
	if(!gsys_supported())
		return unsupported();
	/* Safe start (final review): DEL held at launch. A raw KEYSC read,
	   before the UTLB is touched. */
	g_safe_start = ginp_safe_start();
	gmem_init();
	gtlb_init(g_safe_start);
	gclk_init();
	gtime_init();
	ui_init();

	/* Keep on-chip memory intact across world switches */
	void *onchip = malloc(GINT_ONCHIP_BUFSIZE);
	if(onchip)
		gint_set_onchip_save_mode(GINT_ONCHIP_BACKUP, onchip);
	/* Without the RGB565 frame, the DMA display path falls back to CPU
	   writes; gvid_mode_name() and the benchmark page say so (gint review
	   round 2) */
	if(!gvid_init())
		PL_Log("DMA frame buffer unavailable: DMA display uses CPU\n");
	if(CGD_PRIVATE_STACK_SIZE > 0 && !g_safe_start) {
		g_stack = malloc(CGD_PRIVATE_STACK_SIZE);
		if(g_stack)
			g_stack_words = CGD_PRIVATE_STACK_SIZE / 4;
	}

	if(setjmp(g_exit_jmp) == 0) {
		g_exit_ready = true;
		/* Idle timeout on every screen from here on, including the
		   startup error screens (gint review round 2). idle_quit() saves
		   nothing before load_settings(), and exit_path() then has
		   nothing to write. */
		ginp_idle_hook = idle_quit;

		if(g_safe_start) {
			ginp_wait_release();
			ui_message("Safe start", "DEL was held: this session runs "
				"without UTLB pinning, overclock and private stack. If "
				"CGDoom only starts this way, please report it with the "
				"Diagnostics pages.", true);
		}

		if(gmem_total_free() < CGD_MIN_TOTAL_HEAP) {
			char t[300];
			snprintf(t, sizeof t, "Only %u KiB of RAM are available. "
				"Install the mpm.bin 1.1 of the CGDoom release zip and "
				"try again.", (unsigned)(gmem_total_free() >> 10));
			ui_message("Not enough memory", t, true);
		}
		else {
			load_settings();

			static gfs_entry_t wads[16], demos[24];
			int nw = 0, nd = 0;
			ui_status("CGDoom", "Looking for WAD files...", NULL);
			gfs_list(wads, 16, &nw, demos, 24, &nd);

			static launch_t L = { .episode = 1, .map = 1,
				.record_slot = -1 };
			while(1) {
				launcher(wads, nw, demos, nd, &L);
				if(L.wad < 0) {
					save_settings_if_changed();
					break;
				}
				/* The engine is not re-entrant: once a game has run, we
				   leave (like CGDoom) */
				if(run_game(wads, demos, &L))
					break;
			}
		}
	}

	exit_path();
	return 0;
}
