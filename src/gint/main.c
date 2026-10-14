/* CGDoom gint platform: entry point, launcher, game start, exit path.

   Startup (before any OS call):
     record r15 and check the model and OS string (sys.c): anything but the
     fx-CG100 with OS 02.00/02.20 -> message, exit, with nothing else set up.
     Then remove the OS heap arena (memory.c), snapshot the UTLB and install
     the TLB-miss handler (tlb.c), start the timers, allocate the frame
     buffer and the private stack. Too little heap -> message, exit.
   Launcher (port of CGDoom's cgdoom-ui.c UI_Main to gint on 396x224): the
     WAD list, the controls editor and diagnostics. Everything else is fixed
     (2026-10-14, notes/design.md §11): the F4 overclock, the async DMA
     display path, automatic frameskip, the WAD copied to RAM when it fits,
     and vanilla's title screen with its demos; skill, episode, view size and
     detail are chosen in the game.
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

//---
// Settings: the engine options in CGDOOM.CFG (keymap, view size, detail)
//---

static CGDoom_Options g_saved_engine_opts;
/* Set by load_settings(): before that, g_saved_engine_opts is not a
   baseline and saving would write default settings over the user's file
   (the idle hook runs from the startup error screens since gint review
   round 2) */
static bool g_settings_loaded = false;

static void load_settings(void)
{
	CGD_DefaultOptions(&CGD_Options);
	int n;
	gkm_preset_t const *p = gkm_presets(&n);
	CGD_CopyKeymap(CGD_Options.Keymap, p[0].map);
	CGD_LoadSettings(&CGD_Options);

	memcpy(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options);
	g_settings_loaded = true;
}

/* Queue the settings file if anything changed (written at exit) */
static void save_settings_if_changed(void)
{
	if(!g_settings_loaded)
		return;
	if(!memcmp(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options))
		return;

	int failures = gfs_flush_failures();
	int rc = CGD_SaveSettings(&CGD_Options);
	/* Not queued (external review: CGD_SaveSettings() without memory for
	   its buffer, or PL_FileWrite() refusing): shown at exit, unless the
	   file layer counted it already, and the baseline stays, so that a
	   later call tries again */
	if(rc < 0) {
		if(gfs_flush_failures() == failures)
			gfs_note_failure(CGDOOM_SETTINGS_FILE, GFS_ERR_NO_MEMORY);
		return;
	}

	memcpy(&g_saved_engine_opts, &CGD_Options, sizeof CGD_Options);
}

//---
// Launcher screens
//---

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

/* Returns the index of the WAD to play, or -1 to quit */
static int launcher(gfs_entry_t const *wads, int wad_count)
{
	ui_layout_t l;
	ui_layout_init(&l);

	while(1) {
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
		if(gfs_list_more)
			ui_note(&l, UI_WARN, "More WAD files exist: only %d listed.",
				wad_count);
		ui_space(&l, 4);

		int i_ctrl = ui_item(&l, "Controls...", NULL);
		int i_diag = ui_item(&l, "Diagnostics...", NULL);
		int i_quit = ui_item(&l, "Quit", NULL);
		ui_frame_end(&l, "OK/EXE: select   BACK: quit");

		int k = ginp_getkey();
		if(ui_nav(&l, k))
			continue;
		if(k == GK_BACK || k == GK_QUIT)
			return -1;
		if(k != GK_OK)
			continue;

		int f = l.focus;
		if(f < wad_count)
			return f;
		else if(f == i_ctrl)
			screen_controls();
		else if(f == i_diag)
			gdiag_screen(wads, wad_count);
		else if(f == i_quit)
			return -1;
	}
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
   - The settings are queued: the engine changes view size and low detail
     in game (r_main.c CGD_Set*) and never saves them itself. Before the
     saves (external review): their copies may take most of the heap, and
     the settings copy then failed.
   - The delayed saves (savegames) go to the file layer (a save that cannot
     be copied is written at once, with everything pending).
   No demo is ever recorded here: CGD_RecordDemoSlot stays -1 (the
   launcher's recording was removed on 2026-10-14). */
static void end_game_session(void)
{
	g_game_running = false;
	PL_WadClose();
	save_settings_if_changed();
	CGD_FlushDelayedWrites();
}

static void game_entry(void)
{
	D_DoomMain();
}

/* Returns false if the game could not be started (back to the launcher) */
static bool run_game(gfs_entry_t const *wads, int wad)
{
	static char wad_base[GFS_LIST_NAME];
	char const *wadname = wads[wad].name;

	save_settings_if_changed();
	gfs_drop_read_cache();

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

	/* Engine parameters (only once the game is sure to start).
	   CGD_WADFileName: the whole base name, from which the engine derives
	   an 8.3-safe save prefix (CGD_FilePrefix). Truncating it made WADs
	   with a common start share their saves (gint review round 1). A copy,
	   since wads[] may be rescanned. The engine's own defaults start the
	   title screen with its demos (no autostart); the Doom menu asks for
	   the episode and skill. */
	gnames_wad_base(wad_base, sizeof wad_base, wadname);
	CGD_WADFileName = wad_base;

	CGD_SingleEpisodeUltimate = 0;
	for(int e = 1; e <= 4; e++) {
		char n[12];
		snprintf(n, sizeof n, "doomu%d.wad", e);
		if(!strnicmp(wadname, n, 11))
			CGD_SingleEpisodeUltimate = e;
	}

	/* Automatic frameskip: at most CGD_AUTOFRAMESKIP_DEFAULT skipped
	   frames in a row, which is also the catch-up limit (cgdoom.h). It was
	   1, which slowed the game down as soon as a frame took more than 2
	   tics (gint review round 3.3). */
	CGD_AutoFrameskip = CGD_AUTOFRAMESKIP_DEFAULT;
	gkm_apply_engine_hooks();

	/* Safe start: the OS clock for this session */
	gclk_apply(g_safe_start ? GCLK_OFF : GCLK_F4);
	ui_status("CGDoom", "Starting...", NULL);
	ginp_wait_release();
	gvid_clear_lcd();

	gtime_idle_reset();
	g_game_running = true;
	if(g_stack)
		cgd_call_on_stack(game_entry, g_stack + g_stack_words);
	else
		game_entry();
	g_game_running = false;

	/* Back from the game: unmap, then hand the engine's delayed saves and
	   the in-game option changes to the file layer (written by the exit
	   path in one world switch) */
	gvid_release();
	end_game_session();
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

	/* Keep the saves made during this session (best effort). A failure
	   in there re-enters PL_Fatal(), which leaves at once (g_in_fatal). */
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
   (WAD map dropped, the delayed saves handed over); the settings are queued
   either way, and exit_path() writes everything. */
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
   Quitting goes through leave_to_exit_path(): the explicit saves and the
   settings (including in-game view size / detail changes) are queued and
   exit_path() writes them, as with a normal quit. A game cannot be saved behind the player's back (no slot is
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
		"under MPM, with OS 02.00.0202, 02.00.2202 or 02.20.0202. This "
		"calculator: %s, OS %s.", gsys_model_name(),
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
				"Install the mpm.bin for your OS (1.1 on OS 02.00) "
				"and try again.",
				(unsigned)(gmem_total_free() >> 10));
			ui_message("Not enough memory", t, true);
		}
		else {
			load_settings();

			static gfs_entry_t wads[16];
			int nw = 0;
			ui_status("CGDoom", "Looking for WAD files...", NULL);
			gfs_list(wads, 16, &nw);

			while(1) {
				int w = launcher(wads, nw);
				if(w < 0) {
					save_settings_if_changed();
					break;
				}
				/* The engine is not re-entrant: once a game has run, we
				   leave (like CGDoom) */
				if(run_game(wads, w))
					break;
			}
		}
	}

	exit_path();
	return 0;
}
