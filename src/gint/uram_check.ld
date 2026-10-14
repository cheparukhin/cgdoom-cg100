/* CGDoom: link-time guard on gint's _uram arena.

   Passed to the linker as an implicit linker script (CMakeLists.txt); it
   only adds this assertion to gint's fxcg50.ld.

   gint's _uram arena is [_euram, 0x08100000 + 512 KiB - 16 KiB) (kernel.c,
   CG branch, MPM maps 8 user RAM pages). Before main() runs, kinit() takes
   the 177.5 KB VRAM, the two world buffers and the driver flags from the
   default arenas and fxlibc's constructors allocate too: about 182 KB. On
   the fx-CG100 there is no _ostk, and _uram is the first default arena
   (then mpm.bin's _ld1; no_osheap.c makes the OS heap fail instead of
   calling OS code). Keep at least 192 KiB of _uram, so that startup never
   depends on what the loader announces: if .data + .bss grow past this
   line, _uram alone could not hold these allocations. */
ASSERT(_euram <= 0x0814c000,
  "CGDoom: .data+.bss too large: gint's _uram keeps < 192 KiB for kinit (see src/gint/uram_check.ld)")
