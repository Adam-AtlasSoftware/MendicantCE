/*
PRISM_WORLD_SOURCE.C

prism_world_source.h has what and why.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/collision_bsp_definitions.h"
#include "tag_files/tag_groups.h"

#include "prism_geom.h"
#include "prism_identity.h"
#include "prism_prsx.h"
#include "prism_ref_decode.h"
#include "prism_world_source.h"
#include "prism_deferred.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator: the sidecar's bytes are ours alone) */
#undef malloc
#undef free

/* the sidecar, a data root file (maps_enhanced/<map>.prsx), is opened by
port/linux/include/stdio.h's fopen, which reads a path as the Xbox's under
the data folder; the shipped bake's files by their host paths, as the
renderer that loads them (xgpu_vulkan_bake.c) and prism_bake_menu.c name
them, by the C library's own: display.bake_shipped_folder is a host path,
which that fopen would look for under the data folder (the packs never
found, the map's classic_world_id never known) */
static FILE *data_file_open(char const *path, char const *mode)
{
	return fopen(path, mode);
}
#undef fopen

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
int config_boolean(char const *name);
void platform_log(char const *format, ...);
const char *platform_data_root(void);
/* (port/linux/src/xbox_files.c's, and the cache's open map's slot:
cache_files_windows.c's) */
int cache_slot_source_base_id(short slot, unsigned char id[32]);
short cache_file_open_slot(void);

#define SIDECAR_FOLDER "maps_enhanced"
#define MAXIMUM_BSPS 32

static struct
{
	/* the map's choice made (choose), of a map with a scenario (not the
	menu's), World Geometry as it loaded, and the rest of it (lit_choose)
	made: at its first ask under Enhanced or Path Traced (under Path
	Traced alone for a map loaded under Classic, loaded_lit FALSE: Enhanced
	keeps that map's classic geometry and checksum-keyed bakes, as it always
	has) */
	boolean chosen;
	boolean real;
	boolean geometry_wanted;
	boolean loaded_lit;
	boolean lit_chosen;
	char map[64];
	/* what was drawn at the last ask (logged as it changes) */
	boolean drawn_known;
	enum prism_world_rung drawn_rung;
	/* the sidecar held for the map (opened and verified: sidecar_ok, for
	the rest of the map whatever the lighting and the packs), its reader
	and the BSPs' ids */
	boolean sidecar_ok;
	FILE *file;
	struct prsx prsx;
	boolean open;
	boolean refused;
	char path[512];
	short bsp_count;
	unsigned char world_ids[MAXIMUM_BSPS][PRISM_ID_SIZE];
	char labels[MAXIMUM_BSPS][96];
	/* the map's classic_world_id (the farm's hash of its bake inputs) where
	known, and its BSPs' classic world_ids (v4 geopatch M4) */
	boolean classic_known;
	unsigned char classic_world_id[PRISM_ID_SIZE];
	unsigned char classic_ids[MAXIMUM_BSPS][PRISM_ID_SIZE];
	char classic_from[64];
	/* every BSP's enhanced pack found by lit_choose and not refused by the
	renderer since (prism_world_source_bake_refused), and debug.world_unbaked
	as the map was chosen: without either, only Path Traced draws the
	enhanced geometry (it needs neither: never a bake's state) */
	boolean packs_ok;
	boolean unbaked;
	/* the BSPs whose enhanced geometry could not be made (out of memory, or
	refused by prism_world_geometry.c's checks:
	prism_world_source_geometry_failed): drawn classic, keyed and lit as the
	classic world, for the rest of the map */
	boolean geometry_failed[MAXIMUM_BSPS];
} world_source;

/* ---------- private code */

static char const *map_name(void)
{
	char const *map = cache_file_loaded_map_name(), *part;

	if (!map)
		return "";
	for (part = map; *part; part++)
	{
		if (*part == '\\' || *part == '/')
			map = part + 1;
	}
	return map;
}

static boolean lighting_classic(void)
{
	char const *lighting = config_string("display.lighting");

	return !lighting || !*lighting || !strcmp(lighting, "classic") || !strcmp(lighting, "ce") ||
		!strcmp(lighting, "ce_deferred");
}

static void sidecar_close(void)
{
	if (world_source.open)
		prsx_close(&world_source.prsx);
	if (world_source.file)
		fclose(world_source.file);
	world_source.open = FALSE;
	world_source.file = NULL;
}

/* the map's sidecar's path, <data root>/maps_enhanced/<map>.prsx; FALSE if
there is none (the content-addressed names of updates, <map>.<id8>.prsx,
come with the content job's local manifest, which names the active one: M7) */
static boolean sidecar_path(char const *map, char *path, unsigned long size)
{
	FILE *file;

	/* (relative: the game's files are opened from the data root, its
	working folder, as prism_world_geometry.c's always were) */
	snprintf(path, size, "%s/%s.prsx", SIDECAR_FOLDER, map);
	if ((file = data_file_open(path, "rb")) == NULL)
		return FALSE;
	fclose(file);
	return TRUE;
}

/* a BSP the sidecar has no geometry of (d40's space) is drawn classic */
static boolean bsp_has_geometry(short bsp)
{
	return world_source.open && prsx_find(&world_source.prsx, PRSX_GEOM, (uint16_t)bsp, 0) >= 0;
}

/* ... and so is one whose enhanced geometry could not be made */
static boolean bsp_geometry_drawn(short bsp)
{
	return bsp >= 0 && bsp < world_source.bsp_count && !world_source.geometry_failed[bsp] && bsp_has_geometry(bsp);
}

/* every BSP's enhanced pack: the shipped bake's (display.bake_shipped_folder's
remodel/<map>/) bsp<n>.pbk, named in its manifest.txt with the key
"geo:prsx:<the BSP's enhanced world_id>" */
static boolean packs_present(char const *map, char *why, unsigned long why_size)
{
	char const *shipped = config_string("display.bake_shipped_folder");
	char folder[512], path[600], manifest[600], line[1024];
	short bsp;
	FILE *file;

	if (shipped && *shipped)
		snprintf(folder, sizeof(folder), "%s/remodel/%s", shipped, map);
	else
		snprintf(folder, sizeof(folder), "%s/prism_bake_shipped/remodel/%s", platform_data_root(), map);
	snprintf(manifest, sizeof(manifest), "%s/manifest.txt", folder);
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		char key[96], hex[2 * PRISM_ID_SIZE + 1], prefix[96];
		boolean named = FALSE;

		/* (no enhanced geometry: no enhanced lightmaps to ask for; the farm ships it empty) */
		if (!bsp_has_geometry(bsp))
			continue;
		snprintf(path, sizeof(path), "%s/bsp%d.pbk", folder, (int)bsp);
		if ((file = fopen(path, "rb")) == NULL)
		{
			snprintf(why, why_size, "no enhanced lightmaps for BSP %d (%s)", (int)bsp, path);
			return FALSE;
		}
		fclose(file);
		prism_id_hex(world_source.world_ids[bsp], hex);
		snprintf(key, sizeof(key), "geo:prsx:%s", hex);
		snprintf(prefix, sizeof(prefix), "bsp remodel %s %d ", map, (int)bsp);
		if ((file = fopen(manifest, "r")) != NULL)
		{
			while (!named && fgets(line, sizeof(line), file))
				named = !strncmp(line, prefix, strlen(prefix)) && strstr(line, key) != NULL;
			fclose(file);
		}
		if (!named)
		{
			snprintf(why, why_size, "the enhanced lightmaps of BSP %d are not of this geometry (no %.25s... in %s)",
				(int)bsp, key, manifest);
			return FALSE;
		}
	}
	return TRUE;
}

/* the shipped folder (display.bake_shipped_folder, else the data root's
prism_bake_shipped) */
static void shipped_folder(char *folder, unsigned long size)
{
	char const *shipped = config_string("display.bake_shipped_folder");

	if (shipped && *shipped)
		snprintf(folder, size, "%s", shipped);
	else
		snprintf(folder, size, "%s/prism_bake_shipped", platform_data_root());
}

/* the map's classic_world_id (PLAN_FINAL.md section 2.5: computed on the
farm over the map version's bake inputs, tools/geopatch/classic_world.py),
looked up by the player's map's base_id: until M7's signed manifest, in the
shipped bake's manifests (<shipped>/<stock|remodel>/<map>/manifest.txt, a
line "world <map> <base_id> <classic_world_id>", tools/prism_bake_ship.py's);
else the farm's HALO_CLASSIC_WORLD_ID (the map it bakes). FALSE where
neither has it: a custom map, a map version no release supports */
static boolean classic_world_id_find(char const *map, unsigned char id[PRISM_ID_SIZE], char *from,
	unsigned long from_size)
{
	static char const *const worlds[2] = { "stock", "remodel" };
	unsigned char base[PRISM_ID_SIZE];
	char folder[512], path[700], line[512], base_hex[2 * PRISM_ID_SIZE + 1], prefix[160];
	char const *farm = getenv("HALO_CLASSIC_WORLD_ID");
	int world;

	if (cache_slot_source_base_id(cache_file_open_slot(), base))
	{
		shipped_folder(folder, sizeof(folder));
		prism_id_hex(base, base_hex);
		snprintf(prefix, sizeof(prefix), "world %s %s ", map, base_hex);
		for (world = 0; world < 2; world++)
		{
			FILE *file;

			snprintf(path, sizeof(path), "%s/%s/%s/manifest.txt", folder, worlds[world], map);
			if ((file = fopen(path, "r")) == NULL)
				continue;
			while (fgets(line, sizeof(line), file))
			{
				char hex[2 * PRISM_ID_SIZE + 1];

				if (strncmp(line, prefix, strlen(prefix)))
					continue;
				memcpy(hex, line + strlen(prefix), 2 * PRISM_ID_SIZE);
				hex[2 * PRISM_ID_SIZE] = 0;
				if (prism_id_parse(hex, id))
				{
					fclose(file);
					snprintf(from, from_size, "the %s manifest", worlds[world]);
					return TRUE;
				}
			}
			fclose(file);
		}
	}
	if (farm && prism_id_parse(farm, id))
	{
		snprintf(from, from_size, "HALO_CLASSIC_WORLD_ID");
		return TRUE;
	}
	return FALSE;
}

/* the rest of the map's choice, made at its first ask under Enhanced or Path
Traced lighting (at the load), or, for a map loaded under Classic, at its
first ask under Path Traced (Enhanced keeps that map classic): its classic_world_id, its sidecar opened and verified where World
Geometry (as the map loaded) is Enhanced, and its enhanced packs looked for.
The sidecar is held for the rest of the map whatever the packs: Path Traced
draws the enhanced geometry from it with or without them, Enhanced only
with them (prism_world_rung, asked each frame) */
static void lit_choose(char const *map)
{
	char why[700] = "";
	struct prsx_source source;
	enum prsx_result result;
	short bsp;

	world_source.lit_chosen = TRUE;
	/* (the classic world's ids, which key its bakes, where the map's
	classic_world_id is known) */
	world_source.classic_known = classic_world_id_find(map, world_source.classic_world_id, world_source.classic_from,
		sizeof(world_source.classic_from));
	if (world_source.classic_known)
	{
		char hex[2 * PRISM_ID_SIZE + 1];

		for (bsp = 0; bsp < world_source.bsp_count; bsp++)
			prism_world_id_classic(world_source.classic_world_id, (uint16_t)bsp, world_source.classic_ids[bsp]);
		prism_id_hex(world_source.classic_world_id, hex);
		platform_log("prism world: %s: classic_world_id %.16s... (%s)", map, hex, world_source.classic_from);
	}
	else
		platform_log("prism world: %s: no classic_world_id for this map version: its bakes keyed by its checksum", map);
	if (!world_source.geometry_wanted)
	{
		platform_log("prism world: %s: classic geometry (World Geometry is Classic; rung 2)", map);
		return;
	}
	if (!sidecar_path(map, world_source.path, sizeof(world_source.path)))
	{
		platform_log("prism world: %s: classic geometry: no enhanced map (%s/%s/%s.prsx; rung 2)", map,
			platform_data_root(), SIDECAR_FOLDER, map);
		return;
	}
	world_source.file = data_file_open(world_source.path, "rb");
	if (!world_source.file || !prsx_source_file(&source, world_source.file))
	{
		platform_log("prism world: %s: classic geometry: %s cannot be read (rung 2)", map, world_source.path);
		sidecar_close();
		return;
	}
	result = prsx_open(&world_source.prsx, &source);
	if (result != PRSX_OK)
	{
		platform_log("prism world: %s: classic geometry: %s is refused: %s (rung 2)", map, world_source.path,
			prsx_result_string(result));
		sidecar_close();
		return;
	}
	world_source.open = TRUE;
	/* (a sidecar is of one classic_world_id: never another map version's) */
	if (world_source.classic_known &&
		memcmp(world_source.prsx.classic_world_id, world_source.classic_world_id, PRISM_ID_SIZE))
	{
		platform_log("prism world: %s: classic geometry: %s is of another map version (its classic_world_id is not "
			"this map's; rung 2)", map, world_source.path);
		sidecar_close();
		return;
	}
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		char hex[2 * PRISM_ID_SIZE + 1];
		char const *name = strrchr(world_source.path, '/');

		prism_world_id_enhanced(&world_source.prsx, (uint16_t)bsp, world_source.world_ids[bsp]);
		prism_id_hex(world_source.world_ids[bsp], hex);
		snprintf(world_source.labels[bsp], sizeof(world_source.labels[bsp]), "%.16s %s", hex, name ? name + 1 :
			world_source.path);
	}
	world_source.sidecar_ok = TRUE;
	world_source.packs_ok = packs_present(map, why, sizeof(why));
	world_source.unbaked = config_boolean("debug.world_unbaked") != 0;
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		platform_log("prism world: %s: BSP %d enhanced world_id %s", map, (int)bsp, world_source.labels[bsp]);
	}
	if (!world_source.loaded_lit)
	{
		platform_log("prism world: %s: enhanced geometry from %s: Path Traced draws it (rung 1), traced as it is; "
			"Enhanced lighting draws the classic geometry of a map loaded under Classic (rung 2)", map,
			world_source.path);
	}
	else if (world_source.packs_ok || world_source.unbaked)
	{
		platform_log("prism world: %s: enhanced geometry from %s (rung 1 under Enhanced and Path Traced%s)", map,
			world_source.path, world_source.unbaked ? "; debug.world_unbaked: its lightmaps not asked for" : "");
	}
	else
	{
		platform_log("prism world: %s: enhanced geometry from %s: Path Traced draws it (rung 1), traced as it is; "
			"Enhanced lighting draws the classic geometry (rung 2): %s", map, world_source.path, why);
	}
}

/* the map's choice: what holds for the whole map (its name, its BSPs,
World Geometry as it loaded), at its first ask; and the rest of it
(lit_choose) at its first ask under Enhanced or Path Traced, which a map
loaded under Classic lighting makes at the first change to Path Traced */
static void choose(void)
{
	char const *map = map_name();
	struct scenario *scenario;

	if (!world_source.chosen || strcmp(world_source.map, map))
	{
		sidecar_close();
		world_source.chosen = TRUE;
		world_source.real = FALSE;
		world_source.loaded_lit = FALSE;
		world_source.lit_chosen = FALSE;
		world_source.sidecar_ok = FALSE;
		world_source.refused = FALSE;
		world_source.packs_ok = FALSE;
		world_source.unbaked = FALSE;
		world_source.drawn_known = FALSE;
		snprintf(world_source.map, sizeof(world_source.map), "%s", map);
		memset(world_source.world_ids, 0, sizeof(world_source.world_ids));
		memset(world_source.labels, 0, sizeof(world_source.labels));
		memset(world_source.classic_ids, 0, sizeof(world_source.classic_ids));
		memset(world_source.geometry_failed, 0, sizeof(world_source.geometry_failed));
		world_source.classic_known = FALSE;
		scenario = global_scenario_try_and_get();
		world_source.bsp_count = scenario ? scenario->structure_bsp_references.count : 0;
		if (world_source.bsp_count > MAXIMUM_BSPS)
			world_source.bsp_count = MAXIMUM_BSPS;
		if (!*map || !strcmp(map, "ui"))
			return;
		/* (no scenario yet: no BSPs to check, so draw classic and choose again at the next call) */
		if (!scenario)
		{
			world_source.chosen = FALSE;
			return;
		}
		world_source.real = TRUE;
		/* (World Geometry applies at the next map's load: read here, for the whole map) */
		world_source.geometry_wanted = prism_world_geometry_wanted();
		if (prism_world_ladder(lighting_classic(), FALSE, FALSE, FALSE, FALSE, FALSE) == 3)
		{
			platform_log("prism world: %s: classic geometry, classic lighting (rung 3; the rest chosen at a change "
				"to Enhanced or Path Traced)", map);
			return;
		}
		world_source.loaded_lit = TRUE;
	}
	if (world_source.real && !world_source.lit_chosen && !lighting_classic() &&
		(world_source.loaded_lit || prism_deferred_pathtraced()))
		lit_choose(map);
}

/* ---------- the stock view prism_geom.c reads */

static int stock_material(void *context, uint32_t lightmap_index, uint32_t material_index, uint32_t *count,
	const unsigned char **vertices, const unsigned char **lightmap_vertices)
{
	struct structure_bsp *bsp = (struct structure_bsp *)context;
	struct structure_lightmap *lightmap;
	struct structure_material *material;
	long vertex_size, lightmap_size;
	unsigned long need;

	if ((long)lightmap_index >= bsp->lightmaps.count)
		return 0;
	lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, (long)lightmap_index, struct structure_lightmap);
	if ((long)material_index >= lightmap->materials.count)
		return 0;
	material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, (long)material_index, struct structure_material);
	vertex_size = rasterizer_geometry_get_vertex_size(material->vertices.type);
	if (!material->compressed_vertex_data.address || vertex_size != PRSX_COMPRESSED_VERTEX_SIZE ||
		material->vertices.count < 0 ||
		(unsigned long)material->compressed_vertex_data.size < (unsigned long)material->vertices.count * 32UL)
		return 0;
	*count = (uint32_t)material->vertices.count;
	*vertices = (const unsigned char *)material->compressed_vertex_data.address;
	*lightmap_vertices = NULL;
	lightmap_size = material->lightmap_vertices.count > 0 ?
		rasterizer_geometry_get_vertex_size(material->lightmap_vertices.type) : 0;
	need = (unsigned long)material->vertices.count * (32UL + 8UL);
	if (lightmap_size == PRSX_COMPRESSED_LIGHTMAP_VERTEX_SIZE &&
		material->lightmap_vertices.count == material->vertices.count &&
		(unsigned long)material->compressed_vertex_data.size >= need)
	{
		*lightmap_vertices = *vertices + (unsigned long)material->vertices.count * 32UL;
	}
	return 1;
}

static int stock_collision_vertex(void *context, uint32_t index, uint32_t bits[3])
{
	struct structure_bsp *bsp = (struct structure_bsp *)context;
	struct collision_bsp *collision;
	struct collision_vertex *vertex;

	if (bsp->collision_bsp.count < 1 || !bsp->collision_bsp.address)
		return 0;
	collision = TAG_BLOCK_GET_ELEMENT(&bsp->collision_bsp, 0, struct collision_bsp);
	if ((long)index >= collision->vertices.count || !collision->vertices.address)
		return 0;
	vertex = TAG_BLOCK_GET_ELEMENT(&collision->vertices, (long)index, struct collision_vertex);
	memcpy(bits, &vertex->point, 12);
	return 1;
}

/* ---------- public code */

boolean prism_world_geometry_wanted(void)
{
	char const *value = config_string("display.world_geometry");

	return value && !strcmp(value, "enhanced");
}

/* the ladder now: the map's state (World Geometry as it loaded, its sidecar
held and verified, its packs and debug.world_unbaked) and the lighting of
this frame, asked each time, so that a change of lighting in the middle of a
map draws at once what a load under it would */
enum prism_world_rung prism_world_rung(void)
{
	enum prism_world_rung rung;
	boolean geometry;

	choose();
	if (!world_source.chosen)
		return PRISM_WORLD_RUNG_NONE;
	if (!world_source.real)
		return PRISM_WORLD_RUNG_CLASSIC;
	/* (a map loaded under Classic keeps its classic geometry under Enhanced,
	as it always has; only Path Traced, whose geometry never depends on a
	bake, draws the enhanced geometry in it) */
	geometry = world_source.geometry_wanted && (world_source.loaded_lit || prism_deferred_pathtraced());
	switch (prism_world_ladder(lighting_classic(), geometry,
		world_source.sidecar_ok && world_source.open && !world_source.refused, world_source.packs_ok,
		world_source.unbaked, prism_deferred_pathtraced()))
	{
	case 1:
		rung = PRISM_WORLD_RUNG_ENHANCED;
		break;
	case 2:
		rung = PRISM_WORLD_RUNG_CLASSIC_LIT;
		break;
	default:
		rung = PRISM_WORLD_RUNG_CLASSIC;
		break;
	}
	if (!world_source.drawn_known || rung != world_source.drawn_rung)
	{
		platform_log("prism world: %s: drawn now: %s geometry, %s lighting (rung %d)", world_source.map,
			rung == PRISM_WORLD_RUNG_ENHANCED ? "enhanced" : "classic", lighting_classic() ? "Classic" :
			prism_deferred_pathtraced() ? "Path Traced" : "Enhanced", (int)rung);
		world_source.drawn_known = TRUE;
		world_source.drawn_rung = rung;
	}
	return rung;
}

boolean prism_world_enhanced(void)
{
	return prism_world_rung() == PRISM_WORLD_RUNG_ENHANCED;
}

boolean prism_world_source_image(struct structure_bsp *bsp, short bsp_index, char const *bsp_name,
	unsigned char **image, unsigned long *size)
{
	unsigned char *sections[2] = { NULL, NULL };
	size_t sizes[2] = { 0, 0 }, image_size = 0;
	struct prism_geom_stock stock;
	struct prism_geom_report report;
	enum prism_geom_result decoded;
	int kind;

	*image = NULL;
	*size = 0;
	if (!prism_world_enhanced() || !world_source.open || bsp_index < 0 || !bsp)
		return FALSE;
	/* (one that could not be made stays classic, as its key and its light
	are, when the lighting changes and the geometry is looked for again) */
	if (bsp_index < world_source.bsp_count && world_source.geometry_failed[bsp_index])
		return FALSE;
	for (kind = 0; kind < 2; kind++)
	{
		long index = prsx_find(&world_source.prsx, kind ? PRSX_LMUV : PRSX_GEOM, (uint16_t)bsp_index, 0);
		enum prsx_result read;

		if (index < 0)
		{
			if (!kind)
			{
				platform_log("prism world: %s BSP %d has no enhanced geometry in %s: drawn classic", world_source.map,
					(int)bsp_index, world_source.path);
				return FALSE;
			}
			platform_log("prism world: %s BSP %d has geometry but no lightmap vertices (LMUV) in %s: the sidecar is "
				"refused", world_source.map, (int)bsp_index, world_source.path);
			goto refused;
		}
		sizes[kind] = (size_t)world_source.prsx.entries[index].usize;
		sections[kind] = (unsigned char *)malloc(sizes[kind] ? sizes[kind] : 1);
		read = sections[kind] ? prsx_section_read(&world_source.prsx, (uint32_t)index, sections[kind], sizes[kind]) :
			PRSX_ERROR_MEMORY;
		if (read != PRSX_OK)
		{
			platform_log("prism world: %s BSP %d: %s section of %s: %s: the sidecar is refused", world_source.map,
				(int)bsp_index, kind ? "LMUV" : "GEOM", world_source.path, prsx_result_string(read));
			goto refused;
		}
	}
	stock.context = bsp;
	stock.material = stock_material;
	stock.collision_vertex = stock_collision_vertex;
	decoded = prism_geom_decode(sections[0], sizes[0], sections[1], sizes[1], &stock, bsp_name, image, &image_size,
		&report);
	free(sections[0]);
	free(sections[1]);
	sections[0] = sections[1] = NULL;
	if (decoded != PRISM_GEOM_OK)
	{
		platform_log("prism world: %s BSP %d: %s (part %lu, vertex %lu): the sidecar is refused", world_source.map,
			(int)bsp_index, prism_geom_result_string(decoded), (unsigned long)report.error_part,
			(unsigned long)report.error_vertex);
		goto refused;
	}
	platform_log("prism world: %s BSP %d: %lu parts (%lu not drawable yet), %lu vertices (%lu ours, %lu at stock "
		"positions), %lu triangles from %s", world_source.map, (int)bsp_index, (unsigned long)report.parts,
		(unsigned long)report.parts_skipped, (unsigned long)report.vertices, (unsigned long)report.by_op[0],
		(unsigned long)(report.vertices - report.by_op[0]), (unsigned long)report.triangles, world_source.path);
	*size = (unsigned long)image_size;
	return TRUE;

refused:
	free(sections[0]);
	free(sections[1]);
	/* (for the rest of the map; the next map load chooses again) */
	world_source.refused = TRUE;
	return FALSE;
}

void prism_world_source_world_id(short bsp_index, unsigned char id[32])
{
	memset(id, 0, 32);
	if (bsp_index < 0 || bsp_index >= world_source.bsp_count)
		return;
	if (prism_world_enhanced() && bsp_geometry_drawn(bsp_index))
		memcpy(id, world_source.world_ids[bsp_index], 32);
	/* (a BSP drawn classic is lit by its classic bake, enhanced geometry chosen or not) */
	else if (world_source.classic_known && world_source.lit_chosen && world_source.loaded_lit)
		memcpy(id, world_source.classic_ids[bsp_index], 32);
}

boolean prism_world_source_bsp_enhanced(short bsp_index)
{
	return prism_world_enhanced() && bsp_geometry_drawn(bsp_index);
}

void prism_world_source_geometry_failed(short bsp_index)
{
	if (bsp_index < 0 || bsp_index >= world_source.bsp_count || world_source.geometry_failed[bsp_index])
		return;
	platform_log("prism world: %s BSP %d: its enhanced geometry (%s) could not be made: drawn classic for the rest of "
		"the map, keyed and lit as the classic world (its classic pack where it has one)", world_source.map,
		(int)bsp_index, world_source.labels[bsp_index]);
	world_source.geometry_failed[bsp_index] = TRUE;
}

void prism_world_source_bake_refused(short bsp_index)
{
	if (!prism_world_enhanced() || !world_source.packs_ok || world_source.unbaked || prism_deferred_pathtraced() ||
		!bsp_geometry_drawn(bsp_index))
		return;
	platform_log("prism world: %s BSP %d: warning: the enhanced lightmaps of its geometry (%s) were not loaded (their "
		"pack is not this geometry's, or is damaged: the renderer's log says which): Enhanced lighting draws the "
		"classic geometry for the rest of the map (rung 2)", world_source.map, (int)bsp_index,
		bsp_index >= 0 && bsp_index < world_source.bsp_count ? world_source.labels[bsp_index] : "?");
	/* (for the rest of the map; the next map load chooses again. Path
	Traced, which needs no pack, still draws the enhanced geometry) */
	world_source.packs_ok = FALSE;
}

char const *prism_world_source_label(short bsp_index)
{
	if (prism_world_enhanced() && bsp_index >= 0 && bsp_index < world_source.bsp_count)
		return world_source.labels[bsp_index];
	return "";
}

void prism_world_source_map_unloaded(void)
{
	sidecar_close();
	world_source.chosen = FALSE;
	world_source.refused = FALSE;
	world_source.real = FALSE;
	world_source.loaded_lit = FALSE;
	world_source.lit_chosen = FALSE;
	world_source.sidecar_ok = FALSE;
	world_source.drawn_known = FALSE;
	world_source.map[0] = 0;
}
