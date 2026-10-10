/* r_draw_sh4.S - SH4AL-DSP (SH-4A, big-endian) column and span drawers for CGDoom.

   Drop-in replacements for the vanilla r_draw.c drawers: void(void), reading the same engine
   globals, bit-exact output (see test/ref_draw.c and test/test_sh4.c). Design notes, pipeline
   schedules and measurements: README.md.

   Conventions used throughout:
   - GCC/Renesas SH ABI: r0-r7, T, MACH/MACL are clobbered; r8-r14 are saved when used.
   - Colormap lookups use a "rotated" copy of the COLORMAP lump (cgd_sh4_set_colormaps()): the
     texel is loaded with a sign-extending mov.b and used directly as the index into
     rot + (map - colormaps) + 128. If the colormap pointer is not a 256-byte map of the
     registered lump, the drawer tail-calls its C fallback (r_draw_sh4_c.c). Exception: the
     1-pixel paths of R_DrawColumn/R_DrawColumnLow use dc_colormap directly (extu.b texel).
   - Column texel index: frac9 = frac << 9 is stepped by step9 = iscale << 9 (exact mod 2^32);
     (frac >> 16) & 127 == frac9 >> 25 (logical): one shld, no mask (COLEXTRABITS trick).
   - Loops are DSP zero-overhead repeat loops (ldrs/ldre/ldrc). FOREGROUND ONLY: no code that can
     run in interrupt context may use ldrs/ldre (gint does not save RS/RE). RC (in SR) is saved by
     the exception mechanism, so a plain interrupt during a loop is harmless.
   - Loop bodies are laid out as aligned instruction pairs (two consecutive lines = one issue
     cycle), scheduled for a dual-issue in-order pipeline with: EX->EX 1 cycle, EX->address 2,
     load->EX 2, load->address 3, load->store data 2 (see README "Pipeline model"). */

#include "sh4_asm.h"

/* Rotated colormap check, shared by all drawers.
   In:  CM = colormap pointer, TBL = &cgd_sh4_cmaps.  Clobbers T0, T1, T.
   Out: CM = rotated base, or branch to SLOW.
   off = CM - src must be a multiple of 256 below nmaps*256:
   swap.b(off) < nmaps  <=>  off < 65536 && (off & 255) == 0 && (off >> 8) < nmaps  (nmaps <= 256) */
.macro ROTCM CM, TBL, T0, T1, SLOW
	mov.l	@\TBL, \T0		/* registered lump */
	mov	\CM, \T1
	sub	\T0, \T1
	mov.l	@(4,\TBL), \T0		/* nmaps */
	swap.b	\T1, \T1
	cmp/hs	\T0, \T1
	mov.l	@(8,\TBL), \T0		/* delta = rot + 128 - src */
	bt	\SLOW
	add	\T0, \CM
.endm

/*============================================================================================
   R_DrawColumn

   count = dc_yh - dc_yl; if (count < 0) return;
   dest = ylookup[dc_yl] + columnofs[dc_x];
   frac = dc_texturemid + (dc_yl - centery) * dc_iscale;
   do { *dest = dc_colormap[dc_source[(frac >> 16) & 127]]; dest += 320; frac += dc_iscale; }
   while (count--);

   Loop: 2 pixels (a, b) per iteration, 16 instructions, 8 cycles (4 cycles/pixel), software
   pipelined over 2 stages: an iteration computes the texel addresses of pair k and loads its
   texel a, and finishes pair k-1 (colour a, texel b, store a, colour b, store b).
   Registers: r0 texel/colour index, r1 dest pointer (incremented by 320 before each store),
   r2 step9, r3 -25, r4 frac9 (next pixel), r5 dc_source, r6 rotated colormap base, r7 320,
   r8/r9 texel address of a/b, r10 colour.
   n = count+1 pixels are covered by N = (n+1)/2 pairs: prologue (stage 0 of pair 0), N-1 loop
   iterations, epilogue (stage 1 of pair N-1, whose pixel b is only stored if n is even; when
   n is odd it is computed but not written - its texel read stays inside dc_source[0..127]).
   n = 1 takes a separate straight-line path (.Lc_one, review round 2): no register saves, no
   colormap check, no loop set-up. n = 2 branches once, straight to the epilogue.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawColumn_SH4)
	/* Dispatch: dc_yl and dc_yh are loaded first; a 1-pixel column (dc_yl == dc_yh, half of
	   all calls in sprite-heavy scenes) branches to .Lc_one before anything is saved, with
	   r1-r4 as .Lc_one expects them. The whole set-up is ordered by test/listsched.py
	   (aligned dual-issue list scheduling, frac chain first; the bt .Lc_one was marked
	   "barrier: r1 r2 r3 r4"; nothing after the final cmp/eq writes T). The count and colormap
	   checks branch to the C path (after popping r8-r10) before any store. */
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
cgd_col_disp:
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
	mov.l	@r7, r0			/* registered lump */
	bt	.Lc_slowpop
	mov.l	@(4,r7), r8		/* nmaps */
	shll2	r1
	mov.l	@(8,r7), r9		/* delta */
	sub	r0, r3
	sts	macl, r0
	swap.b	r3, r3
	mov.l	.Lc_columnofs, r7
	cmp/hs	r8, r3			/* not a map of the registered lump: C path */
	mov.l	.Lc_source, r5
	add	r9, r6			/* r6 = rotated colormap base */
	sub	r0, r4			/* r4 = frac9 */
	mov	#-25, r3
	mov.l	.Lc_x, r0
	mov	r4, r8
	add	r2, r4
	mov.l	@r5, r5			/* r5 = dc_source */
	mov	r4, r9
	bt	.Lc_slowpop
	mov.l	@r0, r0			/* dc_x */
	shld	r3, r8
	shld	r3, r9
	shll2	r0
	add	r5, r8			/* texel address a of pair 0 */
	mov.l	@(r0,r7), r7		/* columnofs[dc_x] */
	mov.l	.Lc_ylookup, r0
	add	r5, r9			/* texel address b of pair 0 */
	add	r1, r0
	mov.l	@r0, r1			/* ylookup[dc_yl] */
	LDRS(.Lc_rs)
	mov	#1, r0
	add	r7, r1			/* dest */
	LDRE(.Lc_re)
	mov	#80, r7
	cmp/eq	r0, r10			/* count == 1: no loop iteration */
	shll2	r7			/* r7 = 320 */
	sub	r7, r1			/* r1 = dest - 320 */
	bt/s	.Lc_epi			/* 2 pixels: stage 1 of pair 0 only */
	mov.b	@r8, r0			/* texel a of pair 0 */
cgd_col_n2:
	shlr	r10			/* r10 = iterations (>= 1); T = count & 1 = last pair is complete */
	LDRC(10)
	add	r2, r4			/* frac9 of a(1) */
	ALIGN4
.Lc_rs:
cgd_col_rs:
	mov	r4, r8			/* a(k) index */
	add	r7, r1			/* dest -> pixel a(k-1) */
	mov.b	@(r0,r6), r10		/* colour a(k-1) */
	shld	r3, r8
	mov.b	@r9, r0			/* texel b(k-1) */
	add	r5, r8			/* texel address a(k) */
	mov.b	r10, @r1		/* store a(k-1) */
	add	r2, r4
	mov	r4, r9			/* b(k) index */
	add	r7, r1			/* dest -> pixel b(k-1) */
	mov.b	@(r0,r6), r10		/* colour b(k-1) */
	shld	r3, r9
	mov.b	@r8, r0			/* texel a(k) */
	add	r5, r9			/* texel address b(k) */
	mov.b	r10, @r1		/* store b(k-1) */
.Lc_re:	add	r2, r4
cgd_col_end:
.Lc_epi:	/* stage 1 of the last pair */
	add	r7, r1
	mov.b	@(r0,r6), r10		/* colour a */
	mov.b	@r9, r0			/* texel b */
	mov.b	r10, @r1		/* store a */
	add	r7, r1
	mov.b	@(r0,r6), r2		/* colour b */
	mov.l	@r15+, r10
	bf	1f			/* odd pixel count: no pixel b */
	mov.b	r2, @r1
1:	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8
cgd_col_ret:

.Lc_slowpop:
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Lc_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	ALIGN4
	/* 1 pixel (dc_yl == dc_yh): r1 = dc_yl, r2 = dc_iscale, r3 = centery, r4 = &dc_texturemid.
	   Uses r0-r7 only (nothing saved) and looks the colour up in dc_colormap directly, so it
	   needs no registered colormap. frac = dc_texturemid + (dc_yl - centery) * dc_iscale,
	   colour = dc_colormap[dc_source[(frac >> 16) & 127]], exactly like C (the texel is
	   zero-extended here). Hand-scheduled in aligned LS|EX pairs (the nops keep them aligned);
	   32 cycles per call under "pess" including the dispatch (README section 3). */
.Lc_one:
cgd_col_one:
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
	add	r2, r6			/* dest */
	nop
	mov.b	@(r0,r3), r0		/* texel (sign-extended) */
	nop
	mov.l	@r7, r7			/* dc_colormap */
	extu.b	r0, r0
	mov.b	@(r0,r7), r0		/* colour */
	rts
	mov.b	r0, @r6
cgd_col_one_end:

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
   R_DrawColumnLow (detailshift = 1, Chocolate Doom semantics)

   count = dc_yh - dc_yl; if (count < 0) return;  x = dc_x << 1  (dc_x is not modified)
   dest = ylookup[dc_yl] + columnofs[x]; dest2 = ylookup[dc_yl] + columnofs[x+1];
   frac as R_DrawColumn; each texel is written to *dest and *dest2.

   Fast path needs columnofs[x+1] == columnofs[x] + 1 (always true with R_InitBuffer); the two
   pixels are written right to left with pre-decrement stores from dest + 2, then the pointer
   moves by 322 to the next row. Loop: 2 texels (4 pixels) per iteration, 18 instructions,
   9 cycles (2.25 cycles/pixel). Separate frac registers for texels a and b (each stepped by
   2*step9) remove the ordering constraint between them.
   Registers: r0 texel, r1 frac9 a, r11 frac9 b, r2 2*step9, r3 -25, r4 dest+2, r5 dc_source,
   r6 rotated colormap, r7 322, r8/r9 texel address a/b, r10 colour.
   1 texel (dc_yl == dc_yh) takes a separate straight-line path (.Ll_one, review round 2): no
   register saves, no colormap or contiguity check, no loop set-up. 2 texels branch once, to
   the epilogue.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawColumnLow_SH4)
	/* Dispatch on dc_yl == dc_yh first (as R_DrawColumn): 1 texel goes to .Ll_one before
	   anything is saved, with r1-r4 as .Ll_one expects them. The whole set-up is ordered by
	   test/listsched.py (aligned dual-issue list scheduling, bt .Ll_one marked "barrier: r1 r2
	   r3 r4"; nothing after the cmp/pz writes T); the checks branch to the C path (after
	   restoring the saved registers) before any store. */
	mov.l	.Ll_yl, r1
	mov	#31, r0
	mov.l	.Ll_yh, r5
	shll8	r0			/* 7936: larger counts go to C (RC is 12 bits) */
	mov.l	.Ll_iscale, r2
	mov.l	.Ll_centery, r3
	mov.l	@r1, r1			/* dc_yl */
	mov.l	@r5, r5			/* dc_yh */
	mov.l	.Ll_texmid, r4
	cmp/eq	r1, r5			/* 1 texel */
	mov.l	@r2, r2			/* dc_iscale */
	sub	r1, r5
	mov.l	@r3, r3			/* centery */
	bt	.Ll_one
cgd_coll_disp:
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
	mov.l	@r7, r0			/* registered lump */
	mul.l	r2, r3			/* MACL = (centery - dc_yl) * step9 */
	mov	r6, r3
	mov.l	r11, @-r15
	sub	r0, r3
	mov.l	@(4,r7), r10		/* nmaps */
	mov.l	.Ll_x, r0
	swap.b	r3, r3
	mov.l	@(8,r7), r11		/* delta */
	shll2	r1
	mov.l	.Ll_columnofs, r7
	bt	.Ll_slowpop
	mov.l	@r0, r0			/* dc_x */
	cmp/hs	r10, r3			/* not a map of the registered lump: C path */
	mov.l	.Ll_source, r5
	bt	.Ll_slowpop
	shll2	r0
	mov	#-25, r3
	add	r0, r0			/* (dc_x << 1) * 4 */
	add	r0, r7
	mov.l	.Ll_ylookup, r0
	add	r11, r6			/* r6 = rotated colormap base */
	mov.l	@(4,r7), r10		/* columnofs[x+1] */
	add	#-2, r8
	add	r1, r0
	mov.l	@r7, r4			/* columnofs[x] */
	add	#-1, r10
	mov.l	@r5, r5			/* r5 = dc_source */
	mov.l	@r0, r0			/* ylookup[dc_yl] */
	cmp/eq	r4, r10			/* columnofs not contiguous: C path */
	mov.w	.Ll_322, r7
	bf	.Ll_slowpop
	add	r0, r4			/* dest */
	sts	macl, r0
	add	#2, r4			/* r4 = dest + 2 */
	cmp/pz	r8			/* count >= 2: at least one loop iteration */
	sub	r0, r9			/* frac9 */
	mov	r9, r1			/* r1 = frac9 of texel a */
	mov	r9, r11
	mov	r1, r9
	shld	r3, r9
	add	r5, r9			/* texel address a */
	add	r2, r11			/* r11 = frac9 of texel b */
	mov.b	@r9, r0			/* texel a */
	mov	r11, r9
	add	r2, r2			/* r2 = 2 * step9 */
	shld	r3, r9
	add	r5, r9			/* texel address b */
	mov.b	@(r0,r6), r10		/* colour a */
	add	r2, r1
	mov.b	@r9, r0			/* texel b */
	LDRS(.Ll_rs)
	LDRE(.Ll_re)
	bf/s	.Ll_epi		/* 2 texels: stage 1 of pair 0 only */
	shlr	r8			/* T = count & 1 (pair complete); r8 = iterations - 1 */
cgd_coll_n2:
	add	#1, r8
	LDRC(8)
	add	r2, r11			/* frac9 of b(1) */
	ALIGN4
.Ll_rs:
cgd_coll_rs:
	mov.b	r10, @-r4		/* a(k-1), right pixel */
	mov	r1, r8			/* a(k) index */
	mov.b	r10, @-r4		/* a(k-1), left pixel */
	shld	r3, r8
	mov.b	@(r0,r6), r10		/* colour b(k-1) */
	add	r5, r8
	mov	r11, r9			/* b(k) index */
	add	r7, r4			/* next row */
	mov.b	@r8, r0			/* texel a(k) */
	shld	r3, r9
	mov.b	r10, @-r4		/* b(k-1) */
	add	r5, r9
	mov.b	r10, @-r4		/* b(k-1) */
	add	r2, r11
	mov.b	@(r0,r6), r10		/* colour a(k) */
	add	r7, r4			/* next row */
	mov.b	@r9, r0			/* texel b(k) */
.Ll_re:	add	r2, r1
cgd_coll_end:

.Ll_epi:	/* stage 1 of the last pair */
	mov.b	r10, @-r4
	mov.b	r10, @-r4
	mov.b	@(r0,r6), r10		/* colour b */
	add	r7, r4
	mov.l	@r15+, r11
	bf	1f			/* odd texel count: no texel b */
	mov.b	r10, @-r4
	mov.b	r10, @-r4
1:	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

cgd_coll_ret:
.Ll_slowpop:
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	mov.l	.Ll_slowfn, r0
	jmp	@r0
	mov.l	@r15+, r8

	ALIGN4
	/* 1 texel (dc_yl == dc_yh): r1 = dc_yl, r2 = dc_iscale, r3 = centery, r4 = &dc_texturemid.
	   r0-r7 only, direct dc_colormap lookup (no registered colormap needed), and two
	   independent stores to columnofs[x] and columnofs[x+1] (no contiguity needed), exactly
	   like C. Hand-scheduled in aligned pairs as .Lc_one; 33 cycles under "pess". */
.Ll_one:
cgd_coll_one:
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
	add	r6, r5			/* dest */
	mov.l	@r7, r7			/* dc_colormap */
	add	r6, r2			/* dest2 */
	extu.b	r0, r0
	nop
	mov.b	@(r0,r7), r0		/* colour */
	nop
	mov.b	r0, @r5
	rts
	mov.b	r0, @r2
cgd_coll_one_end:

	.balign 2
.Ll_322:	.word	322
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
   R_DrawSpan

   xfrac = ds_xfrac; yfrac = ds_yfrac; dest = ylookup[ds_y] + columnofs[ds_x1];
   count = ds_x2 - ds_x1;
   do { spot = ((yfrac >> 10) & 4032) + ((xfrac >> 16) & 63);
        *dest++ = ds_colormap[ds_source[spot]]; xfrac += ds_xstep; yfrac += ds_ystep; }
   while (count--);
   (count < 0 returns; the C loop would run 2^32 times.)

   Drawn right to left (pre-decrement stores, no pointer increment), starting from
   x = xfrac + count*xstep, y = yfrac + count*ystep (exact mod 2^32) and stepping backwards.
   X = x << 10 (x texel in bits 26..31), Y = y << 10 (y texel in bits 26..31):
       rm = Y >>> 26;  rn = X;  xtrct rm,rn  ->  rn = (rm << 16) | (X >>> 16);  rn >>>= 10
       -> rn = (ytexel << 6) | xtexel   (3 EX + 2 MT; the garbage fraction bits fall out)
   Loop: 2 pixels per iteration, 22 instructions, 11 cycles (5.5 cycles/pixel), 2 stages, under
   the pess, opt and gcc latency models. 11 instructions per pixel make this the issue-width floor
   (`test/modsched.py span max --shipped` checks it; d_A|a_B and h_A'|f_B' are same-cycle
   read-then-overwrite pairs, which the second instruction of a pair may do).
   Registers: r0 spot/texel, r1 rm, r2 rn, r3 colour, r4 dest (pre-decremented), r5 ds_source,
   r6 rotated colormap, r7 X, r8 Y, r9 xstep << 10, r10 ystep << 10, r11 -26, r12 -10.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawSpan_SH4)
	/* Set-up order produced by test/listsched.py (dual-issue list scheduling); the checks
	   branch to the C path (after restoring the saved registers) before any store. */
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
	cmp/hi	r4, r2			/* count < 0 || count > 7936 */
	mov.l	.Ls_columnofs, r5
	mov.l	.Ls_xstep, r9
	mov.l	@(r0,r3), r3		/* ylookup[ds_y] */
	mov	r1, r0
	mov.l	@r6, r6			/* ds_colormap */
	mov.l	.Ls_ystep, r10
	mov.l	@(r0,r5), r5		/* columnofs[ds_x1] */
	mov	r6, r1
	mov.l	@r11, r0		/* registered lump */
	mov.l	@r9, r9			/* ds_xstep */
	add	r5, r3			/* dest */
	sub	r0, r1
	mov.l	r12, @-r15
	mov.l	@(4,r11), r0		/* nmaps */
	swap.b	r1, r1
	mul.l	r2, r9			/* count * xstep */
	mov.l	.Ls_xfrac, r7
	bt	.Ls_slowpop
	mov.l	@r10, r10		/* ds_ystep */
	cmp/hs	r0, r1			/* not a map of the registered lump: C path */
	mov.l	@(8,r11), r0		/* delta */
	mov	#10, r1
	mov.l	.Ls_yfrac, r8
	shld	r1, r9			/* xstep << 10 */
	add	r0, r6			/* r6 = rotated colormap base */
	mov.l	@r7, r7			/* ds_xfrac */
	sts	macl, r0
	mul.l	r2, r10			/* count * ystep */
	mov.l	@r8, r8			/* ds_yfrac */
	add	r0, r7			/* xfrac of the last pixel */
	mov.l	.Ls_source, r12
	shld	r1, r7			/* X */
	sts	macl, r0
	add	r0, r8			/* yfrac of the last pixel */
	mov	r3, r4
	shld	r1, r8			/* Y */
	mov.l	@r12, r12		/* ds_source */
	shld	r1, r10			/* ystep << 10 */
	mov	#-26, r11
	mov	r8, r1			/* a_A */
	add	r2, r4
	mov	r2, r3			/* r3 = count */
	shld	r11, r1			/* b_A */
	mov	r7, r2			/* c_A */
	mov	r12, r5			/* r5 = ds_source */
	mov	#-10, r12
	xtrct	r1, r2			/* d_A */
	sub	r10, r8			/* k_A */
	bt	.Ls_slowpop
	shld	r12, r2			/* e_A */
	mov	r8, r1			/* a_B */
	sub	r9, r7			/* j_A */
	mov	r2, r0			/* f_A */
	shld	r11, r1			/* b_B */
	mov	r7, r2			/* c_B */
	add	#1, r4			/* r4 = dest + count + 1 */
	mov.b	@(r0,r5), r0		/* g_A: texel A */
	sub	r10, r8			/* k_B */
	sub	r9, r7			/* j_B */
	xtrct	r1, r2			/* d_B */
	add	#-2, r3
	LDRS(.Ls_rs)
	LDRE(.Ls_re)
	cmp/pz	r3			/* count >= 2: at least one loop iteration */
	bf/s	.Ls_epi
	shlr	r3			/* T = count & 1 (pair complete); r3 = iterations - 1 */
	add	#1, r3
	LDRC(3)
	nop
	ALIGN4
.Ls_rs:
cgd_span_rs:
	mov	r8, r1			/* a_A      Y -> rm */
	shld	r12, r2			/* e_B'     */
	mov.b	@(r0,r6), r3		/* h_A'     colour A' */
	mov	r2, r0			/* f_B'     */
	mov	r7, r2			/* c_A      X -> rn */
	shld	r11, r1			/* b_A      */
	mov.b	@(r0,r5), r0		/* g_B'     texel B' */
	sub	r10, r8			/* k_A      */
	mov.b	r3, @-r4		/* i_A'     store A' */
	sub	r9, r7			/* j_A      */
	xtrct	r1, r2			/* d_A      */
	mov	r8, r1			/* a_B      */
	mov.b	@(r0,r6), r3		/* h_B'     colour B' */
	shld	r12, r2			/* e_A      */
	mov	r2, r0			/* f_A      */
	shld	r11, r1			/* b_B      */
	mov	r7, r2			/* c_B      */
	sub	r10, r8			/* k_B      */
	mov.b	@(r0,r5), r0		/* g_A      texel A */
	sub	r9, r7			/* j_B      */
	mov.b	r3, @-r4		/* i_B'     store B' */
.Ls_re:	xtrct	r1, r2			/* d_B      */
cgd_span_end:

.Ls_epi:	/* stage 1 of the last pair */
	shld	r12, r2			/* e_B */
	mov.b	@(r0,r6), r3		/* h_A */
	mov	r2, r0			/* f_B */
	mov.b	r3, @-r4		/* i_A */
	mov.b	@(r0,r5), r0		/* g_B */
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.b	@(r0,r6), r3		/* h_B */
	mov.l	@r15+, r10
	bf	1f			/* odd pixel count: no pixel B */
	mov.b	r3, @-r4		/* i_B */
1:	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

cgd_span_ret:
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
   R_DrawSpanLow (detailshift = 1, Chocolate Doom semantics)

   count = ds_x2 - ds_x1 (before doubling); ds_x1 <<= 1; ds_x2 <<= 1 (globals, as in C);
   dest = ylookup[ds_y] + columnofs[ds_x1]; each texel is written to 2 consecutive pixels.
   Same structure (registers, stage split, prologue and epilogue) as R_DrawSpan with a second
   pre-decrement store per texel: 2 texels (4 pixels) per iteration, 24 instructions, 12 cycles
   (3.0 cycles/pixel) under the pess, opt and gcc latency models (was 26 instructions, 13 cycles).
   The loop body was found by `test/modsched.py spanlow max --stage1 Be,Bf,Bg,Ah,Bh,Ai,Ai2,Bi,Bi2`
   (the R_DrawSpan stage split, so the prologue and epilogue are unchanged) and is checked by
   `modsched.py spanlow max --shipped`: c0..c11 are its rows, each an aligned pair; 8 LS + 10 EX
   + 6 MT fill all 24 slots. Two pairs (c1, c6) read r0 and overwrite it in the same cycle.
  ============================================================================================*/

	CGD_SH4_HOT
	.balign 4
FUNC(R_DrawSpanLow_SH4)
	/* Set-up order produced by test/listsched.py (dual-issue list scheduling); the checks
	   branch to the C path (after restoring the saved registers) before any store. */
	mov.l	.Lsl_y, r0
	mov	#31, r4
	mov.l	.Lsl_x1, r1
	shll8	r4			/* 7936 */
	mov.l	.Lsl_x2, r2
	mov.l	@r0, r0			/* ds_y */
	mov.l	r8, @-r15
	mov.l	.Lsl_ylookup, r3
	shll2	r0
	mov.l	@r1, r1			/* ds_x1 */
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
	mov.l	.Lsl_xstep, r9
	add	r0, r0			/* (ds_x1 << 1) * 4 */
	mov.l	@r6, r6			/* ds_colormap */
	mov.l	.Lsl_ystep, r10
	mov.l	@(r0,r5), r5		/* columnofs[ds_x1 << 1] */
	mov	r6, r1
	mov.l	@r11, r0		/* registered lump */
	mov.l	@r9, r9			/* ds_xstep */
	add	r5, r3			/* dest */
	sub	r0, r1
	mov.l	r12, @-r15
	mov.l	@(4,r11), r0		/* nmaps */
	swap.b	r1, r1
	mul.l	r2, r9			/* count * xstep */
	mov.l	.Lsl_xfrac, r7
	bt	.Lsl_slowpop
	mov.l	@r10, r10		/* ds_ystep */
	cmp/hs	r0, r1			/* not a map of the registered lump: C path */
	mov.l	@(8,r11), r0		/* delta */
	mov	#10, r1
	mov.l	.Lsl_yfrac, r8
	mov	r3, r4
	add	r0, r6			/* r6 = rotated colormap base */
	mov.l	@r7, r7			/* ds_xfrac */
	sts	macl, r0
	mul.l	r2, r10			/* count * ystep */
	mov.l	@r8, r8			/* ds_yfrac */
	add	r0, r7			/* xfrac of the last texel */
	mov.l	.Lsl_source, r12
	shld	r1, r9			/* xstep << 10 */
	sts	macl, r0
	add	r0, r8			/* yfrac of the last texel */
	mov	#-26, r11
	shld	r1, r8			/* Y */
	mov.l	@r12, r12		/* ds_source */
	shld	r1, r7			/* X */
	mov	r2, r3			/* r3 = count */
	shld	r1, r10			/* ystep << 10 */
	mov	r12, r5			/* r5 = ds_source */
	add	r2, r4
	mov	r8, r1			/* a_A */
	add	r2, r4
	mov	#-10, r12
	mov	r7, r2			/* c_A */
	shld	r11, r1			/* b_A */
	xtrct	r1, r2			/* d_A */
	bt	.Lsl_slowpop
	sub	r10, r8			/* k_A */
	shld	r12, r2			/* e_A */
	mov	r8, r1			/* a_B */
	sub	r9, r7			/* j_A */
	mov	r2, r0			/* f_A */
	shld	r11, r1			/* b_B */
	mov	r7, r2			/* c_B */
	add	#2, r4			/* r4 = dest + 2 * (count + 1) */
	mov.b	@(r0,r5), r0		/* g_A: texel A */
	sub	r10, r8			/* k_B */
	sub	r9, r7			/* j_B */
	xtrct	r1, r2			/* d_B */
	add	#-2, r3
	LDRS(.Lsl_rs)
	LDRE(.Lsl_re)
	cmp/pz	r3			/* count >= 2: at least one loop iteration */
	bf/s	.Lsl_epi
	shlr	r3			/* T = count & 1 (pair complete); r3 = iterations - 1 */
	add	#1, r3
	LDRC(3)
	nop
	ALIGN4
.Lsl_rs:
cgd_spanl_rs:
	mov	r8, r1			/* c0  a_A    Y -> rm */
	shld	r12, r2			/*     e_B'   */
	mov.b	@(r0,r6), r3		/* c1  h_A'   colour A' (reads r0 before f_B' overwrites it) */
	mov	r2, r0			/*     f_B'   */
	shld	r11, r1			/* c2  b_A    */
	mov	r7, r2			/*     c_A    X -> rn */
	xtrct	r1, r2			/* c3  d_A    */
	mov.b	@(r0,r5), r0		/*     g_B'   texel B' */
	shld	r12, r2			/* c4  e_A    */
	mov.b	r3, @-r4		/*     i_A'   store A' */
	sub	r10, r8			/* c5  k_A    */
	mov.b	r3, @-r4		/*     i_A'   store A' (second pixel) */
	mov.b	@(r0,r6), r3		/* c6  h_B'   colour B' (reads r0 before f_A overwrites it) */
	mov	r2, r0			/*     f_A    */
	sub	r9, r7			/* c7  j_A    */
	mov	r8, r1			/*     a_B    */
	mov.b	@(r0,r5), r0		/* c8  g_A    texel A */
	shld	r11, r1			/*     b_B    */
	sub	r10, r8			/* c9  k_B    */
	mov.b	r3, @-r4		/*     i_B'   store B' */
	mov	r7, r2			/* c10 c_B    (reads X before j_B updates it) */
	sub	r9, r7			/*     j_B    */
	xtrct	r1, r2			/* c11 d_B    */
.Lsl_re:	mov.b	r3, @-r4		/*     i_B'   store B' (second pixel) */
cgd_spanl_end:

.Lsl_epi:
	shld	r12, r2
	mov.b	@(r0,r6), r3
	mov	r2, r0
	mov.l	.Lsl_x1, r1
	mov.b	r3, @-r4
	mov.l	.Lsl_x2, r2
	mov.b	r3, @-r4
	mov.l	@r1, r7
	mov.b	@(r0,r5), r0
	mov.l	@r2, r8
	mov.l	@r15+, r12
	add	r7, r7
	mov.l	@r15+, r11
	add	r8, r8
	mov.b	@(r0,r6), r3
	mov.l	r7, @r1			/* ds_x1 <<= 1 (as the C version, which does it first) */
	mov.l	@r15+, r10
	mov.l	r8, @r2			/* ds_x2 <<= 1 */
	bf	1f
	mov.b	r3, @-r4
	mov.b	r3, @-r4
1:	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

cgd_spanl_ret:
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

/* R_DrawTranslatedColumn / R_DrawTranslatedColumnLow
   *dest = dc_colormap[dc_translation[dc_source[frac >> 16]]]   (arithmetic shift, no mask)
   Low: x = dc_x << 1, pixels at columnofs[x] and columnofs[x+1] (two pointers, no precondition).
   Shared body: every texel is stored through r4 and through r8, both stepped by 320.
   Low: r8 = second destination. Normal: r8 = r4, so the second store rewrites the same byte
   with the same value (no other memory is touched; review round 1 fixed a version where r8
   pointed at a 4-byte scratch object and walked 320 bytes per pixel through .bss). */

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
	ROTCM	r6, r7, r0, r5, .Lt_slowpop
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
	add	r7, r4			/* dest */
	tst	r3, r3
	mov.l	@(4,r5), r8
	bf/s	1f
	add	r7, r8			/* dest2 = ylookup + columnofs[x+1] (low) */
	mov	r4, r8			/* normal: second store = same byte, same value */
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
	mov	#80, r7
	shll2	r7			/* r7 = 320 */
	ALIGN4
cgd_tr_rs:
2:	swap.w	r1, r0
	add	r2, r1
	exts.w	r0, r0			/* frac >> 16 */
	mov.b	@(r0,r3), r0		/* texel */
	extu.b	r0, r0
	mov.b	@(r0,r5), r0		/* translated (sign-extended) */
	mov.b	@(r0,r6), r0		/* colour */
	mov.b	r0, @r4
	mov.b	r0, @r8
	add	r7, r4
3:	add	r7, r8
cgd_tr_end:
	rts
	mov.l	@r15+, r8

.Lt_slowpop:
	mov.l	@r15+, r0
	jmp	@r0
	mov.l	@r15+, r8

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

/* R_DrawFuzzColumn / R_DrawFuzzColumnLow
   if (!dc_yl) dc_yl = 1; if (dc_yh == viewheight-1) dc_yh = viewheight-2;   (globals)
   count = dc_yh - dc_yl; if (count < 0) return;
   *dest = colormaps[6*256 + dest[fuzzoffset[fuzzpos]]]; fuzzpos = (fuzzpos + 1) % 50 per row.
   The offsets come from a 250-entry unrolled copy of fuzzoffset[] starting at fuzzpos, so the
   loop has no wrap test (count <= 199, else C); fuzzpos += pixels (mod 50) at the end.
   Rows are processed strictly in order (dest[-320] must see the row just written). */

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
	mov	#199, r2
	extu.b	r2, r2
	cmp/hi	r2, r0			/* count < 0 (C returns) or count > 199 */
	bt	.Lf_slowpop
	mov	r0, r2
	mov.l	.Lf_fuzzpos, r5
	mov.l	@r5, r5
	mov	#50, r0
	cmp/hs	r0, r5			/* fuzzpos outside [0,49]: C */
	bt	.Lf_slowpop
	mov.l	.Lf_colormaps, r6
	mov	#6, r0
	mov.l	@r6, r6
	shll8	r0
	add	r0, r6			/* colormaps + 6*256 */
	mov.l	.Lf_cmaps, r7
	ROTCM	r6, r7, r0, r8, .Lf_slowpop
	add	#4, r15
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
	mov.l	@r7, r4
	add	r0, r4			/* dest */
	mov.l	@(4,r7), r8
	add	r0, r8			/* dest2 (low) */
	mov.l	.Lf_tab, r1
	mov	r5, r0
	add	r0, r0
	add	r0, r1			/* r1 = &fuzztab[fuzzpos] */
	add	r2, r5
	add	#1, r5			/* fuzzpos + pixels */
	add	#1, r2			/* pixels */
	LDRS(.Lf_rs1)
	LDRE(.Lf_re1)
	mov	#80, r7
	shll2	r7			/* 320 */
	tst	r3, r3
	bf	.Lf_low
	LDRC(2)
	nop
	ALIGN4
.Lf_rs1:
cgd_fz_rs:
	mov.w	@r1+, r0		/* +-320 */
	mov.b	@(r0,r4), r0		/* dest[offset] */
	mov.b	@(r0,r6), r3		/* colormaps[6*256 + ...] */
	mov.b	r3, @r4
.Lf_re1:	add	r7, r4
cgd_fz_end:
	bra	.Lf_done
	nop

.Lf_low:
	LDRS(.Lf_rs2)
	LDRE(.Lf_re2)
	LDRC(2)
	nop
	ALIGN4
.Lf_rs2:
	mov.w	@r1+, r2
	mov	r2, r0
	mov.b	@(r0,r4), r0
	mov.b	@(r0,r6), r3
	mov.b	r3, @r4
	mov	r2, r0
	mov.b	@(r0,r8), r0
	mov.b	@(r0,r6), r3
	mov.b	r3, @r8
	add	r7, r4
.Lf_re2:	add	r7, r8

.Lf_done:	/* fuzzpos = (fuzzpos + pixels) % 50, at most 248 */
	mov	#50, r1
3:	cmp/hs	r1, r5
	bf	4f
	bra	3b
	sub	r1, r5
4:	mov.l	.Lf_fuzzpos, r0
	mov.l	r5, @r0
	rts
	mov.l	@r15+, r8

.Lf_slowpop:
	mov.l	@r15+, r0
	jmp	@r0
	mov.l	@r15+, r8

	.balign 4
.Lf_yl:		.long	_dc_yl
.Lf_yh:		.long	_dc_yh
.Lf_viewheight:	.long	_viewheight
.Lf_fuzzpos:	.long	_fuzzpos
.Lf_colormaps:	.long	_colormaps
.Lf_cmaps:	.long	_cgd_sh4_cmaps
.Lf_x:		.long	_dc_x
.Lf_columnofs:	.long	_columnofs
.Lf_ylookup:	.long	_ylookup
.Lf_tab:	.long	cgd_sh4_fuzztab
.Lf_slow:	.long	_cgd_sh4_slow_fuzz
.Lf_slowlow:	.long	_cgd_sh4_slow_fuzz_low
ENDFUNC(R_DrawFuzzColumn_SH4)

	.section .rodata
	.balign 4
/* fuzzoffset[] of r_draw.c (FUZZOFF = SCREENWIDTH), repeated 5 times */
cgd_sh4_fuzztab:
	.rept 5
	.short	320,-320,320,-320,320,320,-320,320,320,-320,320,320,320,-320,320,320,320,-320,-320,-320
	.short	-320,320,-320,-320,320,320,320,320,-320,320,-320,320,320,-320,-320,320,320,-320,-320,-320
	.short	-320,320,320,320,320,-320,320,320,-320,320
	.endr

