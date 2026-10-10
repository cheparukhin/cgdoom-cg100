/* Simulator-only measurement hooks for cgdoom-sim.elf (make sim).

   Linked with -Wl,--wrap=main,--wrap=PL_FinishUpdate,--wrap=R_RenderPlayerView,
   --wrap=G_Ticker,--wrap=P_SetupLevel so that neither the engine nor src/host
   needs any sim code.
   Sim-only options are removed from argv before the host's main() sees them:

     -simwin A B   frame window [A, B) for -simstats and the PC profile
                   (frame i = the work that ends with the i-th PL_FinishUpdate;
                   default: every frame). The SIM_PCPROF profile is paused
                   outside the window.
     -simstats     print one SIMFRAMES line (stderr) at exit: per-frame
                   instructions/cycles in the window (frames in which a
                   level was loaded are left out), split into render
                   (R_RenderPlayerView), game (G_Ticker), present
                   (PL_FinishUpdate = CRC/screenshot work of the test
                   platform, excluded from the totals and the profile) and
                   other (status bar, HUD, menus, wipes...)
     -simmarks     sim_mark(frame) at the end of every frame (SIMMARK lines)

   Counters need the sim64 simulator (make sim-tool); without it -simstats
   prints a note and nothing else changes. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sim.h>

struct player_s;

extern int  __real_main(int argc, char **argv);
extern void __real_PL_FinishUpdate(const uint8_t *fb);
extern void __real_R_RenderPlayerView(struct player_s *player);
extern void __real_G_Ticker(void);
extern void __real_P_SetupLevel(int episode, int map, int playermask);

static int win_start = 0, win_end = 0x7fffffff;
static int opt_stats = 0, opt_marks = 0, have_counters = 0;
static int frame = 0;          /* frames completed (PL_FinishUpdate calls) */

/* Counters of the frame in progress, and totals over the counted frames */
struct acc { uint64_t insts, cycles; };
static struct acc cur_render, cur_game, a_frame, a_render, a_game, a_present;
static int cur_load = 0;       /* P_SetupLevel ran during this frame */
static struct sim_counters frame_begin;
static int n_counted = 0, n_load = 0;

#define MAX_FRAMES 16384
static uint32_t frame_cycles[MAX_FRAMES];   /* counted frames, for percentiles */

static inline int in_window(void)
{
    return frame >= win_start && frame < win_end;
}

static inline void now(struct sim_counters *c)
{
    if(have_counters)
        sim_counters(c);
}

static inline void add(struct acc *a, const struct sim_counters *s,
    const struct sim_counters *e)
{
    a->insts += e->insts - s->insts;
    a->cycles += e->cycles - s->cycles;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void report(void)
{
    if(!opt_stats)
        return;
    if(!have_counters) {
        fprintf(stderr, "SIMFRAMES unavailable (needs sh-elf-run64: make sim-tool)\n");
        return;
    }
    int hi = frame < win_end ? frame : win_end;
    int n = n_counted;
    if(n == 0) {
        fprintf(stderr, "SIMFRAMES window=%d..%d frames=0 load_frames=%d\n",
            win_start, hi, n_load);
        return;
    }
    int ns = n < MAX_FRAMES ? n : MAX_FRAMES;
    qsort(frame_cycles, ns, sizeof *frame_cycles, cmp_u32);
    uint64_t other_i = a_frame.insts - a_render.insts - a_game.insts;
    uint64_t other_c = a_frame.cycles - a_render.cycles - a_game.cycles;
    /* Per-frame averages, rounded to whole instructions/cycles */
#define AVG(x) (unsigned long)(((x) + n / 2) / n)
    fprintf(stderr, "SIMFRAMES window=%d..%d frames=%d load_frames=%d"
        " insts=%lu cycles=%lu"
        " render_insts=%lu render_cycles=%lu"
        " game_insts=%lu game_cycles=%lu"
        " other_insts=%lu other_cycles=%lu"
        " present_insts=%lu present_cycles=%lu"
        " min_cycles=%lu median_cycles=%lu p90_cycles=%lu max_cycles=%lu\n",
        win_start, hi, n, n_load,
        AVG(a_frame.insts), AVG(a_frame.cycles),
        AVG(a_render.insts), AVG(a_render.cycles),
        AVG(a_game.insts), AVG(a_game.cycles),
        AVG(other_i), AVG(other_c),
        AVG(a_present.insts), AVG(a_present.cycles),
        (unsigned long)frame_cycles[0], (unsigned long)frame_cycles[ns / 2],
        (unsigned long)frame_cycles[(ns * 9) / 10], (unsigned long)frame_cycles[ns - 1]);
#undef AVG
}

int __wrap_main(int argc, char **argv)
{
    int n = 1;
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "-simwin") && i + 2 < argc) {
            win_start = atoi(argv[i + 1]);
            win_end = atoi(argv[i + 2]);
            if(win_end <= 0)
                win_end = 0x7fffffff;
            i += 2;
        }
        else if(!strcmp(argv[i], "-simstats"))
            opt_stats = 1;
        else if(!strcmp(argv[i], "-simmarks"))
            opt_marks = 1;
        else
            argv[n++] = argv[i];
    }
    argv[n] = NULL;

    struct sim_counters c;
    have_counters = (sim_counters(&c) == 0);
    frame_begin = c;
    /* The profile (if any) only covers the window */
    if(!in_window())
        sim_pcprof(0);
    atexit(report);
    return __real_main(n, argv);
}

void __wrap_PL_FinishUpdate(const uint8_t *fb)
{
    struct sim_counters s, e;
    int was_in = in_window();
    /* The profile leaves out the test platform's CRC/screenshot work */
    if(was_in)
        sim_pcprof(0);
    now(&s);
    __real_PL_FinishUpdate(fb);
    now(&e);

    if(was_in && cur_load)
        n_load++;
    else if(was_in) {
        uint64_t cyc = s.cycles - frame_begin.cycles;
        a_frame.insts += s.insts - frame_begin.insts;
        a_frame.cycles += cyc;
        a_render.insts += cur_render.insts;
        a_render.cycles += cur_render.cycles;
        a_game.insts += cur_game.insts;
        a_game.cycles += cur_game.cycles;
        add(&a_present, &s, &e);
        if(n_counted < MAX_FRAMES)
            frame_cycles[n_counted] = cyc > UINT32_MAX ? UINT32_MAX : (uint32_t)cyc;
        n_counted++;
    }
    if(opt_marks)
        sim_mark(frame);

    frame++;
    cur_render = (struct acc){ 0, 0 };
    cur_game = (struct acc){ 0, 0 };
    cur_load = 0;
    if(in_window())
        sim_pcprof(1);
    now(&frame_begin);
}

void __wrap_R_RenderPlayerView(struct player_s *player)
{
    struct sim_counters s, e;
    now(&s);
    __real_R_RenderPlayerView(player);
    now(&e);
    add(&cur_render, &s, &e);
}

void __wrap_G_Ticker(void)
{
    struct sim_counters s, e;
    now(&s);
    __real_G_Ticker();
    now(&e);
    add(&cur_game, &s, &e);
}

/* Level loads are not frame costs: the frame is left out of the statistics
   and the load itself out of the profile */
void __wrap_P_SetupLevel(int episode, int map, int playermask)
{
    sim_pcprof(0);
    __real_P_SetupLevel(episode, map, playermask);
    cur_load = 1;
    if(in_window())
        sim_pcprof(1);
}
