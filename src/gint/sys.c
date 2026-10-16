/* CGDoom gint platform: exact model/OS check and OS call table.

   One build accepts OS 02.00.0000 (API 2) and 02.20.0000 (API 3).
   User-visible 02.00.0202/2202 and 02.20.0202 use these internal strings.
   The pinned CG100 runtime in tools/gint selects the table before main(),
   and handles the new LCD controllers during world switches. OS 2.20 needs
   the casio-mpm-220 MPM; it plays on an fx-CG100 with the new display.

   Unknown revisions (including 02.00.0010) and other calculator models are
   refused before platform OS calls. gint wraps the BFile functions except
   Bfile_GetBlockAddress, whose per-version address is selected here. */

#include "cgd_gint.h"

#include <gint/hardware.h>
#include <gint/bfile.h>
#include <string.h>

char        g_os_version[11] = "??.??.????";
gos_api_t   g_os;
uint32_t    g_entry_r15 = 0;
static bool g_supported = false;

/* The CG100 capability alias also rejects an incompatible library at link time. */
extern int gint_cg100_get_CASIOWIN_API(void);

/* Bfile_GetBlockAddress on OS 02.00 */
#define OS200_BFILE_BLOCK       0x80333cf2u
/* OS 02.20.0000: address verified against the hash-pinned firmware image. */
#define OS220_BFILE_BLOCK       0x80398a1eu

/* gint's BFile_Open() takes two arguments and sets r6=0 itself */
static int gint_open3(uint16_t const *path, int mode, int zero)
{
	(void)zero;
	return BFile_Open(path, mode);
}

void gsys_init(void)
{
#ifdef CGD_HOST_TEST
	extern char gsys_test_version[10];
	char const *v = gsys_test_version;
#else
	char const *v = (char const *)0x80020020;
#endif
	for(int i = 0; i < 10; i++) {
		char c = v[i];
		g_os_version[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	g_os_version[10] = 0;

	memset(&g_os, 0, sizeof g_os);
	g_supported = gint[HWCALC] == HWCALC_FXCG100
		&& !memcmp(g_os_version, "02.00.0000", 10)
		&& gint_cg100_get_CASIOWIN_API() == 2;
	bool os220 = gint[HWCALC] == HWCALC_FXCG100
		&& !memcmp(g_os_version, "02.20.0000", 10)
		&& gint_cg100_get_CASIOWIN_API() == 3;
	g_supported = g_supported || os220;
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
	if(os220)
		g_os.block = (int (*)(int, int, void **))(uintptr_t)OS220_BFILE_BLOCK;
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
