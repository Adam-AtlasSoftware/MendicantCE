/* Controller layouts (controller_layouts.h): each a small TOML file of the
 * data folder's controller_layouts, read whole and strictly (a file is a
 * player's own or another's copied in, so untrusted: any line it should not
 * have refuses all of it, logged, and nothing of it is used). Only the
 * prompts' pictures and names of a controller's extra buttons come of it;
 * input never does. */
#include "controller_layouts.h"
#include "controller_buttons.h"
#include "platform.h"
#include "posix.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct controller_layout_color const controller_layout_colors[CONTROLLER_LAYOUT_COLORS] = {
    {"GREY", 0x707070UL}, {"WHITE", 0xE8E8E8UL}, {"RED", 0xD03030UL}, {"ORANGE", 0xE07020UL},
    {"YELLOW", 0xE0C020UL}, {"GREEN", 0x30A040UL}, {"BLUE", 0x3070D0UL}, {"PURPLE", 0x8040C0UL}
};

static char const *const shape_names[CONTROLLER_LAYOUT_SHAPES] = {"round", "square"};

char const *controller_layout_shape_name(int shape)
{
    return shape >= 0 && shape < CONTROLLER_LAYOUT_SHAPES ? shape_names[shape] : shape_names[0];
}

/* (WCAG 2's relative luminance of an sRGB channel, 0 to 255) */
static double channel_luminance(unsigned long value)
{
    double c = (double)(value & 0xFF) / 255.0, p = (c + 0.055) / 1.055, q = p * p;
    /* (2.4's power as p^2 * p^0.4, p^0.4 by Newton's steps on its fifth power) */
    double r = 1.0;
    int i;
    if (c <= 0.04045) return c / 12.92;
    for (i = 0; i < 40; i++) r = r - (r * r * r * r * r - q) / (5.0 * r * r * r * r);
    return p * p * r;
}

unsigned long controller_layout_legend_color(unsigned long fill)
{
    double luminance = 0.2126 * channel_luminance(fill >> 16) + 0.7152 * channel_luminance(fill >> 8) +
        0.0722 * channel_luminance(fill);
    /* (white's contrast against black's) */
    return 1.05 / (luminance + 0.05) >= (luminance + 0.05) / 0.05 ? 0xFFFFFFUL : 0x000000UL;
}

void controller_layout_clean(char *text, size_t size, int legend)
{
    size_t from, to = 0;
    for (from = 0; text[from] && to + 1 < size; from++)
    {
        unsigned char c = (unsigned char)text[from];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || (legend && c == ' ')) continue;
        text[to++] = (char)c;
    }
    text[to] = 0;
}

void controller_layout_describe(struct controller_layout_control *control, char const *name)
{
    size_t i, used = 0;
    memset(control, 0, sizeof(*control));
    snprintf(control->name, sizeof(control->name), "%s", name && *name ? name : "BUTTON");
    controller_layout_clean(control->name, sizeof(control->name), 0);
    if (!control->name[0]) snprintf(control->name, sizeof(control->name), "BUTTON");
    /* (a short name its own legend; else its words' initials and its digits:
     * LEFT BACK 2's LB2, MISC 1's M1) */
    if (strlen(control->name) < CONTROLLER_LAYOUT_LEGEND && !strchr(control->name, ' '))
        snprintf(control->legend, sizeof(control->legend), "%s", control->name);
    else
        for (i = 0; control->name[i] && used + 1 < sizeof(control->legend); i++)
        {
            char c = control->name[i];
            if (c == ' ') continue;
            if ((c >= '0' && c <= '9') || i == 0 || control->name[i - 1] == ' ') control->legend[used++] = c;
        }
    control->legend[used ? used : strlen(control->legend)] = 0;
    controller_layout_clean(control->legend, sizeof(control->legend), 1);
    if (!control->legend[0]) snprintf(control->legend, sizeof(control->legend), "?");
    control->shape = CONTROLLER_LAYOUT_ROUND;
    control->fill = controller_layout_colors[0].rgb;
}

/* ---------- the file */

/* (an extra button's token: left_paddle ... misc6) */
static int extra_of(char const *text, size_t length)
{
    int button = controller_remap_token(text, length);
    return button >= CONTROLLER_REMAP_PADDLE && button < CONTROLLER_REMAP_PADDLE + CONTROLLER_REMAP_EXTRAS ?
        button - CONTROLLER_REMAP_PADDLE : -1;
}

static int problem_set(char *problem, size_t size, long line, char const *format, ...)
{
    va_list arguments;
    int used = line ? snprintf(problem, size, "line %ld: ", line) : 0;
    if (used < 0 || (size_t)used >= size) used = 0;
    va_start(arguments, format);
    vsnprintf(problem + used, size - (size_t)used, format, arguments);
    va_end(arguments);
    return 0;
}

/* a string's characters as a file may hold them (between its quotes): no
 * control characters, quotes or backslashes (so no escapes), ASCII */
static int string_valid(char const *text, size_t length, int legend)
{
    size_t i;
    for (i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || (legend && c == ' ')) return 0;
    }
    return 1;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int controller_layout_parse(char const *text, size_t length, struct controller_layout *layout, char *problem,
    size_t size)
{
    struct controller_layout parsed;
    /* (where the lines are: the top level, or a [[control]], whose input
     * is current) */
    enum { TOP, CONTROL } table = TOP;
    int control = -1, current = -1, format = 0, guid = 0;
    unsigned int seen = 0, controls_given = 0;
    long line = 0;
    size_t at = 0;
    if (!text || length > CONTROLLER_LAYOUT_FILE_MAXIMUM)
        return problem_set(problem, size, 0, "larger than %d bytes", CONTROLLER_LAYOUT_FILE_MAXIMUM);
    if (memchr(text, 0, length)) return problem_set(problem, size, 0, "not text");
    memset(&parsed, 0, sizeof(parsed));
    while (at < length)
    {
        char const *start = text + at, *end, *key, *value;
        size_t line_length, key_length, value_length;
        int quoted, number = 0, slot;
        end = memchr(start, '\n', length - at);
        line_length = end ? (size_t)(end - start) : length - at;
        at += line_length + (end ? 1 : 0);
        line++;
        if (line_length && start[line_length - 1] == '\r') line_length--;
        while (line_length && (*start == ' ' || *start == '\t')) start++, line_length--;
        while (line_length && (start[line_length - 1] == ' ' || start[line_length - 1] == '\t')) line_length--;
        if (line_length > 200) return problem_set(problem, size, line, "too long");
        if (!line_length || *start == '#') continue;
        if (line_length == 11 && !strncmp(start, "[[control]]", 11))
        {
            if (++control >= CONTROLLER_REMAP_EXTRAS)
                return problem_set(problem, size, line, "more than %d controls", CONTROLLER_REMAP_EXTRAS);
            table = CONTROL;
            seen = 0;
            current = -1;
            continue;
        }
        if (*start == '[') return problem_set(problem, size, line, "a table this file may not have: %.40s", start);
        /* (key = value: a bare key, then a quoted string or a number, then
         * nothing but a comment) */
        key = start;
        for (key_length = 0; key_length < line_length && ((key[key_length] >= 'a' && key[key_length] <= 'z') ||
            key[key_length] == '_' || (key[key_length] >= '0' && key[key_length] <= '9')); key_length++);
        value = key + key_length;
        while (value < start + line_length && (*value == ' ' || *value == '\t')) value++;
        if (!key_length || value >= start + line_length || *value != '=')
            return problem_set(problem, size, line, "not a key = value line");
        value++;
        while (value < start + line_length && (*value == ' ' || *value == '\t')) value++;
        quoted = value < start + line_length && *value == '"';
        if (quoted)
        {
            char const *close = memchr(value + 1, '"', (size_t)(start + line_length - value - 1));
            char const *rest;
            if (!close) return problem_set(problem, size, line, "a string with no end");
            value_length = (size_t)(close - value - 1);
            value++;
            for (rest = close + 1; rest < start + line_length && (*rest == ' ' || *rest == '\t'); rest++);
            if (rest < start + line_length && *rest != '#')
                return problem_set(problem, size, line, "more after the value");
            if (memchr(value, '\\', value_length)) return problem_set(problem, size, line, "a backslash");
        }
        else
        {
            char const *rest;
            for (value_length = 0; value + value_length < start + line_length && value[value_length] >= '0' &&
                value[value_length] <= '9' && value_length < 6; value_length++)
                number = number * 10 + value[value_length] - '0';
            for (rest = value + value_length; rest < start + line_length && (*rest == ' ' || *rest == '\t'); rest++);
            if (!value_length || (rest < start + line_length && *rest != '#'))
                return problem_set(problem, size, line, "a value that is neither a string nor a number");
        }
#define KEY(name) (key_length == sizeof(name) - 1 && !strncmp(key, name, sizeof(name) - 1))
        if (table == TOP)
        {
            if (KEY("format"))
            {
                if (quoted || number != 1) return problem_set(problem, size, line, "a format this game cannot read");
                format++;
            }
            else if (KEY("guid"))
            {
                size_t i;
                if (!quoted || value_length != CONTROLLER_LAYOUT_GUID - 1)
                    return problem_set(problem, size, line, "a guid is 32 hex digits");
                for (i = 0; i < value_length; i++)
                    if (hex_digit(value[i]) < 0) return problem_set(problem, size, line, "a guid is 32 hex digits");
                for (i = 0; i < value_length; i++)
                    parsed.guid[i] = (char)(value[i] >= 'A' && value[i] <= 'F' ? value[i] - 'A' + 'a' : value[i]);
                guid++;
            }
            else if (KEY("name"))
            {
                if (!quoted || value_length >= sizeof(parsed.name) || !string_valid(value, value_length, 0))
                    return problem_set(problem, size, line, "a name of up to %d letters, digits and signs",
                        (int)sizeof(parsed.name) - 1);
                memcpy(parsed.name, value, value_length);
                parsed.name[value_length] = 0;
            }
            else
                return problem_set(problem, size, line, "a key this file may not have: %.*s", (int)key_length, key);
            if (format > 1 || guid > 1) return problem_set(problem, size, line, "a key given twice");
            continue;
        }
        /* (a control's: its input first, each key once) */
        {
            static char const *const keys[] = {"input", "name", "legend", "shape", "fill"};
            struct controller_layout_control *each;
            for (slot = 0; slot < 5 && !(key_length == strlen(keys[slot]) && !strncmp(key, keys[slot], key_length));
                slot++);
            if (slot == 5)
                return problem_set(problem, size, line, "a key a control may not have: %.*s", (int)key_length, key);
            if (seen & (1u << slot)) return problem_set(problem, size, line, "a key given twice");
            seen |= 1u << slot;
            if (!quoted) return problem_set(problem, size, line, "a control's %s is a string", keys[slot]);
            if (slot == 0)
            {
                int extra = extra_of(value, value_length);
                if (extra < 0)
                    return problem_set(problem, size, line, "an input this file may not name: %.*s",
                        (int)(value_length < 24 ? value_length : 24), value);
                if (controls_given & (1u << extra)) return problem_set(problem, size, line, "an input given twice");
                controls_given |= 1u << extra;
                current = extra;
                parsed.controls[extra].fill = controller_layout_colors[0].rgb;
                continue;
            }
            if (current < 0) return problem_set(problem, size, line, "a control's input comes first");
            each = &parsed.controls[current];
            if (slot == 1)
            {
                if (!value_length || value_length >= sizeof(each->name) || !string_valid(value, value_length, 0))
                    return problem_set(problem, size, line, "a name of 1 to %d letters, digits and signs",
                        (int)sizeof(each->name) - 1);
                memcpy(each->name, value, value_length);
                each->name[value_length] = 0;
            }
            else if (slot == 2)
            {
                if (!value_length || value_length >= sizeof(each->legend) || !string_valid(value, value_length, 1))
                    return problem_set(problem, size, line, "a legend of 1 to %d letters, digits or signs",
                        (int)sizeof(each->legend) - 1);
                memcpy(each->legend, value, value_length);
                each->legend[value_length] = 0;
            }
            else if (slot == 3)
            {
                int shape;
                for (shape = 0; shape < CONTROLLER_LAYOUT_SHAPES && !(strlen(shape_names[shape]) == value_length &&
                    !strncmp(value, shape_names[shape], value_length)); shape++);
                if (shape == CONTROLLER_LAYOUT_SHAPES)
                    return problem_set(problem, size, line, "a shape is round or square");
                each->shape = (unsigned char)shape;
            }
            else
            {
                unsigned long fill = 0;
                size_t i;
                if (value_length != 7 || value[0] != '#') return problem_set(problem, size, line, "a fill is #RRGGBB");
                for (i = 1; i < 7; i++)
                {
                    if (hex_digit(value[i]) < 0) return problem_set(problem, size, line, "a fill is #RRGGBB");
                    fill = fill << 4 | (unsigned long)hex_digit(value[i]);
                }
                each->fill = fill;
            }
        }
#undef KEY
    }
    if (!format) return problem_set(problem, size, 0, "no format = 1");
    if (!guid) return problem_set(problem, size, 0, "no guid");
    /* (every control given a legend; a name defaults to it, a fill to grey) */
    {
        int extra;
        for (extra = 0; extra < CONTROLLER_REMAP_EXTRAS; extra++)
        {
            struct controller_layout_control *each = &parsed.controls[extra];
            if (!(controls_given & (1u << extra)))
            {
                memset(each, 0, sizeof(*each));
                continue;
            }
            if (!each->legend[0])
                return problem_set(problem, size, 0, "the control of %s has no legend",
                    controller_remap_token_name(CONTROLLER_REMAP_PADDLE + extra));
            if (!each->name[0]) snprintf(each->name, sizeof(each->name), "%s", each->legend);
        }
    }
    *layout = parsed;
    return 1;
}

static size_t appended(char *text, size_t size, size_t used, char const *format, ...)
{
    va_list arguments;
    int length;
    if (used >= size) return size;
    va_start(arguments, format);
    length = vsnprintf(text + used, size - used, format, arguments);
    va_end(arguments);
    return length < 0 ? size : used + (size_t)length;
}

size_t controller_layout_format(struct controller_layout const *layout, char *text, size_t size)
{
    size_t used = 0;
    int extra;
    if (!size) return 0;
    used = appended(text, size, used, "# A GOAT H1 controller layout (Settings > Controls > Controller > Button\n"
        "# Remapping > Controller Layouts), kept by the controller's SDL GUID.\nformat = 1\nguid = \"%s\"\n",
        layout->guid);
    if (layout->name[0]) used = appended(text, size, used, "name = \"%s\"\n", layout->name);
    for (extra = 0; extra < CONTROLLER_REMAP_EXTRAS; extra++)
    {
        struct controller_layout_control const *each = &layout->controls[extra];
        if (!each->name[0]) continue;
        used = appended(text, size, used, "\n[[control]]\ninput = \"%s\"\nname = \"%s\"\nlegend = \"%s\"\n"
            "shape = \"%s\"\nfill = \"#%06lX\"\n", controller_remap_token_name(CONTROLLER_REMAP_PADDLE + extra),
            each->name, each->legend, controller_layout_shape_name(each->shape), each->fill & 0xFFFFFFUL);
    }
    if (used >= size) { text[0] = 0; return 0; }
    return used;
}

/* ---------- the folder */

static struct
{
    int scanned;
    unsigned long changes;
    int count;
    struct controller_layout layouts[CONTROLLER_LAYOUT_MAXIMUM];
    char problem[CONTROLLER_LAYOUT_PROBLEM];
} layouts;

static int folder_path(char *path, size_t size, char const *name)
{
    int length = snprintf(path, size, "%s/controller_layouts%s%s", platform_data_root(), name ? "/" : "",
        name ? name : "");
    return length > 0 && (size_t)length < size;
}

static int guid_valid(char const *guid)
{
    size_t i;
    if (strlen(guid) != CONTROLLER_LAYOUT_GUID - 1) return 0;
    for (i = 0; guid[i]; i++) if (!((guid[i] >= '0' && guid[i] <= '9') || (guid[i] >= 'a' && guid[i] <= 'f'))) return 0;
    return 1;
}

/* a file read whole (up to the most a layout may be): its length, or -1
 * (none, or too large) */
static long file_read(char const *path, char *text, size_t size)
{
    FILE *file = fopen(path, "rb");
    size_t length;
    if (!file) return -1;
    length = fread(text, 1, size, file);
    fclose(file);
    return length >= size ? -1 : (long)length;
}

static int file_write(char const *path, char const *text, size_t length)
{
    FILE *file;
    char folder[600];
    if (folder_path(folder, sizeof(folder), NULL)) posix_make_directory(folder);
    file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(text, 1, length, file) != length) { fclose(file); return 0; }
    return fclose(file) == 0;
}

static void layout_keep(struct controller_layout const *layout)
{
    int i;
    for (i = 0; i < layouts.count && strcmp(layouts.layouts[i].guid, layout->guid); i++);
    if (i == layouts.count)
    {
        if (layouts.count == CONTROLLER_LAYOUT_MAXIMUM) return;
        layouts.count++;
    }
    layouts.layouts[i] = *layout;
}

void controller_layouts_scan(void)
{
    static char text[CONTROLLER_LAYOUT_FILE_MAXIMUM + 1];
    char folder[600], name[256], path[900];
    void *directory;
    int pass;
    layouts.scanned = 1;
    layouts.changes++;
    layouts.count = 0;
    layouts.problem[0] = 0;
    if (!folder_path(folder, sizeof(folder), NULL)) return;
    /* (a controller's own files first, <guid>.toml; then the others, copied
     * in, each taken as its controller's in place of its own) */
    for (pass = 0; pass < 2; pass++)
    {
        directory = posix_directory_open(folder);
        if (!directory) return;
        while (posix_directory_next(directory, name, sizeof(name)))
        {
            struct controller_layout layout;
            struct posix_file_information information;
            char problem[CONTROLLER_LAYOUT_PROBLEM];
            size_t length = strlen(name);
            long read;
            int own;
            if (length < 6 || strcmp(name + length - 5, ".toml")) continue;
            own = length == CONTROLLER_LAYOUT_GUID - 1 + 5 && !strncmp(name + CONTROLLER_LAYOUT_GUID - 1, ".toml", 5);
            if (own)
            {
                char guid[CONTROLLER_LAYOUT_GUID];
                memcpy(guid, name, CONTROLLER_LAYOUT_GUID - 1);
                guid[CONTROLLER_LAYOUT_GUID - 1] = 0;
                own = guid_valid(guid);
            }
            if (own != !pass) continue;
            if (snprintf(path, sizeof(path), "%s/%s", folder, name) >= (int)sizeof(path)) continue;
            if (posix_stat(path, &information) || (information.flags & _posix_file_is_directory)) continue;
            read = information.size_high || information.size_low > CONTROLLER_LAYOUT_FILE_MAXIMUM ? -1 :
                file_read(path, text, sizeof(text));
            if (read < 0)
                snprintf(problem, sizeof(problem), "larger than %d bytes, or not read", CONTROLLER_LAYOUT_FILE_MAXIMUM);
            if (read < 0 || !controller_layout_parse(text, (size_t)read, &layout, problem, sizeof(problem)))
            {
                snprintf(layouts.problem, sizeof(layouts.problem), "%.60s: %s", name, problem);
                platform_log("controller layouts: %s refused (%s); not used", name, problem);
                continue;
            }
            if (own && strncmp(name, layout.guid, CONTROLLER_LAYOUT_GUID - 1))
            {
                snprintf(layouts.problem, sizeof(layouts.problem), "%.60s: its guid is another controller's", name);
                platform_log("controller layouts: %s refused (its guid is another's); not used", name);
                continue;
            }
            if (!own)
            {
                /* (an import: kept as its controller's own file, the copy
                 * gone, so that it is read the same way from now on) */
                if (!controller_layout_store(&layout))
                {
                    platform_log("controller layouts: %s could not be taken; not used", name);
                    continue;
                }
                remove(path);
                platform_log("controller layouts: %s taken as controller %s's layout", name, layout.guid);
            }
            layout_keep(&layout);
        }
        posix_directory_close(directory);
    }
    if (layouts.count) platform_log("controller layouts: %d read", layouts.count);
}

struct controller_layout const *controller_layout_for(char const *guid)
{
    int i;
    if (!guid || !guid[0]) return NULL;
    if (!layouts.scanned) controller_layouts_scan();
    for (i = 0; i < layouts.count; i++)
        if (!strcmp(layouts.layouts[i].guid, guid)) return &layouts.layouts[i];
    return NULL;
}

int controller_layout_store(struct controller_layout const *layout)
{
    static char text[CONTROLLER_LAYOUT_FILE_MAXIMUM];
    char path[900], name[CONTROLLER_LAYOUT_GUID + 5];
    size_t length;
    if (!guid_valid(layout->guid)) return 0;
    length = controller_layout_format(layout, text, sizeof(text));
    snprintf(name, sizeof(name), "%s.toml", layout->guid);
    if (!length || !folder_path(path, sizeof(path), name) || !file_write(path, text, length))
    {
        platform_log("controller layouts: %s could not be written", name);
        return 0;
    }
    layout_keep(layout);
    layouts.changes++;
    return 1;
}

char const *controller_layouts_problem(void)
{
    return layouts.problem;
}

unsigned long controller_layouts_changes(void)
{
    return layouts.changes;
}

char const *controller_layout_button_name(int button, struct controller_device const *device)
{
    struct controller_layout const *layout;
    if (device && button >= CONTROLLER_REMAP_PADDLE && button < CONTROLLER_REMAP_PADDLE + CONTROLLER_REMAP_EXTRAS &&
        (layout = controller_layout_for(device->guid)) != NULL && layout->controls[button - CONTROLLER_REMAP_PADDLE].name[0])
        return layout->controls[button - CONTROLLER_REMAP_PADDLE].name;
    return controller_remap_device_button_name(button, device);
}
