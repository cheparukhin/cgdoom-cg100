/* Scripted input for the host build.

   A script is a list of segments, one per line:

       <tics> [key ...]       hold these keys for <tics> game tics
       rand <n> <seed>        n pseudo-random movement segments (see below)
       bot <tics> <seed>      a simple deterministic bot for <tics> tics: walks,
                              turns away when blocked (reads the player's
                              position), opens doors, shoots, changes weapons
       # comment

   Keys (case-insensitive):
       fwd back left right sl sr run fire use w1..w7 map pause
       cheat noclip gamma decvp incvp fps frameskip freemem profiler
       menu enter del          (fixed CGDoom keys: MENU, EXE, DEL)
       k:<NAME>                raw physical key, CGDoom technical name
                               (e.g. k:EXE, k:F1, k:ALPHA)
   Doom actions are translated to physical keys through the current keymap
   (CGD_Options.Keymap). A key held over consecutive segments stays down; put
   a segment without it in between to press it again. When the script ends
   it restarts from the beginning. */

#include "host.h"
#include "platform.h"
#include "cgdoom.h"
#include "cgdoom-kbd.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_KEYS 8

typedef struct {
    int tics;
    int nkeys;
    int16_t keys[MAX_KEYS];     /* Doom keys (>0) or -physical code - 1 */
    unsigned bot_seed;          /* bot segment if nonzero */
} Segment;

static Segment *segs = NULL;
static int nsegs = 0, capsegs = 0;
static int total_tics = 0;

//---
// Key names
//---

static const struct { const char *name; int doomkey; } keynames[] = {
    { "fwd",       KEY_UPARROW },
    { "back",      KEY_DOWNARROW },
    { "left",      KEY_LEFTARROW },
    { "right",     KEY_RIGHTARROW },
    { "sl",        KEY_SLEFTARROW },
    { "sr",        KEY_SRIGHTARROW },
    { "run",       KEY_RSHIFT },
    { "fire",      KEY_RCTRL },
    { "use",       ' ' },
    { "w1",        '1' }, { "w2", '2' }, { "w3", '3' }, { "w4", '4' },
    { "w5",        '5' }, { "w6", '6' }, { "w7", '7' },
    { "map",       KEY_TAB },
    { "pause",     KEY_PAUSE },
    { "cheat",     SKEY_CHEAT },
    { "noclip",    SKEY_NOCLIP },
    { "gamma",     SKEY_GAMMA },
    { "decvp",     SKEY_DECVP },
    { "incvp",     SKEY_INCVP },
    { "fps",       SKEY_FPSCOUNTER },
    { "frameskip", SKEY_FRAMESKIP },
    { "freemem",   SKEY_FREEMEM },
    { "profiler",  SKEY_PROFILER },
    { NULL, 0 },
};

/* ASCII case-insensitive equality (strcasecmp is POSIX, not ISO C) */
static int ieq(const char *a, const char *b)
{
    while(*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        a++, b++;
    return *a == 0 && *b == 0;
}

/* Returns the encoded key, or 0 if unknown */
static int parse_key(const char *tok)
{
    if(!strncmp(tok, "k:", 2) || !strncmp(tok, "K:", 2)) {
        char up[16];
        int i;
        for(i = 0; tok[2+i] && i < 15; i++)
            up[i] = toupper((unsigned char)tok[2+i]);
        up[i] = 0;
        int code = CGD_PhysicalKey_FromTechnicalName(up, -1);
        /* "NONE" parses to -1; require a real key */
        if(code < 0 || strcmp(CGD_PhysicalKey_TechnicalName(code), up))
            return 0;
        return -code - 1;
    }
    if(ieq(tok, "menu"))  return -KEYCODE_MENU - 1;
    if(ieq(tok, "enter")) return -KEYCODE_EXE - 1;
    if(ieq(tok, "del"))   return -KEYCODE_DEL - 1;

    for(int i = 0; keynames[i].name; i++) {
        if(ieq(tok, keynames[i].name))
            return keynames[i].doomkey;
    }
    return 0;
}

static int physical(int key)
{
    if(key < 0)
        return -key - 1;
    for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++) {
        if(CGD_KeymapEntries[i] == key)
            return CGD_Options.Keymap[i];
    }
    return -1;
}

//---
// Script building
//---

static void add_segment(int tics, const int *keys, int nkeys)
{
    if(tics <= 0)
        return;
    if(nsegs >= capsegs) {
        capsegs = capsegs ? 2 * capsegs : 64;
        segs = realloc(segs, capsegs * sizeof *segs);
        if(!segs) {
            fprintf(stderr, "out of memory\n");
            exit(2);
        }
    }
    Segment *s = &segs[nsegs++];
    s->tics = tics;
    s->bot_seed = 0;
    s->nkeys = (nkeys > MAX_KEYS) ? MAX_KEYS : nkeys;
    for(int i = 0; i < s->nkeys; i++)
        s->keys[i] = keys[i];
    total_tics += tics;
}

/* Pseudo-random wandering: mostly forward motion with turns, strafes, shots,
   door use and occasional weapon changes. Fully determined by the seed. */
static void add_random(int count, unsigned seed)
{
    uint32_t x = seed * 2654435761u + 12345;
    #define RND() (x = x * 1103515245u + 12345, (x >> 16) & 0x7fff)

    for(int i = 0; i < count; i++) {
        int keys[MAX_KEYS], n = 0;
        int r = RND() % 16;
        int tics = 8 + RND() % 14;

        switch(r) {
        case 0: case 1: case 2:
            keys[n++] = KEY_UPARROW; break;
        case 3: case 4: case 5:
            keys[n++] = KEY_UPARROW; keys[n++] = KEY_LEFTARROW;
            tics = 10 + RND() % 20; break;
        case 6: case 7: case 8:
            keys[n++] = KEY_UPARROW; keys[n++] = KEY_RIGHTARROW;
            tics = 10 + RND() % 20; break;
        case 9:
            keys[n++] = KEY_UPARROW; keys[n++] = KEY_RCTRL; break;
        case 10:
            keys[n++] = KEY_RIGHTARROW; tics = 15 + RND() % 25; break;
        case 11:
            keys[n++] = KEY_LEFTARROW; keys[n++] = KEY_RCTRL;
            tics = 15 + RND() % 25; break;
        case 12:
            keys[n++] = (RND() & 1) ? KEY_SLEFTARROW : KEY_SRIGHTARROW; break;
        case 13:
            keys[n++] = KEY_DOWNARROW; keys[n++] = KEY_RIGHTARROW; break;
        case 14:
            keys[n++] = KEY_UPARROW; keys[n++] = ' '; tics = 4; break;
        case 15:
            keys[n++] = '2' + RND() % 5; tics = 2; break;
        }
        if(RND() % 4 == 0 && n < MAX_KEYS)
            keys[n++] = KEY_RSHIFT;
        add_segment(tics, keys, n);
        /* Release everything for one tic so that keys re-trigger */
        add_segment(1, keys, 0);
    }
    #undef RND
}

static int parse_line(char *line, const char *where, int lineno)
{
    char *tok = strtok(line, " \t\r\n");
    if(!tok || tok[0] == '#')
        return 0;

    if(!strcmp(tok, "rand")) {
        char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n");
        if(!a || !b) {
            fprintf(stderr, "%s:%d: rand <count> <seed>\n", where, lineno);
            return -1;
        }
        add_random(atoi(a), (unsigned)strtoul(b, NULL, 0));
        return 0;
    }

    if(!strcmp(tok, "bot")) {
        char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n");
        if(!a || !b || atoi(a) <= 0) {
            fprintf(stderr, "%s:%d: bot <tics> <seed>\n", where, lineno);
            return -1;
        }
        add_segment(atoi(a), NULL, 0);
        segs[nsegs-1].bot_seed = (unsigned)strtoul(b, NULL, 0) | 0x80000000u;
        return 0;
    }

    int tics = atoi(tok);
    if(tics <= 0) {
        fprintf(stderr, "%s:%d: bad tic count '%s'\n", where, lineno, tok);
        return -1;
    }
    int keys[MAX_KEYS], n = 0;
    while((tok = strtok(NULL, " \t\r\n")) && tok[0] != '#') {
        int k = parse_key(tok);
        if(!k) {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", where, lineno, tok);
            return -1;
        }
        if(n < MAX_KEYS)
            keys[n++] = k;
    }
    add_segment(tics, keys, n);
    return 0;
}

//---
// Bot: deterministic wandering driven by the player's position
//---

static struct {
    uint32_t rng;
    int mode;           /* 0 forward, 1 turn, 2 back off */
    int mode_left;      /* tics left in turn/back modes */
    int turn_key;
    int fire_left;      /* tics of firing left */
    int strafe_left;
    int strafe_key;
    fixed_t px, py;     /* position some tics ago */
    int stuck_tics;
} bot;

static unsigned bot_rand(void)
{
    bot.rng = bot.rng * 1103515245u + 12345;
    return (bot.rng >> 16) & 0x7fff;
}

static void bot_start(unsigned seed)
{
    memset(&bot, 0, sizeof bot);
    bot.rng = seed * 2654435761u + 1;
}

static void bot_keys(int t, uint8_t state[12])
{
    int keys[MAX_KEYS], n = 0;

    /* Outside levels (intermission, finale text, death): press use/fire now
       and then to move on */
    if(gamestate != GS_LEVEL || !players[consoleplayer].mo
        || players[consoleplayer].playerstate != PST_LIVE) {
        if(t % 40 == 20)
            keys[n++] = ' ';
        if(t % 40 == 30)
            keys[n++] = KEY_RCTRL;
        goto done;
    }

    const mobj_t *mo = players[consoleplayer].mo;

    /* Every 4 tics, check how far we went while trying to walk forward */
    if(bot.mode == 0 && t % 4 == 0) {
        fixed_t dx = abs(mo->x - bot.px), dy = abs(mo->y - bot.py);
        bot.stuck_tics = (dx + dy < 6*FRACUNIT) ? bot.stuck_tics + 4 : 0;
        bot.px = mo->x;
        bot.py = mo->y;
    }

    if(bot.mode == 0 && bot.stuck_tics >= 8) {
        /* Blocked: try a door, back off a bit, then turn */
        bot.mode = (bot_rand() % 3 == 0) ? 2 : 1;
        bot.mode_left = (bot.mode == 2) ? 6 : 8 + bot_rand() % 24;
        bot.turn_key = (bot_rand() & 1) ? KEY_LEFTARROW : KEY_RIGHTARROW;
        bot.stuck_tics = 0;
        keys[n++] = ' ';
    }

    switch(bot.mode) {
    case 0:
        keys[n++] = KEY_UPARROW;
        /* Gentle random curves */
        if(bot_rand() % 8 == 0)
            keys[n++] = (bot_rand() & 1) ? KEY_LEFTARROW : KEY_RIGHTARROW;
        break;
    case 1:
        keys[n++] = bot.turn_key;
        if(--bot.mode_left <= 0)
            bot.mode = 0;
        break;
    case 2:
        keys[n++] = KEY_DOWNARROW;
        if(--bot.mode_left <= 0) {
            bot.mode = 1;
            bot.mode_left = 8 + bot_rand() % 24;
        }
        break;
    }

    /* Shoot in bursts, strafe sometimes, change weapons now and then */
    if(bot.fire_left > 0) {
        bot.fire_left--;
        keys[n++] = KEY_RCTRL;
    }
    else if(bot_rand() % 60 == 0)
        bot.fire_left = 5 + bot_rand() % 20;

    if(bot.strafe_left > 0) {
        bot.strafe_left--;
        keys[n++] = bot.strafe_key;
    }
    else if(bot_rand() % 80 == 0) {
        bot.strafe_left = 5 + bot_rand() % 15;
        bot.strafe_key = (bot_rand() & 1) ? KEY_SLEFTARROW : KEY_SRIGHTARROW;
    }

    if(t % 150 == 75 && n < MAX_KEYS)
        keys[n++] = '1' + bot_rand() % 7;
    if(t % 40 == 0 && n < MAX_KEYS)
        keys[n++] = ' ';

done:
    for(int i = 0; i < n; i++)
        PL_KeySet(state, physical(keys[i]));
}

//---
// Builtin scripts
//---

typedef struct {
    const char *name;
    const char *description;
    const char *text;
} Builtin;

static const Builtin builtins[] = {
    { "none", "no input", "1\n" },

    { "explore",
      "god mode + all weapons, then a bot that walks, turns away from walls, "
      "opens doors, shoots and changes weapons",
      "8\n"
      "1 cheat\n"
      "1 run\n"
      "bot 1200 1\n"
      "rand 40 2\n" },

    { "noclip",
      "god mode + noclip: long runs through walls, all weapons fired, "
      "automap, menu, view size and gamma changes",
      "8\n"
      "1 cheat\n"
      "2\n"
      "1 noclip\n"
      "40 fwd run\n"
      "25 left\n"
      "60 fwd run\n"
      "1 w6\n"
      "30 fire\n"
      "1 map\n"
      "50 fwd\n"
      "1 map\n"
      "20 right run\n"
      "50 fwd run sl\n"
      "1 w5\n"
      "25 fire fwd\n"
      "1 decvp\n"
      "2\n"
      "1 decvp\n"
      "40 fwd right\n"
      "1 incvp\n"
      "2\n"
      "1 incvp\n"
      "1 gamma\n"
      "30 fwd run\n"
      "1 menu\n"
      "20\n"
      "1 menu\n"
      "1 w7\n"
      "40 fire left\n"
      "1 gamma\n"
      "2\n"
      "1 gamma\n"
      "2\n"
      "1 gamma\n"
      "2\n"
      "1 gamma\n"
      "60 fwd run\n"
      "35 back sr\n"
      "1 w2\n"
      "30 fire right\n"
      "1 w1\n"
      "20 fire fwd\n"
      "rand 30 7\n" },

    { "walk",
      "no cheats: bot wandering, the player may die (use respawns)",
      "bot 3000 3\n" },

    /* Step 3: renderer settings changed in the game. The menu remembers
       the last item of each menu (lastOn): the second visit goes straight
       to Options > Detail. */
    { "options",
      "god mode; menu Options > Detail (low), view size keys down to 7 and "
      "up to 11 (full screen), automap and menu over a reduced view, "
      "detail back to high, bot walking in between",
      "8\n"
      "1 cheat\n"
      "1 run\n"
      "1 menu\n"
      "3\n"
      "1 back\n"
      "2\n"
      "1 enter\n"
      "3\n"
      "1 back\n"
      "2\n"
      "1 back\n"
      "2\n"
      "1 enter\n"
      "3\n"
      "1 menu\n"
      "2\n"
      "bot 150 1\n"
      "1 decvp\n"
      "2\n"
      "1 decvp\n"
      "2\n"
      "1 decvp\n"
      "bot 150 2\n"
      "1 map\n"
      "40 fwd\n"
      "1 map\n"
      "bot 100 3\n"
      "1 menu\n"
      "20\n"
      "1 menu\n"
      "bot 60 4\n"
      "1 incvp\n"
      "2\n"
      "1 incvp\n"
      "2\n"
      "1 incvp\n"
      "2\n"
      "1 incvp\n"
      "bot 100 5\n"
      "1 map\n"
      "30 fwd\n"
      "1 map\n"
      "bot 60 6\n"
      "1 menu\n"
      "3\n"
      "1 enter\n"
      "3\n"
      "1 enter\n"
      "3\n"
      "1 menu\n"
      "2\n"
      "1 decvp\n"
      "2\n"
      "1 decvp\n"
      "bot 3000 7\n" },

    { "random",
      "no cheats: blind pseudo-random key segments",
      "rand 120 4\n" },

    { NULL, NULL, NULL },
};

void Input_ListBuiltins(FILE *fp)
{
    for(int i = 0; builtins[i].name; i++)
        fprintf(fp, "  builtin:%-8s %s\n", builtins[i].name,
            builtins[i].description);
}

static int load_text(const char *text, const char *where)
{
    char line[256];
    int lineno = 0;
    while(*text) {
        const char *nl = strchr(text, '\n');
        size_t len = nl ? (size_t)(nl - text) : strlen(text);
        if(len >= sizeof line)
            len = sizeof line - 1;
        memcpy(line, text, len);
        line[len] = 0;
        text += nl ? (size_t)(nl - text) + 1 : strlen(text);
        if(parse_line(line, where, ++lineno) < 0)
            return -1;
    }
    return 0;
}

int Input_Load(const char *spec)
{
    nsegs = 0;
    total_tics = 0;

    if(!strncmp(spec, "builtin:", 8)) {
        for(int i = 0; builtins[i].name; i++) {
            if(!strcmp(builtins[i].name, spec + 8))
                return load_text(builtins[i].text, spec);
        }
        fprintf(stderr, "unknown input script '%s'; builtins:\n", spec);
        Input_ListBuiltins(stderr);
        return -1;
    }

    FILE *fp = fopen(spec, "r");
    if(!fp) {
        fprintf(stderr, "cannot open input script '%s'\n", spec);
        return -1;
    }
    char line[256];
    int lineno = 0, rc = 0;
    while(rc == 0 && fgets(line, sizeof line, fp))
        rc = parse_line(line, spec, ++lineno);
    fclose(fp);
    return rc;
}

void Input_GetKeys(int tic, uint8_t state[12])
{
    if(nsegs == 0 || total_tics == 0)
        return;

    /* Scripts loop */
    tic %= total_tics;

    /* Linear search is fine for scripts of a few hundred segments, but keep
       a cursor for the common sequential case */
    static int cur = 0, cur_start = 0;
    if(tic < cur_start || cur >= nsegs) {
        cur = 0;
        cur_start = 0;
    }
    while(cur < nsegs && tic >= cur_start + segs[cur].tics) {
        cur_start += segs[cur].tics;
        cur++;
    }
    if(cur >= nsegs)
        return;

    const Segment *s = &segs[cur];
    if(s->bot_seed) {
        if(tic == cur_start)
            bot_start(s->bot_seed);
        bot_keys(tic - cur_start, state);
        return;
    }
    for(int i = 0; i < s->nkeys; i++)
        PL_KeySet(state, physical(s->keys[i]));
}
