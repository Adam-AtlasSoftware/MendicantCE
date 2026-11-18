/*
MENU_SPARTAN_STALE.C (test)

The menus' Spartan's objects after the map they were made in (#1186):
menu_spartan.c's spartan_object_current, menu_spartan_object (which the
scene's view leaves out: render_objects.c) and spartan_delete against fake
objects and a fake tags' generation. A map's objects are made in the same
order each time one loads, so the ui map's Spartan and rifle (indices 1 and
2) have the same indices as the next map's first scenery (ratrace's
teleporters): kept past the map, they hid those, and deleting them would
have deleted those. test_menu_spartan_stale.py takes the code.
*/

#include "harness.h"

#define MAXIMUM_MENU_SPARTANS 12
#define MAXIMUM_OBJECTS 32

/* ---------- the fake game */

struct object_datum
{
	long definition_index;
};

struct menu_spartan
{
	long object_index;
	long rifle_index;
	boolean posed, rifle_posed;
};

static struct
{
	struct menu_spartan spartans[MAXIMUM_MENU_SPARTANS];
	unsigned long generation;
	long spartan_definition, rifle_definition;
} menu_spartan;

/* the objects of the map loaded now: a slot's index (its salt and place),
its definition, NONE when free */
static long object_indices[MAXIMUM_OBJECTS];
static struct object_datum objects[MAXIMUM_OBJECTS];
static unsigned long tags_generation = 1;
static long deleted[4];
static int deleted_count;

static unsigned long cache_files_tags_generation(void)
{
	return tags_generation;
}

static struct object_datum *object_try_and_get(long object_index)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	if (object_index == NONE || slot >= MAXIMUM_OBJECTS || object_indices[slot] != object_index)
		return NULL;
	return &objects[slot];
}

static void object_delete(long object_index)
{
	if (deleted_count < 4)
		deleted[deleted_count++] = object_index;
}

/* a map loaded: its objects made from the first slot on, each with the
index its salt sequence gives it (the same each load) */
static void map_load(long const *definitions, int count)
{
	int slot;

	tags_generation++;
	for (slot = 0; slot < MAXIMUM_OBJECTS; slot++)
		object_indices[slot] = NONE;
	for (slot = 0; slot < count; slot++)
	{
		object_indices[slot] = (long)(0xE26F0000UL + 0x10000UL * (unsigned long)slot) | slot;
		objects[slot].definition_index = definitions[slot];
	}
}

static void spartans_none(void)
{
	int index;

	for (index = 0; index < MAXIMUM_MENU_SPARTANS; index++)
	{
		menu_spartan.spartans[index].object_index = NONE;
		menu_spartan.spartans[index].rifle_index = NONE;
	}
}

#include "under_test.inc"

/* ---------- the cases */

/* the ui map: the ring (0), a Spartan (1) and its rifle (2); its tags'
generation the Spartan's own (spartan_forget) */
enum { UI_RING = 100, UI_SPARTAN = 101, UI_RIFLE = 102, TELEPORTER = 200, WEAPON = 201 };

static void ui_menu(void)
{
	static long const ui[] = { UI_RING, UI_SPARTAN, UI_RIFLE };

	map_load(ui, 3);
	spartans_none();
	menu_spartan.generation = tags_generation;
	menu_spartan.spartan_definition = UI_SPARTAN;
	menu_spartan.rifle_definition = UI_RIFLE;
	menu_spartan.spartans[0].object_index = object_indices[1];
	menu_spartan.spartans[0].rifle_index = object_indices[2];
	deleted_count = 0;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* the menus: its own Spartan and rifle left out of the scene, nothing
	else */
	CASE("own")
	{
		ui_menu();
		CHECK(menu_spartan_object(object_indices[1]), "the Spartan drawn in the scene");
		CHECK(menu_spartan_object(object_indices[2]), "the rifle drawn in the scene");
		CHECK(!menu_spartan_object(object_indices[0]), "the ring left out");
		CHECK(!menu_spartan_object(NONE), "none left out");
		spartan_delete(&menu_spartan.spartans[0]);
		CHECK(deleted_count == 2, "the Spartan and its rifle not deleted (%d)", deleted_count);
		return 0;
	}

	/* a game's map loaded after the menus: its teleporters have the
	Spartan's and the rifle's indices, and are drawn (the tags they were
	of gone: a definition's index there means another tag; here the
	teleporter's is the old rifle's) */
	CASE("map-changed")
	{
		static long const ratrace[] = { TELEPORTER, TELEPORTER, UI_RIFLE, TELEPORTER };

		ui_menu();
		map_load(ratrace, 4);
		CHECK(object_indices[2] == menu_spartan.spartans[0].rifle_index, "the test's indices do not repeat");
		CHECK(!menu_spartan_object(object_indices[1]), "teleporter #1 left out of the view");
		CHECK(!menu_spartan_object(object_indices[2]), "teleporter #2 left out of the view");
		spartan_delete(&menu_spartan.spartans[0]);
		CHECK(deleted_count == 0, "a teleporter deleted as the Spartan's (%d)", deleted_count);
		return 0;
	}

	/* the same map's objects made again (its reset), its tags the same:
	another object in the Spartan's slot is not the Spartan's */
	CASE("slot-reused")
	{
		ui_menu();
		objects[1].definition_index = WEAPON;
		objects[2].definition_index = WEAPON;
		CHECK(!menu_spartan_object(object_indices[1]), "another object in the Spartan's slot left out");
		CHECK(!menu_spartan_object(object_indices[2]), "another object in the rifle's slot left out");
		spartan_delete(&menu_spartan.spartans[0]);
		CHECK(deleted_count == 0, "another object deleted as the Spartan's (%d)", deleted_count);
		return 0;
	}

	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
