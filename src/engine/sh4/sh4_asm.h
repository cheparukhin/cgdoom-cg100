/* sh4_asm.h - section selection for the SH4 assembly files (included from .S files only).

   CGD_SH4_HOT   hot drawers (column/span + low detail): ".ilram" by default, so gint's linker
                 script copies them to the on-chip IL memory (0xe5200000, 1-cycle fetch, never
                 evicted). Define CGD_SH4_NO_ILRAM to keep them in .text (A/B tests, or when
                 IL memory is needed for something else). The SH simulator harness (src/sim/sim.ld)
                 maps ".ilram" into its plain RAM, so sim builds work either way.
   CGD_SH4_COLD  rare drawers (fuzz, translated) and the presenter routines: always .text.
                 Define CGD_SH4_EXPAND_ILRAM to put the palette expansion into .ilram as well. */
#ifndef CGD_SH4_ASM_H
#define CGD_SH4_ASM_H

#ifdef CGD_SH4_NO_ILRAM
# define CGD_SH4_HOT .text
#else
# define CGD_SH4_HOT .section .ilram, "ax"
#endif

#define CGD_SH4_COLD .text

#if defined(CGD_SH4_EXPAND_ILRAM) && !defined(CGD_SH4_NO_ILRAM)
# define CGD_SH4_EXPAND .section .ilram, "ax"
#else
# define CGD_SH4_EXPAND .text
#endif

/* Pad with nops (0x0009) up to a 4-byte boundary: loop heads must be aligned so that the
   instruction pairs written on consecutive lines are fetched/issued together. */
#define ALIGN4 .balignw 4, 0x0009

/* DSP repeat-loop set-up, encoded as raw opcodes so that these files assemble with or without
   -Wa,--dsp (gas rejects the mnemonics unless the DSP ISA is enabled). The game builds (CMake and
   src/sim/cgdoom.mk) pass -Wa,--dsp -DCGD_SH4_DSP_MNEMONICS and use the mnemonics. Encodings (binutils sh-opc.h,
   GDB sim gencode.c):
     ldrs @(disp,PC)  1000 1100 dddd dddd   RS = PC + 4 + 2*sext(disp)
     ldre @(disp,PC)  1000 1110 dddd dddd   RE = PC + 4 + 2*sext(disp)
     ldrc Rm          0100 mmmm 0011 0100
   LDRS(label)/LDRE(label) take the label of the first/last instruction of the loop body, exactly
   like the mnemonics; LDRC(n) takes a register NUMBER. test/Makefile assembles every file both
   ways (raw without --dsp, mnemonics with --dsp -DCGD_SH4_DSP_MNEMONICS) and checks that the
   section bytes and relocations are identical, which also catches an out-of-range displacement
   (gas range-checks the mnemonic form). */
#ifdef CGD_SH4_DSP_MNEMONICS
# define LDRS(l) ldrs l
# define LDRE(l) ldre l
# define LDRC(n) ldrc r##n
#else
# define LDRS(l) .word 0x8c00 | ((((l) - . - 4) >> 1) & 0xff)
# define LDRE(l) .word 0x8e00 | ((((l) - . - 4) >> 1) & 0xff)
# define LDRC(n) .word 0x4034 | (((n) & 15) << 8)
#endif

#define FUNC(name) \
	.global _##name; \
	.type _##name, @function; \
	_##name:

#define ENDFUNC(name) .size _##name, . - _##name

#endif
