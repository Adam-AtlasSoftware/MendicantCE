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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's allocator: the sidecar's bytes are ours alone) */
#undef malloc
#undef free

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
int config_boolean(char const *name);
void platform_log(char const *format, ...);
const char *platform_data_root(void);

#define SIDECAR_FOLDER "maps_enhanced"
#define MAXIMUM_BSPS 32

static struct
{
	boolean chosen;
	enum prism_world_rung rung;
	char map[64];
	/* the sidecar held for the map (rung 1), its reader and the BSPs' ids */
	FILE *file;
	struct prsx prsx;
	boolean open;
	boolean refused;
	char path[512];
	short bsp_count;
	unsigned char world_ids[MAXIMUM_BSPS][PRISM_ID_SIZE];
	char labels[MAXIMUM_BSPS][96];
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
	if ((file = fopen(path, "rb")) == NULL)
		return FALSE;
	fclose(file);
	return TRUE;
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
		snprintf(folder, sizeof(folder), "prism_bake_shipped/remodel/%s", map);
	snprintf(manifest, sizeof(manifest), "%s/manifest.txt", folder);
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		char key[96], hex[2 * PRISM_ID_SIZE + 1], prefix[96];
		boolean named = FALSE;

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

static void choose(void)
{
	char const *map = map_name();
	char why[700] = "";
	struct prsx_source source;
	enum prsx_result result;
	struct scenario *scenario;
	short bsp;

	if (world_source.chosen && !strcmp(world_source.map, map))
		return;
	sidecar_close();
	world_source.chosen = TRUE;
	world_source.refused = FALSE;
	snprintf(world_source.map, sizeof(world_source.map), "%s", map);
	memset(world_source.world_ids, 0, sizeof(world_source.world_ids));
	memset(world_source.labels, 0, sizeof(world_source.labels));
	scenario = global_scenario_get();
	world_source.bsp_count = scenario ? scenario->structure_bsp_references.count : 0;
	if (world_source.bsp_count > MAXIMUM_BSPS)
		world_source.bsp_count = MAXIMUM_BSPS;
	if (!*map || !strcmp(map, "ui"))
	{
		world_source.rung = PRISM_WORLD_RUNG_CLASSIC;
		return;
	}
	if (prism_world_ladder(lighting_classic(), FALSE, FALSE, FALSE, FALSE) == 3)
	{
		world_source.rung = PRISM_WORLD_RUNG_CLASSIC;
		platform_log("prism world: %s: classic geometry, classic lighting (rung 3)", map);
		return;
	}
	world_source.rung = PRISM_WORLD_RUNG_CLASSIC_LIT;
	if (!prism_world_geometry_wanted())
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
	world_source.file = fopen(world_source.path, "rb");
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
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		char hex[2 * PRISM_ID_SIZE + 1];
		char const *name = strrchr(world_source.path, '/');

		prism_world_id_enhanced(&world_source.prsx, (uint16_t)bsp, world_source.world_ids[bsp]);
		prism_id_hex(world_source.world_ids[bsp], hex);
		snprintf(world_source.labels[bsp], sizeof(world_source.labels[bsp]), "%.16s %s", hex, name ? name + 1 :
			world_source.path);
	}
	if (prism_world_ladder(FALSE, TRUE, TRUE, packs_present(map, why, sizeof(why)),
		config_boolean("debug.world_unbaked")) != 1)
	{
		platform_log("prism world: %s: classic geometry: %s (rung 2)", map, why);
		sidecar_close();
		return;
	}
	world_source.rung = PRISM_WORLD_RUNG_ENHANCED;
	for (bsp = 0; bsp < world_source.bsp_count; bsp++)
	{
		platform_log("prism world: %s: BSP %d enhanced world_id %s", map, (int)bsp, world_source.labels[bsp]);
	}
	platform_log("prism world: %s: enhanced geometry from %s (rung 1%s)", map, world_source.path,
		config_boolean("debug.world_unbaked") ? "; debug.world_unbaked: its lightmaps not asked for" : "");
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
	char const *value = config_string("display.prism_world_geometry");

	return value && (!strcmp(value, "remodel") || !strcmp(value, "enhanced"));
}

enum prism_world_rung prism_world_rung(void)
{
	choose();
	return world_source.rung;
}

boolean prism_world_enhanced(void)
{
	choose();
	return world_source.rung == PRISM_WORLD_RUNG_ENHANCED && !world_source.refused && !lighting_classic();
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
	if (prism_world_enhanced() && bsp_index >= 0 && bsp_index < world_source.bsp_count)
		memcpy(id, world_source.world_ids[bsp_index], 32);
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
	world_source.rung = PRISM_WORLD_RUNG_NONE;
	world_source.map[0] = 0;
}
