/* CGDoom gint platform: overclocking.

   Levels:
   - GCLK_OFF: leave the OS clock alone.
   - GCLK_F4 (default): gint's CLOCK_SPEED_F4 (235.93 MHz CPU, 58.98 MHz
     bus). Shipped by OutRun on the fx-CG100.
   - GCLK_PTUNE4_F5 (experimental, opt-in with a warning): CalcLoverHK's
     Ptune4 F5 as applied by gpSP-sh4-jit (src/overclock.c) on fx-CG100
     hardware: PLL x32, CPU 1/2 = 235.93 MHz, SuperHyway and SDRAM bus
     1/4 = 117.96 MHz (CL2, TRC6), Pphi 1/16 = 29.49 MHz. Some units show
     bit flips with the faster bus. gpSP writes 0 to CS2BCR/CS2WCR; area 2
     is unused ("don't care" in gint PR 50), so we keep the values the OS had
     instead of writing 0.

   gint saves the OS clock when it starts, restores it on every world switch
   and when the add-in exits (drv_cpg hsave/hrestore), then re-applies ours
   when coming back. We never restore it ourselves. */

#include "cgd_gint.h"

#include <gint/clock.h>

static struct cpg_overclock_setting g_os_setting;
static int g_current = GCLK_OFF;

void gclk_init(void)
{
	cpg_get_overclock_setting(&g_os_setting);
	g_current = GCLK_OFF;
}

void gclk_apply(int level)
{
	if(level == g_current)
		return;

	switch(level) {
	case GCLK_F4:
		/* From F5 (custom), go back to the OS setting first so that
		   clock_set_speed() starts from a known table entry */
		if(g_current == GCLK_PTUNE4_F5)
			cpg_set_overclock_setting(&g_os_setting);
		clock_set_speed(CLOCK_SPEED_F4);
		break;

	case GCLK_PTUNE4_F5: {
		struct cpg_overclock_setting s = {
			.FLLFRQ  = 0x00004384,
			.FRQCR   = 0x1f001103,
			.CS0BCR  = 0x44900400,
			.CS2BCR  = g_os_setting.CS2BCR,
			.CS3BCR  = 0x04904400,
			.CS5aBCR = 0x17df0400,
			.CS0WCR  = 0x000004c0,
			.CS2WCR  = g_os_setting.CS2WCR,
			.CS3WCR  = 0x000024d2,
			.CS5aWCR = 0x000203c1,
		};
		cpg_set_overclock_setting(&s);
		break;
	}

	default:
		level = GCLK_OFF;
		cpg_set_overclock_setting(&g_os_setting);
		break;
	}

	g_current = level;
	gtime_resync();
}

int gclk_current(void)
{
	return g_current;
}

char const *gclk_name(int level)
{
	switch(level) {
	case GCLK_OFF:       return "Off (OS clock)";
	case GCLK_F4:        return "F4: 236 MHz CPU";
	case GCLK_PTUNE4_F5: return "Ptune4 F5 (exp.)";
	default:             return "?";
	}
}
