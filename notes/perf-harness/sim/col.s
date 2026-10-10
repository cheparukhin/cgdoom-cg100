/* Column drawers for the SH simulator benchmark.
   void col_asm(u8 *dest, const u8 *src, const u8 *cm2, int count, u32 frac9, u32 step9)
   r4 dest, r5 src (128-byte texture column), r6 cm2 = doubled colormap + 256
   (indexed by the sign-extended texel), r7 count >= 1, @(0,r15) frac<<9,
   @(4,r15) step<<9.  Texel index = frac9 >> 25 == (frac>>16) & 127.
   Row-major 8-bit framebuffer, stride 320. */
	.text
	.align 4
	.global _col_asm
_col_asm:
	mov.l	@r15, r1	/* frac9 */
	mov.l	@(4,r15), r2	/* step9 */
	mov	#-25, r3
	mov.l	r8, @-r15
	mov.w	.Lstride, r8
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov	r1, r10
	shld	r3, r10		/* r10 = index of pixel 0 */
	add	r2, r1		/* frac now refers to pixel 1 */
.Lloop:
	mov	r10, r0		/* MT  r0 = index n            */
	mov	r1, r10		/* MT  start index n+1          */
	mov.b	@(r0,r5), r0	/* LS  texel n (sign-extended)  */
	shld	r3, r10		/* EX  index n+1                */
	add	r2, r1		/* EX  frac += step             */
	dt	r7		/* EX                           */
	mov.b	@(r0,r6), r9	/* LS  color n                  */
	mov.b	r9, @r4		/* LS  store n                  */
	bf/s	.Lloop
	add	r8, r4		/* EX  dest += 320 (delay slot) */
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

/* Same, but with the SH4AL-DSP zero-overhead repeat loop (as used by Azur):
   no dt/bf in the body. RC is 12 bits, so count must be <= 4095. */
	.align 4
	.global _col_asm_rpt
_col_asm_rpt:
	mov.l	@r15, r1
	mov.l	@(4,r15), r2
	mov	#-25, r3
	mov.l	r8, @-r15
	mov.w	.Lstride, r8
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov	r1, r10
	shld	r3, r10
	add	r2, r1
	ldrs	2f
	ldre	3f
	ldrc	r7
	nop
2:	mov	r10, r0
	mov	r1, r10
	mov.b	@(r0,r5), r0
	shld	r3, r10
	add	r2, r1
	mov.b	@(r0,r6), r9
	mov.b	r9, @r4
3:	add	r8, r4
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

	.align 1
.Lstride:	.short	320
