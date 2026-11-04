/*
RENDER_PREPARED.H

A map's preparation recorded and played back (render_prepared.c).
*/

#ifndef __HALO_PORT_RENDER_PREPARED_H
#define __HALO_PORT_RENDER_PREPARED_H

#include "cseries.h"

/* the map's preparation made from its record, in a frame about to be
prepared (mirrors: whether it draws a mirror's window): TRUE, or FALSE
where there is none to play (its walk to be made, and recorded) */
boolean render_prepared_replay(boolean mirrors);
/* the walk recorded, from its preparation's beginning to its end */
void render_prepared_record_begin(boolean mirrors);
void render_prepared_record_end(void);
/* of the draws a record has (their hashes, in order), those the draws made
(in order) do not have */
unsigned long render_prepared_draws_missing(unsigned long long const *recorded, unsigned long recorded_count,
	unsigned long long const *made, unsigned long made_count);

#endif
