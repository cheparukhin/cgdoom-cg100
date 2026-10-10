/* CGDoom: link-time guard on gint's _uram arena (gint review round 1).

   Passed to the linker as an implicit linker script (CMakeLists.txt); it
   only adds this assertion to gint's fxcg50.ld.

   gint's _uram arena is [_euram, 0x08100000 + 512 KiB - 16 KiB) (kernel.c,
   CG branch, MPM maps 8 user RAM pages). Before main() runs, kinit() takes
   the 177.5 KB VRAM, the two world buffers and the driver flags from the
   default arenas and fxlibc's constructors allocate too: about 182 KB. On
   the fx-CG100 there is no _ostk and, under dynload MPM, no _ld1, so these
   come from _uram only (no_osheap.c makes the OS heap fail instead of
   calling OS code). Keep at least 192 KiB of _uram: if .data + .bss grow
   past this line, startup would run out of memory before main(). */
ASSERT(_euram <= 0x0814c000,
  "CGDoom: .data+.bss too large: gint's _uram keeps < 192 KiB for kinit (see src/gint/uram_check.ld)")
