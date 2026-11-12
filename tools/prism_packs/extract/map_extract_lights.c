/*
MAP_EXTRACT_LIGHTS.C

PRISM lightmap re-bake: the light sources of a map, read as map_extract
reads the map (the game's tag validator first; every read bounds checked),
written as JSON for the local baker (relight/relight_bake.py). Nothing is
written into the map; the output stays on this machine.

Part of the extractor (map_extract_tool.c includes map_extract.c, then this
file), as its "lights" command:

usage: map_extract lights [--data <data root>] --out <file.json> <map>

What it writes:
  skies[]: each sky the scenario names: its indoor and outdoor ambient
    radiosity (colour, power) and its lights (flags: 1 affects exteriors,
    2 affects interiors; colour, power, test distance, direction as yaw and
    pitch in radians, diameter in radians), as tool's radiosity reads them;
  bsps[]: each structure bsp: its tag, surface count, the cluster of each
    surface (from the clusters' surface lists), each cluster's sky, and
    each lightmap material: its shader (group, radiosity properties: flags,
    detail level, power, emitted colour, tint), an environment shader's
    self-illumination (its primary, secondary and plasma on/off colours and
    map), the game's runtime light of
    the material (ambient colour, distant lights, shadow vector and colour)
    and its first surface and count.
*/

enum
{
	SCENARIO_SKIES_OFFSET = 0x30,
	SKY_GROUP_TAG = 'sky ',
	SKY_LIGHTS_OFFSET = 0xC4,
	SKY_LIGHT_SIZE = 0x74,
	CLUSTER_SIZE = 0x68,
	CLUSTER_SUBCLUSTERS_OFFSET = 0x34,
	SUBCLUSTER_SIZE = 0x24,
	/* shader_environment_definition's self-illumination
	(rasterizer_xbox_environment.c's shader_environment_self_illumination_properties):
	the shader's 0x28 bytes, the environment's 0x44, the diffuse's 0x114 */
	SENV_GROUP_TAG = 'senv',
	SENV_SELF_ILLUMINATION_OFFSET = 0x28 + 0x44 + 0x114,
	SENV_SELF_ILLUMINATION_SIZE = 0xD4,
};

/* shader_definitions.h's shader_radiosity_properties, at a shader's start */
struct shader_radiosity_like
{
	unsigned short flags;
	short detail_level;
	float power;
	float color[3];
	float tint[3];
};

static void rgb(FILE *f, float const *c)
{
	fprintf(f, "[");
	json_float(f, c[0]);
	fprintf(f, ", ");
	json_float(f, c[1]);
	fprintf(f, ", ");
	json_float(f, c[2]);
	fprintf(f, "]");
}

static void skies_write(FILE *f)
{
	struct tag_instance *scenario = instance_get(globals.tags->scenario_tag_index);
	struct tag_block *skies = (struct tag_block *)((unsigned char *)scenario->base_address + SCENARIO_SKIES_OFFSET);
	long s, l;

	fprintf(f, "  \"skies\": [");
	for (s = 0; s < skies->count; s++)
	{
		struct tag_reference *reference = (struct tag_reference *)cache_bytes(
			(struct tag_reference *)skies->address + s, sizeof(struct tag_reference));
		struct tag_instance *sky = reference ? instance_get(reference->index) : NULL;
		unsigned char *base;
		struct tag_block *lights;

		fprintf(f, "%s\n    {", s ? "," : "");
		if (!sky || sky->group_tag != SKY_GROUP_TAG || !cache_bytes(sky->base_address, 0xD0))
		{
			fprintf(f, "\"tag\": null}");
			continue;
		}
		base = sky->base_address;
		fprintf(f, "\"tag\": ");
		json_string(f, tag_name(reference->index));
		fprintf(f, ", \"indoor_ambient\": ");
		rgb(f, (float *)(base + 0x38));
		fprintf(f, ", \"indoor_power\": ");
		json_float(f, *(float *)(base + 0x44));
		fprintf(f, ", \"outdoor_ambient\": ");
		rgb(f, (float *)(base + 0x48));
		fprintf(f, ", \"outdoor_power\": ");
		json_float(f, *(float *)(base + 0x54));
		fprintf(f, ", \"outdoor_fog\": ");
		rgb(f, (float *)(base + 0x58));
		fprintf(f, ", \"lights\": [");
		lights = (struct tag_block *)(base + SKY_LIGHTS_OFFSET);
		for (l = 0; l < lights->count && l < 8; l++)
		{
			unsigned char *light = cache_bytes((unsigned char *)lights->address + l * SKY_LIGHT_SIZE, SKY_LIGHT_SIZE);

			if (!light)
				break;
			fprintf(f, "%s{\"flags\": %lu, \"color\": ", l ? ", " : "", (unsigned long)*(uint32_t *)(light + 0x4C));
			rgb(f, (float *)(light + 0x50));
			fprintf(f, ", \"power\": ");
			json_float(f, *(float *)(light + 0x5C));
			fprintf(f, ", \"test_distance\": ");
			json_float(f, *(float *)(light + 0x60));
			fprintf(f, ", \"yaw\": ");
			json_float(f, *(float *)(light + 0x68));
			fprintf(f, ", \"pitch\": ");
			json_float(f, *(float *)(light + 0x6C));
			fprintf(f, ", \"diameter\": ");
			json_float(f, *(float *)(light + 0x70));
			fprintf(f, ", \"lens_flare\": ");
			json_string(f, tag_name(((struct tag_reference *)light)->index));
			fprintf(f, "}");
		}
		fprintf(f, "]}");
	}
	fprintf(f, "\n  ],\n");
}

static void bsp_write(FILE *f, long bsp_index, int first)
{
	struct structure_bsp_header *header = bsp_load(bsp_index);
	struct tag_schema_group const *group = schema_group(STRUCTURE_BSP_GROUP_TAG);
	struct tag_schema_field const *surfaces_field, *lightmaps_field, *clusters_field;
	struct tag_block *surfaces_block, *lightmaps_block, *clusters_block;
	unsigned char *bsp;
	long *surface_cluster;
	long c, l, m, i;

	if (!header || !group || !group->definition)
		return;
	bsp = header->base_address;
	surfaces_field = schema_field(group->definition, "surfaces");
	lightmaps_field = schema_field(group->definition, "lightmaps");
	clusters_field = schema_field(group->definition, "clusters");
	if (!surfaces_field || !lightmaps_field || !clusters_field)
		fail("the structure bsp schema is not the one this tool knows");
	surfaces_block = (struct tag_block *)(bsp + surfaces_field->offset);
	lightmaps_block = (struct tag_block *)(bsp + lightmaps_field->offset);
	clusters_block = (struct tag_block *)(bsp + clusters_field->offset);
	surface_cluster = allocate((surfaces_block->count + 1) * sizeof(long));
	for (i = 0; i < surfaces_block->count; i++)
		surface_cluster[i] = NONE;

	fprintf(f, "%s\n    {\"index\": %ld, \"tag\": ", first ? "" : ",", bsp_index);
	json_string(f, tag_name(globals.loaded_bsp));
	fprintf(f, ", \"surface_count\": %ld, \"clusters\": [", surfaces_block->count);
	for (c = 0; c < clusters_block->count; c++)
	{
		unsigned char *cluster = cache_bytes((unsigned char *)clusters_block->address + c * CLUSTER_SIZE, CLUSTER_SIZE);
		struct tag_block *list;
		long *indices;
		long count, index = 0, surfaces = 0;

		if (!cluster)
			break;
		/* (the retail clusters have no surface lists: their subclusters'
		lists, structure_visibility_traverse_subclusters) */
		list = (struct tag_block *)(cluster + CLUSTER_SUBCLUSTERS_OFFSET);
		count = list->count;
		for (index = 0; index < count; index++)
		{
			unsigned char *subcluster = cache_bytes((unsigned char *)list->address + index * SUBCLUSTER_SIZE,
				SUBCLUSTER_SIZE);
			struct tag_block *surface_list;
			long k;

			if (!subcluster)
				break;
			surface_list = (struct tag_block *)(subcluster + 0x18);
			indices = surface_list->count > 0 ? cache_bytes(surface_list->address, surface_list->count * sizeof(long)) : NULL;
			for (k = 0; indices && k < surface_list->count; k++)
			{
				if (indices[k] >= 0 && indices[k] < surfaces_block->count)
				{
					if (surface_cluster[indices[k]] == NONE)
						surfaces++;
					surface_cluster[indices[k]] = c;
				}
			}
		}
		fprintf(f, "%s{\"sky\": %d, \"surfaces\": %ld}", c ? ", " : "", *(short *)cluster, surfaces);
	}
	fprintf(f, "],\n     \"surface_cluster\": [");
	for (i = 0; i < surfaces_block->count; i++)
		fprintf(f, "%s%ld", i ? "," : "", surface_cluster[i]);
	fprintf(f, "],\n     \"materials\": [");
	{
		int any = 0;

		for (l = 0; l < lightmaps_block->count; l++)
		{
			struct structure_lightmap *lightmap = (struct structure_lightmap *)lightmaps_block->address + l;

			for (m = 0; m < lightmap->materials.count; m++)
			{
				struct structure_material *material = (struct structure_material *)lightmap->materials.address + m;
				struct tag_instance *shader = instance_get(material->shader.index);
				float const *lighting = (float const *)material->lighting;
				char group_name[5];

				fprintf(f, "%s\n      {\"lightmap_index\": %ld, \"material_index\": %ld, \"bitmap_index\": %d, "
					"\"first_surface\": %ld, \"surface_count\": %ld, \"shader\": ", any ? "," : "", l, m,
					lightmap->bitmap_index, material->first_surface_index, material->surface_count);
				any = 1;
				json_string(f, tag_name(material->shader.index));
				fprintf(f, ", \"group\": ");
				json_string(f, shader ? group_string(shader->group_tag, group_name) : "");
				if (shader && cache_bytes(shader->base_address, sizeof(struct shader_radiosity_like)))
				{
					struct shader_radiosity_like const *r = shader->base_address;

					fprintf(f, ", \"radiosity\": {\"flags\": %u, \"detail\": %d, \"power\": ", r->flags, r->detail_level);
					json_float(f, r->power);
					fprintf(f, ", \"color\": ");
					rgb(f, r->color);
					fprintf(f, ", \"tint\": ");
					rgb(f, r->tint);
					fprintf(f, "}");
				}
				/* (an environment shader's self-illumination: what its lamp looks like, colour by
				colour; the relight track checks the emitted light's hue against it) */
				if (shader && shader->group_tag == SENV_GROUP_TAG && cache_bytes((unsigned char *)shader->base_address +
					SENV_SELF_ILLUMINATION_OFFSET, SENV_SELF_ILLUMINATION_SIZE))
				{
					unsigned char const *si = (unsigned char const *)shader->base_address + SENV_SELF_ILLUMINATION_OFFSET;

					fprintf(f, ", \"self_illumination\": {\"flags\": %u, \"primary_on\": ", *(unsigned short const *)si);
					rgb(f, (float const *)(si + 0x1C));
					fprintf(f, ", \"primary_off\": ");
					rgb(f, (float const *)(si + 0x28));
					fprintf(f, ", \"secondary_on\": ");
					rgb(f, (float const *)(si + 0x58));
					fprintf(f, ", \"secondary_off\": ");
					rgb(f, (float const *)(si + 0x64));
					fprintf(f, ", \"plasma_on\": ");
					rgb(f, (float const *)(si + 0x94));
					fprintf(f, ", \"plasma_off\": ");
					rgb(f, (float const *)(si + 0xA0));
					fprintf(f, ", \"map\": ");
					json_string(f, tag_name(((struct tag_reference const *)(si + 0xC4))->index));
					fprintf(f, "}");
				}
				fprintf(f, ", \"ambient\": ");
				rgb(f, lighting);
				fprintf(f, ", \"distant_count\": %d, \"distant\": [", *(short const *)(material->lighting + 12));
				rgb(f, lighting + 4);
				fprintf(f, ", ");
				rgb(f, lighting + 7);
				fprintf(f, ", ");
				rgb(f, lighting + 10);
				fprintf(f, ", ");
				rgb(f, lighting + 13);
				fprintf(f, "], \"shadow_vector\": ");
				rgb(f, lighting + 23);
				fprintf(f, ", \"shadow_color\": ");
				rgb(f, lighting + 26);
				fprintf(f, "}");
			}
		}
	}
	fprintf(f, "]}");
	free(surface_cluster);
}

int main(int argc, char **argv)
{
	char const *data = getenv("HALO_DATA_ROOT");
	char const *out = NULL, *map = NULL;
	char path[2048], name[256];
	FILE *f;
	long index;
	int i;

	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--data") && i + 1 < argc)
			data = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc)
			out = argv[++i];
		else if (argv[i][0] == '-' || map)
		{
			fprintf(stderr, "usage: map_extract lights [--data <data root>] --out <file.json> <map>\n");
			return 2;
		}
		else
			map = argv[i];
	}
	if (!map || !out)
	{
		fprintf(stderr, "usage: map_extract lights [--data <data root>] --out <file.json> <map>\n");
		return 2;
	}
	snprintf(path, sizeof(path), "%s/maps/%s.map", data ? data : ".", map);
	snprintf(name, sizeof(name), "%s", map);
	globals.name = name;
	globals.scale = 1.0f;
	globals.tag_cache = mmap((void *)TAG_CACHE_BASE_ADDRESS, TAG_CACHE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (globals.tag_cache != (unsigned char *)TAG_CACHE_BASE_ADDRESS)
		fail("the tag cache's address (%08x) is taken", TAG_CACHE_BASE_ADDRESS);
	map_load(path);
	f = fopen(out, "w");
	if (!f)
		fail("cannot write %s", out);
	fprintf(f, "{\n  \"map\": ");
	json_string(f, name);
	fprintf(f, ",\n");
	skies_write(f);
	fprintf(f, "  \"bsps\": [");
	{
		struct tag_block *bsps = scenario_bsps();

		for (index = 0; index < bsps->count && index < MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO; index++)
			bsp_write(f, index, index == 0);
	}
	fprintf(f, "\n  ]\n}\n");
	fclose(f);
	printf("%s: light sources written to %s (%ld warnings)\n", name, out, globals.errors);
	return 0;
}
