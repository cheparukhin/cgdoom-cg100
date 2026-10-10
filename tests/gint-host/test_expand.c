#include <gint/gint.h>
#include "cgd_gint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test of the default (weak) cgd_expand_build_pal32 and
   cgd_expand_p8_rgb565_p32 in src/gint/video.c, and of PL_SetPalette's
   persistent table through the CPU-independent pieces:
   - the table layout is the one of src/engine/sh4/cgd_sh4.h
     (pal32[k] = pal[k ^ 0x80] * 0x00010001);
   - a whole frame (DMA mode, one call) and 10-row strips (strip mode)
     expand to the big-endian memory image the SH would see;
   - a count that is not a positive multiple of 16 writes nothing (as the
     asm entry). */
void PL_Log(const char *f, ...) { (void)f; }
bool gfs_is_p1(void const *p, size_t s) { (void)p; (void)s; return false; }

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

#define NPIX (320 * 200)
static uint8_t src[NPIX] __attribute__((aligned(32)));
static uint8_t be[NPIX] __attribute__((aligned(32)));
static uint16_t dst[NPIX + 16] __attribute__((aligned(32)));
static uint32_t pal32[256] __attribute__((aligned(32)));

/* Compare words the way the SH sees memory: the source words are
   byte-swapped (be[]) so that native word reads on a little-endian host
   give the big-endian pixel order, and each 32-bit output word holds the
   left pixel in its high half. */
static int check_image(uint16_t const *pal, int first, int n)
{
	int bad = 0;
	uint32_t const *w = (void *)(dst + first);
	for(int i = 0; i < n; i += 2) {
		uint32_t want = ((uint32_t)pal[src[first + i]] << 16)
			| pal[src[first + i + 1]];
		if(w[i / 2] != want)
			bad++;
	}
	return bad;
}

int main(void)
{
	uint16_t pal[256];
	for(int i = 0; i < 256; i++) pal[i] = (uint16_t)(i * 251 + 7);
	for(int i = 0; i < NPIX; i++) src[i] = (uint8_t)rand();
	for(int i = 0; i < NPIX; i += 4) {
		be[i] = src[i+3]; be[i+1] = src[i+2];
		be[i+2] = src[i+1]; be[i+3] = src[i];
	}

	cgd_expand_build_pal32(pal32, pal);
	int badt = 0;
	for(int k = 0; k < 256; k++)
		badt += pal32[k] != (uint32_t)pal[k ^ 0x80] * 0x00010001u;
	CHECK(badt == 0, "pal32 layout (%d bad entries)", badt);

	/* DMA mode: one call for the frame */
	memset(dst, 0x5a, sizeof dst);
	cgd_expand_p8_rgb565_p32(dst, be, pal32, NPIX);
	int bad = check_image(pal, 0, NPIX);
	CHECK(bad == 0, "frame: %d bad words", bad);
	CHECK(dst[NPIX] == 0x5a5a, "nothing written past the frame");

	/* Strip mode: 20 calls of 3200 pixels */
	memset(dst, 0, sizeof dst);
	for(int s = 0; s < 20; s++)
		cgd_expand_p8_rgb565_p32(dst + s * 3200, be + s * 3200, pal32,
			3200);
	bad = check_image(pal, 0, NPIX);
	CHECK(bad == 0, "strips: %d bad words", bad);

	/* Palette change: rebuilding the table changes the output */
	for(int i = 0; i < 256; i++) pal[i] = (uint16_t)(0xffff - i * 3);
	cgd_expand_build_pal32(pal32, pal);
	cgd_expand_p8_rgb565_p32(dst, be, pal32, NPIX);
	bad = check_image(pal, 0, NPIX);
	CHECK(bad == 0, "after palette change: %d bad words", bad);

	/* Invalid counts write nothing */
	int const odd[] = { 0, -16, 8, 17, 3201 };
	for(size_t i = 0; i < sizeof odd / sizeof *odd; i++) {
		memset(dst, 0x5a, 64);
		cgd_expand_p8_rgb565_p32(dst, be, pal32, odd[i]);
		bool untouched = true;
		for(int j = 0; j < 32; j++)
			untouched &= dst[j] == 0x5a5a;
		CHECK(untouched, "n = %d: nothing written", odd[i]);
	}

	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
