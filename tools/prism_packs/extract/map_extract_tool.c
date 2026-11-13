/*
MAP_EXTRACT_TOOL.C

PRISM's pack tools' extractor (tools/prism_packs): one program of two
commands, built by tools/x86_game_abi.py --map-extract with the game's tag
validator and zlib, a 32-bit program with the tag cache at the game's
address.

	map_extract [--data <data root>] [--out <folder>] ... <map>
		a map's bitmaps, models and structure bsps (map_extract.c)
	map_extract lights [--data <data root>] --out <file.json> <map>
		its light sources, for the lightmaps' re-bake (map_extract_lights.c)

Everything either writes is made of the player's own map: it stays on the
player's computer.
*/

#include <string.h>

#define main map_extract_main
#include "map_extract.c"
#undef main

#define main map_extract_lights_main
#include "map_extract_lights.c"
#undef main

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "lights"))
		return map_extract_lights_main(argc - 1, argv + 1);
	return map_extract_main(argc, argv);
}
