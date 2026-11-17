/*
MENU_POPUP.C (test)

The menus' pop-ups (ui_widget.c's ui_widget_port_popups): a screen that
menu_screen_popup names (a hand-written screen with a <dialog>; the
keyboard's) opens over the screen up without deleting it or adding to its
history. The screen under it is not deleted (its "deleted" handler, which a
screen that closes itself runs, does not run), takes no events, and is up
again, the same screen, as the pop-up's back closes it. A screen opened from
the pop-up closes it and remembers the screen under it; the screen under it
deleted, or every screen closed, closes it; over no screen (a game's) it
takes its player's events and is drawn in its player's view.
test_menu_popup.py takes the game's code.
*/

#include "harness.h"

typedef short wchar16;
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define MAXIMUM_GAMEPADS 4
#define _error_silent 0
#define match_vwarn(...) ((void)0)
#define _widget_dont_push_history_data_bit 14
enum { _widget_controller0, _widget_controller1, _widget_controller2, _widget_controller3, _widget_controller_any };

struct widget_instance
{
	long definition_tag_index;
	short local_player_index;
	struct widget_instance *parent;
	boolean live;
};

struct ui_widget_definition
{
	long flags;
	short controller_index;
};

struct widget_stack_data
{
	long previous_widget_tag;
	long focused_child_parent_widget_tag;
	short focused_child_index;
	short local_player_index;
};

struct widget_stack_node
{
	struct widget_stack_data data;
	struct widget_stack_node *next;
};

static struct
{
	struct widget_instance *active_widgets[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	struct widget_stack_node *widget_stack[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
	boolean initialized;
} widget_globals = { { NULL }, { NULL }, TRUE };

/* the screens' tags: a screen that closes itself (the new campaign's
creating_profile: its "deleted" handler decides), an editor, a pop-up, and
another screen */
enum { TAG_SELF_CLOSING, TAG_EDITOR, TAG_POPUP, TAG_OTHER, NUMBER_OF_TAGS };
static struct ui_widget_definition definitions[NUMBER_OF_TAGS];
static int deleted_handlers[NUMBER_OF_TAGS];
static int instances;
static char const *case_name;
static void *widget_memory_pool;

static struct ui_widget_definition *ui_widget_definition_get(long tag_index)
{
	return &definitions[tag_index];
}

static void *pool_new_pointer(void *pool, long size, char const *file, long line)
{
	return calloc(1, (size_t)size);
}

static void dispose_pointer(void *pool, void *pointer)
{
	free(pointer);
}

static long tag_loaded(long group, char const *name)
{
	return NONE;
}
#define UI_WIDGET_DEFINITION_TAG 0

static void error(int code, char const *format, ...)
{
}

static void widget_instance_initialize(struct widget_instance *widget, struct widget_instance *parent,
	struct ui_widget_definition *definition, long tag_index, short local_player_index, short widget_stack)
{
	widget->definition_tag_index = tag_index;
	widget->local_player_index = local_player_index;
	widget->parent = parent;
	widget->live = TRUE;
	instances++;
}

static void pc_menu_screen_shown(long tag_index)
{
}

static void menu_engine_screen_opened(struct widget_instance *widget)
{
}

static void widget_instance_set_focused_child_by_index(long tag_index, struct widget_instance *widget,
	short child_index)
{
}

static void mcc_ui_settings_close(short local_player_index)
{
}

/* (the old keyboard's, until it goes) */
static boolean virtual_keyboard_active(void)
{
	return FALSE;
}

static void virtual_keyboard_close(void)
{
}

/* (menu_screen.c) */
int menu_screen_popup(long tag_index)
{
	return tag_index == TAG_POPUP;
}

static struct widget_instance *ui_widget_port_popups[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
static void ui_widget_port_screen_gone(struct widget_instance *widget);
void ui_widget_delete(struct widget_instance *widget);
struct widget_instance *ui_widget_port_top_screen(short local_player_index);

#include "under_test.inc"

/* the game's delete, as far as the screens go: its "deleted" handler run,
then the screen gone (ui_widget_port_screen_gone) */
void ui_widget_delete(struct widget_instance *widget)
{
	CHECK(widget->live, "a screen deleted twice");
	widget->live = FALSE;
	deleted_handlers[widget->definition_tag_index]++;
	instances--;
	ui_widget_port_screen_gone(widget);
	/* (left allocated: the test asks what was deleted) */
}

static struct widget_instance *open_screen(long tag, struct widget_instance *from)
{
	return ui_widget_load_by_name_or_tag(NULL, tag, NULL, 0, from ? from->definition_tag_index : NONE, NONE, NONE);
}

static long history_depth(void)
{
	long depth = 0;
	struct widget_stack_node *node;

	for (node = widget_globals.widget_stack[0]; node; node = node->next)
		depth++;
	return depth;
}

int main(int argc, char **argv)
{
	long tag;

	for (tag = 0; tag < NUMBER_OF_TAGS; tag++)
		definitions[tag].controller_index = _widget_controller_any;
	CHECK(argc == 2, "a case");
	case_name = argv[1];
	CASE("kept")
	{
		/* (a pop-up over the screen that closes itself: it is not deleted,
		its handler does not run, the history is as it was) */
		struct widget_instance *under = open_screen(TAG_SELF_CLOSING, NULL);
		struct widget_instance *popup = open_screen(TAG_POPUP, under);

		CHECK(popup && under->live, "the screen under the pop-up was deleted");
		CHECK(!deleted_handlers[TAG_SELF_CLOSING], "its deleted handler ran");
		CHECK(widget_globals.active_widgets[0] == under && history_depth() == 0, "the history grew");
		CHECK(ui_widget_port_top_screen(0) == popup && ui_widget_port_top_screen(NONE) == popup,
			"the pop-up does not take the events");
		return 0;
	}
	CASE("back")
	{
		/* (its back closes it alone: the same screen up, the history as it
		was) */
		struct widget_instance *first = open_screen(TAG_OTHER, NULL);
		struct widget_instance *editor = open_screen(TAG_EDITOR, first);
		struct widget_instance *popup = open_screen(TAG_POPUP, editor);

		CHECK(history_depth() == 1, "the history: %ld", history_depth());
		widget_instance_go_back_to_previous(popup);
		CHECK(!popup->live && editor->live && widget_globals.active_widgets[0] == editor, "back left the editor");
		CHECK(history_depth() == 1 && ui_widget_port_top_screen(0) == editor && !ui_widget_port_popups[0],
			"back went further than the pop-up");
		CHECK(!deleted_handlers[TAG_EDITOR] && instances == 1, "a screen made or deleted: %d", instances);
		return 0;
	}
	CASE("opened")
	{
		/* (a screen opened from the pop-up closes it; the history remembers
		the screen under it, not the pop-up) */
		struct widget_instance *editor = open_screen(TAG_EDITOR, NULL);
		struct widget_instance *popup = open_screen(TAG_POPUP, editor);
		struct widget_instance *other = open_screen(TAG_OTHER, popup);

		CHECK(other && !popup->live && !ui_widget_port_popups[0] && widget_globals.active_widgets[0] == other,
			"the pop-up stayed");
		CHECK(widget_globals.widget_stack[0] && widget_globals.widget_stack[0]->data.previous_widget_tag == TAG_EDITOR,
			"the history does not remember the editor");
		return 0;
	}
	CASE("replaced")
	{
		/* (the screen under it replaced by the game or that screen, not the
		pop-up: the pop-up stays, over the new screen, which waits under it) */
		struct widget_instance *under = open_screen(TAG_OTHER, NULL);
		struct widget_instance *popup = open_screen(TAG_POPUP, under);
		struct widget_instance *self_closing = open_screen(TAG_SELF_CLOSING, under);

		CHECK(popup->live && ui_widget_port_popups[0] == popup, "the pop-up went with the screen under it");
		CHECK(!under->live && widget_globals.active_widgets[0] == self_closing && !deleted_handlers[TAG_SELF_CLOSING],
			"the new screen is not under the pop-up");
		CHECK(ui_widget_port_top_screen(0) == popup, "the new screen takes the events");
		return 0;
	}
	CASE("closed")
	{
		/* (the screen under it deleted, or every screen closed, closes it) */
		struct widget_instance *editor = open_screen(TAG_EDITOR, NULL);
		struct widget_instance *popup = open_screen(TAG_POPUP, editor);

		ui_widget_delete(editor);
		CHECK(!popup->live && !ui_widget_port_popups[0], "the pop-up outlived the screen under it");
		editor = open_screen(TAG_EDITOR, NULL);
		popup = open_screen(TAG_POPUP, editor);
		ui_widgets_close_all();
		CHECK(!popup->live && !editor->live && !ui_widget_port_popups[0] && !widget_globals.active_widgets[0],
			"close all left a screen");
		popup = open_screen(TAG_POPUP, NULL);
		ui_widgets_close_all();
		CHECK(!popup->live && !ui_widget_port_popups[0], "close all kept a pop-up over no screen");
		popup = open_screen(TAG_POPUP, NULL);
		ui_widget_port_popups_close();
		CHECK(!popup->live && !ui_widget_port_popups[0], "the map let go of kept the pop-up");
		return 0;
	}
	CASE("game")
	{
		/* (over no screen: its player's, taking its events, in its view;
		another opened over it replaces it) */
		struct widget_instance *popup = ui_widget_load_by_name_or_tag(NULL, TAG_POPUP, NULL, 2, NONE, NONE, NONE);
		struct widget_instance *again;

		CHECK(popup && ui_widget_port_popups[2] == popup && !widget_globals.active_widgets[2], "not player 3's");
		CHECK(ui_widget_port_top_screen(2) == popup && !ui_widget_port_top_screen(0), "its events");
		CHECK(ui_widget_port_popup_in_view(popup, 2, 2) && !ui_widget_port_popup_in_view(popup, 2, 0),
			"its view");
		again = ui_widget_load_by_name_or_tag(NULL, TAG_POPUP, NULL, 2, NONE, NONE, NONE);
		CHECK(!popup->live && ui_widget_port_popups[2] == again, "two pop-ups for one player");
		widget_instance_go_back_to_previous(again);
		CHECK(!again->live && !ui_widget_port_top_screen(2) && history_depth() == 0, "its back");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
