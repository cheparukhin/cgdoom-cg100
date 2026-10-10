/* CGDoom gint platform: files (PL_File*), world switches, delayed writes.

   - Every OS call happens inside gfs_os_call(): one gint_world_switch() per
     batch of work (never one per block or per chunk), followed by the UTLB
     repair (tlb.c) and a note that the OS may have drawn into the LCD.
   - Paths and small buffers handed to the OS live in a P1 heap scratch area;
     bulk reads go to P1 buffers (bounced if the caller's buffer is not P1).
   - Writes and deletes are DELAYED (CGDoom's delayed-save concept): kept in
     RAM and performed by gfs_flush() in a single world switch, which main()
     calls after PL_WadClose() (when returning from the game or quitting).
     Writing to storage makes Fugue move records, which would invalidate the
     WAD block map; gfs_flush() refuses to run while the map is live.
   - Files we create must have strict 8.3 names (the fx-CG100 Memory app
     truncates longer names).
   - Overwriting a file of the same size is done in place (fast path, gpSP);
     otherwise remove + create(final size) + write.
   - Small files read with PL_FileSize()/PL_FileRead() are cached so that
     the size + read pair costs one world switch. */

#include "cgd_gint.h"

#include <gint/gint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gfs_world_switches = 0;
char const *(*gfs_settings_appender)(void) = NULL;
char const *gfs_settings_name = NULL;

#define GFS_MAX_PENDING   16
#define GFS_CACHE_MAX     (16 << 10)
#define GFS_WRITE_CHUNK   (32 << 10)

//---
// Helpers
//---

static gfs_scratch_t g_scratch_static;
static gfs_scratch_t *g_scratch = NULL;

gfs_scratch_t *gfs_scratch(void)
{
	if(!g_scratch) {
		/* malloc() returns P1 memory; the static fallback is P0 (.bss),
		   which works too (gpSP passes P0 paths) but is second choice */
		g_scratch = malloc(sizeof *g_scratch);
		if(!g_scratch)
			g_scratch = &g_scratch_static;
	}
	return g_scratch;
}

bool gfs_is_p1(void const *p, size_t size)
{
	uintptr_t a = (uintptr_t)p;
	return a >= 0x80000000u && a < 0xa0000000u
		&& size <= 0xa0000000u - a;
}

static bool name_char_ok(char c)
{
	if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
		return true;
	if(c >= '0' && c <= '9')
		return true;
	return strchr("_-!#$%()@^", c) != NULL && c != 0;
}

bool gfs_valid_83(char const *name)
{
	int base = 0, ext = -1;
	if(!name)
		return false;
	for(char const *c = name; *c; c++) {
		if(*c == '.') {
			if(ext >= 0 || base == 0)
				return false;
			ext = 0;
			continue;
		}
		if(!name_char_ok(*c))
			return false;
		if(ext >= 0) {
			if(++ext > 3)
				return false;
		}
		else if(++base > 8)
			return false;
	}
	return base > 0 && ext != 0;
}

static bool name_eq(char const *a, char const *b)
{
	for(;; a++, b++) {
		char ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
		char cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
		if(ca != cb)
			return false;
		if(!ca)
			return true;
	}
}

bool gfs_make_path(uint16_t *path, int max, char const *name)
{
	static char const prefix[] = "\\\\fls0\\";
	int n = 0;
	if(!name || !name[0])
		return false;
	for(int i = 0; prefix[i]; i++)
		path[n++] = (uint8_t)prefix[i];
	for(int i = 0; name[i]; i++) {
		char c = name[i];
		if(c < 0x20 || c >= 0x7f || c == '\\' || c == '/')
			return false;
		if(n >= max - 1)
			return false;
		path[n++] = (uint8_t)c;
	}
	path[n] = 0;
	return true;
}

bool gfs_read_ok(int rc, int size, int remaining_after)
{
	return rc == size || (rc >= 0 && rc == remaining_after);
}

int gfs_os_call(int (*fn)(void *), void *arg)
{
	/* Finish the LCD DMA and hand the full window back first; gint saves
	   our window and restores the OS's for the duration of the switch */
	gvid_release();
	int rc = gint_world_switch(GINT_CALL(fn, arg));
	gtlb_repair();
	gfs_world_switches++;
	gvid_note_os_activity();
	return rc;
}

//---
// Reading (with a one-entry cache for small files)
//---

typedef struct {
	uint16_t const *path;
	void *buf;
	int bufsize;
	int fsize;     /* file size, -1 if missing */
	int got;       /* bytes read, <0 on error */
	bool whole_only;  /* read nothing unless the whole file fits */
} rd_ctx_t;

static int os_read_file(void *arg)
{
	rd_ctx_t *r = arg;
	r->fsize = -1;
	r->got = -1;

	int fd = g_os.open(r->path, BFile_ReadOnly, 0);
	if(fd < 0)
		return fd;
	int fsize = g_os.size(fd);
	r->fsize = fsize;
	if(fsize >= 0) {
		int n = (fsize < r->bufsize) ? fsize : r->bufsize;
		if(r->whole_only && fsize > r->bufsize)
			n = 0;
		if(n <= 0 || !r->buf)
			r->got = 0;
		else {
			int rc = g_os.read(fd, r->buf, n, 0);
			r->got = gfs_read_ok(rc, n, fsize - n) ? n : -1;
		}
	}
	g_os.close(fd);
	return 0;
}

static struct {
	bool valid;
	char name[GFS_LIST_NAME];
	int size;           /* -1: file is missing */
	uint8_t *data;      /* whole file (size <= GFS_CACHE_MAX), or NULL */
} g_cache;

void gfs_drop_read_cache(void)
{
	free(g_cache.data);
	g_cache.data = NULL;
	g_cache.valid = false;
}

static void cache_invalidate(char const *name)
{
	if(g_cache.valid && (!name || name_eq(g_cache.name, name)))
		gfs_drop_read_cache();
}

/* Load the cache entry for name (one world switch). Returns false if the
   file could not be examined at all. */
static bool cache_load(char const *name)
{
	if(g_cache.valid && name_eq(g_cache.name, name))
		return true;
	if(strlen(name) >= GFS_LIST_NAME)
		return false;
	gfs_drop_read_cache();

	gfs_scratch_t *s = gfs_scratch();
	if(!gfs_make_path(s->path, sizeof s->path / 2, name))
		return false;

	uint8_t *buf = malloc(GFS_CACHE_MAX);
	rd_ctx_t r = {
		.path = s->path, .buf = buf, .bufsize = buf ? GFS_CACHE_MAX : 0,
		.whole_only = true,
	};
	gfs_os_call(os_read_file, &r);

	strcpy(g_cache.name, name);
	g_cache.size = r.fsize;
	g_cache.data = NULL;
	if(buf && r.fsize >= 0 && r.fsize <= GFS_CACHE_MAX && r.got == r.fsize) {
		/* Shrink to the file size (krealloc splits in place) */
		void *small = realloc(buf, r.fsize > 0 ? r.fsize : 1);
		g_cache.data = small ? small : buf;
	}
	else
		free(buf);
	g_cache.valid = true;
	return true;
}

//---
// Delayed writes
//---

typedef struct {
	char name[GFS_NAME_MAX];
	uint8_t const *data;   /* NULL for a delete */
	int size;
	bool owned;            /* data is our malloc'd copy */
	bool del;
	int result;
} pending_t;

static pending_t g_pend[GFS_MAX_PENDING];
static int g_npend = 0;
static int g_flush_failures = 0;
static char g_fail_name[GFS_NAME_MAX];
static int g_fail_result = 0;

bool gfs_last_failure(char name[GFS_NAME_MAX], int *result)
{
	if(!g_fail_name[0])
		return false;
	strcpy(name, g_fail_name);
	*result = g_fail_result;
	return true;
}

/* A failure that leaves nothing pending (final review: PL_FileWrite()'s
   refusal while the WAD map is live was not counted, so the exit screen
   stayed silent about the lost file) */
static void note_failure(char const *name, int result)
{
	if(strlen(name) < sizeof g_fail_name)
		strcpy(g_fail_name, name);
	g_fail_result = result;
	g_flush_failures++;
}

int gfs_pending_count(void)
{
	return g_npend;
}

int gfs_flush_failures(void)
{
	return g_flush_failures;
}

/* "savegame ", "demo ", ... for a file name (by extension) */
static char const *file_kind(char const *name)
{
	char const *dot = strrchr(name, '.');
	if(!dot)
		return "";
	if(name_eq(dot, ".DSG"))
		return "savegame ";
	if(name_eq(dot, ".LMP"))
		return "demo ";
	if(name_eq(dot, ".CFG"))
		return "settings ";
	return "";
}

bool gfs_failure_text(char *text, size_t size, int flush_result)
{
	/* Every failure of the session counts, not only the exit flush's
	   (gint review round 2): PL_FileWrite()'s no-memory path writes at
	   once and leaves nothing pending, and its callers
	   (CGD_FlushDelayedWrites) drop the result. A negative or uncounted
	   flush result (map live, unsupported OS) gets the generic text.
	   Final review: the text says that the file is lost (the game is
	   over when it is shown) and what to do. */
	if(flush_result == 0 && g_flush_failures == 0)
		return false;
	if(g_flush_failures > 0 && g_fail_name[0]) {
		/* The no-memory refusal only happens with DelaySaves=0 (the
		   engine then writes while the WAD map is live) */
		bool nomem = (g_fail_result == GFS_ERR_NO_MEMORY);
		char reason[48];
		if(nomem)
			snprintf(reason, sizeof reason, "no memory to keep it");
		else
			snprintf(reason, sizeof reason, "error %d, storage full?",
				g_fail_result);
		bool more = g_flush_failures > 1;
		snprintf(text, size, "NOT SAVED: %s%s%s (%s). %s lost. %s",
			file_kind(g_fail_name), g_fail_name,
			more ? " and other files" : "", reason,
			more ? "They are" : "It is", nomem
			? "Set DelaySaves=1 in CGDOOM.CFG."
			: "Free some storage space before playing again.");
	}
	else
		snprintf(text, size, "Some files were NOT saved (storage full?) "
			"and are lost. Free some storage space before playing "
			"again.");
	return true;
}

static pending_t *pending_find(char const *name)
{
	for(int i = 0; i < g_npend; i++) {
		if(name_eq(g_pend[i].name, name))
			return &g_pend[i];
	}
	return NULL;
}

static void pending_release(pending_t *p)
{
	if(p->owned)
		free((void *)p->data);
	p->data = NULL;
	p->owned = false;
}

/* Get a slot for name (reusing an existing entry for the same file) */
static pending_t *pending_slot(char const *name)
{
	pending_t *p = pending_find(name);
	if(p) {
		pending_release(p);
	}
	else {
		if(g_npend >= GFS_MAX_PENDING)
			return NULL;
		p = &g_pend[g_npend++];
		memset(p, 0, sizeof *p);
		strcpy(p->name, name);
	}
	p->result = 0;
	return p;
}

typedef struct {
	pending_t *ops;
	int count;
	gfs_scratch_t *s;
} flush_ctx_t;

static int write_all(int fd, uint8_t const *data, int size)
{
	int pos = 0;
	while(pos < size) {
		int chunk = size - pos;
		if(chunk > GFS_WRITE_CHUNK)
			chunk = GFS_WRITE_CHUNK;
		int rc = g_os.write(fd, data + pos, chunk);
		/* Fugue: bytes written; CASIOWIN: new offset */
		if(rc != chunk && rc != pos + chunk)
			return (rc < 0) ? rc : -1;
		pos += chunk;
	}
	return 0;
}

static int os_flush(void *arg)
{
	flush_ctx_t *c = arg;
	uint16_t *path = c->s->path;

	for(int i = 0; i < c->count; i++) {
		pending_t *op = &c->ops[i];
		if(!gfs_make_path(path, sizeof c->s->path / 2, op->name)) {
			op->result = -1;
			continue;
		}

		if(op->del) {
			int rc = g_os.remove(path);
			op->result = (rc >= 0 || rc == BFile_EntryNotFound) ? 0 : rc;
			continue;
		}

		/* Existing file? */
		int existing = -1;
		int fd = g_os.open(path, BFile_ReadOnly, 0);
		if(fd >= 0) {
			existing = g_os.size(fd);
			g_os.close(fd);
		}

		/* Fast path: overwrite a file of the same size in place */
		if(existing == op->size && op->size > 0) {
			fd = g_os.open(path, BFile_WriteOnly, 0);
			if(fd >= 0) {
				int rc = write_all(fd, op->data, op->size);
				g_os.close(fd);
				if(rc >= 0) {
					op->result = 0;
					continue;
				}
			}
		}

		/* Remove, create with the final size, write. The old file has to
		   go first: storage is nearly full with the full WAD installed,
		   and a temporary copy (create + rename) would need room for both
		   files exactly when space is short (gint review round 1). If
		   create() fails now, the new data does not fit even in the old
		   file's space; the failure is reported by name at exit. */
		if(existing >= 0)
			g_os.remove(path);
		int size = op->size;
		int rc = g_os.create(path, BFile_File, &size);
		if(rc < 0) {
			op->result = rc;
			continue;
		}
		fd = g_os.open(path, BFile_WriteOnly, 0);
		rc = (fd < 0) ? fd
			: (op->size > 0) ? write_all(fd, op->data, op->size) : 0;
		if(fd >= 0)
			g_os.close(fd);
		if(rc < 0) {
			/* Do not leave a full-size file of Fugue's preallocated
			   contents (or a half-written one) behind: the next session
			   would read it as a corrupt save or settings file */
			g_os.remove(path);
		}
		op->result = (rc < 0) ? rc : 0;
	}
	return 0;
}

int gfs_flush(void)
{
	if(g_npend == 0)
		return 0;
	if(!gsys_files_supported())
		return g_npend;
	/* Never write storage while flash pointers into the WAD are in use */
	if(gwad_map_live())
		return -1;

	flush_ctx_t ctx = { .ops = g_pend, .count = g_npend, .s = gfs_scratch() };
	gfs_os_call(os_flush, &ctx);

	int failures = 0;
	for(int i = 0; i < g_npend; i++) {
		if(g_pend[i].result < 0) {
			if(failures == 0) {
				strcpy(g_fail_name, g_pend[i].name);
				g_fail_result = g_pend[i].result;
			}
			failures++;
			PL_Log("gfs_flush: %s failed (%d)\n", g_pend[i].name,
				g_pend[i].result);
		}
		pending_release(&g_pend[i]);
	}
	g_npend = 0;
	g_flush_failures += failures;
	cache_invalidate(NULL);
	return failures;
}

//---
// PL_File* API
//---

int PL_FileSize(const char *name)
{
	if(!name || !gsys_files_supported())
		return -1;

	pending_t *p = pending_find(name);
	if(p)
		return p->del ? -1 : p->size;

	if(!cache_load(name))
		return -1;
	return g_cache.size;
}

int PL_FileRead(const char *name, void *buf, int size)
{
	if(!name || size < 0 || (size > 0 && !buf) || !gsys_files_supported())
		return -1;

	pending_t *p = pending_find(name);
	if(p) {
		if(p->del)
			return -1;
		int n = (p->size < size) ? p->size : size;
		memcpy(buf, p->data, n);
		return n;
	}

	if(!cache_load(name))
		return -1;
	if(g_cache.size < 0)
		return -1;
	if(g_cache.data) {
		int n = (g_cache.size < size) ? g_cache.size : size;
		memcpy(buf, g_cache.data, n);
		return n;
	}

	/* Large file: read directly (bounce if the buffer is not P1) */
	int n = (g_cache.size < size) ? g_cache.size : size;
	void *dst = buf;
	if(n > 0 && !gfs_is_p1(buf, n)) {
		dst = malloc(n);
		if(!dst)
			return -1;
	}

	gfs_scratch_t *s = gfs_scratch();
	if(!gfs_make_path(s->path, sizeof s->path / 2, name)) {
		if(dst != buf)
			free(dst);
		return -1;
	}
	rd_ctx_t r = { .path = s->path, .buf = dst, .bufsize = n };
	gfs_os_call(os_read_file, &r);

	if(dst != buf) {
		if(r.got > 0)
			memcpy(buf, dst, r.got);
		free(dst);
	}
	return r.got;
}

int PL_FileWrite(const char *name, const void *buf, int size)
{
	if(!name || size < 0 || (size > 0 && !buf) || !gsys_files_supported())
		return -1;
	/* Files we create must have strict 8.3 names (the engine produces
	   them); anything else is a bug: report and refuse */
	if(!gfs_valid_83(name)) {
		PL_Log("PL_FileWrite: '%s' is not 8.3\n", name);
		return -1;
	}
	/* Implausible size (an engine bug): refuse, keeping the existing file */
	if(size > GFS_WRITE_MAX) {
		PL_Log("PL_FileWrite: '%s' size %d refused\n", name, size);
		return -1;
	}

	char const *extra = NULL;
	int extra_len = 0;
	if(gfs_settings_appender && gfs_settings_name
		&& name_eq(name, gfs_settings_name)) {
		extra = gfs_settings_appender();
		extra_len = extra ? (int)strlen(extra) : 0;
	}


	cache_invalidate(name);

	/* Normal case: keep a private copy until gfs_flush() */
	uint8_t *copy = malloc(size + extra_len + 1);
	if(copy) {
		pending_t *p = pending_slot(name);
		if(!p && !gwad_map_live() && gfs_flush() >= 0)
			p = pending_slot(name);
		if(p) {
			if(size > 0)
				memcpy(copy, buf, size);
			if(extra_len > 0)
				memcpy(copy + size, extra, extra_len);
			p->data = copy;
			p->size = size + extra_len;
			p->owned = true;
			p->del = false;
			return size;
		}
		free(copy);
		return -1;
	}

	/* No memory for a copy: write right now (with everything pending),
	   unless the WAD map is live */
	if(gwad_map_live()) {
		PL_Log("PL_FileWrite: no memory to delay '%s'\n", name);
		note_failure(name, GFS_ERR_NO_MEMORY);
		return -1;
	}
	pending_t *p = pending_slot(name);
	if(!p) {
		if(gfs_flush() < 0)
			return -1;
		p = pending_slot(name);
		if(!p)
			return -1;
	}
	p->data = buf;
	p->size = size;
	p->owned = false;
	p->del = false;
	gfs_flush();
	/* The entry is released by gfs_flush() but keeps its result */
	return (p->result < 0) ? -1 : size;
}

int PL_FileDelete(const char *name)
{
	if(!name || !gsys_files_supported())
		return -1;
	if(!gfs_valid_83(name)) {
		PL_Log("PL_FileDelete: '%s' is not 8.3\n", name);
		return -1;
	}

	cache_invalidate(name);
	pending_t *p = pending_slot(name);
	if(!p) {
		if(gwad_map_live() || gfs_flush() < 0)
			return -1;
		p = pending_slot(name);
		if(!p)
			return -1;
	}
	p->data = NULL;
	p->size = 0;
	p->owned = false;
	p->del = true;
	return 0;
}

//---
// Directory listing
//---

int gfs_list_more = 0;

typedef struct {
	char const *ext;          /* ".wad" or ".lmp" */
	gfs_entry_t *out;
	int max, count;
	bool more;                /* stopped at max with entries left */
} list_one_t;

typedef struct {
	list_one_t l[2];
	gfs_scratch_t *s;
} list_ctx_t;

static void pattern(uint16_t *path, char const *ext)
{
	static char const prefix[] = "\\\\fls0\\*";
	int n = 0;
	for(int i = 0; prefix[i]; i++)
		path[n++] = (uint8_t)prefix[i];
	for(int i = 0; ext[i]; i++)
		path[n++] = (uint8_t)ext[i];
	path[n] = 0;
}

static int os_list(void *arg)
{
	list_ctx_t *c = arg;
	gfs_scratch_t *s = c->s;

	for(int k = 0; k < 2; k++) {
		list_one_t *l = &c->l[k];
		if(!l->out || l->max <= 0)
			continue;

		pattern(s->path, l->ext);
		int sh = -1;
		int rc = g_os.find_first(s->path, &sh, s->found, &s->info);

		while(rc >= 0 && l->count < l->max) {
			int t = s->info.type;
			bool is_file = t != BFile_Type_Directory && t != BFile_Type_Dot
				&& t != BFile_Type_DotDot && t != BFile_Type_Volume;

			/* FONTCHARACTER -> ASCII (skip names we cannot represent) */
			gfs_entry_t *e = &l->out[l->count];
			int n = 0;
			bool ok = is_file;
			for(; ok && s->found[n]; n++) {
				uint16_t ch = s->found[n];
				if(n >= GFS_LIST_NAME - 1 || ch < 0x20 || ch >= 0x7f)
					ok = false;
				else
					e->name[n] = (char)ch;
			}
			if(ok && n > 0) {
				e->name[n] = 0;
				e->size = (int)s->info.file_size;
				l->count++;
			}
			rc = g_os.find_next(sh, s->found, &s->info);
		}
		/* Final review: the launcher says when a list is cut */
		l->more = (rc >= 0 && l->count >= l->max);
		if(sh >= 0)
			g_os.find_close(sh);
	}
	return 0;
}

int gfs_list(gfs_entry_t *wads, int wad_max, int *wad_count,
	gfs_entry_t *demos, int demo_max, int *demo_count)
{
	*wad_count = 0;
	*demo_count = 0;
	gfs_list_more = 0;
	if(!gsys_files_supported())
		return -1;

	list_ctx_t ctx = {
		.l = {
			{ .ext = ".wad", .out = wads, .max = wad_max },
			{ .ext = ".lmp", .out = demos, .max = demo_max },
		},
		.s = gfs_scratch(),
	};
	gfs_os_call(os_list, &ctx);

	*wad_count = ctx.l[0].count;
	*demo_count = ctx.l[1].count;
	gfs_list_more = (ctx.l[0].more ? GFS_MORE_WADS : 0)
		| (ctx.l[1].more ? GFS_MORE_DEMOS : 0);
	return 0;
}
