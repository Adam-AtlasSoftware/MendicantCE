/*
XGPU_VULKAN_TEXTURES.H

The Vulkan backend's cache of the textures the command stream's stages draw
that are not render targets (xgpu_vulkan_textures.c): the Xbox's textures,
read from the guest's memory, and the high-res replacements' images. Called
on the render thread.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_TEXTURES_H
#define __HALO_LINUX_XGPU_VULKAN_TEXTURES_H

#include <stdint.h>

struct xgpu_stream_texture_stage;
struct xgpu_vulkan_image;

/* the image a stage draws: an Xbox texture's (XGPU_SOURCE_TEXTURE), made
from the guest's memory and made again once its pages have been written,
or an image's (XGPU_STREAM_SOURCE_IMAGE); NULL if there is none */
struct xgpu_vulkan_image *xgpu_vulkan_textures_get(const struct xgpu_stream_texture_stage *stage);

/* the VR mode's effects: the thickness of the Xbox texture a stage draws
(an effect's bitmap), RGBA8 (xgpu_vulkan_effects.h), baked from its first
level the first time it is asked for and kept with the texture (made
again with its texels); NULL for one it cannot be (a cube, a volume, too
large, an image, one whose texels could not be read yet) */
struct xgpu_vulkan_image *xgpu_vulkan_textures_thickness(const struct xgpu_stream_texture_stage *stage);
/* changes whenever a thickness image is destroyed (baked again, its
texture let go of by xgpu_vulkan_textures_frame_end, or read with another
palette): one kept from xgpu_vulkan_textures_thickness may be drawn with
only while this is the same as when it was looked up */
uint32_t xgpu_vulkan_textures_thickness_epoch(void);

/* display.prism_rt: whether a stage's Xbox texture is a cutout (a fifth of
its first level's texels or more under half alpha), read once each time its
texels change: 1 yes, 0 no, -1 not known */
int xgpu_vulkan_textures_cutout(const struct xgpu_stream_texture_stage *stage);

/* A texture whose header is named by the VR stream (the visor's plasma
noise), checked and decoded through the same cache as a draw's stage. */
struct xgpu_vulkan_image *xgpu_vulkan_textures_header(uint32_t header);

/* debug.texture_dump_directory: the folder the first level of each 2D Xbox
texture is written into as it is read (again, once its texels change):
tex<number>_fmt<format>_<width>x<height>.tga, 32-bit BGRA (NULL or empty:
none; the string is kept) */
void xgpu_vulkan_textures_dump(const char *folder);

/* the folder the ASTC encoder keeps the DXT textures' encoded blocks in
(port/linux/astc/xgpu_astc.h; NULL or empty: none; the string is copied),
where DXT textures are ASTC (HALO_ASTC builds) */
void xgpu_vulkan_textures_astc_folder(const char *folder);

/* a frame has been played: the textures no stage has drawn for half a
minute of time are let go (but those with a thickness baked from texels the
guest still has) */
void xgpu_vulkan_textures_frame_end(void);

/* statistics since the last call: textures made, and made again from pages
that were written; images; their bytes read from the guest; thicknesses
baked, and the seconds they took */
struct xgpu_vulkan_texture_statistics
{
	uint32_t made, remade, images, thickness;
	uint64_t bytes;
	double thickness_seconds;
};
void xgpu_vulkan_textures_statistics(struct xgpu_vulkan_texture_statistics *statistics);
void xgpu_vulkan_textures_statistics_peek(struct xgpu_vulkan_texture_statistics *statistics);

#endif
