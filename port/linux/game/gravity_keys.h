/*
GRAVITY_KEYS.H

Gravity's keys: change them here. A key is one of HALO_MOD_KEY_*
(port/linux/include/halo_mod.h); it works only together with Ctrl, and
then no longer reaches the game while Ctrl is held. With Shift held too it
goes the other way.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __GRAVITY_KEYS_H
#define __GRAVITY_KEYS_H

/* the next gravity (with Shift, the one before) */
#define GRAVITY_CYCLE_KEY HALO_MOD_KEY_G

#endif
