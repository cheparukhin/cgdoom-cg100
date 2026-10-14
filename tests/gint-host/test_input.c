/* Host unit test for src/gint/input.c:
   - ginp_wait_release() is bounded (3 s, or a poll cap when the clock does
     not advance) and still waits for a real release;
   - ginp_wait_any_key() and the idle hook of ginp_getkey().
   KIUDATA is simulated (cgd_host_keysc); sleep() advances a fake clock. */

#include "cgd_gint.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

volatile uint16_t cgd_host_keysc[6];

static uint32_t now_us = 0;
static uint32_t step_us = 2000;      /* per sleep(); 0 = frozen clock */
static long sleeps = 0;
static uint32_t release_at = 0;      /* clear the keys at this time (0: never) */
static uint32_t press_at = 0;        /* press a key at this time (0: never) */
static int press_word = 0, press_bits = 0;

uint32_t PL_GetMicros(void) { return now_us; }
/* main.c's foreground safe point */
static long polls = 0;
void gquit_poll(void) { polls++; }

/* time.c's tic scan: tics since the keyboard state changed,
   sampled at every sleep() and every query */
static bool ticking = true;
static uint16_t seen_keys[6];
static uint32_t change_us = 0;
static void track_keys(void)
{
	if(memcmp(seen_keys, (void const *)cgd_host_keysc, sizeof seen_keys)) {
		memcpy(seen_keys, (void const *)cgd_host_keysc, sizeof seen_keys);
		change_us = now_us;
	}
}
bool gtime_ticking(void) { return ticking; }
int gtime_idle_tics(void)
{
	if(!ticking)
		return 0;
	track_keys();
	return (int)((uint64_t)(now_us - change_us) * 35 / 1000000);
}
void gtime_idle_reset(void) { track_keys(); change_us = now_us; }

void sleep(void)
{
	sleeps++;
	now_us += step_us;
	if(release_at && now_us >= release_at) {
		memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
		release_at = 0;
	}
	if(press_at && now_us >= press_at) {
		cgd_host_keysc[press_word] = (uint16_t)press_bits;
		press_at = 0;
	}
	track_keys();
}

static void reset(void)
{
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
	now_us = 1000; step_us = 2000; sleeps = 0;
	release_at = press_at = 0;
	ticking = true;
	gtime_idle_reset();
}

static void test_wait_release(void)
{
	/* Nothing held: three clean scans */
	reset();
	ginp_wait_release();
	CHECK(sleeps == 2, "clean: %ld sleeps", sleeps);

	/* Held for 1 s, then released: waits for the release */
	reset();
	cgd_host_keysc[2] = 0x0400;
	release_at = 1000 + 1000000;
	ginp_wait_release();
	CHECK(now_us >= 1000000 && now_us < 1100000, "released after 1 s (t=%u)", now_us);

	/* Stuck key: gives up after ~3 s instead of hanging */
	reset();
	cgd_host_keysc[5] = 0x0001;
	ginp_wait_release();
	CHECK(now_us - 1000 >= GINP_RELEASE_TIMEOUT_US - step_us
		&& now_us - 1000 <= GINP_RELEASE_TIMEOUT_US + step_us,
		"stuck key: bounded at %u us", now_us - 1000);

	/* Stuck key and a clock that does not advance: poll cap */
	reset();
	step_us = 0;
	cgd_host_keysc[0] = 0x8000;
	ginp_wait_release();
	CHECK(sleeps == GINP_RELEASE_MAX_POLLS, "frozen clock: %ld polls", sleeps);
}

static void test_wait_any_key(void)
{
	reset();
	CHECK(!ginp_wait_any_key(5000), "no key: false");
	CHECK(now_us - 1000 >= 5000000 && now_us - 1000 < 5100000, "after 5 s (%u)", now_us);

	reset();
	press_at = 2000000; press_word = 3; press_bits = 0x0010;
	CHECK(ginp_wait_any_key(60000), "key at 2 s: true");
	CHECK(now_us < 2100000, "returned at once (%u)", now_us);

	reset();
	step_us = 0;
	CHECK(!ginp_wait_any_key(1000), "frozen clock: bounded");
}

/* Edge-triggered; a key held or stuck at entry is no press */
static void test_wait_any_key_edge(void)
{
	/* Pinned key: idle_quit()'s "press any key to stay" times out */
	reset();
	cgd_host_keysc[2] = 0x0400;
	CHECK(!ginp_wait_any_key(60000), "pinned key: no press");
	CHECK(now_us - 1000 >= 60000000 && now_us - 1000 < 60100000, "pinned: after 60 s (%u)", now_us);

	/* Held at entry, then a second key: a press */
	reset();
	cgd_host_keysc[2] = 0x0400;
	press_at = 3000000; press_word = 2; press_bits = 0x0401;
	CHECK(ginp_wait_any_key(60000), "second key while one is held");
	CHECK(now_us >= 3000000 && now_us < 3100000, "second key at once (%u)", now_us);

	/* Held at entry, released, pressed again: a press (PL_Fatal: the
	   player lets go of the fire key, reads, presses it) */
	reset();
	cgd_host_keysc[4] = 0x0008;
	release_at = 1000000;
	press_at = 5000000; press_word = 4; press_bits = 0x0008;
	CHECK(ginp_wait_any_key(60000), "re-press of the held key");
	CHECK(now_us >= 5000000 && now_us < 5100000, "re-press at 5 s (%u)", now_us);

	/* Held at entry and still held 3 s later (PL_Fatal after its bounded
	   release wait): the message is not dismissed */
	reset();
	cgd_host_keysc[0] = 0x8000;
	ginp_wait_release();
	CHECK(!ginp_wait_any_key(10000), "still-held key does not dismiss");
}

static int hook_calls = 0;
static void hook(void)
{
	hook_calls++;
}

static void test_idle_hook(void)
{
	reset();
	step_us = 100000;   /* coarse steps */
	ginp_idle_hook = hook;
	hook_calls = 0;
	int k = ginp_getkey();
	CHECK(k == GK_NONE && hook_calls == 1, "idle: hook called, GK_NONE (k=%d)", k);
	CHECK(now_us >= (uint32_t)GINP_IDLE_MS * 1000u, "after %u us", now_us);

	/* A key before the timeout: no hook (OK = 0x84 on the fx-CG100: row 8
	   is byte 9, col 3 is bit 3 -> word 4, low byte) */
	reset();
	hook_calls = 0;
	press_at = 5000000; press_word = 4; press_bits = 0x0008;
	k = ginp_getkey();
	CHECK(k == GK_OK && hook_calls == 0, "key: %d, hook %d", k, hook_calls);
	ginp_idle_hook = NULL;
}

/* The idle hook under a pinned key (EXE, not an arrow: no repeat)
   quits: idle_quit()'s wait returns false */
static bool idle_stay = true;
static void hook_like_idle_quit(void)
{
	hook_calls++;
	idle_stay = ginp_wait_any_key(60 * 1000);
}
static void test_idle_pinned(void)
{
	reset();
	step_us = 100000;
	cgd_host_keysc[0] = 0x0002;          /* EXE 0x15: row 1 byte 0, col 2 */
	ginp_idle_hook = hook_like_idle_quit;
	hook_calls = 0;
	polls = 0;
	int k = ginp_getkey();
	CHECK(k == GK_NONE && hook_calls == 1 && !idle_stay,
		"pinned EXE: hook %d, stay %d", hook_calls, idle_stay);
	CHECK(polls > 0, "ginp_getkey_timeout polls the quit request (%ld)", polls);
	ginp_idle_hook = NULL;
}

/* Idleness is "no keyboard change", not "no key returned" */
static int code_word(int code) { return PL_KEY_BYTE(code) / 2; }
static int code_bits(int code)
{
	int b = PL_KEY_BYTE(code);
	return (b & 1) ? PL_KEY_BIT(code) : PL_KEY_BIT(code) << 8;
}
static void test_idle_activity(void)
{
	/* An arrow pressed, then pinned: it auto-repeats every 70 ms, and the
	   hook still runs GINP_IDLE_MS after the press (it never did) */
	reset();
	step_us = 10000;
	ginp_idle_hook = hook;
	hook_calls = 0;
	press_at = 1000000; press_word = code_word(0x94); press_bits = code_bits(0x94);
	long repeats = 0, calls = 0;
	while(hook_calls == 0 && calls < 200000) {
		int k = ginp_getkey();
		calls++;
		if(k == GK_UP) repeats++;
		else CHECK(k == GK_NONE, "pinned UP: unexpected key %d", k);
	}
	CHECK(hook_calls == 1, "pinned arrow: hook after %ld calls", calls);
	CHECK(repeats > 5000, "pinned arrow repeats (%ld)", repeats);
	uint32_t at = now_us - 1000000;
	CHECK(at >= (uint32_t)GINP_IDLE_MS * 1000u
		&& at < (uint32_t)GINP_IDLE_MS * 1000u + 100000u,
		"pinned arrow: hook %u us after the press", at);

	/* The hook returned ("stay"): a fresh idle period, not a hook per call */
	uint32_t t0 = now_us;
	hook_calls = 0;
	int k = ginp_getkey();
	CHECK(hook_calls == 0 && k == GK_UP, "after the hook: a repeat (%d), no hook", k);
	CHECK(now_us - t0 < 200000, "repeat at once (%u)", now_us - t0);
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);

	/* A non-UI key (and its release) is activity: the hook comes
	   GINP_IDLE_MS after the release, not after the call started */
	reset();
	step_us = 100000;
	hook_calls = 0;
	press_at = 500000000; press_word = code_word(0x16); press_bits = code_bits(0x16);
	release_at = 501000000;
	k = ginp_getkey();
	CHECK(k == GK_NONE && hook_calls == 1, "non-UI key: hook %d (k=%d)", hook_calls, k);
	at = now_us - 501000000;
	CHECK(at >= (uint32_t)GINP_IDLE_MS * 1000u
		&& at < (uint32_t)GINP_IDLE_MS * 1000u + 200000u,
		"non-UI key: hook %u us after the release", at);

	/* The controls editor's capture timed out after GINP_IDLE_MS: the
	   hook runs at the next ginp_getkey(), not 10 minutes later */
	reset();
	step_us = 100000;
	hook_calls = 0;
	CHECK(ginp_wait_raw_key(GINP_IDLE_MS) == -1, "capture timed out");
	t0 = now_us;
	k = ginp_getkey();
	CHECK(k == GK_NONE && hook_calls == 1 && now_us - t0 < 1000000,
		"capture timeout: hook at once (%d, %u us)", hook_calls, now_us - t0);

	/* No tic timer: a per-call GINP_IDLE_MS timeout */
	reset();
	ticking = false;
	step_us = 100000;
	hook_calls = 0;
	k = ginp_getkey();
	CHECK(k == GK_NONE && hook_calls == 1
		&& now_us - 1000 >= (uint32_t)GINP_IDLE_MS * 1000u
		&& now_us - 1000 < (uint32_t)GINP_IDLE_MS * 1000u + 200000u,
		"no timer: hook after GINP_IDLE_MS (%u)", now_us);
	ginp_idle_hook = NULL;
}

/* The emergency abort chord (time.c's tic interrupt) */
static void set_key(uint8_t st[12], int code) { st[PL_KEY_BYTE(code)] |= PL_KEY_BIT(code); }
static void test_abort_chord(void)
{
	uint8_t st[12];
	int keys[4] = { 0x71, 0x41, 0x23, 0x07 };
	memset(st, 0, sizeof st);
	for(int k = 0; k < 4; k++) set_key(st, keys[k]);
	CHECK(ginp_abort_chord(st), "chord");
	/* any three of the four: no abort */
	for(int drop = 0; drop < 4; drop++) {
		memset(st, 0, sizeof st);
		for(int k = 0; k < 4; k++) if(k != drop) set_key(st, keys[k]);
		CHECK(!ginp_abort_chord(st), "without key %d", drop);
	}
	/* the fx-CG50's SHIFT position (SETTINGS here) does not count */
	memset(st, 0, sizeof st);
	set_key(st, 0x81);
	for(int k = 1; k < 4; k++) set_key(st, keys[k]);
	CHECK(!ginp_abort_chord(st), "wrong SHIFT");
	/* Through a real KIUDATA scan: CGDoom layout, row r is byte r^1 */
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
	memset(st, 0, sizeof st);
	set_key(st, 0x71); set_key(st, 0x41); set_key(st, 0x23); set_key(st, 0x07);
	for(int i = 0; i < 6; i++)
		cgd_host_keysc[i] = (uint16_t)(st[2*i] << 8 | st[2*i + 1]);
	uint8_t scan[12];
	ginp_scan(scan);
	CHECK(ginp_abort_chord(scan), "chord through ginp_scan");
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
}

/* The key capture of the controls editor is bounded */
static void test_wait_raw_key(void)
{
	reset();
	press_at = 1000 + 500000; press_word = 4; press_bits = 0x2000;
	int code = ginp_wait_raw_key(60000);
	CHECK(code >= 0 && now_us >= 500000 && now_us < 600000, "raw key %02x at %u", code, now_us);
	reset();
	code = ginp_wait_raw_key(60000);
	CHECK(code == -1 && now_us >= 60000000 && now_us < 61000000, "raw key timeout (t=%u)", now_us);
	reset(); step_us = 0;
	code = ginp_wait_raw_key(1000);
	CHECK(code == -1, "raw key timeout with a frozen clock");
}

/* Safe start = DEL (0x44) held when the add-in starts, read
   from a raw scan */
static void test_safe_start(void)
{
	uint8_t st[12];
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
	CHECK(!ginp_safe_start(), "no key, no safe start");
	memset(st, 0, sizeof st);
	set_key(st, 0x44);
	for(int i = 0; i < 6; i++)
		cgd_host_keysc[i] = (uint16_t)(st[2*i] << 8 | st[2*i + 1]);
	CHECK(ginp_safe_start(), "DEL held");
	/* The launch key (EXE or OK) is not it */
	memset(st, 0, sizeof st);
	set_key(st, 0x15); set_key(st, 0x84);
	for(int i = 0; i < 6; i++)
		cgd_host_keysc[i] = (uint16_t)(st[2*i] << 8 | st[2*i + 1]);
	CHECK(!ginp_safe_start(), "EXE/OK held is not safe start");
	memset((void *)cgd_host_keysc, 0, sizeof cgd_host_keysc);
}

int main(void)
{
	test_safe_start();
	test_abort_chord();
	test_wait_raw_key();
	test_wait_release();
	test_wait_any_key();
	test_wait_any_key_edge();
	test_idle_hook();
	test_idle_pinned();
	test_idle_activity();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
