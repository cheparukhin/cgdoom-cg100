/* CGDoom build configuration: the screen format, chosen at compile time.

   Shared by the engine, the platforms and the SH4 assembly, so it holds only
   #defines. The switches come from -D (Makefile VARIANT=wide|hd; one add-in
   per build in CMakeLists.txt):

   - CGD_WIDESCREEN=1: the engine renders the panel's 396x224 instead of
     320x200 (the 3D view widens; 2D art keeps its 320x200 layout, centred).
   - CGD_HD=1: widescreen, with the features of CGD_EXTRAS (the "HD" build).

   The default (all 0) is the 320x200 build. Pixels are 8-bit palette
   indices in every build. */

#ifndef CGD_CONFIG_H
#define CGD_CONFIG_H

#ifndef CGD_HD
#define CGD_HD 0
#endif
#ifndef CGD_WIDESCREEN
#define CGD_WIDESCREEN CGD_HD
#endif

#if CGD_WIDESCREEN
#define SCREENWIDTH   396
#define SCREENHEIGHT  224
#else
#define SCREENWIDTH   320
#define SCREENHEIGHT  200
#endif

/* CGD_EXTRAS: the rendering and HUD features beyond vanilla (engine:
   cgdoom.h CGD_Features), compiled in the HD build */
#ifndef CGD_EXTRAS
#define CGD_EXTRAS CGD_HD
#endif

#endif /* CGD_CONFIG_H */
