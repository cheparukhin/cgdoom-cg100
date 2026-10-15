/* r_draw16_sh4.S - SH4AL-DSP (SH-4A, big-endian) column and span drawers for CGDoom, RGB565
   pixels (CGD_TRUECOLOR; the 8-bit drawers are in r_draw_sh4.S).

   Drop-in replacements for the r_draw.c drawers of the true-colour build: void(void), reading
   the same engine globals, bit-exact output (see test/ref_draw.c and test/test_sh4.c). The
   screens and the light tables (colormaps, dc_colormap, ds_colormap) hold 16-bit pixels; the
   textures, flats and translation tables are bytes, as in the 8-bit build. Design notes,
   pipeline schedules and measurements: README.md. Any SCREENWIDTH that is a multiple of 4 below
   512 (src/platform/cgd_config.h: 320 or 396).

   Conventions: as r_draw_sh4.S, with 2-byte pixels:
   - Colormap lookups use a rotated copy of the RGB565 light tables (cgd_sh4_set_colormaps(),
     rebuilt by cgd_sh4_colormaps_changed() at every palette change): the texel is loaded with a
     sign-extending mov.b, doubled, and used as the byte offset into rot + (map - colormaps) + 256
     bytes. If the colormap pointer is not a 256-entry map of the registered tables, the drawer
     tail-calls its C fallback (r_draw_sh4_c.c). The 1-pixel paths of R_DrawColumn/R_DrawColumnLow
     use dc_colormap directly (extu.b texel).
   - The rotated-map check (ROTCM16): off = cm - registered tables; swap.b(off) rotated right by
     one bit is below nmaps (<= 128) iff off is a multiple of 512 below nmaps * 512.
   - Loops: DSP zero-overhead repeat loops, FOREGROUND ONLY (see r_draw_sh4.S). Loop bodies are
     aligned instruction pairs (two consecutive lines = one issue cycle) for the pipeline model of
     README section 3; their schedules come from test/modsched.py (kinds col16m, collow16, span16,
     spanlow16). */

#include "sh4_asm.h"
#include "cgd_config.h"

#if CGD_TRUECOLOR

#if SCREENWIDTH % 4 != 0 || SCREENWIDTH >= 512
#error "r_draw16_sh4.S: SCREENWIDTH must be a multiple of 4 below 512"
#endif
#define ROWB		(2 * SCREENWIDTH)	/* row step in bytes */
#define FUZZ_MAXCOUNT	(SCREENHEIGHT - 1)
#define FUZZ_COPIES	((FUZZ_MAXCOUNT + 1 + 49 + 49) / 50)
#if FUZZ_MAXCOUNT > 255
#error "r_draw16_sh4.S: SCREENHEIGHT above 256"
#endif

/* Rotated colormap check of the rare drawers.
   In:  CM = colormap pointer, TBL = &cgd_sh4_cmaps.  Clobbers T0, T1, T.
   Out: CM = rotated base (+256 bytes), or branch to SLOW. */
.macro ROTCM16 CM, TBL, T0, T1, SLOW
	mov.l	@\TBL, \T0		/* registered tables */
	mov	\CM, \T1
	sub	\T0, \T1
	mov.l	@(4,\TBL), \T0		/* nmaps */
	swap.b	\T1, \T1
	rotr	\T1
	cmp/hs	\T0, \T1
	mov.l	@(8,\TBL), \T0		/* delta */
	bt	\SLOW
	add	\T0, \CM
.endm

/*============================================================================================
   R_DrawColumn (16-bit)

   As the 8-bit R_DrawColumn, with dest += SCREENWIDTH pixels (ROWB bytes) and a 16-bit colour.
   Loop: 2 pixels (a, b) per iteration, 20 instructions, 10 cycles (5 cycles/pixel) under the
   pess, opt and gcc latency models: `test/modsched.py col16m max --stages 2` (EX-bound: 5 EX per
   pixel). Each texel is loaded into its own index register (mov.b @rI,rI) and doubled into r0
   (mov rI,r0; add rI,r0), which keeps r0 free for 3 of the 5 cycles of a pixel.
   Stage 0 (pass k): a and b index, address and texel, a doubled; dest -> pixel a.
   Stage 1 (pass k-1): colour a, store a; b doubled, colour b, dest -> pixel b, store b.
   Registers: r0 doubled texel, r1 dest, r2 step9, r3 -25, r4 frac9 (next pixel), r5 dc_source,
   r6 rotated colormap base, r7 ROWB, r8/r9 index then texel of a/b, r10 colour.
   n = count+1 pixels: prologue (stage 0 of pass 0, in the set-up), N-1 = count/2 iterations,
   epilogue (stage 1 of the last pass; its pixel b is only stored if n is even). n = 1 takes the
   straight-line path .Lc_one; n = 2 branches once, to the epilogue.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawColumn_SH4)
	/* Set-up ordered by test/listsched.py (the bt .Lc_one marked "barrier: r1 r2 r3 r4"). The
	   count and colormap checks branch to the C path (after popping r8-r10) before any store. */
	mov.l	.Lc_yl, r1
	mov	#31, r0
	mov.l	.Lc_yh, r5
	shll8	r0			/* 7936: larger counts go to C (RC is 12 bits) */
	mov.l	.Lc_iscale, r2
	mov.l	.Lc_centery, r3
	mov.l	@r1, r1			/* dc_yl */
	mov.l	@r5, r5			/* dc_yh */
	mov.l	.Lc_texmid, r4
	mov.l	@r2, r2			/* dc_iscale */
	cmp/eq	r1, r5			/* 1 pixel */
	mov.l	@r3, r3			/* centery */
	bt	.Lc_one
cgd_c16_disp:
	mov.l	.Lc_colormap, r6
	mov.l	@r4, r4			/* dc_texturemid */
	shll8	r2
	mov.l	.Lc_cmaps, r7
	shll	r2			/* r2 = step9 */
	mov.l	r8, @-r15
	sub	r1, r3			/* centery - dc_yl */
	shll8	r4
	mov.l	r9, @-r15
	sub	r1, r5			/* count */
	mov.l	r10, @-r15
	shll	r4			/* dc_texturemid << 9 */
	mov.l	@r6, r6			/* dc_colormap */
	mov	r5, r10			/* r10 = count */
	mul.l	r2, r3			/* MACL = (centery - dc_yl) * step9 */
	cmp/hi	r0, r10			/* count < 0 || count > 7936 */
	mov	r6, r3
	mov.l	@r7, r0			/* registered tables */
	bt	.Lc_slowpop
	mov.l	@(4,r7), r8		/* nmaps */
	shll2	r1
	mov.l	@(8,r7), r9		/* delta */
	sub	r0, r3
	sts	macl, r0
	swap.b	r3, r3
	mov.l	.Lc_columnofs, r7
	rotr	r3
	mov.l	.Lc_source, r5
	cmp/hs	r8, r3			/* not a map of the registered tables: C path */
	sub	r0, r4			/* r4 = frac9 */
	mov	#-25, r3
	mov.l	.Lc_x, r0
	mov	r4, r8			/* Am */
	add	r2, r4			/* Aff */
	mov.l	@r5, r5			/* r5 = dc_source */
	add	r9, r6			/* r6 = rotated colormap base */
	bt	.Lc_slowpop
	mov.l	@r0, r0			/* dc_x */
	mov	r4, r9			/* Bm */
	shld	r3, r8			/* As */
	shll2	r0
	shld	r3, r9			/* Bs */
	mov.l	@(r0,r7), r7		/* columnofs[dc_x] */
	mov.l	.Lc_ylookup, r0
	add	r5, r8			/* Aa */
	add	r5, r9			/* Ba */
	add	r1, r0
	mov.b	@r8, r8			/* Al: texel a of pass 0 */
	add	r7, r7			/* columnofs[dc_x] * 2 */
	mov.b	@r9, r9			/* Bl: texel b of pass 0 */
	add	#-2, r10
	mov.l	@r0, r1			/* ylookup[dc_yl] */
	LDRS(.Lc_rs)
	mov	r8, r0			/* Av */
	LDRE(.Lc_re)
	add	r7, r1			/* r1 = dest */
	mov.w	.Lc_row, r7		/* r7 = ROWB */
	cmp/pz	r10			/* count >= 2: at least one loop iteration */
	add	r8, r0			/* Ax */
	bf/s	.Lc_epi			/* 2 pixels: stage 1 of pass 0 only */
	shlr	r10			/* T = count & 1 (pass complete); r10 = iterations - 1 */
cgd_c16_n2:
	add	#1, r10
	LDRC(10)
	add	r2, r4			/* Bff: frac9 of a(1) */
	ALIGN4
.Lc_rs:
cgd_c16_rs:
	mov	r4, r8			/* Am   a(k) index */
	add	r2, r4			/* Aff  */
	shld	r3, r8			/* As   */
	mov.w	@(r0,r6), r10		/* Ac'  colour a(k-1) */
	add	r5, r8			/* Aa   texel address a(k) */
	mov	r9, r0			/* Bv'  texel b(k-1) */
	add	r9, r0			/* Bx'  ... doubled */
	mov	r4, r9			/* Bm   b(k) index (after Bx' has read r9) */
	mov.b	@r8, r8			/* Al   texel a(k) */
	shld	r3, r9			/* Bs   */
	add	r5, r9			/* Ba   texel address b(k) */
	mov.w	r10, @r1		/* Ast' store a(k-1) */
	mov.w	@(r0,r6), r10		/* Bc'  colour b(k-1) */
	add	r7, r1			/* Bdd' dest -> pixel b(k-1) */
	mov	r8, r0			/* Av   texel a(k) */
	add	r2, r4			/* Bff  */
	add	r8, r0			/* Ax   ... doubled */
	mov.b	@r9, r9			/* Bl   texel b(k) */
	mov.w	r10, @r1		/* Bst' store b(k-1) */
.Lc_re:	add	r7, r1			/* Add  dest -> pixel a(k) */
cgd_c16_end:
.Lc_epi:	/* stage 1 of the last pass */
	mov.w	@(r0,r6), r10		/* colour a */
	mov	r9, r0
	add	r9, r0			/* texel b doubled */
	mov.w	r10, @r1		/* store a */
	add	r7, r1
	mov.w	@(r0,r6), r2		/* colour b */
	mov.l	@r15+, r10
	bf	1f			/* odd pixel count: no pixel b */
	mov.w	r2, @r1
1:	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8
cgd_c16_ret:

.Lc_slowpop:
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Lc_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	ALIGN4
	/* 1 pixel (dc_yl == dc_yh): r1 = dc_yl, r2 = dc_iscale, r3 = centery, r4 = &dc_texturemid.
	   r0-r7 only (nothing saved), dc_colormap indexed directly with the zero-extended texel
	   (no registered tables needed), exactly like C. Hand-scheduled in aligned pairs. */
.Lc_one:
cgd_c16_one:
	mov.l	.Lc_x, r5
	mov	r1, r7
	mov.l	.Lc_ylookup, r6
	sub	r3, r7			/* dc_yl - centery */
	mov.l	@r4, r4			/* dc_texturemid */
	mul.l	r2, r7			/* MACL = (dc_yl - centery) * dc_iscale */
	mov.l	@r5, r5			/* dc_x */
	shll2	r1
	mov.l	.Lc_columnofs, r2
	add	r1, r6			/* &ylookup[dc_yl] */
	sts	macl, r0
	shll2	r5
	mov.l	.Lc_source, r3
	add	r5, r2			/* &columnofs[dc_x] */
	mov.l	@r6, r6			/* ylookup[dc_yl] */
	nop
	mov.l	@r2, r2			/* columnofs[dc_x] */
	add	r4, r0			/* frac */
	mov.l	@r3, r3			/* dc_source */
	shlr16	r0
	mov.l	.Lc_colormap, r7
	and	#127, r0
	mov.b	@(r0,r3), r0		/* texel (sign-extended) */
	add	r2, r2			/* columnofs[dc_x] * 2 */
	mov.l	@r7, r7			/* dc_colormap */
	add	r2, r6			/* dest */
	extu.b	r0, r0
	nop
	add	r0, r0
	nop
	mov.w	@(r0,r7), r0		/* colour */
	rts
	mov.w	r0, @r6
cgd_c16_one_end:

	.balign 2
.Lc_row:	.word	ROWB
	.balign 4
.Lc_yl:		.long	_dc_yl
.Lc_yh:		.long	_dc_yh
.Lc_cmaps:	.long	_cgd_sh4_cmaps
.Lc_colormap:	.long	_dc_colormap
.Lc_x:		.long	_dc_x
.Lc_columnofs:	.long	_columnofs
.Lc_ylookup:	.long	_ylookup
.Lc_centery:	.long	_centery
.Lc_iscale:	.long	_dc_iscale
.Lc_texmid:	.long	_dc_texturemid
.Lc_source:	.long	_dc_source
.Lc_slowfn:	.long	_cgd_sh4_slow_column
ENDFUNC(R_DrawColumn_SH4)

/*============================================================================================
   R_DrawColumnLow (16-bit; detailshift = 1, Chocolate Doom semantics as the 8-bit version)

   Fast path needs columnofs[x+1] == columnofs[x] + 1 (always true with R_InitBuffer): both
   pixels of a texel are written right to left with two pre-decrement mov.w stores from dest + 4,
   then the pointer moves by ROWB + 4 to the next row. Loop: 2 texels (4 pixels) per iteration,
   20 instructions, 10 cycles (2.5 cycles/pixel) under pess and opt, 12 under gcc
   (`test/modsched.py collow16 pess --stages 2`; EX-bound: 5 EX per texel).
   Stage 0 (pass k): a and b index and address, texel a and its doubling; the fracs.
   Stage 1 (pass k-1): dest, colour a, its two stores; texel b, doubled, colour b, its stores.
   Registers: r0 texel, r1 frac9 a, r11 frac9 b, r2 2*step9, r3 -25, r4 dest (pre-decrement),
   r5 dc_source, r6 rotated colormap, r7 ROWB + 4, r8/r9 texel address a/b, r10 colour.
   1 texel (dc_yl == dc_yh) takes the straight-line path .Ll_one; 2 texels branch once, to the
   epilogue.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawColumnLow_SH4)
	/* Set-up ordered by test/listsched.py (the bt .Ll_one marked "barrier: r1 r2 r3 r4");
	   the checks branch to the C path (after restoring the saved registers) before any store. */
	mov.l	.Ll_yl, r1
	mov	#31, r0
	mov.l	.Ll_yh, r5
	shll8	r0			/* 7936: larger counts go to C (RC is 12 bits) */
	mov.l	.Ll_iscale, r2
	mov.l	@r1, r1			/* dc_yl */
	mov.l	@r5, r5			/* dc_yh */
	mov.l	.Ll_centery, r3
	cmp/eq	r1, r5			/* 1 texel */
	mov.l	.Ll_texmid, r4
	mov.l	@r2, r2			/* dc_iscale */
	sub	r1, r5
	mov.l	@r3, r3			/* centery */
	bt	.Ll_one
cgd_cl16_disp:
	mov.l	r8, @-r15
	shll8	r2
	mov.l	r9, @-r15
	shll	r2			/* r2 = step9 */
	mov.l	@r4, r9			/* dc_texturemid */
	mov	r5, r8			/* r8 = count */
	mov.l	.Ll_colormap, r6
	sub	r1, r3			/* centery - dc_yl */
	mov.l	.Ll_cmaps, r7
	shll8	r9
	shll	r9			/* dc_texturemid << 9 */
	mov.l	r10, @-r15
	mov.l	@r6, r6			/* dc_colormap */
	cmp/hi	r0, r8			/* count < 0 || count > 7936 */
	mov.l	@r7, r0			/* registered tables */
	mul.l	r2, r3			/* MACL = (centery - dc_yl) * step9 */
	mov	r6, r3
	mov.l	r11, @-r15
	sub	r0, r3
	mov.l	@(4,r7), r10		/* nmaps */
	mov.l	.Ll_x, r0
	swap.b	r3, r3
	mov.l	@(8,r7), r11		/* delta */
	bt	.Ll_slowpop
	mov.l	.Ll_columnofs, r7
	rotr	r3
	mov.l	@r0, r0			/* dc_x */
	shll2	r1
	cmp/hs	r10, r3			/* not a map of the registered tables: C path */
	mov.l	.Ll_source, r5
	shll2	r0
	bt	.Ll_slowpop
	add	r0, r0			/* (dc_x << 1) * 4 */
	mov	#-25, r3
	add	r0, r7
	mov.l	@r5, r5			/* r5 = dc_source */
	mov.l	.Ll_ylookup, r0
	add	r11, r6			/* r6 = rotated colormap base */
	mov.l	@(4,r7), r10		/* columnofs[x+1] */
	add	#-2, r8
	add	r1, r0
	mov.l	@r7, r4			/* columnofs[x] */
	add	#-1, r10
	mov.w	.Ll_row4, r7		/* r7 = ROWB + 4 */
	mov.l	@r0, r0			/* ylookup[dc_yl] */
	cmp/eq	r4, r10			/* columnofs not contiguous: C path */
	bf	.Ll_slowpop
	add	r4, r4			/* columnofs[x] * 2 */
	add	r0, r4			/* dest */
	sts	macl, r0
	sub	r7, r4
	add	#4, r4			/* r4 = dest - ROWB: the first dd moves it to dest + 4 */
	sub	r0, r9			/* frac9 */
	mov	r9, r1			/* r1 = frac9 of texel a */
	mov	r9, r11
	cmp/pz	r8			/* count >= 2: at least one loop iteration */
	mov	r1, r9
	add	r2, r11			/* r11 = frac9 of texel b */
	shld	r3, r9
	add	r5, r9			/* texel address a */
	add	r2, r2			/* r2 = 2 * step9 */
	mov.b	@r9, r0			/* texel a */
	mov	r11, r9
	LDRS(.Ll_rs)
	shld	r3, r9
	LDRE(.Ll_re)
	add	r5, r9			/* texel address b */
	add	r0, r0			/* texel a doubled */
	add	r2, r1			/* frac9 of a(1) */
	bf/s	.Ll_epi			/* 2 texels: stage 1 of pass 0 only */
	shlr	r8			/* T = count & 1 (pass complete); r8 = iterations - 1 */
cgd_cl16_n2:
	add	#1, r8
	LDRC(8)
	add	r2, r11			/* frac9 of b(1) */
	ALIGN4
.Ll_rs:
cgd_cl16_rs:
	mov	r1, r8			/* Am   a(k) index */
	add	r7, r4			/* Add' next row */
	shld	r3, r8			/* As   */
	mov.w	@(r0,r6), r10		/* Ac'  colour a(k-1) */
	add	r5, r8			/* Aa   texel address a(k) */
	mov.b	@r9, r0			/* Bl'  texel b(k-1) */
	add	r2, r1			/* Aff  */
	mov.w	r10, @-r4		/* Ast' a(k-1), right pixel */
	mov.w	r10, @-r4		/* Ast2' a(k-1), left pixel */
	add	r0, r0			/* Bx'  texel b(k-1) doubled */
	mov	r11, r9			/* Bm   b(k) index */
	add	r7, r4			/* Bdd' next row */
	shld	r3, r9			/* Bs   */
	mov.w	@(r0,r6), r10		/* Bc'  colour b(k-1) */
	mov.b	@r8, r0			/* Al   texel a(k) */
	add	r5, r9			/* Ba   texel address b(k) */
	add	r2, r11			/* Bff  */
	mov.w	r10, @-r4		/* Bst' b(k-1) */
	add	r0, r0			/* Ax   texel a(k) doubled */
.Ll_re:	mov.w	r10, @-r4		/* Bst2' b(k-1) */
cgd_cl16_end:

.Ll_epi:	/* stage 1 of the last pass */
	add	r7, r4
	mov.w	@(r0,r6), r10		/* colour a */
	mov.b	@r9, r0			/* texel b */
	mov.w	r10, @-r4
	mov.w	r10, @-r4
	add	r0, r0
	add	r7, r4
	mov.w	@(r0,r6), r10		/* colour b */
	mov.l	@r15+, r11
	bf	1f			/* odd texel count: no texel b */
	mov.w	r10, @-r4
	mov.w	r10, @-r4
1:	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

cgd_cl16_ret:
.Ll_slowpop:
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Ll_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	ALIGN4
	/* 1 texel (dc_yl == dc_yh): r1 = dc_yl, r2 = dc_iscale, r3 = centery, r4 = &dc_texturemid.
	   r0-r7 only, direct dc_colormap lookup (no registered tables needed), and two independent
	   stores to columnofs[x] and columnofs[x+1] (no contiguity needed), exactly like C. */
.Ll_one:
cgd_cl16_one:
	mov.l	.Ll_x, r5
	mov	r1, r7
	mov.l	.Ll_ylookup, r6
	sub	r3, r7			/* dc_yl - centery */
	mov.l	@r4, r4			/* dc_texturemid */
	mul.l	r2, r7			/* MACL = (dc_yl - centery) * dc_iscale */
	mov.l	@r5, r5			/* dc_x */
	shll2	r1
	mov.l	.Ll_columnofs, r2
	add	r1, r6			/* &ylookup[dc_yl] */
	sts	macl, r0
	shll2	r5
	mov.l	.Ll_source, r3
	add	r5, r5			/* (dc_x << 1) * 4 */
	mov.l	@r6, r6			/* ylookup[dc_yl] */
	add	r5, r2			/* &columnofs[x] */
	mov.l	.Ll_colormap, r7
	add	r4, r0			/* frac */
	mov.l	@r3, r3			/* dc_source */
	shlr16	r0
	mov.l	@r2, r5			/* columnofs[x] */
	and	#127, r0
	mov.l	@(4,r2), r2		/* columnofs[x+1] */
	nop
	mov.b	@(r0,r3), r0		/* texel (sign-extended) */
	add	r5, r5
	mov.l	@r7, r7			/* dc_colormap */
	add	r2, r2
	extu.b	r0, r0
	add	r6, r5			/* dest */
	add	r0, r0
	add	r6, r2			/* dest2 */
	mov.w	@(r0,r7), r0		/* colour */
	nop
	mov.w	r0, @r5
	rts
	mov.w	r0, @r2
cgd_cl16_one_end:

	.balign 2
.Ll_row4:	.word	ROWB + 4
	.balign 4
.Ll_yl:		.long	_dc_yl
.Ll_yh:		.long	_dc_yh
.Ll_cmaps:	.long	_cgd_sh4_cmaps
.Ll_colormap:	.long	_dc_colormap
.Ll_x:		.long	_dc_x
.Ll_columnofs:	.long	_columnofs
.Ll_ylookup:	.long	_ylookup
.Ll_centery:	.long	_centery
.Ll_iscale:	.long	_dc_iscale
.Ll_texmid:	.long	_dc_texturemid
.Ll_source:	.long	_dc_source
.Ll_slowfn:	.long	_cgd_sh4_slow_column_low
ENDFUNC(R_DrawColumnLow_SH4)

/*============================================================================================
   R_DrawSpan (16-bit)

   As the 8-bit R_DrawSpan (right to left from x = xfrac + count*xstep, y = yfrac + count*ystep,
   pre-decrement mov.w stores, the xtrct spot computation), with the texel doubled before the
   colour lookup (x: add r0,r0). Loop: 2 pixels per iteration, 24 instructions and 2 nops,
   13 cycles (6.5 cycles/pixel) under pess and opt, 15 under gcc. 13 is the minimum under pess
   (`test/modsched.py span16 pess`: 12 is infeasible for any number of stages: r0 is busy 6
   cycles per pixel, f -> g -> x -> h, and the two rows that pair h with f' leave too few EX
   slots); the schedule keeps the 8-bit stage split plus x (`--stage1 Be,Bf,Bg,Bx,Ax,Ah,Bh,Ai,Bi`)
   so the prologue (stage 0 of pass 0) is the 8-bit one.
   Registers: r0 spot/texel, r1 rm, r2 rn, r3 colour, r4 dest (pre-decremented), r5 ds_source,
   r6 rotated colormap, r7 X, r8 Y, r9 xstep << 10, r10 ystep << 10, r11 -26, r12 -10.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawSpan_SH4)
	/* Set-up ordered by test/listsched.py; the checks branch to the C path (after restoring
	   the saved registers) before any store. */
	mov.l	.Ls_y, r0
	mov	#31, r4
	mov.l	.Ls_x1, r1
	shll8	r4			/* 7936 */
	mov.l	.Ls_x2, r2
	mov.l	r8, @-r15
	mov.l	@r0, r0			/* ds_y */
	mov.l	r9, @-r15
	mov.l	.Ls_ylookup, r3
	shll2	r0
	mov.l	@r1, r1			/* ds_x1 */
	mov.l	@r2, r2			/* ds_x2 */
	mov.l	r10, @-r15
	mov.l	.Ls_colormap, r6
	sub	r1, r2			/* r2 = count */
	mov.l	r11, @-r15
	shll2	r1
	mov.l	.Ls_cmaps, r11
	mov.l	.Ls_columnofs, r5
	cmp/hi	r4, r2			/* count < 0 || count > 7936 */
	mov.l	@(r0,r3), r3		/* ylookup[ds_y] */
	mov	r1, r0
	mov.l	.Ls_xstep, r9
	mov.l	@r6, r6			/* ds_colormap */
	mov.l	@(r0,r5), r5		/* columnofs[ds_x1] */
	mov.l	@r11, r0		/* registered tables */
	mov	r6, r1
	mov.l	.Ls_ystep, r10
	mov.l	@r9, r9			/* ds_xstep */
	sub	r0, r1
	mov.l	r12, @-r15
	swap.b	r1, r1
	mov.l	@(4,r11), r0		/* nmaps */
	mul.l	r2, r9			/* count * xstep */
	mov.l	.Ls_xfrac, r7
	bt	.Ls_slowpop
	rotr	r1
	mov.l	@r10, r10		/* ds_ystep */
	cmp/hs	r0, r1			/* not a map of the registered tables: C path */
	mov.l	.Ls_yfrac, r8
	mov.l	@(8,r11), r0		/* delta */
	add	r5, r5			/* columnofs[ds_x1] * 2 */
	mov.l	@r7, r7			/* ds_xfrac */
	add	r5, r3			/* dest */
	add	r0, r6			/* r6 = rotated colormap base */
	mov	#10, r1
	sts	macl, r0
	mov	r3, r4
	mul.l	r2, r10			/* count * ystep */
	mov.l	@r8, r8			/* ds_yfrac */
	mov.l	.Ls_source, r12
	shld	r1, r9			/* xstep << 10 */
	add	r0, r7			/* xfrac of the last pixel */
	mov	#-26, r11
	sts	macl, r0
	shld	r1, r7			/* X */
	mov.l	@r12, r12		/* ds_source */
	shld	r1, r10			/* ystep << 10 */
	add	r2, r4
	mov	r2, r3			/* r3 = count */
	add	r0, r8			/* yfrac of the last pixel */
	mov	r12, r5			/* r5 = ds_source */
	shld	r1, r8			/* Y */
	mov	#-10, r12
	mov	r8, r1			/* a_A */
	add	r2, r4
	shld	r11, r1			/* b_A */
	mov	r7, r2			/* c_A */
	xtrct	r1, r2			/* d_A */
	bt	.Ls_slowpop
	shld	r12, r2			/* e_A */
	mov	r2, r0			/* f_A */
	sub	r10, r8			/* k_A */
	mov	r8, r1			/* a_B */
	sub	r9, r7			/* j_A */
	mov.b	@(r0,r5), r0		/* g_A: texel A */
	shld	r11, r1			/* b_B */
	mov	r7, r2			/* c_B */
	add	#-2, r3
	LDRS(.Ls_rs)
	add	#2, r4			/* r4 = dest + 2 * (count + 1) */
	LDRE(.Ls_re)
	sub	r10, r8			/* k_B */
	sub	r9, r7			/* j_B */
	xtrct	r1, r2			/* d_B */
	cmp/pz	r3			/* count >= 2: at least one loop iteration */
	bf/s	.Ls_epi
	shlr	r3			/* T = count & 1 (pass complete); r3 = iterations - 1 */
	add	#1, r3
	LDRC(3)
	nop
	ALIGN4
.Ls_rs:
cgd_s16_rs:
	mov	r8, r1			/* a_A      Y -> rm */
	add	r0, r0			/* x_A'     texel A' doubled */
	shld	r12, r2			/* e_B'     */
	nop
	mov.w	@(r0,r6), r3		/* h_A'     colour A' */
	mov	r2, r0			/* f_B'     (after h_A' has read r0) */
	mov	r7, r2			/* c_A      X -> rn */
	shld	r11, r1			/* b_A      */
	xtrct	r1, r2			/* d_A      */
	mov.b	@(r0,r5), r0		/* g_B'     texel B' */
	sub	r10, r8			/* k_A      */
	mov.w	r3, @-r4		/* i_A'     store A' */
	mov	r8, r1			/* a_B      */
	add	r0, r0			/* x_B'     texel B' doubled */
	shld	r11, r1			/* b_B      */
	nop
	shld	r12, r2			/* e_A      */
	mov.w	@(r0,r6), r3		/* h_B'     colour B' */
	mov	r2, r0			/* f_A      */
	sub	r9, r7			/* j_A      */
	mov	r7, r2			/* c_B      (reads X before j_B updates it) */
	sub	r9, r7			/* j_B      */
	xtrct	r1, r2			/* d_B      */
	mov.b	@(r0,r5), r0		/* g_A      texel A */
	sub	r10, r8			/* k_B      */
.Ls_re:	mov.w	r3, @-r4		/* i_B'     store B' */
cgd_s16_end:

.Ls_epi:	/* stage 1 of the last pass */
	add	r0, r0			/* x_A */
	shld	r12, r2			/* e_B */
	mov.w	@(r0,r6), r3		/* h_A */
	mov	r2, r0			/* f_B */
	mov.w	r3, @-r4		/* i_A */
	mov.b	@(r0,r5), r0		/* g_B */
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	add	r0, r0			/* x_B */
	mov.l	@r15+, r10
	mov.w	@(r0,r6), r3		/* h_B */
	mov.l	@r15+, r9
	bf	1f			/* odd pixel count: no pixel B */
	mov.w	r3, @-r4		/* i_B */
1:	rts
	mov.l	@r15+, r8

cgd_s16_ret:
.Ls_slowpop:
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Ls_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	.balign 4
.Ls_x1:		.long	_ds_x1
.Ls_x2:		.long	_ds_x2
.Ls_cmaps:	.long	_cgd_sh4_cmaps
.Ls_colormap:	.long	_ds_colormap
.Ls_y:		.long	_ds_y
.Ls_ylookup:	.long	_ylookup
.Ls_columnofs:	.long	_columnofs
.Ls_xstep:	.long	_ds_xstep
.Ls_ystep:	.long	_ds_ystep
.Ls_xfrac:	.long	_ds_xfrac
.Ls_yfrac:	.long	_ds_yfrac
.Ls_source:	.long	_ds_source
.Ls_slowfn:	.long	_cgd_sh4_slow_span
ENDFUNC(R_DrawSpan_SH4)

/*============================================================================================
   R_DrawSpanLow (16-bit; detailshift = 1, Chocolate Doom semantics as the 8-bit version)

   count = ds_x2 - ds_x1 (before doubling); ds_x1 <<= 1; ds_x2 <<= 1 (globals, as in C);
   each texel is written to 2 consecutive pixels (two pre-decrement mov.w stores). Same
   registers and stage split as R_DrawSpan (x_A in stage 0): 2 texels (4 pixels) per iteration,
   26 instructions, 13 cycles (3.25 cycles/pixel) under pess and opt, 15 under gcc
   (`test/modsched.py spanlow16 pess --stage1 Be,Bf,Bg,Bx,Ah,Bh,Ai,Bi,Ai2,Bi2`; 13 is the
   resource floor: 26 instructions).
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawSpanLow_SH4)
	/* Set-up ordered by test/listsched.py; the checks branch to the C path (after restoring
	   the saved registers) before any store. */
	mov.l	.Lsl_y, r0
	mov	#31, r4
	mov.l	.Lsl_x1, r1
	shll8	r4			/* 7936 */
	mov.l	.Lsl_x2, r2
	mov.l	@r0, r0			/* ds_y */
	mov.l	r8, @-r15
	mov.l	.Lsl_ylookup, r3
	mov.l	@r1, r1			/* ds_x1 */
	shll2	r0
	mov.l	@r2, r2			/* ds_x2 */
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	sub	r1, r2			/* r2 = count (before doubling) */
	mov.l	.Lsl_colormap, r6
	shll2	r1
	mov.l	r11, @-r15
	cmp/hi	r4, r2			/* count < 0 || count > 7936 */
	mov.l	.Lsl_cmaps, r11
	mov.l	.Lsl_columnofs, r5
	mov.l	@(r0,r3), r3		/* ylookup[ds_y] */
	mov	r1, r0
	add	r0, r0			/* (ds_x1 << 1) * 4 */
	mov.l	.Lsl_xstep, r9
	mov.l	@r6, r6			/* ds_colormap */
	mov.l	@(r0,r5), r5		/* columnofs[ds_x1 << 1] */
	mov.l	@r11, r0		/* registered tables */
	mov	r6, r1
	mov.l	.Lsl_ystep, r10
	add	r5, r5			/* columnofs[ds_x1 << 1] * 2 */
	mov.l	@r9, r9			/* ds_xstep */
	sub	r0, r1
	mov.l	r12, @-r15
	swap.b	r1, r1
	mov.l	@(4,r11), r0		/* nmaps */
	mul.l	r2, r9			/* count * xstep */
	mov.l	.Lsl_xfrac, r7
	bt	.Lsl_slowpop
	rotr	r1
	mov.l	@r10, r10		/* ds_ystep */
	cmp/hs	r0, r1			/* not a map of the registered tables: C path */
	mov.l	.Lsl_yfrac, r8
	mov.l	@(8,r11), r0		/* delta */
	mov	#10, r1
	mov.l	@r7, r7			/* ds_xfrac */
	add	r5, r3			/* dest */
	add	r0, r6			/* r6 = rotated colormap base */
	mov	r3, r4
	sts	macl, r0
	shld	r1, r9			/* xstep << 10 */
	mul.l	r2, r10			/* count * ystep */
	mov.l	@r8, r8			/* ds_yfrac */
	mov.l	.Lsl_source, r12
	mov	#-26, r11
	add	r0, r7			/* xfrac of the last texel */
	mov	r2, r3			/* r3 = count */
	sts	macl, r0
	shld	r1, r7			/* X */
	mov.l	@r12, r12		/* ds_source */
	shld	r1, r10			/* ystep << 10 */
	shll2	r2
	bt	.Lsl_slowpop
	add	r0, r8			/* yfrac of the last texel */
	mov	r12, r5			/* r5 = ds_source */
	shld	r1, r8			/* Y */
	mov	#-10, r12
	mov	r8, r1			/* a_A */
	add	r2, r4			/* dest + 4 * count */
	mov	r7, r2			/* c_A */
	shld	r11, r1			/* b_A */
	xtrct	r1, r2			/* d_A */
	shld	r12, r2			/* e_A */
	mov	r2, r0			/* f_A */
	sub	r10, r8			/* k_A */
	mov	r8, r1			/* a_B */
	sub	r9, r7			/* j_A */
	mov.b	@(r0,r5), r0		/* g_A: texel A */
	shld	r11, r1			/* b_B */
	mov	r7, r2			/* c_B */
	add	#-2, r3
	LDRS(.Lsl_rs)
	add	#4, r4			/* r4 = dest + 4 * (count + 1) */
	sub	r10, r8			/* k_B */
	LDRE(.Lsl_re)
	sub	r9, r7			/* j_B */
	xtrct	r1, r2			/* d_B */
	add	r0, r0			/* x_A */
	cmp/pz	r3			/* count >= 2: at least one loop iteration */
	bf/s	.Lsl_epi
	shlr	r3			/* T = count & 1 (pass complete); r3 = iterations - 1 */
	add	#1, r3
	LDRC(3)
	nop
	ALIGN4
.Lsl_rs:
cgd_sl16_rs:
	mov	r8, r1			/* a_A    Y -> rm */
	shld	r12, r2			/* e_B'   */
	mov.w	@(r0,r6), r3		/* h_A'   colour A' */
	mov	r2, r0			/* f_B'   (after h_A' has read r0) */
	mov	r7, r2			/* c_A    X -> rn */
	shld	r11, r1			/* b_A    */
	xtrct	r1, r2			/* d_A    */
	mov.b	@(r0,r5), r0		/* g_B'   texel B' */
	sub	r10, r8			/* k_A    */
	mov.w	r3, @-r4		/* i_A'   store A' */
	add	r0, r0			/* x_B'   texel B' doubled */
	mov.w	r3, @-r4		/* i2_A'  store A' (second pixel) */
	shld	r12, r2			/* e_A    */
	mov	r8, r1			/* a_B    */
	shld	r11, r1			/* b_B    */
	mov.w	@(r0,r6), r3		/* h_B'   colour B' */
	mov	r2, r0			/* f_A    (after h_B' has read r0) */
	sub	r9, r7			/* j_A    */
	mov	r7, r2			/* c_B    (reads X before j_B updates it) */
	sub	r9, r7			/* j_B    */
	xtrct	r1, r2			/* d_B    */
	mov.b	@(r0,r5), r0		/* g_A    texel A */
	sub	r10, r8			/* k_B    */
	mov.w	r3, @-r4		/* i_B'   store B' */
	add	r0, r0			/* x_A    texel A doubled */
.Lsl_re:	mov.w	r3, @-r4		/* i2_B'  store B' (second pixel) */
cgd_sl16_end:

.Lsl_epi:	/* stage 1 of the last pass, and ds_x1 <<= 1, ds_x2 <<= 1 */
	shld	r12, r2			/* e_B */
	mov.w	@(r0,r6), r3		/* h_A */
	mov	r2, r0			/* f_B */
	mov.l	.Lsl_x1, r1
	mov.w	r3, @-r4		/* i_A */
	mov.l	.Lsl_x2, r2
	mov.w	r3, @-r4		/* i2_A */
	mov.l	@r1, r7
	mov.b	@(r0,r5), r0		/* g_B */
	mov.l	@r2, r8
	mov.l	@r15+, r12
	add	r7, r7
	add	r0, r0			/* x_B */
	add	r8, r8
	mov.l	@r15+, r11
	mov.l	r7, @r1			/* ds_x1 <<= 1 (as the C version, which does it first) */
	mov.w	@(r0,r6), r3		/* h_B */
	mov.l	r8, @r2			/* ds_x2 <<= 1 */
	mov.l	@r15+, r10
	bf	1f
	mov.w	r3, @-r4		/* i_B */
	mov.w	r3, @-r4		/* i2_B */
1:	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

cgd_sl16_ret:
.Lsl_slowpop:
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Lsl_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	.balign 4
.Lsl_x1:	.long	_ds_x1
.Lsl_x2:	.long	_ds_x2
.Lsl_cmaps:	.long	_cgd_sh4_cmaps
.Lsl_colormap:	.long	_ds_colormap
.Lsl_y:		.long	_ds_y
.Lsl_ylookup:	.long	_ylookup
.Lsl_columnofs:	.long	_columnofs
.Lsl_xstep:	.long	_ds_xstep
.Lsl_ystep:	.long	_ds_ystep
.Lsl_xfrac:	.long	_ds_xfrac
.Lsl_yfrac:	.long	_ds_yfrac
.Lsl_source:	.long	_ds_source
.Lsl_slowfn:	.long	_cgd_sh4_slow_span_low
ENDFUNC(R_DrawSpanLow_SH4)

/*============================================================================================
   Rare drawers (sprites only): normal .text, simple loops.
  ============================================================================================*/

/* R_DrawTranslatedColumn / R_DrawTranslatedColumnLow (16-bit)
   *dest = dc_colormap[dc_translation[dc_source[frac >> 16]]]   (arithmetic shift, no mask)
   Low: x = dc_x << 1, pixels at columnofs[x] and columnofs[x+1] (two pointers, no precondition).
   Shared body as the 8-bit version: every texel is stored through r4 and through r8, both stepped
   by ROWB; normal: r8 = r4 (the second store rewrites the same pixel with the same value). */

	CGD_SH4_COLD
	.balign 4
FUNC(R_DrawTranslatedColumnLow_SH4)
	mov.l	.Lt_slowlow, r0
	bra	.Lt_common
	mov	#1, r3
FUNC(R_DrawTranslatedColumn_SH4)
	mov.l	.Lt_slow, r0
	mov	#0, r3
.Lt_common:
	mov.l	r8, @-r15
	mov.l	.Lt_yl, r1
	mov.l	r0, @-r15		/* C fallback for this entry point */
	mov.l	.Lt_yh, r2
	mov.l	@r1, r1			/* dc_yl */
	mov.l	@r2, r2			/* dc_yh */
	mov	#15, r0
	sub	r1, r2			/* count */
	shll8	r0			/* 3840 */
	cmp/hi	r0, r2
	bt	.Lt_slowpop
	mov.l	.Lt_cmaps, r7
	mov.l	.Lt_colormap, r6
	mov.l	@r6, r6
	ROTCM16	r6, r7, r0, r5, .Lt_slowpop
	add	#4, r15			/* drop the fallback address */
	mov.l	.Lt_x, r0
	mov.l	.Lt_columnofs, r5
	mov.l	@r0, r0			/* dc_x */
	shld	r3, r0			/* x = dc_x << low */
	shll2	r0
	add	r0, r5
	mov.l	.Lt_ylookup, r7
	mov	r1, r0
	shll2	r0
	mov.l	@(r0,r7), r7		/* ylookup[dc_yl] */
	mov.l	@r5, r4
	add	r4, r4
	add	r7, r4			/* dest */
	tst	r3, r3
	mov.l	@(4,r5), r8
	add	r8, r8
	bf/s	1f
	add	r7, r8			/* dest2 = ylookup + columnofs[x+1] (low) */
	mov	r4, r8			/* normal: second store = same pixel, same value */
1:	mov.l	.Lt_centery, r3
	mov.l	.Lt_iscale, r5
	mov.l	@r3, r3
	mov.l	@r5, r5			/* fracstep */
	sub	r3, r1
	mul.l	r5, r1			/* MACL = (dc_yl - centery) * dc_iscale */
	add	#1, r2			/* pixels */
	LDRS(2f)
	LDRE(3f)
	LDRC(2)
	mov.l	.Lt_source, r0
	mov.l	@r0, r3			/* r3 = dc_source */
	mov.l	.Lt_translation, r0
	mov	r5, r2			/* r2 = fracstep */
	mov.l	@r0, r5			/* r5 = dc_translation */
	sts	macl, r1
	mov.l	.Lt_texmid, r0
	mov.l	@r0, r0
	add	r0, r1			/* r1 = frac */
	mov.w	.Lt_row, r7		/* r7 = ROWB */
	ALIGN4
cgd_t16_rs:
2:	swap.w	r1, r0
	add	r2, r1
	exts.w	r0, r0			/* frac >> 16 */
	mov.b	@(r0,r3), r0		/* texel */
	extu.b	r0, r0
	mov.b	@(r0,r5), r0		/* translated (sign-extended) */
	add	r0, r0
	mov.w	@(r0,r6), r0		/* colour */
	mov.w	r0, @r4
	mov.w	r0, @r8
	add	r7, r4
3:	add	r7, r8
cgd_t16_end:
	rts
	mov.l	@r15+, r8

.Lt_slowpop:
	mov.l	@r15+, r0
	jmp	@r0
	mov.l	@r15+, r8

	.balign 2
.Lt_row:	.word	ROWB
	.balign 4
.Lt_yl:		.long	_dc_yl
.Lt_yh:		.long	_dc_yh
.Lt_cmaps:	.long	_cgd_sh4_cmaps
.Lt_colormap:	.long	_dc_colormap
.Lt_x:		.long	_dc_x
.Lt_columnofs:	.long	_columnofs
.Lt_ylookup:	.long	_ylookup
.Lt_centery:	.long	_centery
.Lt_iscale:	.long	_dc_iscale
.Lt_texmid:	.long	_dc_texturemid
.Lt_source:	.long	_dc_source
.Lt_translation: .long	_dc_translation
.Lt_slow:	.long	_cgd_sh4_slow_translated
.Lt_slowlow:	.long	_cgd_sh4_slow_translated_low
ENDFUNC(R_DrawTranslatedColumn_SH4)

/* R_DrawFuzzColumn / R_DrawFuzzColumnLow (16-bit)
   if (!dc_yl) dc_yl = 1; if (dc_yh == viewheight-1) dc_yh = viewheight-2;   (globals)
   count = dc_yh - dc_yl; if (count < 0) return;
   *dest = FUZZDARK(dest[fuzzoffset[fuzzpos]]); fuzzpos = (fuzzpos + 1) % 50 per row, where
   FUZZDARK (r_draw.c FuzzDark) scales each RGB565 channel by 13/16 with rounding, on the pixel
   spread over 32 bits: s = (p | p << 16) & 0x07e0f81f; s = ((13 * s + 0x01004008) >> 4) &
   0x07e0f81f; p' = s | s >> 16. No colormap is read, so no registration is needed.
   Rows are processed strictly in order (dest[-SCREENWIDTH] must see the row just written).
   The offsets come from an unrolled copy of fuzzoffset[] (FUZZ_COPIES x 50 entries) starting at
   fuzzpos (count <= FUZZ_MAXCOUNT, else C), each plus ROWB: the dest pointer stays one row
   behind until the pixel's load has issued, so the next row's load address is never waiting
   on the pointer step; fuzzpos += pixels (mod 50) at the end. 17 cycles per row in the model
   (the FUZZDARK chain: 12 EX after the load). */

/* FUZZDARK of P (the pixel, loaded sign-extended) into the low 16 bits of P (what mov.w
   stores); clobbers T. M = 0x07e0f81f, K = 0x01004008 (8 in each field), S = -4.
   xtrct P,T after shll16 T gives (p << 16) | p with both halves exactly the 16 pixel bits (the
   sign bits of the load are shifted out); 13s = ((2s + s) << 2) + s; the fields cannot overflow
   into each other (13 * 63 + 8 < 2^10 for green at bit 21, 13 * 31 + 8 < 2^9 for red and blue);
   swap.w + or bring green back next to red and blue. */
.macro FUZZDARK16 P, T, M, K, S
	mov	\P, \T
	shll16	\T
	xtrct	\P, \T			/* p | p << 16 */
	and	\M, \T			/* s */
	mov	\T, \P
	add	\P, \P
	add	\T, \P			/* 3s */
	shll2	\P
	add	\T, \P			/* 13s */
	add	\K, \P
	shld	\S, \P			/* (13s + K) >> 4 */
	and	\M, \P
	swap.w	\P, \T
	or	\T, \P			/* low 16 bits: s | s >> 16 */
.endm

	CGD_SH4_COLD
	.balign 4
FUNC(R_DrawFuzzColumnLow_SH4)
	mov.l	.Lf_slowlow, r0
	bra	.Lf_common
	mov	#1, r3
FUNC(R_DrawFuzzColumn_SH4)
	mov.l	.Lf_slow, r0
	mov	#0, r3
.Lf_common:
	mov.l	r8, @-r15
	mov.l	.Lf_yl, r1
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov.l	r0, @-r15		/* C fallback for this entry point */
	mov.l	@r1, r0			/* dc_yl */
	tst	r0, r0
	bf	1f
	mov	#1, r0
	mov.l	r0, @r1			/* dc_yl = 1 */
1:	mov	r0, r1			/* r1 = yl */
	mov.l	.Lf_viewheight, r2
	mov.l	.Lf_yh, r5
	mov.l	@r2, r2
	mov.l	@r5, r0			/* dc_yh */
	add	#-1, r2
	cmp/eq	r2, r0
	bf	2f
	add	#-1, r0
	mov.l	r0, @r5			/* dc_yh = viewheight - 2 */
2:	sub	r1, r0			/* count */
	mov	#FUZZ_MAXCOUNT, r2
	extu.b	r2, r2
	cmp/hi	r2, r0			/* count < 0 (C returns) or count > FUZZ_MAXCOUNT */
	bt	.Lf_slowpop
	mov	r0, r2
	mov.l	.Lf_fuzzpos, r5
	mov.l	@r5, r5
	mov	#50, r0
	cmp/hs	r0, r5			/* fuzzpos outside [0,49]: C */
	bt	.Lf_slowpop
	add	#4, r15			/* drop the fallback address */
	mov.l	.Lf_x, r0
	mov.l	.Lf_columnofs, r7
	mov.l	@r0, r0
	shld	r3, r0			/* x = dc_x << low */
	shll2	r0
	add	r0, r7
	mov.l	.Lf_ylookup, r8
	mov	r1, r0
	shll2	r0
	mov.l	@(r0,r8), r0		/* ylookup[yl] */
	mov.w	.Lf_row, r9
	mov.l	@r7, r4
	sub	r9, r0			/* ylookup[yl] - ROWB: one row behind */
	add	r4, r4
	add	r0, r4			/* dest - ROWB */
	mov.l	@(4,r7), r8
	add	r8, r8
	add	r0, r8			/* dest2 - ROWB (low) */
	mov	r9, r7			/* ROWB */
	mov.l	.Lf_tab, r1
	mov	r5, r0
	add	r0, r0
	add	r0, r1			/* r1 = &fuzztab[fuzzpos] */
	add	r2, r5
	add	#1, r5			/* fuzzpos + pixels */
	add	#1, r2			/* pixels */
	mov.l	.Lf_mask, r6		/* the FUZZDARK constants */
	mov.l	.Lf_round, r9
	mov	#-4, r10
	mov.w	@r1+, r0		/* offset of the first row, + ROWB */
	LDRS(.Lf_rs1)
	LDRE(.Lf_re1)
	tst	r3, r3
	bf	.Lf_low
	LDRC(2)
	nop
	ALIGN4
.Lf_rs1:
cgd_f16_rs:
	mov.w	@(r0,r4), r2		/* dest[offset] (r4 is the row above) */
	add	r7, r4			/* r4 = dest */
	mov.w	@r1+, r0		/* next row's offset, + ROWB */
	nop				/* (keeps the macro's first pair aligned) */
	FUZZDARK16 r2, r3, r6, r9, r10
.Lf_re1:	mov.w	r2, @r4
cgd_f16_end:
	bra	.Lf_done
	nop

.Lf_low:
	LDRS(.Lf_rs2)
	LDRE(.Lf_re2)
	LDRC(2)
	nop
	ALIGN4
.Lf_rs2:
cgd_fl16_rs:
	mov.w	@(r0,r4), r2		/* dest[offset] */
	add	r7, r4
	FUZZDARK16 r2, r3, r6, r9, r10
	mov.w	r2, @r4
	mov.w	@(r0,r8), r2		/* dest2[offset], after the store to dest */
	add	r7, r8
	mov.w	@r1+, r0		/* next row's offset, + ROWB */
	nop				/* (keeps the macro's first pair aligned) */
	FUZZDARK16 r2, r3, r6, r9, r10
.Lf_re2:	mov.w	r2, @r8
cgd_fl16_end:

.Lf_done:	/* fuzzpos = (fuzzpos + pixels) % 50 */
	mov	#50, r1
3:	cmp/hs	r1, r5
	bf	4f
	bra	3b
	sub	r1, r5
4:	mov.l	.Lf_fuzzpos, r0
	mov.l	r5, @r0
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

.Lf_slowpop:
	mov.l	@r15+, r0
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	jmp	@r0
	mov.l	@r15+, r8

	.balign 2
.Lf_row:	.word	ROWB
	.balign 4
.Lf_mask:	.long	0x07e0f81f
.Lf_round:	.long	0x01004008
.Lf_yl:		.long	_dc_yl
.Lf_yh:		.long	_dc_yh
.Lf_viewheight:	.long	_viewheight
.Lf_fuzzpos:	.long	_fuzzpos
.Lf_x:		.long	_dc_x
.Lf_columnofs:	.long	_columnofs
.Lf_ylookup:	.long	_ylookup
.Lf_tab:	.long	cgd_sh4_fuzztab16
.Lf_slow:	.long	_cgd_sh4_slow_fuzz
.Lf_slowlow:	.long	_cgd_sh4_slow_fuzz_low
ENDFUNC(R_DrawFuzzColumn_SH4)

	.section .rodata
	.balign 4
/* fuzzoffset[] of r_draw.c (FUZZOFF = SCREENWIDTH pixels = ROWB bytes) plus ROWB: FZP for
   +FUZZOFF, FZM for -FUZZOFF; FUZZ_COPIES times */
#define FZP (2 * ROWB)
#define FZM 0
cgd_sh4_fuzztab16:
	.rept FUZZ_COPIES
	.short	FZP,FZM,FZP,FZM,FZP,FZP,FZM,FZP,FZP,FZM,FZP,FZP,FZP,FZM,FZP,FZP,FZP,FZM,FZM,FZM
	.short	FZM,FZP,FZM,FZM,FZP,FZP,FZP,FZP,FZM,FZP,FZM,FZP,FZP,FZM,FZM,FZP,FZP,FZM,FZM,FZM
	.short	FZM,FZP,FZP,FZP,FZP,FZM,FZP,FZP,FZM,FZP
	.endr

#endif /* CGD_TRUECOLOR */
