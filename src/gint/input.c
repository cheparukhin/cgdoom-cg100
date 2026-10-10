/* CGDoom gint platform: keyboard.

   Raw KEYSC scan only. The six 16-bit KIUDATA registers at 0xa44b0000 are
   read exactly like CGDoom (cgdoom-kbd.c) and MPM do; reading them has no
   side effect, and gint's own keyboard driver (keysc.c) reads the same
   registers from its 128 Hz timer interrupt, so both can coexist. The words
   are stored big-endian, which is CGDoom's layout: row r is byte r^1, key
   code (row << 4) | (7 - col) is bit (col) of that byte; AC/ON is 0x07.

   gint's keydown()/getkey()/events are never used: on the fx-CG100 gint
   remaps rows 5-9 through its own table, so matrix codes would not mean
   physical keys any more. The launcher UI also works from raw scans and
   interprets keys by physical position for each model. gint's event queue
   is simply left to overflow (events are dropped).

   Consequence (gint review round 2): gint's freeze-abort chord
   SHIFT+7+3+AC/ON (keysc_tick) never fires. It tests keydown(), which
   answers from the keydev state that only advances when events are
   un-queued (pollevent/getkey/clearevents), and a full queue also stops
   the state updates. Draining the queue would not help during a hang (no
   code runs to drain it). Instead, time.c checks the same chord on raw
   scans from the 35 Hz tic interrupt (ginp_abort_chord) and latches a
   quit request that the foreground takes (gquit_poll, also called from
   every wait loop of this file); abort() is only the fallback for a hang
   (round 3.1, see time.c). */

#include "cgd_gint.h"

#include <gint/cpu.h>
#include <string.h>

#ifndef CGD_HOST_TEST
#define KEYSC ((volatile uint16_t *)0xa44b0000)
#else
/* tests/gint-host: simulated KIUDATA words */
extern volatile uint16_t cgd_host_keysc[6];
#define KEYSC cgd_host_keysc
#endif

void ginp_scan(uint8_t state[12])
{
	for(int i = 0; i < 6; i++) {
		uint16_t w = KEYSC[i];
		state[2*i]     = w >> 8;
		state[2*i + 1] = w & 0xff;
	}
}

void PL_ScanKeyboard(uint8_t state[12])
{
	ginp_scan(state);
}

bool ginp_abort_chord(uint8_t const st[12])
{
	/* SHIFT+7+3+AC/ON by physical position: SHIFT is (8,1) on the fx-CG50
	   and (7,1) on the fx-CG100; rows 1..4 and AC/ON are the same on both
	   models (keymaps.c) */
	int shift = g_is_cg100 ? 0x71 : 0x81;
	return PL_KeyDown(st, shift) && PL_KeyDown(st, 0x41)
		&& PL_KeyDown(st, 0x23) && PL_KeyDown(st, 0x07);
}

bool ginp_safe_start(void)
{
	uint8_t st[12];
	ginp_scan(st);
	return PL_KeyDown(st, GINP_SAFE_START_KEY);
}

bool ginp_any_key(void)
{
	for(int i = 0; i < 6; i++) {
		if(KEYSC[i])
			return true;
	}
	return false;
}

/* Wait ~2 ms between polls (the 128 Hz keyboard timer and the 35 Hz tic
   timer wake us up from sleep()) */
static void idle(void)
{
	sleep();
}

void ginp_wait_release(void)
{
	/* Require a few consecutive empty scans (bounces). Bounded (gint
	   review round 1): a stuck or worn key, or a matrix position that reads
	   1 on some unit, must not keep us from ever returning to MPM. gpSP
	   bounds the same wait (20000 polls). The poll cap only covers the
	   case where PL_GetMicros() does not advance: polls are ~2 ms apart
	   (the 3 s deadline comes first), and at most one 128 Hz keyboard tick
	   apart, so the cap is ~16 s at worst. */
	int clean = 0;
	uint32_t start = PL_GetMicros();
	for(int polls = 0; clean < 3; polls++) {
		/* Every UI wait is a quit-chord poll point (gint review round
		   3.1): the chord itself may be what is held here */
		gquit_poll();
		if(ginp_any_key())
			clean = 0;
		else
			clean++;
		if(clean >= 3)
			break;
		if(polls >= GINP_RELEASE_MAX_POLLS
			|| PL_GetMicros() - start >= GINP_RELEASE_TIMEOUT_US)
			break;
		idle();
	}
}

bool ginp_wait_any_key(int timeout_ms)
{
	/* Edge-triggered (gint review round 3.1): a key already down at entry
	   (still held from the game, pinned under an object, a stuck matrix
	   bit) is not a press, or idle_quit()'s "press any key to stay" never
	   quits and PL_Fatal's message is dismissed unread. A key released
	   since entry leaves the baseline, so pressing it again counts. */
	uint8_t base[12], now[12];
	ginp_scan(base);
	uint32_t start = PL_GetMicros();
	uint32_t limit = (uint32_t)timeout_ms * 1000u;
	/* Poll cap for a clock that does not advance, as above (polls are
	   1-8 ms apart, so the deadline normally comes first) */
	int max_polls = timeout_ms + 16;
	for(int polls = 0; polls < max_polls; polls++) {
		gquit_poll();
		ginp_scan(now);
		for(int i = 0; i < 12; i++) {
			if(now[i] & ~base[i])
				return true;
			base[i] &= now[i];
		}
		if(PL_GetMicros() - start >= limit)
			return false;
		idle();
	}
	return false;
}

/* First key down in a state, CGDoom order (rows 9..1, then AC/ON) */
static int first_key(uint8_t const *st)
{
	for(int row = 9; row >= 1; row--) {
		for(int c = 1; c <= 6; c++) {
			int code = (row << 4) | c;
			if(PL_KeyDown(st, code))
				return code;
		}
	}
	if(PL_KeyDown(st, 0x07))
		return 0x07;
	return -1;
}

int ginp_wait_raw_key(int timeout_ms)
{
	/* Bounded (gint review round 2): no OS auto power-off while we own
	   the interrupts. Poll cap for a frozen clock, as in ginp_wait_any_key */
	ginp_wait_release();
	uint8_t st[12];
	uint32_t start = PL_GetMicros();
	uint32_t limit = (uint32_t)timeout_ms * 1000u;
	for(int polls = 0; polls < timeout_ms + 16; polls++) {
		idle();
		gquit_poll();
		ginp_scan(st);
		int code = first_key(st);
		if(code >= 0)
			return code;
		if(PL_GetMicros() - start >= limit)
			break;
	}
	return -1;
}

//---
// UI keys
//---

/* Physical positions as matrix codes */
static int ui_action(int code)
{
	if(g_is_cg100) {
		switch(code) {
		case 0x94: return GK_UP;        /* UP       (row 9) */
		case 0x74: return GK_DOWN;      /* DOWN     (row 7) */
		case 0x83: return GK_LEFT;      /* LEFT     (row 8) */
		case 0x85: return GK_RIGHT;     /* RIGHT    (row 8) */
		case 0x84: return GK_OK;        /* OK       (row 8) */
		case 0x82: return GK_BACK;      /* BACK     (row 8) */
		case 0x92: return GK_QUIT;      /* HOME     (row 9) */
		}
	}
	else {
		switch(code) {
		case 0x86: return GK_UP;
		case 0x75: return GK_DOWN;
		case 0x85: return GK_LEFT;
		case 0x76: return GK_RIGHT;
		case 0x74: return GK_BACK;      /* EXIT */
		case 0x84: return GK_QUIT;      /* MENU */
		}
	}
	switch(code) {
	case 0x15: return GK_OK;            /* EXE */
	case 0x44: return GK_DEL;           /* DEL */
	case 0x11: return GK_DIGIT0 + 0;
	case 0x21: return GK_DIGIT0 + 1;
	case 0x22: return GK_DIGIT0 + 2;
	case 0x23: return GK_DIGIT0 + 3;
	case 0x31: return GK_DIGIT0 + 4;
	case 0x32: return GK_DIGIT0 + 5;
	case 0x33: return GK_DIGIT0 + 6;
	case 0x41: return GK_DIGIT0 + 7;
	case 0x42: return GK_DIGIT0 + 8;
	case 0x43: return GK_DIGIT0 + 9;
	}
	return GK_NONE;
}

static bool is_arrow(int action)
{
	return action == GK_UP || action == GK_DOWN || action == GK_LEFT
		|| action == GK_RIGHT;
}

#define REPEAT_FIRST_US  400000u
#define REPEAT_NEXT_US    70000u

int ginp_getkey_timeout(int timeout_ms)
{
	static uint8_t prev[12];
	static bool prev_valid = false;
	static int held_action = GK_NONE;
	static uint32_t repeat_at = 0;

	uint32_t start = PL_GetMicros();
	uint8_t now[12];

	/* Keys already held on the very first call (e.g. the EXE/OK that
	   launched us from MPM) are not new presses */
	if(!prev_valid) {
		ginp_scan(prev);
		prev_valid = true;
	}

	while(1) {
		/* Quit chord request (gint review round 3.1): may not return */
		gquit_poll();
		ginp_scan(now);
		uint32_t t = PL_GetMicros();

		/* New presses */
		int result = GK_NONE;
		for(int row = 9; row >= 0 && result == GK_NONE; row--) {
			int byte = row ^ 1;
			uint8_t pressed = now[byte] & ~prev[byte];
			if(!pressed)
				continue;
			for(int col = 0; col < 8; col++) {
				if(!(pressed & (1 << col)))
					continue;
				int code = (row << 4) | (7 - col);
				int a = ui_action(code);
				if(a != GK_NONE) {
					result = a;
					held_action = is_arrow(a) ? a : GK_NONE;
					repeat_at = t + REPEAT_FIRST_US;
					break;
				}
			}
		}

		/* Repeat a held arrow */
		if(result == GK_NONE && held_action != GK_NONE) {
			bool still = false;
			for(int i = 0; i < 12 && !still; i++) {
				for(int col = 0; col < 8; col++) {
					if(!(now[i] & (1 << col)))
						continue;
					int code = ((i ^ 1) << 4) | (7 - col);
					if(ui_action(code) == held_action)
						still = true;
				}
			}
			if(!still)
				held_action = GK_NONE;
			else if((int32_t)(t - repeat_at) >= 0) {
				result = held_action;
				repeat_at = t + REPEAT_NEXT_US;
			}
		}

		memcpy(prev, now, sizeof prev);
		if(result != GK_NONE)
			return result;
		if(timeout_ms >= 0
			&& (t - start) >= (uint32_t)timeout_ms * 1000u)
			return GK_NONE;
		idle();
	}
}

void (*ginp_idle_hook)(void) = NULL;

int ginp_getkey(void)
{
	if(!ginp_idle_hook)
		return ginp_getkey_timeout(-1);

	/* Idle timeout (critique §C7, gint review round 1): the add-in owns
	   the interrupts, so the OS's auto power-off never runs while we wait
	   here. After GINP_IDLE_MS of idleness, let the hook decide (it offers
	   to quit, or longjmps to the exit path). When it returns, the screen
	   it drew must be repainted: report GK_NONE, which every caller treats
	   as "redraw and wait again".

	   Idleness is time.c's "no change of the keyboard state" (gint review
	   round 3.2), the measure of the in-game idle exit. A per-call timeout
	   restarted on every returned key: an arrow pinned down auto-repeats
	   every 70 ms forever and kept the launcher drawing at ~14 Hz until
	   the battery was flat. It also counts the time spent in other waits
	   (the controls editor's key capture times out after GINP_IDLE_MS, and
	   the hook then runs at once, not 10 minutes later). */
	if(!gtime_ticking()) {
		/* No tic timer: the per-call timeout of round 1 */
		int k = ginp_getkey_timeout(GINP_IDLE_MS);
		if(k != GK_NONE)
			return k;
	}
	else {
		int const limit = GINP_IDLE_MS / 1000 * 35;
		while(1) {
			int idle = gtime_idle_tics();
			if(idle >= limit)
				break;
			/* +1 ms: never a zero timeout (that would spin) */
			int k = ginp_getkey_timeout((limit - idle) * 1000 / 35 + 1);
			if(k != GK_NONE)
				return k;
			/* GK_NONE: a non-UI key (or a release) may have restarted the
			   idle time; the loop decides */
		}
	}
	ginp_idle_hook();
	/* The hook returned ("stay", or it had nothing to do): a fresh idle
	   period, or a hook that returns at once would run on every call */
	gtime_idle_reset();
	return GK_NONE;
}
