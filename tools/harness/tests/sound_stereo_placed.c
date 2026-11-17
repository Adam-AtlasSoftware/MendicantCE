/*
SOUND_STEREO_PLACED.C (test)

sound_manager.c's one rule for a stereo sound of the world (#1168): the
game's own (an Xbox map's tags, and Halo PC's on a Custom Edition map) play
as the Xbox played them, unplaced and faded linearly with distance; a mod's
or a loose tag file's in place of a map's sound, and a Custom Edition map's
own, are placed (spread by the mixer) and fade as a 3D sound does.
*/

#include "harness.h"

/* the tags: replaced by a mod or loose tag file, and a Custom Edition map
loaded with its Halo PC tags */
static boolean replaced[8], stock[8], custom_edition_loaded;

static boolean mod_sounds_replaced(long tag_index) { return replaced[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)]; }
static boolean custom_edition_cache_tags_loaded(void) { return custom_edition_loaded; }
static boolean custom_edition_cache_stock_tag(long tag_index)
{
	return custom_edition_loaded && stock[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)];
}

#include "under_test.inc"

#define TAG(index) ((long)(0xE1000000UL | (index)))
enum { FLYBY = 1, REPLACED_GUNFIRE = 2, CUSTOM_GUNFIRE = 3, HALO_PC_FLYBY = 4 };

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	/* a sound 4 units away, its minimum distance 1, maximum 9: the linear
	fade 1 - (4 - 1) / (9 - 1) */
	real linear = 1.f - 3.f / 8.f, distance = 4.f, minimum = 1.f;

	replaced[REPLACED_GUNFIRE] = TRUE;
	stock[HALO_PC_FLYBY] = TRUE;
	CASE("stock")
	{
		/* an Xbox map's stereo flyby: unplaced, the linear fade */
		boolean placed = sound_stereo_placed(TAG(FLYBY));

		CHECK(!placed, "an Xbox map's stereo flyby is placed");
		CHECK(sound_stereo_fade(placed, linear, distance, minimum) == linear,
			"an Xbox map's stereo flyby fades %f, not linearly (%f)", sound_stereo_fade(placed, linear, distance, minimum),
			linear);
		return 0;
	}
	CASE("replacement")
	{
		boolean placed = sound_stereo_placed(TAG(REPLACED_GUNFIRE));

		CHECK(placed, "a mod's stereo sound in place of a map's is not placed");
		CHECK(sound_stereo_fade(placed, linear, distance, minimum) == 0.25f, "a placed stereo sound fades %f, not "
			"as a 3D sound (0.25)", sound_stereo_fade(placed, linear, distance, minimum));
		CHECK(sound_stereo_fade(placed, linear, 0.f, minimum) == 1.f && sound_stereo_fade(placed, linear, 0.5f, 0.f) == 1.f,
			"a placed stereo sound within its minimum distance, or of none, is faded");
		return 0;
	}
	CASE("custom-edition")
	{
		CHECK(!sound_stereo_placed(TAG(CUSTOM_GUNFIRE)), "a tag is placed with no Custom Edition map loaded");
		custom_edition_loaded = TRUE;
		CHECK(sound_stereo_placed(TAG(CUSTOM_GUNFIRE)), "a Custom Edition map's own stereo sound is not placed");
		CHECK(!sound_stereo_placed(TAG(HALO_PC_FLYBY)), "a Halo PC stereo flyby on a Custom Edition map is placed");
		CHECK(sound_stereo_placed(TAG(REPLACED_GUNFIRE)), "a replacement on a Custom Edition map is not placed");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
