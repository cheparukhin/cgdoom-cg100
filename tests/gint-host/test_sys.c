/* Exercise the real startup gate without reading physical firmware memory.
   Neither table selection nor rejection may call an OS routine. */
#include "cgd_gint.h"
#include <gint/hardware.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t gint[16];
char gsys_test_version[10];
static int api, calls;
static uint16_t const *opened_path;
static int opened_mode;
int gint_cg100_get_CASIOWIN_API(void) { return api; }

int BFile_Open(uint16_t const *p, int m)
{ calls++; opened_path = p; opened_mode = m; return 42; }
int BFile_Close(int fd) { calls++; return 0; }
int BFile_Size(int fd) { calls++; return 0; }
int BFile_Read(int fd, void *buf, int size, int whence) { calls++; return 0; }
int BFile_Write(int fd, void const *buf, int size) { calls++; return 0; }
int BFile_Create(uint16_t const *path, int type, int *size) { calls++; return 0; }
int BFile_Remove(uint16_t const *path) { calls++; return 0; }
int BFile_FindFirst(uint16_t const *pattern, int *handle, uint16_t *found,
    struct BFile_FileInfo *info) { calls++; return 0; }
int BFile_FindNext(int handle, uint16_t *found, struct BFile_FileInfo *info)
{ calls++; return 0; }
int BFile_FindClose(int handle) { calls++; return 0; }

static void select_os(int model, char const *version, int runtime_api)
{
    gint[HWCALC] = model;
    memcpy(gsys_test_version, version, 10);
    api = runtime_api;
    calls = 0;
    gsys_init();
    assert(calls == 0);
    assert(g_os_version[10] == 0);
}

static void supported(char const *version, int runtime_api, uintptr_t block)
{
    select_os(HWCALC_FXCG100, version, runtime_api);
    assert(gsys_supported());
    assert(!strcmp(g_os_version, version));
    assert((uintptr_t)g_os.block == block);
    assert(g_os.close == BFile_Close && g_os.size == BFile_Size);
    assert(g_os.read == BFile_Read && g_os.write == BFile_Write);
    assert(g_os.create == BFile_Create && g_os.remove == BFile_Remove);
    assert(g_os.find_first == BFile_FindFirst && g_os.find_next == BFile_FindNext);
    assert(g_os.find_close == BFile_FindClose);
    uint16_t path[] = { 'x', 0 };
    assert(g_os.open(path, BFile_ReadOnly, 0) == 42);
    assert(calls == 1 && opened_path == path && opened_mode == BFile_ReadOnly);
}

int main(void)
{
    supported("02.00.0000", 2, 0x80333cf2u);
    supported("02.20.0000", 3, 0x80398a1eu);
    char const *versions[] = { "02.00.0000", "02.20.0000", "02.10.0000",
        "02.00.0010", "02.20.0010", "01.00.0000", "03.00.0000", "xxxxxxxxxx" };
    int models[] = { HWCALC_FXCG100, HWCALC_FXCG50, HWCALC_PRIZM,
        HWCALC_FXCG_MANAGER, 99 };
    for(unsigned m = 0; m < sizeof models / sizeof *models; m++)
        for(unsigned v = 0; v < sizeof versions / sizeof *versions; v++)
            for(int a = 0; a <= 4; a++) {
                bool expected = models[m] == HWCALC_FXCG100
                    && ((v == 0 && a == 2) || (v == 1 && a == 3));
                /* Rejecting after a successful init must clear stale pointers. */
                supported("02.20.0000", 3, 0x80398a1eu);
                select_os(models[m], versions[v], a);
                assert(gsys_supported() == expected);
                if(!expected) {
                    gos_api_t empty = { 0 };
                    assert(!memcmp(&g_os, &empty, sizeof g_os));
                }
            }
    select_os(HWCALC_FXCG100, "02.20.\0\xff\n0", 3);
    assert(!gsys_supported());
    assert(!strcmp(g_os_version, "02.20.???0"));
    puts("OK (OS/model/API combinations, table reset and open wrapper)");
    return 0;
}
