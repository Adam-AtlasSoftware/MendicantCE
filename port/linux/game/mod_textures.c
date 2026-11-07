/*
MOD_TEXTURES.C

PRISM's replacement textures (display.prism_textures; docs/mods.md,
"Textures"): KTX2 files a mod ships in its textures folder,
textures\<tier>\<the bitmap tag's path>#<bitmap index>.ktx2, which the host
reads straight into the GPU's memory and draws in place of the bitmap
(port/linux/vulkan/xgpu_texture_store.h). No texel of them is in the game's
memory: the game goes on loading the stock bitmap (or a mod's .bitmap of
it), which stays what it draws until the replacement is in, what every size
it computes with comes from, and what a renderer without the store draws
(Metal's: the stream's texture_store capability 0).

As a map loads, as the mods are found again (mods_reload, the Mods menu)
and as the settings change (read once a frame), each bitmap that can take
a replacement (2D or cube, swizzled, not palettized) is looked for in the
mods played on the map: the tier's file of the mod loaded last, and for
max, enhanced's where max has none. The files found go to the host in one
record (xgpu_backend_texture_store_map), which answers each with an id (0:
refused, the host's log says why); the ids are kept by tag and bitmap, and
the encoder asks each draw's Xbox texture stages for theirs
(mod_textures_id: the texture cache's datum of the stage's header, as
render_prepared.c finds it). Stock sends nothing and installs no hook: the
command stream is as it was before the store.

Textures are drawn, not simulated: nothing a network game's machines agree
on reads them.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "main/console.h"
#include "tag_files/tag_groups.h"
#include "mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform layer's renderer's (port/linux/src/xgpu_backend.h, which
the game's units do not include: keep the two the same) */
struct xgpu_texture_store_query
{
	char const *path;
	unsigned long file_size, stamp_seconds;
	int cube;
	unsigned long stock_width, stock_height, stock_levels;
};
struct xgpu_texture_store_answer
{
	unsigned long id;
	unsigned long width, height, levels;
	int status, format;
};
int xgpu_backend_texture_store_map(unsigned long generation, unsigned long tier, unsigned long budget_megabytes,
	int loading, struct xgpu_texture_store_query const *queries, unsigned long count,
	struct xgpu_texture_store_answer *answers);
void xgpu_backend_texture_store_hook(unsigned long (*hook)(unsigned long resource));
int xgpu_backend_texture_store_available(void);

/* the game's (port) and the platform layer's (port_config.c) */
char *tag_get_name(long tag_index);
char const *config_string(char const *name);
long config_integer(char const *name);
unsigned long config_changes(void);
int config_boolean(char const *name);

/* ---------- constants */

enum
{
	_tier_stock,
	_tier_enhanced,
	_tier_max,
};

/* bitmaps.c's types, formats and flags (mod_bitmaps.c's) */
enum
{
	_bitmap_type_2d,
	_bitmap_type_3d,
	_bitmap_type_cube_map,
};
enum
{
	_bitmap_format_p8 = 17,
};
enum
{
	_bitmap_has_power_of_two_dimensions_bit,
	_bitmap_compressed_bit,
	_bitmap_palettized_bit,
	_bitmap_swizzled_bit,
	_bitmap_linear_bit,
};

/* the most queries a record has (xgpu_stream.h's) */
#define MAXIMUM_QUERIES 65536
#define MAXIMUM_PATH 1024

/* ---------- definitions */

/* a bitmap tag's ids, by its bitmaps (the tag's index to check it is the
same tag) */
struct texture_ids
{
	long tag_index;
	long count;
	unsigned long *ids;
};

/* ---------- globals */

static struct
{
	/* the settings as read (config_changes + 1), and as last sent */
	unsigned long settings_read;
	int tier, sent_tier;
	long budget, sent_budget;
	/* a store is open on the host (a record with files sent) */
	boolean open;
	boolean map_loaded;
	unsigned long generation;
	struct texture_ids *tags;
	long tag_count;
	long accepted;
} mod_textures_globals;

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

	if (mod_textures_globals.settings_read == config_changes() + 1)
	{
		return;
	}
	mod_textures_globals.settings_read = config_changes() + 1;
	tier = config_string("display.prism_textures");
	mod_textures_globals.tier = tier && !strcmp(tier, "max") ? _tier_max :
		tier && !strcmp(tier, "enhanced") ? _tier_enhanced : _tier_stock;
	mod_textures_globals.budget = config_integer("display.prism_texture_budget_mb");
	if (mod_textures_globals.budget < 0)
	{
		mod_textures_globals.budget = 0;
	}

	return;
}

static void ids_clear(
	void)
{
	long index;

	xgpu_backend_texture_store_hook(NULL);
	for (index = 0; index < mod_textures_globals.tag_count; index++)
	{
		memory_release(mod_textures_globals.tags[index].ids);
	}
	memory_release(mod_textures_globals.tags);
	mod_textures_globals.tags = NULL;
	mod_textures_globals.tag_count = 0;
	mod_textures_globals.accepted = 0;

	return;
}

/* the store id of the texture whose header is at resource (the encoder's
hook: each Xbox texture stage of a draw) */
static unsigned long mod_textures_id(
	unsigned long resource)
{
	unsigned long tag_index, bitmap_index, absolute;
	struct texture_ids const *ids;

	if (!texture_cache_port_identify((void const *)resource, &tag_index, &bitmap_index))
	{
		return 0;
	}
	absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index);
	if (absolute >= (unsigned long)mod_textures_globals.tag_count)
	{
		return 0;
	}
	ids = &mod_textures_globals.tags[absolute];
	if (!ids->ids || ids->tag_index != (long)tag_index || bitmap_index >= (unsigned long)ids->count)
	{
		return 0;
	}

	return ids->ids[bitmap_index];
}

/* a bitmap can take a replacement: 2D or a cube map, swizzled, its sides
powers of two, not palettized */
static boolean bitmap_replaceable(
	struct bitmap_data const *bitmap)
{
	return (bitmap->type == _bitmap_type_2d || bitmap->type == _bitmap_type_cube_map) &&
		bitmap->format != _bitmap_format_p8 && !TEST_FLAG(bitmap->flags, _bitmap_palettized_bit) &&
		TEST_FLAG(bitmap->flags, _bitmap_has_power_of_two_dimensions_bit) &&
		!TEST_FLAG(bitmap->flags, _bitmap_linear_bit) && bitmap->width > 0 && bitmap->height > 0;
}

/* the file of a bitmap's replacement in the tier (max: else enhanced's) */
static long file_find(
	char const *tag_name,
	long bitmap_index,
	int tier)
{
	static char const *names[] = { "stock", "enhanced", "max" };
	char key[MAXIMUM_PATH];
	long file;

	for (; tier >= _tier_enhanced; tier--)
	{
		if (snprintf(key, sizeof(key), "textures\\%s\\%s#%ld.ktx2", names[tier], tag_name, bitmap_index) >=
			(int)sizeof(key))
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

/* the store closed on the host (its images let go of) */
static void store_close(
	void)
{
	if (mod_textures_globals.open)
	{
		xgpu_backend_texture_store_map(++mod_textures_globals.generation, 0, 0, FALSE, NULL, 0, NULL);
	}
	mod_textures_globals.open = FALSE;

	return;
}

/* the map's replacements found and sent; the ids kept */
static void textures_send(
	boolean at_load)
{
	struct xgpu_texture_store_query *queries = NULL;
	struct xgpu_texture_store_answer *answers = NULL;
	long *query_tag = NULL;
	long *query_bitmap = NULL;
	struct tag_iterator iterator;
	long tag_index, count = 0, capacity = 0, tag_count = 0, replaceable = 0, index;
	long refused = 0;

	ids_clear();
	mod_textures_globals.sent_tier = mod_textures_globals.tier;
	mod_textures_globals.sent_budget = mod_textures_globals.budget;
	if (mod_textures_globals.tier == _tier_stock || !xgpu_backend_texture_store_available())
	{
		store_close();
		return;
	}
	tag_iterator_new(&iterator, BITMAP_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		struct bitmap_group const *group = bitmap_group_get(tag_index);
		char const *tag_name = tag_get_name(tag_index);
		short bitmap_index;

		tag_count = MAX(tag_count, (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) + 1);
		if (!group || !tag_name)
		{
			continue;
		}
		for (bitmap_index = 0; bitmap_index < group->bitmaps.count; bitmap_index++)
		{
			struct bitmap_data const *bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, bitmap_index, struct bitmap_data);
			long file;
			unsigned long size, stamp[2];

			if (!bitmap_replaceable(bitmap))
			{
				continue;
			}
			replaceable++;
			file = file_find(tag_name, bitmap_index, mod_textures_globals.tier);
			if (file < 0 || !port_mods_file_stamp(file, &size, stamp) || count >= MAXIMUM_QUERIES)
			{
				continue;
			}
			if (count == capacity)
			{
				long grown = capacity ? capacity * 2 : 64;
				struct xgpu_texture_store_query *more_queries = memory_zeroed((size_t)grown * sizeof(*queries));
				long *more_tags = memory_zeroed((size_t)grown * sizeof(*query_tag));
				long *more_bitmaps = memory_zeroed((size_t)grown * sizeof(*query_bitmap));

				if (!more_queries || !more_tags || !more_bitmaps)
				{
					memory_release(more_queries);
					memory_release(more_tags);
					memory_release(more_bitmaps);
					break;
				}
				if (count)
				{
					memcpy(more_queries, queries, (size_t)count * sizeof(*queries));
					memcpy(more_tags, query_tag, (size_t)count * sizeof(*query_tag));
					memcpy(more_bitmaps, query_bitmap, (size_t)count * sizeof(*query_bitmap));
				}
				memory_release(queries);
				memory_release(query_tag);
				memory_release(query_bitmap);
				queries = more_queries;
				query_tag = more_tags;
				query_bitmap = more_bitmaps;
				capacity = grown;
			}
			memset(&queries[count], 0, sizeof(queries[count]));
			queries[count].path = port_mods_file_path(file);
			queries[count].file_size = size;
			queries[count].stamp_seconds = stamp[0];
			queries[count].cube = bitmap->type == _bitmap_type_cube_map;
			queries[count].stock_width = (unsigned long)bitmap->width;
			queries[count].stock_height = (unsigned long)bitmap->height;
			queries[count].stock_levels = (unsigned long)bitmap->mipmap_count + 1;
			query_tag[count] = tag_index;
			query_bitmap[count] = bitmap_index;
			count++;
		}
	}
	if (!count)
	{
		store_close();
		error(_error_silent, "texstore: %s: no replacements of the map's %ld bitmaps that can take one",
			mod_textures_globals.tier == _tier_max ? "max" : "enhanced", replaceable);
		memory_release(queries);
		memory_release(query_tag);
		memory_release(query_bitmap);
		return;
	}
	answers = malloc((size_t)count * sizeof(*answers));
	mod_textures_globals.tags = tag_count > 0 ? memory_zeroed((size_t)tag_count * sizeof(*mod_textures_globals.tags)) : NULL;
	if (!answers || !mod_textures_globals.tags || !xgpu_backend_texture_store_map(++mod_textures_globals.generation,
		(unsigned long)mod_textures_globals.tier, (unsigned long)mod_textures_globals.budget, at_load, queries,
		(unsigned long)count, answers))
	{
		memory_release(answers);
		memory_release(queries);
		memory_release(query_tag);
		memory_release(query_bitmap);
		memory_release(mod_textures_globals.tags);
		mod_textures_globals.tags = NULL;
		return;
	}
	mod_textures_globals.open = TRUE;
	mod_textures_globals.tag_count = tag_count;
	for (index = 0; index < count; index++)
	{
		long absolute = (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(query_tag[index]);
		struct texture_ids *ids = &mod_textures_globals.tags[absolute];

		if (!answers[index].id)
		{
			refused++;
			continue;
		}
		if (!ids->ids)
		{
			ids->count = bitmap_group_get(query_tag[index])->bitmaps.count;
			ids->tag_index = query_tag[index];
			ids->ids = memory_zeroed((size_t)ids->count * sizeof(*ids->ids));
			if (!ids->ids)
			{
				continue;
			}
		}
		ids->ids[query_bitmap[index]] = answers[index].id;
		mod_textures_globals.accepted++;
	}
	if (mod_textures_globals.accepted)
	{
		xgpu_backend_texture_store_hook(mod_textures_id);
	}
	error(_error_silent, "texstore: %s: %ld files of the map's %ld bitmaps that can take one, %ld accepted, %ld refused",
		mod_textures_globals.tier == _tier_max ? "max" : "enhanced", count, replaceable, mod_textures_globals.accepted,
		refused);
	if (mods_say())
	{
		console_printf(FALSE, "textures: %ld replaced, %ld refused", mod_textures_globals.accepted, refused);
	}
	memory_release(answers);
	memory_release(queries);
	memory_release(query_tag);
	memory_release(query_bitmap);

	return;
}

/* ---------- public code */

long mod_textures_update(
	boolean remove_all,
	boolean at_load)
{
	settings_read();
	mod_textures_globals.map_loaded = TRUE;
	if (remove_all)
	{
		ids_clear();
		store_close();
		return 0;
	}
	textures_send(at_load);

	return mod_textures_globals.accepted;
}

void mod_textures_tags_unloaded(
	void)
{
	ids_clear();
	store_close();
	mod_textures_globals.map_loaded = FALSE;

	return;
}

/* (main_game_render, once a frame) the settings read again; the files sent
again if the tier or the budget changed */
void mod_textures_frame(
	void)
{
	settings_read();
	if (!mod_textures_globals.map_loaded || !xgpu_backend_texture_store_available())
	{
		return;
	}
	if (mod_textures_globals.tier != mod_textures_globals.sent_tier ||
		mod_textures_globals.budget != mod_textures_globals.sent_budget)
	{
		textures_send(FALSE);
	}

	return;
}
