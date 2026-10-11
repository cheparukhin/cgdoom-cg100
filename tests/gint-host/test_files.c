/* Host unit test for src/gint/files.c and src/gint/wad.c (RAM image, BFile
   modes, delayed writes, 8.3 mapping) against a simulated Fugue/BFile.
   Usage: test_files [file.wad...] (WAD tests are skipped without files).
   gint review round 1: failed writes leave no partial file behind, and the
   first failure is reported by name (gfs_last_failure).
   gint review round 2: files.c is #included with malloc interposed, to
   drive PL_FileWrite()'s no-memory path; every failure of the session is
   reported at exit (gfs_failure_text), not only the exit flush's.
   A replacement with a file of another size that fails (storage full, write
   error) puts the previous version back from a RAM backup, and the exit
   text says so (test_replace_rollback, test_flush_mixed); the simulated
   storage has a capacity for that. */
#include "cgd_gint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* malloc() of files.c fails above this size (-1: never); every call is
   counted, with the largest size asked for */
static int g_malloc_fail_over = -1;
static int g_malloc_calls = 0;
static size_t g_malloc_largest = 0;
static void *t_malloc(size_t n)
{
	g_malloc_calls++;
	if(n > g_malloc_largest)
		g_malloc_largest = n;
	if(g_malloc_fail_over >= 0 && n > (size_t)g_malloc_fail_over)
		return NULL;
	return malloc(n);
}
#define malloc t_malloc
#include "files.c"
#undef malloc

char g_os_version[11] = "02.00.0000";
gos_api_t g_os;
uint32_t g_entry_r15;
gopt_t g_opt;
gtlb_stats_t gtlb_stats;
void gvid_release(void) {}
void gvid_note_os_activity(void) {}
void gtlb_repair(void) {}
void ui_status(char const *a, char const *b, char const *c) { (void)a; (void)b; (void)c; }
size_t gmem_total_free(void) { return 64u << 20; }
size_t gmem_largest_free(void) { return 64u << 20; }
void PL_Log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  [log] "); vprintf(fmt, a); va_end(a); }
/* main.c's load-phase quit poll (round 3.2), called by wad.c */
void gquit_poll_load(void) {}

/* ---- simulated file system ---- */
typedef struct { char name[64]; uint8_t *data; int size; bool used; } sfile_t;
static sfile_t fs[32];
typedef struct { int file; int pos; bool open; int mode; } sfd_t;
static sfd_t fds[16];
static int os_calls;
/* Failure injection: create() fails; write() fails after n successes
   (fail_write_after), fail_write_count times (-1: from then on); remove()
   fails and leaves the file */
static int fail_create = 0;
static int fail_write_after = -1;
static int fail_write_count = -1;
static int fail_remove = 0;
/* The next n create() calls fail but leave an empty entry behind */
static int fail_create_entry = 0;
/* Storage capacity in bytes (-1: unlimited): create() and writes that grow
   a file fail with -4 once the files would not fit */
static int fs_capacity = -1;
static int fs_used(void);
static void add_file(char const *name, void const *data, int size);

static void path_to_name(uint16_t const *p, char *out)
{
	/* skip \\fls0\ */
	int i = 7, n = 0;
	while(p[i]) out[n++] = (char)p[i++];
	out[n] = 0;
}
static int find_file(char const *name)
{
	for(int i = 0; i < 32; i++) if(fs[i].used && !strcasecmp(fs[i].name, name)) return i;
	return -1;
}
static int s_open(uint16_t const *path, int mode, int zero)
{
	(void)zero; os_calls++;
	char n[64]; path_to_name(path, n);
	int f = find_file(n); if(f < 0) return BFile_EntryNotFound;
	for(int i = 0; i < 16; i++) if(!fds[i].open) { fds[i] = (sfd_t){ f, 0, true, mode }; return 3 + i; }
	return -12;
}
static int s_close(int fd) { os_calls++; fds[fd-3].open = false; return 0; }
static int s_size(int fd) { os_calls++; return fs[fds[fd-3].file].size; }
static int s_read(int fd, void *buf, int size, int whence)
{
	os_calls++;
	sfd_t *d = &fds[fd-3]; sfile_t *f = &fs[d->file];
	if(whence >= 0) d->pos = whence;
	int n = size; if(d->pos + n > f->size) n = f->size - d->pos; if(n < 0) n = 0;
	memcpy(buf, f->data + d->pos, n); d->pos += n;
	return n;   /* Fugue: bytes read */
}
static int s_write(int fd, void const *buf, int size)
{
	os_calls++;
	sfd_t *d = &fds[fd-3]; sfile_t *f = &fs[d->file];
	if(!(d->mode & BFile_WriteOnly)) return -8;
	if(fail_write_after == 0) {
		if(fail_write_count > 0 && --fail_write_count == 0) fail_write_after = -1;
		return -5;
	}
	if(fail_write_after > 0) fail_write_after--;
	if(d->pos + size > f->size) {
		if(fs_capacity >= 0 && fs_used() + d->pos + size - f->size > fs_capacity) return -4;
		f->data = realloc(f->data, d->pos + size); f->size = d->pos + size;
	}
	memcpy(f->data + d->pos, buf, size); d->pos += size;
	return size;
}
static int s_create(uint16_t const *path, int type, int *size)
{
	(void)type; os_calls++;
	if(fail_create) return -4;   /* BFile_DeviceFull-like */
	char n[64]; path_to_name(path, n);
	if(find_file(n) >= 0) return -13;
	if(fail_create_entry > 0) {
		fail_create_entry--;
		add_file(n, "", 0);
		return -4;
	}
	if(fs_capacity >= 0 && fs_used() + *size > fs_capacity) return -4;
	for(int i = 0; i < 32; i++) if(!fs[i].used) {
		fs[i].used = true; strcpy(fs[i].name, n); fs[i].size = *size;
		fs[i].data = malloc(*size + 1); memset(fs[i].data, 0xff, *size); return 0; }
	return -12;
}
static int s_remove(uint16_t const *path)
{
	os_calls++;
	char n[64]; path_to_name(path, n);
	int f = find_file(n); if(f < 0) return BFile_EntryNotFound;
	if(fail_remove) return -7;
	free(fs[f].data); fs[f].used = false; return 0;
}
static int fs_used(void)
{
	int u = 0;
	for(int i = 0; i < 32; i++) if(fs[i].used) u += fs[i].size;
	return u;
}
static char pat_ext[8]; static int pat_pos;
static int next_match(uint16_t *found, struct BFile_FileInfo *info)
{
	for(; pat_pos < 32; pat_pos++) {
		sfile_t *f = &fs[pat_pos]; if(!f->used) continue;
		char const *e = strrchr(f->name, '.'); if(!e || strcasecmp(e, pat_ext)) continue;
		int i = 0; for(; f->name[i]; i++) found[i] = (uint8_t)f->name[i]; found[i] = 0;
		info->type = BFile_Type_File; info->file_size = f->size; pat_pos++; return 0;
	}
	return -16;
}
static int s_ff(uint16_t const *pattern, int *sh, uint16_t *found, struct BFile_FileInfo *info)
{
	os_calls++;
	char n[64]; path_to_name(pattern, n);   /* "*.wad" */
	strcpy(pat_ext, n + 1); pat_pos = 0; *sh = 1;
	int rc = next_match(found, info); return rc < 0 ? BFile_EntryNotFound : 0;
}
static int s_fn(int sh, uint16_t *found, struct BFile_FileInfo *info) { (void)sh; os_calls++; return next_match(found, info); }
static int s_fc(int sh) { (void)sh; os_calls++; return 0; }

static void add_file(char const *name, void const *data, int size)
{
	for(int i = 0; i < 32; i++) if(!fs[i].used) {
		fs[i].used = true; strcpy(fs[i].name, name); fs[i].size = size;
		fs[i].data = malloc(size + 1); memcpy(fs[i].data, data, size); return; }
}

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

static uint32_t le32(uint8_t const *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void test_wad(char const *path)
{
	FILE *fp = fopen(path, "rb"); if(!fp) { printf("skip %s\n", path); return; }
	fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
	uint8_t *data = malloc(sz); fread(data, 1, sz, fp); fclose(fp);
	int old = find_file("TEST.WAD");
	if(old >= 0) { free(fs[old].data); fs[old].used = false; }
	add_file("TEST.WAD", data, (int)sz);

	g_opt.wad_in_ram = 1;
	int rc = PL_WadOpen("test.wad");
	CHECK(rc == sz, "open %d", rc);
	CHECK(gwad_stats.mode == GWAD_RAM, "mode %d", gwad_stats.mode);
	printf("%s: %ld bytes, RAM image %d KiB, %d segments, %d skipped (%d KiB)\n", path, sz,
		gwad_stats.ram_bytes >> 10, gwad_stats.ram_segments, gwad_stats.ram_skipped,
		gwad_stats.ram_skipped_bytes >> 10);

	uint32_t n = le32(data + 4), dir = le32(data + 8);
	int mapped = 0, unmapped = 0;
	for(uint32_t i = 0; i < n; i++) {
		uint8_t const *e = data + dir + 16*i;
		uint32_t pos = le32(e), size = le32(e + 4);
		if(!size) continue;
		/* the engine maps lump + 136 pad bytes, clamped to the WAD end */
		uint32_t padded = size + 136;
		if(padded > sz - pos) padded = (sz - pos > size) ? sz - pos : size;
		uint8_t const *m = PL_WadMap(pos, padded);
		if(m) {
			mapped++;
			CHECK(((uintptr_t)m & 3) == 0, "lump %u unaligned", i);
			CHECK(!memcmp(m, data + pos, padded), "lump %u content+pad", i);
			for(uint32_t k = padded; k < size + 136; k++)
				CHECK(m[k] == 0, "lump %u zero tail", i);
			CHECK(PL_IsWadPointer(m), "lump %u not a WAD pointer", i);
		} else {
			unmapped++;
			char nm[9] = {0}; memcpy(nm, e + 8, 8);
			CHECK(nm[0] == 'D' || !strncmp(nm, "GENMIDI", 7), "lump %s not mapped", nm);
			uint8_t *buf = malloc(size);
			CHECK(PL_WadRead(buf, pos, size) == (int)size, "read %s", nm);
			CHECK(!memcmp(buf, data + pos, size), "read content %s", nm);
			free(buf);
		}
	}
	/* random ranges */
	srand(1);
	uint8_t *buf = malloc(1 << 20);
	for(int k = 0; k < 3000; k++) {
		int off = rand() % (int)sz, len = rand() % (k < 2000 ? 4096 : (1 << 20));
		if(off + len > sz) len = sz - off;
		int got = PL_WadRead(buf, off, len);
		CHECK(got == len, "range %d+%d got %d", off, len, got);
		if(got == len) CHECK(!memcmp(buf, data + off, len), "range content %d+%d", off, len);
	}
	int r = PL_WadRead(buf, (int)sz - 10, 100);
	CHECK(r == 10, "clamped read %d", r);
	CHECK(PL_WadMap((int)sz - 4, 8) == NULL, "map past end");
	CHECK(!PL_IsWadPointer(buf), "heap ptr");
	free(buf);
	printf("  mapped %d, unmapped %d, fallback reads %d, os calls %d\n", mapped, unmapped,
		gwad_stats.fallback_reads, os_calls);
	/* Final review: gwad_has_lump() (run_game's start-map check) finds
	   every directory name, case-insensitively, and nothing else */
	{ char first[9] = {0}, last[9] = {0}, low[9];
	  memcpy(first, data + dir + 8, 8); memcpy(last, data + dir + 16 * (n - 1) + 8, 8);
	  for(int k = 0; k < 9; k++) low[k] = (char)tolower((unsigned char)first[k]);
	  CHECK(gwad_has_lump(first) && gwad_has_lump(last) && gwad_has_lump(low),
		"has_lump %s %s %s", first, last, low);
	  CHECK(!gwad_has_lump("ZZZZZZZZ") && !gwad_has_lump("MAP33") && !gwad_has_lump("E5M1"),
		"has_lump: missing lumps");
	  CHECK(gwad_has_lump("MAP01") != gwad_has_lump("E1M1"), "MAP01 xor E1M1"); }
	PL_WadClose();

	/* BFile mode: no RAM image and no block map (g_os.block is NULL) */
	g_opt.wad_in_ram = 0;
	rc = PL_WadOpen("TEST.WAD");
	CHECK(rc == sz && gwad_stats.mode == GWAD_BFILE, "bfile open");
	uint8_t hdr[12]; CHECK(PL_WadRead(hdr, 0, 12) == 12 && !memcmp(hdr, data, 12), "bfile hdr");
	CHECK(PL_WadMap(0, 12) == NULL, "bfile map");
	PL_WadClose();
	g_opt.wad_in_ram = 1;
	free(data);
}

static char const *appender(void) { return "CG.Test=1\n"; }

static void test_files(void)
{
	char buf[256];
	gfs_settings_name = "CGDoom.cfg";
	gfs_settings_appender = appender;
	CHECK(PL_FileWrite("CGDoom.cfg", "Version=1\n", 10) == 10, "cfg write");
	gfs_settings_appender = NULL;
	CHECK(PL_FileSize("cgdoom.cfg") == 20, "cfg size %d", PL_FileSize("cgdoom.cfg"));
	int n = PL_FileRead("CGDoom.cfg", buf, sizeof buf);
	CHECK(n == 20 && !memcmp(buf, "Version=1\nCG.Test=1\n", 20), "cfg read");
	CHECK(find_file("CGDoom.cfg") < 0, "not written yet");

	char demo[300]; memset(demo, 7, sizeof demo);
	CHECK(PL_FileWrite("doom1_demo00.lmp", demo, 300) < 0, "non-8.3 rejected");
	CHECK(PL_FileWrite("DOOM1D00.LMP", demo, 300) == 300, "demo write");
	CHECK(PL_FileSize("doom1d00.lmp") == 300, "demo size pending");
	CHECK(PL_FileWrite("doom1_0.dsg", demo, 100) == 100, "save write");
	int calls = os_calls;
	CHECK(gfs_flush() == 0, "flush");
	printf("flush: %d os calls\n", os_calls - calls);
	CHECK(find_file("CGDoom.cfg") >= 0 && fs[find_file("CGDoom.cfg")].size == 20, "cfg on disk");
	CHECK(find_file("DOOM1D00.LMP") >= 0, "demo on disk");
	CHECK(PL_FileSize("DOOM1D00.LMP") == 300, "demo size after flush");
	n = PL_FileRead("DOOM1D00.LMP", buf, 50);
	CHECK(n == 50 && buf[0] == 7, "demo read");

	/* same size overwrite, different size recreate */
	memset(demo, 9, sizeof demo);
	PL_FileWrite("doom1_0.dsg", demo, 100);
	gfs_flush();
	CHECK(fs[find_file("doom1_0.dsg")].data[0] == 9 && fs[find_file("doom1_0.dsg")].size == 100, "overwrite");
	PL_FileWrite("doom1_0.dsg", demo, 60);
	gfs_flush();
	CHECK(fs[find_file("doom1_0.dsg")].size == 60, "recreate");
	CHECK(PL_FileSize("doom1_0.dsg") == 60, "size after recreate");
	PL_FileDelete("doom1_0.dsg");
	CHECK(PL_FileSize("doom1_0.dsg") < 0, "deleted pending");
	gfs_flush();
	CHECK(find_file("doom1_0.dsg") < 0, "deleted on disk");
	CHECK(PL_FileSize("missing.txt") < 0, "missing");
	CHECK(PL_FileRead("missing.txt", buf, 10) < 0, "missing read");

	/* listing */
	gfs_entry_t w[4], d[4]; int nw, nd;
	gfs_list(w, 4, &nw, d, 4, &nd);
	CHECK(nd == 1, "demo listed %d", nd);
	printf("list: %d wad, %d lmp (%s)\n", nw, nd, nd ? d[0].name : "");
}

static void rm_file(char const *name)
{
	int f = find_file(name);
	if(f >= 0) { free(fs[f].data); fs[f].used = false; }
}
static bool file_is(char const *name, void const *data, int size)
{
	int f = find_file(name);
	return f >= 0 && fs[f].size == size && !memcmp(fs[f].data, data, size);
}
/* Forget the failures of the session (files.c is #included), so that a test
   can check the exact exit text */
static void reset_failures(void)
{
	g_flush_failures = 0;
	g_flush_kept = 0;
	g_fail_name[0] = 0;
	g_fail_result = 0;
}

/* External review: with no memory for its copy, the settings file is
   refused (counted for the exit screen, the file in storage untouched)
   instead of being written without the platform's CG.* lines */
static void test_settings_nomem(void)
{
	char text[256] = "", name[GFS_NAME_MAX];
	int rc = 0;
	/* The session's failures so far, put back afterwards */
	int f = g_flush_failures, k = g_flush_kept, r = g_fail_result;
	strcpy(name, g_fail_name);
	char const *sname = gfs_settings_name;
	reset_failures();

	add_file("SET.CFG", "Version=1\nCG.Old=1\n", 20);
	gfs_settings_name = "SET.CFG";
	gfs_settings_appender = appender;
	g_malloc_fail_over = 8;
	int w = PL_FileWrite("SET.CFG", "Version=1\n", 10);
	g_malloc_fail_over = -1;
	gfs_settings_appender = NULL;
	CHECK(w < 0, "settings without memory refused (%d)", w);
	CHECK(gfs_pending_count() == 0 && gfs_flush() == 0
		&& file_is("SET.CFG", "Version=1\nCG.Old=1\n", 20), "old settings untouched");
	CHECK(gfs_last_failure(text, &rc) && !strcmp(text, "SET.CFG")
		&& rc == GFS_ERR_NO_MEMORY, "failure named %s %d", text, rc);
	CHECK(gfs_failure_text(text, sizeof text, 0) && strstr(text, "settings SET.CFG")
		&& strstr(text, "previous settings") && !strstr(text, "DelaySaves"),
		"exit text: %s", text);
	printf("settings nomem: %s\n", text);

	/* With memory: written whole */
	gfs_settings_appender = appender;
	CHECK(PL_FileWrite("SET.CFG", "Version=1\n", 10) == 10, "settings write");
	gfs_settings_appender = NULL;
	CHECK(gfs_flush() == 0 && file_is("SET.CFG", "Version=1\nCG.Test=1\n", 20), "settings whole");

	rm_file("SET.CFG");
	gfs_settings_name = sname;
	g_flush_failures = f; g_flush_kept = k; g_fail_result = r;
	strcpy(g_fail_name, name);
}

/* gint review round 1: os_flush() failure paths. A replacement that does
   not fit puts the previous version back (test_replace_rollback); these are
   the failures where that cannot work either, and nothing may be left
   behind. */
static void test_flush_failures(void)
{
	char old[64], neu[200], name[GFS_NAME_MAX];
	int rc = 0;
	memset(old, 1, sizeof old);
	memset(neu, 2, sizeof neu);

	/* An existing save of another size, then every write fails, the new
	   file's and the restored old one's alike: no full-size 0xFF file
	   may stay behind, and nothing is reported as kept */
	PL_FileWrite("FREE0_1.DSG", old, sizeof old);
	CHECK(gfs_flush() == 0, "seed save");
	CHECK(!gfs_last_failure(name, &rc), "no failure yet");
	fail_write_after = 0;
	PL_FileWrite("FREE0_1.DSG", neu, sizeof neu);
	int f = gfs_flush();
	fail_write_after = -1;
	CHECK(f == 1, "write failure counted (%d)", f);
	CHECK(find_file("FREE0_1.DSG") < 0, "no partial file after failed writes");
	CHECK(gfs_flush_kept() == 0, "nothing kept when the restore fails too");
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "FREE0_1.DSG") && rc == -5,
		"last failure %s %d", name, rc);

	/* Multi-chunk write of a new file failing in the second chunk */
	static char big[40 << 10];
	memset(big, 3, sizeof big);
	fail_write_after = 1;
	PL_FileWrite("BIG.DAT", big, sizeof big);
	f = gfs_flush();
	fail_write_after = -1;
	CHECK(f == 1 && find_file("BIG.DAT") < 0, "half-written file removed (%d)", f);

	/* create() failing every time (not for lack of space): the old file
	   cannot be created again either. Reported by name, nothing left */
	PL_FileWrite("CGDOOM.CFG", old, 10);
	CHECK(gfs_flush() == 0, "cfg seed");
	fail_create = 1;
	PL_FileWrite("CGDOOM.CFG", neu, 100);
	f = gfs_flush();
	fail_create = 0;
	CHECK(f == 1, "create failure counted (%d)", f);
	CHECK(find_file("CGDOOM.CFG") < 0 && gfs_flush_kept() == 0, "create failure: nothing left");
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "CGDOOM.CFG") && rc == -4,
		"create failure name %s %d", name, rc);

	/* Same size: in-place fast path still works and succeeds */
	PL_FileWrite("FAST.DAT", old, sizeof old);
	gfs_flush();
	memset(old, 9, sizeof old);
	PL_FileWrite("FAST.DAT", old, sizeof old);
	CHECK(gfs_flush() == 0 && fs[find_file("FAST.DAT")].data[0] == 9, "in-place overwrite");
	printf("flush failure paths: %d failures so far\n", gfs_flush_failures());
}

/* A replacement with a file of another size (remove, create, write) keeps
   the previous version in RAM and writes it back when the new one does not
   fit; before, both versions were lost */
static void test_replace_rollback(void)
{
	static uint8_t old[64], neu[5000], big[40 << 10], small[24], same[64];
	static uint8_t huge[GFS_BACKUP_MAX + 4096];
	char name[GFS_NAME_MAX], text[256];
	int rc = 0;
	for(int i = 0; i < (int)sizeof old; i++) old[i] = (uint8_t)(i * 7 + 1);
	memset(neu, 2, sizeof neu);
	memset(big, 3, sizeof big);
	memset(small, 4, sizeof small);
	memset(same, 0x5a, sizeof same);
	memset(huge, 5, sizeof huge);
	reset_failures();

	/* Growing: storage only has room for the new file once the old one
	   is gone (a temporary copy + rename would not fit) */
	PL_FileWrite("ROLL0_1.DSG", old, sizeof old);
	CHECK(gfs_flush() == 0 && file_is("ROLL0_1.DSG", old, sizeof old), "rollback: seed");
	fs_capacity = fs_used() + (int)sizeof neu - (int)sizeof old;
	PL_FileWrite("ROLL0_1.DSG", neu, sizeof neu);
	int f = gfs_flush();
	fs_capacity = -1;
	CHECK(f == 0 && file_is("ROLL0_1.DSG", neu, sizeof neu), "rollback: tight replacement written (%d)", f);
	/* Shrinking */
	PL_FileWrite("ROLL0_1.DSG", old, sizeof old);
	CHECK(gfs_flush() == 0 && file_is("ROLL0_1.DSG", old, sizeof old), "rollback: shrinking replacement");
	CHECK(gfs_flush_failures() == 0, "rollback: no failure so far");

	/* create() fails: the new file does not fit even in the old one's
	   space. The old one is created again and written back */
	fs_capacity = fs_used() + 1000;
	PL_FileWrite("ROLL0_1.DSG", neu, sizeof neu);
	f = gfs_flush();
	fs_capacity = -1;
	CHECK(f == 1, "rollback: create failure counted (%d)", f);
	CHECK(file_is("ROLL0_1.DSG", old, sizeof old), "rollback: previous version restored after create()");
	CHECK(gfs_flush_kept() == 1, "rollback: kept counted (%d)", gfs_flush_kept());
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "ROLL0_1.DSG") && rc == -4,
		"rollback: failure %s %d", name, rc);
	CHECK(gfs_failure_text(text, sizeof text, f) && !strcmp(text, "NOT SAVED: savegame "
		"ROLL0_1.DSG (error -4, storage full?). The previous version was kept. Free some "
		"storage space before playing again."), "rollback: text: %s", text);
	printf("rollback: %s\n", text);
	CHECK(PL_FileSize("ROLL0_1.DSG") == (int)sizeof old, "rollback: size read back");

	/* write() fails once, in the second 32 KiB chunk: partial file
	   removed, previous version restored */
	reset_failures();
	fail_write_after = 1;
	fail_write_count = 1;
	PL_FileWrite("ROLL0_1.DSG", big, sizeof big);
	f = gfs_flush();
	fail_write_after = -1;
	fail_write_count = -1;
	CHECK(f == 1, "rollback: write failure counted (%d)", f);
	CHECK(file_is("ROLL0_1.DSG", old, sizeof old), "rollback: previous version restored after write()");
	CHECK(gfs_flush_kept() == 1 && gfs_last_failure(name, &rc) && rc == -5,
		"rollback: write failure kept %d rc %d", gfs_flush_kept(), rc);
	CHECK(gfs_failure_text(text, sizeof text, f) && strstr(text, "previous version was kept")
		&& !strstr(text, "lost"), "rollback: write failure text: %s", text);

	/* remove() fails: nothing is created, the old file is still there,
	   unchanged, and reported as kept with remove()'s error */
	reset_failures();
	fail_remove = 1;
	PL_FileWrite("ROLL0_1.DSG", neu, sizeof neu);
	f = gfs_flush();
	fail_remove = 0;
	CHECK(f == 1 && file_is("ROLL0_1.DSG", old, sizeof old) && gfs_flush_kept() == 1
		&& gfs_last_failure(name, &rc) && rc == -7,
		"rollback: remove failure (%d, kept %d, rc %d)", f, gfs_flush_kept(), rc);

	/* create() fails but leaves an empty entry: the entry is removed and
	   the previous version written back (not reported as kept because a
	   file of that name exists) */
	reset_failures();
	fail_create_entry = 1;
	PL_FileWrite("ROLL0_1.DSG", neu, sizeof neu);
	f = gfs_flush();
	CHECK(f == 1 && file_is("ROLL0_1.DSG", old, sizeof old) && gfs_flush_kept() == 1,
		"rollback: create() left an entry (%d, kept %d)", f, gfs_flush_kept());

	/* No memory for the backup: the replacement goes ahead without one,
	   so saving still works when memory is short... */
	reset_failures();
	PL_FileWrite("ROLL0_1.DSG", small, sizeof small);
	g_malloc_fail_over = 32;           /* the 64-byte backup fails */
	f = gfs_flush();
	g_malloc_fail_over = -1;
	CHECK(f == 0 && file_is("ROLL0_1.DSG", small, sizeof small), "no backup: replacement written");
	/* ...but a failure then loses the old file (known limitation) */
	PL_FileWrite("ROLL0_1.DSG", neu, sizeof neu);
	fs_capacity = fs_used() + 1000;
	g_malloc_fail_over = 16;           /* the 24-byte backup fails */
	f = gfs_flush();
	g_malloc_fail_over = -1;
	fs_capacity = -1;
	CHECK(f == 1 && find_file("ROLL0_1.DSG") < 0 && gfs_flush_kept() == 0,
		"no backup: old file lost (%d)", f);
	CHECK(gfs_failure_text(text, sizeof text, f) && !strcmp(text, "NOT SAVED: savegame "
		"ROLL0_1.DSG (error -4, storage full?). It is lost. Free some storage space "
		"before playing again."), "no backup: text: %s", text);

	/* An old file above GFS_BACKUP_MAX is not read into RAM */
	add_file("HUGE.DAT", huge, sizeof huge);
	PL_FileWrite("HUGE.DAT", small, sizeof small);
	g_malloc_largest = 0;
	f = gfs_flush();
	CHECK(f == 0 && file_is("HUGE.DAT", small, sizeof small) && g_malloc_largest < sizeof huge,
		"huge old file replaced without a backup (largest malloc %zu)", g_malloc_largest);
	rm_file("HUGE.DAT");

	/* Same size: in place (open, size, close, open, write, close), after
	   the backup (open, read, close; one malloc) */
	PL_FileWrite("ROLL0_1.DSG", old, sizeof old);
	CHECK(gfs_flush() == 0, "same size: seed");
	PL_FileWrite("ROLL0_1.DSG", same, sizeof same);
	int calls = os_calls, mallocs = g_malloc_calls;
	f = gfs_flush();
	CHECK(f == 0 && file_is("ROLL0_1.DSG", same, sizeof same) && os_calls - calls == 9
		&& g_malloc_calls == mallocs + 1, "same size in place (%d os calls, %d mallocs)",
		os_calls - calls, g_malloc_calls - mallocs);

	/* Same size, but the in-place write fails half-way. A replacement
	   that works (one-off error) writes the new file; one that fails too
	   puts the previous version back from the backup */
	static uint8_t big2[40 << 10];
	memset(big2, 6, sizeof big2);
	PL_FileWrite("ROLL0_2.DSG", big, sizeof big);
	CHECK(gfs_flush() == 0, "in-place failure: seed");
	fail_write_after = 1;
	fail_write_count = 1;
	PL_FileWrite("ROLL0_2.DSG", big2, sizeof big2);
	f = gfs_flush();
	fail_write_after = -1;
	fail_write_count = -1;
	CHECK(f == 0 && file_is("ROLL0_2.DSG", big2, sizeof big2), "in-place failure, then replaced (%d)", f);
	reset_failures();
	fail_write_after = 1;              /* in place, then the replacement */
	fail_write_count = 2;
	PL_FileWrite("ROLL0_2.DSG", big, sizeof big);
	f = gfs_flush();
	fail_write_after = -1;
	fail_write_count = -1;
	CHECK(f == 1 && file_is("ROLL0_2.DSG", big2, sizeof big2) && gfs_flush_kept() == 1,
		"in-place failure, replacement fails: previous version restored (%d)", f);

	/* The same without memory for the backup: the file is a mix of both
	   versions after the in-place write and is not restored as the old
	   one; a replacement that fails too leaves nothing */
	reset_failures();
	PL_FileWrite("ROLL0_2.DSG", big, sizeof big);
	fail_write_after = 1;
	g_malloc_fail_over = 32 << 10;    /* the 40 KiB backup fails */
	f = gfs_flush();
	g_malloc_fail_over = -1;
	fail_write_after = -1;
	CHECK(f == 1 && find_file("ROLL0_2.DSG") < 0 && gfs_flush_kept() == 0,
		"in-place failure without a backup: nothing left (%d)", f);

	/* Deletes: one remove() each, a missing file is no failure */
	PL_FileDelete("ROLL0_1.DSG");
	PL_FileDelete("NONE0_1.DSG");
	calls = os_calls;
	f = gfs_flush();
	CHECK(f == 0 && find_file("ROLL0_1.DSG") < 0 && os_calls - calls == 2,
		"deletes (%d, %d os calls)", f, os_calls - calls);
}

/* Several files in one flush: a replacement that does not fit is rolled
   back and a new file that does not fit is lost, while the others are
   written and the delete is done */
static void test_flush_mixed(void)
{
	static uint8_t a[100], b_old[64], b_new[5000], c[32], d_old[48], d_new[48], e[50000];
	char name[GFS_NAME_MAX], text[256];
	int rc = 0;
	memset(a, 0xa1, sizeof a);
	for(int i = 0; i < (int)sizeof b_old; i++) b_old[i] = (uint8_t)(255 - i);
	memset(b_new, 0xb2, sizeof b_new);
	memset(c, 0xc3, sizeof c);
	memset(d_old, 0xd4, sizeof d_old);
	memset(d_new, 0xd5, sizeof d_new);
	memset(e, 0xe6, sizeof e);
	add_file("MIXB.DSG", b_old, sizeof b_old);
	add_file("MIXC.DSG", c, sizeof c);
	add_file("MIXD.DSG", d_old, sizeof d_old);
	reset_failures();

	PL_FileWrite("MIXA.DSG", a, sizeof a);
	PL_FileWrite("MIXB.DSG", b_new, sizeof b_new);
	PL_FileDelete("MIXC.DSG");
	PL_FileWrite("MIXD.DSG", d_new, sizeof d_new);
	PL_FileWrite("MIXE.LMP", e, sizeof e);
	CHECK(gfs_pending_count() == 5, "mixed: 5 pending");
	fs_capacity = fs_used() + (int)sizeof a + 1000;
	int f = gfs_flush();
	fs_capacity = -1;
	CHECK(f == 2, "mixed: 2 failures (%d)", f);
	CHECK(file_is("MIXA.DSG", a, sizeof a), "mixed: new file written");
	CHECK(file_is("MIXB.DSG", b_old, sizeof b_old), "mixed: replacement rolled back");
	CHECK(find_file("MIXC.DSG") < 0, "mixed: deleted");
	CHECK(file_is("MIXD.DSG", d_new, sizeof d_new), "mixed: same size written in place");
	CHECK(find_file("MIXE.LMP") < 0, "mixed: new file that does not fit leaves nothing");
	CHECK(gfs_flush_failures() == 2 && gfs_flush_kept() == 1, "mixed: counts %d %d",
		gfs_flush_failures(), gfs_flush_kept());
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "MIXB.DSG") && rc == -4,
		"mixed: first failure %s %d", name, rc);
	CHECK(gfs_failure_text(text, sizeof text, f) && !strcmp(text, "NOT SAVED: savegame "
		"MIXB.DSG and other files (error -4, storage full?). 1 of them kept their previous "
		"version, the others are lost. Free some storage space before playing again."),
		"mixed: text: %s", text);
	printf("mixed: %s\n", text);

	/* Two replacements that do not fit: both rolled back */
	reset_failures();
	PL_FileWrite("MIXB.DSG", b_new, sizeof b_new);
	PL_FileWrite("MIXD.DSG", b_new, sizeof b_new);
	fs_capacity = fs_used() + 1000;
	f = gfs_flush();
	fs_capacity = -1;
	CHECK(f == 2 && file_is("MIXB.DSG", b_old, sizeof b_old)
		&& file_is("MIXD.DSG", d_new, sizeof d_new) && gfs_flush_kept() == 2,
		"mixed: both rolled back (%d, kept %d)", f, gfs_flush_kept());
	CHECK(gfs_failure_text(text, sizeof text, f) && strstr(text, " and other files ")
		&& strstr(text, "Their previous versions were kept.") && !strstr(text, "lost"),
		"mixed: all kept text: %s", text);
	printf("mixed: %s\n", text);
	rm_file("MIXA.DSG");
	rm_file("MIXB.DSG");
	rm_file("MIXD.DSG");
}

/* Round 2: what exit_path() shows */
static void test_failure_text_clean(void)
{
	char text[256];   /* as exit_path() */
	CHECK(gfs_flush_failures() == 0, "no failure yet");
	CHECK(!gfs_failure_text(text, sizeof text, 0), "nothing to report");
	/* A refused flush wrote nothing: storage is unchanged */
	CHECK(gfs_failure_text(text, sizeof text, -1) && !strcmp(text, "Some files were NOT "
		"saved and are lost. The files already in storage were not changed."),
		"refused flush: generic text: %s", text);
}

/* Round 2: PL_FileWrite()'s no-memory path writes at once; a failure there
   leaves nothing pending but must still be reported at exit */
static void test_nomem(void)
{
	char name[GFS_NAME_MAX], text[256];
	int rc = 0;
	static char save[100 << 10];
	memset(save, 4, sizeof save);
	int before = gfs_flush_failures();
	/* End of a game: the engine hands over a save, malloc cannot copy it,
	   and storage is full */
	g_malloc_fail_over = 64 << 10;
	fail_create = 1;
	int w = PL_FileWrite("FRE0P_3.DSG", save, sizeof save);
	fail_create = 0;
	g_malloc_fail_over = -1;
	CHECK(w < 0, "write failure returned to the caller");
	CHECK(gfs_pending_count() == 0, "nothing pending after the immediate write");
	CHECK(gfs_flush_failures() == before + 1, "failure counted");
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "FRE0P_3.DSG"), "named %s", name);
	/* exit_path(): nothing pending, its own flush returns 0, yet the
	   failure is shown */
	CHECK(gfs_flush() == 0, "exit flush has nothing to do");
	CHECK(gfs_failure_text(text, sizeof text, 0) && strstr(text, "FRE0P_3.DSG")
		&& strstr(text, "other files"), "reported at exit: %s", text);
	printf("nomem: %s\n", text);

	/* And the no-memory path still succeeds when storage has room */
	g_malloc_fail_over = 64 << 10;
	w = PL_FileWrite("FRE0P_4.DSG", save, sizeof save);
	g_malloc_fail_over = -1;
	CHECK(w == (int)sizeof save && find_file("FRE0P_4.DSG") >= 0
		&& gfs_flush_failures() == before + 1, "immediate write ok");
}

/* Round 3.3: the bogus size an unbegun recording used to produce in the
   engine's G_CheckDemoStatus() ("demo_p - demobuffer" with demo_p NULL + 1)
   is refused before the slot's existing demo is touched, on both
   PL_FileWrite() paths. (end_game_session()'s own gdemo_begun() check was
   removed: the engine's "begun" flag now drops an armed recording, tested by
   tests/demo-unbegun.sh; this stays as defence in depth.) */
static void test_demo_exit(void)
{
	static uint8_t demobuf[0x20000];
	char old[300];
	memset(old, 7, sizeof old);
	PL_FileWrite("FRE0D00.LMP", old, sizeof old);
	CHECK(gfs_flush() == 0 && find_file("FRE0D00.LMP") >= 0, "seed demo");
	int before = gfs_flush_failures();
	/* The engine's int on the calculator: (byte *)1 - 0x8c1a0000 */
	int bogus = (int)(uint32_t)(1u - 0x8c1a0000u);
	CHECK(bogus > GFS_WRITE_MAX, "bogus size %d", bogus);
	CHECK(PL_FileWrite("FRE0D00.LMP", demobuf, bogus) < 0, "bogus size refused");
	CHECK(gfs_pending_count() == 0, "nothing queued for the slot");
	g_malloc_fail_over = 64 << 10;
	CHECK(PL_FileWrite("FRE0D00.LMP", demobuf, bogus) < 0, "refused without memory too");
	g_malloc_fail_over = -1;
	CHECK(gfs_flush() == 0 && gfs_flush_failures() == before, "no flush failure");
	int i = find_file("FRE0D00.LMP");
	CHECK(i >= 0 && fs[i].size == (int)sizeof old, "the slot's demo is intact");
	/* The largest legitimate size still goes through */
	CHECK(PL_FileWrite("FRE0D00.LMP", demobuf, sizeof demobuf) == (int)sizeof demobuf
		&& gfs_flush() == 0 && fs[find_file("FRE0D00.LMP")].size == (int)sizeof demobuf,
		"a full 128 KiB demo is written");
}

/* Final review: a segment chained to the next (file-adjacent, size a
   multiple of 4) gets no pad of its own; reads and maps across every
   boundary still return the file's bytes, zeros past EOF, 4-aligned */
static void le32w(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void test_packed_image(void)
{
	static int const sizes[] = { 4, 8, 12, 136, 200, 3, 5, 1000, 4, 1, 140, 64, 2, 8 };
	enum { NL = sizeof sizes / sizeof *sizes };
	int pos[NL], off = 12;
	for(int i = 0; i < NL; i++) { pos[i] = off; off += sizes[i]; }
	int dirofs = off, fsize = dirofs + 16 * NL;
	uint8_t *f = malloc(fsize);
	for(int i = 0; i < fsize; i++) f[i] = (uint8_t)(i * 13 + 7);
	memcpy(f, "PWAD", 4); le32w(f + 4, NL); le32w(f + 8, dirofs);
	for(int i = 0; i < NL; i++) {
		uint8_t *e = f + dirofs + 16 * i;
		le32w(e, pos[i]); le32w(e + 4, sizes[i]);
		char nm[9]; snprintf(nm, sizeof nm, "LUMP%d", i); memset(e + 8, 0, 8); memcpy(e + 8, nm, strlen(nm));
	}
	int old = find_file("PACK.WAD");
	if(old >= 0) { free(fs[old].data); fs[old].used = false; }
	add_file("PACK.WAD", f, fsize);

	g_opt.wad_in_ram = 1;
	CHECK(PL_WadOpen("PACK.WAD") == fsize && gwad_stats.mode == GWAD_RAM, "packed: RAM mode");
	/* Expected size, by the rule: header, lumps, directory are segments */
	int segsz[NL + 2], ns = 0;
	segsz[ns++] = 12;
	for(int i = 0; i < NL; i++) segsz[ns++] = sizes[i];
	segsz[ns++] = 16 * NL;
	uint32_t end = 0;
	int chained = 0;
	for(int i = 0; i < ns; i++) {
		bool ch = i + 1 < ns && (segsz[i] & 3) == 0;
		chained += ch;
		end = ((end + 3) & ~3u) + segsz[i] + (ch ? 0 : 136);
	}
	end = (end + 31) & ~31u;
	uint32_t unchained = 0;
	for(int i = 0; i < ns; i++) unchained = ((unchained + 3) & ~3u) + segsz[i] + 136;
	CHECK((uint32_t)gwad_stats.ram_bytes == end, "packed image %d bytes, expected %u",
		gwad_stats.ram_bytes, end);
	printf("packed image: %d bytes (%d chained segments; %u without chaining)\n",
		gwad_stats.ram_bytes, chained, (unchained + 31) & ~31u);
	CHECK(chained >= 8 && (uint32_t)gwad_stats.ram_bytes < unchained, "chaining saves pads");
	uint8_t buf[2048];
	for(int o = 0; o < fsize; o++)
	for(int len = 0; len <= 300 && o + len <= fsize; len += (len < 20 ? 1 : 37)) {
		memset(buf, 0x55, len);
		CHECK(PL_WadRead(buf, o, len) == len && !memcmp(buf, f + o, len), "packed read %d+%d", o, len);
	}
	for(int i = 0; i < NL; i++) {
		int padded = sizes[i] + 136;
		if(padded > fsize - pos[i]) padded = fsize - pos[i];
		uint8_t const *m = PL_WadMap(pos[i], padded);
		CHECK(m && ((uintptr_t)m & 3) == 0, "packed lump %d mapped, aligned", i);
		if(m) CHECK(!memcmp(m, f + pos[i], padded), "packed lump %d content+pad", i);
	}
	PL_WadClose();
	free(f);
}

/* Final review: PL_WadOpen() says why a file is not usable */
static void test_wad_errors(void)
{
	uint8_t b[200];
	memset(b, 0, sizeof b);
	add_file("SHORT.WAD", "IWAD\0\0\0", 8);
	memcpy(b, "ZWAD", 4); b[8] = 12;
	add_file("MAGIC.WAD", b, sizeof b);
	memcpy(b, "IWAD", 4); le32w(b + 4, 100); le32w(b + 8, 50);   /* 1600-byte dir */
	add_file("TRUNC.WAD", b, sizeof b);
	g_opt.wad_in_ram = 1;
	CHECK(PL_WadOpen("SHORT.WAD") == GWAD_ERR_SHORT, "short");
	CHECK(PL_WadOpen("MAGIC.WAD") == GWAD_ERR_MAGIC, "magic");
	CHECK(PL_WadOpen("TRUNC.WAD") == GWAD_ERR_TRUNCATED, "truncated");
	CHECK(PL_WadOpen("NONE.WAD") == GWAD_ERR_OPEN, "missing");
	CHECK(PL_WadOpen("a_name_much_longer_than_the_forty_chars_allowed.wad") == GWAD_ERR_NAME, "name");
	char const *t[6];
	for(int i = 0; i < 6; i++) {
		t[i] = gwad_error_text(-1 - i);
		CHECK(t[i] && t[i][0], "text %d", -1 - i);
		for(int j = 0; j < i; j++) CHECK(strcmp(t[i], t[j]), "texts %d %d differ", i, j);
	}
	CHECK(gwad_map_live() == false, "nothing open after the errors");
}

/* Final review: a list cut at its maximum is reported (gfs_list_more) */
static void test_list_more(void)
{
	char d[16];
	for(int i = 0; i < 5; i++) {
		char nm[16]; snprintf(nm, sizeof nm, "MORE%dD00.LMP", i);
		memset(d, i, sizeof d);
		add_file(nm, d, sizeof d);
	}
	gfs_entry_t w[64], dm[64]; int nw, nd;
	gfs_list(w, 64, &nw, dm, 64, &nd);
	CHECK(!(gfs_list_more & GFS_MORE_DEMOS), "all demos listed (%d)", nd);
	int all = nd;
	gfs_list(w, 64, &nw, dm, all - 1, &nd);
	CHECK(nd == all - 1 && (gfs_list_more & GFS_MORE_DEMOS), "demo list cut (%d of %d)", nd, all);
	gfs_list(w, 64, &nw, dm, all, &nd);
	CHECK(nd == all && !(gfs_list_more & GFS_MORE_DEMOS), "exactly full is not cut");
	for(int i = 0; i < 5; i++) {
		char nm[16]; snprintf(nm, sizeof nm, "MORE%dD00.LMP", i);
		int f = find_file(nm); if(f >= 0) { free(fs[f].data); fs[f].used = false; }
	}
}

/* Final review: PL_FileWrite()'s refusal while the WAD block map is live
   (no memory for a copy) is counted, so the exit screen names the lost
   file; G_DoSaveGame() shows "save failed" from the -1 */
static int s_block_p0(int fd, int offset, void **ptr) { (void)fd; *ptr = (void *)(uintptr_t)(0x00112900u + offset); return 0; }
static void test_refusal_counted(void)
{
	uint8_t b[64];
	memset(b, 0, sizeof b);
	memcpy(b, "IWAD", 4); b[8] = 12;
	add_file("MAP.WAD", b, sizeof b);
	g_os.block = s_block_p0;
	g_opt.wad_in_ram = 0;
	CHECK(PL_WadOpen("MAP.WAD") == (int)sizeof b && gwad_map_live(), "block map live");
	int before = gfs_flush_failures();
	static char save[100 << 10];
	g_malloc_fail_over = 64 << 10;
	CHECK(PL_FileWrite("FRE0P_5.DSG", save, sizeof save) < 0, "refused while the map is live");
	g_malloc_fail_over = -1;
	CHECK(gfs_flush_failures() == before + 1, "refusal counted");
	char name[GFS_NAME_MAX], text[256]; int rc;
	CHECK(gfs_last_failure(name, &rc) && !strcmp(name, "FRE0P_5.DSG") && rc == GFS_ERR_NO_MEMORY,
		"refusal named %s %d", name, rc);
	PL_WadClose();
	g_os.block = NULL;
	CHECK(gfs_failure_text(text, sizeof text, 0) && strstr(text, "NOT SAVED")
		&& strstr(text, "savegame FRE0P_5.DSG") && strstr(text, "no memory")
		&& strstr(text, "lost"), "exit text: %s", text);
	printf("refusal: %s\n", text);
}

int main(int argc, char **argv)
{
	g_os = (gos_api_t){ s_open, s_close, s_size, s_read, s_write, s_create, s_remove, s_ff, s_fn, s_fc, NULL };
	test_failure_text_clean();
	test_files();
	test_flush_failures();
	test_replace_rollback();
	test_flush_mixed();
	test_nomem();
	test_settings_nomem();
	test_demo_exit();
	test_refusal_counted();
	test_list_more();
	test_wad_errors();
	test_packed_image();
	for(int i = 1; i < argc; i++) test_wad(argv[i]);
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
