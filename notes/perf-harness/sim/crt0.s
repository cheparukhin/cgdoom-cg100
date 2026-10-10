	.section .text.entry
	.global _start
_start:
	mov.l	stack_top, r15
	mov.l	main_addr, r0
	jsr	@r0
	nop
	mov	r0, r5
	mov	#1, r4
	trapa	#34
1:	bra	1b
	nop
	.align 2
stack_top:	.long 0x00700000
main_addr:	.long _main
