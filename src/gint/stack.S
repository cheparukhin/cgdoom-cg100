/* CGDoom gint platform: run a function on a private stack.

     void cgd_call_on_stack(void (*fn)(void), void *stack_top);

   Under MPM the add-in runs on the OS stack, whose size is unknown (gint
   never sets r15; Fugue's BFile has been seen overflowing 8-12 KiB stacks).
   main() runs the game on a heap stack of known size instead (P1 RAM, so
   interrupt handlers and OS code during world switches can use it too).

   r14 is callee-saved in the SH ABI, so it keeps the old stack pointer
   across the call. A longjmp() out of fn (PL_Fatal) restores the r15 saved
   by setjmp() on the original stack, so leaving early is safe as well. */

	.section .text
	.align	2
	.global	_cgd_call_on_stack

_cgd_call_on_stack:
	mov.l	r14, @-r15
	sts.l	pr, @-r15
	mov	r15, r14		/* r14 = old stack */
	jsr	@r4
	mov	r5, r15			/* delay slot: switch to the new stack */
	mov	r14, r15		/* back to the old stack */
	lds.l	@r15+, pr
	rts
	mov.l	@r15+, r14
