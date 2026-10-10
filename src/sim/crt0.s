/* crt0 for the sh-elf-run harness (SH4, big endian).
   Sets up the stack and enters __sim_start() (syscalls.c), which runs the constructors, builds
   argc/argv through the simulator's argc/argn syscalls, calls main() and exits via "trapa #34".
   (Assembler symbols carry the toolchain's '_' prefix: ___sim_start is the C function __sim_start.)
   .bss is NOT cleared: the simulator's RAM is mmap'ed and therefore zero-filled at start, and a
   clearing loop would only add noise to the instruction statistics. */
	.section .text.entry,"ax"
	.global	_start
	.align	2
_start:
	mov.l	.Lstack, r15
	mov.l	.Lstart, r0
	jsr	@r0
	nop
	/* __sim_start never returns; if it does, exit(1) */
	mov	#1, r4
	mov	#1, r5
	trapa	#34
.Lhang:
	bra	.Lhang
	nop

	.align	2
.Lstack:	.long	___sim_stack_top
.Lstart:	.long	___sim_start
