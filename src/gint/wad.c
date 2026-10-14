/* CGDoom gint platform: WAD access (PL_Wad*).

   Three backends, chosen by PL_WadOpen():

   1. RAM image (primary path). NOR flash line fills are several times
      slower than SDRAM and overclocking does not speed them up, so when the
      heap allows it every lump the engine can use is copied to RAM:
      - the directory is read first; sound/music lumps (DSxx/DPxx outside the
        sprite/flat/patch namespaces, D_xx, GENMIDI, DMXxx) stay in storage;
      - the remaining segments (header, lumps, directory) are read with one
        BFile_Read per run of file-contiguous segments, all inside ONE world
        switch, packed in a single heap block;
      - each segment is then moved (memmove, back to front) to a 4-byte
        aligned position, so every lump pointer is 4-aligned;
      - lookups binary-search the sorted segment table. Ranges that fall in
        a skipped lump are served by a world-switched BFile_Read.
      Used when the image fits in the largest free block and the heap keeps
      CGD_WAD_RAM_MARGIN (malloc reserve + minimum zone) afterwards.

   2. Block map (fallback). Bfile_GetBlockAddress for every 4 KiB block, all
      in ONE world switch (gpSP nor_rom.c). Pointers are sanitized like gpSP:
      P2 flash (0xa0000000-0xa1ffffff) is converted to its cached P1 alias,
      P1 flash (0x80000000-0x81ffffff) is kept, everything else (P0/TLB
      addresses such as 0x001129xx, errors, NULL) and unaligned pointers are
      rejected; freshly mapped lines are invalidated (ocbi) once, then
      synco. Physically contiguous runs are detected; PL_WadMap() returns a
      pointer only when the whole range is inside one run. PL_WadRead()
      gathers with memcpy and falls back to one world-switched BFile_Read
      when a rejected block is involved.
      The map is valid until the next storage write: all writes are delayed
      until after PL_WadClose() (files.c refuses to flush while it is live).

   3. BFile (when nothing else works: no RAM image and the block map is
      unavailable or did not match the file): every read is a world-switched
      BFile_Read. Slow.

   The file stays open until PL_WadClose() in every mode (gwad_quit() on
   the abort() path, gint review round 3.1). */

#include "cgd_gint.h"

#include <gint/gint.h>
#include <gint/rtc.h>
#include <gint/defs/util.h>
#include <stdlib.h>
#include <string.h>

gwad_stats_t gwad_stats;
bool gwad_ram_allowed = true;

#define BLOCK_SHIFT 12
#define BLOCK_SIZE  (1 << BLOCK_SHIFT)

typedef struct {
	uint32_t off;
	uint32_t size;
	uint8_t *ram;
} seg_t;

static struct {
	gwad_mode_t mode;
	char name[GFS_LIST_NAME];
	int fd;
	int size;
	uint8_t header[12];

	/* RAM image */
	uint8_t *ram;
	uint32_t ram_size;
	seg_t *segs;
	int nsegs;

	/* Block map */
	uint8_t const **blk;
	uint16_t *run_end;
	int nblocks;
	uintptr_t map_lo, map_hi;
} W = { .fd = -1 };

static uint32_t le32(uint8_t const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int rtc_ms(uint32_t t0)
{
	return (int)((rtc_ticks() - t0) * 1000 / 128);
}

static bool same_name(char const *a, char const *b)
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

char const *gwad_mode_name(void)
{
	switch(gwad_stats.mode) {
	case GWAD_RAM:   return "RAM image";
	case GWAD_MAP:   return "flash block map";
	case GWAD_BFILE: return "BFile reads";
	default:         return "none";
	}
}

bool gwad_map_live(void)
{
	return W.mode == GWAD_MAP;
}

//---
// OS-world helpers
//---

typedef struct {
	uint16_t const *path;
	int fd, size;
	uint8_t *header;      /* 12 bytes, P1 */
	int header_rc;
} open_ctx_t;

static int os_open(void *arg)
{
	open_ctx_t *c = arg;
	c->size = -1;
	c->header_rc = -1;
	c->fd = g_os.open(c->path, BFile_ReadOnly, 0);
	if(c->fd < 0)
		return 0;
	c->size = g_os.size(c->fd);
	if(c->size >= 12)
		c->header_rc = g_os.read(c->fd, c->header, 12, 0);
	return 0;
}

static int os_close(void *arg)
{
	return g_os.close(*(int *)arg);
}

typedef struct {
	int fd;
	void *buf;
	int size, offset;
	int file_size;
	bool ok;
} read_ctx_t;

static int os_read_at(void *arg)
{
	read_ctx_t *c = arg;
	int rc = g_os.read(c->fd, c->buf, c->size, c->offset);
	c->ok = gfs_read_ok(rc, c->size, c->file_size - c->offset - c->size);
	return rc;
}

/* World-switched read of [offset, offset+size) into dst */
static int bfile_read_range(void *dst, int offset, int size)
{
	if(W.fd < 0 || size <= 0)
		return (size == 0) ? 0 : -1;

	void *buf = dst;
	if(!gfs_is_p1(dst, size)) {
		buf = malloc(size);
		if(!buf)
			return -1;
	}

	read_ctx_t c = {
		.fd = W.fd, .buf = buf, .size = size, .offset = offset,
		.file_size = W.size,
	};
	gfs_os_call(os_read_at, &c);
	gwad_stats.fallback_reads++;

	if(buf != dst) {
		if(c.ok)
			memcpy(dst, buf, size);
		free(buf);
	}
	if(!c.ok) {
		gwad_stats.read_errors++;
		return -1;
	}
	return size;
}

//---
// RAM image
//---

typedef struct {
	int fd;
	int file_size;
	uint8_t *base;
	seg_t *segs;          /* .ram holds the packed read position */
	int nsegs;
	int errors;
} ram_read_ctx_t;

static int os_read_segments(void *arg)
{
	ram_read_ctx_t *c = arg;
	int i = 0;
	while(i < c->nsegs) {
		/* Run of file-contiguous segments, packed contiguously too */
		int j = i + 1;
		uint32_t end = c->segs[i].off + c->segs[i].size;
		while(j < c->nsegs && c->segs[j].off == end) {
			end += c->segs[j].size;
			j++;
		}
		int size = end - c->segs[i].off;
		int rc = g_os.read(c->fd, c->segs[i].ram, size, c->segs[i].off);
		if(!gfs_read_ok(rc, size, c->file_size - (int)end))
			c->errors++;
		i = j;
	}
	return 0;
}

/* Pads whose following file bytes are not the next segment: read them
   directly into their final place (second world switch) */
typedef struct {
	int fd;
	int file_size;
	seg_t const *segs;
	int const *list;
	int count;
	int errors;
} pad_read_ctx_t;

static int os_read_pads(void *arg)
{
	pad_read_ctx_t *c = arg;
	for(int k = 0; k < c->count; k++) {
		seg_t const *sg = &c->segs[c->list[k]];
		int from = sg->off + sg->size;
		int n = c->file_size - from;
		if(n > GWAD_LUMP_PAD)
			n = GWAD_LUMP_PAD;
		if(n <= 0)
			continue;
		int rc = g_os.read(c->fd, sg->ram + sg->size, n, from);
		if(!gfs_read_ok(rc, n, c->file_size - from - n))
			c->errors++;
	}
	return 0;
}

static bool name_is(uint8_t const *n, char const *s)
{
	int len = (int)strlen(s);    /* at most 8 */
	for(int i = 0; i < 8; i++) {
		char c = (n[i] >= 'a' && n[i] <= 'z') ? n[i] - 32 : n[i];
		char e = (i < len) ? s[i] : 0;
		if(c != e)
			return false;
		if(!e)
			return true;
	}
	return true;
}

/* Lumps CGDoom never reads (no sound): left in storage */
static bool skip_lump(uint8_t const *n, int size, bool in_namespace)
{
	if(size <= 0 || in_namespace)
		return false;
	if((n[0] == 'D' && (n[1] == 'S' || n[1] == 'P' || n[1] == '_')))
		return true;
	if(name_is(n, "GENMIDI") || (n[0] == 'D' && n[1] == 'M' && n[2] == 'X'))
		return true;
	return false;
}

static void sort_segs(seg_t *s, int n)
{
	/* Shell sort by file offset (directory order is usually sorted) */
	for(int gap = n / 2; gap > 0; gap /= 2)
	for(int i = gap; i < n; i++) {
		seg_t x = s[i];
		int j = i;
		while(j >= gap && s[j - gap].off > x.off) {
			s[j] = s[j - gap];
			j -= gap;
		}
		s[j] = x;
	}
}

/* Segment i is followed in RAM by segment i+1 with no pad and no gap:
   adjacent in the file, and its size keeps the next one 4-aligned. Both
   are then laid out exactly as in the file, so the pad of segment i (the
   GWAD_LUMP_PAD bytes after it) is segment i+1 and what follows it, which
   is again either chained or a real pad. Overlap with the next segment is
   harmless: nothing writes into the image (PL_WadMap returns const). */
static bool seg_chained(seg_t const *segs, int n, int i)
{
	return i + 1 < n && segs[i+1].off == segs[i].off + segs[i].size
		&& (segs[i].size & 3) == 0;
}

static void ram_free(void)
{
	free(W.ram);
	free(W.segs);
	W.ram = NULL;
	W.segs = NULL;
	W.nsegs = 0;
	W.ram_size = 0;
}

static bool try_ram_image(void)
{
	uint32_t numlumps = le32(W.header + 4);
	uint32_t dirofs = le32(W.header + 8);
	uint32_t dirsize = numlumps * 16;
	if(numlumps == 0 || numlumps > 65536 || dirofs > (uint32_t)W.size
		|| dirsize > (uint32_t)W.size - dirofs)
		return false;

	/* Read the directory (one world switch) */
	uint8_t *dir = malloc(dirsize);
	if(!dir)
		return false;
	if(bfile_read_range(dir, dirofs, dirsize) != (int)dirsize) {
		free(dir);
		return false;
	}
	gwad_stats.fallback_reads--;   /* not a fallback */

	seg_t *segs = malloc((numlumps + 2) * sizeof *segs);
	if(!segs) {
		free(dir);
		return false;
	}

	/* Segments: header, directory, and every useful lump */
	int n = 0;
	segs[n++] = (seg_t){ 0, 12, NULL };
	segs[n++] = (seg_t){ dirofs, dirsize, NULL };
	int ns = 0;   /* namespace depth (sprites, flats, patches) */
	for(uint32_t i = 0; i < numlumps; i++) {
		uint8_t const *e = dir + 16 * i;
		uint32_t pos = le32(e), size = le32(e + 4);
		uint8_t const *name = e + 8;

		if(name_is(name, "S_START") || name_is(name, "SS_START")
		|| name_is(name, "F_START") || name_is(name, "FF_START")
		|| name_is(name, "P_START") || name_is(name, "PP_START"))
			ns++;
		else if((name_is(name, "S_END") || name_is(name, "SS_END")
		|| name_is(name, "F_END") || name_is(name, "FF_END")
		|| name_is(name, "P_END") || name_is(name, "PP_END")) && ns > 0)
			ns--;

		if(size == 0 || pos > (uint32_t)W.size || size > (uint32_t)W.size - pos)
			continue;
		if(skip_lump(name, size, ns > 0)) {
			gwad_stats.ram_skipped++;
			gwad_stats.ram_skipped_bytes += size;
			continue;
		}
		segs[n++] = (seg_t){ pos, size, NULL };
	}
	free(dir);

	/* Sort and merge overlapping segments (some WADs share lump data) */
	sort_segs(segs, n);
	int m = 0;
	for(int i = 0; i < n; i++) {
		if(m > 0 && segs[i].off < segs[m-1].off + segs[m-1].size) {
			uint32_t end = segs[i].off + segs[i].size;
			uint32_t pend = segs[m-1].off + segs[m-1].size;
			if(end > pend)
				segs[m-1].size = end - segs[m-1].off;
			continue;
		}
		segs[m++] = segs[i];
	}
	n = m;

	/* Final layout: every segment 4-aligned and followed by GWAD_LUMP_PAD
	   bytes holding the WAD bytes that follow it in the file (zeros past
	   the end), like the engine's heap copies (w_wad.h, W_LUMP_PAD).
	   A segment chained to the next one (seg_chained) needs no pad of its
	   own: the next segment follows it directly, so its pad bytes are the
	   true file bytes already (final review: a wadtool-packed WAD saved
	   ~150 KB of duplicated pads this way). */
	uint32_t total = 0;
	for(int i = 0; i < n; i++)
		total = ((total + 3) & ~3u) + segs[i].size
			+ (seg_chained(segs, n, i) ? 0 : GWAD_LUMP_PAD);
	total = (total + 31) & ~31u;

	/* Shrink the segment table before measuring the heap */
	seg_t *s2 = realloc(segs, n * sizeof *segs);
	if(s2)
		segs = s2;

	/* The image must fit in one block and leave the zone + malloc margin */
	size_t total_free = gmem_total_free();
	uint32_t *fo = malloc(n * sizeof *fo);
	int *padlist = malloc(n * sizeof *padlist);
	uint8_t *ram = NULL;
	if(fo && padlist && gmem_largest_free() >= total + 32
		&& total_free >= (size_t)total + CGD_WAD_RAM_MARGIN)
		ram = malloc(total);
	if(!ram) {
		free(fo);
		free(padlist);
		free(segs);
		return false;
	}

	/* Final offsets, and packed read positions (never above them) */
	uint32_t end = 0, pos = 0;
	for(int i = 0; i < n; i++) {
		fo[i] = (end + 3) & ~3u;
		end = fo[i] + segs[i].size
			+ (seg_chained(segs, n, i) ? 0 : GWAD_LUMP_PAD);
		segs[i].ram = ram + pos;
		pos += segs[i].size;
	}

	ui_status("Loading", W.name, "Copying the WAD to RAM...");
	uint32_t t0 = rtc_ticks();
	ram_read_ctx_t c = {
		.fd = W.fd, .file_size = W.size, .base = ram, .segs = segs,
		.nsegs = n,
	};
	gfs_os_call(os_read_segments, &c);

	if(c.errors || memcmp(ram, W.header, 12)) {
		gwad_stats.read_errors += c.errors;
		free(ram);
		free(fo);
		free(padlist);
		free(segs);
		return false;
	}

	/* Move every segment to its final position, back to front: final
	   offsets are never below packed offsets, so no unmoved data is
	   overwritten */
	for(int i = n - 1; i >= 0; i--) {
		uint8_t *dst = ram + fo[i];
		if(dst != segs[i].ram)
			memmove(dst, segs[i].ram, segs[i].size);
		segs[i].ram = dst;
	}

	/* Pads: zeros by default; read the ones not followed by the next
	   segment in the file (one more world switch), then copy the others
	   from the (complete) next segment, back to front */
	int npad = 0;
	for(int i = 0; i < n; i++) {
		/* A chained segment's pad is the next segment itself */
		if(seg_chained(segs, n, i))
			continue;
		uint32_t after = segs[i].off + segs[i].size;
		memset(segs[i].ram + segs[i].size, 0, GWAD_LUMP_PAD);
		bool next_adjacent = (i + 1 < n && segs[i+1].off == after);
		if(after < (uint32_t)W.size && !next_adjacent)
			padlist[npad++] = i;
	}
	if(npad > 0) {
		pad_read_ctx_t pc = {
			.fd = W.fd, .file_size = W.size, .segs = segs,
			.list = padlist, .count = npad,
		};
		gfs_os_call(os_read_pads, &pc);
		if(pc.errors) {
			gwad_stats.read_errors += pc.errors;
			free(ram);
			free(fo);
			free(padlist);
			free(segs);
			return false;
		}
	}
	for(int i = n - 2; i >= 0; i--) {
		if(seg_chained(segs, n, i))
			continue;
		if(segs[i+1].off == segs[i].off + segs[i].size)
			memcpy(segs[i].ram + segs[i].size, segs[i+1].ram,
				GWAD_LUMP_PAD);
	}
	gwad_stats.load_ms = rtc_ms(t0);
	free(fo);
	free(padlist);

	W.ram = ram;
	W.ram_size = total;
	W.segs = segs;
	W.nsegs = n;
	gwad_stats.ram_bytes = total;
	gwad_stats.ram_segments = n;
	return true;
}

/* Last segment with off <= offset, or NULL */
static seg_t const *seg_find(uint32_t offset)
{
	int lo = 0, hi = W.nsegs;
	while(lo < hi) {
		int mid = (lo + hi) >> 1;
		if(W.segs[mid].off <= offset)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo > 0) ? &W.segs[lo - 1] : NULL;
}

//---
// Block map
//---

typedef struct {
	int fd;
	int nblocks;
	void **raw;
	int errors;
} blk_ctx_t;

static int os_query_blocks(void *arg)
{
	blk_ctx_t *c = arg;
	for(int i = 0; i < c->nblocks; i++) {
		void *p = NULL;
		int rc = g_os.block(c->fd, i << BLOCK_SHIFT, &p);
		if(rc < 0) {
			p = NULL;
			c->errors++;
		}
		c->raw[i] = p;
	}
	return 0;
}

static void map_free(void)
{
	free(W.blk);
	free(W.run_end);
	W.blk = NULL;
	W.run_end = NULL;
	W.nblocks = 0;
	W.map_lo = W.map_hi = 0;
}

static void invalidate_block(uint8_t const *p)
{
#ifdef __sh__
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)31;
	uintptr_t end = (uintptr_t)p + BLOCK_SIZE;
	for(; a < end; a += 32)
		__asm__ volatile("ocbi @%0" :: "r"(a) : "memory");
#else
	(void)p;   /* host unit tests */
#endif
}

static bool build_map(void)
{
	int n = (W.size + BLOCK_SIZE - 1) >> BLOCK_SHIFT;
	if(n <= 0 || n > 65535 || !g_os.block)
		return false;

	map_free();
	W.blk = malloc(n * sizeof *W.blk);
	W.run_end = malloc(n * sizeof *W.run_end);
	if(!W.blk || !W.run_end) {
		map_free();
		return false;
	}
	memset(W.blk, 0, n * sizeof *W.blk);
	W.nblocks = n;

	gwad_stats.blocks = n;
	gwad_stats.mapped = 0;
	gwad_stats.raw_p1 = gwad_stats.raw_p2 = gwad_stats.raw_p0 = 0;
	gwad_stats.raw_other = gwad_stats.raw_error = 0;
	gwad_stats.rej_unaligned = 0;
	gwad_stats.phys_min = 0xffffffff;
	gwad_stats.phys_max = 0;

	uint32_t t0 = rtc_ticks();
	blk_ctx_t c = { .fd = W.fd, .nblocks = n, .raw = (void **)W.blk };
	gfs_os_call(os_query_blocks, &c);
	gwad_stats.load_ms = rtc_ms(t0);

	/* Sanitize (in place) */
	for(int i = 0; i < n; i++) {
		uintptr_t v = (uintptr_t)W.blk[i];
		uintptr_t p = 0;

		if(!v)
			gwad_stats.raw_error++;
		else if(v >= 0xa0000000u && v < 0xa2000000u) {
			gwad_stats.raw_p2++;
			p = v - 0x20000000u;
		}
		else if(v >= 0x80000000u && v < 0x82000000u) {
			gwad_stats.raw_p1++;
			p = v;
		}
		else if(v < 0x80000000u)
			gwad_stats.raw_p0++;
		else
			gwad_stats.raw_other++;

		if(p && (p & 3)) {
			gwad_stats.rej_unaligned++;
			p = 0;
		}
		if(p) {
			uint32_t phys = p & 0x1fffffffu;
			if(phys < gwad_stats.phys_min) gwad_stats.phys_min = phys;
			if(phys > gwad_stats.phys_max) gwad_stats.phys_max = phys;
			invalidate_block((uint8_t const *)p);
			gwad_stats.mapped++;
		}
		W.blk[i] = (uint8_t const *)p;
	}
	synco();
	if(gwad_stats.mapped == 0)
		gwad_stats.phys_min = 0;

	/* Contiguous runs, address range */
	gwad_stats.runs = 0;
	gwad_stats.longest_run = 0;
	W.map_lo = (uintptr_t)-1;
	W.map_hi = 0;
	for(int i = n - 1; i >= 0; i--) {
		if(W.blk[i] && i + 1 < n && W.blk[i + 1]
			&& W.blk[i + 1] == W.blk[i] + BLOCK_SIZE)
			W.run_end[i] = W.run_end[i + 1];
		else
			W.run_end[i] = i;
	}
	for(int i = 0; i < n; i++) {
		if(!W.blk[i])
			continue;
		if(i == 0 || W.run_end[i - 1] != W.run_end[i] || !W.blk[i - 1]) {
			gwad_stats.runs++;
			int len = W.run_end[i] - i + 1;
			if(len > gwad_stats.longest_run)
				gwad_stats.longest_run = len;
		}
		uintptr_t lo = (uintptr_t)W.blk[i];
		uintptr_t hi = lo + BLOCK_SIZE;
		if(lo < W.map_lo) W.map_lo = lo;
		if(hi > W.map_hi) W.map_hi = hi;
	}
	if(W.map_hi == 0)
		W.map_lo = 0;
	return true;
}

void gwad_remap(void)
{
	if(W.mode != GWAD_MAP)
		return;

	if(build_map() && W.blk[0] && !memcmp(W.blk[0], W.header, 12))
		return;

	/* The map cannot be trusted any more: plain BFile reads */
	map_free();
	W.mode = GWAD_BFILE;
	gwad_stats.mode = GWAD_BFILE;
}

//---
// PL_Wad* API
//---

/* Final review: the map is checked against the file before it is used.
   gwad_remap() compared block 0 with the header; the first open did not,
   so wrong Bfile_GetBlockAddress semantics on some OS build (offset units,
   stale pointers, fragment headers) would have fed the engine garbage.
   Block 0 must hold the header, and a few sample blocks (middle, last,
   the directory's) must match a BFile_Read of the same bytes (one world
   switch each; no storage write, so the map stays valid). Unmapped blocks
   (NULL) are read through BFile anyway and need no check. */
static bool map_block_ok(int b)
{
	if(b < 0 || b >= W.nblocks || !W.blk[b])
		return true;
	uint8_t buf[64];
	int off = b << BLOCK_SHIFT;
	int n = W.size - off;
	if(n > (int)sizeof buf)
		n = sizeof buf;
	if(n <= 0)
		return true;
	int got = bfile_read_range(buf, off, n);
	gwad_stats.fallback_reads--;   /* not a fallback */
	return got == n && !memcmp(W.blk[b], buf, n);
}

static bool map_verify(void)
{
	if(W.blk[0] && memcmp(W.blk[0], W.header, 12))
		return false;
	int dir_block = (int)(le32(W.header + 8) >> BLOCK_SHIFT);
	return map_block_ok(W.nblocks / 2) && map_block_ok(W.nblocks - 1)
		&& map_block_ok(dir_block);
}

int PL_WadOpen(const char *name)
{
	if(!name || strlen(name) >= GFS_LIST_NAME)
		return GWAD_ERR_NAME;

	if(W.mode != GWAD_NONE) {
		if(same_name(name, W.name))
			return W.size;
		PL_WadClose();
	}
	memset(&gwad_stats, 0, sizeof gwad_stats);

	gfs_scratch_t *s = gfs_scratch();
	if(!gfs_make_path(s->path, sizeof s->path / 2, name))
		return GWAD_ERR_NAME;
	open_ctx_t c = { .path = s->path, .header = s->small };
	gfs_os_call(os_open, &c);
	if(c.fd < 0)
		return GWAD_ERR_OPEN;

	W.fd = c.fd;
	W.size = c.size;
	if(c.size < 12 || !gfs_read_ok(c.header_rc, 12, c.size - 12)) {
		PL_WadClose();
		return GWAD_ERR_SHORT;
	}
	if(memcmp(s->small, "IWAD", 4) && memcmp(s->small, "PWAD", 4)) {
		PL_WadClose();
		return GWAD_ERR_MAGIC;
	}
	/* The directory must be inside the file (a truncated copy is the
	   likely cause otherwise; final review) */
	uint32_t numlumps = le32(s->small + 4), dirofs = le32(s->small + 8);
	if(dirofs > (uint32_t)c.size
		|| numlumps > ((uint32_t)c.size - dirofs) / 16) {
		PL_WadClose();
		return GWAD_ERR_TRUNCATED;
	}
	memcpy(W.header, s->small, 12);
	strcpy(W.name, name);
	gwad_stats.size = W.size;

	if(gwad_ram_allowed && try_ram_image())
		W.mode = GWAD_RAM;
	else if(g_os.block && build_map()) {
		W.mode = GWAD_MAP;
		if(!map_verify()) {
			PL_Log("WAD map does not match the file: BFile reads\n");
			map_free();
			gwad_stats.map_rejected = true;
			W.mode = GWAD_BFILE;
		}
	}
	else
		W.mode = GWAD_BFILE;

	if(W.mode != GWAD_RAM && gwad_ram_allowed)
		gwad_stats.ram_failed = true;
	gwad_stats.mode = W.mode;
	return W.size;
}

char const *gwad_error_text(int rc)
{
	switch(rc) {
	case GWAD_ERR_NAME:
		return "The file name is too long or has characters CGDoom "
			"cannot use. Rename it (for example doom1.wad).";
	case GWAD_ERR_OPEN:
		return "The file could not be opened. Copy it again.";
	case GWAD_ERR_SHORT:
		return "The file is too small to be a WAD: probably an "
			"incomplete copy. Copy it again.";
	case GWAD_ERR_MAGIC:
		return "This is not a WAD file (no IWAD/PWAD header).";
	case GWAD_ERR_TRUNCATED:
		return "The WAD is truncated (its directory is past the end of "
			"the file): copy it again, completely.";
	default:
		return "The file could not be read as a WAD.";
	}
}

void PL_WadClose(void)
{
	if(W.fd >= 0) {
		int fd = W.fd;
		gfs_os_call(os_close, &fd);
	}
	W.fd = -1;
	ram_free();
	map_free();
	W.mode = GWAD_NONE;
	W.name[0] = 0;
	W.size = 0;
}

void gwad_quit(void)
{
	/* fxlibc's abort() closes no file ("TODO: Close BFile handles
	   (essential)"); gint is still fully up while .dtors run */
	if(W.fd >= 0) {
		int fd = W.fd;
		W.fd = -1;
		gfs_os_call(os_close, &fd);
	}
}

/* Both entry points are quit-chord safe points (gint review round 3.2):
   the startup R_Init scan, level setup with R_PrecacheLevel and game loads
   read the WAD all along but never call PL_GetTics, so a slow load held the
   chord into the 2 s abort() fallback and lost the session's saves. The
   poll comes first and may not return (main.c leave_to_exit_path(), which
   closes the WAD); nothing has been touched yet. PL_WadOpen() reads
   through bfile_read_range() and is not a poll point. */
int PL_WadRead(void *dst, int offset, int size)
{
	gquit_poll_load();
	if(W.mode == GWAD_NONE || offset < 0 || size < 0 || offset > W.size)
		return -1;
	if(size > W.size - offset)
		size = W.size - offset;
	if(size == 0)
		return 0;

	uint8_t *out = dst;
	uint32_t pos = offset;
	int left = size;

	if(W.mode == GWAD_RAM) {
		while(left > 0) {
			/* A segment and its pad hold the true file bytes */
			seg_t const *sg = seg_find(pos);
			if(!sg || pos >= sg->off + sg->size + GWAD_LUMP_PAD)
				break;
			int n = sg->off + sg->size + GWAD_LUMP_PAD - pos;
			if(n > left)
				n = left;
			memcpy(out, sg->ram + (pos - sg->off), n);
			out += n;
			pos += n;
			left -= n;
		}
	}
	else if(W.mode == GWAD_MAP) {
		/* Only memcpy if every block of the range is mapped */
		int b0 = pos >> BLOCK_SHIFT;
		int b1 = (pos + left - 1) >> BLOCK_SHIFT;
		bool all = true;
		for(int b = b0; b <= b1; b = W.run_end[b] + 1) {
			if(!W.blk[b]) {
				all = false;
				break;
			}
		}
		while(all && left > 0) {
			int b = pos >> BLOCK_SHIFT;
			uint32_t run_end = ((uint32_t)W.run_end[b] + 1) << BLOCK_SHIFT;
			int n = run_end - pos;
			if(n > left)
				n = left;
			memcpy(out, W.blk[b] + (pos & (BLOCK_SIZE - 1)), n);
			out += n;
			pos += n;
			left -= n;
		}
	}

	if(left > 0 && bfile_read_range(out, pos, left) != left)
		return size - left;
	return size;
}

const void *PL_WadMap(int offset, int size)
{
	gquit_poll_load();
	if(offset < 0 || size <= 0 || offset > W.size || size > W.size - offset)
		return NULL;

	void const *p = NULL;
	if(W.mode == GWAD_RAM) {
		seg_t const *sg = seg_find(offset);
		if(sg && (uint32_t)offset + size
			<= sg->off + sg->size + GWAD_LUMP_PAD)
			p = sg->ram + (offset - sg->off);
	}
	else if(W.mode == GWAD_MAP) {
		int b0 = offset >> BLOCK_SHIFT;
		int b1 = (offset + size - 1) >> BLOCK_SHIFT;
		/* A range ending at EOF is one the engine clamped (it asks for
		   size + W_LUMP_PAD, cut at the end of the WAD): the renderer may
		   read up to W_LUMP_PAD bytes past it, which in flash are the
		   rest of the cluster or another file, where the heap copy and
		   the RAM image have zeros (w_wad.h). Decline: the engine makes a
		   zero-padded heap copy, only ever for the lump at EOF (gint
		   review round 2). A range clamped at an engine wad_end below
		   W.size cannot be recognised here; RAM mode behaves the same. */
		bool at_eof = (uint32_t)offset + (uint32_t)size
			>= (uint32_t)W.size;
		if(!at_eof && W.blk[b0] && W.run_end[b0] >= b1)
			p = W.blk[b0] + (offset & (BLOCK_SIZE - 1));
	}

	if(p)
		gwad_stats.map_hits++;
	else
		gwad_stats.map_misses++;
	return p;
}

int PL_IsWadPointer(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	if(W.mode == GWAD_RAM)
		return a - (uintptr_t)W.ram < W.ram_size;
	if(W.mode == GWAD_MAP)
		return a >= W.map_lo && a < W.map_hi;
	return 0;
}

/* platform.h: 1 when lumps live in NOR flash (block map) or are read from
   the file (BFile), so the engine keeps hot lumps in the zone; 0 for the RAM
   image. Fixed between PL_WadOpen and PL_WadClose: gwad_remap() only ever
   turns MAP into BFILE, and both are slow (gint review round 3.3; the
   engine's weak default was 0, so flash lumps were always used in place). */
int PL_WadIsSlow(void)
{
	return W.mode == GWAD_MAP || W.mode == GWAD_BFILE;
}
