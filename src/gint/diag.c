/* CGDoom gint platform: diagnostics screen, microbenchmarks, PL_Log.

   Everything here exists to answer the questions that only hardware can
   answer (the hardware probes listed in docs/DEVELOPMENT.md §7.1): what
   the loader announced, which
   LCD controller we run on, where the stack is, the clock registers, the heap
   layout, the WAD block map (P1/P2/P0 counts, physical range), UTLB
   repairs, and flash vs SDRAM / display push timings. Shown on screen only
   (an fxlink text channel would need the USB driver and a connected host).

   Pages (LEFT/RIGHT): System, Memory, Storage/WAD (with a read-only
   block-map probe of a WAD), Benchmarks, Log. */

#include "cgd_gint.h"

#include <gint/display.h>
#include <gint/clock.h>
#include <gint/prof.h>
#include <gint/hardware.h>
#include <gint/config.h>
#include <gint/defs/util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not declared in gint's headers but exported by libgint */
extern uint16_t r61524_get_version(void);
extern uint32_t *gint_load_info;

//---
// PL_Log: ring buffer of the last lines (shown on the Log page)
//---

#define LOG_LINES 12
#define LOG_WIDTH 64
static char g_log[LOG_LINES][LOG_WIDTH];
static int g_log_next = 0, g_log_count = 0;

void PL_Log(const char *fmt, ...)
{
	char *line = g_log[g_log_next];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, LOG_WIDTH, fmt, args);
	va_end(args);
	/* One line per entry */
	for(char *c = line; *c; c++) {
		if(*c == '\n' || *c == '\r') {
			*c = 0;
			break;
		}
	}
	g_log_next = (g_log_next + 1) % LOG_LINES;
	if(g_log_count < LOG_LINES)
		g_log_count++;
}

//---
// Benchmarks
//---

typedef struct {
	bool done;
	int clock;
	uint32_t flash_us, sdram_us;
	uint32_t push_us;
} bench_t;

/* [0]: OS clock, [1]: the game's F4 overclock */
static bench_t g_bench[2];
static volatile uint32_t g_sink;

static uint32_t read_64k(void const *p)
{
	uint32_t const *w = p;
	uint32_t sum = 0;
	for(int i = 0; i < (64 << 10) / 4; i += 8) {
		sum += w[i] + w[i+1] + w[i+2] + w[i+3]
		     + w[i+4] + w[i+5] + w[i+6] + w[i+7];
	}
	return sum;
}

static void run_bench_once(bench_t *b, uint8_t const *fb)
{
	memset(b, 0, sizeof *b);
	b->clock = gclk_current();

	/* 64 KiB from flash through P1 (an OS region), cold: the lines are
	   invalidated first (read-only, no write-back) */
	uint8_t const *flash = (void *)0x80100000u;
	for(int off = 0; off < (64 << 10); off += 32)
		__asm__ volatile("ocbi @%0" :: "r"(flash + off) : "memory");
	synco();
	b->flash_us = prof_exec({ g_sink = read_64k(flash); });

	/* 64 KiB from SDRAM through P1, cold (purged after filling) */
	uint8_t *ram = malloc((64 << 10) + 32);
	if(ram) {
		uint8_t *r = (uint8_t *)(((uintptr_t)ram + 31) & ~(uintptr_t)31);
		memset(r, 0x5a, 64 << 10);
		for(int off = 0; off < (64 << 10); off += 32)
			__asm__ volatile("ocbp @%0" :: "r"(r + off) : "memory");
		synco();
		b->sdram_us = prof_exec({ g_sink = read_64k(r); });
		free(ram);
	}

	/* One 320x200 push with the game's display path: the async DMA, or
	   CPU writes without the frame buffer (the page says which) */
	if(fb) {
		gvid_clear_lcd();
		b->push_us = prof_exec({ gvid_push_test(fb); });
	}
	b->done = true;
}

static void run_bench(void)
{
	/* prof_exec() reads *prof_tcnt, NULL when gint's profiler found no
	   free TMU */
	if(!gtime_prof_ok())
		return;

	/* Gradient test frame and palette */
	uint8_t *fbraw = malloc(320 * 200 + 32);
	uint8_t *fb = NULL;
	if(fbraw) {
		fb = (uint8_t *)(((uintptr_t)fbraw + 31) & ~(uintptr_t)31);
		for(int y = 0; y < 200; y++)
		for(int x = 0; x < 320; x++)
			fb[320 * y + x] = (uint8_t)(x + y);
		uint16_t pal[256];
		for(int i = 0; i < 256; i++)
			pal[i] = C_RGB(i >> 3, (i >> 2) & 31, 31 - (i >> 3));
		PL_SetPalette(pal);
	}

	gclk_apply(GCLK_OFF);
	run_bench_once(&g_bench[0], fb);
	gclk_apply(GCLK_F4);
	run_bench_once(&g_bench[1], fb);
	gclk_apply(GCLK_OFF);
	free(fbraw);
}

//---
// Pages
//---

static int g_y;
#define LINE(...) do { ui_printf(8, g_y, UI_FG, DTEXT_LEFT, __VA_ARGS__); \
	g_y += 12; } while(0)

static void page_system(void)
{
	struct cpg_overclock_setting cpg;
	cpg_get_overclock_setting(&cpg);
	clock_frequency_t const *f = clock_freq();

	LINE("%s, OS %s", gsys_model_name(), g_os_version);
	LINE("gint %s, LCD controller id %d", GINT_VERSION,
		(int)r61524_get_version());
	LINE("r15 at entry: %08x", (unsigned)g_entry_r15);
	if(gint_load_info) {
		char buf[96];
		int n = snprintf(buf, sizeof buf, "loadinfo:");
		for(int i = 0; i < 8 && n < (int)sizeof buf - 10; i++) {
			n += snprintf(buf + n, sizeof buf - n, " %x",
				(unsigned)gint_load_info[i]);
			if(!gint_load_info[i])
				break;
		}
		LINE("%s", buf);
	}
	else
		LINE("loadinfo: none (no 'MPM0')");
	LINE("phys(00300000)=%08x phys(08100000)=%08x",
		(unsigned)gmem_stats.rom_phys, (unsigned)gmem_stats.uram_phys);
	LINE("Image %u KiB", (unsigned)(gmem_stats.image_size >> 10));
	LINE("FRQCR %08x FLLFRQ %08x speed F%d", (unsigned)cpg.FRQCR,
		(unsigned)cpg.FLLFRQ, clock_get_speed());
	LINE("CS0WCR %08x CS3WCR %08x CS3BCR %08x", (unsigned)cpg.CS0WCR,
		(unsigned)cpg.CS3WCR, (unsigned)cpg.CS3BCR);
	LINE("I %d.%02d  B %d.%02d  P %d.%02d MHz",
		f->Iphi_f / 1000000, (f->Iphi_f / 10000) % 100,
		f->Bphi_f / 1000000, (f->Bphi_f / 10000) % 100,
		f->Pphi_f / 1000000, (f->Pphi_f / 10000) % 100);
	LINE("UTLB: %d pages, handler %s, misses %u",
		gtlb_stats.pages, gtlb_stats.handler ? "on" : "off",
		(unsigned)*gtlb_stats.misses);
	LINE("UTLB checks %d, repairs %d; world switches %d",
		gtlb_stats.checks, gtlb_stats.repairs, gfs_world_switches);
	if(gtlb_stats.pinned)
		LINE("UTLB pinned %d pages: URB %d (was %d), URB resets %d",
			gtlb_stats.pinned, gtlb_stats.urb, gtlb_stats.urb_orig,
			gtlb_stats.urb_fixes);
	else if(gtlb_stats.handler && !gtlb_stats.executor)
		LINE("UTLB executor off: not pinned, URB %d", gtlb_stats.urb_orig);
	else
		LINE("UTLB pages not pinned (URB %d)", gtlb_stats.urb_orig);
	LINE("Clock now: %s; games: %s", gclk_name(gclk_current()),
		gtlb_stats.safe ? "OS clock (safe start)" : "F4");
	/* Whether the 35 Hz TMU tic was obtained (one of the hardware probes
	   listed in docs/DEVELOPMENT.md §7.1) */
	if(gtime_ticking())
		LINE("Tic timer: TMU %d (35 Hz)%s", gtime_tic_timer(),
			gtlb_stats.safe ? "; SAFE START" : "");
	else
		LINE("Tic timer: none (polled clock)%s",
			gtlb_stats.safe ? "; SAFE START" : "");
}

static void page_memory(void)
{
	LINE("Arena     start    end       free   largest");
	for(int i = 0;; i++) {
		char const *name;
		uintptr_t s, e;
		size_t fr, lg;
		if(!gmem_arena_info(i, &name, &s, &e, &fr, &lg))
			break;
		if(!name)
			continue;
		LINE("%-8s %08x %08x %5uk %5uk", name, (unsigned)s, (unsigned)e,
			(unsigned)(fr >> 10), (unsigned)(lg >> 10));
	}
	LINE("Total free %u KiB, largest block %u KiB",
		(unsigned)(gmem_total_free() >> 10),
		(unsigned)(gmem_largest_free() >> 10));
	LINE("OS heap arena: %s", gmem_stats.os_heap_removed ? "removed" :
		"present");
	if(!gmem_stats.zone_count)
		LINE("Zones and malloc reserve: set up when a game starts.");
	if(gmem_stats.zone_count) {
		for(int i = 0; i < gmem_stats.zone_count; i++)
			LINE("Zone %d: %08x, %d KiB", i,
				(unsigned)gmem_stats.zone_base[i],
				gmem_stats.zone_size[i] >> 10);
		LINE("malloc reserve: %d KiB", gmem_stats.reserve >> 10);
	}
}

/* Storage probe: open a WAD in block-map mode from the launcher, without
   the RAM image, and close it again (nothing is written), so the Storage
   page shows the Bfile_GetBlockAddress results and the UTLB slots the
   world switches changed (hardware probes listed in docs/DEVELOPMENT.md
   §7.1) before any game. The engine is not re-entrant: after a game the
   add-in exits, so this is the only way to see them on this screen. */
static gfs_entry_t const *g_wads;
static int g_wad_count, g_wad_sel;
static char g_probe_name[GFS_LIST_NAME];
static int g_probe_rc;
static int g_utlb_changed, g_utlb_shown;
static char g_utlb_slots[56];
static int g_urb_before, g_urb_after;

static void probe_wad(void)
{
	if(g_wad_sel < 0 || g_wad_sel >= g_wad_count)
		return;
	char const *name = g_wads[g_wad_sel].name;
	static gtlb_view_t before, after;
	gwad_ram_allowed = false;

	ui_status("Storage probe", name, "Mapping the WAD (read-only)...");
	gtlb_read_view(&before);
	g_urb_before = gtlb_mmucr_urb(*(volatile uint32_t *)0xff000010u);
	g_probe_rc = PL_WadOpen(name);
	gtlb_read_view(&after);
	g_urb_after = gtlb_mmucr_urb(*(volatile uint32_t *)0xff000010u);
	PL_WadClose();

	gwad_ram_allowed = true;
	strcpy(g_probe_name, name);

	g_utlb_changed = 0;
	g_utlb_shown = 0;
	int n = 0;
	g_utlb_slots[0] = 0;
	for(int i = 0; i < GTLB_SLOTS; i++) {
		if(before.addr[i] == after.addr[i] && before.data[i] == after.data[i])
			continue;
		g_utlb_changed++;
		if(n < (int)sizeof g_utlb_slots - 4) {
			n += snprintf(g_utlb_slots + n, sizeof g_utlb_slots - n,
				" %d", i);
			g_utlb_shown++;
		}
	}
}

static void page_wad(void)
{
	gwad_stats_t const *w = &gwad_stats;
	if(g_probe_name[0])
		LINE("Probe of %s: %s", g_probe_name, g_probe_rc < 0
			? gwad_error_text(g_probe_rc) : "opened and closed");
	else if(w->mode == GWAD_NONE) {
		LINE("No WAD opened yet: a game opens one. Here, EXE/OK");
		LINE("maps a WAD read-only (UP/DOWN: which one).");
	}
	if(g_wad_count > 0)
		LINE("EXE/OK: probe %s", g_wads[g_wad_sel].name);
	if(g_probe_name[0]) {
		LINE("UTLB: %d slots changed, URB %d -> %d", g_utlb_changed,
			g_urb_before, g_urb_after);
		if(g_utlb_changed)
			LINE("  slots:%s%s", g_utlb_slots,
				g_utlb_shown < g_utlb_changed ? " ..." : "");
	}
	if(w->mode == GWAD_NONE)
		return;
	LINE("WAD mode: %s%s, %d bytes", gwad_mode_name(),
		w->map_rejected ? " (map did not match)" : "", w->size);
	if(w->mode == GWAD_RAM || w->ram_failed) {
		LINE("RAM image: %d KiB, %d segments%s", w->ram_bytes >> 10,
			w->ram_segments, w->ram_failed ? " (did not fit)" : "");
		LINE("Left in storage: %d lumps, %d KiB", w->ram_skipped,
			w->ram_skipped_bytes >> 10);
	}
	if(w->blocks) {
		LINE("Blocks %d: mapped %d, P1 %d P2 %d P0 %d", w->blocks,
			w->mapped, w->raw_p1, w->raw_p2, w->raw_p0);
		LINE("  other %d, error %d, unaligned %d", w->raw_other,
			w->raw_error, w->rej_unaligned);
		LINE("Runs %d, longest %d blocks", w->runs, w->longest_run);
		LINE("Physical %08x - %08x", (unsigned)w->phys_min,
			(unsigned)w->phys_max);
	}
	LINE("Load/map time: %d ms", w->load_ms);
	LINE("Map hits %d, misses %d, BFile fallbacks %d", w->map_hits,
		w->map_misses, w->fallback_reads);
	LINE("Read errors %d; pending writes %d, failed %d", w->read_errors,
		gfs_pending_count(), gfs_flush_failures());
}

static void page_bench(void)
{
	LINE("EXE/OK: run at the OS clock and at %s",
		gclk_name(GCLK_F4));
	for(int k = 0; k < 2; k++) {
		bench_t const *b = &g_bench[k];
		if(!b->done)
			continue;
		LINE("-- %s --", gclk_name(b->clock));
		LINE("64 KiB read: flash P1 %u us, SDRAM P1 %u us (cold)",
			(unsigned)b->flash_us, (unsigned)b->sdram_us);
		LINE("Push 320x200 (%s): %u us", gvid_dma_available() ? "DMA"
			: "CPU, no DMA frame buffer", (unsigned)b->push_us);
	}
	if(!gtime_prof_ok())
		LINE("Profiler timer unavailable: no benchmarks");
	else if(!g_bench[0].done)
		LINE("(not run yet)");
}

static void page_log(void)
{
	if(!g_log_count)
		LINE("(empty)");
	for(int i = 0; i < g_log_count; i++) {
		int idx = (g_log_next - g_log_count + i + LOG_LINES) % LOG_LINES;
		LINE("%s", g_log[idx]);
	}
}

void gdiag_screen(gfs_entry_t const *wads, int count)
{
	g_wads = wads;
	g_wad_count = count;
	if(g_wad_sel >= count)
		g_wad_sel = 0;
	static char const *titles[] = {
		"Diagnostics 1/5: system", "Diagnostics 2/5: memory",
		"Diagnostics 3/5: storage", "Diagnostics 4/5: benchmarks",
		"Diagnostics 5/5: log",
	};
	int page = 0;

	while(1) {
		ui_begin();
		drect(0, 0, DWIDTH - 1, 17, UI_TITLE);
		ui_text(DWIDTH / 2, 4, UI_BG, DTEXT_CENTER, titles[page]);
		g_y = 24;
		switch(page) {
		case 0: page_system(); break;
		case 1: page_memory(); break;
		case 2: page_wad(); break;
		case 3: page_bench(); break;
		case 4: page_log(); break;
		}
		drect(0, DHEIGHT - 14, DWIDTH - 1, DHEIGHT - 1, UI_HEAD);
		ui_text(DWIDTH / 2, DHEIGHT - 12, UI_BG, DTEXT_CENTER,
			"LEFT/RIGHT: page   BACK: return");
		ui_end();

		int k = ginp_getkey();
		if(k == GK_LEFT)
			page = (page + 4) % 5;
		else if(k == GK_RIGHT)
			page = (page + 1) % 5;
		else if(k == GK_OK && page == 3) {
			ui_status("Benchmarks", "Running...", NULL);
			run_bench();
		}
		else if(k == GK_OK && page == 2)
			probe_wad();
		else if((k == GK_UP || k == GK_DOWN) && page == 2 && count > 0)
			g_wad_sel = (g_wad_sel + (k == GK_UP ? count - 1 : 1)) % count;
		else if(k == GK_BACK || k == GK_QUIT)
			break;
	}
}
