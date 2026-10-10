/* SH4A / SH4AL-DSP instructions that GCC (-m4-nofpu) never emits but hand-written asm may use.
   Assembled with -isa=sh4al-dsp (SIM_ASFLAGS). Called from selftest.c. */
	.text
	.align	2

/* uint32_t t_movua(const void *p)  - unaligned 32-bit load */
	.global	_t_movua
_t_movua:
	movua.l	@r4, r0
	rts
	nop

/* uint32_t t_llsc(volatile uint32_t *p)  - atomic increment with movli.l/movco.l, returns new value */
	.global	_t_llsc
_t_llsc:
1:	movli.l	@r4, r0
	add	#1, r0
	movco.l	r0, @r4
	bf	1b
	rts
	nop

/* void t_cacheops(void *line)  - cache/ordering instructions (no-ops in the simulator) */
	.global	_t_cacheops
_t_cacheops:
	pref	@r4
	synco
	ocbp	@r4
	ocbwb	@r4
	ocbi	@r4
	prefi	@r4
	icbi	@r4
	mov	#0, r0
	movca.l	r0, @r4
	rts
	nop

/* uint32_t t_dsp_loop(uint32_t count)  - zero-overhead repeat loop (SH4AL-DSP), returns iterations done.
   Needs the final ELF to be tagged sh4al-dsp, otherwise the simulator does not decode ldrs/ldre/ldrc. */
	.global	_t_dsp_loop
_t_dsp_loop:
	mov	#0, r0
	ldrs	2f
	ldre	3f
	ldrc	r4
	nop
2:	add	#1, r0
	nop
	nop
3:	nop
	rts
	nop

/* uint32_t t_dsp_alu(void)  - a DSP data-path op: A0 = X0 + Y0 with X0 = 0x00030000, Y0 = 0x00040000 */
	.global	_t_dsp_alu
_t_dsp_alu:
	mov	#3, r1
	mov	#4, r2
	shll16	r1
	shll16	r2
	lds	r1, x0
	lds	r2, y0
	padd	x0, y0, a0
	sts	a0, r0
	rts
	nop
