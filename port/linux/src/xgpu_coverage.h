/*
XGPU_COVERAGE.H

The share of a texture's texels an alpha test draws, for code that cannot
include xgpu.h (the game's: world_sdf.c, whose objects' occlusion fields
take an alpha-tested leaf's card as blocking that share of what crosses it).
Plain types only. Defined in xbox_texture_decode.c; with display.prism_rt's
materials' mean colours and alpha maps (prism_rt.c).
*/

#ifndef XGPU_COVERAGE_H
#define XGPU_COVERAGE_H

/* the share of a 2D texture's texels (its D3D Format and Size words, its
texels at texels, levels after one another as they lie) whose alpha is over
reference, at its first level no larger than largest each way; 1 for one
that cannot be read (a cube map, a volume, a format not known, no memory):
taken as drawn whole */
float xgpu_texture_alpha_coverage(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned int reference, unsigned long largest);

/* display.prism_rt's materials (port/linux/game/prism_rt.c): a 2D
texture's mean colour at its first level no larger than largest each way,
its red, green and blue made linear (a byte to the power 2.2) and its alpha
(0 to 1) as it is; 0 where it cannot be read */
int xgpu_texture_mean_color(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned long largest, float mean[4]);
/* ... and its alpha, a byte a texel at that level, into alpha (room for
largest x largest), its size in *width and *height; 0 where it cannot be
read */
int xgpu_texture_alpha_map(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned long largest, unsigned char *alpha, unsigned long *width, unsigned long *height);

#endif
