/* Host unit test for src/gint/time.c (gint review round 3.1), #included
   with gint's timer, profiler, clock and interrupt-cancellable sleep
   stubbed; input.c is linked for the raw scan and the chord test.

   - The quit chord only latches a request in the tic interrupt; the
     foreground takes it once. abort() is the hang fallback: only after
     GTIME_ABORT_HOLD_TICS tics of chord without the request being taken,
     only once, never after gtime_take_quit() or gtime_disarm_abort().
   - The tic clears the published PL_WaitTics sleep (no dangling pointer
     after a chord abort longjmps out of PL_WaitTics).
   - Key activity: tics since the keyboard state last changed.
   - The timerless PL_GetTics fallback stays monotonic past 2^32 us. */

#include <stdlib.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int abort_calls = 0;
static jmp_buf *abort_jmp = NULL;
static void test_abort(void)
{
	abort_calls++;
	if(abort_jmp)
		longjmp(*abort_jmp, 1);
}
#define abort test_abort
#include "time.c"
#undef abort

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

/* ---- stubs ---- */

volatile uint16_t cgd_host_keysc[6];
void sleep(void) {}

static long polls = 0;
void gquit_poll(void) { polls++; }

static int timer_result = 0;
static gint_call_t timer_cb;
int timer_configure(int timer, uint64_t delay_us, gint_call_t cb)
{
	(void)timer; (void)delay_us;
	timer_cb = cb;
	return timer_result;
}
void timer_start(int timer) { (void)timer; }
void timer_stop(int timer) { (void)timer; }

static uint32_t volatile tcnt = 0xffffffffu;
uint32_t volatile *prof_tcnt = &tcnt;
static int prof_result = 0;
int prof_init(void) { return prof_result; }
void prof_quit(void) {}
static clock_frequency_t freq = { .Pphi_f = 29491200 };
const clock_frequency_t *clock_freq(void) { return &freq; }

/* cpu_csleep: the "interrupt" (one tic) arrives during the sleep */
static int cancels = 0;
static cpu_csleep_t *last_cancel = NULL;
static cpu_csleep_t *slept = NULL;
void cpu_csleep_init(cpu_csleep_t *ics) { memset(ics, 0, sizeof *ics); }
void cpu_csleep_cancel(cpu_csleep_t *ics) { cancels++; last_cancel = ics; }
void cpu_csleep(cpu_csleep_t *ics)
{
	slept = ics;
	CHECK(g_tic_ics == ics, "sleep published");
	tic_callback();
	CHECK(last_cancel == ics, "tic cancelled the sleep");
	CHECK(g_tic_ics == NULL, "tic cleared the published sleep");
}

/* ---- helpers ---- */

static void keys(int const *codes, int n)
{
	uint8_t st[12] = { 0 };
	for(int i = 0; i < n; i++)
		PL_KeySet(st, codes[i]);
	for(int i = 0; i < 6; i++)
		cgd_host_keysc[i] = (uint16_t)(st[2*i] << 8 | st[2*i + 1]);
}
static int const chord[4] = { 0x71, 0x41, 0x23, 0x07 };

static void reset(void)
{
	g_tics = 0;
	g_tic_ics = NULL;
	g_quit_req = false;
	g_abort_armed = true;
	g_chord_tics = 0;
	memset(g_last_scan, 0, sizeof g_last_scan);
	g_key_tic = 0;
	abort_calls = 0;
	abort_jmp = NULL;
	cancels = 0;
	last_cancel = NULL;
	keys(NULL, 0);
	timer_result = 0;
	prof_result = 0;
	g_us_total = 0;
	g_us_frac = 0;
	gtime_init();
}

static void tics(int n)
{
	for(int i = 0; i < n; i++)
		tic_callback();
}

/* ---- tests ---- */

static void test_chord_request(void)
{
	reset();
	tics(5);
	CHECK(!gtime_take_quit(), "no request without the chord");
	keys(chord, 4);
	tics(1);
	CHECK(abort_calls == 0, "no abort on the first chord tic");
	keys(NULL, 0);
	tics(3);
	CHECK(gtime_take_quit(), "request latched (sticky after release)");
	CHECK(!gtime_take_quit(), "taken once");

	/* Taken, then held for a long time: never aborts (the exit path
	   writes the saves) */
	reset();
	keys(chord, 4);
	tics(1);
	CHECK(gtime_take_quit(), "request");
	tics(10 * GTIME_ABORT_HOLD_TICS);
	CHECK(abort_calls == 0, "no abort once the request is taken (%d)", abort_calls);

	/* Three of the four keys: nothing */
	reset();
	keys(chord, 3);
	tics(2 * GTIME_ABORT_HOLD_TICS);
	CHECK(!gtime_take_quit() && abort_calls == 0, "three keys: nothing");
}

static void test_hang_fallback(void)
{
	/* Nobody takes the request: abort after the hold time, once */
	reset();
	keys(chord, 4);
	tics(GTIME_ABORT_HOLD_TICS - 1);
	CHECK(abort_calls == 0, "not before %d tics", GTIME_ABORT_HOLD_TICS);
	tics(1);
	CHECK(abort_calls == 1, "abort after %d tics (%d)", GTIME_ABORT_HOLD_TICS, abort_calls);
	tics(5 * GTIME_ABORT_HOLD_TICS);
	CHECK(abort_calls == 1, "abort only once while the chord stays held (%d)", abort_calls);
	keys(NULL, 0); tics(3); keys(chord, 4); tics(2 * GTIME_ABORT_HOLD_TICS);
	CHECK(abort_calls == 1, "not re-armed by a new chord (%d)", abort_calls);

	/* Released and pressed again within the hold time: the count restarts */
	reset();
	keys(chord, 4);
	tics(GTIME_ABORT_HOLD_TICS - 10);
	keys(NULL, 0);
	tics(1);
	keys(chord, 4);
	tics(GTIME_ABORT_HOLD_TICS - 10);
	CHECK(abort_calls == 0, "hold count restarts on release");

	/* Disarmed (PL_Fatal, exit path) */
	reset();
	gtime_disarm_abort();
	keys(chord, 4);
	tics(3 * GTIME_ABORT_HOLD_TICS);
	CHECK(abort_calls == 0, "disarmed: no abort");
}

static void test_waittics_ics(void)
{
	/* Normal wake-ups: each tic cancels and clears the sleep */
	reset();
	PL_WaitTics(5);
	CHECK(g_tics == 5 && cancels == 5, "waited 5 tics (%d, %d cancels)", g_tics, cancels);
	CHECK(g_tic_ics == NULL, "nothing published after the wait");
	CHECK(polls > 0, "PL_GetTics is a quit-poll point");

	/* The hang fallback fires from the tic while PL_WaitTics sleeps and
	   longjmps out of it: later tics must not touch the dead sleep */
	reset();
	keys(chord, 4);
	tics(GTIME_ABORT_HOLD_TICS - 1);
	jmp_buf jb;
	abort_jmp = &jb;
	if(!setjmp(jb)) {
		PL_WaitTics(g_tics + 100);
		CHECK(0, "PL_WaitTics returned instead of aborting");
	}
	abort_jmp = NULL;
	CHECK(abort_calls == 1, "aborted from the tic (%d)", abort_calls);
	CHECK(g_tic_ics == NULL, "no dangling sleep after the abort");
	int before = cancels;
	tics(3 * GTIME_ABORT_HOLD_TICS);
	CHECK(cancels == before, "later tics do not cancel the dead sleep (%d)", cancels - before);
	CHECK(abort_calls == 1, "and do not abort again (%d)", abort_calls);
}

static void test_idle(void)
{
	reset();
	tics(100);
	CHECK(gtime_idle_tics() == 100, "idle %d", gtime_idle_tics());
	keys((int[]){ 0x15 }, 1);
	tics(1);
	CHECK(gtime_idle_tics() == 0, "a press resets (%d)", gtime_idle_tics());
	/* Pinned key: no change, idle time grows */
	tics(500);
	CHECK(gtime_idle_tics() == 500, "pinned key: idle %d", gtime_idle_tics());
	keys(NULL, 0);
	tics(1);
	CHECK(gtime_idle_tics() == 0, "a release resets (%d)", gtime_idle_tics());
	tics(7);
	gtime_idle_reset();
	CHECK(gtime_idle_tics() == 0, "reset");

	/* No tic timer: never idle (nothing measured) */
	reset();
	timer_result = -1;
	gtime_init();
	CHECK(gtime_idle_tics() == 0, "no timer: 0");
}

/* In-game idle policy (integration follow-up: CGD_IsPaused) */
static void test_idle_policy(void)
{
	bool lose = false;
	/* Nothing to lose: 10 minutes */
	CHECK(gtime_game_idle_limit_ms(GIDLE_DEMO | GIDLE_NOLEVEL, false, true, &lose)
		== GINP_IDLE_MS && !lose, "title demo");
	CHECK(gtime_game_idle_limit_ms(GIDLE_DEMO, false, false, &lose)
		== GINP_IDLE_MS && !lose, "replay (-playdemo)");
	CHECK(gtime_game_idle_limit_ms(GIDLE_DEMO | GIDLE_MENU, false, false, &lose)
		== GINP_IDLE_MS && !lose, "menu over a demo");
	CHECK(gtime_game_idle_limit_ms(GIDLE_NOLEVEL, true, true, &lose)
		== GINP_IDLE_MS && !lose, "title page after an ended game");
	CHECK(gtime_game_idle_limit_ms(GIDLE_NOLEVEL | GIDLE_MENU, false, false, &lose)
		== GINP_IDLE_MS && !lose, "no game yet, menu");
	/* The player's game: 30 minutes, progress at stake */
	CHECK(gtime_game_idle_limit_ms(0, true, false, &lose)
		== GINP_GAME_IDLE_MS && lose, "playing (or automap)");
	CHECK(gtime_game_idle_limit_ms(GIDLE_PAUSED, true, false, &lose)
		== GINP_GAME_IDLE_MS && lose, "paused");
	CHECK(gtime_game_idle_limit_ms(GIDLE_MENU, true, false, &lose)
		== GINP_GAME_IDLE_MS && lose, "menu / save-name entry");
	CHECK(gtime_game_idle_limit_ms(GIDLE_PAUSED | GIDLE_MENU, true, false, &lose)
		== GINP_GAME_IDLE_MS && lose, "paused behind the menu");
	CHECK(gtime_game_idle_limit_ms(GIDLE_NOLEVEL, true, false, &lose)
		== GINP_GAME_IDLE_MS && lose, "intermission / finale");
	CHECK(gtime_game_idle_limit_ms(0, true, false, NULL) == GINP_GAME_IDLE_MS,
		"NULL lose_progress");
}

static void test_fallback_wrap(void)
{
	reset();
	timer_result = -1;          /* no tic timer: tics from the microseconds */
	gtime_init();
	CHECK(g_tic_timer < 0 && g_prof_ok, "fallback mode");
	int prev = PL_GetTics();
	/* 100 s per step (Pphi/4 = 7.3728 MHz ticks), 5000 s in total: past
	   2^32 us = 4295 s */
	uint32_t step = 7372800u * 100u;
	bool mono = true;
	for(int i = 0; i < 50; i++) {
		tcnt -= step;
		int t = PL_GetTics();
		if(t < prev) mono = false;
		prev = t;
	}
	CHECK(mono, "fallback tics are monotonic");
	CHECK(prev >= 5000 * 35 - 2 && prev <= 5000 * 35, "5000 s = %d tics", prev);

	/* PL_WaitTics in fallback mode returns once the time has come */
	int until = PL_GetTics() + 1;
	tcnt -= 7372800u / 10;      /* 100 ms */
	PL_WaitTics(until);
	CHECK(PL_GetTics() >= until, "fallback wait");

	/* No clock at all: PL_WaitTics does not hang */
	reset();
	timer_result = -1;
	prof_result = -1;
	gtime_init();
	PL_WaitTics(10);
	CHECK(1, "no clock: returned");
}

int main(void)
{
	test_chord_request();
	test_hang_fallback();
	test_waittics_ics();
	test_idle();
	test_idle_policy();
	test_fallback_wrap();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
