/* CGDoom build configuration: the screen format, chosen at compile time.

   Shared by the engine, the platforms and the SH4 assembly, so it holds only
   #defines. Pass the switches with -D (Makefile VARIANT=..., CMake
   -DCGDOOM_VARIANT=...):

   - CGD_WIDESCREEN=1: the engine renders the panel's 396x224 instead of
     320x200 (the 3D view widens; 2D art keeps its 320x200 layout, centred).
   - CGD_TRUECOLOR=1: the engine renders RGB565 pixels instead of 8-bit
     palette indices, with its light tables in RGB565.
   - CGD_HD=1: both (the "HD" build).

   The default (all 0) is the 320x200 8-bit build. */

#ifndef CGD_CONFIG_H
#define CGD_CONFIG_H

#ifndef CGD_HD
#define CGD_HD 0
#endif
#ifndef CGD_WIDESCREEN
#define CGD_WIDESCREEN CGD_HD
#endif
#ifndef CGD_TRUECOLOR
#define CGD_TRUECOLOR CGD_HD
#endif

#if CGD_WIDESCREEN
#define SCREENWIDTH   396
#define SCREENHEIGHT  224
#else
#define SCREENWIDTH   320
#define SCREENHEIGHT  200
#endif

/* Bytes per pixel of the engine's screens */
#if CGD_TRUECOLOR
#define PIXELBYTES    2
#else
#define PIXELBYTES    1
#endif

#endif /* CGD_CONFIG_H */
