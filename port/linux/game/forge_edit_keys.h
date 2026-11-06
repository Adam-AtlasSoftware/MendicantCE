/*
FORGE_EDIT_KEYS.H

Forge edit's keys: change them here. A key is one of HALO_MOD_KEY_*
(port/linux/include/halo_mod.h); with its _CTRL TRUE it works only
together with Ctrl, and then no longer reaches the game while Ctrl is held
(Ctrl+C does not also crouch, Ctrl+Z does not also zoom).

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __FORGE_EDIT_KEYS_H
#define __FORGE_EDIT_KEYS_H

/* copy the object under the crosshair */
#define FORGE_EDIT_COPY_KEY HALO_MOD_KEY_C
#define FORGE_EDIT_COPY_CTRL TRUE

/* place a copy at the crosshair */
#define FORGE_EDIT_PASTE_KEY HALO_MOD_KEY_V
#define FORGE_EDIT_PASTE_CTRL TRUE

/* remove the object under the crosshair */
#define FORGE_EDIT_DELETE_KEY HALO_MOD_KEY_DELETE
#define FORGE_EDIT_DELETE_CTRL FALSE

/* bring back the last removed object */
#define FORGE_EDIT_UNDO_KEY HALO_MOD_KEY_Z
#define FORGE_EDIT_UNDO_CTRL TRUE

#endif
