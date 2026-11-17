/*
PRACTICE.H

Practice keys for runners and the PRACTICE mark (practice.c).
*/

#ifndef PRACTICE_H
#define PRACTICE_H

/* whether the mark is on: a practice key, a cheat, game_speed, a core's
load or a skipped cutscene used since the level's fresh start (in a demo's
playback, the recording's mark) */
boolean practice_marked(void);
/* why (the first use since the fresh start): "practice key", "cheat",
"game_speed", "core_load", "skip_cinematics"; "" with the mark off */
char const *practice_reason(void);

/* a line the console (or init.txt, the telnet console, a cheat's buttons)
evaluates (hs.c's hs_compile_and_evaluate): the mark set if it uses a tool */
void practice_console_line(char const *line);
/* a level's fresh start (game.c's game_initialize_for_new_map: a map
loaded or started over): the mark off */
void practice_new_level(void);
/* each frame (main.c): the practice keys (game.practice_keys), and the
cheats and settings that set the mark while they are on */
void practice_frame(void);
/* in a demo's playback, the recording's mark as it changed (demo.c's Q) */
void practice_playback_mark(boolean marked, char const *reason);
/* the mark drawn in a view (hud.c), whatever Hide HUD hides */
void practice_draw(void);

#endif
