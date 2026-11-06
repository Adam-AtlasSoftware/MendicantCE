/*
FORGE_ZONES_KEYS.H

Forge zones' keys: change them here. A key is one of HALO_MOD_KEY_*
(port/linux/include/halo_mod.h). The edit keys work only while the zones
are shown (FORGE_ZONES_TOGGLE_KEY with Ctrl); the size keys and the move
key are the same keys with Ctrl held, and then do not reach the game.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __FORGE_ZONES_KEYS_H
#define __FORGE_ZONES_KEYS_H

/* with Ctrl: show the zones and their keys, or hide them again (without
Ctrl, F6 is the game's own debug key "erase all actors") */
#define FORGE_ZONES_TOGGLE_KEY HALO_MOD_KEY_F6

/* place a new zone, of the last kind placed, at the crosshair */
#define FORGE_ZONES_NEW_KEY HALO_MOD_KEY_INSERT
/* the selected zone's kind: kill, gravity, teleport; with Ctrl, move it to the
crosshair, turned the way the camera looks */
#define FORGE_ZONES_KIND_KEY HALO_MOD_KEY_HOME
/* remove the selected zone */
#define FORGE_ZONES_REMOVE_KEY HALO_MOD_KEY_END
/* the selected zone's setting (a kill zone's delay, a gravity zone's
gravity); with Ctrl, its height */
#define FORGE_ZONES_SETTING_UP_KEY HALO_MOD_KEY_PAGE_UP
#define FORGE_ZONES_SETTING_DOWN_KEY HALO_MOD_KEY_PAGE_DOWN
/* with Ctrl, the selected zone's width and length (Shift: bigger steps) */
#define FORGE_ZONES_WIDER_KEY HALO_MOD_KEY_RIGHT
#define FORGE_ZONES_NARROWER_KEY HALO_MOD_KEY_LEFT
#define FORGE_ZONES_LONGER_KEY HALO_MOD_KEY_UP
#define FORGE_ZONES_SHORTER_KEY HALO_MOD_KEY_DOWN

#endif
