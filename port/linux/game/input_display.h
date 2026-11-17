/*
INPUT_DISPLAY.H

The on-screen input display (display.input_display, off by default:
input_display.c).
*/

#ifndef INPUT_DISPLAY_H
#define INPUT_DISPLAY_H

struct player_action;

/* each tick, the players' actions as the tick takes them (players.c's
players_update_before_game): the machine's controllers', or a demo's
playback's recorded ones; indexed by the players' absolute indices */
void input_display_tick(struct player_action const *actions, short count);
/* a view's panel, in the HUD (hud.c, while the HUD is shown): the view's
local player and its player (a datum index, NONE for none) */
void input_display_draw(short local_player_index, long player_index, long font_index);

#endif
