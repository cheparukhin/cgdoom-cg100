/* CGDoom gint platform: the WAD name given to the engine.

   gnames_wad_base(): the WAD base name handed to the engine as
   CGD_WADFileName. The engine derives its 8.3 save prefix from the WHOLE
   base name (CGD_FilePrefix, src/engine/cgdoom-names.c); cutting it (say,
   to 6 characters) would make freedoom1/freedoom2 and plutonia/plutonia2
   share their saves.

   Pure logic (host-tested in tests/gint-host with the engine's
   cgdoom-names.c). */

#include "cgd_gint.h"
#include "cgdoom.h"

#include <string.h>

void gnames_wad_base(char *out, size_t size, char const *wadname)
{
	size_t i = 0;
	if(size == 0)
		return;
	/* Everything up to the first '.' (CGD_FilePrefix stops there too) */
	for(; wadname && wadname[i] && wadname[i] != '.' && i < size - 1; i++)
		out[i] = wadname[i];
	out[i] = 0;
}
