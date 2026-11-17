#ifndef CONTROLLER_LAYOUTS_H
#define CONTROLLER_LAYOUTS_H

/* Controller layouts (#1143): what a player says of a controller's extra
 * buttons (its back paddles, touchpad and misc buttons, which the HUD has no
 * pictures of), each a name and a symbol (a legend of 1 to 3 characters in a
 * round or square cap of a color), kept by the controller's SDL GUID in a
 * small TOML file of the data folder's controller_layouts. Button
 * Remapping's prompts then draw the symbol in place of words like "P3".
 * Nothing changes for a controller without one. docs/controller_remapping.md,
 * "Controller layouts". */

#include <stddef.h>
#include "controller_remap.h"

struct controller_device;

#define CONTROLLER_LAYOUT_GUID 33
#define CONTROLLER_LAYOUT_NAME 21
#define CONTROLLER_LAYOUT_LEGEND 4
#define CONTROLLER_LAYOUT_TITLE 32
/* (a file larger than this is refused unread) */
#define CONTROLLER_LAYOUT_FILE_MAXIMUM 16384
/* (the layouts the folder holds that are read: one each controller) */
#define CONTROLLER_LAYOUT_MAXIMUM 32
#define CONTROLLER_LAYOUT_PROBLEM 192
#define CONTROLLER_LAYOUT_COLORS 8
enum
{
    CONTROLLER_LAYOUT_ROUND,
    CONTROLLER_LAYOUT_SQUARE,
    CONTROLLER_LAYOUT_SHAPES
};

/* an extra button described: its name (empty: not described, its family's
 * words as before), its legend, its cap's shape and fill (0xRRGGBB) */
struct controller_layout_control
{
    char name[CONTROLLER_LAYOUT_NAME];
    char legend[CONTROLLER_LAYOUT_LEGEND];
    unsigned char shape;
    unsigned long fill;
};

/* a controller's layout: its GUID, its name, each extra button's (by
 * CONTROLLER_REMAP_PADDLE's order: the paddles, the touchpad, misc1 to
 * misc6), and (#1144: an exported file's, where its player chose to include
 * them) Button Remapping's bindings, bound's bit i for action i given */
struct controller_layout
{
    char guid[CONTROLLER_LAYOUT_GUID];
    char name[CONTROLLER_LAYOUT_TITLE];
    struct controller_layout_control controls[CONTROLLER_REMAP_EXTRAS];
    unsigned long bound;
    unsigned char bindings[CONTROLLER_REMAP_ACTIONS];
};

struct controller_layout_color
{
    char const *name;
    unsigned long rgb;
};
/* the fills offered (a file may give any other) */
extern struct controller_layout_color const controller_layout_colors[CONTROLLER_LAYOUT_COLORS];

/* ---------- the file (no state) */

/* text read as a layout: TRUE, or FALSE with what is wrong (its line) in
 * problem, the layout then unchanged. The whole file is refused on any error */
int controller_layout_parse(char const *text, size_t length, struct controller_layout *layout, char *problem,
    size_t size);
/* a layout written as its file (its [bindings] where it has them): its
 * length, 0 where size is too small */
size_t controller_layout_format(struct controller_layout const *layout, char *text, size_t size);
/* the legend's color on a fill: black or white, whichever reads better
 * (WCAG's contrast ratio), 0xRRGGBB */
unsigned long controller_layout_legend_color(unsigned long fill);
char const *controller_layout_shape_name(int shape);
/* an action's key in a file's [bindings] ("jump", "push_to_talk"), NULL */
char const *controller_layout_action_name(int action);
/* (#1144) how many of a layout's bindings differ from a player's buttons
 * once applied through Button Remapping's assign rule (controller_remap_assign:
 * a button another action has is taken from it), which buttons gets */
int controller_layout_bindings_apply(struct controller_layout const *layout, unsigned char *buttons);
/* an extra button (0 to CONTROLLER_REMAP_EXTRAS - 1) described: a new
 * control's words from its name on the controller ("P3", "LEFT BACK 2"):
 * the name, a legend of it (its words' initials and digits), round, grey */
void controller_layout_describe(struct controller_layout_control *control, char const *name);
/* a name or legend as a file may hold it: its characters that may be
 * (ASCII; no quote or backslash; a legend no space) */
void controller_layout_clean(char *text, size_t size, int legend);

/* ---------- the folder (the data folder's controller_layouts) */

/* the folder read again: each file's layout by its GUID; a file of another
 * name (copied in: an import) taken as its controller's (written as
 * <guid>.toml, the copy removed); a refused file left as it is, logged, and
 * its problem kept (controller_layouts_problem). Read once by itself */
void controller_layouts_scan(void);
/* the layout of the controller of this GUID, NULL for none */
struct controller_layout const *controller_layout_for(char const *guid);
/* a layout kept (written as <guid>.toml): FALSE where it cannot be */
int controller_layout_store(struct controller_layout const *layout);
/* (#1144) a layout written for sharing, with the player's buttons as its
 * [bindings] (NULL: none), into the folder's shared folder, named for it
 * (the file's path in written): FALSE where it cannot be */
int controller_layout_export(struct controller_layout const *layout, unsigned char const *buttons, char *written,
    size_t size);
/* the last refused file's problem ("name.toml: line 4: ..."), "" for none */
char const *controller_layouts_problem(void);
/* moves each time the folder is read or a layout kept */
unsigned long controller_layouts_changes(void);

/* a button's name on a controller: its layout's for an extra button it
 * describes, else controller_remap_device_button_name's */
char const *controller_layout_button_name(int button, struct controller_device const *device);

#endif
