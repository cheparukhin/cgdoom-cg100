/* CGDoom gint platform: time (PL_GetTics / PL_GetMicros / PL_WaitTics).

   - Tics: a TMU timer (precise, ~0.0015% error) fires every 28571 us and
     runs tic_callback(): counter increment, PL_WaitTics wake-up, quit
     chord, key activity. gint rescales running timers on clock changes and
     saves/restores them across world switches (the counter simply pauses
     while the OS runs, as does the game).
   - Microseconds: gint's profiler TMU (prof_init: Pphi/4, TCOR=0xffffffff,
     down-counting) read through prof_tcnt. Deltas are converted with a
     32.32 multiplier recomputed whenever Pphi changes and accumulated in
     64 bits; PL_GetMicros returns the low 32 bits (wraps at 2^32 us), the
     timerless PL_GetTics fallback uses all of them.
   - PL_WaitTics sleeps until the tic counter reaches the target, with an
     interrupt-cancellable sleep: a plain
     `while(tics < until) sleep();` loses the wake-up when the tic lands
     between the test and the sleep instruction, and the frame then waits
     for the next interrupt (up to ~8 ms, the 128 Hz keyboard scan).
   - Emergency quit: gint's SHIFT+7+3+AC/ON
     chord cannot fire in CGDoom (input.c), so the tic interrupt checks
     the same chord on a raw scan. It only latches a request: the
     foreground takes it at its next poll (gquit_poll(), main.c, from
     PL_GetTics and the UI key waits; gquit_poll_load() from every WAD
     read/map), closes the WAD, hands the engine's delayed saves and the
     settings to the file layer and takes the normal exit path, which
     writes them. Only if nothing
     takes the request while the chord stays held for GTIME_ABORT_HOLD_TICS
     does the interrupt call abort(), once: gint's exit (main.c's
     destructor: WAD file close, UTLB normalisation; then kquit) without
     the saves. That means a hang, or a phase that reads no WAD data and
     never asks for the time for 2 s of gint time (OS calls do not count:
     the tic timer stops during world switches); the slow startup and
     level loads read the WAD all along. Taking the request, PL_Fatal and
     the exit path disarm that fallback, so it never interrupts the exit's
     file writes.
   - Key activity: the same scan records the tic of the last
     change of the keyboard state, for main.c's in-game idle exit. A change,
     not "a key is down", so that a pinned or stuck key does not keep the
     calculator on forever. */

#include "cgd_gint.h"

#include <gint/timer.h>
#include <gint/prof.h>
#include <gint/clock.h>
#include <gint/cpu.h>
#include <stdlib.h>

static volatile int g_tics = 0;
static int g_tic_timer = -1;
static bool g_prof_ok = false;
/* Sleep of the running PL_WaitTics (on its stack), cancelled by the tic.
   The tic clears it: after a chord abort the foreground never
   returns to clear it itself, and later tics must not write into that dead
   stack frame. */
static cpu_csleep_t * volatile g_tic_ics = NULL;

/* Emergency quit */
static volatile bool g_quit_req = false;    /* chord seen, not yet taken */
static volatile bool g_abort_armed = true;  /* hang fallback allowed */
static int g_chord_tics = 0;                /* tics in a row with the chord */
/* Key activity */
static uint8_t g_last_scan[12];
static volatile int g_key_tic = 0;          /* tic of the last scan change */

/* Interrupt context (timer callback) */
static int tic_callback(void)
{
	int t = g_tics + 1;
	g_tics = t;
	cpu_csleep_t *ics = g_tic_ics;
	if(ics) {
		g_tic_ics = NULL;
		cpu_csleep_cancel(ics);
	}

	uint8_t st[12];
	ginp_scan(st);
	bool changed = false;
	for(int i = 0; i < 12; i++) {
		changed |= (st[i] != g_last_scan[i]);
		g_last_scan[i] = st[i];
	}
	if(changed)
		g_key_tic = t;

	if(!ginp_abort_chord(st)) {
		g_chord_tics = 0;
		return TIMER_CONTINUE;
	}
	g_quit_req = true;
	if(g_chord_tics < GTIME_ABORT_HOLD_TICS)
		g_chord_tics++;
	if(g_chord_tics >= GTIME_ABORT_HOLD_TICS && g_abort_armed) {
		/* Once: tics keep coming until kquit, the chord may still be
		   held, and .dtors must not be restarted */
		g_abort_armed = false;
		abort();
	}
	return TIMER_CONTINUE;
}

bool gtime_take_quit(void)
{
	if(!g_quit_req)
		return false;
	g_abort_armed = false;
	g_quit_req = false;
	return true;
}

void gtime_disarm_abort(void)
{
	g_abort_armed = false;
}

int gtime_idle_tics(void)
{
	if(g_tic_timer < 0)
		return 0;
	return g_tics - g_key_tic;
}

void gtime_idle_reset(void)
{
	g_key_tic = g_tics;
}

/* In-game idle policy (cgd_gint.h). Foreground only (main.c calls it from
   gquit_poll, i.e. from PL_GetTics; never from the tic interrupt). */
int gtime_game_idle_limit_ms(int paused, bool usergame, bool title,
	bool *lose_progress)
{
	bool nothing_to_lose = (paused & GIDLE_DEMO) || title
		|| ((paused & GIDLE_NOLEVEL) && !usergame);
	if(lose_progress)
		*lose_progress = !nothing_to_lose;
	return nothing_to_lose ? GINP_IDLE_MS : GINP_GAME_IDLE_MS;
}

bool gtime_ticking(void)
{
	return g_tic_timer >= 0;
}

int gtime_tic_timer(void)
{
	return g_tic_timer;
}

bool gtime_prof_ok(void)
{
	return g_prof_ok;
}

/* Microseconds since gtime_init, split so that the integer part does not
   wrap (a 32.32 accumulator would wrap after 2^32 us = 71.6 min, and the
   fallback tic count derived from it would jump back by ~150k tics) */
static uint64_t g_us_total = 0;     /* whole microseconds */
static uint32_t g_us_frac = 0;      /* fraction, 0.32 fixed point */
static uint32_t g_us_last = 0;      /* last prof_tcnt value */
static uint32_t g_us_mul = 0;       /* 2^32 * 1e6 / (Pphi/4) */
static int g_us_pphi = 0;

static void recompute_mul(void)
{
	int pphi = clock_freq()->Pphi_f;
	if(pphi <= 0)
		pphi = 29491200;
	g_us_pphi = pphi;
	/* tick rate = Pphi/4 -> us per tick = 4e6 / Pphi */
	g_us_mul = (uint32_t)((4000000ull << 32) / (uint64_t)pphi);
}

void gtime_init(void)
{
	/* The profiler first: it wants TMU2 (lowest TMU interrupt priority) */
	g_prof_ok = (prof_init() == 0) && prof_tcnt != NULL;
	if(g_prof_ok) {
		recompute_mul();
		g_us_last = *prof_tcnt;
	}

	int t = timer_configure(TIMER_TMU, 1000000 / 35,
		GINT_CALL(tic_callback));
	if(t < 0)
		t = timer_configure(TIMER_ANY, 1000000 / 35,
			GINT_CALL(tic_callback));
	if(t >= 0)
		timer_start(t);
	g_tic_timer = t;
}

void gtime_quit(void)
{
	if(g_tic_timer >= 0)
		timer_stop(g_tic_timer);
	g_tic_timer = -1;
	if(g_prof_ok)
		prof_quit();
	g_prof_ok = false;
}

void gtime_resync(void)
{
	if(g_prof_ok) {
		recompute_mul();
		g_us_last = *prof_tcnt;
	}
}

uint32_t PL_GetMicros(void)
{
	if(!g_prof_ok)
		return (uint32_t)g_tics * 28571u;

	uint32_t now = *prof_tcnt;
	uint32_t delta = g_us_last - now;     /* down-counter */
	g_us_last = now;
	if(clock_freq()->Pphi_f != g_us_pphi)
		recompute_mul();
	/* < 2^64: delta, g_us_mul < 2^32 (Pphi > 4 MHz), frac < 2^32 */
	uint64_t t = (uint64_t)delta * g_us_mul + g_us_frac;
	g_us_total += t >> 32;
	g_us_frac = (uint32_t)t;
	return (uint32_t)g_us_total;
}

/* Engine time; also the engine's safe point for the chord request and the
   in-game idle exit (main.c, gquit_poll) */
static int get_tics(void)
{
	if(g_tic_timer >= 0)
		return g_tics;
	/* No timer available: derive from the microsecond counter (monotonic,
	   no wrap in practice) */
	PL_GetMicros();
	return (int)(g_us_total * 35 / 1000000);
}

int PL_GetTics(void)
{
	gquit_poll();
	return get_tics();
}

void PL_WaitTics(int until)
{
	if(g_tic_timer < 0) {
		/* No clock at all: run unpaced rather than wait forever */
		if(!g_prof_ok)
			return;
		while(PL_GetTics() < until) {}
		return;
	}
	while(PL_GetTics() < until) {
		/* gint's pattern (dma.c dma_channel_wait): publish the ICS, then
		   test; a tic after the test turns the sleep into a nop. The
		   buffer holds code: it must be on the stack, not in .data. */
		cpu_csleep_t ics;
		cpu_csleep_init(&ics);
		g_tic_ics = &ics;
		if(get_tics() < until)
			cpu_csleep(&ics);
		g_tic_ics = NULL;
	}
}
