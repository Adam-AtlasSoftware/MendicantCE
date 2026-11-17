#include "harness.h"

/* PRISM v4's projected decals (port/linux/game/prism_deferred.c): the box a
decal is projected by, kept from its projection as decals.c makes it, and
the SCENE_DECAL record its draw sends in its place */
#include "config.inc"
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;
typedef struct { real scale; real_vector3d forward, left, up; real_point3d position; } real_matrix4x3;

/* (the record the stream would take) */
static unsigned char sent[256];
static int sent_count;
static void xgpu_backend_scene_decal(const void *decal)
{
	memcpy(sent, decal, 120);
	sent_count++;
}
#include "under_test.inc"

static real_matrix4x3 basis = { 1.0f, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
	{ 10.0f, 20.0f, 5.0f } };
static real_rectangle2d extent = { -1.0f, 3.0f, -2.0f, 2.0f };
static real_rectangle2d sprite = { 0.25f, 0.75f, 0.5f, 1.0f };

static boolean near(float a, float b)
{
	return a - b < 1.0e-5f && b - a < 1.0e-5f;
}

int main(int argc, char **argv)
{
	struct decal_record const *record = (struct decal_record const *)sent;
	char const *which = argc > 1 ? argv[1] : "";
	long decal = (7L << 16) | 12;

	CHECK(sizeof(struct decal_record) == 120, "the record is SCENE_DECAL's 120 bytes");
	if (!strcmp(which, "box"))
	{
		/* (the box's centre is the extent's middle on the surface, its axes
		the extent's halves across and up and the decal's reach out of it) */
		prism_deferred_decal_made(decal, &basis, &extent, &sprite, 1.5f);
		CHECK(prism_deferred_decal(decal, 0x81234560UL, 2, 0xff336699UL, 128, TRUE), "sent");
		CHECK(sent_count == 1, "one record");
		CHECK(near(record->centre[0], 11.0f) && near(record->centre[1], 20.0f) && near(record->centre[2], 5.0f),
			"centre %f %f %f", record->centre[0], record->centre[1], record->centre[2]);
		CHECK(near(record->axes[0][0], 2.0f) && near(record->axes[0][1], 0.0f) && near(record->axes[1][1], 2.0f) &&
			near(record->axes[1][0], 0.0f) && near(record->axes[2][2], 2.0f), "axes");
		CHECK(near(record->rectangle[0], 0.25f) && near(record->rectangle[1], 0.5f) &&
			near(record->rectangle[2], 0.75f) && near(record->rectangle[3], 1.0f), "the sprite's rectangle");
		CHECK(near(record->color[0], 0x33 / 255.0f) && near(record->color[1], 0x66 / 255.0f) &&
			near(record->color[2], 0x99 / 255.0f) && near(record->color[3], 128 / 255.0f), "colour");
		CHECK(record->bitmap == 0x81234560UL && record->blend == 2 && record->flags == 4 && record->decal == 13,
			"bitmap %lx blend %lu flags %lu decal %lu", record->bitmap, record->blend, record->flags, record->decal);
		return 0;
	}
	if (!strcmp(which, "stale"))
	{
		/* (another decal in the slot: none, CE draws it) */
		prism_deferred_decal_made(decal, &basis, &extent, &sprite, 1.5f);
		CHECK(!prism_deferred_decal((8L << 16) | 12, 0x1000UL, 0, 0xffffffffUL, 255, FALSE), "a stale box sent");
		CHECK(sent_count == 0, "nothing sent");
		return 0;
	}
	if (!strcmp(which, "unloaded"))
	{
		/* (the map let go: no box kept for a decal of the next one with the
		same handle) */
		prism_deferred_decal_made(decal, &basis, &extent, &sprite, 1.5f);
		prism_deferred_map_unloaded();
		CHECK(!prism_deferred_decal(decal, 0x1000UL, 0, 0xffffffffUL, 255, FALSE), "a box of the map before sent");
		CHECK(sent_count == 0, "nothing sent");
		return 0;
	}
	if (!strcmp(which, "refused"))
	{
		/* (no box made, no texture, a blend past CE's eight) */
		CHECK(!prism_deferred_decal(decal, 0x1000UL, 0, 0xffffffffUL, 255, FALSE), "no box");
		prism_deferred_decal_made(decal, &basis, &extent, &sprite, 1.5f);
		CHECK(!prism_deferred_decal(decal, 0, 0, 0xffffffffUL, 255, FALSE), "no texture");
		CHECK(!prism_deferred_decal(decal, 0x1000UL, 8, 0xffffffffUL, 255, FALSE), "blend 8");
		CHECK(sent_count == 0, "nothing sent");
		return 0;
	}
	fprintf(stderr, "no case %s\n", which);
	return 2;
}
