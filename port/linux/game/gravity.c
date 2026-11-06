/*
GRAVITY.C

One of Forge's parts (halo_mod.h, halo_mods.c): the gravity of the whole map.

The dev tools' menu (1 / D-pad right) has a "Gravity" tab with one row:
left and right change the gravity (normal, low, very low, moon, high, very
high), so it works on a controller as it does on a keyboard. While it is
not normal, the top right of the screen says which it is.

The game has one gravity for everything, global_gravity
(physics/physics.c): rigid bodies (scaled by their physics tag's gravity
scale), bipeds, vehicles and point physics all fall by it. This mod sets it
to the chosen fraction of the game's own at the start of every tick (the
tick hook, halo_mod.h), which runs only in local games; each new map starts
from the game's own gravity (the new map hook), so a system link game
always has it. The choice itself is kept from map to map.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/terminal.h"
#include "math/real_math.h"
#include "physics/physics.h"

#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	GRAVITY_LOCAL_PLAYER_INDEX = 0,
	GRAVITY_LABEL_MARGIN = 4
};

/* 0xAARRGGBB */
#define GRAVITY_LABEL_COLOR 0xffffff80UL
#define GRAVITY_LABEL_BACKGROUND_COLOR 0x90000000UL

/* ---------- structures */

struct gravity_choice
{
	real scale;
	char const *name;
};

/* ---------- globals */

static struct gravity_choice const gravity_choices[] =
{
	{ 1.f, "normal" },
	{ 0.5f, "low" },
	{ 0.25f, "very low" },
	{ 0.17f, "moon" },
	{ 1.5f, "high" },
	{ 2.f, "very high" }
};

#define NUMBER_OF_GRAVITY_CHOICES ((short)(sizeof(gravity_choices) / sizeof(gravity_choices[0])))

static struct
{
	/* the game's own gravity, read before anything changed it */
	boolean default_read;
	real default_gravity;
	short choice_index;

	/* this frame's label, for the render hook */
	boolean labelled;
} gravity_globals = { { 0 } };

/* ---------- private code */

static void gravity_read_default(
	void)
{
	if (!gravity_globals.default_read)
	{
		gravity_globals.default_gravity = global_gravity;
		gravity_globals.default_read = TRUE;
	}

	return;
}

static void gravity_update(
	void)
{
	gravity_globals.labelled = local_player_get_player_index(GRAVITY_LOCAL_PLAYER_INDEX) != NONE &&
		gravity_globals.choice_index != 0 &&
		game_connection() != _game_connection_film_playback;

	return;
}

static void gravity_render(
	void)
{
	short x0;
	short y0;
	short x1;
	short y1;
	long font;

	if (gravity_globals.labelled &&
		halo_mod_screen(&x0, &y0, &x1, &y1) &&
		(font = halo_mod_font(FALSE)) != NONE)
	{
		struct gravity_choice const *choice = &gravity_choices[gravity_globals.choice_index];
		char label[64];
		short line_height = halo_mod_line_height(font);
		short width;
		short top = (short)(y0 + 2 * GRAVITY_LABEL_MARGIN);
		short right = (short)(x1 - 2 * GRAVITY_LABEL_MARGIN);

		_snprintf(label, sizeof(label), "gravity: %s (%d%%)",
			choice->name, (int)(choice->scale * 100.f + 0.5f));
		width = (short)(halo_mod_text_width(font, label) + 2 * GRAVITY_LABEL_MARGIN);

		halo_mod_draw_box(
			(short)(right - width),
			(short)(top - GRAVITY_LABEL_MARGIN),
			right,
			(short)(top + line_height + GRAVITY_LABEL_MARGIN),
			GRAVITY_LABEL_BACKGROUND_COLOR);
		halo_mod_draw_text(font, (short)(right - width), top, (short)(right - GRAVITY_LABEL_MARGIN),
			(short)(top + line_height), HALO_MOD_TEXT_RIGHT, GRAVITY_LABEL_COLOR, label);
	}

	/* drawn only after an update: nothing is left over when the game stops
	updating the mods (the console, menus, loading) */
	gravity_globals.labelled = FALSE;

	return;
}

/* local and system link games (halo_mods_tick): every machine's physics */
static void gravity_tick(
	void)
{
	gravity_read_default();
	global_gravity = gravity_globals.default_gravity * gravity_choices[gravity_globals.choice_index].scale;

	return;
}

static void gravity_new_map(
	void)
{
	gravity_read_default();
	global_gravity = gravity_globals.default_gravity;
	/* a system link game has the gravity of its layout (every machine
	alike), or the normal one; port: and a game of another game type than
	Forge the game's own, whatever a Forge game chose before (a layout's
	gravity, chosen in the map list, comes after: forge_layout.c) */
	if (game_connection() != _game_connection_local || !game_variant_set_is_forge())
		gravity_globals.choice_index = 0;

	return;
}

/* ---------- the tools' menu page */

static short gravity_menu_row_count(
	void)
{
	return 1;
}

static void gravity_menu_row_text(
	short row,
	char *label,
	unsigned long label_size,
	char *value,
	unsigned long value_size)
{
	struct gravity_choice const *choice = &gravity_choices[gravity_globals.choice_index];

	_snprintf(label, label_size, "Gravity of the whole map");
	_snprintf(value, value_size, "%s (%d%%)", choice->name, (int)(choice->scale * 100.f + 0.5f));

	return;
}

static int gravity_menu_row_change(
	short row,
	int direction)
{
	if (direction != 0)
	{
		if (!halo_mods_authoritative())
		{
			/* (its layout takes it to its clients, forge_layout.c) */
			terminal_printf(global_real_argb_orange, "gravity: the gravity is the host's");
		}
		else
		{
			short step = direction < 0 ? NUMBER_OF_GRAVITY_CHOICES - 1 : 1;

			gravity_globals.choice_index = (short)((gravity_globals.choice_index + step) % NUMBER_OF_GRAVITY_CHOICES);
		}
	}

	return FALSE;
}

static struct halo_mod_menu const gravity_menu =
{
	"Gravity",
	gravity_menu_row_count,
	gravity_menu_row_text,
	gravity_menu_row_change,
	NULL
};

/* ---------- layouts (port/linux/game/forge_layout.c) */

static void gravity_layout_save(
	struct halo_layout_writer *writer)
{
	halo_layout_printf(writer, "gravity %s", gravity_choices[gravity_globals.choice_index].name);

	return;
}

static void gravity_layout_clear(
	void)
{
	gravity_globals.choice_index = 0;

	return;
}

static void gravity_layout_load(
	char const *line)
{
	short index;

	if (strncmp(line, "gravity ", 8) == 0)
	{
		for (index = 0; index < NUMBER_OF_GRAVITY_CHOICES; index++)
		{
			if (strcmp(line + 8, gravity_choices[index].name) == 0)
				gravity_globals.choice_index = index;
		}
	}

	return;
}

/* ---------- the mod */

struct halo_mod const gravity_mod =
{
	"gravity",
	gravity_update,
	gravity_render,
	gravity_tick,
	gravity_new_map,
	NULL,
	&gravity_menu,
	NULL,
	NULL,
	gravity_layout_save,
	gravity_layout_clear,
	gravity_layout_load
};
