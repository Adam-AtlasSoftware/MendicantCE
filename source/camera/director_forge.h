/*
DIRECTOR_FORGE.H

The flying camera of the Forge game type (port/linux/game/forge.c), kept
out of director.h so the original is untouched.

From SaftiGlas's Forge (https://github.com/SaftiGlas/halo-ce-universal,
main 173fc93ae), AGPL-3.0-or-later; ported to OpenH1 (CREDITS.md).
*/

#ifndef __DIRECTOR_FORGE_H__
#define __DIRECTOR_FORGE_H__

boolean director_forge_flying(
	short local_player_index);
void director_forge_set_flying(
	short local_player_index,
	boolean flying);

#endif
