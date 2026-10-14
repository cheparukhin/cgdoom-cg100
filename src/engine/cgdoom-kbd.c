#include "os.h"
#include "cgdoom.h"
#include "cgdoom-kbd.h"
#include "doomdef.h"

//---
// Keymaps
//---

/* List of Doom keys in their order of assignment in keymaps */
int16_t CGD_KeymapEntries[CGDOOM_KEYMAP_SIZE] = {
    KEY_LEFTARROW,
    KEY_RIGHTARROW,
    KEY_UPARROW,
    KEY_DOWNARROW,
    ' ',                     /* Open doors etc */
    KEY_RCTRL,               /* Shoot */
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    KEY_TAB,
    KEY_PAUSE,
    KEY_SLEFTARROW,
    KEY_SRIGHTARROW,
    KEY_RSHIFT,              /* Speed toggle */

    /* Special CGDoom keys */
    SKEY_DECVP,
    SKEY_INCVP,
    SKEY_CHEAT,
    SKEY_NOCLIP,
    SKEY_GAMMA,
    SKEY_FPSCOUNTER,
};

/* Default keymap: CGDoom 0.03 */
const CGDoom_Keymap CGD_Keymap_CGDoom_0_03 = {
    KEYCODE_LEFT,
    KEYCODE_RIGHT,
    KEYCODE_UP,
    KEYCODE_DOWN,
    KEYCODE_EXE,
    KEYCODE_ALPHA,
    KEYCODE_F1,
    KEYCODE_F2,
    KEYCODE_F3,
    KEYCODE_F4,
    KEYCODE_F5,
    KEYCODE_F6,
    KEYCODE_SHIFT,
    KEYCODE_EXIT,
    KEYCODE_OPTN,
    KEYCODE_0,
    KEYCODE_DOT,
    -1,
    KEYCODE_MINUS,
    KEYCODE_PLUS,
    KEYCODE_POWER,
    KEYCODE_ARROW,
    KEYCODE_FRAC,
    KEYCODE_LEFTP,
};
/* Default keymap: thumbs only */
const CGDoom_Keymap CGD_Keymap_ThumbsOnly = {
    KEYCODE_LEFT,
    KEYCODE_RIGHT,
    KEYCODE_UP,
    KEYCODE_DOWN,
    KEYCODE_SQUARE,
    KEYCODE_ALPHA,
    KEYCODE_F1,
    KEYCODE_F2,
    KEYCODE_F3,
    KEYCODE_F4,
    KEYCODE_F5,
    KEYCODE_F6,
    KEYCODE_7,
    KEYCODE_EXIT,
    KEYCODE_ACON,
    KEYCODE_XOT,
    KEYCODE_LOG,
    KEYCODE_TAN,
    KEYCODE_MINUS,
    KEYCODE_PLUS,
    KEYCODE_COMMA,
    KEYCODE_ARROW,
    KEYCODE_FRAC,
    KEYCODE_LEFTP,
};
/* Default keymap: full-hands */
const CGDoom_Keymap CGD_Keymap_FullHands = {
    KEYCODE_F5,
    KEYCODE_F6,
    KEYCODE_OPTN,
    KEYCODE_SQUARE,
    KEYCODE_MUL,
    KEYCODE_DEL,
    KEYCODE_1,
    KEYCODE_2,
    KEYCODE_3,
    KEYCODE_4,
    KEYCODE_5,
    KEYCODE_6,
    KEYCODE_7,
    KEYCODE_TAN,
    KEYCODE_ACON,
    KEYCODE_ALPHA,
    KEYCODE_POWER,
    KEYCODE_COS,
    KEYCODE_MINUS,
    KEYCODE_PLUS,
    KEYCODE_COMMA,
    KEYCODE_ARROW,
    KEYCODE_FRAC,
    KEYCODE_LEFTP,
};

/* The maps above are the fx-CG50 ones. fx-CG100 / Graph Math+ keymaps live
   in the platform (src/gint/keymaps.c), which copies a preset into
   CGD_Options.Keymap (design.md §8). */

void CGD_CopyKeymap(CGDoom_Keymap dest, const CGDoom_Keymap src)
{
    memcpy(dest, src, sizeof(CGDoom_Keymap));
}

static int DecodeKeymap(const CGDoom_Keymap map, int key)
{
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
        if(CGD_KeymapEntries[i] == key)
            return map[i];
    }
    return -1;
}

//---
// Physical keyboard state (scanned by the platform)
//---

/* Physical key for KEY_ESCAPE when the keymap does not map it (design.md §8:
   gint sets it to BACK on the fx-CG100, where 0x84 is the D-pad's OK key) */
int CGD_EscapeKeycode = KEYCODE_MENU;

/* Second physical key for KEY_ENTER, besides EXE, when the keymap does not
   use it for an action (-1: none). gint sets it to OK (0x84) on the
   fx-CG100, whose launcher selects with OK or EXE (final review). */
int CGD_EnterKeycode2 = -1;

/* ALPHA modifier of save-name entry (gint review §12: the platform sets it
   to 0x72 on the fx-CG100, where 0x71 is SHIFT). Plain int, written by the
   platform through a weak reference before D_DoomMain. */
int CGD_AlphaKeycode = KEYCODE_ALPHA;

static CGDoom_KeyboardState st_prev = { 0 };
static CGDoom_KeyboardState st_now = { 0 };
/* CGDoom (step 2): st_prev != st_now. When the two scans are equal, KeyDown()
   gives the same answer for both and no key was just pressed or released, so
   I_StartTic's ~40 key queries per tic return at once (each one used to
   decode the keymap twice: ~10k instructions per frame). */
static int st_changed = 0;

int KeycodeDown(const uint8_t *state, int code)
{
    return PL_KeyDown(state, code);
}

int CGD_PRGM_Getkey(void)
{
    CGDoom_KeyboardState st;
    PL_ScanKeyboard(st);

    for(int code_y = 9; code_y >= 1; code_y--) {
        for(int code_x = 1; code_x <= 6 - (code_y < 5); code_x++) {
            int code = (code_y << 4) | code_x;
            if(code == 0x45) code = KEYCODE_ACON;

            if(KeycodeDown(st, code))
                return code;
        }
    }
    return -1;
}

//---
// Doom keys
//---

static int DoomKeyToKeycode(int key)
{
    int code = DecodeKeymap(CGD_Options.Keymap, key);
    if(code != -1)
        return code;

    switch(key)
    {
    /* Interface constants */
    case KEY_ESCAPE:        return CGD_EscapeKeycode;
    case KEY_ENTER:         return KEYCODE_EXE;

    /* Alphabetic keys, typed without ALPHA (CGDoom's choice: letters are
       the common case). The table is by matrix code, and it is also the
       fx-CG100's orange ALPHA legends: the fx-CG100 manual (v2.10, key
       sequences such as ALPHA [X,θ,t] (A), ALPHA [Frac] (B), ALPHA [√] (C),
       ALPHA [e^x] (F), ALPHA [7] (M), ALPHA [×] (S), ALPHA [1] (U),
       ALPHA [+] (X), ALPHA [−] (Y)) puts A..F on row 6 (0x61..0x66) and
       M..Z on the digit rows exactly where the fx-CG50 has them; G..L are
       then the six keys of row 5 (0x51..0x56) in reading order, as on the
       fx-CG50. Rows 5 and 6 carry different keys on the two models, but
       the same letters at the same matrix positions, so no per-model table
       is needed (notes/engine-changes.md, "Follow-ups"). */
    case 'A':               return KEYCODE_XOT;
    case 'B':               return KEYCODE_LOG;
    case 'C':               return KEYCODE_LN;
    case 'D':               return KEYCODE_SIN;
    case 'E':               return KEYCODE_COS;
    case 'F':               return KEYCODE_TAN;
    case 'G':               return KEYCODE_FRAC;
    case 'H':               return KEYCODE_FD;
    case 'I':               return KEYCODE_LEFTP;
    case 'J':               return KEYCODE_RIGHTP;
    case 'K':               return KEYCODE_COMMA;
    case 'L':               return KEYCODE_ARROW;
    case 'M':               return KEYCODE_7;
    case 'N':               return KEYCODE_8;
    case 'O':               return KEYCODE_9;
    case 'P':               return KEYCODE_4;
    case 'Q':               return KEYCODE_5;
    case 'R':               return KEYCODE_6;
    case 'S':               return KEYCODE_MUL;
    case 'T':               return KEYCODE_DIV;
    case 'U':               return KEYCODE_1;
    case 'V':               return KEYCODE_2;
    case 'W':               return KEYCODE_3;
    case 'X':               return KEYCODE_PLUS;
    case 'Y':               return KEYCODE_MINUS;
    case 'Z':               return KEYCODE_0;
    case -' ':              return KEYCODE_DOT;
    case KEY_BACKSPACE:     return KEYCODE_DEL;

    /* Numeric keys (handled differently, require a modifier) */
    case -'0':              return KEYCODE_0;
    case -'1':              return KEYCODE_1;
    case -'2':              return KEYCODE_2;
    case -'3':              return KEYCODE_3;
    case -'4':              return KEYCODE_4;
    case -'5':              return KEYCODE_5;
    case -'6':              return KEYCODE_6;
    case -'7':              return KEYCODE_7;
    case -'8':              return KEYCODE_8;
    case -'9':              return KEYCODE_9;

    default:                return -1;
    }
}

/* Keys typed in save-name entry whose meaning depends on the ALPHA
   modifier: digits (with ALPHA), letters and space (without) */
static int IsTextKey(int key)
{
    return (key >= -'9' && key <= -'0') || (key >= 'A' && key <= 'Z')
        || key == -' ';
}

static int KeymapUsesCode(int code)
{
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
        if(CGD_Options.Keymap[i] == code)
            return 1;
    }
    return 0;
}

static int KeyDown(const uint8_t *state, int key)
{
    int code = DoomKeyToKeycode(key);

    /* KEY_ENTER: EXE, or the platform's second Enter key if no action
       is bound to it */
    if(key == KEY_ENTER && CGD_EnterKeycode2 >= 0
        && DecodeKeymap(CGD_Options.Keymap, KEY_ENTER) == -1
        && KeycodeDown(state, CGD_EnterKeycode2)
        && !KeymapUsesCode(CGD_EnterKeycode2))
        return 1;

    /* For alphanumeric keys, watch the status of ALPHA */
    if(key >= -'9' && key <= -'0')
        return KeycodeDown(state, code)
            && KeycodeDown(state, CGD_AlphaKeycode);
    else if((key >= 'A' && key <= 'Z') || key == -' ')
        return KeycodeDown(state, code)
            && !KeycodeDown(state, CGD_AlphaKeycode);
    else
        return KeycodeDown(state, code);
}

void CGD_UpdateKeyboardState(void)
{
	memcpy(st_prev, st_now, sizeof st_now);
	PL_ScanKeyboard(st_now);
	st_changed = memcmp(st_prev, st_now, sizeof st_now) != 0;
}

int CGD_KeyWasJustPressed(int key)
{
    if(!st_changed)
        return 0;
    if(!(!KeyDown(st_prev, key) && KeyDown(st_now, key)))
        return 0;
    /* A text key also needs a press of its own key: releasing ALPHA over a
       held 7 would otherwise type 'M' after the '7' (and pressing ALPHA over
       a held 7 would type '7' after the 'M') */
    if(IsTextKey(key) && KeycodeDown(st_prev, DoomKeyToKeycode(key)))
        return 0;
    return 1;
}

int CGD_KeyWasJustReleased(int key)
{
    if(!st_changed)
        return 0;
    return KeyDown(st_prev, key) && !KeyDown(st_now, key);
}

//---
// Key names and identification
//---

static const char * const display_keynames[] = {
    "F1",      "F2",      "F3",      "F4",      "F5",      "F6",
    "SHIFT",   "OPTN",    "VARS",    "MENU",    "Left",    "Up",
    "ALPHA",   "x^2",     "^",       "EXIT",    "Down",    "Right",
    "X,O,T",   "log",     "ln",      "sin",     "cos",     "tan",
    "Frac",    "S<>D",    "(",       ")",       ",",       "->",
    "7",       "8",       "9",       "DEL",     "AC/ON",   "(None)",
    "4",       "5",       "6",       "x",       "/",       NULL,
    "1",       "2",       "3",       "+",       "-",       NULL,
    "0",       ".",       "x10^x",   "(-)",     "EXE",     NULL,
};
static const char * const technical_keynames[] = {
    "F1",      "F2",      "F3",      "F4",      "F5",      "F6",
    "SHIFT",   "OPTN",    "VARS",    "MENU",    "LEFT",    "UP",
    "ALPHA",   "SQUARE",  "POWER",   "EXIT",    "DOWN",    "RIGHT",
    "XOT",     "LOG",     "LN",      "SIN",     "COS",     "TAN",
    "FRAC",    "FD",      "LEFTP",   "RIGHTP",  "COMMA",   "ARROW",
    "7",       "8",       "9",       "DEL",     "ACON",    "NONE",
    "4",       "5",       "6",       "MUL",     "DIV",     NULL,
    "1",       "2",       "3",       "PLUS",    "MINUS",   NULL,
    "0",       "DOT",     "EXP",     "NEG",     "EXE",     NULL,
};

static const char *KeyName(int key, const char * const *names)
{
    if(key == KEYCODE_ACON)
        key = 0x45;
    if(key <= 0)
        key = 0x46;

    int row = 9 - (key >> 4);
    int col = (key & 0xf) - 1;
    return names[6*row + col];
}

const char *CGD_PhysicalKey_DisplayName(int key)
{
    return KeyName(key, display_keynames);
}

const char *CGD_PhysicalKey_TechnicalName(int key)
{
    return KeyName(key, technical_keynames);
}

int CGD_PhysicalKey_FromTechnicalName(const char *name, int length)
{
    int i, n = sizeof(technical_keynames) / sizeof(technical_keynames[0]);
    if(length < 0)
        length = strlen(name);

    /* Exact match (a prefix such as "F" used to match "F1") */
    for(i = 0; i < n; i++) {
        const char *t = technical_keynames[i];
        if(t && (int)strlen(t) == length && !strncmp(name, t, length)) break;
    }

    int row = 9 - (i / 6);
    int col = (i % 6) + 1;
    int key = (row << 4) | col;

    if(i >= n || key == 0x46)
        key = -1;
    if(key == 0x45)
        key = KEYCODE_ACON;

    return key;
}

struct DoomkeyInfo {
    int key;
    const char *technical_name;
    const char *display_name;
};

static const struct DoomkeyInfo doomkeys[] = {
    { KEY_LEFTARROW,     "Left",                "Look left"         },
    { KEY_RIGHTARROW,    "Right",               "Look right"        },
    { KEY_UPARROW,       "Up",                  "Move forward"      },
    { KEY_DOWNARROW,     "Down",                "Move backward"     },
    { ' ',               "Inteact",             "Interact"          },
    { KEY_RCTRL,         "Shoot",               "Shoot"             },
    { '1',               "Weapon1",             "1-Hand"            },
    { '2',               "Weapon2",             "2-Pistol"          },
    { '3',               "Weapon3",             "3-Shotgun"         },
    { '4',               "Weapon4",             "4-Chaingun"        },
    { '5',               "Weapon5",             "5-Rockets"         },
    { '6',               "Weapon6",             "6-Plasma"          },
    { '7',               "Weapon7",             "7-BFG9000"         },
    { KEY_TAB,           "Map",                 "Map"               },
    { KEY_PAUSE,         "Pause",               "Pause"             },
    { KEY_SLEFTARROW,    "StrafeLeft",          "Strafe left"       },
    { KEY_SRIGHTARROW,   "StrafeRight",         "Strafe right"      },
    { KEY_RSHIFT,        "Run",                 "Toggle run"        },
    { SKEY_DECVP,        "DecreaseViewport",    "Smaller view"      },
    { SKEY_INCVP,        "IncreaseViewport",    "Larger view"       },
    { SKEY_CHEAT,        "Cheat",               "Cheat"             },
    { SKEY_NOCLIP,       "ToggleNoclip",        "Noclip"            },
    { SKEY_GAMMA,        "GammaCorrection",     "Brightness"        },
    { SKEY_FPSCOUNTER,   "FPSCounter",          "FPS counter"       },
    { -1,                NULL,                  NULL                },
};

const char *CGD_DoomKey_DisplayName(int key)
{
    for(int i = 0; doomkeys[i].key >= 0; i++) {
        if(doomkeys[i].key == key)
            return doomkeys[i].display_name;
    }
    return "(None)";
}

const char *CGD_DoomKey_TechnicalName(int key)
{
    for(int i = 0; doomkeys[i].key >= 0; i++) {
        if(doomkeys[i].key == key)
            return doomkeys[i].technical_name;
    }
    return "NONE";
}

int CGD_DoomKey_FromTechnicalName(const char *name, int length)
{
    if(length < 0)
        length = strlen(name);

    /* Exact match (a prefix such as "Weapon" used to match "Weapon1") */
    for(int i = 0; doomkeys[i].key >= 0; i++) {
        const char *t = doomkeys[i].technical_name;
        if((int)strlen(t) == length && !strncmp(t, name, length))
            return doomkeys[i].key;
    }
    return -1;
}
