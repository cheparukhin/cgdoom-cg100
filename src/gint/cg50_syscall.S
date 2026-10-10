/* CGDoom gint platform: fx-CG50 trampoline for Bfile_GetBlockAddress.

     int cgd_cg50_get_block_address(int fd, int offset, void **ptr);

   gint does not wrap this OS function. On the fx-CG50 family it is syscall
   0x1DAA (libfxcg: Bfile_GetBlockAddress, SimonLothar's syscall list): r0 is
   the syscall number, r4-r6 pass through untouched, and the syscall gate at
   0x80020070 returns (PC = PR) straight to our caller with the result in r0.

   MUST be called in the OS world, i.e. through gint_world_switch(), like
   gint's own BFile_* wrappers. Never used on the fx-CG100, which has no
   syscall table (sys.c uses the per-OS addresses there).

   Pattern from gpSP-sh4-jit (KaraRyougi), ports/fxcg100/gint-gpsp/src/
   cg50_syscall.S, hardware-proven on the fx-CG50. */

	.section .text
	.align	2
	.global	_cgd_cg50_get_block_address

_cgd_cg50_get_block_address:
	mov.l	1f, r2
	mov.l	2f, r0
	jmp	@r2
	nop

	.align	2
1:	.long	0x80020070
2:	.long	0x00001daa
