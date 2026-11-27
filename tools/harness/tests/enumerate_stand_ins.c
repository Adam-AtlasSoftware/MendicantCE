/* A map's first frame draws its other structures' materials by letting the
first structure's materials stand in for them, a round at a time, one of
its lightmaps drawn without its picture where a key to draw has none
(port/linux/game/render_enumerate.c's stand_ins_next): once the rounds end,
every material has its own shader and permutation back and every lightmap
its own picture, whatever the keys were. A lightmap left without its
picture lost its lightmap for the session, and SCENE_STATIC's set, the
bake's key and its plan with it. */
#include "harness.h"

#include "enums.inc"

struct tag_reference { long index; };
struct block { long count; void *address; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])

struct structure_material
{
	struct tag_reference shader;
	short permutation_index;
	unsigned short flags;
	long first_surface_index;
	long surface_count;
};
struct structure_lightmap
{
	short bitmap_index;
	struct block materials;
};
struct structure_bsp
{
	struct block lightmaps;
	struct block surfaces;
};

static struct { boolean mirrors; } enumerate;

/* (material_key's & and | as the game writes them) */
#pragma GCC diagnostic ignored "-Wparentheses"
#include "under_test.inc"

#define LIGHTMAPS 3
#define MATERIALS 2
static struct structure_lightmap lightmaps[LIGHTMAPS];
static struct structure_material materials[LIGHTMAPS][MATERIALS];
static struct structure_bsp bsp;
static struct enumerated_materials enumerated;

/* a structure: lightmap 0 with no picture, 1 and 2 with pictures 4 and 5;
each two materials of shaders 10 + 2 * lightmap + material, flags 1 */
static void structure_make(void)
{
	int l, m;

	memset(&bsp, 0, sizeof(bsp));
	for (l = 0; l < LIGHTMAPS; l++)
	{
		lightmaps[l].bitmap_index = l == 0 ? NONE : (short)(3 + l);
		lightmaps[l].materials.count = MATERIALS;
		lightmaps[l].materials.address = materials[l];
		for (m = 0; m < MATERIALS; m++)
		{
			materials[l][m].shader.index = 10 + 2 * l + m;
			materials[l][m].permutation_index = (short)(l + 1);
			materials[l][m].flags = 1;
			materials[l][m].first_surface_index = 2 * l + m;
			materials[l][m].surface_count = 1;
		}
	}
	bsp.lightmaps.count = LIGHTMAPS;
	bsp.lightmaps.address = lightmaps;
	bsp.surfaces.count = 2 * LIGHTMAPS;
	memset(&stand_ins, 0, sizeof(stand_ins));
	memset(&enumerated, 0, sizeof(enumerated));
}

/* a key still to draw: its shader, whether its lightmap has a picture, its flags */
static void pending_add(long shader, boolean picture, unsigned long flags)
{
	stand_ins.pending_shader_indices[stand_ins.pending.count] = shader;
	stand_ins.pending.keys[stand_ins.pending.count++] = (unsigned long)(shader & 0xffff) | (picture ? 0x10000UL : 0) |
		(flags & 0x3fffUL) << 17;
}

/* the structure as it was made */
static void check_whole(const char *when)
{
	int l, m;

	for (l = 0; l < LIGHTMAPS; l++)
	{
		CHECK(lightmaps[l].bitmap_index == (l == 0 ? NONE : 3 + l), "%s: lightmap %d's picture is %d", when, l,
			lightmaps[l].bitmap_index);
		for (m = 0; m < MATERIALS; m++)
		{
			CHECK(materials[l][m].shader.index == 10 + 2 * l + m, "%s: lightmap %d material %d's shader is %ld", when, l,
				m, materials[l][m].shader.index);
			CHECK(materials[l][m].permutation_index == l + 1, "%s: lightmap %d material %d's permutation is %d", when, l,
				m, materials[l][m].permutation_index);
		}
	}
}

static int rounds_run(void)
{
	int rounds = 0;

	while (stand_ins_next(&bsp, &enumerated))
	{
		CHECK(++rounds <= MAXIMUM_STAND_IN_ROUNDS, "more rounds than %d", MAXIMUM_STAND_IN_ROUNDS);
		CHECK(stand_ins.count > 0, "a round with no material standing in");
	}
	return rounds;
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";
	int rounds;

	structure_make();
	if (!strcmp(which, "none-stands-in"))
	{
		/* (a key with no picture whose class no material has: the unlit
		lightmap is chosen, no material stands in, no round is drawn) */
		pending_add(90, FALSE, 7);
		rounds = rounds_run();
		CHECK(rounds == 0, "%d rounds", rounds);
		check_whole("after no round");
		CHECK(stand_ins.unlit == NULL, "a lightmap still standing in without its picture");
	}
	else if (!strcmp(which, "one-round"))
	{
		/* (a key with no picture of the materials' class: lightmap 1 drawn
		without its picture for a round, then given it back) */
		pending_add(91, FALSE, 1);
		CHECK(stand_ins_next(&bsp, &enumerated), "no round");
		CHECK(lightmaps[1].bitmap_index == NONE, "the round's lightmap kept its picture (%d)", lightmaps[1].bitmap_index);
		rounds = 1 + rounds_run();
		CHECK(rounds == 1, "%d rounds", rounds);
		check_whole("after the round");
		CHECK(materials_have(&enumerated, 91UL | (1UL << 17)), "the key drawn not counted as drawn");
	}
	else if (!strcmp(which, "mixed"))
	{
		/* (one key that can stand in and one that cannot: a round, then
		the last call, which stands in none, must give everything back) */
		pending_add(92, FALSE, 1);
		pending_add(93, FALSE, 5);
		rounds = rounds_run();
		CHECK(rounds == 1, "%d rounds", rounds);
		check_whole("after the rounds");
	}
	else if (!strcmp(which, "pictured"))
	{
		/* (a key with a picture: no lightmap loses its picture) */
		pending_add(94, TRUE, 1);
		rounds = rounds_run();
		CHECK(rounds == 1, "%d rounds", rounds);
		check_whole("after the round");
	}
	else
	{
		fprintf(stderr, "no case %s\n", which);
		return 2;
	}
	return 0;
}
