/*
GEOMETRY_TEST.C

PRISM's resolved geometry (xgpu_geometry.c) on the CPU: the level's
hysteresis kept per object however many objects there are (more than the
256 slots the table before it had) and however long one is not drawn; a
shadow drawn first at the level the view will draw; the ladder's drawable
floor; the traced triangles' budget shared out the farthest first, never to
stock, never hunting; the variant stock until the mesh is in, then the
remodel; a new map (a settings change) forgetting every choice; the notes'
switches, flips and mismatches; the table's ageing. Run by
tools/test_geometry_resolver.py.
*/

#include "xgpu_geometry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { failures++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

static struct xgpu_geometry_ladder ladder_of(uint32_t a, uint32_t b, uint32_t c, float cut0, float cut1)
{
	struct xgpu_geometry_ladder ladder;

	memset(&ladder, 0, sizeof(ladder));
	ladder.lod_count = 3;
	ladder.triangles[0] = a;
	ladder.triangles[1] = b;
	ladder.triangles[2] = c;
	ladder.cutoff[0] = cut0;
	ladder.cutoff[1] = cut1;
	ladder.cutoff[2] = 0.0f;
	return ladder;
}

static struct xgpu_geometry *made(float hysteresis, uint32_t budget)
{
	struct xgpu_geometry *geometry = xgpu_geometry_create();
	struct xgpu_geometry_settings settings;

	memset(&settings, 0, sizeof(settings));
	settings.hysteresis = hysteresis;
	settings.traced_budget = budget;
	settings.coarse_reach = 40.0f;
	xgpu_geometry_settings_set(geometry, &settings);
	return geometry;
}

static struct xgpu_geometry_choice draw(struct xgpu_geometry *geometry, uint32_t object, int shadow,
	const struct xgpu_geometry_ladder *ladder, float pixels, float x)
{
	struct xgpu_geometry_choice choice;
	float origin[3];

	origin[0] = x;
	origin[1] = 0.0f;
	origin[2] = 0.0f;
	xgpu_geometry_resolve(geometry, object, 7u, shadow, ladder, pixels, origin, &choice);
	xgpu_geometry_note(geometry, object, 7u, shadow ? XGPU_GEOMETRY_SHADOW : XGPU_GEOMETRY_VIEW, choice.variant,
		choice.lod);
	return choice;
}

static const float eye[3] = { 0.0f, 0.0f, 0.0f };

/* a size near a cutoff, back and forth: one switch, no flip */
static void test_hysteresis(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder ladder = ladder_of(2000, 500, 125, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	int frame;

	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 100.0f, 10.0f).lod == 1);
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 145.0f, 10.0f).lod == 0);
	for (frame = 0; frame < 200; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		CHECK(draw(geometry, 1, 0, &ladder, frame & 1 ? 100.0f : 139.0f, 10.0f).lod == 0);
	}
	/* (under the cutoff less the hysteresis: the coarser level) */
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 97.0f, 10.0f).lod == 1);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.switches[XGPU_GEOMETRY_VIEW] == 2);
	CHECK(statistics.flips[XGPU_GEOMETRY_VIEW] == 0);
	/* (not drawn for 100 frames: the level it had kept, its hysteresis too) */
	for (frame = 0; frame < 100; frame++)
		xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 130.0f, 10.0f).lod == 1);
	xgpu_geometry_destroy(geometry);
}

/* more objects than the old table's 256 slots, each near its cutoff: no
object loses its hysteresis */
static void test_many_objects(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder ladder = ladder_of(2000, 500, 125, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	uint32_t object;
	int frame;

	xgpu_geometry_frame(geometry, eye);
	for (object = 0; object < 1000; object++)
		draw(geometry, object * 4096u + 3u, 0, &ladder, 150.0f, 5.0f);
	for (frame = 0; frame < 120; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 1000; object++)
			CHECK(draw(geometry, object * 4096u + 3u, 0, &ladder, (frame + object) & 1 ? 100.0f : 141.0f, 5.0f).lod == 0);
	}
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.switches[XGPU_GEOMETRY_VIEW] == 0);
	CHECK(statistics.entries == 1000);
	xgpu_geometry_destroy(geometry);
}

/* a shadow drawn before the view: the view's level (by its size then and
its distance), not the shadow's own small size; an object only ever seen by
the shadows: its own size */
static void test_shadow_first(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder ladder = ladder_of(2000, 500, 125, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	int frame;

	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 150.0f, 10.0f).lod == 0);
	for (frame = 0; frame < 50; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		CHECK(draw(geometry, 1, 1, &ladder, 20.0f, 10.0f).lod == 0);
		CHECK(draw(geometry, 1, 0, &ladder, 150.0f, 10.0f).lod == 0);
	}
	/* (off screen, moving away: the level follows the distance, 150 px at
	10 wu is 50 px at 30 wu, under 140 x 0.7) */
	for (frame = 0; frame < 5; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		CHECK(draw(geometry, 1, 1, &ladder, 20.0f, 30.0f).lod == 1);
	}
	/* (never in the view: the shadow's own size) */
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 2, 1, &ladder, 20.0f, 30.0f).lod == 2);
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.shadow_mismatch == 0);
	xgpu_geometry_destroy(geometry);
}

/* a ladder that drops 16x a step: drawn to level 0 only; a quarter a step:
every level */
static void test_floor(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder steep = ladder_of(1984, 124, 31, 140.0f, 45.0f);
	struct xgpu_geometry_ladder gentle = ladder_of(1984, 496, 124, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;

	CHECK(xgpu_geometry_drawable_last(&steep) == 0);
	CHECK(xgpu_geometry_drawable_last(&gentle) == 2);
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &steep, 5.0f, 10.0f).lod == 0);
	CHECK(draw(geometry, 2, 0, &gentle, 5.0f, 10.0f).lod == 2);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.floor_held == 1);
	xgpu_geometry_destroy(geometry);
}

/* the traced budget: the farthest made coarser first, never stock, the
nearest kept as drawn, within the budget, and no hunting as the objects
jitter */
static void test_budget(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 1000000u);
	struct xgpu_geometry_ladder ladder = ladder_of(20000, 5000, 1250, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	struct xgpu_geometry_choice choices[100];
	uint64_t total, switches_after;
	uint32_t object;
	int frame;

	for (frame = 0; frame < 3; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 100; object++)
			choices[object] = draw(geometry, object + 1, 0, &ladder, 200.0f, 2.0f + (float)object);
	}
	for (object = 0, total = 0; object < 100; object++)
	{
		CHECK(choices[object].variant == XGPU_GEOMETRY_REMODEL);
		CHECK(choices[object].lod == 0);
		CHECK(choices[object].traced_lod >= choices[object].lod);
		total += ladder.triangles[choices[object].traced_lod];
	}
	CHECK(total <= 1000000u);
	CHECK(choices[0].traced_lod == 0);
	CHECK(choices[99].traced_lod > 0);
	/* (nearer never coarser than farther) */
	for (object = 1; object < 100; object++)
		CHECK(choices[object].traced_lod >= choices[object - 1].traced_lod);
	xgpu_geometry_statistics(geometry, &statistics);
	switches_after = statistics.over_budget;
	CHECK(switches_after == 0);
	/* (jitter: the traced levels hold) */
	for (frame = 0; frame < 100; frame++)
	{
		struct xgpu_geometry_choice now;

		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 100; object++)
		{
			now = draw(geometry, object + 1, 0, &ladder, 200.0f, 2.0f + (float)object + (frame & 1 ? 0.3f : -0.3f));
			xgpu_geometry_note(geometry, object + 1, 7u, XGPU_GEOMETRY_TRACED, now.variant, now.traced_lod);
		}
	}
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.flips[XGPU_GEOMETRY_TRACED] == 0);
	CHECK(statistics.switches[XGPU_GEOMETRY_TRACED] == 0);
	/* (the near ones within the coarse reach: never past the drawable floor,
	even far over budget) */
	xgpu_geometry_destroy(geometry);
	geometry = made(0.3f, 1u);
	{
		struct xgpu_geometry_ladder steep = ladder_of(20000, 1000, 100, 140.0f, 45.0f);

		for (frame = 0; frame < 3; frame++)
		{
			xgpu_geometry_frame(geometry, eye);
			choices[0] = draw(geometry, 1, 0, &steep, 200.0f, 10.0f);
			choices[1] = draw(geometry, 2, 0, &steep, 200.0f, 80.0f);
		}
		CHECK(choices[0].traced_lod == 0);
		CHECK(choices[1].traced_lod == 2);
		CHECK(choices[0].variant == XGPU_GEOMETRY_REMODEL && choices[1].variant == XGPU_GEOMETRY_REMODEL);
	}
	xgpu_geometry_destroy(geometry);
}

/* the skinned vertices' budget: the rigged objects drawn the farthest
coarser first within it, the still ones not counted, never stock, no
hunting; let go once room comes back */
static void test_skinned_budget(void)
{
	struct xgpu_geometry *geometry = xgpu_geometry_create();
	struct xgpu_geometry_settings settings;
	struct xgpu_geometry_ladder rig = ladder_of(60000, 18000, 5000, 300.0f, 100.0f);
	struct xgpu_geometry_ladder rock = ladder_of(20000, 5000, 1250, 140.0f, 45.0f);
	struct xgpu_geometry_choice choices[20], rock_choice;
	struct xgpu_geometry_statistics statistics;
	uint64_t total;
	uint32_t object;
	int frame;

	rig.vertices[0] = 50000;
	rig.vertices[1] = 17000;
	rig.vertices[2] = 5000;
	rig.per_frame = 1;
	rock.vertices[0] = 30000;
	rock.vertices[1] = 8000;
	rock.vertices[2] = 2000;
	memset(&settings, 0, sizeof(settings));
	settings.hysteresis = 0.3f;
	settings.coarse_reach = 40.0f;
	settings.skinned_budget = 500000u;
	xgpu_geometry_settings_set(geometry, &settings);
	for (frame = 0; frame < 4; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 20; object++)
			choices[object] = draw(geometry, object + 1, 0, &rig, 400.0f, 2.0f + (float)object);
		rock_choice = draw(geometry, 100, 0, &rock, 400.0f, 1.0f);
	}
	for (object = 0, total = 0; object < 20; object++)
	{
		CHECK(choices[object].variant == XGPU_GEOMETRY_REMODEL);
		total += rig.vertices[choices[object].lod];
	}
	CHECK(total <= 500000u);
	CHECK(choices[0].lod == 0);
	CHECK(choices[19].lod > 0);
	for (object = 1; object < 20; object++)
		CHECK(choices[object].lod >= choices[object - 1].lod);
	CHECK(rock_choice.lod == 0);
	/* (a still object made to move: skinned into the ring, counted) */
	xgpu_geometry_skinned(geometry, 100, 7u, 0);
	xgpu_geometry_frame(geometry, eye);
	for (object = 0; object < 20; object++)
		draw(geometry, object + 1, 0, &rig, 400.0f, 2.0f + (float)object);
	draw(geometry, 100, 0, &rock, 400.0f, 1.0f);
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.skinned_vertices_last >= 30000u);
	/* (kept again for a while: no longer counted) */
	for (frame = 0; frame < 32; frame++)
		xgpu_geometry_skinned(geometry, 100, 7u, 1);
	/* (still: no hunting) */
	xgpu_geometry_statistics(geometry, &statistics);
	{
		uint64_t switches = statistics.switches[XGPU_GEOMETRY_VIEW];

		for (frame = 0; frame < 60; frame++)
		{
			xgpu_geometry_frame(geometry, eye);
			for (object = 0; object < 20; object++)
				draw(geometry, object + 1, 0, &rig, 400.0f, 2.0f + (float)object);
		}
		xgpu_geometry_statistics(geometry, &statistics);
		CHECK(statistics.switches[XGPU_GEOMETRY_VIEW] == switches);
		CHECK(statistics.skinned_over_budget == 0);
	}
	/* (half of them gone: the others drawn finer again, once their levels
	have been held 150 frames) */
	for (frame = 0; frame < 30; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 8; object++)
			choices[object] = draw(geometry, object + 1, 0, &rig, 400.0f, 2.0f + (float)object);
	}
	CHECK(choices[7].lod > 0);
	for (frame = 0; frame < 200; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 8; object++)
			choices[object] = draw(geometry, object + 1, 0, &rig, 400.0f, 2.0f + (float)object);
	}
	for (object = 0; object < 8; object++)
		CHECK(choices[object].lod == 0);
	xgpu_geometry_destroy(geometry);
}

/* stock until the mesh is in, then the remodel, once; a new map (the tier
or the meshes changed) forgets every choice */
static void test_variant_and_map(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder ladder = ladder_of(2000, 500, 125, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	int frame;

	for (frame = 0; frame < 10; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		CHECK(draw(geometry, 1, 0, NULL, 150.0f, 10.0f).variant == XGPU_GEOMETRY_STOCK);
	}
	for (frame = 0; frame < 10; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		CHECK(draw(geometry, 1, 0, &ladder, 150.0f, 10.0f).variant == XGPU_GEOMETRY_REMODEL);
	}
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.to_remodel == 1);
	CHECK(statistics.to_stock == 0);
	CHECK(statistics.switches[XGPU_GEOMETRY_VIEW] == 1);
	/* (prism_models "stock": a new set, nothing replaced) */
	xgpu_geometry_map(geometry);
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, NULL, 150.0f, 10.0f).variant == XGPU_GEOMETRY_STOCK);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.to_stock == 0);
	CHECK(statistics.entries == 1);
	/* (and "max" again: the remodel at once, its level from its size) */
	xgpu_geometry_map(geometry);
	xgpu_geometry_frame(geometry, eye);
	CHECK(draw(geometry, 1, 0, &ladder, 100.0f, 10.0f).lod == 1);
	xgpu_geometry_destroy(geometry);
}

/* the notes: a change undone within the window is a flip, one undone later
is not; a shadow drawing another level is a mismatch; a traced stock shape
under a drawn remodel too */
static void test_notes(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_statistics statistics;
	int frame;

	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_REMODEL, 0);
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_REMODEL, 1);
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_REMODEL, 0);
	for (frame = 0; frame < 200; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_REMODEL, 0);
	}
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_VIEW, XGPU_GEOMETRY_REMODEL, 1);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_SHADOW, XGPU_GEOMETRY_REMODEL, 0);
	xgpu_geometry_note(geometry, 5, 9, XGPU_GEOMETRY_TRACED, XGPU_GEOMETRY_STOCK, 0);
	xgpu_geometry_frame(geometry, eye);
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.switches[XGPU_GEOMETRY_VIEW] == 3);
	CHECK(statistics.flips[XGPU_GEOMETRY_VIEW] == 1);
	CHECK(statistics.shadow_mismatch == 1);
	CHECK(statistics.traced_stock == 1);
	xgpu_geometry_destroy(geometry);
}

/* the table: objects not drawn lately let go of, the others found with
their state after the shifts */
static void test_ageing(void)
{
	struct xgpu_geometry *geometry = made(0.3f, 0);
	struct xgpu_geometry_ladder ladder = ladder_of(2000, 500, 125, 140.0f, 45.0f);
	struct xgpu_geometry_statistics statistics;
	uint32_t object;
	int frame;

	xgpu_geometry_frame(geometry, eye);
	for (object = 0; object < 3000; object++)
		draw(geometry, object * 977u, 0, &ladder, 150.0f, 5.0f);
	for (frame = 0; frame < (int)XGPU_GEOMETRY_AGE_FRAMES + 5; frame++)
	{
		xgpu_geometry_frame(geometry, eye);
		for (object = 0; object < 3000; object += 2)
			draw(geometry, object * 977u, 0, &ladder, 110.0f, 5.0f);
	}
	xgpu_geometry_statistics(geometry, &statistics);
	CHECK(statistics.entries == 1500);
	/* (each kept: level 0 still, by its hysteresis at 110 px) */
	xgpu_geometry_frame(geometry, eye);
	for (object = 0; object < 3000; object += 2)
		CHECK(draw(geometry, object * 977u, 0, &ladder, 110.0f, 5.0f).lod == 0);
	/* (each let go of: new, level 1 at 110 px) */
	for (object = 1; object < 3000; object += 2)
		CHECK(draw(geometry, object * 977u, 0, &ladder, 110.0f, 5.0f).lod == 1);
	xgpu_geometry_destroy(geometry);
}

int main(void)
{
	test_hysteresis();
	test_many_objects();
	test_shadow_first();
	test_floor();
	test_budget();
	test_skinned_budget();
	test_variant_and_map();
	test_notes();
	test_ageing();
	if (failures)
	{
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
