/*
KEYBOARD.H

The on-screen keyboard (port/linux/game/keyboard.c; its rules,
keyboard_core.c): one keyboard for a player's name, the menus' text fields
and the chat, a pop-up of the menu engine's (screens/keyboard.xml) over the
screen it is typed for, which waits under it as it was.

Which keyboard a mode has is decided here alone (keyboard_new): the new menus
(display.menu_engine "new" with display.menus "pc", the defaults) have this
one; the original-faithful modes (the classic engine, the Xbox's menus) keep
the game's own (source/interface/virtual_keyboard.c) and the chat's
(network_chat.c). Every caller goes through the functions here, which ask it.
*/

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "cseries.h"

struct widget_instance;
struct event_record;

/* what is typed: a name (a profile's or a gametype's, which the game's names
rules keep: Latin-1 and Latin Extended-A), a menu's field (bytes: Latin-1),
a chat line (ASCII) */
enum keyboard_kind
{
	KEYBOARD_NAME,
	KEYBOARD_FIELD,
	KEYBOARD_CHAT
};

/* a keyboard asked for: what is typed, for whom (local_player: NONE for the
menus' own; controller: the gamepad that types, NONE for any), the text
(written as it is done, its maximum characters), its title and line under
it, a password's stars, and what is told as it closes (kept: done, the text
written) */
struct keyboard_request
{
	short kind;
	short local_player;
	short controller;
	wchar_t *text;
	short maximum;
	wchar_t const *title;
	wchar_t const *caption;
	boolean masked;
	void (*closed)(boolean kept, wchar_t const *text, void *context);
	void *context;
};

/* whether this mode has the new keyboard (the new menus) or the game's own */
boolean keyboard_new(void);
/* the new keyboard opened: FALSE if it cannot be (not this mode's, or its
screen not there) */
boolean keyboard_open(struct keyboard_request const *request);

/* either mode's (keyboard_new): a name's keyboard (the game's own callers'
caption index: 8, 9 or 10), a field's (its title and caption), whether one is
up, the last closed with its text kept, and closed as a Cancel does */
boolean keyboard_name_launch(wchar_t *text, word size, short caption_index);
boolean keyboard_field_launch(wchar_t *text, word size, wchar_t const *title, wchar_t const *caption);
boolean keyboard_active(void);
boolean keyboard_last_kept(void);
void keyboard_close(void);

/* the new keyboard's: up for a chat line (network_chat.c), over no screen (a
game's view, the chat's lines shown under it), and the gamepad it takes from
the game there (NONE for none) */
boolean keyboard_chat_up(void);
boolean keyboard_over_game(void);
short keyboard_controller(void);

/* (menu_screen.c's) its screen's: each frame it is drawn (its input read),
an event of its screen's (all of them its own), its anchor drawn, and its
screen closed */
void keyboard_frame(struct widget_instance *screen);
int keyboard_event(struct event_record const *event);
void keyboard_draw(real x, real y);
void keyboard_screen_closed(void);
/* (ui_widget.c's) the pointer as the menus read it, once each update */
struct halo_ui_pointer;
void keyboard_pointer(struct halo_ui_pointer const *pointer, boolean active);

#endif
