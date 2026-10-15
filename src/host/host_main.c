/* Headless host front-end for the CGDoom engine (see docs/DEVELOPMENT.md §3,
   "Host CLI").

   cgdoom-host -iwad <path> [-warp E M | -warp M] [-skill N] [-playdemo LUMP]
               [-tics N] [-input builtin:<name> | <file>] [-crc <file>]
               [-shot <prefix> [-shotevery N]] [-fragment N] [-zone KiB[,KiB]]
               [-frameskip N] [-exitat T,...] [-demos] [-data <dir>] [-stats]
               [-rlimits small] [-lowdetail] [-viewsize N]
               [-autoframeskip N] [-rendercost T] [-ticcost T] [-realtime]
               [-slowwad] [-noasm]
   cgdoom-host -savelayout

   Runs are deterministic: the platform clock advances one tic per presented
   frame, so the same command always renders the same frames. */

#include "platform.h"
#include "host.h"

#include "os.h"
#include "cgdoom.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "g_game.h"
#include "z_zone.h"
#include "p_local.h"
#include "info.h"
#include "m_random.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -features: all, none or a comma-separated list of CGD_FeatureNames */
static int parse_features(const char *list)
{
    if(!strcmp(list, "all")) {
        CGD_Features = CGD_FEATURES_ALL;
        return 0;
    }
    CGD_Features = 0;
    if(!strcmp(list, "none"))
        return 0;
    while(*list) {
        size_t n = strcspn(list, ",");
        int k;
        for(k = 0; k < CGD_FEATURE_COUNT; k++)
            if(strlen(CGD_FeatureNames[k]) == n
                && !strncmp(CGD_FeatureNames[k], list, n))
                break;
        if(k == CGD_FEATURE_COUNT || !(CGD_FEATURES_ALL & (1 << k))) {
            fprintf(stderr, "unknown feature (in this build): %.*s\n",
                (int)n, list);
            return -1;
        }
        CGD_Features |= 1 << k;
        list += n + (list[n] == ',');
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
"usage: cgdoom-host -iwad <file.wad> [options]\n"
"  -warp E M | -warp M   start at a map (E M for Doom, M for Doom II)\n"
"  -skill N              1..5 (default 4, CGDoom's default)\n"
"  -playdemo LUMP        play a demo lump (e.g. DEMO1) and quit at its end\n"
"  -playdemofile NAME    play a demo file from the -data directory\n"
"  -timedemo             play DEMO1..DEMO4 (those present) uncapped, every\n"
"                        tic rendered, and print each one's frame rate\n"
"  -record N             record demo slot N (0..99) to <prefix>D<NN>.LMP,\n"
"                        written at exit when -data is given\n"
"  -wadname NAME         WAD name used for save/demo names (default: the\n"
"                        -iwad base name)\n"
"  -tics N               quit after N game tics\n"
"  -input SPEC           builtin:<name> or a script file (see host_input.c)\n"
"  -crc FILE             write per-frame CRC32 lines ('-' = stdout)\n"
"  -crc565 FILE          write per-frame CRC32 lines of the frame as RGB565\n"
"                        (equal in the 8-bit and true-colour builds; ' F'\n"
"                        marks frames that differ on purpose: spectre fuzz,\n"
"                        after a gamma change, a wipe across a palette\n"
"                        change)\n"
"  -tcexact              true-colour build: light tables from COLORMAP, so\n"
"                        frames equal the 8-bit build's (CGD_TrueColorExact)\n"
"  -features LIST        rendering features (CGD_Features): all, none, or\n"
"                        names separated by commas (default: the build's)\n"
"  -palredraw            8-bit build: redraw the status bar at every palette\n"
"                        change, as the true-colour build does\n"
"                        (CGD_PaletteRedraw; for -tcexact comparisons)\n"
"  -shot PREFIX          write PPM screenshots PREFIX#####.ppm\n"
"  -shotevery N          ... every N frames (default: last frame only)\n"
"  -fragment N           PL_WadMap fails across N-byte blocks (copy path)\n"
"  -zone KiB[,KiB...]    zone sizes (default 16384)\n"
"  -frameskip N          CGDoom frameskip (default 0: render every tic)\n"
"  -poison N             fill new zone blocks with byte N (RANGECHECK builds)\n"
"  -exitat T[,T...]      exit the current level at these tics (intermission,\n"
"                        finale coverage)\n"
"  -demos                play WAD demos in the title loop (CGD_TitleDemos;\n"
"                        the calculator always does)\n"
"  -data DIR             directory for config/saves/demos (default .)\n"
"  -stats                print memory/timing statistics at exit\n"
"  -demotrace FILE       write one line of game state per tic (gametic,\n"
"                        P_Random index, player position/momentum/angle,\n"
"                        health/armor/ammo/weapon, hash of all mobjs) at\n"
"                        the end of each G_Ticker, to compare demo sync\n"
"                        with a vanilla engine ('-' = stdout)\n"
"  -gamelog              log level starts/completions, deaths, changes of\n"
"                        CGD_IsPaused() and the final game state (GAMELOG\n"
"                        lines on stderr)\n"
"  -rlimits small        start the growable renderer arrays tiny (tests\n"
"                        their growth; the output must not change)\n"
"  -lowdetail            low detail (CGD_Options.LowDetail): 160 columns\n"
"  -viewsize N           screenblocks 3..11 (CGD_Options.ViewSize, def. 10)\n"
"  -nodelaysaves         write saves and demos at once (DelaySaves=0)\n"
"  -autoframeskip N      automatic frameskip, at most N frames skipped in a\n"
"                        row (CGD_AutoFrameskip; 0 = fixed -frameskip)\n"
"  -rendercost T         each presented frame advances the virtual clock by\n"
"                        T tics (decimal, default 1: one tic per frame)\n"
"  -ticcost T            each game tic advances it by T tics (default 0)\n"
"  -realtime             wall-clock 35 Hz instead of the virtual clock (not\n"
"                        reproducible; host only)\n"
"  -slowwad              PL_WadIsSlow() returns 1 (copy mapped lumps to the\n"
"                        zone, precache levels; the output must not change)\n"
"  -noasm                accepted for compatibility (host has no asm)\n"
"  -savelayout           print this build's savegame record layout (struct\n"
"                        sizes, index field offsets; tests/savetool.py) and\n"
"                        exit\n"
"builtin input scripts:\n");
    Input_ListBuiltins(stderr);
    exit(1);
}

/* -exitat: tics at which the current level is exited */
static int exit_tics[16];
static int exit_count = 0;

/* -gamelog: game events on stderr (level starts and completions, player
   deaths) and the final game state, to compare runs (e.g. demo sync) */
static int gamelog = 0;
extern boolean demoplayback;   /* g_game.c */
static int log_state = -1, log_map = -1, log_dead = 0, log_idle = -1;

static void game_state_line(const char *what, int tic)
{
    const player_t *p = &players[consoleplayer];
    fprintf(stderr, "GAMELOG %-9s tic=%d gametic=%d map=E%dM%d state=%d "
        "leveltime=%d health=%d armor=%d kills=%d/%d items=%d/%d "
        "secrets=%d/%d demoplayback=%d skill=%d sargtics=%d idle=%d\n", what,
        tic, gametic, gameepisode, gamemap, (int)gamestate, leveltime,
        p->health, p->armorpoints, p->killcount, totalkills, p->itemcount,
        totalitems, p->secretcount, totalsecret, (int)demoplayback,
        (int)gameskill, P_SargRunTics(), CGD_IsPaused());
}

static void gamelog_tic(int tic)
{
    int map = gameepisode * 100 + gamemap;
    int dead = gamestate == GS_LEVEL && players[consoleplayer].playerstate
        == PST_DEAD;
    if(gamestate == GS_LEVEL && (log_state != GS_LEVEL || map != log_map))
        game_state_line("level", tic);
    if(gamestate == GS_INTERMISSION && log_state != GS_INTERMISSION)
        game_state_line("completed", tic);
    if(dead && !log_dead)
        game_state_line("died", tic);
    /* CGD_IsPaused() (the platform's idle signal) changed */
    int idle = CGD_IsPaused();
    if(idle != log_idle)
        game_state_line("idle", tic);
    log_idle = idle;
    log_state = gamestate;
    log_map = map;
    log_dead = dead;
}

/* -demotrace: one line per tic at the end of G_Ticker (G_TicEndHook), in
   the format of the linuxdoom-1.10 oracle used for demo sync
   (tools/demosync-oracle). The format is defined twice, here and in the
   oracle's ORACLE_TraceTic, and the two must stay identical. It only reads
   game state. */
static FILE *demotrace_file = NULL;
extern int prndindex;   /* m_random.c */

static unsigned trace_mix(unsigned h, unsigned v)
{
    return (h ^ v) * 16777619u;
}

static void demotrace_tic(void)
{
    const player_t *p = &players[consoleplayer];
    const mobj_t *mo = p->mo;
    unsigned h = 2166136261u;
    int nmobj = 0;

    for(const thinker_t *th = thinkercap.next; th != &thinkercap;
        th = th->next) {
        if(th->function != TP_MobjThinker)
            continue;
        const mobj_t *m = (const mobj_t *)th;
        h = trace_mix(h, (unsigned)m->x);
        h = trace_mix(h, (unsigned)m->y);
        h = trace_mix(h, (unsigned)m->z);
        h = trace_mix(h, (unsigned)m->health);
        h = trace_mix(h, (unsigned)(m->state - states));
        nmobj++;
    }
    fprintf(demotrace_file, "%d gs=%d m=%d%d rnd=%d", gametic, (int)gamestate,
        gameepisode, gamemap, prndindex);
    if(gamestate == GS_LEVEL && mo)
        fprintf(demotrace_file, " x=%d y=%d z=%d a=%u mx=%d my=%d", mo->x,
            mo->y, mo->z, (unsigned)mo->angle, mo->momx, mo->momy);
    fprintf(demotrace_file, " hp=%d ar=%d am=%d,%d,%d,%d w=%d n=%d h=%08x\n",
        p->health, p->armorpoints, p->ammo[0], p->ammo[1], p->ammo[2],
        p->ammo[3], (int)p->readyweapon, nmobj, h);
}

void Host_TicHook(int tic)
{
    for(int i = 0; i < exit_count; i++) {
        if(exit_tics[i] == tic && gamestate == GS_LEVEL && gameaction ==
            ga_nothing)
            G_ExitLevel();
    }
    if(gamelog)
        gamelog_tic(tic);
}

/* "T" or "T.ddd" (tics, up to 3 decimals) -> 1/1000 tics; -1 if invalid */
static int parse_mtics(const char *s)
{
    int v = 0, scale = 1000;
    if(!s || !*s)
        return -1;
    for(; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if(v > 1000000)
            return -1;
    }
    v *= 1000;
    if(*s == '.') {
        for(s++; *s >= '0' && *s <= '9'; s++) {
            scale /= 10;
            if(scale == 0)
                return -1;
            v += (*s - '0') * scale;
        }
    }
    return *s ? -1 : v;
}

static int is_number(const char *s)
{
    if(!s || !*s)
        return 0;
    if(*s == '-')
        s++;
    for(; *s; s++) {
        if(*s < '0' || *s > '9')
            return 0;
    }
    return 1;
}

/* Base name of the WAD without directory and extension (for save names) */
static const char *wad_basename(const char *path)
{
    static char name[32];
    const char *b = path;
    for(const char *p = path; *p; p++) {
        if(*p == '/' || *p == '\\')
            b = p + 1;
    }
    int i;
    for(i = 0; b[i] && b[i] != '.' && i < 31; i++)
        name[i] = b[i];
    name[i] = 0;
    return name;
}

/* -savelayout: the layout of the records p_saveg.c archives (they are memory
   images of the structs, so it depends on the build), for tests/savetool.py,
   which patches index fields of saves to test G_DoLoadGame */
static void print_save_layout(void)
{
    /* the specials in the order of their class bytes (p_saveg.c) */
    static const int special_size[] = {
        sizeof(ceiling_t), sizeof(vldoor_t), sizeof(floormove_t),
        sizeof(plat_t), sizeof(lightflash_t), sizeof(strobe_t),
        sizeof(glow_t),
    };
    static const int special_sector[] = {
        offsetof(ceiling_t, sector), offsetof(vldoor_t, sector),
        offsetof(floormove_t, sector), offsetof(plat_t, sector),
        offsetof(lightflash_t, sector), offsetof(strobe_t, sector),
        offsetof(glow_t, sector),
    };
    FILE *f = stdout;

#define F(name, type, field) \
    fprintf(f, " %s.%s=%d", name, #field, (int)offsetof(type, field))
    /* header: description, version, skill, episode, map, players,
       leveltime (g_game.c) */
    fprintf(f, "SAVELAYOUT ptr=%d align=%d endian=%s header=%d maxplayers=%d",
        (int)sizeof(void *), (int)(sizeof(void *) > 4 ? sizeof(void *) : 4),
        *(const unsigned char *)&(int){1} ? "little" : "big",
        24 + 16 + 3 + MAXPLAYERS + 3, MAXPLAYERS);
    fprintf(f, " player=%d pspdef=%d", (int)sizeof(player_t),
        (int)sizeof(pspdef_t));
    F("player", player_t, psprites); F("player", player_t, readyweapon);
    F("player", player_t, pendingweapon); F("player", player_t, health);
    F("player", player_t, fixedcolormap);
    fprintf(f, " mobj=%d", (int)sizeof(mobj_t));
    F("mobj", mobj_t, sprite); F("mobj", mobj_t, frame);
    F("mobj", mobj_t, type); F("mobj", mobj_t, state);
    F("mobj", mobj_t, flags); F("mobj", mobj_t, movedir);
    F("mobj", mobj_t, player); F("mobj", mobj_t, lastlook);
    for(int i = 0; i < (int)(sizeof special_size / sizeof *special_size);
        i++)
        fprintf(f, " special%d=%d special%d.sector=%d", i, special_size[i], i,
            special_sector[i]);
    fprintf(f, "\n");
#undef F
}

static void print_stats(void)
{
    const CGDoom_Stats *s = &CGD_Stats;
    int zones = 0;
    for(int i = 0; i < Z_ZoneCount(); i++)
        zones += Z_ZoneSize(i);

    fprintf(stderr, "frames=%d tics=%d gametic=%d\n", host_frames, host_tics,
        gametic);
    fprintf(stderr, "zone: size=%d KiB peak_used=%d KiB peak_static=%d KiB%s\n",
        zones >> 10, s->ZoneUsedPeak >> 10, s->ZoneStaticPeak >> 10,
        sizeof(void *) > 4 ? " (64-bit host: upper bound)" : "");
    fprintf(stderr, "lumps: loaded=%d (%d KiB) referenced=%d (%d KiB) "
        "unaligned-loaded=%d slow-copied=%d (%d KiB) slow-in-place=%d\n",
        s->LumpsLoaded, (int)(s->LumpsLoadedTotal >> 10), s->LumpsReferenced,
        (int)(s->LumpsReferencedTotal >> 10), s->UnalignedLumpsLoaded,
        s->SlowLumpsCopied, (int)(s->SlowLumpsCopiedTotal >> 10),
        s->SlowLumpsInPlace);
    fprintf(stderr, "render limits: peak visplanes=%d drawsegs=%d "
        "vissprites=%d openings=%d; frames over vanilla limits=%d; dropped "
        "walls=%d sprites=%d; arrays %d KiB (peak %d KiB)\n",
        s->VisplanesPeak, s->DrawsegsPeak, s->VisspritesPeak, s->OpeningsPeak,
        s->FramesOverVanillaLimits, s->DrawsegsDropped, s->VisspritesDropped,
        s->RenderArrayBytes >> 10, s->RenderArrayBytesPeak >> 10);
#ifndef PL_SIM
    if(host_frames > 1) {
        double avg = (double)host_frame_us_total / (host_frames - 1);
        fprintf(stderr, "frame time: avg=%.3f ms max=%.3f ms\n", avg / 1000,
            host_frame_us_max / 1000.0);
    }
#endif
}

int main(int argc, char **argv)
{
    const char *iwad = NULL;
    const char *input = "builtin:none";
    const char *crc_path = NULL;
    const char *crc565_path = NULL;
    char *demo = NULL;
    char *demo_file = NULL;
    const char *wadname = NULL;
    int record_slot = -1;
    int warp_episode = 0, warp_map = 0;
    int skill_arg = 4;
    int frameskip = 0;
    int autoframeskip = 0;
    int lowdetail = 0, viewsize = 10, nodelaysaves = 0;
    int demos = 0;
    int stats = 0;

    for(int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if(!strcmp(a, "-iwad") && next)
            iwad = argv[++i];
        else if(!strcmp(a, "-warp") && is_number(next)) {
            warp_episode = 1;
            warp_map = atoi(argv[++i]);
            if(i + 1 < argc && is_number(argv[i + 1])) {
                warp_episode = warp_map;
                warp_map = atoi(argv[++i]);
            }
        }
        else if(!strcmp(a, "-skill") && is_number(next))
            skill_arg = atoi(argv[++i]);
        else if(!strcmp(a, "-playdemo") && next)
            demo = argv[++i];
        else if(!strcmp(a, "-playdemofile") && next)
            demo_file = argv[++i];
        else if(!strcmp(a, "-timedemo"))
            CGD_TimeDemo = 1;
        else if(!strcmp(a, "-record") && is_number(next))
            record_slot = atoi(argv[++i]);
        else if(!strcmp(a, "-wadname") && next)
            wadname = argv[++i];
        else if(!strcmp(a, "-tics") && is_number(next))
            host.max_tics = atoi(argv[++i]);
        else if(!strcmp(a, "-input") && next)
            input = argv[++i];
        else if(!strcmp(a, "-crc") && next)
            crc_path = argv[++i];
        else if(!strcmp(a, "-crc565") && next)
            crc565_path = argv[++i];
        else if(!strcmp(a, "-tcexact"))
            CGD_TrueColorExact = 1;
        else if(!strcmp(a, "-palredraw"))
            CGD_PaletteRedraw = 1;
        else if(!strcmp(a, "-features") && next) {
            if(parse_features(argv[++i]) < 0)
                return 1;
        }
        else if(!strcmp(a, "-shot") && next)
            host.shot_prefix = argv[++i];
        else if(!strcmp(a, "-shotevery") && is_number(next))
            host.shot_every = atoi(argv[++i]);
        else if(!strcmp(a, "-fragment") && is_number(next))
            host.fragment = atoi(argv[++i]);
        else if(!strcmp(a, "-zone") && next) {
            const char *p = argv[++i];
            host.zone_count = 0;
            while(*p && host.zone_count < 8) {
                host.zone_kib[host.zone_count++] = atoi(p);
                while(*p && *p != ',')
                    p++;
                if(*p == ',')
                    p++;
            }
        }
        else if(!strcmp(a, "-exitat") && next) {
            const char *p = argv[++i];
            while(*p && exit_count < 16) {
                exit_tics[exit_count++] = atoi(p);
                while(*p && *p != ',')
                    p++;
                if(*p == ',')
                    p++;
            }
        }
        else if(!strcmp(a, "-poison") && next) {
            int v = (int)strtol(argv[++i], NULL, 0);
#ifdef RANGECHECK
            Z_PoisonValue = v & 0xff;
#else
            (void)v;
            fprintf(stderr, "-poison needs a RANGECHECK build; ignored\n");
#endif
        }
        else if(!strcmp(a, "-frameskip") && is_number(next))
            frameskip = atoi(argv[++i]);
        else if(!strcmp(a, "-demos"))
            demos = 1;
        else if(!strcmp(a, "-data") && next)
            host.data_dir = argv[++i];
        else if(!strcmp(a, "-stats"))
            stats = 1;
        else if(!strcmp(a, "-gamelog"))
            gamelog = 1;
        else if(!strcmp(a, "-demotrace") && next) {
            const char *t = argv[++i];
            demotrace_file = !strcmp(t, "-") ? stdout : fopen(t, "w");
            if(!demotrace_file) {
                fprintf(stderr, "cannot open %s\n", t);
                return 1;
            }
            G_TicEndHook = demotrace_tic;
        }
        else if(!strcmp(a, "-rlimits") && next) {
            const char *v = argv[++i];
            if(!strcmp(v, "small"))
                CGD_SmallRenderLimits = 1;
            else if(strcmp(v, "default")) {
                fprintf(stderr, "-rlimits: small or default\n");
                return 1;
            }
        }
        else if(!strcmp(a, "-lowdetail"))
            lowdetail = 1;
        else if(!strcmp(a, "-nodelaysaves"))
            nodelaysaves = 1;
        else if(!strcmp(a, "-viewsize") && is_number(next)) {
            viewsize = atoi(argv[++i]);
            if(viewsize < 3 || viewsize > 11) {
                fprintf(stderr, "-viewsize must be 3..11\n");
                return 1;
            }
        }
        else if(!strcmp(a, "-autoframeskip") && is_number(next))
            autoframeskip = atoi(argv[++i]);
        else if((!strcmp(a, "-rendercost") || !strcmp(a, "-ticcost"))
            && next) {
            int mt = parse_mtics(argv[++i]);
            if(mt < 0 || (a[1] == 'r' && mt == 0)) {
                fprintf(stderr, "%s: bad value %s\n", a, argv[i]);
                return 1;
            }
            if(a[1] == 'r')
                host.frame_mtics = mt;
            else
                host.tic_mtics = mt;
        }
        else if(!strcmp(a, "-realtime")) {
#ifdef PL_SIM
            fprintf(stderr, "-realtime is not available in the simulator\n");
            return 1;
#else
            host.realtime = 1;
#endif
        }
        else if(!strcmp(a, "-slowwad"))
            host.slow_wad = 1;
        else if(!strcmp(a, "-noasm"))
            {}
        else if(!strcmp(a, "-savelayout")) {
            print_save_layout();
            return 0;
        }
        else {
            fprintf(stderr, "bad argument: %s\n", a);
            usage();
        }
    }

    if(!iwad)
        usage();
    if(skill_arg < 1 || skill_arg > 5) {
        fprintf(stderr, "-skill must be 1..5\n");
        return 1;
    }

    if(crc565_path) {
        host.crc565_file = fopen(crc565_path, "w");
        if(!host.crc565_file) {
            fprintf(stderr, "cannot open %s\n", crc565_path);
            return 1;
        }
    }
    if(crc_path) {
        host.crc_file = !strcmp(crc_path, "-") ? stdout : fopen(crc_path, "w");
        if(!host.crc_file) {
            fprintf(stderr, "cannot open %s\n", crc_path);
            return 1;
        }
    }

    /* Options and keymap must be set before the input script is resolved */
    CGD_DefaultOptions(&CGD_Options);
    CGD_TitleDemos = demos;
    CGD_Options.LowDetail = lowdetail;
    if(nodelaysaves)
        CGD_Options.DelaySaves = 0;
    CGD_Options.ViewSize = viewsize;
    CGD_Frameskip = frameskip;
    CGD_AutoFrameskip = autoframeskip;

    if(Input_Load(input) < 0)
        return 1;

    if(PL_WadOpen(iwad) < 0) {
        fprintf(stderr, "cannot open IWAD %s\n", iwad);
        return 1;
    }
    CGD_WADFileName = wadname ? wadname : wad_basename(iwad);
    CGD_RecordDemoSlot = record_slot;

    skill = skill_arg - 1;
    if(warp_map > 0) {
        startepisode = warp_episode;
        startmap = warp_map;
        autostart = true;
    }
    if(demo) {
        G_DeferedPlayDemo(demo);
        CGD_PlayDemoOnly = 1;
    }
    else if(demo_file) {
        G_DeferedPlayDemoFile(demo_file);
        CGD_PlayDemoOnly = 1;
    }

    D_DoomMain();

    Host_Finish();
    if(CGD_TimeDemo) {
        if(!CGD_TimeDemoCount)
            fprintf(stdout, "timedemo: no demo in the WAD\n");
        for(int i = 0; i < CGD_TimeDemoCount; i++) {
            const CGD_TimeDemoRun *r = &CGD_TimeDemoRuns[i];
            double s = r->micros / 1e6;
            fprintf(stdout, "timedemo %s: %d frames in %.3f s, %.1f fps\n",
                r->name, r->frames, s, s > 0 ? r->frames / s : 0.0);
        }
    }
    if(stats)
        print_stats();
    if(gamelog)
        game_state_line("end", host_tics);
    if(demotrace_file) {
        fprintf(demotrace_file, "END gametic=%d\n", gametic);
        if(demotrace_file != stdout)
            fclose(demotrace_file);
    }

    /* The cgdoom.h order (as on the calculator): close the WAD, then
       write the delayed files. Only touch the file system when asked to. */
    PL_WadClose();
    if(strcmp(host.data_dir, ".") != 0)
        CGD_FlushDelayedWrites();
    if(host.crc_file && host.crc_file != stdout)
        fclose(host.crc_file);
    if(host.crc565_file)
        fclose(host.crc565_file);
    return 0;
}
