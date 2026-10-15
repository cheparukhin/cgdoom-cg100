/* test_globals.h - the engine globals the drawers read, with CGDoom's declarations
   (r_draw.c, r_main.c, r_data.c), defined by test_sh4.c. The screen size (SCREENWIDTH,
   SCREENHEIGHT) comes from src/platform/cgd_config.h, as in the engine: the test is built once
   per size (Makefile TV=...). */
#ifndef TEST_GLOBALS_H
#define TEST_GLOBALS_H

#include <stdint.h>
#include "cgd_config.h"

typedef unsigned char byte;
typedef byte lighttable_t;
typedef int fixed_t;

#define MAXWIDTH     1120
#define MAXHEIGHT    832

extern const lighttable_t *dc_colormap;
extern int dc_x, dc_yl, dc_yh;
extern fixed_t dc_iscale, dc_texturemid;
extern const byte *dc_source;
extern byte *dc_translation;
extern int ds_y, ds_x1, ds_x2;
extern const lighttable_t *ds_colormap;
extern fixed_t ds_xfrac, ds_yfrac, ds_xstep, ds_ystep;
extern const byte *ds_source;
extern byte *ylookup[MAXHEIGHT];
extern int columnofs[MAXWIDTH];
extern int centery, viewheight;
extern const lighttable_t *colormaps;
extern int fuzzpos;

void ref_R_DrawColumn(void);
void ref_R_DrawColumnLow(void);
void ref_R_DrawFuzzColumn(void);
void ref_R_DrawFuzzColumnLow(void);
void ref_R_DrawTranslatedColumn(void);
void ref_R_DrawTranslatedColumnLow(void);
void ref_R_DrawSpan(void);
void ref_R_DrawSpanLow(void);
void ref_expand(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n);

#endif
