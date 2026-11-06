/* spectate_director.h: the spectator's auto-director (spectate_director.c) */

#ifndef __SPECTATE_DIRECTOR_H
#define __SPECTATE_DIRECTOR_H

/* a new game: everyone's signals forgotten, nobody watched */
void spectate_director_reset(
	void);
/* (spectate.c, each frame of the DIRECTOR camera) whom to watch, given who
is watched now (another than the director chose: a person picked them); with
glide whether a cut to them glides (else it is hard) */
long spectate_director_update(
	real seconds,
	long watched,
	boolean *glide);

#endif
