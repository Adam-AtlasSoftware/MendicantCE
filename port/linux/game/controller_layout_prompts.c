/* A prompt's extra button drawn as the symbol its controller's layout gives it
 * (#1143: port/linux/include/controller_layouts.h): the HUD's messages'
 * (hud_messaging.c) and the help screens' (ui_widget.c), as a key's cap is
 * drawn (hud_key_prompts.c's hud_key_prompt_draw_symbol). Only what this
 * machine draws. */
#include "cseries.h"
#include "controller_layouts.h"
#include "controller_glyphs.h"

boolean hud_key_prompt_draw_symbol(wchar_t const *legend, rectangle2d *bounds, rectangle2d *cursor, pixel32 color,
    unsigned long fill, unsigned long legend_color, boolean round);

boolean controller_glyph_layout_draw(short local_player_index, short icon_index, rectangle2d *bounds,
    rectangle2d *cursor, pixel32 color)
{
    struct controller_layout_control symbol;
    wchar_t legend[CONTROLLER_LAYOUT_LEGEND];
    short i;

    if (!controller_remap_prompt_symbol(local_player_index, icon_index, &symbol)) return FALSE;
    for (i = 0; symbol.legend[i] && i < CONTROLLER_LAYOUT_LEGEND - 1; i++)
        legend[i] = (wchar_t)(unsigned char)symbol.legend[i];
    legend[i] = 0;
    return hud_key_prompt_draw_symbol(legend, bounds, cursor, color, symbol.fill,
        controller_layout_legend_color(symbol.fill), symbol.shape == CONTROLLER_LAYOUT_ROUND);
}
