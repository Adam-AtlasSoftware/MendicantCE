/*
XGPU_VULKAN_POST.H

display.anti_aliasing's passes for the Vulkan backend (xgpu_vulkan_post.c):
FXAA or SMAA antialias a window's 3D view in place before the HUD and menus
are drawn over it (their GLSL: tools/vulkan_post_shaders.py).
Supersampling and multisampling are the front end's and the core's. And
display.hdr's tone map and encoding (port/linux/src/xgpu_hdr.c's shaders).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_POST_H
#define __HALO_LINUX_XGPU_VULKAN_POST_H

#include <stdint.h>

struct xgpu_vulkan_image;

struct xgpu_hdr_frame;
struct xgpu_vulkan_output;

/* the pipelines of FXAA, or of SMAA's passes, for a target of a format
(XGPU_VULKAN_FORMAT_BGRA8, or display.hdr's RGBA16F), and SMAA's lookup
textures, made now; queued for the pipelines' compiler instead where
preparing (a map's preparation, which waits for them); 0 if they cannot be
made (logged; once 0, it stays so) */
int xgpu_vulkan_post_prepare(int smaa, uint8_t format, int preparing);

/* FXAA, or SMAA, over the corners x0, y0 to x1, y1 (pixels from row 0) of a
layer of a BGRA8 or RGBA16F target, in the stream's order; the targets
bound before are not bound after. 0 if its pipelines cannot be made */
int xgpu_vulkan_post_anti_alias(int smaa, struct xgpu_vulkan_image *target, uint32_t layer, const int32_t corners[4]);

/* display.panini (port/linux/game/panini.c): its pipeline for a target of
a format (BGRA8, or display.hdr's RGBA16F) and a number of samples, made
now or queued where preparing, 0 if it cannot be (logged once); and the
corners of a layer of such a target drawn again through the Panini
projection (port/linux/include/panini_projection.h): read from source (not
multisampled: the target's pixels, resolved), drawn into into (the same
image, or the target's multisampled attachment, every sample of a pixel
given the pixel's value, so that what is drawn over it after is
multisampled and resolved with it); projection its d, s, magnification at
the middle, 0, and the rectilinear view's tangents at the corners' left,
right, top and bottom. In the stream's order; the targets bound before are
not bound after. 0 if it was not drawn */
int xgpu_vulkan_post_panini_prepare(uint8_t format, uint32_t samples, int preparing);
/* display.lighting "ce_deferred": the colour target (one sample) made light
(its own pixels) x base + additions, or one of them alone (view: 0 the
frame, 1 the base, 2 the light, 3 the additions); relight (NULL none):
display.lighting "raster", which makes the light the resolve reads of the
light's sampled copy, the base and the additions (NULL: the copy's own),
and says (whole) where what it made is the frame itself, not a light the
base multiplies; 1 if it was drawn */
int xgpu_vulkan_post_deferred_resolve(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *gbuffer, struct xgpu_vulkan_image *extras,
	struct xgpu_vulkan_image *depth, int view,
	struct xgpu_vulkan_image *(*relight)(struct xgpu_vulkan_image *light, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, int *whole, void *context), void *context);

/* PRISM v4 M4: the level's base colour where the G-buffer pass drew a
pack's or a mod's material's maps (the albedo target; gbuffer the G-buffer's
words: a level pixel of the pack or mod tier) drawn into the base target in
its colour's place, its alpha kept; 0 if it cannot be */
int xgpu_vulkan_post_albedo_over(struct xgpu_vulkan_image *base, struct xgpu_vulkan_image *albedo,
	struct xgpu_vulkan_image *gbuffer);
int xgpu_vulkan_post_panini(struct xgpu_vulkan_image *source, struct xgpu_vulkan_image *into, uint32_t layer,
	const int32_t corners[4], const float projection[8]);

/* display.hdr (docs/hdr.md). The tone map's pipeline for an RGBA16F target
of a number of samples drawn with a depth attachment (XGPU_VULKAN_FORMAT_
D24S8) or none, made now or queued where preparing; 0 if it cannot be.
The tone map over the corners of a target (its first layer), in the pass
drawing into it and its depth (bound after as they are, so that the pass
goes on): a draw reading each pixel where it is drawn (the core's local
reads), its colour alone written; hdr the mode and the peak. */
int xgpu_vulkan_post_prepare_hdr(uint8_t depth_format, uint32_t samples, int preparing);
/* display.prism_bloom (xgpu_backend.h's XGPU_BLOOM_*): the light past white
of the target's rectangle (an RGBA16F target of one sample, the 3D view's
before its tone map) glows, added into it; 0 if none was drawn */
int xgpu_vulkan_post_bloom(struct xgpu_vulkan_image *target, const int32_t corners[4], int mode);
/* display.prism_tone (xgpu_backend.h's XGPU_PRISM_TONE_*): the target's
rectangle (as the bloom's) graded in place by its local mean: local
contrast, lift, saturation and exposure; 0 if none was drawn */
int xgpu_vulkan_post_prism_tone(struct xgpu_vulkan_image *target, const int32_t corners[4], int mode);
void xgpu_vulkan_post_tone_map(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *depth,
	const int32_t corners[4], const float hdr[4]);
/* The frame's last pass before the swapchain (the core's options'
encode, #1108): encode_prepare makes its pipelines at the start, for the
swapchains' formats (XGPU_VULKAN_FORMAT_RGBA16F, scRGB's;
XGPU_VULKAN_FORMAT_RGB10A2, HDR10's and a 10-bit SDR swapchain's;
XGPU_VULKAN_FORMAT_BGRA8, SDR's), as the loading screen's presentation may
draw it while a map is prepared: 0 if they cannot be made (logged). encode
draws the frame (BGRA8 or RGBA16F, the frame's values) scaled to the
output's size (sharply by its factors), encoded (xgpu_hdr.c's hdr_encode:
the frame's paper white, peak, output and SDR curve; an SDR output curved
by the output's power) and dithered to its levels, into an image of its own
of the format, in the stream's order, and returns it; the targets bound
before are not bound after. NULL if it cannot. */
int xgpu_vulkan_post_encode_prepare(void);
struct xgpu_vulkan_image *xgpu_vulkan_post_encode(struct xgpu_vulkan_image *image, const struct xgpu_hdr_frame *frame,
	uint8_t format, const struct xgpu_vulkan_output *output);

/* The present step's turn, for a swapchain made for the display's own
orientation while the phone is held another way (Android's pre-rotation:
the compositor then has nothing to turn). prepare makes its pipeline, at the
start (not while a map is prepared, since rotate may be called by the
loading screen's presentation then): 0 if it cannot be made (logged).
rotate draws an image turned by quarter turns clockwise (1 to 3) into an
image of its own (its width and height swapped for an odd count), in the
stream's order, and returns it; the targets bound before are not bound
after. NULL if it cannot. */
int xgpu_vulkan_post_rotate_prepare(void);
struct xgpu_vulkan_image *xgpu_vulkan_post_rotate(struct xgpu_vulkan_image *image, int quarter_turns);

/* the terrain overhaul's debug views (xgpu_vulkan_terrain.c) over a
target's resolved frame, in place: the target copied, then drawn again
from the copy, the G-buffer's words (NULL none), the classes' table
(RGBA8, a texel a material) and the events (RGBA32F, two texels each; NULL
none); bounds: the views (1 the classes, 2 the events), the events' count,
1 where there are words. Its colour alone (CE's destination alpha kept).
0 if it cannot be drawn */
int xgpu_vulkan_post_terrain_debug(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *gbuffer,
	struct xgpu_vulkan_image *classes, struct xgpu_vulkan_image *events, const float bounds[4]);
/* the terrain overhaul's HQ layers (T1; xgpu_vulkan_terrain.c): the level's
pixels of a material with a layer in the base target (BGRA8 or RGBA16F, one
sample) take CE's colour times the layers' detail, and their G-buffer words
(RGBA32UI, the base's size) the layers' normal and roughness, from the
second target's words (extras: the distance), the classes' table, the
layers' detail and surface arrays and the macro map; values: the uniforms
(the camera, the cues, the layers; metrics are the pass's). The words are
the second target while it draws (none after). 0 if it cannot be drawn */
struct xgpu_vulkan_post_uniforms;
int xgpu_vulkan_post_terrain_layers(struct xgpu_vulkan_image *base, struct xgpu_vulkan_image *words,
	struct xgpu_vulkan_image *extras, struct xgpu_vulkan_image *classes, struct xgpu_vulkan_image *detail,
	struct xgpu_vulkan_image *surface, struct xgpu_vulkan_image *macro,
	const struct xgpu_vulkan_post_uniforms *values);

#endif
