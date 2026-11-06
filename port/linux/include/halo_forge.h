/*
HALO_FORGE.H

The in-game dev tools of the native ports, on in local and system link
games of the Forge game type (a slayer variant among the default game
types) (port/linux/game/forge.c). Shared by the game, through
halo_linux_source_fixups.h, and the platform layer, which reads their keys
(port/linux/src/xinput_sdl.c).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __HALO_FORGE_H
#define __HALO_FORGE_H

#include <stddef.h>

/* keyboard, mouse / first gamepad */
struct halo_linux_forge_keys
{
	int toggle_flying; /* home / D-pad up */
	int menu; /* 1 / D-pad right, only while flying */
	int up; /* R / right shoulder (RB) */
	int down; /* F / left shoulder (LB) */
	int fast; /* shift / left trigger (LT) */
	/* the keyboard's move keys ([controls], W A S D), which in the game are
	actions rather than controller 1's left stick: -1 to 1, forward and right
	positive */
	int move_forward;
	int move_right;
	int faster; /* while flying, the speed: arrow up and down */
	int slower;
	int menu_up; /* arrows / D-pad */
	int menu_down;
	int menu_left;
	int menu_right;
	int menu_select; /* enter, left button / A */
	int menu_close; /* escape, backspace / B */
	/* the right mouse button: closes the menu, and held while flying the
	mouse turns what is held */
	int mouse_right;
	/* the left mouse button (menu_select too): held in the menu, the wheel
	changes a row's value */
	int mouse_left;
	int grab; /* F4 */
	int rotation_axis; /* T */
	int rotation_snap; /* V */
	int remove_object; /* delete / back */
	int tab_previous; /* page up / left shoulder (LB) */
	int tab_next; /* page down / right shoulder (RB) */
	int pad_menu; /* X: the menu while flying; holding, the object's popup, else the step */
	int pad_remove; /* Y: remove the object under the crosshair or held */
	int pad_turn; /* right trigger (RT) held: the right stick turns what is held */
	/* the left trigger (LT) alone (fast is shift too): while a line is typed,
	it moves the cursor left, as RT (pad_turn) moves it right */
	int pad_left_trigger;
	/* A, B and start of a gamepad or the touch controls alone (menu_select is
	enter too, menu_close escape and backspace): while a line is typed, A
	presses the on-screen keyboard's chosen key, B leaves the line and start
	takes it */
	int pad_select;
	int pad_cancel;
	int pad_start;
	float pad_look_x; /* the right stick, -1 to 1, right and up positive */
	float pad_look_y;
};
/* TRUE while the tools are on: a local or system link game of the Forge
game type (GAME_VARIANT_FORGE_FLAG); for the platform layer and for mods */
int forge_mode_on(void);
/* the keys as held now; FALSE, with nothing held, unless the tools are on
and the console is closed */
int halo_linux_forge_read_keys(struct halo_linux_forge_keys *keys);
/* the notches the mouse's wheel has turned since this was last asked (up
positive); 0 while the mouse is released */
int halo_linux_forge_read_wheel(void);
/* while TRUE (the menu is open, or an object is being placed), the menu's
keys and buttons stop driving controller 1 */
void halo_linux_forge_capture_menu_keys(int capture);
/* while TRUE (the camera flies), the D-pad's up and the shoulder buttons
stop driving controller 1 */
void halo_linux_forge_set_flying(int flying);
/* while TRUE (the free camera flies: not the monitor, whose look is the
player's), the touch controls' swipe and gyroscope turn the camera, read with
halo_linux_forge_touch_look (radians, as halo_linux_mouse_look's), and not
the player */
void halo_linux_forge_set_free_camera(int free_camera);
int halo_linux_forge_touch_look(float *yaw, float *pitch);
/* while TRUE (a name is being typed, forge_text_entry_begin), the keyboard
drives neither controller 1 nor the mods' keys (halo_mod_key_down);
halo_linux_forge_read_text_keys still reads it: whether each of the first
count keys is held, by SDL scancode, all at once (nothing held while the
console is open) */
void halo_linux_forge_capture_text(int capture);
void halo_linux_forge_read_text_keys(unsigned char *keys, int count);
void forge_update(void);
void forge_render(void);

/* for the flying camera (camera/director.c), about local player 0: TRUE
while the menu is open (the rise and sink keys are then the menu's); TRUE
while the right stick turns a held object rather than looking; above 0,
the distance the right stick orbits the camera at around a held object;
forge_camera_zoom takes a step towards the held object (negative away)
while it is turned, and returns the step allowed by the nearest and
farthest it may be; and, after each update of the flying camera, where it
is and looks */
int forge_menu_is_open(void);
int forge_camera_turning(void);
/* the mouse's look while the right button turns a held object, radians */
void forge_camera_turn_held(float yaw, float pitch);
float forge_camera_orbit_distance(void);
float forge_camera_zoom(float step);
void forge_flying_camera_moved(float const position[3], float const forward[3]);

/* asks for a line of text, typed on the keyboard over the tools' menu:
letters, digits, space and a few marks; backspace deletes, enter (or A)
takes it and calls done with it, escape (or B) leaves it. initial is what
the line starts as, maximum_length the most characters it may have. FALSE
when a line is being typed already. */
int forge_text_entry_begin(char const *prompt, char const *initial, unsigned long maximum_length,
	void (*done)(char const *text));

/* layouts (port/linux/game/forge_layout.c): map variants, kept as .mvar
files in the forge_maps folder beside custom_maps */
/* the name of a map's folder of u:\forge, where its prefabs are kept (and
its layouts were, before they were map variants): the last part of its
scenario's name, without what would make it another folder */
void forge_map_folder_name(char const *scenario_name, char *name, unsigned long size);

/* a new map, after the mods' new map hooks: notes the map's own objects,
then, in a local game, loads the layout chosen to play on the map */
void forge_layout_new_map(void);
/* an object the tools or a mod made (spawned, pasted, brought back), kept
by the layout */
void forge_layout_note_spawned(long object_index);
/* an object the tools put down (a map's own one is saved as moved, any
other as made), and one they are about to remove (a map's own one is saved
as removed): call before object_delete */
void forge_layout_note_placed(long object_index);
void forge_layout_note_removed(long object_index);
/* the objects the tools and the mods made that the layout keeps, and the
most it keeps: one made past that is not saved with it (the spawn menu makes
none then, and says so) */
void forge_layout_object_budget(short *used, short *maximum);

/* what a variant keeps about each of its objects besides its tag and
place (docs/forge_mvar.md): an id of its own in the variant (given when it
is made, or when one of the map's own is first saved); its team (NONE, -1,
none, else 0 to 15); its spawn flags (placed at start: made when the
variant loads; with the tools off, one that is not, or is not of the game
type, is not made); the seconds it respawns after (0 the game type's; kept,
not used yet); the game types it is in (a bit for each, 1 << game_engine_ctf
and so on, 0 every one); and the editor's notes, which a system link host
does not send: a group (0 none), whether it is locked, a comment */
enum
{
	FORGE_OBJECT_SPAWN_AT_START = 0x01,
	FORGE_OBJECT_SPAWN_NEVER_RESPAWN = 0x02,
	FORGE_OBJECT_MAXIMUM_RESPAWN_SECONDS = 600,
	FORGE_OBJECT_MAXIMUM_GROUP = 99,
	FORGE_OBJECT_COMMENT_SIZE = 64
};
struct forge_object_properties
{
	unsigned long id;
	short team;
	short spawn_flags;
	short respawn_seconds;
	unsigned short game_types;
	short group;
	short locked;
	char comment[FORGE_OBJECT_COMMENT_SIZE];
};
/* the object's properties (FALSE, with a new object's, when the layout
does not keep it: a player's, an object of the game's); setting them is the
game's machine's, and keeps the object's id (FALSE when they were not set) */
int forge_layout_get_properties(long object_index, struct forge_object_properties *properties);
int forge_layout_set_properties(long object_index, struct forge_object_properties const *properties);

/* variants shown as maps of their own in the multiplayer map list of local
games (the "Show in the map list" row): after the game's 13 maps, with the
picture of the map they are made on (its index in that list, 0 to 12) and
the variant's name. forge_custom_maps_refresh reads them again and returns
how many there are; forge_custom_map_select chooses the one to play the
next game with (NONE: none, a map of the game), and forge_custom_map_selected
is that one's index, or NONE. */
short forge_custom_maps_refresh(void);
short forge_custom_map_base_index(short index);
char const *forge_custom_map_base_name(short index);
wchar_t const *forge_custom_map_title(short index);
/* what the layout's "description" line says, or NULL when it has none */
wchar_t const *forge_custom_map_description(short index);
void forge_custom_map_select(short index);
short forge_custom_map_selected(void);
/* whether the game hosted plays a Forge layout, chosen as a map or loaded in
it, which marks its advertisement (port: forge_layout.c) */
int forge_layout_hosting(void);
/* system link (port/linux/game/network_distributed.c): the host sends the
layout of the map, as a variant, to a client that has loaded it, and again
to every client when the tools or a mod have changed it; the client applies
what the host's objects do not bring (scenery, devices, the mods' parts),
and saves nothing. While it does, forge_layout_loading_for_client is TRUE,
and mods leave out what is the host's (forge_ai's characters); while the
host writes that variant,
forge_layout_saving_for_client is TRUE, and mods leave the same out of
what they save (it only moves about, and would be sent over and over). */
void forge_layout_send_to_client(long machine_index);
void forge_layout_handle_message(void const *payload, unsigned long size);
int forge_layout_loading_for_client(void);
int forge_layout_saving_for_client(void);
/* a client's tools change nothing themselves: they ask the host, which
spawns an object of the definition at position, or moves or removes its
object (object_index when the client has the host's object at that index,
else the one of the definition nearest original). FALSE when this machine
is not a client. The host's answer is its objects and its layout. */
enum
{
	_forge_edit_spawn = 0,
	_forge_edit_move,
	_forge_edit_remove,
	/* the client's player becomes the monitor, or gets its body back
	(forge_monitor.c); nothing else of the message is read */
	_forge_edit_monitor_enter,
	_forge_edit_monitor_leave
};
int forge_layout_client_edit(int kind, long object_index, long definition_index, float const original[3],
	float const position[3], float const forward[3], float const up[3]);
void forge_layout_handle_edit(long machine_index, void const *payload, unsigned long size);

/* the builder as the monitor (port/linux/game/forge_monitor.c): a player
flies in Forge while their unit is the monitor biped, which comes from The
Maw (c40.map) as a Forge game's map loads (game.forge_monitor). */
/* whether the map that is loading gets the monitor's tags (tag_import.c) */
int forge_monitor_import_wanted(void);
/* the monitor's biped in this map, or -1 (NONE): Forge then flies its
camera free of the player (camera/director_forge.c) */
long forge_monitor_definition(void);
/* the monitor's biped is looked for again (game/halo_mods.c, a new map) */
void forge_monitor_new_map(void);
int forge_unit_is_monitor(long unit_index);
int forge_player_is_monitor(long player_index);
/* the game's machine only: makes the player the monitor or gives them
their body back; FALSE when that cannot be now */
int forge_monitor_set(long player_index, int monitor);
/* local player 0's rise (1) and sink (-1) keys and fast key, which are the
monitor's jump, crouch and exact facing (game/player_control.c) */
void forge_monitor_set_keys(int rise, int fast);
unsigned long forge_monitor_control_flags(short local_player_index, unsigned long flags);
/* a local player's look (yaw, pitch) and throttle (forward, left) of this
frame, before the game takes them (game/player_control.c): while the monitor
turns what it holds they are the held object's, as the free camera's are */
void forge_monitor_control_input(short local_player_index, float facing_delta[2], float throttle[2], float seconds);
/* whether a unit's lens flares are left out of the view being drawn: the
monitor's own in first person (objects/object_lights.c) */
int forge_monitor_hides_lens_flares(long unit_index);
/* TRUE while a local player flies in Forge: as the monitor, or with the
camera free of them; for the tools and for mods */
int forge_flying(short local_player_index);
/* the base map's name as the game shows it ("Blood Gulch") */
char const *forge_custom_map_base_title(short index);
/* string indices, past the end of every string list, that the menus show
the Forge maps' names and descriptions with (text/text_group.c) */
#define FORGE_CUSTOM_MAP_NAME_STRING 1000
#define FORGE_CUSTOM_MAP_DESCRIPTION_STRING 2000
#define FORGE_CUSTOM_MAP_STRING_COUNT 64

/* for source mods (halo_mod.h), about local player 0 */
/* a line of text in the tools' menu font (the high-res one with
display.high_res_text), as halo_mod_draw_text draws one: its line's height,
a text's width, and the text at the left, right or center of x0 to x1 with
its line's top at y, colour 0xAARRGGBB */
short forge_label_height(void);
short forge_label_width(char const *text);
void forge_draw_label(short x0, short y, short x1, int justification, unsigned long argb, char const *text);
/* TRUE while the spawn menu is open or an object is held: the tools then
have the menu keys, delete included */
int forge_busy(void);
/* the object under the crosshair (flying) or in the player's view that the
tools would pick up, or -1 (NONE): never the player, what they ride or
carry, nor projectiles, sound scenery and placeholders */
long forge_object_at_crosshair(void);
/* TRUE when objects of the definition are a game type's flag or ball, and
when the object is one or carries one: the game type keeps those, and the
game halts if one is deleted while it runs (objects.c), so the tools move
them but never make, copy or remove them */
int forge_definition_is_flag(long definition_index);
int forge_object_holds_flag(long object_index);
/* where an object of the definition would be placed at the crosshair,
standing on the surface there; FALSE when the camera is outside the map */
int forge_placement_at_crosshair(long definition_index, float position[3]);
/* the surface point the crosshair points at and its normal, or, in the
open, a point a little in front of the camera with an upward normal; FALSE
when the camera is outside the map */
int forge_point_at_crosshair(float position[3], float normal[3]);

/* a mod's own thing held like an object: aimed to move, turned, raised,
put down or cancelled with the tools' keys and buttons (so on a controller
too). While it is held the tools' menu keys belong to it and forge_busy()
is TRUE. */
struct halo_forge_hold_input
{
	/* the time since the last update, at most a tenth of a second */
	float seconds;
	/* the keys held now (D-pad or arrows) */
	int left, right, up, down;
	/* the same, once when pressed and again while held (repeating) */
	int left_pressed, right_pressed, up_pressed, down_pressed;
	/* T, and V or X */
	int axis_pressed, snap_pressed;
	/* enter, click or A; escape, right click or B (not a right click when it
	turns the thing, below); delete, Back or Y */
	int place, cancel, delete_pressed;
	/* flying, with turns_with_look: how far the right stick (RT held) and the
	mouse (its right button held) turned it since the last update, radians,
	as they turn an object (left and up are more); mouse_turning while the
	mouse's button turns it, whose turn goes by the chosen step */
	int mouse_turning;
	float turn_yaw, turn_pitch;
};

struct halo_forge_hold
{
	/* what is held: the text drawn while it is */
	char const *name;
	/* more about it, or NULL (not drawn: only the name is) */
	void (*describe)(char *line, unsigned long size);
	/* once a frame while it is held; returns TRUE when it is done (put
	down, cancelled or removed). With a NULL input the hold has been cut
	short (a new map, a checkpoint): put things back and return TRUE. */
	int (*update)(struct halo_forge_hold_input const *input);
	/* TRUE when it is turned as an object is, by the right stick and the
	mouse (turn_yaw and turn_pitch) */
	int turns_with_look;
};

/* starts holding; FALSE when another hold is going or an
object is held. The hold is called from the next frame on. */
int forge_mod_hold_begin(struct halo_forge_hold const *hold);

/* an object a mod has just made, held as the tools hold one they spawned:
moved with the crosshair, turned about each axis, raised, orbited by the
flying camera, and put down, or removed when it is cancelled. Other objects
of the mod's may go with it (a prefab's). */
struct halo_forge_object_hold
{
	/* what is held: the text drawn while it is, or NULL for the object's
	tag */
	char const *name;
	/* each time the tools have put the held object somewhere: the place to
	put what goes with it; may be NULL */
	void (*moved)(long object_index);
	/* TRUE for an object that goes with the held one: the crosshair points
	through it, as through the held one; may be NULL */
	int (*with)(long object_index);
	/* once, when it is let go: placed TRUE when it was put down (the layout
	keeps it), FALSE when it is about to be removed, or is gone already (a
	new map, a checkpoint); may be NULL */
	void (*ended)(long object_index, int placed);
};

/* starts holding the object; FALSE when the tools hold something already,
or this machine's game is not the game's (a system link client). hold may
be NULL: the object alone. */
int forge_hold_object(long object_index, struct halo_forge_object_hold const *hold);
/* picks up an object of the map as the tools' pick up key does: put down
where it is let go, or put back when that is cancelled (a system link
client asks the host to move it); FALSE when the tools hold something
already */
int forge_pick_up(long object_index);

/* shows a page as a popup beside the crosshair, in the menu's place and
with its keys, but for the tabs: rows of a label and a value about one
thing (an object's properties). The same as a mod's popup (halo_mod.h)
opens for X on its thing. FALSE when the tools are busy (their menu is
open, something is held or typed) or the camera does not fly. */
struct halo_mod_menu;
int forge_popup_open(struct halo_mod_menu const *popup);
/* TRUE while that popup is shown */
int forge_popup_is_open(struct halo_mod_menu const *popup);
/* closes that popup, as escape or B do */
void forge_popup_close(struct halo_mod_menu const *popup);

/* the angles the tools turn an object by, of its forward and up and back
again (radians): its yaw (about the upright, left positive), then its pitch
(the nose up), then its roll (about its length, to the right positive) */
void forge_angles_of_orientation(float const forward[3], float const up[3], float angles[3]);
void forge_orientation_of_angles(float const angles[3], float forward[3], float up[3]);

/* the object the tools hold, or -1 (NONE). A popup may be shown while one
is held (forge_popup_open, or X on a controller when a mod's properties
hook takes it): the object then stays where it is until the popup closes,
and these change how it is held: its angles (as forge_angles_of_orientation
has them), one of them (axis 0 the yaw, 1 the pitch, 2 the roll) turned
(radians) and the object raised (world units); the step it turns in while
held, after going to the next (direction 1) or the last (-1), as its name
("free", "15 degrees"); and put down where it is, as enter or A do. */
long forge_held_object(void);
void forge_held_angles(float angles[3]);
void forge_held_adjust(int axis, float angle, float height);
char const *forge_held_step(int direction);
void forge_held_put_down(void);

#endif
