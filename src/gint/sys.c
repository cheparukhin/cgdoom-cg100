/* CGDoom gint platform: model and OS check, OS call table.

   CGDoom runs on the fx-CG100 / Graph Math+ under MPM, whose installer only
   accepts OS 02.00.0202 and 02.00.2202 (both report "02.00.0000"). gint
   knows the syscall-free OS entry points of that OS (CASIOWIN API 2), but
   it compares only the first 5 characters of the version string ("02.00"),
   which wrongly accepts "02.00.0010" (2.00.1, new display, no MPM support),
   and on other versions it falls back to the fx-CG50 syscall gate at
   0x80020070, which does not exist on the fx-CG100.

   gsys_init() therefore checks the model and the full 10-character version
   string at 0x80020020 before any OS call. On anything else, the fx-CG50
   included (the g3a also starts there from the main MENU), main() shows a
   message and exits before any OS call or MMU change. The table holds
   gint's BFile_* wrappers, plus Bfile_GetBlockAddress, which gint does not
   wrap (address from MPM's casiowin.yaml, "BFile_Block"). */

#include "cgd_gint.h"

#include <gint/hardware.h>
#include <gint/bfile.h>
#include <string.h>

char        g_os_version[11] = "??.??.????";
gos_api_t   g_os;
uint32_t    g_entry_r15 = 0;
static bool g_supported = false;

/* Exported by gint's syscalls.S but not declared in any header */
extern int gint_get_CASIOWIN_API(void);

/* Bfile_GetBlockAddress on OS 02.00 */
#define OS200_BFILE_BLOCK       0x80333cf2u

/* gint's BFile_Open() takes two arguments and sets r6=0 itself */
static int gint_open3(uint16_t const *path, int mode, int zero)
{
	(void)zero;
	return BFile_Open(path, mode);
}

void gsys_init(void)
{
	char const *v = (char const *)0x80020020;
	for(int i = 0; i < 10; i++) {
		char c = v[i];
		g_os_version[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	g_os_version[10] = 0;

	memset(&g_os, 0, sizeof g_os);
	g_supported = gint[HWCALC] == HWCALC_FXCG100
		&& !memcmp(g_os_version, "02.00.0000", 10)
		&& gint_get_CASIOWIN_API() == 2;
	if(!g_supported)
		return;

	g_os.open       = gint_open3;
	g_os.close      = BFile_Close;
	g_os.size       = BFile_Size;
	g_os.read       = BFile_Read;
	g_os.write      = BFile_Write;
	g_os.create     = BFile_Create;
	g_os.remove     = BFile_Remove;
	g_os.find_first = BFile_FindFirst;
	g_os.find_next  = BFile_FindNext;
	g_os.find_close = BFile_FindClose;
	g_os.block      = (int (*)(int, int, void **))(uintptr_t)OS200_BFILE_BLOCK;
}

bool gsys_supported(void)
{
	return g_supported;
}

char const *gsys_model_name(void)
{
	switch(gint[HWCALC]) {
	case HWCALC_FXCG100:      return "fx-CG100/Graph Math+";
	case HWCALC_FXCG50:       return "fx-CG50/Graph 90+E";
	case HWCALC_PRIZM:        return "fx-CG10/20 (Prizm)";
	case HWCALC_FXCG_MANAGER: return "fx-CG Manager";
	default:                  return "unknown fx-CG";
	}
}
