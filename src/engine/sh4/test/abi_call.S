/* abi_call.S - calling-convention check for the void(void) drawers (test only).

   void cgd_test_abi_call(void (*fn)(void), uint32_t out[9], uint32_t p);

   Loads the callee-saved registers with values derived from p, calls fn, and stores what it
   finds afterwards: out[0..6] = r8..r14, out[7] = r15 after the call, out[8] = r15 before it.
   The caller (test_sh4.c, abi_expect()) recomputes the expected values:
     r8 = p, r9 = ~p, r10 = swap.w p, r11 = swap.b p, r12 = -p, r13 = p << 1, r14 = p >> 1.
   Added in review round 2 with the 1-pixel column paths, which must not touch r8-r14 because
   they skip the register saves of the main path. */

	.text
	.balign 4
	.global _cgd_test_abi_call
_cgd_test_abi_call:
	mov.l	r8, @-r15
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov.l	r11, @-r15
	mov.l	r12, @-r15
	mov.l	r13, @-r15
	mov.l	r14, @-r15
	sts.l	pr, @-r15
	mov.l	r5, @-r15
	mov.l	r15, @(32,r5)		/* out[8] = r15 before the call */
	mov	r6, r8
	not	r6, r9
	swap.w	r6, r10
	swap.b	r6, r11
	neg	r6, r12
	mov	r6, r13
	shll	r13
	mov	r6, r14
	jsr	@r4
	shlr	r14
	mov	r15, r1
	mov.l	@r15+, r0		/* out */
	mov.l	r8, @r0
	mov.l	r9, @(4,r0)
	mov.l	r10, @(8,r0)
	mov.l	r11, @(12,r0)
	mov.l	r12, @(16,r0)
	mov.l	r13, @(20,r0)
	mov.l	r14, @(24,r0)
	mov.l	r1, @(28,r0)		/* out[7] = r15 after the call */
	lds.l	@r15+, pr
	mov.l	@r15+, r14
	mov.l	@r15+, r13
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8
	.size	_cgd_test_abi_call, . - _cgd_test_abi_call
