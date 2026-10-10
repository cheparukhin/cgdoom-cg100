/* Linker script for programs run by sh-elf-run (GDB SH simulator), big endian SH4.

   The simulator maps one flat RAM at address 0 (default 2^30 bytes = 1 GiB, lazily allocated
   by mmap; a numeric command-line option such as "sh-elf-run 27 prog.elf" selects 2^27).  There
   is no MMU, no caches, no P1/P2 aliasing: every address below the RAM size is plain memory and
   everything above raises a bus error.  We only use the first ___sim_ram_end bytes (default 64 MiB).

     0x00000000 .. 0x00000fff   unused (never mapped by the linker; catches nothing, just a gap)
     0x00001000 ..              .text (entry, code, GILRAM code), .rodata, .init_array,
                                .data (incl. GXRAM/GYRAM ".xyram"), .bss          (cache-line aligned)
     ___heap_start .. ___heap_end malloc() arena (libc-glue.c), everything between .bss and the stack
     ___stack_limit .. ___sim_stack_top   main stack, grows down, ___sim_stack_size bytes (default 1 MiB)
     ___sim_ram_end (0x04000000)   top of the RAM we use

   NOTE: this toolchain prefixes C symbols with '_' (sh-elf ELF), so the C symbol __heap_start is
   written ___heap_start below.

   Override from the link command line with e.g.
     -Wl,--defsym,___sim_ram_end=0x00800000 -Wl,--defsym,___sim_stack_size=0x20000
   to emulate a smaller machine (8 MiB total RAM, 128 KiB stack). */

OUTPUT_FORMAT("elf32-sh")
OUTPUT_ARCH(sh)
ENTRY(_start)

PROVIDE(___sim_ram_end    = 0x04000000);
PROVIDE(___sim_stack_size = 0x00100000);

SECTIONS
{
	. = 0x1000;

	.text : {
		*(.text.entry)
		*(.text .text.* .stub .gnu.linkonce.t.*)
		*(.pretext)
		*(.ilram .ilram.*)         /* gint GILRAM: plain RAM here */
		. = ALIGN(4);
	}

	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		. = ALIGN(4);
	}

	.init_array : {
		. = ALIGN(4);
		___init_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		KEEP(*(.ctors))
		___init_array_end = .;
	}

	. = ALIGN(32);
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.xyram .xyram.*)         /* gint GXRAM/GYRAM: plain RAM here */
		. = ALIGN(4);
	}

	. = ALIGN(32);
	.bss (NOLOAD) : {
		___bss_start = .;
		*(.bss .bss.* .gnu.linkonce.b.*)
		*(COMMON)
		. = ALIGN(32);
		___bss_end = .;
	}

	_end = .;                  /* C symbol: end */
	___heap_start = ALIGN(32);
	___heap_end   = ___sim_ram_end - ___sim_stack_size;
	___stack_limit = ___heap_end;
	___sim_stack_top = ___sim_ram_end;

	/DISCARD/ : {
		*(.eh_frame .eh_frame_hdr .gcc_except_table .comment .note.* .fini_array .dtors)
	}
}
