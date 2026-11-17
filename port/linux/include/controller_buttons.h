#ifndef CONTROLLER_BUTTONS_H
#define CONTROLLER_BUTTONS_H

enum controller_family
{
    CONTROLLER_UNKNOWN,
    CONTROLLER_XBOX,
    CONTROLLER_PLAYSTATION,
    CONTROLLER_SWITCH,
    /* the Steam Deck's own controls (A B X Y, L1 R1, L2 R2, View, L4 L5 R4 R5) */
    CONTROLLER_STEAM_DECK
};

struct controller_device
{
    int family;
    int type;
    unsigned int instance;
    unsigned short vendor, product;
    /* its extra buttons a layout can take: the back paddles, the touchpad,
     * misc1 to misc6 (controller_remap_paddles' bits) */
    unsigned int paddles;
    /* a PlayStation pad's generation (3, 4 or 5), else 0 */
    int playstation;
    char name[128];
    /* SDL's GUID of the pad, its 32 hex digits (empty for none): what a
     * controller layout is kept by (controller_layouts.h) */
    char guid[33];
};

/* Uses the same port assignment as XInput, including keyboard/pad split screen.
 * A disconnected port returns a cleared identity; no saved mapping is changed. */
void controller_device_get(short port, struct controller_device *device);
void controller_device_read(void *gamepad, struct controller_device *device);
void controller_device_update(short port, void *gamepad);
/* the extra buttons (controller_remap_paddles' bits) a controller has that
 * are not for a layout: on Valve's pads that report their sticks' touch (the
 * Steam Deck's own controls, 28de:1205, and the second Steam Controller,
 * 28de:1302 to 1305: gyro_aim.h's gyro_stick_touch_known) misc1 is the Quick
 * Access button, Steam's own, and misc3 to misc6 are touches, not presses */
unsigned int controller_extras_reserved(unsigned short vendor, unsigned short product);
/* the HUD's button styles (display.hud_button_style and its families'
 * display.hud_button_style_xbox, _ps and _switch): a style's index by its
 * name, its controller family, a family's setting, and a port's style, its
 * controller's family's where input.auto_button_detection says (OpenCE's
 * button glyphs: port/linux/game/controller_glyphs.c) */
int controller_button_style_index(char const *name);
int controller_button_style_family(int style);
char const *controller_button_family_setting(int family);
int controller_button_style_for_port(short port);

#endif
