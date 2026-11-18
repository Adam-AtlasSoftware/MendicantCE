/*
PERF_OVERLAY.H

The performance overlay at the screen's bottom right (display.perf_overlay:
perf_overlay.c), drawn by main.c's main_framerate_render.
*/

#ifndef __PERF_OVERLAY_H
#define __PERF_OVERLAY_H

enum
{
	_perf_overlay_off = 0,
	/* the frame rate's counter */
	_perf_overlay_simple,
	/* ... and the frame's times */
	_perf_overlay_detailed
};

enum
{
	/* "detailed"'s cells, two to a row: the frame's times (6); with
	debug.timings the busiest CPU sections, the counts and the GPU's passes
	too, each group from a row of its own */
	PERF_OVERLAY_CELLS = 20,
	PERF_OVERLAY_COLUMNS = 2,
	PERF_OVERLAY_LABEL_SIZE = 20,
	PERF_OVERLAY_VALUE_SIZE = 16,
	/* (a GPU pass group's label as the backend logs it) */
	PERF_OVERLAY_LINE_SIZE = 32,
	PERF_OVERLAY_CPU_SECTIONS = 4,
	PERF_OVERLAY_GPU_GROUPS = 4
};

/* a cell: its label at its left, its value at its right (an empty label
and value: a group's last row's empty half) */
struct perf_overlay_cell
{
	char label[PERF_OVERLAY_LABEL_SIZE];
	char value[PERF_OVERLAY_VALUE_SIZE];
};

/* where and how large the cells are drawn in a view (perf_overlay_place):
the text's scale about the anchor (the box's bottom right, in the view's
units), the rows that fit (from the top), and the box's size and a row's
height before the scale */
struct perf_overlay_place
{
	real scale;
	short anchor_x, anchor_y;
	short rows;
	short width, height, row_height, cell_width, column_gap;
};

/* the setting's mode (_perf_overlay_*), read again when it changes */
short perf_overlay_mode(void);
/* "detailed"'s cells, top to bottom and left to right, above the counter:
their count */
short perf_overlay_cells(struct perf_overlay_cell cells[PERF_OVERLAY_CELLS]);
/* debug.timings (off by default), read again when it changes */
int perf_overlay_timings(void);

/* a CPU section's name (profile_enter's, PROFILE_SECTION's) as the overlay
shows it: a short label in capitals, the developer console's name kept */
void perf_overlay_section_label(char const *name, char *label, short size);
/* milliseconds as every cell shows them ("--" for none measured) */
void perf_overlay_milliseconds(real milliseconds, char *value, short size);
/* the cells' place in a view of these bounds (the view's units, 480 to
the screen's height), at pixels display pixels a unit, a split screen's
or not: cells of cell_width units widest (label, gap and value) and their
font's capitals capital_height units tall */
void perf_overlay_place(short x0, short y0, short x1, short y1, real pixels, boolean split, short cells,
	short cell_width, short capital_height, struct perf_overlay_place *place);

#endif
