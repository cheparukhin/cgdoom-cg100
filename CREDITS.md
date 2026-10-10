# Credits

CGDoom for the fx-CG100 / Graph Math+ is free software under the GNU General Public License, version 2
(see `LICENSE`). Like every CGDoom before it, it is a derivative work of the original DOOM source code
and of nDoom.

## Lineage of the engine

| Step | Who | What |
|---|---|---|
| DOOM | id Software | The original game. Its source code was later released under the GNU GPL. |
| nDoom | Mrakoplaz | A port to the TI-Nspire, based on the MS-DOS game. |
| nDoom for the Nspire CX | Critor | The Nspire CX port, with support for every commercial game that runs on the original engine. |
| CGDoom | MPoupe | The first playable port to the Casio fx-CG10/20 (Prizm). |
| CGDoom for the fx-CG50 | ComputerNerd, then Lephenixnoir, with extensive feedback and guidance from Computer_Freak_2004 | ComputerNerd made the first attempts. Lephenixnoir wrote the fx-CG50 version this port starts from. |
| CGDoom for the fx-CG100 | this project | Port to gint and MPM. It adds the launcher, the fx-CG100 keymaps, vanilla-exact fixed-point math, the SH4 assembly drawers, the RAM/flash WAD access, low detail, view size, flat floors, auto frameskip, the diagnostics screens, and the host and simulator test harnesses. |

## Platform and tools

- **gint** (kernel, drivers, display, timers, DMA, USB) and the **fxSDK** (build system, `fxgxa`,
  `fxconv`, `fxlink`): Lephenixnoir and contributors, Planète Casio. This port uses gint's dev branch,
  which supports the fx-CG100.
- **fxlibc**, the C library: the Vhex-Kernel-Core project.
- **OpenLibm**, built as part of the toolchain.
- **MPM** (Math+ Mod: the OS patch and `mpm.bin`, the add-in loader): the MPM authors, including
  Lephenixnoir and CalcLoverHK, Planète Casio. Without MPM, add-ins cannot run on the fx-CG100.
- **GCC, binutils and the GDB SuperH simulator** (GNU). The simulator runs the SH4 build for bit-exact
  tests and instruction counts.

## Patterns reused from other projects

- **gpSP-sh4-jit, fx-CG100 port** (KaraRyougi). These techniques were proven on fx-CG100 hardware, and
  this port follows them:
  - the 10-row display strips through on-chip X/Y memory with ping-pong DMA, and the partial-window LCD
    set-up (`src/gint/video.c`);
  - building the flash block map with `Bfile_GetBlockAddress` in one world switch, and the rules for
    accepting P1/P2 pointers and rejecting P0 pointers (`src/gint/wad.c`);
  - the Ptune4 F5 overclock register set (`src/gint/clock.c`);
  - the bounded key-release wait (`src/gint/input.c`);
  - the fx-CG50 syscall trampoline (`src/gint/cg50_syscall.S`);
  - the RAM ranges that are safe to use under MPM (`src/gint/memory.c`).
- **Ptune4** (CalcLoverHK): the F5 clock preset, as applied by gpSP.
- **GBADoom:** the `COLEXTRABITS` trick in the SH4 column drawers (`src/engine/sh4/r_draw_sh4.S`). The
  texture coordinate is pre-shifted, so that a single shift gives the texel row.
- **Chocolate Doom:** the corrected low-detail semantics, used by the C drawers in `src/engine/r_draw.c`
  and by the SH4 drawers. Also `M_ChangeDetail`, and the `MAXSEGS` sizing of the clip-range array in
  `r_bsp.c`.

## Projects consulted

These projects were read as references while the port was designed. No code was taken from them unless
listed above:

- KhiCAS (Bernard Parisse), and its use of MPM and the faster bus;
- OutRun and Squish It (Slyvtt);
- NESizm, Prizoop, fxcraft and libsyscall (CalcLoverHK);
- PythonExtra;
- the Planète Casio, TI-Planet and Cemetech forum threads on the fx-CG100 and MPM.

## Test data

- **Freedoom** 0.13.0 (BSD license) is used for the host and simulator golden-frame tests. It is not
  distributed with this project, and it is too large for the calculator.
- No id Software data is included. DOOM is a trademark of id Software.
