/*
MOD_MESHES.C

PRISM's replacement meshes (display.prism_models; docs/mods.md, "Meshes"):
.pmesh files a mod ships in its meshes folder, meshes\<tier>\<the model tag's
path>.pmesh, which the host reads straight into the GPU's memory and draws in
place of the model's parts (port/linux/vulkan/xgpu_mesh_store.h): 32-bit
indices, any number of vertices, four weights and up to 1024 bones each,
tangents and two texture coordinate sets, levels of detail of their own. No
vertex of them is in the game's memory: the game goes on loading and drawing
the stock model, whose nodes, markers, regions and collision stay what it
simulates with, and which a renderer without the store draws (Metal's: the
stream's mesh_store capability 0).

As a map loads, as the mods are found again (mods_reload, the Mods menu) and
as the settings change (read once a frame), each model tag is looked for in
the mods played on the map: the tier's file of the mod loaded last, and for
max, enhanced's where max has none. The files found go to the host in one
record (xgpu_backend_mesh_store_map) with what the host checks them against
(the model's nodes' count and checksum, the parts it can draw, its base map
scale); it answers each with an id (0: refused, the host's log says why) and
the radius the mesh reaches, which the object definitions that draw the
model take as their render-only culling radius where it is larger
(render_bounding_radius: physics, the AI and the network read the bounding
radius, which is left as it is).

Each frame, render_model names the model it draws (mod_meshes_model_begin):
a model with an id sends its nodes' matrices (MESH_INSTANCE), and each part
it draws names itself before its draws (MESH_DRAW): the host draws the
mesh's pieces of that region, permutation and shader at the first part of
the geometry with that shader (the carrier), nothing at the others, and the
stock part until the mesh is in. Stock sends nothing: the command stream is
as it was before the store.

debug.prism_mesh_dump (a model tag's name, or "*" for all) writes the map's
models as the game draws them into the data folder's prism_dump folder, the
input of tools/pmesh_from_dump.py and the art pipeline's exporter (local
only: they are the map's data).

Meshes are drawn, not simulated: nothing a network game's machines agree on
reads them.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "cache/cache_files.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "main/console.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "tag_files/tag_groups.h"
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* the platform layer's renderer's (port/linux/src/xgpu_backend.h, which
the game's units do not include: keep the two the same) */
struct xgpu_mesh_store_query
{
	char const *path;
	unsigned long file_size, stamp_seconds;
	unsigned long node_count, node_crc;
	unsigned char const *table;
	unsigned long table_count;
	float base_map_scale[2];
};
struct xgpu_mesh_store_answer
{
	unsigned long id;
	float render_radius;
	unsigned long bones, lods;
	int status;
};
int xgpu_backend_mesh_store_map(unsigned long generation, unsigned long tier, unsigned long budget_megabytes,
	int loading, struct xgpu_mesh_store_query const *queries, unsigned long count,
	struct xgpu_mesh_store_answer *answers);
int xgpu_backend_mesh_store_available(void);
void xgpu_backend_mesh_instance(unsigned long instance, unsigned long mesh, unsigned long object, float lod_pixels,
	unsigned long flags, float const (*matrices)[3][4], unsigned long node_count, unsigned long game_ticks,
	float game_fraction, float const *functions);
void xgpu_backend_mesh_draw(unsigned long instance, int region, int permutation, int shader, unsigned long flags,
	int geometry, int part);

/* the game's (port) and the platform layer's (port_config.c, xbox_files.c) */
char *tag_get_name(long tag_index);
char const *config_string(char const *name);
long config_integer(char const *name);
int config_boolean(char const *name);
unsigned long config_changes(void);
char const *platform_data_root(void);
/* (sys/stat.h's, which the game's strict ANSI hides; not translated, so
given the folder's whole path) */
int mkdir(char const *path, unsigned int mode);

/* ---------- constants */

enum
{
	_tier_stock,
	_tier_enhanced,
	_tier_max,
};

enum
{
	_model_geometry_part_stripped_bit = 0,
	_model_geometry_part_local_nodes_bit,
};

/* models.c's render_model flags */
enum
{
	_render_model_shadow_bit = 1,
	_render_model_first_person_bit = 3,
};

/* (xgpu_stream.h's) */
#define MESH_INSTANCE_SHADOW 1u
#define MESH_INSTANCE_FIRST_PERSON 2u
#define MESH_INSTANCE_NO_SECONDARY 4u
#define MESH_DRAW_CARRIER 1u
#define MAXIMUM_QUERIES 4096
/* a model's table's entries (regions, permutations and shaders its parts
draw with: retail models have a few) */
#define MODEL_TABLE_ENTRIES 256
#define MAXIMUM_PATH 1024

/* a part's flags in the model's table */
#define PART_CARRIER 1u
#define PART_DRAWN 2u

/* ---------- definitions */

/* this build's model geometry and part, as models.c and rasterizer.c each
define them for themselves (no header declares them) */
struct model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

typedef char verify_model_geometry_part_size[sizeof(struct model_geometry_part) == 0x68 ? 1 : -1];

struct model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

/* a model tag's mesh: its id (0 none), its geometries' first part in parts
and their parts' PART_* flags */
struct mesh_model
{
	long tag_index;
	unsigned long id;
	real radius;
	long geometry_count;
	short *part_first;
	byte *parts;
};

/* ---------- globals */

static struct
{
	/* the settings as read (config_changes + 1), and as last sent */
	unsigned long settings_read;
	int tier, sent_tier;
	long budget, sent_budget;
	/* display.prism_secondary_motion */
	boolean secondary_motion;
	char dump[256];
	/* a store is open on the host (a record with files sent) */
	boolean open;
	boolean map_loaded;
	unsigned long generation;
	/* the models' meshes, by their tags' absolute indices */
	struct mesh_model *models;
	long model_count;
	long accepted;
	/* the model being drawn (NULL none) and its instance, and a part
	named (a MESH_DRAW of it sent) */
	struct mesh_model const *drawing;
	unsigned long instance;
	unsigned long instances;
	boolean part_named;
} mod_meshes_globals;

/* ---------- private code */

/* (the game's malloc and free, cseries.h's: free takes no NULL, and there
is no calloc of theirs) */
static void *memory_zeroed(
	size_t size)
{
	void *memory = size ? malloc(size) : NULL;

	if (memory)
	{
		memset(memory, 0, size);
	}

	return memory;
}

static void memory_release(
	void *memory)
{
	if (memory)
	{
		free(memory);
	}

	return;
}

static void settings_read(
	void)
{
	char const *tier;
	char const *dump;

	if (mod_meshes_globals.settings_read == config_changes() + 1)
	{
		return;
	}
	mod_meshes_globals.settings_read = config_changes() + 1;
	tier = config_string("display.prism_models");
	mod_meshes_globals.tier = tier && !strcmp(tier, "max") ? _tier_max :
		tier && !strcmp(tier, "enhanced") ? _tier_enhanced : _tier_stock;
	mod_meshes_globals.budget = config_integer("display.prism_model_budget_mb");
	if (mod_meshes_globals.budget < 0)
	{
		mod_meshes_globals.budget = 0;
	}
	mod_meshes_globals.secondary_motion = config_boolean("display.prism_secondary_motion") != 0;
	dump = config_string("debug.prism_mesh_dump");
	strncpy(mod_meshes_globals.dump, dump ? dump : "", sizeof(mod_meshes_globals.dump) - 1);
	mod_meshes_globals.dump[sizeof(mod_meshes_globals.dump) - 1] = 0;

	return;
}

/* the standard CRC-32 (zlib's), continued from crc */
static unsigned long crc32_continue(
	unsigned long crc,
	void const *data,
	long size)
{
	byte const *bytes = data;
	long index;

	crc = ~crc & 0xffffffffUL;
	for (index = 0; index < size; index++)
	{
		int bit;

		crc ^= bytes[index];
		for (bit = 0; bit < 8; bit++)
		{
			crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
		}
	}

	return ~crc & 0xffffffffUL;
}

/* the model's nodes' checksum the .pmesh skeleton carries: each node's name
(its 32 bytes, zeros after its end), its parent (int16), its default
translation (3 floats) and rotation (4 floats), little-endian */
static unsigned long model_node_crc(
	struct model const *model)
{
	unsigned long crc = 0;
	long node_index;

	for (node_index = 0; node_index < model->nodes.count && node_index < MAXIMUM_NODES_PER_MODEL; node_index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);
		char name[TAG_STRING_LENGTH + 1];
		short parent = node->parent_node_index;
		size_t length = 0;

		while (length < TAG_STRING_LENGTH && node->name[length])
		{
			length++;
		}
		memset(name, 0, sizeof(name));
		memcpy(name, node->name, length);
		crc = crc32_continue(crc, name, sizeof(name));
		crc = crc32_continue(crc, &parent, sizeof(parent));
		crc = crc32_continue(crc, &node->default_translation, sizeof(node->default_translation));
		crc = crc32_continue(crc, &node->default_rotation, sizeof(node->default_rotation));
	}

	return crc;
}

/* a geometry the model has, else NULL */
static struct model_geometry const *model_geometry_get(
	struct model const *model,
	long geometry_index)
{
	if (geometry_index < 0 || geometry_index >= model->geometries.count || !model->geometries.address ||
		model->geometries.count > MAXIMUM_GEOMETRIES_PER_MODEL)
	{
		return NULL;
	}

	return TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct model_geometry);
}

/* a geometry's parts, if it has a count the engine draws (else 0) */
static long geometry_part_count(
	struct model_geometry const *geometry)
{
	if (!geometry || geometry->parts.count <= 0 || geometry->parts.count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY ||
		!geometry->parts.address)
	{
		return 0;
	}

	return geometry->parts.count;
}

/* the model's table: each region, permutation and shader one of its parts
draws with (render_model_parts' loop over every level), unique; the count */
static long model_table(
	struct model const *model,
	unsigned char *table,
	long maximum)
{
	long count = 0;
	long region_index;

	for (region_index = 0; region_index < model->regions.count && region_index < MAXIMUM_REGIONS_PER_MODEL; region_index++)
	{
		struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
		long permutation_index;

		for (permutation_index = 0;
			permutation_index < region->permutations.count && permutation_index < MAXIMUM_PERMUTATIONS_PER_MODEL_REGION;
			permutation_index++)
		{
			struct model_region_permutation const *permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations,
				permutation_index, struct model_region_permutation);
			long level;

			for (level = 0; level < 5; level++)
			{
				struct model_geometry const *geometry = model_geometry_get(model, permutation->geometry_indices[level]);
				long part_index, parts = geometry_part_count(geometry);

				for (part_index = 0; part_index < parts; part_index++)
				{
					struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
						struct model_geometry_part);
					long entry;

					if (part->shader_index < 0 || part->shader_index >= model->shaders.count ||
						part->shader_index >= MAXIMUM_SHADERS_PER_MODEL)
					{
						continue;
					}
					for (entry = 0; entry < count; entry++)
					{
						if (table[entry * 4] == region_index && table[entry * 4 + 1] == permutation_index &&
							table[entry * 4 + 2] == part->shader_index)
						{
							break;
						}
					}
					if (entry == count && count < maximum)
					{
						table[count * 4] = (unsigned char)region_index;
						table[count * 4 + 1] = (unsigned char)permutation_index;
						table[count * 4 + 2] = (unsigned char)part->shader_index;
						table[count * 4 + 3] = 0;
						count++;
					}
				}
			}
		}
	}

	return count;
}

/* each geometry's parts' flags: drawn (not stripped, a shader the model
has), and the carrier of its shader (the first drawn part with it) */
static boolean model_parts_mark(
	struct model const *model,
	struct mesh_model *mesh)
{
	long geometry_index, total = 0;

	mesh->geometry_count = MIN(model->geometries.count, MAXIMUM_GEOMETRIES_PER_MODEL);
	if (mesh->geometry_count <= 0)
	{
		return FALSE;
	}
	mesh->part_first = memory_zeroed((size_t)(mesh->geometry_count + 1) * sizeof(*mesh->part_first));
	if (!mesh->part_first)
	{
		return FALSE;
	}
	for (geometry_index = 0; geometry_index < mesh->geometry_count; geometry_index++)
	{
		mesh->part_first[geometry_index] = (short)total;
		total += geometry_part_count(model_geometry_get(model, geometry_index));
	}
	mesh->part_first[mesh->geometry_count] = (short)total;
	mesh->parts = memory_zeroed((size_t)MAX(total, 1));
	if (!mesh->parts)
	{
		return FALSE;
	}
	for (geometry_index = 0; geometry_index < mesh->geometry_count; geometry_index++)
	{
		struct model_geometry const *geometry = model_geometry_get(model, geometry_index);
		unsigned long carried = 0;
		long part_index, parts = geometry_part_count(geometry);

		for (part_index = 0; part_index < parts; part_index++)
		{
			struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
				struct model_geometry_part);
			byte *flags = &mesh->parts[mesh->part_first[geometry_index] + part_index];

			if (TEST_FLAG(part->flags, _model_geometry_part_stripped_bit) || part->shader_index < 0 ||
				part->shader_index >= model->shaders.count || part->shader_index >= MAXIMUM_SHADERS_PER_MODEL)
			{
				continue;
			}
			*flags = PART_DRAWN;
			if (!(carried & (1UL << part->shader_index)))
			{
				carried |= 1UL << part->shader_index;
				*flags |= PART_CARRIER;
			}
		}
	}

	return TRUE;
}

static void models_clear(
	void)
{
	long index;

	mod_meshes_globals.drawing = NULL;
	mod_meshes_globals.part_named = FALSE;
	for (index = 0; index < mod_meshes_globals.model_count; index++)
	{
		memory_release(mod_meshes_globals.models[index].part_first);
		memory_release(mod_meshes_globals.models[index].parts);
	}
	memory_release(mod_meshes_globals.models);
	mod_meshes_globals.models = NULL;
	mod_meshes_globals.model_count = 0;
	mod_meshes_globals.accepted = 0;

	return;
}

/* the file of a model's mesh in the tier (max: else enhanced's) */
static long file_find(
	char const *tag_name,
	int tier)
{
	static char const *names[] = { "stock", "enhanced", "max" };
	char key[MAXIMUM_PATH];
	long file;

	for (; tier >= _tier_enhanced; tier--)
	{
		if (snprintf(key, sizeof(key), "meshes\\%s\\%s.pmesh", names[tier], tag_name) >= (int)sizeof(key))
		{
			return -1;
		}
		file = port_mods_file_find(key, mods_map_name());
		if (file >= 0)
		{
			return file;
		}
	}

	return -1;
}

/* the store closed on the host (its buffers let go of) */
static void store_close(
	void)
{
	if (mod_meshes_globals.open)
	{
		xgpu_backend_mesh_store_map(++mod_meshes_globals.generation, 0, 0, FALSE, NULL, 0, NULL);
	}
	mod_meshes_globals.open = FALSE;

	return;
}

/* the object definitions that draw a model take the mesh's radius as their
render-only culling radius where it is larger (objects.h's
object_get_render_bounding_sphere; never smaller, never the bounding radius
physics and the game state read) */
static long render_radii_grow(
	long model_tag_index,
	real radius)
{
	struct tag_iterator iterator;
	long object_index, grown = 0;

	tag_iterator_new(&iterator, OBJECT_DEFINITION_TAG);
	while ((object_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct object_definition *definition = object_definition_get(object_index);

		if (definition && definition->object.model.index == model_tag_index &&
			radius > definition->object.render_bounding_radius)
		{
			definition->object.render_bounding_radius = radius;
			grown++;
		}
	}

	return grown;
}

/* the map's meshes found and sent; the ids kept */
static void meshes_send(
	boolean at_load)
{
	struct xgpu_mesh_store_query *queries = NULL;
	struct xgpu_mesh_store_answer *answers = NULL;
	unsigned char *tables = NULL;
	long *query_tag = NULL;
	struct tag_iterator iterator;
	long tag_index, count = 0, tag_count = 0, model_total = 0, index;
	long refused = 0, grown = 0;

	models_clear();
	mod_meshes_globals.sent_tier = mod_meshes_globals.tier;
	mod_meshes_globals.sent_budget = mod_meshes_globals.budget;
	if (mod_meshes_globals.tier == _tier_stock || !xgpu_backend_mesh_store_available())
	{
		store_close();
		return;
	}
	tag_iterator_new(&iterator, MODELS_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		tag_count = MAX(tag_count, (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) + 1);
		model_total++;
	}
	if (!model_total)
	{
		store_close();
		return;
	}
	queries = memory_zeroed((size_t)MIN(model_total, MAXIMUM_QUERIES) * sizeof(*queries));
	query_tag = memory_zeroed((size_t)MIN(model_total, MAXIMUM_QUERIES) * sizeof(*query_tag));
	tables = memory_zeroed((size_t)MIN(model_total, MAXIMUM_QUERIES) * 4 * MODEL_TABLE_ENTRIES);
	mod_meshes_globals.models = memory_zeroed((size_t)tag_count * sizeof(*mod_meshes_globals.models));
	if (!queries || !query_tag || !tables || !mod_meshes_globals.models)
	{
		memory_release(queries);
		memory_release(query_tag);
		memory_release(tables);
		memory_release(mod_meshes_globals.models);
		mod_meshes_globals.models = NULL;
		store_close();
		return;
	}
	mod_meshes_globals.model_count = tag_count;
	tag_iterator_new(&iterator, MODELS_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE && count < MAXIMUM_QUERIES)
	{
		struct model const *model = model_definition_get(tag_index);
		char const *tag_name = tag_get_name(tag_index);
		unsigned long size, stamp[2];
		long file;

		if (!model || !tag_name)
		{
			continue;
		}
		file = file_find(tag_name, mod_meshes_globals.tier);
		if (file < 0 || !port_mods_file_stamp(file, &size, stamp))
		{
			continue;
		}
		queries[count].path = port_mods_file_path(file);
		queries[count].file_size = size;
		queries[count].stamp_seconds = stamp[0];
		queries[count].node_count = (unsigned long)MIN(model->nodes.count, MAXIMUM_NODES_PER_MODEL);
		queries[count].node_crc = model_node_crc(model);
		queries[count].table = tables + count * 4 * MODEL_TABLE_ENTRIES;
		queries[count].table_count = (unsigned long)model_table(model, tables + count * 4 * MODEL_TABLE_ENTRIES,
			MODEL_TABLE_ENTRIES);
		queries[count].base_map_scale[0] = model->base_map_scale.i;
		queries[count].base_map_scale[1] = model->base_map_scale.j;
		query_tag[count] = tag_index;
		count++;
	}
	if (!count)
	{
		store_close();
		error(_error_silent, "meshstore: %s: no replacements of the map's %ld models",
			mod_meshes_globals.tier == _tier_max ? "max" : "enhanced", model_total);
		memory_release(queries);
		memory_release(query_tag);
		memory_release(tables);
		return;
	}
	answers = memory_zeroed((size_t)count * sizeof(*answers));
	if (!answers || !xgpu_backend_mesh_store_map(++mod_meshes_globals.generation,
		(unsigned long)mod_meshes_globals.tier, (unsigned long)mod_meshes_globals.budget, at_load, queries,
		(unsigned long)count, answers))
	{
		memory_release(answers);
		memory_release(queries);
		memory_release(query_tag);
		memory_release(tables);
		models_clear();
		return;
	}
	mod_meshes_globals.open = TRUE;
	for (index = 0; index < count; index++)
	{
		long absolute = (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(query_tag[index]);
		struct mesh_model *mesh = &mod_meshes_globals.models[absolute];

		if (!answers[index].id)
		{
			refused++;
			error(_error_silent, "meshstore: %s refused (status %d)", tag_get_name(query_tag[index]),
				answers[index].status);
			continue;
		}
		mesh->tag_index = query_tag[index];
		if (!model_parts_mark(model_definition_get(query_tag[index]), mesh))
		{
			memory_release(mesh->part_first);
			memory_release(mesh->parts);
			mesh->part_first = NULL;
			mesh->parts = NULL;
			continue;
		}
		mesh->id = answers[index].id;
		mesh->radius = answers[index].render_radius;
		grown += render_radii_grow(query_tag[index], mesh->radius);
		mod_meshes_globals.accepted++;
		error(_error_silent, "meshstore: %s accepted: id %08lx, %lu bones, %lu levels, radius %.3f",
			tag_get_name(query_tag[index]), answers[index].id, answers[index].bones, answers[index].lods,
			answers[index].render_radius);
	}
	error(_error_silent, "meshstore: %s: %ld files of the map's %ld models, %ld accepted, %ld refused; "
		"%ld render radii grown", mod_meshes_globals.tier == _tier_max ? "max" : "enhanced", count, model_total,
		mod_meshes_globals.accepted, refused, grown);
	if (mods_say())
	{
		console_printf(FALSE, "meshes: %ld replaced, %ld refused", mod_meshes_globals.accepted, refused);
	}
	memory_release(answers);
	memory_release(queries);
	memory_release(query_tag);
	memory_release(tables);

	return;
}

/* ---------- debug.prism_mesh_dump */

static void dump_name(
	FILE *file,
	char const *name)
{
	long index;

	fputc('"', file);
	for (index = 0; index < TAG_STRING_LENGTH && name[index]; index++)
	{
		char c = name[index];

		if (c == '"' || c == '\\')
		{
			fputc('\\', file);
		}
		fputc((unsigned char)c >= 32 && (unsigned char)c < 127 ? c : '?', file);
	}
	fputc('"', file);

	return;
}

/* a packed normal (11, 11 and 10 bits, signed) as three reals */
static void dump_unpack(
	unsigned long packed,
	real vector[3])
{
	long x = (long)(packed << 21) >> 21;
	long y = (long)(packed << 10) >> 21;
	long z = (long)packed >> 22;

	vector[0] = (real)x / 1023.f;
	vector[1] = (real)y / 1023.f;
	vector[2] = (real)z / 511.f;

	return;
}

/* one model, as the game draws it: <data>/prism_dump/<map>/<tag>.json and
.bin (each part's vertices as 17 floats: position, normal, binormal,
tangent, texture coordinates with the base map scale applied, the two nodes
and the first's weight; then its indices as they are, uint16) */
static boolean dump_model(
	long tag_index)
{
	struct model const *model = model_definition_get(tag_index);
	char const *tag_name = tag_get_name(tag_index);
	char folder[MAXIMUM_PATH], path[MAXIMUM_PATH], name[MAXIMUM_PATH];
	FILE *json, *bin;
	unsigned long offset = 0;
	long index, length;

	if (!model || !tag_name)
	{
		return FALSE;
	}
	snprintf(folder, sizeof(folder), "%s/prism_dump", platform_data_root());
	mkdir(folder, 0755);
	snprintf(folder, sizeof(folder), "%s/prism_dump/%s", platform_data_root(), mods_map_name());
	mkdir(folder, 0755);
	strncpy(name, tag_name, sizeof(name) - 1);
	name[sizeof(name) - 1] = 0;
	for (length = 0; name[length]; length++)
	{
		if (name[length] == '\\' || name[length] == '/' || name[length] == ' ')
		{
			name[length] = '_';
		}
	}
	snprintf(path, sizeof(path), "prism_dump/%s/%s.json", mods_map_name(), name);
	json = fopen(path, "wb");
	snprintf(path, sizeof(path), "prism_dump/%s/%s.bin", mods_map_name(), name);
	bin = fopen(path, "wb");
	if (!json || !bin)
	{
		if (json)
		{
			fclose(json);
		}
		if (bin)
		{
			fclose(bin);
		}
		return FALSE;
	}
	fprintf(json, "{\"tag\": ");
	dump_name(json, tag_name);
	fprintf(json, ", \"map\": \"%s\", \"node_list_checksum\": %lu, \"node_crc\": %lu, "
		"\"base_map_scale\": [%.9g, %.9g], \"detail_cutoff_pixels\": [%.9g, %.9g, %.9g, %.9g, %.9g],\n \"nodes\": [",
		mods_map_name(), model->node_list_checksum, model_node_crc(model), model->base_map_scale.i,
		model->base_map_scale.j, model->detail_cutoff_pixels[0], model->detail_cutoff_pixels[1],
		model->detail_cutoff_pixels[2], model->detail_cutoff_pixels[3], model->detail_cutoff_pixels[4]);
	for (index = 0; index < model->nodes.count && index < MAXIMUM_NODES_PER_MODEL; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 const *inverse = &node->runtime_default_inverse_matrix;

		fprintf(json, "%s\n  {\"name\": ", index ? "," : "");
		dump_name(json, node->name);
		fprintf(json, ", \"parent\": %d, \"translation\": [%.9g, %.9g, %.9g], \"rotation\": [%.9g, %.9g, %.9g, %.9g], "
			"\"inverse\": [%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g]}",
			node->parent_node_index, node->default_translation.x, node->default_translation.y,
			node->default_translation.z, node->default_rotation.v.i, node->default_rotation.v.j, node->default_rotation.v.k,
			node->default_rotation.w, inverse->scale, inverse->forward.i, inverse->forward.j, inverse->forward.k,
			inverse->left.i, inverse->left.j, inverse->left.k, inverse->up.i, inverse->up.j, inverse->up.k,
			inverse->position.x, inverse->position.y, inverse->position.z);
	}
	fprintf(json, "],\n \"regions\": [");
	for (index = 0; index < model->regions.count && index < MAXIMUM_REGIONS_PER_MODEL; index++)
	{
		struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, index, struct model_region);
		long permutation_index;

		fprintf(json, "%s\n  {\"name\": ", index ? "," : "");
		dump_name(json, region->name);
		fprintf(json, ", \"permutations\": [");
		for (permutation_index = 0;
			permutation_index < region->permutations.count && permutation_index < MAXIMUM_PERMUTATIONS_PER_MODEL_REGION;
			permutation_index++)
		{
			struct model_region_permutation const *permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations,
				permutation_index, struct model_region_permutation);

			fprintf(json, "%s{\"name\": ", permutation_index ? ", " : "");
			dump_name(json, permutation->name);
			fprintf(json, ", \"geometries\": [%d, %d, %d, %d, %d]}", permutation->geometry_indices[0],
				permutation->geometry_indices[1], permutation->geometry_indices[2], permutation->geometry_indices[3],
				permutation->geometry_indices[4]);
		}
		fprintf(json, "]}");
	}
	fprintf(json, "],\n \"shaders\": [");
	for (index = 0; index < model->shaders.count && index < MAXIMUM_SHADERS_PER_MODEL; index++)
	{
		struct model_shader_reference const *shader = TAG_BLOCK_GET_ELEMENT(&model->shaders, index,
			struct model_shader_reference);
		char const *shader_name = shader->shader.index != NONE ? tag_get_name(shader->shader.index) : NULL;

		fprintf(json, "%s{\"name\": ", index ? ", " : "");
		dump_name(json, shader_name ? shader_name : "");
		fprintf(json, ", \"permutation\": %d}", shader->permutation_index);
	}
	fprintf(json, "],\n \"markers\": [");
	for (index = 0; index < model->markers.count && index < MAXIMUM_MARKERS_PER_MODEL; index++)
	{
		struct model_marker const *marker = TAG_BLOCK_GET_ELEMENT(&model->markers, index, struct model_marker);
		long instance_index;

		fprintf(json, "%s\n  {\"name\": ", index ? "," : "");
		dump_name(json, marker->name);
		fprintf(json, ", \"instances\": [");
		for (instance_index = 0;
			instance_index < marker->instances.count && instance_index < MAXIMUM_INSTANCES_PER_MODEL_MARKER;
			instance_index++)
		{
			struct model_marker_instance const *instance = TAG_BLOCK_GET_ELEMENT(&marker->instances, instance_index,
				struct model_marker_instance);

			fprintf(json, "%s{\"region\": %d, \"permutation\": %d, \"node\": %d, \"translation\": [%.9g, %.9g, %.9g], "
				"\"rotation\": [%.9g, %.9g, %.9g, %.9g]}", instance_index ? ", " : "", instance->region_index,
				instance->permutation_index, instance->node_index, instance->translation.x, instance->translation.y,
				instance->translation.z, instance->rotation.v.i, instance->rotation.v.j, instance->rotation.v.k,
				instance->rotation.w);
		}
		fprintf(json, "]}");
	}
	fprintf(json, "],\n \"geometries\": [");
	for (index = 0; index < model->geometries.count && index < MAXIMUM_GEOMETRIES_PER_MODEL; index++)
	{
		struct model_geometry const *geometry = model_geometry_get(model, index);
		long part_index, parts = geometry_part_count(geometry);

		fprintf(json, "%s\n  {\"parts\": [", index ? "," : "");
		for (part_index = 0; part_index < parts; part_index++)
		{
			struct model_geometry_part const *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
				struct model_geometry_part);
			struct vertex_buffer const *vertices = &part->vertex_buffer;
			struct triangle_buffer const *triangles = &part->triangle_buffer;
			long vertex_count = 0, index_count = 0, vertex_index;
			unsigned long vertex_offset = offset, index_offset;
			byte *vertex_data = NULL;
			word *index_data = NULL;
			byte const *palette = NULL;
			short palette_count = 0;

			if (vertices->type == _rasterizer_vertex_type_model_compressed && vertices->hardware_format &&
				!vertices->offset && vertices->count > 0 && vertices->count <= MAXIMUM_VERTICES_PER_MODEL_GEOMETRY_PART)
			{
				IDirect3DVertexBuffer8_Lock(vertices->hardware_format, 0, 0, &vertex_data, D3DLOCK_READONLY);
			}
			if (triangles->hardware_format && triangles->count > 0)
			{
				IDirect3DIndexBuffer8_Lock(triangles->hardware_format, 0, 0, (byte **)&index_data, D3DLOCK_READONLY);
			}
			if (vertex_data && index_data)
			{
				extern short custom_edition_part_palette(struct vertex_buffer const *vertex_buffer, byte const **nodes);

				palette_count = custom_edition_part_palette(vertices, &palette);
				vertex_count = vertices->count;
				index_count = triangles->type == _triangle_buffer_type_triangles ? triangles->count * 3 :
					triangles->count + 2;
				for (vertex_index = 0; vertex_index < vertex_count; vertex_index++)
				{
					struct model_vertex_compressed const *from =
						(struct model_vertex_compressed const *)(vertex_data + vertex_index * 32);
					float out[17];
					long node_index;

					out[0] = from->position.x;
					out[1] = from->position.y;
					out[2] = from->position.z;
					dump_unpack(from->normal, out + 3);
					dump_unpack(from->binormal, out + 6);
					dump_unpack(from->tangent, out + 9);
					out[12] = (float)from->texcoord.x / 32767.f * model->base_map_scale.i;
					out[13] = (float)from->texcoord.y / 32767.f * model->base_map_scale.j;
					for (node_index = 0; node_index < 2; node_index++)
					{
						long node = from->nodes[node_index] / 3;

						if (palette && palette_count > 0)
						{
							node = palette[MIN(node, palette_count - 1)];
						}
						out[14 + node_index] = (float)node;
					}
					out[16] = (float)from->node_weight / 32767.f;
					fwrite(out, sizeof(out), 1, bin);
				}
				offset += vertex_count * 17 * sizeof(float);
				index_offset = offset;
				fwrite(index_data, sizeof(word), (size_t)index_count, bin);
				offset += index_count * sizeof(word);
				if (offset & 3)
				{
					static byte const zeros[4] = { 0 };

					fwrite(zeros, 1, 4 - (offset & 3), bin);
					offset += 4 - (offset & 3);
				}
			}
			else
			{
				index_offset = offset;
			}
			if (vertex_data)
			{
				IDirect3DVertexBuffer8_Unlock(vertices->hardware_format);
			}
			if (index_data)
			{
				IDirect3DIndexBuffer8_Unlock(triangles->hardware_format);
			}
			fprintf(json, "%s\n   {\"shader\": %d, \"flags\": %lu, \"strip\": %s, \"vertex_count\": %ld, "
				"\"vertex_offset\": %lu, \"index_count\": %ld, \"index_offset\": %lu, \"centroid\": [%.9g, %.9g, %.9g], "
				"\"centroid_nodes\": [%d, %d]}", part_index ? "," : "", part->shader_index, part->flags,
				triangles->type == _triangle_buffer_type_triangles ? "false" : "true", vertex_count, vertex_offset,
				index_count, index_offset, part->centroid.x, part->centroid.y, part->centroid.z,
				part->centroid_primary_node_index, part->centroid_secondary_node_index);
		}
		fprintf(json, "]}");
	}
	fprintf(json, "]}\n");
	fclose(json);
	fclose(bin);
	error(_error_silent, "meshstore: dumped %s to prism_dump/%s/%s.json (%lu bytes of vertices and indices)", tag_name,
		mods_map_name(), name, offset);

	return TRUE;
}

static void meshes_dump(
	void)
{
	struct tag_iterator iterator;
	long tag_index;

	if (!mod_meshes_globals.dump[0])
	{
		return;
	}
	tag_iterator_new(&iterator, MODELS_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		char const *tag_name = tag_get_name(tag_index);

		if (tag_name && (!strcmp(mod_meshes_globals.dump, "*") || !strcmp(mod_meshes_globals.dump, tag_name)))
		{
			dump_model(tag_index);
		}
	}

	return;
}

/* a node's matrix as the vertex shader's three constants (rasterizer_xbox.c's
node_matrix_constants) */
static void node_matrix_constants(
	float constants[3][4],
	real_matrix4x3 const *matrix)
{
	real scale = matrix->scale;

	constants[0][0] = scale * matrix->forward.i;
	constants[0][1] = scale * matrix->left.i;
	constants[0][2] = scale * matrix->up.i;
	constants[0][3] = matrix->position.x;
	constants[1][0] = scale * matrix->forward.j;
	constants[1][1] = scale * matrix->left.j;
	constants[1][2] = scale * matrix->up.j;
	constants[1][3] = matrix->position.y;
	constants[2][0] = scale * matrix->forward.k;
	constants[2][1] = scale * matrix->left.k;
	constants[2][2] = scale * matrix->up.k;
	constants[2][3] = matrix->position.z;

	return;
}

/* ---------- public code */

long mod_meshes_update(
	boolean remove_all,
	boolean at_load)
{
	settings_read();
	mod_meshes_globals.map_loaded = TRUE;
	if (remove_all)
	{
		models_clear();
		store_close();
		return 0;
	}
	if (at_load)
	{
		meshes_dump();
	}
	meshes_send(at_load);

	return mod_meshes_globals.accepted;
}

void mod_meshes_tags_unloaded(
	void)
{
	models_clear();
	store_close();
	mod_meshes_globals.map_loaded = FALSE;

	return;
}

/* (main_game_render, once a frame) the settings read again; the files sent
again if the tier or the budget changed */
void mod_meshes_frame(
	void)
{
	settings_read();
	if (!mod_meshes_globals.map_loaded || !xgpu_backend_mesh_store_available())
	{
		return;
	}
	if (mod_meshes_globals.tier != mod_meshes_globals.sent_tier ||
		mod_meshes_globals.budget != mod_meshes_globals.sent_budget)
	{
		meshes_send(FALSE);
	}

	return;
}

/* (render_model, before its parts) the model drawn: with a mesh, its nodes'
matrices sent as an instance the parts' draws name */
void mod_meshes_model_begin(
	long model_index,
	struct render_skinning const *skinning,
	real level_of_detail_pixels,
	unsigned long flags,
	long object_index)
{
	struct mesh_model const *mesh;
	float matrices[MAXIMUM_NODES_PER_MODEL][3][4];
	real const *functions;
	long absolute, node_index, node_count;

	mod_meshes_globals.drawing = NULL;
	if (!mod_meshes_globals.accepted || model_index == NONE || !skinning || !skinning->node_matrices)
	{
		return;
	}
	absolute = (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(model_index);
	if (absolute < 0 || absolute >= mod_meshes_globals.model_count)
	{
		return;
	}
	mesh = &mod_meshes_globals.models[absolute];
	if (!mesh->id || mesh->tag_index != model_index)
	{
		return;
	}
	node_count = MIN(skinning->node_matrix_count, MAXIMUM_NODES_PER_MODEL);
	for (node_index = 0; node_index < node_count; node_index++)
	{
		node_matrix_constants(matrices[node_index], &skinning->node_matrices[node_index]);
	}
	if (!++mod_meshes_globals.instances)
	{
		mod_meshes_globals.instances = 1;
	}
	mod_meshes_globals.instance = mod_meshes_globals.instances;
	/* (an object's function values drive helper bones: render-side, the
	interpolated values the renderer reads anyway) */
	functions = object_index != NONE ? render_interpolation_object_function_values(object_index) : NULL;
	xgpu_backend_mesh_instance(mod_meshes_globals.instance, mesh->id, (unsigned long)object_index,
		(float)level_of_detail_pixels,
		(TEST_FLAG(flags, _render_model_shadow_bit) ? MESH_INSTANCE_SHADOW : 0) |
		(TEST_FLAG(flags, _render_model_first_person_bit) ? MESH_INSTANCE_FIRST_PERSON : 0) |
		(mod_meshes_globals.secondary_motion ? 0 : MESH_INSTANCE_NO_SECONDARY),
		(float const (*)[3][4])matrices, (unsigned long)node_count, (unsigned long)game_time_get(),
		(float)render_interpolation_fraction(), (float const *)functions);
	mod_meshes_globals.drawing = mesh;

	return;
}

/* (render_model_parts, before an opaque, decal or shadow part's draws) the
part named for the draws that follow */
void mod_meshes_part(
	short region_index,
	short permutation_index,
	short geometry_index,
	short part_index,
	short shader_index)
{
	struct mesh_model const *mesh = mod_meshes_globals.drawing;
	byte flags;

	if (!mesh || geometry_index < 0 || geometry_index >= mesh->geometry_count || part_index < 0 ||
		mesh->part_first[geometry_index] + part_index >= mesh->part_first[geometry_index + 1])
	{
		return;
	}
	flags = mesh->parts[mesh->part_first[geometry_index] + part_index];
	if (!(flags & PART_DRAWN))
	{
		return;
	}
	xgpu_backend_mesh_draw(mod_meshes_globals.instance, region_index, permutation_index, shader_index,
		(flags & PART_CARRIER) ? MESH_DRAW_CARRIER : 0, geometry_index, part_index);
	mod_meshes_globals.part_named = TRUE;

	return;
}

/* (after the part's draws) no part named */
void mod_meshes_part_end(
	void)
{
	if (mod_meshes_globals.part_named)
	{
		xgpu_backend_mesh_draw(0, 0, 0, 0, 0, 0, 0);
		mod_meshes_globals.part_named = FALSE;
	}

	return;
}

/* (render_model, after its parts) */
void mod_meshes_model_end(
	void)
{
	mod_meshes_part_end();
	mod_meshes_globals.drawing = NULL;

	return;
}

/* (the console's prism_set "debug_pose <frame> <animation>") the first player's unit put in one of its
own graph's base animations at a frame, as unit_custom_animation_at_frame does, the graph the unit's own
(a script's tag references need not name it), and its nodes posed there at once (a stopped game shows the
pose without a tick, so without the animation's root motion): for checking a replacement mesh's
deformation in captures.
Only the console calls it. */
void mod_meshes_debug_pose(
	char const *arguments)
{
	long player_index = data_next_index(player_data, NONE);
	long frame = -1;
	char const *name = arguments;
	struct player_datum *player;
	struct unit_datum *unit;

	while (name && *name >= '0' && *name <= '9')
	{
		frame = (frame < 0 ? 0 : frame * 10) + (*name - '0');
		name++;
	}
	while (name && *name == ' ')
	{
		name++;
	}
	/* (it changes the unit's animation, which is the game's state: never in a network game, where every
	machine must keep the same) */
	if (game_connection() != _game_connection_local)
	{
		console_warning("debug_pose: only in a local game");
		return;
	}
	if (frame < 0 || !name || !*name || player_index == NONE)
	{
		console_warning("prism_set \"debug_pose <frame> <animation>\": a player and one of its graph's base animations");
		return;
	}
	player = player_get(player_index);
	if (player->unit_index == NONE)
	{
		console_warning("debug_pose: the player has no unit");
		return;
	}
	unit = unit_get(player->unit_index);
	if (!unit_custom_animation_at_frame(player->unit_index, unit->object.animation.animation_graph_index, name, FALSE,
		(short)frame))
	{
		console_warning("debug_pose: '%s' at frame %ld could not be started", name, frame);
		return;
	}
	/* (the nodes posed at that frame now, with no tick run: no root motion, the game may stay stopped) */
	object_compute_node_matrices_recursive(player->unit_index);
	console_printf(FALSE, "debug_pose: %s at %ld", name, frame);

	return;
}
