/*
ZLIB_PREFIXED.H

The port's zlib (1.3.2, its inflate only: what the port inflates is other
people's data, the maps, the menus' and the HUD's PNGs, updates), beside
the game's own (source/memory/zlib, 1.1.3): its names begin z_, so the two
never meet. Include this, not zlib.h, so that the names always match.

Taken unchanged from the release (zlib.net, signed by Mark Adler): adler32.c,
crc32.c, crc32.h, gzguts.h, inffast.c, inffast.h, inffixed.h, inflate.c,
inflate.h, inftrees.c, inftrees.h, uncompr.c, zconf.h, zlib.h, zutil.c,
zutil.h, LICENSE and README. To update, copy those from a newer release.

deflate.c, deflate.h, trees.c and trees.h (the same release, unchanged) are
for the farm's sidecar tool only (tools/geopatch/native.py): one deflate,
built from these sources on every machine, so that a sidecar built twice is
the same bytes wherever it is built (a system's zlib differs: zlib-ng, or
another version, writes other, equally valid, streams). The game never
compiles them (tools/build_common.py's ZLIB_SOURCES).
*/

#ifndef ZLIB_PREFIXED_H
#define ZLIB_PREFIXED_H

#ifndef Z_PREFIX
#define Z_PREFIX
#endif
#include "zlib.h"

#endif
