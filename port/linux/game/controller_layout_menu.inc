/* ---- Controller Layouts (#1143: port/linux/include/controller_layouts.h)

Settings > Controls > Controller > Button Remapping > CONTROLLER LAYOUTS, a
page of Settings' (tools/port_settings.py's CONTROLLER_LAYOUTS): the
controller of the player whose profile Settings edits (settings_local_player)
and how many of its extra buttons its layout describes (layout_rows_show),
and EDIT BUTTONS ›, which opens Controller Layout (screens/
controller_layout.xml): its extra buttons in a table (the row source
layout.controls), each its words on the controller, its symbol and its
name. A names the chosen button on the on-screen keyboard (describing it
first, with its words' name and legend, round and grey), Y types its legend,
left and right step its color and the panel's SHAPE its shape, X lets it go
back to its words. Each change is kept at once (controller_layout_store):
the page's OK and Cancel are its settings'. Nothing of it is sent.

(#1144) The page's SHARE: EXPORT writes the layout into the folder's
shared folder (controller_layout_export), with Button Remapping's buttons of
the player as its [bindings] where INCLUDE MY BINDINGS (unsaved, as the page
shows it) says; FILES says whether a file copied into the folder was
refused, its panel why; ITS BINDINGS (only where the controller's layout
came with them) how many differ from the player's, and A opens the
question (screens/layout_bindings.xml): KEEP MINE, focused, or USE THIS
LAYOUT'S, put on the player's port through Button Remapping's assign rule
(layout_bindings_use). Never on a controller's connecting. */

static struct
{
	/* the edited controller's layout as kept, its GUID (empty: none) and
	the folder's state it was read at */
	struct controller_layout layout;
	char guid[CONTROLLER_LAYOUT_GUID];
	unsigned long read_at;
	boolean read;
	/* the table: its first row, the row chosen; and what the keyboard types
	(NONE: nothing; 0 a name, 1 a legend) for which button */
	short first, chosen;
	boolean opened;
	short typing, typing_extra;
	wchar_t typed[CONTROLLER_LAYOUT_NAME];
} layout_editor = { {{0}}, {0}, 0, FALSE, 0, 0, FALSE, NONE, 0, {0} };

/* the controller being described: the Settings player's, or with none
connected debug.controller_layout_guid's (tests and pictures) */
static void layout_editor_device(struct controller_device *device)
{
	controller_device_get(settings_local_player, device);
	if (!device->guid[0])
	{
		snprintf(device->guid, sizeof(device->guid), "%s", config_string("debug.controller_layout_guid"));
		if (device->guid[0] && !device->name[0])
			snprintf(device->name, sizeof(device->name), "%s", "Test controller");
	}
}

/* the edited layout read again where the controller or the folder changed */
static void layout_editor_read(void)
{
	struct controller_device device;
	struct controller_layout const *kept;

	layout_editor_device(&device);
	if (layout_editor.read && !strcmp(device.guid, layout_editor.guid) &&
		layout_editor.read_at == controller_layouts_changes())
	{
		return;
	}
	kept = controller_layout_for(device.guid);
	memset(&layout_editor.layout, 0, sizeof(layout_editor.layout));
	if (kept)
		layout_editor.layout = *kept;
	snprintf(layout_editor.guid, sizeof(layout_editor.guid), "%s", device.guid);
	snprintf(layout_editor.layout.guid, sizeof(layout_editor.layout.guid), "%s", device.guid);
	if (!layout_editor.layout.name[0])
	{
		snprintf(layout_editor.layout.name, sizeof(layout_editor.layout.name), "%s", device.name);
		controller_layout_clean(layout_editor.layout.name, sizeof(layout_editor.layout.name), FALSE);
	}
	layout_editor.read_at = controller_layouts_changes();
	layout_editor.read = TRUE;
}

/* (#1144) the last export's words for EXPORT's row ("" for none yet); the
sharing's, below */
static char layout_exported[96];
static int layout_bindings_differ(void);
static boolean layout_bindings_use(void);
static void layout_bindings_words(char *value, unsigned long size);

static boolean layout_editor_set(short extra)
{
	return extra >= 0 && extra < CONTROLLER_REMAP_EXTRAS && layout_editor.layout.controls[extra].name[0] != 0;
}

/* the edited layout kept, and read again from what was kept */
static void layout_editor_keep(void)
{
	if (!layout_editor.guid[0])
		return;
	if (!controller_layout_store(&layout_editor.layout))
		platform_log("controller layouts: the layout of %s could not be kept", layout_editor.guid);
	layout_editor.read = FALSE;
	layout_editor_read();
}

/* an extra button's words on the controller (P3, L4, MISC 1) */
static char const *layout_editor_words(short extra)
{
	struct controller_device device;

	layout_editor_device(&device);
	return controller_remap_device_button_name(CONTROLLER_REMAP_PADDLE + extra, &device);
}

/* the chosen button described (where it is not yet): its words its name
and its legend, round and grey */
static boolean layout_editor_describe(short extra)
{
	if (!layout_editor.guid[0] || extra < 0 || extra >= CONTROLLER_REMAP_EXTRAS)
		return FALSE;
	if (!layout_editor_set(extra))
		controller_layout_describe(&layout_editor.layout.controls[extra], layout_editor_words(extra));
	return TRUE;
}

static void layout_source_opened(void)
{
	layout_editor.opened = TRUE;
	/* (the folder read again: a file copied in is taken now) */
	controller_layouts_scan();
	layout_editor.read = FALSE;
}

static void layout_source_update(struct menu_table_view *view)
{
	short shown = view->shown, focused = view->focused;
	short rows = CONTROLLER_REMAP_EXTRAS;

	layout_editor_read();
	if (layout_editor.opened && shown)
	{
		layout_editor.opened = FALSE;
		layout_editor.first = 0;
		layout_editor.chosen = 0;
		view->focus = focused = 0;
	}
	if (focused == shown - 1 && layout_editor.first + shown < rows && list_scrolls_at_end())
	{
		layout_editor.first++;
		view->focus = --focused;
	}
	else if (focused == 0 && layout_editor.first > 0 && list_scrolls_at_end())
	{
		layout_editor.first--;
		view->focus = ++focused;
	}
	layout_editor.first = (short)PIN(layout_editor.first, 0, MAX(0, rows - shown));
	if (focused != NONE && focused >= 0 && focused < shown)
		layout_editor.chosen = (short)(layout_editor.first + focused);
	layout_editor.chosen = (short)PIN(layout_editor.chosen, 0, rows - 1);
	view->count = rows;
	view->first = layout_editor.first;
	view->chosen = layout_editor.chosen;
}

static int layout_source_heading(short index, unsigned short *words, short size)
{
	(void)index;
	(void)words;
	(void)size;
	return FALSE;
}

static void layout_cell_text(struct menu_table_cell *cell, char const *text)
{
	short index;

	for (index = 0; text[index] && index < MENU_TABLE_TEXT_LENGTH - 1; index++)
		cell->text[index] = (unsigned short)(unsigned char)text[index];
	cell->text[index] = 0;
}

/* a row's button's words, its symbol (its legend in its cap: the cap's
fill and shape, the legend black or white on it) and its name; a button
not described a dash and NOT DESCRIBED, dimmer */
static void layout_source_cell(short index, char const *column, int focused, struct menu_table_cell *cell)
{
	struct controller_layout_control const *control;

	(void)focused;
	cell->frame = NONE;
	if (index < 0 || index >= CONTROLLER_REMAP_EXTRAS)
		return;
	control = &layout_editor.layout.controls[index];
	if (!strcmp(column, "button"))
		layout_cell_text(cell, layout_editor_words(index));
	else if (!strcmp(column, "symbol"))
	{
		if (!control->name[0])
		{
			layout_cell_text(cell, "-");
			cell->color = 0x6F9CD0FFUL;
			return;
		}
		layout_cell_text(cell, control->legend);
		cell->fill = (control->fill & 0xFFFFFFUL) << 8 | 0xFFUL;
		cell->frame = control->shape;
		cell->color = controller_layout_legend_color(control->fill) << 8 | 0xFFUL;
	}
	else if (!strcmp(column, "name"))
	{
		layout_cell_text(cell, control->name[0] ? control->name : "NOT DESCRIBED");
		if (!control->name[0])
			cell->color = 0x6F9CD0FFUL;
	}
}

struct menu_table_source const pc_menu_layout_source =
{
	layout_source_update, layout_source_heading, layout_source_cell, layout_source_opened,
};

/* a color's name: the offered ones', else its #RRGGBB (a file's own) */
static void layout_color_name(unsigned long fill, char *value, unsigned long size)
{
	short index;

	for (index = 0; index < CONTROLLER_LAYOUT_COLORS; index++)
	{
		if (controller_layout_colors[index].rgb == (fill & 0xFFFFFFUL))
		{
			snprintf(value, size, "%s", controller_layout_colors[index].name);
			return;
		}
	}
	snprintf(value, size, "#%06lX", fill & 0xFFFFFFUL);
}

/* a layout.* binding's value (UTF-8; nonzero if it has one): the controller
(its name, or NO CONTROLLER), the chosen button's words, whether it is
described, its shape and color, a line of what to do, and for the footer's
and the choices' ifs whether it is described (set) or not (unset) */
int pc_menu_layout_value(char const *name, char *value, unsigned long size)
{
	struct controller_device device;
	short chosen = (short)PIN(layout_editor.chosen, 0, CONTROLLER_REMAP_EXTRAS - 1);
	struct controller_layout_control const *control;

	layout_editor_read();
	control = &layout_editor.layout.controls[chosen];
	value[0] = 0;
	if (!strcmp(name, "layout.controller"))
	{
		layout_editor_device(&device);
		snprintf(value, size, "%s", device.guid[0] ? (device.name[0] ? device.name : "Controller") :
			"NO CONTROLLER");
	}
	else if (!strcmp(name, "layout.title"))
		snprintf(value, size, "%s", layout_editor_words(chosen));
	else if (!strcmp(name, "layout.state"))
	{
		if (!layout_editor.guid[0])
			snprintf(value, size, "Connect a controller.");
		else if (control->name[0])
			snprintf(value, size, "Prompts show it as %s.", control->legend);
		else
			snprintf(value, size, "Prompts name it %s.", layout_editor_words(chosen));
	}
	else if (!strcmp(name, "layout.shape"))
		snprintf(value, size, "%s", control->name[0] ? (control->shape ? "SQUARE" : "ROUND") : "-");
	else if (!strcmp(name, "layout.color"))
	{
		if (control->name[0])
			layout_color_name(control->fill, value, size);
		else
			snprintf(value, size, "-");
	}
	else if (!strcmp(name, "layout.about"))
		snprintf(value, size, "%s", "Prompts draw its symbol in place of its words. Each controller keeps its "
			"own.");
	else if (!strcmp(name, "layout.bindings"))
		layout_bindings_words(value, size);
	else if (!strcmp(name, "layout.set"))
	{
		if (control->name[0])
			snprintf(value, size, "yes");
	}
	else if (!strcmp(name, "layout.unset"))
	{
		if (!control->name[0])
			snprintf(value, size, "yes");
	}
	else
		return FALSE;
	return value[0] != 0;
}

/* the keyboard closed: the name or legend typed kept (an empty one leaves
it as it was) */
static void layout_keyboard_closed(boolean kept, wchar_t const *text, void *context)
{
	struct controller_layout_control *control;
	char typed[CONTROLLER_LAYOUT_NAME];
	short index;

	(void)context;
	if (!kept || layout_editor.typing == NONE || !layout_editor_set(layout_editor.typing_extra))
	{
		layout_editor.typing = NONE;
		return;
	}
	for (index = 0; text && text[index] && index < (short)sizeof(typed) - 1; index++)
		typed[index] = text[index] < 0x80 ? (char)text[index] : '?';
	typed[index] = 0;
	control = &layout_editor.layout.controls[layout_editor.typing_extra];
	controller_layout_clean(typed, layout_editor.typing ? sizeof(control->legend) : sizeof(control->name),
		layout_editor.typing);
	if (typed[0])
	{
		if (layout_editor.typing)
			snprintf(control->legend, sizeof(control->legend), "%s", typed);
		else
			snprintf(control->name, sizeof(control->name), "%s", typed);
		layout_editor_keep();
		platform_log("controller layouts: %s's %s is now %s", layout_editor_words(layout_editor.typing_extra),
			layout_editor.typing ? "legend" : "name", typed);
	}
	layout_editor.typing = NONE;
}

/* the chosen button's name (0) or legend (1) typed on the keyboard */
static boolean layout_keyboard_open(short extra, short what)
{
	struct controller_layout_control const *control = &layout_editor.layout.controls[extra];
	struct keyboard_request request;
	char const *text = what ? control->legend : control->name;
	short index;

	for (index = 0; text[index] && index < NUMBEROF(layout_editor.typed) - 1; index++)
		layout_editor.typed[index] = (wchar_t)(unsigned char)text[index];
	layout_editor.typed[index] = 0;
	memset(&request, 0, sizeof(request));
	request.kind = KEYBOARD_FIELD;
	request.local_player = NONE;
	request.controller = NONE;
	request.text = layout_editor.typed;
	request.maximum = (short)(what ? CONTROLLER_LAYOUT_LEGEND - 1 : CONTROLLER_LAYOUT_NAME - 1);
	request.title = what ? L"BUTTON LEGEND" : L"BUTTON NAME";
	request.caption = what ? L"1 to 3 letters, digits or signs, drawn in its symbol" :
		L"Its name in Button Remapping and the prompts' words";
	request.closed = layout_keyboard_closed;
	layout_editor.typing = what;
	layout_editor.typing_extra = extra;
	if (keyboard_open(&request))
		return TRUE;
	layout_editor.typing = NONE;
	return FALSE;
}

/* a layout action ("layout <what>"; row: the table's row it is for, NONE
for the chosen one): FALSE if it did nothing */
int pc_menu_layout_action(char const *name, short row)
{
	short extra;
	struct controller_layout_control *control;

	layout_editor_read();
	if (row != NONE)
		layout_editor.chosen = (short)PIN(layout_editor.first + row, 0, CONTROLLER_REMAP_EXTRAS - 1);
	extra = (short)PIN(layout_editor.chosen, 0, CONTROLLER_REMAP_EXTRAS - 1);
	control = &layout_editor.layout.controls[extra];
	if (!strcmp(name, "layout bindings use"))
		return layout_bindings_use();
	if (!layout_editor.guid[0])
	{
		platform_log("controller layouts: no controller to describe");
		return FALSE;
	}
	if (!strcmp(name, "layout clear"))
	{
		if (!layout_editor_set(extra))
			return FALSE;
		memset(control, 0, sizeof(*control));
		layout_editor_keep();
		return TRUE;
	}
	if (!strcmp(name, "layout name") || !strcmp(name, "layout legend"))
	{
		short what = (short)(strcmp(name, "layout name") != 0);

		if (what && !layout_editor_set(extra))
			return FALSE;
		if (!layout_editor_set(extra))
		{
			layout_editor_describe(extra);
			layout_editor_keep();
		}
		return layout_keyboard_open(extra, what);
	}
	if (!strncmp(name, "layout shape ", 13) || !strncmp(name, "layout color ", 13))
	{
		int step = !strcmp(name + 13, "left") ? -1 : 1;

		if (!layout_editor_set(extra))
		{
			/* (a color stepped on a button not described describes it) */
			if (name[7] == 's')
				return FALSE;
			layout_editor_describe(extra);
		}
		else if (name[7] == 's')
			control->shape = (unsigned char)((control->shape + CONTROLLER_LAYOUT_SHAPES + step) % CONTROLLER_LAYOUT_SHAPES);
		else
		{
			short index;

			for (index = 0; index < CONTROLLER_LAYOUT_COLORS &&
				controller_layout_colors[index].rgb != (control->fill & 0xFFFFFFUL); index++);
			/* (a file's own color steps to the first offered) */
			index = (short)(index == CONTROLLER_LAYOUT_COLORS ? 0 :
				(index + CONTROLLER_LAYOUT_COLORS + step) % CONTROLLER_LAYOUT_COLORS);
			control->fill = controller_layout_colors[index].rgb;
		}
		layout_editor_keep();
		ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	}
	return FALSE;
}

/* the page's rows (each frame): the controller, and how many of its buttons
its layout describes */
static void layout_rows_show(struct widget_instance *list)
{
	char text[96];
	short extra, described = 0;

	if (!settings_model_row(list, "layout_controller"))
		return;
	layout_editor_read();
	for (extra = 0; extra < CONTROLLER_REMAP_EXTRAS; extra++)
		described = (short)(described + layout_editor_set(extra));
	if (!layout_editor.guid[0])
		snprintf(text, sizeof(text), "NONE CONNECTED");
	else if (!described)
		snprintf(text, sizeof(text), "NO BUTTONS DESCRIBED");
	else
		snprintf(text, sizeof(text), "%d BUTTON%s DESCRIBED", described, described == 1 ? "" : "S");
	settings_status_set(list, "layout_controller", text);
	/* (#1144: EXPORT's last result, FILES', ITS BINDINGS') */
	settings_status_set(list, "layout_export", layout_exported);
	settings_status_set(list, "layout_files", controller_layouts_problem()[0] ? "ONE REFUSED" : "ALL READ");
	if (layout_editor.layout.bound)
	{
		short differ = (short)layout_bindings_differ();

		if (differ)
			snprintf(text, sizeof(text), "%d DIFFER", differ);
		else
			snprintf(text, sizeof(text), "SAME AS YOURS");
		settings_status_set(list, "layout_bindings", text);
	}
}

/* ---- sharing (#1144) */

/* the Settings player's buttons now: their layout, else their profile's */
static void layout_player_buttons(unsigned char *buttons)
{
	controller_remap_profile(settings_local_player, buttons);
	controller_remap_read(settings_local_player, buttons);
}

/* how many of the player's buttons the layout's bindings would change */
static int layout_bindings_differ(void)
{
	unsigned char buttons[CONTROLLER_REMAP_ACTIONS];

	layout_editor_read();
	if (!layout_editor.layout.bound)
		return 0;
	layout_player_buttons(buttons);
	return controller_layout_bindings_apply(&layout_editor.layout, buttons);
}

/* (settings_condition_term's "layout_bindings": ITS BINDINGS shown) */
static boolean layout_has_bindings(void)
{
	layout_editor_read();
	return layout_editor.layout.bound != 0;
}

/* "port settings layout export" (EXPORT): the layout written for sharing,
with the player's buttons as its bindings where INCLUDE MY BINDINGS says */
static void layout_export_press(void)
{
	unsigned char buttons[CONTROLLER_REMAP_ACTIONS];
	char include[16], written[80];
	boolean bindings = settings_value("input.controller_layout_bindings", include, sizeof(include)) &&
		!strcmp(include, "true");

	layout_editor_read();
	if (!layout_editor.guid[0])
	{
		snprintf(layout_exported, sizeof(layout_exported), "NO CONTROLLER");
		return;
	}
	if (bindings)
		layout_player_buttons(buttons);
	if (controller_layout_export(&layout_editor.layout, bindings ? buttons : NULL, written, sizeof(written)))
		snprintf(layout_exported, sizeof(layout_exported), "%s", bindings ? "SAVED WITH BINDINGS" : "SAVED");
	else
		snprintf(layout_exported, sizeof(layout_exported), "NOT SAVED");
	ui_play_audio_feedback_sound(SOUND_CURSOR);
}

/* USE THIS LAYOUT'S: the layout's bindings put on the player's port through
Button Remapping's assign rule, written as its OK writes a player's (their
profile's, else the port's), and Button Remapping's pending buttons of that
port with them */
static boolean layout_bindings_use(void)
{
	unsigned char buttons[CONTROLLER_REMAP_ACTIONS];
	char text[CONTROLLER_REMAP_TEXT] = {0};
	char folder[1024];
	short port = settings_local_player;
	int differ;

	layout_editor_read();
	if (!layout_editor.layout.bound || port < 0 || port >= 4)
		return FALSE;
	layout_player_buttons(buttons);
	differ = controller_layout_bindings_apply(&layout_editor.layout, buttons);
	controller_remap_format(buttons, text, sizeof(text));
	if (!text[0])
		return FALSE;
	if (player_settings_profile_folder(player_ui_get_active_player_profile_index(port), folder, sizeof(folder)))
	{
		if (!config_profile_write(folder, "input.controller", text))
			return FALSE;
	}
	else if (!config_write(controller_remap_setting(port), text))
		return FALSE;
	memcpy(pad_editor.buttons[port], buttons, sizeof(buttons));
	pad_editor.custom[port] = TRUE;
	platform_log("controller layouts: player %d took the layout's bindings (%d changed)", port + 1, differ);
	return TRUE;
}

/* (screens/layout_bindings.xml's question) layout.bindings's words */
static void layout_bindings_words(char *value, unsigned long size)
{
	int differ = layout_bindings_differ();

	if (!differ)
		snprintf(value, size, "This layout's bindings are the same as yours.");
	else
		snprintf(value, size, "This layout came with its owner's bindings: %d of your buttons would change. "
			"Yours stay unless you choose its.", differ);
}

/* FILES' panel: why a file copied in was refused */
static boolean layout_files_panel(struct widget_instance *row, wchar_t const **help)
{
	static wchar_t words[SETTINGS_PANEL_TEXT_LENGTH];
	char text[CONTROLLER_LAYOUT_PROBLEM + 96];

	if (!settings_row_is(row, "layout_files") || !controller_layouts_problem()[0])
		return FALSE;
	snprintf(text, sizeof(text), "A file was refused, and nothing of it used: %s.|Fix it or copy it again.",
		controller_layouts_problem());
	discord_widen(text, words, SETTINGS_PANEL_TEXT_LENGTH);
	*help = words;
	return TRUE;
}
