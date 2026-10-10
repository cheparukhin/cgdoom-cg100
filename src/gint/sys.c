/* CGDoom gint platform: model and OS detection, OS call table.

   gint (dev) knows the syscall-free OS entry points of the fx-CG100 only for
   OS 01.00 (CASIOWIN API 1) and 02.00 (API 2); on any other version it falls
   back to the fx-CG50 syscall gate at 0x80020070, which does not exist on
   the fx-CG100. gint also compares only the first 5 characters ("02.00"),
   which wrongly accepts "02.00.0010" (2.00.1, new display, no MPM support).

   This file therefore checks the full 10-character version string at
   0x80020020 and builds a table of the BFile functions used by the platform:
   - fx-CG100 "02.00.0000" (02.00.0202 / 02.00.2202): the only firmware MPM
     can be installed on today; gint's BFile_* wrappers (API 2);
   - fx-CG100 "01.00.0000": gint API 1, experimental (no MPM installer);
   - fx-CG100 "02.10.0000": direct addresses from MPM's casiowin.yaml (main,
     cbd1a41), experimental (no MPM installer yet);
   - fx-CG50 family: gint's BFile_* wrappers over the syscall table;
   - anything else: no file access (main() shows a message and exits before
     any OS call).
   Bfile_GetBlockAddress is not wrapped by gint: fx-CG100 addresses from MPM
   (BFile_Block), fx-CG50 syscall 0x1DAA through cg50_syscall.S. */

#include "cgd_gint.h"

#include <gint/hardware.h>
#include <gint/bfile.h>
#include <string.h>

bool        g_is_cg100 = false;
gos_kind_t  g_os_kind = GOS_UNSUPPORTED;
char        g_os_version[11] = "??.??.????";
gos_api_t   g_os;
uint32_t    g_entry_r15 = 0;

/* Exported by gint's syscalls.S but not declared in any header */
extern int gint_get_CASIOWIN_API(void);

/* fx-CG50: syscall 0x1DAA trampoline (cg50_syscall.S) */
extern int cgd_cg50_get_block_address(int fd, int offset, void **ptr);

//---
// fx-CG100 OS 02.10 entry points (MPM casiowin.yaml, third column).
// Every entry is exactly +0x62A68 from the OS 02.00 address used by gint,
// which cross-checks the table.
//
// BFile_Write: MPM's yaml says 0x80396a92 but libsyscall/NESizm (and
// Kin1009's gint fork) say 0x80396a06 = 0x80333f9e + 0x62A68. We use
// 0x80396a06 for consistency with every other entry. UNVERIFIED on hardware.
//---

#define OS210_BFILE_REMOVE      0x80396c7au
#define OS210_BFILE_CREATE      0x80396958u
#define OS210_BFILE_OPEN        0x80396338u
#define OS210_BFILE_CLOSE       0x803964b6u
#define OS210_BFILE_SIZE        0x8039656cu
#define OS210_BFILE_WRITE       0x80396a06u   /* unverified, see above */
#define OS210_BFILE_READ        0x8039682au
#define OS210_BFILE_BLOCK       0x8039675au
#define OS210_BFILE_FINDFIRST   0x80397030u
#define OS210_BFILE_FINDNEXT    0x803972aeu
#define OS210_BFILE_FINDCLOSE   0x803973b8u

/* Bfile_GetBlockAddress ("BFile_Block" in MPM) */
#define OS100_BFILE_BLOCK       0x8023ffb2u
#define OS200_BFILE_BLOCK       0x80333cf2u

typedef int (*open_fn)(uint16_t const *, int, int);
typedef int (*fd_fn)(int);
typedef int (*read_fn)(int, void *, int, int);
typedef int (*write_fn)(int, void const *, int);
typedef int (*create_fn)(uint16_t const *, int, int *);
typedef int (*path_fn)(uint16_t const *);
typedef int (*ff_fn)(uint16_t const *, int *, uint16_t *,
	struct BFile_FileInfo *);
typedef int (*fn_fn)(int, uint16_t *, struct BFile_FileInfo *);
typedef int (*block_fn)(int, int, void **);

#define OSFN(type, addr) ((type)(uintptr_t)(addr))

/* gint's BFile_Open() takes two arguments and sets r6=0 itself */
static int gint_open3(uint16_t const *path, int mode, int zero)
{
	(void)zero;
	return BFile_Open(path, mode);
}

static void use_gint_bfile(void)
{
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
}

static void use_os210_table(void)
{
	g_os.open       = OSFN(open_fn,   OS210_BFILE_OPEN);
	g_os.close      = OSFN(fd_fn,     OS210_BFILE_CLOSE);
	g_os.size       = OSFN(fd_fn,     OS210_BFILE_SIZE);
	g_os.read       = OSFN(read_fn,   OS210_BFILE_READ);
	g_os.write      = OSFN(write_fn,  OS210_BFILE_WRITE);
	g_os.create     = OSFN(create_fn, OS210_BFILE_CREATE);
	g_os.remove     = OSFN(path_fn,   OS210_BFILE_REMOVE);
	g_os.find_first = OSFN(ff_fn,     OS210_BFILE_FINDFIRST);
	g_os.find_next  = OSFN(fn_fn,     OS210_BFILE_FINDNEXT);
	g_os.find_close = OSFN(fd_fn,     OS210_BFILE_FINDCLOSE);
	g_os.block      = OSFN(block_fn,  OS210_BFILE_BLOCK);
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
	g_is_cg100 = (gint[HWCALC] == HWCALC_FXCG100);
	int api = gint_get_CASIOWIN_API();

	if(!g_is_cg100) {
		/* fx-CG50 family: syscalls. gint picks API 1 for any OS string
		   starting with "01.00" regardless of the model (very old
		   Prizm OS); refuse that combination instead of jumping to
		   fx-CG100 addresses. */
		if(api != 0) {
			g_os_kind = GOS_UNSUPPORTED;
			return;
		}
		g_os_kind = GOS_SYSCALL;
		use_gint_bfile();
		g_os.block = cgd_cg50_get_block_address;
		return;
	}

	if(!memcmp(g_os_version, "01.00.0000", 10) && api == 1) {
		g_os_kind = GOS_CG100_100;
		use_gint_bfile();
		g_os.block = OSFN(block_fn, OS100_BFILE_BLOCK);
	}
	else if(!memcmp(g_os_version, "02.00.0000", 10) && api == 2) {
		g_os_kind = GOS_CG100_200;
		use_gint_bfile();
		g_os.block = OSFN(block_fn, OS200_BFILE_BLOCK);
	}
	else if(!memcmp(g_os_version, "02.10.0000", 10)) {
		/* gint selected API 0 (syscall gate): never use gint's BFile
		   wrappers here, only the table */
		g_os_kind = GOS_CG100_210;
		use_os210_table();
	}
	else {
		g_os_kind = GOS_UNSUPPORTED;
	}
}

bool gsys_files_supported(void)
{
	return g_os_kind != GOS_UNSUPPORTED;
}

bool gsys_os_experimental(void)
{
	return g_os_kind == GOS_CG100_100 || g_os_kind == GOS_CG100_210;
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

char const *gsys_os_path_name(void)
{
	switch(g_os_kind) {
	case GOS_SYSCALL:     return "OS syscalls";
	case GOS_CG100_200:   return "gint API 2 (OS 2.00)";
	case GOS_CG100_100:   return "gint API 1 (OS 1.00, exp.)";
	case GOS_CG100_210:   return "own table (OS 2.10, exp.)";
	default:              return "unsupported";
	}
}

bool gsys_cg50_extra_ram_allowed(void)
{
	/* Same gate as CGDoom (cgdoom.c): OS 3.60 or earlier */
	return gint[HWCALC] == HWCALC_FXCG50
		&& !memcmp(g_os_version, "03.", 3)
		&& g_os_version[3] <= '6';
}
