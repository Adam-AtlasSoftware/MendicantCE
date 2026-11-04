/*
PREPARED_SHIPPED.H

The stock maps' preparations' records shipped with the game
(port/assets/prepared, made by tools/prepared_records.py on a Linux machine
from the stock maps: render states, texture references by tag and bitmap
index, mirror pages and shader ids, no map data). tools/embed_assets.py
makes them C data, each as zlib compressed it, and the game reads them as
they are, read-only, where the player's own z:\prepared has no record
(port/linux/game/render_prepared.c).
*/

#ifndef PREPARED_SHIPPED_H
#define PREPARED_SHIPPED_H

/* a record: its map's cache file checksum and its key (as its file's name
has them), its words as zlib compressed them, and its size inflated */
struct prepared_shipped
{
	unsigned int checksum;
	unsigned int key_high, key_low;
	const unsigned int *data;
	unsigned int compressed_size, size;
};

extern const struct prepared_shipped prepared_shipped[];
extern const unsigned int prepared_shipped_count;

/* the shipped record of a map (its checksum) and key, inflated: malloc's,
of size bytes; NULL where none is shipped, or it does not inflate whole */
void *prepared_shipped_read(unsigned int checksum, unsigned int key_high, unsigned int key_low, unsigned int *size);
/* a record of that map and key is shipped */
int prepared_shipped_exists(unsigned int checksum, unsigned int key_high, unsigned int key_low);

#endif
