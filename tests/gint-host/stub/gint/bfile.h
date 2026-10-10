/* tests/gint-host: stub of <gint/bfile.h> (only what src/gint uses) */
#ifndef STUB_BFILE
#define STUB_BFILE
#include <stdint.h>
#define BFile_File 1
#define BFile_ReadOnly 0x01
#define BFile_WriteOnly 0x02
#define BFile_EntryNotFound -1
#define BFile_Type_Directory 0
#define BFile_Type_File 1
#define BFile_Type_Dot 8
#define BFile_Type_DotDot 9
#define BFile_Type_Volume 10
struct BFile_FileInfo { uint16_t index, type; uint32_t file_size, data_size, property; void *address; };
#endif
