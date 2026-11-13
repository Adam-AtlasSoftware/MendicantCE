/*
MENDICANT_SHIPPED.H

MENDICANT AI's trained models as the builds ship them: one file,
mendicant_models.aibw (port/assets/mendicant/models.aibw, exported by
tools/mendicant/export_models.py), which the builds put beside the game as
they do prepared.records (beside config.toml: the build's folder and the
Windows build's; the Linux release's share/openh1). The game takes it where
no mod carries ai.mendicant.weights (port/linux/game/mendicant/
mendicant_models.c): a mod's file is found first, as before.
*/

#ifndef MENDICANT_SHIPPED_H
#define MENDICANT_SHIPPED_H

#define MENDICANT_SHIPPED_FILE "mendicant_models.aibw"

/* the file's size and its path (for the log); 0 where the build has none, or
it is larger than maximum_size. Read once and kept until
mendicant_shipped_models_release. */
unsigned long mendicant_shipped_models_size(unsigned long maximum_size, char *path, int path_size);
/* ...copied into a buffer of that size (the caller's, whose memory is the
game's): whether it was */
int mendicant_shipped_models_copy(void *into, unsigned long size);
/* ...and let go */
void mendicant_shipped_models_release(void);

#endif
