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
int BFile_Open(uint16_t const *path, int mode);
int BFile_Close(int fd);
int BFile_Size(int fd);
int BFile_Read(int fd, void *buf, int size, int whence);
int BFile_Write(int fd, void const *buf, int size);
int BFile_Create(uint16_t const *path, int type, int *size);
int BFile_Remove(uint16_t const *path);
int BFile_FindFirst(uint16_t const *pattern, int *handle, uint16_t *found,
    struct BFile_FileInfo *info);
int BFile_FindNext(int handle, uint16_t *found, struct BFile_FileInfo *info);
int BFile_FindClose(int handle);
#endif
