/* CGDoom gint platform: keymaps and key names.

   Keymaps are CGDoom_Keymap arrays of physical matrix codes (row << 4 |
   (7 - col)) in the order of the engine's CGD_KeymapEntries (27 Doom
   actions). The engine keeps all keymap logic; the platform only chooses
   which array is copied into CGD_Options.Keymap (CGD_CopyKeymap), shows key
   names, and lets the user edit bindings. No registration hook is needed
   for the maps themselves.

   fx-CG100 physical layout of rows 9..5 (cols 6..1), as matrix codes:
     row 9: ON 91  HOME 92     PREVTAB 93  UP 94       NEXTTAB 95  PAGEUP 96
     row 8: SETTINGS 81 BACK 82 LEFT 83    OK 84       RIGHT 85    PAGEDOWN 86
     row 7: SHIFT 71 ALPHA 72  VARIABLE 73 DOWN 74     CATALOG 75  TOOLS 76
     row 6: X,O,T 61 FRAC 62   SQRT 63     ^ 64        x^2 65      e^x 66
     row 5: , 51     sin 52    cos 53      tan 54      ( 55        ) 56
   rows 4..1 are the digit/operator block as on the fx-CG50, except (1,3) =
   FORMAT (was (-)), and AC is 0x07 (gint keydev.c CG100_keymap, MPM, gpSP).

   The Doom menu key (KEY_ESCAPE) is hardwired to KEYCODE_MENU (0x84) in the
   engine, which is the OK key in the middle of the fx-CG100 D-pad. If the
   engine exports `int CGD_EscapeKeycode` (requested), we set it to BACK
   (0x82); otherwise OK stays the menu key and is left
   unbound by our presets. With BACK as the menu key, OK is a second Enter
   key in the Doom menu (CGD_EnterKeycode2, final review) as long as no
   action is bound to it. */

#include "cgd_gint.h"
#include "cgdoom-kbd.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

_Static_assert(CGDOOM_KEYMAP_SIZE == 27, "keymaps below have 27 entries");

#define CG100_ON        0x91
#define CG100_HOME      0x92
#define CG100_PREVTAB   0x93
#define CG100_UP        0x94
#define CG100_NEXTTAB   0x95
#define CG100_PAGEUP    0x96
#define CG100_SETTINGS  0x81
#define CG100_BACK      0x82
#define CG100_LEFT      0x83
#define CG100_OK        0x84
#define CG100_RIGHT     0x85
#define CG100_PAGEDOWN  0x86
#define CG100_SHIFT     0x71
#define CG100_ALPHA     0x72
#define CG100_VARIABLE  0x73
#define CG100_DOWN      0x74
#define CG100_CATALOG   0x75
#define CG100_TOOLS     0x76
#define CG100_XOT       0x61
#define CG100_FRAC      0x62
#define CG100_SQRT      0x63
#define CG100_POWER     0x64
#define CG100_SQUARE    0x65
#define CG100_EXPFUN    0x66
#define CG100_COMMA     0x51
#define CG100_SIN       0x52
#define CG100_COS       0x53
#define CG100_TAN       0x54
#define CG100_LEFTP     0x55
#define CG100_RIGHTP    0x56
#define CG100_FORMAT    0x14

/* Default fx-CG100 layout: right thumb on the D-pad cluster (arrows, strafe
   on the tab keys beside UP, map on CATALOG, pause/FPS on the page keys),
   left thumb on SHIFT (fire) / ALPHA (use) / VARIABLE (run), BACK opens the
   Doom menu, weapons on the digits. Every one of the 27 actions is bound;
   OK, HOME, ON and SETTINGS are left free. No binding needs the abort
   chord SHIFT+7+3+AC (time.c). */
static const CGDoom_Keymap keymap_cg100_thumbs = {
	CG100_LEFT,       /* Turn left */
	CG100_RIGHT,      /* Turn right */
	CG100_UP,         /* Move forward */
	CG100_DOWN,       /* Move backward */
	CG100_ALPHA,      /* Use / open */
	CG100_SHIFT,      /* Fire */
	KEYCODE_1,        /* Weapon 1..7 */
	KEYCODE_2,
	KEYCODE_3,
	KEYCODE_4,
	KEYCODE_5,
	KEYCODE_6,
	KEYCODE_7,
	CG100_CATALOG,    /* Automap */
	CG100_PAGEUP,     /* Pause */
	CG100_PREVTAB,    /* Strafe left */
	CG100_NEXTTAB,    /* Strafe right */
	CG100_VARIABLE,   /* Run */
	KEYCODE_MINUS,    /* Smaller view */
	KEYCODE_PLUS,     /* Larger view */
	CG100_COMMA,      /* Cheat */
	CG100_XOT,        /* Noclip */
	CG100_FRAC,       /* Brightness */
	CG100_SQRT,       /* Heap statistics */
	CG100_PAGEDOWN,   /* FPS counter */
	CG100_TOOLS,      /* Frameskip */
	CG100_RIGHTP,     /* Profiler */
};

/* Variant: turn with PREVTAB/NEXTTAB, strafe with LEFT/RIGHT */
static const CGDoom_Keymap keymap_cg100_strafe = {
	CG100_PREVTAB,    /* Turn left */
	CG100_NEXTTAB,    /* Turn right */
	CG100_UP,
	CG100_DOWN,
	CG100_ALPHA,
	CG100_SHIFT,
	KEYCODE_1,
	KEYCODE_2,
	KEYCODE_3,
	KEYCODE_4,
	KEYCODE_5,
	KEYCODE_6,
	KEYCODE_7,
	CG100_CATALOG,
	CG100_PAGEUP,
	CG100_LEFT,       /* Strafe left */
	CG100_RIGHT,      /* Strafe right */
	CG100_VARIABLE,
	KEYCODE_MINUS,
	KEYCODE_PLUS,
	CG100_COMMA,
	CG100_XOT,
	CG100_FRAC,
	CG100_SQRT,
	CG100_PAGEDOWN,
	CG100_TOOLS,
	CG100_RIGHTP,
};

static const gkm_preset_t presets_cg100[] = {
	{ "Thumbs (CG100)",        keymap_cg100_thumbs },
	{ "Strafe arrows (CG100)", keymap_cg100_strafe },
};

gkm_preset_t const *gkm_presets(int *count)
{
	*count = sizeof presets_cg100 / sizeof *presets_cg100;
	return presets_cg100;
}

/* fx-CG100 legends, indexed like the engine's tables: 6 * (9 - row) +
   (code & 0xf) - 1; 0x45 stands for AC (code 0x07) */
static char const * const names_cg100[] = {
	"ON",       "HOME",     "PrevTab",  "Up",       "NextTab",  "PageUp",
	"SETTINGS", "BACK",     "Left",     "OK",       "Right",    "PageDown",
	"SHIFT",    "ALPHA",    "VARIABLE", "Down",     "CATALOG",  "TOOLS",
	"X,O,t",    "Frac",     "sqrt",     "^",        "x^2",      "e^x",
	",",        "sin",      "cos",      "tan",      "(",        ")",
	"7",        "8",        "9",        "DEL",      "AC",       NULL,
	"4",        "5",        "6",        "x",        "/",        NULL,
	"1",        "2",        "3",        "+",        "-",        NULL,
	"0",        ".",        "x10^x",    "FORMAT",   "EXE",      NULL,
};

char const *gkm_key_name(int code)
{
	if(code < 0)
		return "(none)";
	if(code == KEYCODE_ACON)
		code = 0x45;
	int row = code >> 4, c = code & 0xf;
	if(row < 1 || row > 9 || c < 1 || c > 6)
		return "?";
	char const *n = names_cg100[6 * (9 - row) + c - 1];
	return n ? n : "?";
}

/* Requested engine hook (cgdoom-kbd.c): the matrix code that produces
   KEY_ESCAPE, defaulting to KEYCODE_MENU. Weak: absent -> address 0. */
extern int CGD_EscapeKeycode __attribute__((weak));

bool gkm_menu_key_configurable(void)
{
	return &CGD_EscapeKeycode != 0;
}

int gkm_menu_key(void)
{
	if(gkm_menu_key_configurable())
		return CG100_BACK;
	return KEYCODE_MENU;
}

/* Engine hook (gint review round 1; cgdoom-kbd.h, defined by the engine
   since the engine follow-ups): the modifier the engine watches while the
   player types a save-game name (digits with it held, letters without).
   Its default KEYCODE_ALPHA (0x71) is SHIFT on the fx-CG100, where ALPHA
   is 0x72. */

/* Engine hook (final review): OK, the select key of every launcher screen,
   also acts as Enter in the Doom menu (unless the keymap binds it to an
   action). Only when BACK is the menu key: otherwise OK is KEY_ESCAPE
   (KEYCODE_MENU). Weak, like CGD_EscapeKeycode. */
extern int CGD_EnterKeycode2 __attribute__((weak));

int gkm_enter_key2(void)
{
	if(gkm_menu_key() != CG100_OK)
		return CG100_OK;
	return -1;
}

void gkm_apply_engine_hooks(void)
{
	if(gkm_menu_key_configurable())
		CGD_EscapeKeycode = gkm_menu_key();
	if(&CGD_EnterKeycode2 != 0)
		CGD_EnterKeycode2 = gkm_enter_key2();
	CGD_AlphaKeycode = CG100_ALPHA;
}

int gkm_find_binding(int16_t const *map, int code, int except)
{
	if(code < 0)
		return -1;
	for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
		if(i != except && map[i] == code)
			return i;
	}
	return -1;
}

int gkm_rebind_check(int code, char const **why)
{
	*why = NULL;
	if(code < 0)
		return GKM_BIND_CANCEL;

	/* The keys that mean "back" in every launcher screen cancel the
	   capture (gint review round 1: pressing BACK/HOME to back out used to
	   bind that key) */
	if(code == CG100_BACK || code == CG100_HOME)
		return GKM_BIND_CANCEL;

	/* AC/ON: SHIFT+7+3+AC/ON is the emergency abort chord (time.c), and
	   bare AC is not a key a game action should sit on */
	if(code == KEYCODE_ACON) {
		*why = "AC/ON cannot be bound (SHIFT+7+3+AC aborts).";
		return GKM_BIND_RESERVED;
	}
	/* The Doom menu key (KEY_ESCAPE) is not part of the keymap: binding
	   it would trigger the menu along with the action. In a real build
	   the cancel keys above already include it (BACK); this only fires
	   when the engine lacks the CGD_EscapeKeycode hook and OK (0x84) stays
	   the menu key.
	   The text names the key (final review: it said "OK" whatever the
	   menu key was). */
	if(code == gkm_menu_key()) {
		static char text[64];
		snprintf(text, sizeof text,
			"%s opens the Doom menu and cannot be bound.",
			gkm_key_name(code));
		*why = text;
		return GKM_BIND_RESERVED;
	}
	return GKM_BIND_OK;
}
