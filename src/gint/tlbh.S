/* CGDoom gint platform: TLB-miss handler and UTLB writer (fx-CG100, MPM).

   MPM maps the add-in image with 64 KiB UTLB entries at 0x00300000, page i
   in slot i: slots 0..54 under mpm.bin v1.1 and dynload, only 0..47 under
   the Feb-2025 public build (and a1a2cc3, 0fb6cec), which leave 48..54
   untouched and never flush the TLB between launches (see tlb.c, exit
   normalisation). All set URB=54, so entries 0..53 can be replaced by any
   LDTLB. gint calls the OS with the MMU on, and the OS does load TLB entries
   (gpSP saw P0 block pointers from Bfile_GetBlockAddress), so one of our
   code pages may silently disappear during a world switch. gint's own miss
   handler then jumps to the fx-CG50 syscall gate (0x80020070, syscall 12),
   which does not exist on the fx-CG100.

   1. cgd_tlbh_start..cgd_tlbh_end: miss handler, copied by tlb.c over
   gint's at VBR+0x400 (fx-CG100 only; the fx-CG50 needs gint's handler for
   the OS demand paging). For a TEA in [0x00300000, 0x00700000) it reloads
   the page from the snapshot taken at startup (PTEH/PTEL + ldtlb, the
   sequence of mpm/loader.c MMU_Map), counts the miss and returns with rte.
   Anything else goes to VBR+0x100 like gint's handler does (panic screen).

   It is only reached while gint's VBR is installed. In the window between
   gint_world_switch_out() and gint_world_switch_in() (our os_* callbacks,
   gint_call, the on-chip memory restore...) the OS's VBR is active and a
   miss on our pages goes to the OS's handler instead: that window is
   covered by pinning (tlb.c / tlbplan.c), not by this handler.

   Execution context: exception entry, SR.MD=1, SR.BL=1, SR.RB=1. Only
   bank-1 r0..r7 are used (free in exception context, as in gint's handler),
   no stack, no memory access through the TLB: the code runs in the VBR area
   (P1) and the table is a P1 heap buffer.

   Position independent: copied as a block, literals are PC-relative, and
   the table address is patched into cgd_tlbh_table after the copy. The
   block also carries cgd_tlbh_set_mmucr (round 3.1), a P1 MMUCR writer.

   2. cgd_tlb_apply: performs a list of UTLB loads (gtlb_op_t). tlb.c calls
   it through its P2 alias with interrupts masked, so that moving the entry
   of the page this code lives on is harmless. */

	.section .text
	.align	4
	.global	_cgd_tlbh_start
	.global	_cgd_tlbh_end
	.global	_cgd_tlbh_table

_cgd_tlbh_start:
	mov.l	.Ltea, r0
	mov.l	@r0, r1			/* r1 = TEA (faulting address) */
	mov.l	.Lbase, r2
	mov	r1, r3
	sub	r2, r3			/* r3 = TEA - 0x00300000 */
	mov.l	.Lspan, r2
	cmp/hs	r2, r3			/* T = (r3 >= span), unsigned */
	bt	.Lpanic
	shlr16	r3			/* page index 0..63 */
	shll2	r3
	shll	r3			/* byte offset in the table (8/entry) */
	mov.l	.Ltable, r2
	tst	r2, r2
	bt	.Lpanic			/* no table */
	add	r2, r3			/* r3 = &table[page] */
	mov.l	@(4,r3), r5		/* PTEL value */
	tst	r5, r5
	bt	.Lpanic			/* not one of our pages */
	mov.l	@r3, r4			/* PTEH value */

	/* Miss counter at table + 512 (diagnostics) */
	mov	#2, r6
	shll8	r6			/* 512 */
	add	r2, r6
	mov.l	@r6, r7
	add	#1, r7
	mov.l	r7, @r6

	/* Load the entry at MMUCR.URC (a replaceable slot) */
	mov.l	.Lpteh, r0
	mov.l	r4, @r0			/* PTEH */
	mov.l	r5, @(4,r0)		/* PTEL */
	ldtlb
	nop
	rte
	nop

.Lpanic:
	/* Same as gint's handler: default to the general exception handler */
	stc	vbr, r0
	mov	#1, r1
	shll8	r1
	add	r1, r0
	jmp	@r0
	nop

	.align	2
.Ltea:	.long	0xff00000c		/* TEA */
.Lbase:	.long	0x00300000
.Lspan:	.long	0x00400000		/* 64 pages of 64 KiB */
.Lpteh:	.long	0xff000000		/* PTEH; PTEL at +4 */
_cgd_tlbh_table:
.Ltable: .long	0			/* patched by tlb.c */

/* void set_mmucr(uint32_t mmucr), called by tlb.c at its copy in the VBR
   area (gint review round 3.1): MMUCR = r4, then icbi, run from P1 as the
   SH-4A requires for an MMUCR write. No ldtlb, nothing translated. Only
   used when the P2 executor below is unavailable, to keep the URB inside
   the slots MPM rewrites (tlb.c, contain_urb). Interrupts masked by the
   caller. Never reached by the miss handler above (rte/jmp before it). */
	.global	_cgd_tlbh_set_mmucr
_cgd_tlbh_set_mmucr:
	mov.l	.Lmmucr1, r1
	mov.l	r4, @r1			/* MMUCR */
	mov.l	.Lp2base1, r1
	icbi	@r1
	rts
	nop
	.align	2
.Lmmucr1:	.long	0xff000010
.Lp2base1:	.long	0xa0000000
_cgd_tlbh_end:

/* void cgd_tlb_apply(gtlb_op_t const *ops, int n, uint32_t mmucr)

   For each op { slot, pteh, ptel } (12 bytes, cgd_gint.h):
     - if ptel has V set: invalidate every OTHER valid UTLB entry whose
       64 KiB page equals pteh's (loading an invalid entry with ldtlb), so
       that the UTLB never holds two valid entries for one address;
     - MMUCR.URC = slot; icbi; PTEH = pteh; PTEL = ptel; ldtlb.
   Then MMUCR = mmucr (the caller's URB, URC = 0) and icbi.

   Must be called through its P2 alias (0xa0000000 | physical address)
   with interrupts masked: it fetches no instruction and reads no data
   through the TLB (ops is a P1 buffer; the UTLB arrays and MMU registers
   are in P4). The stack is only used to save r8..r14 (gint's stack and our
   private stack are P1; MPM's user RAM pages are above URB anyway). */

	.align	4
	.global	_cgd_tlb_apply

.macro	LOADTLB rs, rh, rl		/* clobbers r0, r7 */
	mov.l	@r8, r7
	and	r10, r7			/* MMUCR without URC */
	mov	\rs, r0
	shll8	r0
	shll2	r0			/* slot << 10 */
	or	r0, r7
	mov.l	r7, @r8			/* MMUCR.URC = slot */
	icbi	@r11
	mov.l	\rh, @r9		/* PTEH */
	mov.l	\rl, @(4,r9)		/* PTEL */
	ldtlb
	nop
.endm

_cgd_tlb_apply:
	mov.l	r8, @-r15
	mov.l	r9, @-r15
	mov.l	r10, @-r15
	mov.l	r11, @-r15
	mov.l	r12, @-r15
	mov.l	r13, @-r15
	mov.l	r14, @-r15

	mov.l	.Lmmucr, r8		/* r8 = &MMUCR */
	mov.l	.Lpteh2, r9		/* r9 = &PTEH (PTEL at +4) */
	mov.l	.Lurcmask, r10		/* r10 = ~URC */
	mov.l	.Lp2base, r11		/* r11 = 0xa0000000 (icbi target) */
	mov	#0, r13			/* r13 = 0 (invalid PTEL) */
	tst	r5, r5
	bt	.Ldone

.Lop:
	mov.l	@r4+, r1		/* r1 = slot */
	mov.l	@r4+, r2		/* r2 = pteh */
	mov.l	@r4+, r3		/* r3 = ptel */
	mov	r3, r0
	shlr8	r0
	tst	#1, r0
	bt	.Lload			/* invalid entry: nothing to dedupe */

	mov.l	.Lvpn64, r12
	and	r2, r12			/* r12 = 64 KiB page of the new entry */
	mov	#63, r14		/* r14 = i, 63 down to 0 */
.Lscan:
	cmp/eq	r14, r1
	bt	.Lnext			/* the target slot itself */
	mov	r14, r7
	shll8	r7
	mov.l	.Laddrarr, r0
	or	r0, r7
	mov.l	@r7, r7			/* address array entry i */
	mov	r7, r0
	shlr8	r0
	tst	#1, r0
	bt	.Lnext			/* not valid */
	mov.l	.Lvpn64, r0
	and	r7, r0
	cmp/eq	r12, r0
	bf	.Lnext			/* another page */
	LOADTLB	r14, r12, r13		/* invalidate slot i */
.Lnext:
	add	#-1, r14
	cmp/pz	r14
	bt	.Lscan

.Lload:
	LOADTLB	r1, r2, r3
	dt	r5
	bf	.Lop

.Ldone:
	mov.l	r6, @r8			/* final MMUCR (URB, URC = 0) */
	icbi	@r11

	mov.l	@r15+, r14
	mov.l	@r15+, r13
	mov.l	@r15+, r12
	mov.l	@r15+, r11
	mov.l	@r15+, r10
	mov.l	@r15+, r9
	rts
	mov.l	@r15+, r8

	.align	2
.Lmmucr:	.long	0xff000010
.Lpteh2:	.long	0xff000000
.Lurcmask:	.long	0xffff03ff
.Lp2base:	.long	0xa0000000
.Lvpn64:	.long	0xffff0000
.Laddrarr:	.long	0xf6000000
