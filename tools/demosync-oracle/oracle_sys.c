/* Headless linuxdoom-1.10 demo-sync oracle (GPLv2, like linuxdoom-1.10).
 *
 * Replaces i_main/i_system/i_video/i_sound/i_net with stubs: no window, no
 * sound, a fake clock, one tic per loop (singletics, nothing drawn). Writes
 * a per-tic game-state trace at the end of every G_Ticker (ORACLE_TraceTic,
 * called from the patched g_game.c) in exactly the format of cgdoom-host's
 * -demotrace, to the file named by $ORACLE_TRACE (default stdout).
 * See build.sh and notes/engine-changes.md, "Demo sync". */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "d_main.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_net.h"
#include "p_local.h"
#include "info.h"
#include "m_random.h"
#include "v_video.h"
extern int prndindex;

static FILE *tracef;
static int faketime;

/* ---- i_system ---- */
ticcmd_t emptycmd;
void I_Tactile(int on, int off, int total) {}
ticcmd_t *I_BaseTiccmd(void) { return &emptycmd; }
int I_GetHeapSize(void) { return 64 * 1024 * 1024; }
byte *I_ZoneBase(int *size) { *size = 64 * 1024 * 1024; return calloc(1, *size); }
int I_GetTime(void) { return faketime++; }
void I_Init(void) {}
void I_StartFrame(void) {}
void I_StartTic(void) {}
byte *I_AllocLow(int length) { return calloc(1, length); }
void I_Quit(void)
{
    if (tracef) {
        fprintf(tracef, "END gametic=%d\n", gametic);
        fflush(tracef);
    }
    exit(0);
}
void I_Error(char *error, ...)
{
    va_list ap;
    va_start(ap, error);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, error, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (tracef) { fprintf(tracef, "ERROR gametic=%d\n", gametic); fflush(tracef); }
    exit(1);
}
void I_WaitVBL(int count) {}
void I_BeginRead(void) {}
void I_EndRead(void) {}

/* ---- i_video ---- */
void I_InitGraphics(void)
{
    /* called by D_DoomLoop just before the loop: run headless, one tic
     * per loop iteration (the -timedemo path, without its timing) */
    nodrawers = true;
    singletics = true;
}
void I_ShutdownGraphics(void) {}
void I_SetPalette(byte *palette) {}
void I_UpdateNoBlit(void) {}
void I_FinishUpdate(void) {}
void I_ReadScreen(byte *scr) { memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT); }

/* ---- i_sound ---- */
void I_InitSound() {}
void I_UpdateSound(void) {}
void I_SubmitSound(void) {}
void I_ShutdownSound(void) {}
void I_SetChannels() {}
int I_GetSfxLumpNum(sfxinfo_t *sfx) { return 0; }
int I_StartSound(int id, int vol, int sep, int pitch, int priority) { return id; }
void I_StopSound(int handle) {}
int I_SoundIsPlaying(int handle) { return 0; }
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {}
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) {}
void I_PauseSong(int handle) {}
void I_ResumeSong(int handle) {}
int I_RegisterSong(void *data) { return 1; }
void I_PlaySong(int handle, int looping) {}
void I_StopSong(int handle) {}
void I_UnRegisterSong(int handle) {}

/* ---- i_net ---- */
void I_InitNetwork(void)
{
    doomcom = calloc(1, sizeof(*doomcom));
    doomcom->id = DOOMCOM_ID;
    doomcom->ticdup = 1;
    doomcom->extratics = 0;
    netgame = false;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
}
void I_NetCmd(void) {}

/* ---- trace ---- */
static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

void ORACLE_TraceTic(void)
{
    thinker_t *th;
    uint32_t h = 2166136261u;
    int nmobj = 0;
    player_t *p = &players[consoleplayer];
    mobj_t *mo = p->mo;

    if (!tracef)
        return;
    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        mobj_t *m;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        m = (mobj_t *)th;
        h = mix(h, m->x); h = mix(h, m->y); h = mix(h, m->z);
        h = mix(h, m->health); h = mix(h, (uint32_t)(m->state - states));
        nmobj++;
    }
    fprintf(tracef, "%d gs=%d m=%d%d rnd=%d", gametic, gamestate, gameepisode, gamemap, prndindex);
    if (gamestate == GS_LEVEL && mo)
        fprintf(tracef, " x=%d y=%d z=%d a=%u mx=%d my=%d",
                mo->x, mo->y, mo->z, (unsigned)mo->angle, mo->momx, mo->momy);
    fprintf(tracef, " hp=%d ar=%d am=%d,%d,%d,%d w=%d n=%d h=%08x\n",
            p->health, p->armorpoints, p->ammo[0], p->ammo[1], p->ammo[2], p->ammo[3],
            p->readyweapon, nmobj, h);
}

int main(int argc, char **argv)
{
    const char *t = getenv("ORACLE_TRACE");
    tracef = t ? fopen(t, "w") : stdout;
    myargc = argc;
    myargv = argv;
    D_DoomMain();
    return 0;
}
int mb_used = 2;
