/* CGDoom: brightmaps (CGD_FEAT_BRIGHTMAPS, the CGD_EXTRAS builds).

   A brightmap is 256 flags, one per palette index: the texels of a wall
   texture, flat or sprite whose flag is set (switch lights, computer
   screens, exit signs, lit panels) are drawn at full brightness, the
   others in the light of their sector. The tables are Crispy Doom's
   (r_bmaps.c), for DOOM and DOOM II. A lookup returns NULL for an image
   without a brightmap; such images are drawn by the usual drawers. */

#ifndef __R_BMAPS__
#define __R_BMAPS__

#include "doomtype.h"

#if CGD_EXTRAS
/* Builds the lookup tables for the loaded WAD (R_InitData, once the
   textures, flats and sprite lumps are known) */
void		R_InitBrightmaps (void);

/* By wall texture number (after texturetranslation) */
const byte*	R_TextureBrightmap (int texnum);
/* By flat number (after flattranslation) */
const byte*	R_FlatBrightmap (int flatnum);
/* By sprite lump, relative to firstspritelump (vissprite_t patch): the
   things' sprites */
const byte*	R_SpriteBrightmap (int spritelump);
/* By state number: the weapon sprites */
const byte*	R_StateBrightmap (int state);
#endif

#endif
