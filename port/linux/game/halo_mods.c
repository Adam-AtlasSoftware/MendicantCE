/*
MODS.C

The hooks of Forge's parts (halo_mod.h): each part is a struct halo_mod of
its own file, listed in halo_mod_parts below, and the dev tools' own hooks
in main/main.c and interface/interface.c (forge_update, forge_render) call
every part's first, in the list's order; game/game.c calls the tick and new
map hooks, render/render.c the world drawing hooks. The list is the same in
every build, so the menu's tabs and the order of the hooks are too.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#include "cseries.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "interface/interface.h"
#include "math/integer_math.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/text_group.h"

#include <stdio.h>

/* ---------- constants */

/* the large font leaves room for this many lines on a 480 line screen */
#define HALO_MOD_LARGE_FONT_LINES 18
#define HALO_MOD_SCREEN_HEIGHT 480

/* ---------- globals */

/* Forge's parts, in the order of their menu tabs and of their hooks */
extern struct halo_mod const forge_ai_mod;
extern struct halo_mod const forge_edit_mod;
extern struct halo_mod const forge_prefabs_mod;
extern struct halo_mod const forge_spawns_mod;
extern struct halo_mod const forge_zones_mod;
extern struct halo_mod const gravity_mod;
extern struct halo_mod const forge_layout_mod;

static struct
{
	short count;
	struct halo_mod const *mods[HALO_MOD_MAXIMUM_COUNT];
} halo_mod_globals =
{
	7,
	{
		&forge_ai_mod,
		&forge_edit_mod,
		&forge_prefabs_mod,
		&forge_spawns_mod,
		&forge_zones_mod,
		&gravity_mod,
		/* port: (no sky tab: its sky was drawn with OpenGL, which OpenH1's
		renderers are not) */
		&forge_layout_mod
	}
};

/* the fonts chosen (for so many lines) and the map they were chosen in */
#define HALO_MOD_FONT_CACHE_COUNT 4

static struct
{
	short lines;
	char map_name[256];
	long font_tag_index;
} halo_mod_font_globals[HALO_MOD_FONT_CACHE_COUNT];

/* ---------- private code */

static boolean halo_mod_font_holds_text(
	long font_tag_index)
{
	struct font_header *font = font_definition_get(font_tag_index);

	return font_get_character_by_ascii_code(font, 'a') != NULL &&
		font_get_character_by_ascii_code(font, 'Z') != NULL &&
		font_get_character_by_ascii_code(font, '0') != NULL &&
		font_get_character_by_ascii_code(font, '>') != NULL;
}

static void halo_mod_set_font(
	long font,
	int justification,
	unsigned long argb)
{
	real_argb_color color;

	color.alpha = (real)((argb >> 24) & 0xff) / 255.f;
	color.red = (real)((argb >> 16) & 0xff) / 255.f;
	color.green = (real)((argb >> 8) & 0xff) / 255.f;
	color.blue = (real)(argb & 0xff) / 255.f;
	draw_string_set_draw_mode(font, _text_style_plain, (short)justification, 0, &color);

	return;
}

/* ---------- public code */

void halo_mod_register(
	struct halo_mod const *mod)
{
	if (halo_mod_globals.count < HALO_MOD_MAXIMUM_COUNT)
	{
		halo_mod_globals.mods[halo_mod_globals.count++] = mod;
	}
	else
	{
		fprintf(stderr, "halo-linux: more than %d mods, %s does not run\n", HALO_MOD_MAXIMUM_COUNT, mod->name);
	}

	return;
}

void halo_mods_update(
	void)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->update)
			halo_mod_globals.mods[mod_index]->update();
	}

	return;
}

int halo_mods_grab(
	void)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->grab && halo_mod_globals.mods[mod_index]->grab())
			return TRUE;
	}

	return FALSE;
}

short halo_mods_count(
	void)
{
	return halo_mod_globals.count;
}

struct halo_mod const *halo_mods_get(
	short index)
{
	return index >= 0 && index < halo_mod_globals.count ? halo_mod_globals.mods[index] : NULL;
}

int halo_mods_remove(
	void)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->remove_object && halo_mod_globals.mods[mod_index]->remove_object())
			return TRUE;
	}

	return FALSE;
}

short halo_mods_menu_page_count(
	void)
{
	short mod_index;
	short count = 0;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->menu)
			count++;
	}

	return count;
}

short halo_mods_properties_page(
	struct halo_mod_menu const **popup)
{
	short mod_index;
	short page_index = 0;
	short nearest_page_index = NONE;
	float nearest_distance = 0.f;
	boolean found = FALSE;

	*popup = NULL;
	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		struct halo_mod const *mod = halo_mod_globals.mods[mod_index];
		float distance = 0.f;

		if ((mod->menu || mod->popup) && mod->properties && mod->properties(&distance) &&
			(!found || distance < nearest_distance))
		{
			found = TRUE;
			nearest_page_index = mod->menu ? page_index : NONE;
			nearest_distance = distance;
			*popup = mod->popup;
		}
		if (mod->menu)
			page_index++;
	}

	return nearest_page_index;
}

struct halo_mod_menu const *halo_mods_menu_page(
	short index)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->menu && index-- == 0)
			return halo_mod_globals.mods[mod_index]->menu;
	}

	return NULL;
}

void halo_mods_render(
	void)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->render)
			halo_mod_globals.mods[mod_index]->render();
	}

	return;
}

/* at the start of every game tick (game/game.c) */
int halo_mods_authoritative(
	void)
{
	return game_connection() == _game_connection_local || game_connection() == _game_connection_network_server;
}

void halo_mods_tick(
	void)
{
	short mod_index;

	/* a film replays what was recorded */
	if (game_connection() == _game_connection_film_playback)
		return;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->tick)
			halo_mod_globals.mods[mod_index]->tick();
	}

	return;
}

/* at the end of game_initialize_for_new_map (game/game.c) */
void halo_mods_new_map(
	void)
{
	short mod_index;

	/* (Forge's monitor is found again in this map's tags, forge_monitor.c) */
	forge_monitor_new_map();
	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->new_map)
			halo_mod_globals.mods[mod_index]->new_map();
	}
	/* then the layout that plays on this map, over what the mods set up */
	forge_layout_new_map();

	return;
}

/* in each player's window, after render_debug (render/render.c) */
void halo_mods_render_world(
	void)
{
	short mod_index;

	for (mod_index = 0; mod_index < halo_mod_globals.count; mod_index++)
	{
		if (halo_mod_globals.mods[mod_index]->render_world)
			halo_mod_globals.mods[mod_index]->render_world();
	}

	return;
}

int halo_mod_key_pressed(
	struct halo_mod_key_state *state,
	int key,
	int ctrl)
{
	int down = halo_mod_key_down(key) && (!ctrl || halo_mod_key_down(HALO_MOD_KEY_CTRL));
	int pressed = down && !state->down;

	state->down = down;

	return pressed;
}

int halo_mod_screen(
	short *x0,
	short *y0,
	short *x1,
	short *y1)
{
	rectangle2d window = render.camera.window_bounds;

	if (local_player_get_player_index(0) == NONE)
		return FALSE;
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	*x0 = window.x0;
	*y0 = window.y0;
	*x1 = window.x1;
	*y1 = window.y1;

	return TRUE;
}

long halo_mod_font(
	int large)
{
	return large
		? halo_mod_font_for_lines(HALO_MOD_LARGE_FONT_LINES)
		: interface_get_tag_index(_interface_font_terminal);
}

long halo_mod_font_for_lines(
	short lines)
{
	long terminal = interface_get_tag_index(_interface_font_terminal);
	char const *map_name;
	short cache_index;

	if (lines <= 0 || global_scenario_index == NONE || terminal == NONE)
		return terminal;

	map_name = tag_get_name(global_scenario_index);
	for (cache_index = 0; cache_index < HALO_MOD_FONT_CACHE_COUNT - 1; cache_index++)
	{
		if (halo_mod_font_globals[cache_index].lines == lines || halo_mod_font_globals[cache_index].lines == 0)
			break;
	}
	if (halo_mod_font_globals[cache_index].lines != lines ||
		strcmp(map_name, halo_mod_font_globals[cache_index].map_name) != 0)
	{
		struct tag_iterator iterator;
		long font_tag_index;
		short best_height = halo_mod_line_height(terminal);

		halo_mod_font_globals[cache_index].lines = lines;
		halo_mod_font_globals[cache_index].font_tag_index = terminal;
		tag_iterator_new(&iterator, FONT_GROUP_TAG);
		while ((font_tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			short height = halo_mod_line_height(font_tag_index);

			if (height > best_height &&
				height <= HALO_MOD_SCREEN_HEIGHT / lines &&
				halo_mod_font_holds_text(font_tag_index))
			{
				best_height = height;
				halo_mod_font_globals[cache_index].font_tag_index = font_tag_index;
			}
		}
		_snprintf(halo_mod_font_globals[cache_index].map_name, sizeof(halo_mod_font_globals[cache_index].map_name),
			"%s", map_name);
	}

	return halo_mod_font_globals[cache_index].font_tag_index;
}

short halo_mod_line_height(
	long font)
{
	struct font_header *header = font_definition_get(font);

	return (short)(header->ascending_height + header->descending_height + header->leading_height);
}

short halo_mod_text_width(
	long font,
	char const *text)
{
	rectangle2d bounds;
	rectangle2d text_bounds;
	rectangle2d cursor_bounds;

	set_rectangle2d(&bounds, 0, 0, SHORT_MAX, SHORT_MAX);
	halo_mod_set_font(font, HALO_MOD_TEXT_LEFT, 0xffffffff);
	draw_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);

	return (short)(text_bounds.x1 - text_bounds.x0);
}

void halo_mod_draw_text(
	long font,
	short x0,
	short y0,
	short x1,
	short y1,
	int justification,
	unsigned long argb,
	char const *text)
{
	rectangle2d bounds;

	set_rectangle2d(&bounds, x0, y0, x1, y1);
	halo_mod_set_font(font, justification, argb);
	rasterizer_draw_string(&bounds, NULL, NULL, 0, text);

	return;
}

void halo_mod_draw_box(
	short x0,
	short y0,
	short x1,
	short y1,
	unsigned long argb)
{
	rectangle2d bounds;

	set_rectangle2d(&bounds, x0, y0, x1, y1);
	draw_quad(&bounds, (pixel32)argb);

	return;
}

/* as draw_quad (cutscene/cinematics.c), a shape of four corners, clockwise,
between the screen's units: the screen is drawn with several of the display's
pixels to a unit, so thin shapes can be as sharp as the display */
static void halo_mod_draw_corners(
	float const x[4],
	float const y[4],
	unsigned long argb)
{
	struct game_globals *game_globals = scenario_get_game_globals();
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	struct dynamic_screen_vertex vertices[4];
	short vertex_index;

	if (game_globals->rasterizer_data.count <= 0)
		return;

	for (vertex_index = 0; vertex_index < NUMBEROF(vertices); vertex_index++)
	{
		vertices[vertex_index].position.x = x[vertex_index];
		vertices[vertex_index].position.y = y[vertex_index];
		vertices[vertex_index].color = (pixel32)argb;
		vertices[vertex_index].texture_coordinates.x = 0.f;
		vertices[vertex_index].texture_coordinates.y = 0.f;
	}

	csmemset(&parameters, 0, sizeof(parameters));
	parameters.framebuffer_blend_function = 0;
	parameters.map_texture_scale[0].i = 1.f;
	parameters.map_texture_scale[0].j = 1.f;
	parameters.map_scale[0].i = 1.f;
	parameters.map_scale[0].j = 1.f;
	parameters.meter_parameters = NULL;
	parameters.point_sampled = FALSE;
	parameters.map[0] = TAG_BLOCK_GET_ELEMENT(
		&bitmap_group_get(TAG_BLOCK_GET_ELEMENT(&game_globals->rasterizer_data, 0,
			struct game_globals_rasterizer_data)->default_textures[0].index)->bitmaps,
		1,
		struct bitmap_data);

	rasterizer_globals.current_lock_operation = _rasterizer_lock_cinematics;
	rasterizer_psuedo_dynamic_screen_quad_draw(&parameters, vertices);
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;

	return;
}

void halo_mod_draw_box_real(
	float x0,
	float y0,
	float x1,
	float y1,
	unsigned long argb)
{
	float x[4];
	float y[4];

	/* clockwise from the top left */
	x[0] = x0;
	x[1] = x1;
	x[2] = x1;
	x[3] = x0;
	y[0] = y0;
	y[1] = y0;
	y[2] = y1;
	y[3] = y1;
	halo_mod_draw_corners(x, y, argb);

	return;
}

/* a quad whose last two corners are one */
void halo_mod_draw_triangle_real(
	float x0,
	float y0,
	float x1,
	float y1,
	float x2,
	float y2,
	unsigned long argb)
{
	float x[4];
	float y[4];

	x[0] = x0;
	x[1] = x1;
	x[2] = x2;
	x[3] = x2;
	y[0] = y0;
	y[1] = y1;
	y[2] = y2;
	y[3] = y2;
	halo_mod_draw_corners(x, y, argb);

	return;
}
