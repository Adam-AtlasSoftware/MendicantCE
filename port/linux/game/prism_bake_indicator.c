/*
PRISM_BAKE_INDICATOR.C

The in-game baker's indicator (docs/prism_bake.md; prism_bake.c): while a
level's light is baked in the background (display.lighting "raster",
display.bake), "Baking lightmaps 37%" at the screen's top left, and "Lightmaps
baked" for a few seconds once the BSP's bake is done; a paused bake ("ask",
not asked) says so. Off with display.bake_indicator false, and in the runs
a camera path drives (the quality gates' captures: nothing over their
frames).
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "interface/interface.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "../src/xgpu_stream.h"

#include <stdio.h>
#include <string.h>

/* port_config.c's, cseries_windows.c's; prism_bake.c's */
int config_boolean(const char *name);
char const *config_string(char const *name);
unsigned long config_changes(void);
unsigned long system_milliseconds(void);
boolean prism_bake_wanted(void);
unsigned long prism_bake_status(int word);
float prism_bake_status_real(int word);

/* its top left (the HUD's 480 lines), how long "baked" stays */
#define INDICATOR_LEFT 12
#define INDICATOR_TOP_FRACTION 0.06f
#define INDICATOR_DONE_MILLISECONDS 5000ul

static struct
{
	unsigned long changes;
	boolean enabled;
	unsigned long state, done_at;
	unsigned long bsp;
} indicator = { (unsigned long)-1 };

void prism_bake_indicator_draw(
	void)
{
	long font_index;
	unsigned long state, now = system_milliseconds();
	char text[64];
	real_argb_color color;
	rectangle2d bounds;
	struct font_header *font;
	short line_height;

	if (indicator.changes != config_changes())
	{
		char const *path = config_string("debug.camera_path");

		indicator.changes = config_changes();
		indicator.enabled = config_boolean("display.bake_indicator") && !(path && *path);
	}
	if (!indicator.enabled || !prism_bake_wanted())
		return;
	state = prism_bake_status(XGPU_BAKE_STATUS_STATE);
	if (state != indicator.state || prism_bake_status(XGPU_BAKE_STATUS_BSP) != indicator.bsp)
	{
		if (state == XGPU_BAKE_STATE_DONE && indicator.state == XGPU_BAKE_STATE_BAKING)
			indicator.done_at = now;
		indicator.state = state;
		indicator.bsp = prism_bake_status(XGPU_BAKE_STATUS_BSP);
	}
	color.alpha = 0.85f;
	color.red = 0.75f;
	color.green = 0.85f;
	color.blue = 1.0f;
	if (state == XGPU_BAKE_STATE_BAKING)
		snprintf(text, sizeof(text), "Baking lightmaps %d%%",
			(int)(prism_bake_status_real(XGPU_BAKE_STATUS_PROGRESS) * 100.0f));
	else if (state == XGPU_BAKE_STATE_PAUSED)
		snprintf(text, sizeof(text), "Lightmaps not baked yet");
	else if (state == XGPU_BAKE_STATE_DONE && indicator.done_at && now - indicator.done_at < INDICATOR_DONE_MILLISECONDS)
		snprintf(text, sizeof(text), "Lightmaps baked");
	else
		return;
	font_index = interface_get_tag_index(_interface_font_terminal);
	if (font_index == NONE)
		return;
	font = font_definition_get(font_index);
	line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	if (line_height <= 0)
		return;
	bounds = render.camera.window_bounds;
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	bounds.y0 = (short)(bounds.y0 + (bounds.y1 - bounds.y0) * INDICATOR_TOP_FRACTION);
	bounds.y1 = (short)(bounds.y0 + line_height);
	bounds.x0 = (short)(bounds.x0 + INDICATOR_LEFT);
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &color);
	rasterizer_draw_string(&bounds, NULL, NULL, 0, text);
}
