/*
BENCHMARK.H

The built-in benchmark (benchmark.c): a fixed minute of Blood Gulch with
bots, measured and written to benchmarks/, its results shown.
*/

#ifndef __BENCHMARK_H
#define __BENCHMARK_H

struct menu_table_source;

/* what a match's guard looks at (benchmark_live_problem): the real seconds
since the match began, the game's ticks since then, the players in it, the
bots of them, and the kills (since the measure began, at its end) */
struct benchmark_live
{
	double real_seconds;
	long ticks;
	long players;
	long bots;
	long kills;
};

/* why the match is not one to measure, NULL if it is: frozen (its game
time standing still, or nearly), always; with measure, also a match with
no player, no bots or no kills */
char const *benchmark_live_problem(struct benchmark_live const *live, int measure);

/* Settings' RUN BENCHMARK (menu_functions.c): begun at the next frame of
the main menu; pacing "uncapped" or "settings" (NULL: display.benchmark_pacing) */
void benchmark_request(char const *pacing);
/* each frame (main.c): begun when asked (or by halo --benchmark), measured,
and its results written and shown */
void benchmark_frame(int main_menu_loaded);
/* the results screen's (port/assets/menus/screens/benchmark.xml, the new
menus'): its table's rows, the source "benchmark.results" (menu_table.h),
and its bindings' values ("benchmark.<name>", UTF-8: nonzero if it has one) */
extern struct menu_table_source const benchmark_results_source;
int benchmark_value(char const *name, char *value, unsigned long size);

#endif
