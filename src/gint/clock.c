/* CGDoom gint platform: overclocking.

   Levels:
   - GCLK_OFF: the OS clock (safe start, and the Diagnostics benchmarks'
     first run).
   - GCLK_F4 (every game): gint's CLOCK_SPEED_F4 (235.93 MHz CPU, the bus
     left at the OS's 58.98 MHz). Shipped by OutRun on the fx-CG100.
   CalcLoverHK's Ptune4 F5 (the same CPU clock with a 117.96 MHz bus) is not
   offered: some units cannot run it, and the game plays normally at F4.

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

	if(level == GCLK_F4)
		clock_set_speed(CLOCK_SPEED_F4);
	else {
		level = GCLK_OFF;
		cpg_set_overclock_setting(&g_os_setting);
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
	case GCLK_OFF: return "OS clock";
	case GCLK_F4:  return "F4: 236 MHz CPU";
	default:       return "?";
	}
}
