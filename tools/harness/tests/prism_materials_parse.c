/*
PRISM_MATERIALS_PARSE.C (test)

material_parse (port/linux/game/prism_materials.c): a material file of
PRISM's format 2 read into what SCENE_MATERIAL_MAPS sends: its images'
files, its flags (the normal map's coordinates, the environment shader's
type and detail functions, CE's where the file says "ce"), its scalars (the
detail maps' scales as CE's texture pass computes them, rescaled by the
base map's size over each detail map's; the file's own where it gives
them), its model; files of another format, or with no base, refused.
test_prism_materials_parse.py takes the code, its constants and its
structures from the source; the mods' files, the bitmaps and the shader's
fields are the test's.
*/

#include "harness.h"

#include <math.h>
#include <stdarg.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define _error_silent 0

/* the bitmaps (bitmap_group.h's, as far as the code reads them) */
struct bitmap_data
{
	short width, height;
};
struct bitmap_group
{
	struct
	{
		long count;
		void *address;
	} bitmaps;
};
#define TAG_BLOCK_GET_ELEMENT(block, index, type) ((type *)(block)->address + (index))

#include "types.inc"

static void error(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

static boolean mods_finite_real(real value)
{
	return isfinite(value);
}

static const char *mods_map_name(void)
{
	return "test";
}

/* the mods' files: the material's images, a file index each */
static const char *const files[] =
{
	"textures\\prism_v4\\panel\\albedo.ktx2",
	"textures\\prism_v4\\panel\\normal.ktx2",
	"textures\\prism_v4\\panel\\orm.ktx2",
	"textures\\prism_v4\\details\\primary.ktx2",
	"textures\\prism_v4\\details\\secondary.ktx2",
	"textures\\prism_v4\\details\\micro.ktx2",
};

static long port_mods_file_find(const char *path, const char *map)
{
	long index;

	(void)map;
	for (index = 0; index < (long)(sizeof(files) / sizeof(files[0])); index++)
	{
		if (!strcmp(files[index], path))
			return index;
	}
	return NONE;
}

/* the bitmaps by their tag indices: the base map 512 x 256, the primary
detail map 128 x 128, the secondary 64 x 32, the micro 64 x 64 */
static struct bitmap_data bitmap_data[4] = { { 512, 256 }, { 128, 128 }, { 64, 32 }, { 64, 64 } };
static struct bitmap_group bitmap_groups[4];

static struct bitmap_group *bitmap_group_get(long index)
{
	return index >= 0 && index < 4 ? &bitmap_groups[index] : NULL;
}

#include "under_test.inc"

/* the environment shader's fields (rasterizer_xbox_environment.c's
offsets): its type, the diffuse flags (rescale detail maps), the base map,
the detail functions, scales and maps */
static unsigned char shader[0x400];

static void put_short(long offset, short value)
{
	memcpy(shader + offset, &value, sizeof(value));
}

static void put_real(long offset, float value)
{
	memcpy(shader + offset, &value, sizeof(value));
}

static void put_reference(long offset, long index)
{
	memcpy(shader + offset + 12, &index, sizeof(index));
}

static void shader_make(void)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		bitmap_groups[index].bitmaps.count = 1;
		bitmap_groups[index].bitmaps.address = &bitmap_data[index];
	}
	memset(shader, 0, sizeof(shader));
	put_short(SENV_TYPE, 1);
	put_short(SENV_DIFFUSE_FLAGS, DIFFUSE_RESCALE_DETAIL_MAPS);
	put_reference(SENV_BASE_MAP, 0);
	put_short(SENV_DETAIL_FUNCTION, 2);
	put_real(SENV_PRIMARY_DETAIL_SCALE, 4.0f);
	put_reference(SENV_PRIMARY_DETAIL_MAP, 1);
	put_real(SENV_SECONDARY_DETAIL_SCALE, 0.5f);
	put_reference(SENV_SECONDARY_DETAIL_MAP, 2);
	put_short(SENV_MICRO_DETAIL_FUNCTION, 1);
	put_real(SENV_MICRO_DETAIL_SCALE, 2.0f);
	put_reference(SENV_MICRO_DETAIL_MAP, 3);
}

/* text parsed as material_read parses it, then read */
static boolean parse(const char *text, boolean environment, struct material *material)
{
	static jsmntok_t tokens[MAXIMUM_TOKENS];
	struct json_document json;
	jsmn_parser parser;

	json.text = text;
	json.tokens = tokens;
	jsmn_init(&parser);
	json.count = jsmn_parse(&parser, text, strlen(text), tokens, MAXIMUM_TOKENS);
	CHECK(json.count > 0, "the test's JSON does not parse (%d)", json.count);
	memset(material, 0, sizeof(*material));
	return material_parse(&json, "test.pack.json", shader, environment, material);
}

static int near(float a, float b)
{
	return fabsf(a - b) < 1.0e-5f;
}

int main(int argc, char **argv)
{
	struct material material;
	const char *name = argc > 1 ? argv[1] : "";

	shader_make();
	if (!strcmp(name, "ce"))
	{
		/* (CE's type, functions and scales: the rescaled ones) */
		CHECK(parse("{\"format\": 2, \"family\": \"senv\", \"base\": {"
			"\"albedo\": \"textures/prism_v4/panel/albedo.ktx2\", \"normal\": \"textures/prism_v4/panel/normal.ktx2\","
			"\"normal_uv\": \"bump\", \"orm\": \"textures/prism_v4/panel/orm.ktx2\"},"
			"\"layers\": [{\"slot\": \"primary_detail\", \"albedo\": \"textures/prism_v4/details/primary.ktx2\","
			"\"scale\": \"ce\", \"function\": \"ce\"}, {\"slot\": \"micro_detail\","
			"\"albedo\": \"textures/prism_v4/details/micro.ktx2\", \"scale\": \"ce\"}],"
			"\"type\": \"ce\", \"specular\": {\"f0\": 0.06}}", TRUE, &material), "a file of format 2 refused");
		CHECK(material.files[_map_albedo] == 0 && material.files[_map_normal] == 1 && material.files[_map_orm] == 2,
			"images %ld %ld %ld", material.files[_map_albedo], material.files[_map_normal], material.files[_map_orm]);
		CHECK(material.files[_map_primary_detail] == 3 && material.files[_map_micro_detail] == 5 &&
			material.files[_map_secondary_detail] == NONE, "layers %ld %ld %ld", material.files[_map_primary_detail],
			material.files[_map_secondary_detail], material.files[_map_micro_detail]);
		CHECK(material.flags == (FLAG_NORMAL_AT_BUMP | (1UL << TYPE_SHIFT) | (2UL << DETAIL_SHIFT) | (1UL << MICRO_SHIFT)),
			"flags %lx", material.flags);
		CHECK(near(material.scalars[_scalar_primary_detail], 16.0f) &&
			near(material.scalars[_scalar_primary_detail + 1], 8.0f), "primary scale %g %g",
			material.scalars[_scalar_primary_detail], material.scalars[_scalar_primary_detail + 1]);
		CHECK(near(material.scalars[_scalar_secondary_detail], 4.0f) &&
			near(material.scalars[_scalar_secondary_detail + 1], 4.0f), "secondary scale %g %g",
			material.scalars[_scalar_secondary_detail], material.scalars[_scalar_secondary_detail + 1]);
		CHECK(near(material.scalars[_scalar_micro_detail], 16.0f) &&
			near(material.scalars[_scalar_micro_detail + 1], 8.0f), "micro scale %g %g",
			material.scalars[_scalar_micro_detail], material.scalars[_scalar_micro_detail + 1]);
		CHECK(near(material.scalars[_scalar_specular], 0.75f) && near(material.scalars[_scalar_albedo], 1.0f) &&
			near(material.scalars[_scalar_roughness], 1.0f), "specular %g", material.scalars[_scalar_specular]);
		CHECK(material.model == MODEL_AUTO, "model %lu", material.model);
	}
	else if (!strcmp(name, "explicit"))
	{
		/* (the file's own type, function, scale and model) */
		CHECK(parse("{\"format\": 2, \"model\": \"foliage\", \"base\": {\"albedo\": \"textures/prism_v4/panel/albedo.ktx2\","
			"\"roughness\": 0.5, \"albedo_factor\": [0.5, 1, 2]}, \"type\": \"normal\", \"layers\": ["
			"{\"slot\": \"primary_detail\", \"albedo\": \"textures/prism_v4/details/primary.ktx2\", \"function\": "
			"\"multiply\"}, {\"slot\": \"secondary_detail\", \"albedo\": \"textures/prism_v4/details/secondary.ktx2\","
			"\"scale\": [3, 5]}], \"translucency\": 0.25}", TRUE, &material), "a file of format 2 refused");
		CHECK(material.flags == (FLAG_FOLIAGE | (0UL << TYPE_SHIFT) | (1UL << DETAIL_SHIFT) | (1UL << MICRO_SHIFT)),
			"flags %lx", material.flags);
		CHECK(material.model == MODEL_FOLIAGE, "model %lu", material.model);
		CHECK(near(material.scalars[_scalar_secondary_detail], 3.0f) &&
			near(material.scalars[_scalar_secondary_detail + 1], 5.0f), "secondary scale %g %g",
			material.scalars[_scalar_secondary_detail], material.scalars[_scalar_secondary_detail + 1]);
		CHECK(near(material.scalars[_scalar_roughness], 0.5f) && near(material.scalars[_scalar_albedo], 0.5f) &&
			near(material.scalars[_scalar_albedo + 2], 2.0f) && near(material.scalars[_scalar_translucency], 0.25f),
			"factors %g %g %g", material.scalars[_scalar_roughness], material.scalars[_scalar_albedo],
			material.scalars[_scalar_translucency]);
		CHECK(material.files[_map_normal] == NONE, "a normal named");
	}
	else if (!strcmp(name, "model"))
	{
		/* (a model shader's: no layers or type read, its flags its normal's
		coordinates alone) */
		CHECK(parse("{\"format\": 2, \"family\": \"soso\", \"base\": {\"albedo\": \"textures/prism_v4/panel/albedo.ktx2\","
			"\"normal\": \"textures/prism_v4/panel/normal.ktx2\"}, \"layers\": [{\"slot\": \"primary_detail\","
			"\"albedo\": \"textures/prism_v4/details/primary.ktx2\"}]}", FALSE, &material), "a file of format 2 refused");
		CHECK(material.flags == 0 && material.files[_map_primary_detail] == NONE, "flags %lx, primary %ld",
			material.flags, material.files[_map_primary_detail]);
	}
	else if (!strcmp(name, "refused"))
	{
		CHECK(!parse("{\"format\": 1, \"base\": {}}", TRUE, &material), "format 1 read");
		CHECK(!parse("{\"format\": 3, \"base\": {}}", TRUE, &material), "format 3 read");
		CHECK(!parse("{\"format\": 2}", TRUE, &material), "a file with no base read");
		CHECK(parse("{\"format\": 2, \"base\": {\"albedo\": \"textures/none.ktx2\"}}", TRUE, &material) &&
			material.files[_map_albedo] == NONE, "an image no mod has");
	}
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
