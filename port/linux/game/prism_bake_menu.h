/*
PRISM_BAKE_MENU.H

PRISM's light bake on the main menu (prism_bake_menu.c, docs/prism_bake.md).
*/

#ifndef PRISM_BAKE_MENU_H
#define PRISM_BAKE_MENU_H

/* the settings the bake holds, as the note of the BSPs baked keeps them */
void prism_bake_key(char *key, unsigned long size);
/* whether a change of the setting bakes every level again */
boolean prism_bake_key_setting(char const *name);
/* the main menu's bake runs (a PRISM lighting, a card that traces rays,
display.bake "auto") */
boolean prism_bake_menu_wanted(void);
/* debug.bake_worker's map (this game is the main menu's worker), or NULL */
char const *prism_bake_worker_map(void);
/* the worker's GPU time a frame, 0 for a game */
float prism_bake_worker_budget(void);

/* between frames (d3d8_device.c), the main menu drawn (menu_functions.c),
over the main menu (interface.c), the main menu's frame cap
(sdl_platform.c: 0 none) */
void prism_bake_menu_frame(void);
void prism_bake_menu_main_shown(void);
void prism_bake_menu_draw(void);
int prism_bake_menu_frame_cap(void);

/* the questions' (menu_functions.c): a level not baked yet, WAIT's level
baked first, a level started, REBAKE ALL, Light Bake's row of a level */
boolean prism_bake_menu_level_unbaked(char const *map_name);
void prism_bake_menu_first(char const *map_name);
void prism_bake_menu_level_started(char const *map_name);
void prism_bake_menu_rebake_all(void);
boolean prism_bake_menu_level_words(char const *map, char *text, unsigned long size);
/* the level the question is of, and the questions' bindings ("bake.level",
"bake.left") */
void prism_bake_menu_asked(char const *map_name);
int prism_bake_menu_value(char const *name, char *value, unsigned long size);

#endif
