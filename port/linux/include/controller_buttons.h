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
    /* its back paddles (controller_remap_paddles' bits) */
    unsigned int paddles;
    char name[128];
};

/* Uses the same port assignment as XInput, including keyboard/pad split screen.
 * A disconnected port returns a cleared identity; no saved mapping is changed. */
void controller_device_get(short port, struct controller_device *device);
void controller_device_read(void *gamepad, struct controller_device *device);
void controller_device_update(short port, void *gamepad);

#endif
