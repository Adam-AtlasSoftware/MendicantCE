/*
XGPU_VULKAN_POST.C

The post-process passes of display.anti_aliasing (xgpu_vulkan_post.h): FXAA
and SMAA, run on a render target in place, over the rectangle of a window's
3D view, their GLSL compiled to SPIR-V (xgpu_vulkan_post_shaders.c, by
tools/vulkan_post_shaders.py). Each pass draws one triangle covering the
rectangle (its corners from the vertex's index, with no vertex data) and
finds its texels from the fragment's coordinates; the target's rows are
the picture's from the top, as the passes' own images' are, so no
coordinate is turned over. The target's pixels are copied first, so that no
pass reads what it writes, and only colour is written back: the game keeps
values of its own in destination alpha.

Each pass is a pass of the core's (xgpu_vulkan.c) of its own: the barrier
that ends one makes what it drew visible to the next one's samples, and
the copy's barrier its pixels to the first. SMAA's edges and weights are
emptied whole before their passes, as the pass begins (a clear of the
whole attachment loads none of it), so that no edge of another window's
pass is found: they write only where there is an edge. The intermediate
images are the target's size (the shaders' coordinates are the
target's).

The present step's turn (xgpu_vulkan_post_rotate) is a pass of the same
kind: the frame drawn turned by quarter turns into an image of its own, for
a swapchain made for the display's orientation while the phone is held
another way (xgpu_vulkan.c, present). Its pipeline is made at the start,
where the window's surface can be turned (xgpu_vulkan_post_rotate_prepare).

The pipelines and SMAA's lookup textures are made as the setting is chosen
(xgpu_vulkan_post_prepare), or queued as a map is prepared, not in the
middle of a frame. The lookup textures are RGBA8 images of SMAA's one and
two channels (the rest zero), sampled as an R8 and an RG8 texture would be.
FXAA's and SMAA's blend into a target of its own format (display.hdr's
half floats too); the copy is the target's format, SMAA's edges and
weights 8 bits.

display.hdr's tone map (xgpu_vulkan_post_tone_map) is no pass of its own:
a draw in the pass that drew the view, reading each pixel where it is drawn
as the core's local reads let a draw (as Metal's reads tile memory), each
sample's own of a multisampled target, writing its colour (the game keeps
values of its own in alpha). Its pipeline is made as HDR is chosen, or
queued as a map is prepared. The encoding for an HDR swapchain
(xgpu_vulkan_post_encode) is a pass like the turn's: the frame drawn
encoded into an image of the swapchain's format, which the core then
letterboxes into the swapchain as it does an SDR frame; its pipelines are
made at the start (xgpu_vulkan_post_encode_prepare). display.gamma's curve
(xgpu_vulkan_post_gamma) is the same kind of pass for an SDR frame: the
swapchain's image is filled by a blit, which has no arithmetic, so the curve
is drawn into an image of its own first, and only where the setting is not
1 (the core's present skips it).
*/

#include "xgpu_vulkan_post.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_post_shaders.h"
#include "../src/xgpu_hdr.h"
#include "../src/xgpu_stream.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* the draw set's constants' range (xgpu_vulkan.c draw_set_make), which the
uniforms' (2048 bytes) fits in: one range of the ring for both */
#define UNIFORMS_RANGE (192 * 16)

static struct
{
	VkShaderModule vertex, pixel[XGPU_VULKAN_POST_PROGRAM_COUNT];
	struct xgpu_vulkan_pipeline_description pipelines[XGPU_VULKAN_POST_PROGRAM_COUNT];
	/* FXAA's and SMAA's blend into display.hdr's half floats */
	struct xgpu_vulkan_pipeline_description half_float[XGPU_VULKAN_POST_PROGRAM_COUNT];
	/* FXAA's, and SMAA's, pipelines or textures could not be made */
	int failed[2];
	/* the size and format of the images below */
	uint32_t width, height;
	uint8_t format;
	/* the target's pixels before the pass, and SMAA's edges and blending
	weights */
	struct xgpu_vulkan_image *color, *edges, *weights;
	/* SMAA's lookup textures */
	struct xgpu_vulkan_image *area, *search;
	/* linear and clamped, as FXAA and SMAA sample; nearest, for the G-buffer's
	words (integers: fetched, never filtered); and linear between the levels
	too, the projected decals' bitmaps' */
	VkSampler sampler, nearest, levels;
	/* the present step's turn: its pipeline (made at the start, never
	while a map is prepared), whether it cannot be, and the turned frame */
	VkPipeline rotate;
	int rotate_failed;
	struct xgpu_vulkan_image *turned;
	/* display.hdr: the tone map's pipeline could not be made (logged
	once); the encoding's pipelines (scRGB's, HDR10's), and its image */
	int tone_map_failed;
	VkPipeline encode[2];
	struct xgpu_vulkan_image *encoded;
	/* display.gamma: its curve's pipeline, and the frame curved */
	VkPipeline gamma;
	struct xgpu_vulkan_image *curved;
	/* display.panini's pipelines could not be made (logged once) */
	int panini_failed;
	/* (display.lighting "ce_deferred"'s resolve: the G-buffer's words, its
	fourth texture; NULL otherwise) */
	struct xgpu_vulkan_image *gbuffer;
	/* (its stand-in where there is none: a texel of words, never read, but
	of the type the shader declares) */
	struct xgpu_vulkan_image *gbuffer_stand_in;
} post;

/* ---------- images */

static struct xgpu_vulkan_image *image_make(uint8_t format, uint8_t usage, uint32_t width, uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = usage;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* a lookup texture of SMAA's, its channels' bytes (four to a word) each
made a texel of four */
static struct xgpu_vulkan_image *lookup_texture(const uint32_t *words, uint32_t width, uint32_t height,
	uint32_t channels)
{
	struct xgpu_vulkan_image *image = image_make(XGPU_VULKAN_FORMAT_RGBA8, XGPU_VULKAN_USAGE_SAMPLED, width, height);
	unsigned char *texels = malloc(width * height * 4);
	uint32_t texel, channel, byte;

	if (!image || !texels)
	{
		free(texels);
		xgpu_vulkan_image_destroy(image);
		return NULL;
	}
	for (texel = 0; texel < width * height; texel++)
	{
		for (channel = 0; channel < 4; channel++)
		{
			byte = texel * channels + channel;
			texels[texel * 4 + channel] = channel < channels ?
				(unsigned char)(words[byte / 4] >> (byte % 4 * 8)) : 0;
		}
	}
	xgpu_vulkan_image_upload(image, 0, 0, 0, 0, 0, width, height, 1, texels, width * 4, width * height * 4);
	free(texels);
	return image;
}

/* the passes' images, of the target's size (SMAA's only for SMAA); color
is the target's format, so that the copy is exact */
static int images_fit(int smaa, uint8_t format, uint32_t width, uint32_t height)
{
	if (post.width != width || post.height != height || post.format != format || !post.color)
	{
		xgpu_vulkan_image_destroy(post.color);
		xgpu_vulkan_image_destroy(post.edges);
		xgpu_vulkan_image_destroy(post.weights);
		post.color = post.edges = post.weights = NULL;
		post.width = width;
		post.height = height;
		post.format = format;
		post.color = image_make(format, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET, width, height);
	}
	if (smaa && !post.edges)
	{
		post.edges = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
		post.weights = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
	}
	return post.color && (!smaa || (post.edges && post.weights));
}

/* ---------- pipelines */

/* the passes' sampler, made the first time: 1 if it is */
static int sampler_make(void)
{
	struct xgpu_vulkan_sampler_description sampler;

	if (post.sampler)
		return 1;
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	post.sampler = xgpu_vulkan_sampler(&sampler);
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
	post.nearest = xgpu_vulkan_sampler(&sampler);
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
	sampler.mip = XGPU_VULKAN_MIP_LINEAR;
	post.levels = xgpu_vulkan_sampler(&sampler);
	return post.sampler != VK_NULL_HANDLE && post.nearest != VK_NULL_HANDLE && post.levels != VK_NULL_HANDLE;
}

static int prepare_failed(int smaa, const char *what)
{
	host_logf(HOST_LOG_WARN, "vulkan: %s's %s cannot be made, so the 3D view is not antialiased",
		smaa ? "SMAA" : "FXAA", what);
	post.failed[smaa] = 1;
	return 0;
}

/* a pass's pipeline for a target of a format: FXAA's and SMAA's blend into
the target's (display.hdr's half floats, or 8 bits), the rest into their
own 8-bit images */
static struct xgpu_vulkan_pipeline_description *pipeline_of(int program, uint8_t format)
{
	return format == XGPU_VULKAN_FORMAT_RGBA16F && (program == XGPU_VULKAN_POST_FXAA ||
		program == XGPU_VULKAN_POST_SMAA_BLEND || program == XGPU_VULKAN_POST_PANINI ||
		program == XGPU_VULKAN_POST_DEFERRED_RESOLVE) ?
		&post.half_float[program] : &post.pipelines[program];
}

int xgpu_vulkan_post_prepare(int smaa, uint8_t format, int preparing)
{
	int first = smaa ? XGPU_VULKAN_POST_SMAA_EDGES : XGPU_VULKAN_POST_FXAA;
	int last = smaa ? XGPU_VULKAN_POST_SMAA_BLEND : XGPU_VULKAN_POST_FXAA;
	int program;

	smaa = smaa != 0;
	if (format != XGPU_VULKAN_FORMAT_RGBA16F)
		format = XGPU_VULKAN_FORMAT_BGRA8;
	if (post.failed[smaa])
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	for (program = first; program <= last; program++)
	{
		struct xgpu_vulkan_pipeline_description *description = pipeline_of(program, format);
		int into_target = program == XGPU_VULKAN_POST_FXAA || program == XGPU_VULKAN_POST_SMAA_BLEND;

		if (!post.pixel[program])
			post.pixel[program] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[program].words,
				xgpu_vulkan_post_programs[program].count);
		if (!post.vertex || !post.pixel[program])
			return prepare_failed(smaa, "shaders");
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[program];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		/* (into the target its colour alone; into an intermediate, all
		of it) */
		description->color_mask = into_target ? 7 : 15;
		description->color_format = into_target ? format : XGPU_VULKAN_FORMAT_BGRA8;
		description->samples = 1;
		if (preparing ? !xgpu_vulkan_pipeline_prepare(description) : !xgpu_vulkan_pipeline(description))
			return prepare_failed(smaa, "pipelines");
	}
	if (smaa && !post.area)
	{
		post.area = lookup_texture(xgpu_vulkan_post_area_texture, XGPU_VULKAN_POST_AREA_WIDTH,
			XGPU_VULKAN_POST_AREA_HEIGHT, 2);
		post.search = lookup_texture(xgpu_vulkan_post_search_texture, XGPU_VULKAN_POST_SEARCH_WIDTH,
			XGPU_VULKAN_POST_SEARCH_HEIGHT, 1);
		if (!post.area || !post.search)
			return prepare_failed(smaa, "lookup textures");
	}
	if (!sampler_make())
		return prepare_failed(smaa, "sampler");
	return 1;
}

int xgpu_vulkan_post_rotate_prepare(void)
{
	struct xgpu_vulkan_pipeline_description *description = &post.pipelines[XGPU_VULKAN_POST_ROTATE];

	if (post.rotate)
		return 1;
	if (post.rotate_failed)
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_ROTATE])
		post.pixel[XGPU_VULKAN_POST_ROTATE] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[XGPU_VULKAN_POST_ROTATE].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_ROTATE].count);
	if (post.vertex && post.pixel[XGPU_VULKAN_POST_ROTATE] && sampler_make())
	{
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[XGPU_VULKAN_POST_ROTATE];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		description->color_mask = 15;
		description->color_format = XGPU_VULKAN_FORMAT_BGRA8;
		description->samples = 1;
		post.rotate = xgpu_vulkan_pipeline(description);
	}
	if (!post.rotate)
	{
		host_logf(HOST_LOG_WARN, "vulkan: the frame cannot be turned for the display: the compositor turns it");
		post.rotate_failed = 1;
	}
	return post.rotate != VK_NULL_HANDLE;
}

/* ---------- the passes */

/* a program's triangle drawn into a layer of an image over the rectangle,
sampling images (NULL: none) */
static void pass_draw_with(int program, struct xgpu_vulkan_image *into, uint32_t layer, const int32_t rectangle[4],
	struct xgpu_vulkan_image *image0, struct xgpu_vulkan_image *image1, struct xgpu_vulkan_image *image2,
	const float bounds[4], const float window[4], const float panini[8])
{
	uint8_t format = xgpu_vulkan_image_description(into)->format;
	uint32_t into_samples = xgpu_vulkan_image_description(into)->samples;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_image *images[3];
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage;

	memset(&draw, 0, sizeof(draw));
	/* (of the samples of the image drawn into: a multisampled attachment's
	every sample given its pixel's value) */
	pipeline = *pipeline_of(program, format);
	pipeline.samples = (uint8_t)(into_samples > 1 ? into_samples : 1);
	draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
	if (!draw.pipeline)
		return;
	images[0] = image0;
	images[1] = image1;
	images[2] = image2;
	for (stage = 0; stage < 4; stage++)
	{
		/* (the fourth: the deferred resolve's G-buffer alone) */
		draw.textures[stage].image = stage < 3 ? images[stage] : post.gbuffer;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = stage < 3 ? post.sampler : post.nearest;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)post.width;
	uniforms->metrics[1] = 1.0f / (float)post.height;
	uniforms->metrics[2] = (float)post.width;
	uniforms->metrics[3] = (float)post.height;
	if (bounds)
		memcpy(uniforms->bounds, bounds, sizeof(uniforms->bounds));
	if (window)
		memcpy(uniforms->window, window, sizeof(uniforms->window));
	if (panini)
	{
		memcpy(uniforms->panini, panini, sizeof(uniforms->panini));
		memcpy(uniforms->tangents, panini + 4, sizeof(uniforms->tangents));
	}
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(into, NULL, layer, 1);
	xgpu_vulkan_draw(&draw);
}

static void pass_draw(int program, struct xgpu_vulkan_image *into, uint32_t layer, const int32_t rectangle[4],
	struct xgpu_vulkan_image *image0, struct xgpu_vulkan_image *image1, struct xgpu_vulkan_image *image2,
	const float bounds[4])
{
	pass_draw_with(program, into, layer, rectangle, image0, image1, image2, bounds, NULL, NULL);
}

/* an intermediate emptied, all of it, so that no edge of another window's
pass is found: SMAA's passes write only where there is an edge */
static void intermediate_clear(struct xgpu_vulkan_image *image)
{
	static const float transparent[4];
	int32_t whole[1][4];

	whole[0][0] = whole[0][1] = 0;
	whole[0][2] = (int32_t)post.width;
	whole[0][3] = (int32_t)post.height;
	xgpu_vulkan_set_targets(image, NULL, 0);
	xgpu_vulkan_clear((const int32_t (*)[4])whole, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS,
		transparent, 1.0f, 0);
}

int xgpu_vulkan_post_anti_alias(int smaa, struct xgpu_vulkan_image *target, uint32_t layer, const int32_t corners[4])
{
	const struct xgpu_vulkan_image_description *description;
	int32_t rectangle[4];
	int index;

	smaa = smaa != 0;
	if (!target)
		return 0;
	description = xgpu_vulkan_image_description(target);
	if ((description->format != XGPU_VULKAN_FORMAT_BGRA8 && description->format != XGPU_VULKAN_FORMAT_RGBA16F) ||
		description->samples > 1)
		return 0;
	/* (made already, as the setting was chosen) */
	if (!xgpu_vulkan_post_prepare(smaa, description->format, 0))
		return 0;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	if (!images_fit(smaa, description->format, description->width, description->height))
		return 0;

	/* the target's pixels, read while the target is drawn into */
	xgpu_vulkan_image_copy_rectangle(target, layer, post.color, rectangle);
	if (!smaa)
	{
		float bounds[4];

		/* (split screen: no texel of the next window is read) */
		bounds[0] = ((float)rectangle[0] + 0.5f) / (float)post.width;
		bounds[1] = ((float)rectangle[1] + 0.5f) / (float)post.height;
		bounds[2] = ((float)rectangle[2] - 0.5f) / (float)post.width;
		bounds[3] = ((float)rectangle[3] - 0.5f) / (float)post.height;
		pass_draw(XGPU_VULKAN_POST_FXAA, target, layer, rectangle, post.color, NULL, NULL, bounds);
	}
	else
	{
		intermediate_clear(post.edges);
		pass_draw(XGPU_VULKAN_POST_SMAA_EDGES, post.edges, 0, rectangle, post.color, NULL, NULL, NULL);

		intermediate_clear(post.weights);
		pass_draw(XGPU_VULKAN_POST_SMAA_WEIGHTS, post.weights, 0, rectangle, post.edges, post.area, post.search, NULL);

		pass_draw(XGPU_VULKAN_POST_SMAA_BLEND, target, layer, rectangle, post.color, post.weights, NULL, NULL);
	}
	return 1;
}

/* ---------- display.lighting "ce_deferred" (xgpu_vulkan_stream.c)

The resolve of CE's passes drawn apart: the colour target's pixels (the
light) copied, as FXAA's are, then each pixel drawn from the copy, the base
and the additions (DEFERRED_RESOLVE: light x base + additions, the
additions' alpha; or one of them alone: view). All four channels: the
alpha is CE's destination alpha after its passes. A target of one sample
only (the stream draws a multisampled one as "ce"). */

int xgpu_vulkan_post_deferred_resolve(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, struct xgpu_vulkan_image *gbuffer, struct xgpu_vulkan_image *extras,
	struct xgpu_vulkan_image *depth, int view,
	struct xgpu_vulkan_image *(*relight)(struct xgpu_vulkan_image *light, struct xgpu_vulkan_image *base,
	struct xgpu_vulkan_image *additions, int *whole, void *context), void *context)
{
	int whole = 0;
	struct xgpu_vulkan_image *light = NULL;
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_pipeline_description *pipeline;
	int32_t rectangle[4];
	float bounds[4];

	if (!target || !base || !additions)
		return 0;
	description = xgpu_vulkan_image_description(target);
	if ((description->format != XGPU_VULKAN_FORMAT_BGRA8 && description->format != XGPU_VULKAN_FORMAT_RGBA16F) ||
		description->samples > 1)
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_DEFERRED_RESOLVE])
		post.pixel[XGPU_VULKAN_POST_DEFERRED_RESOLVE] = xgpu_vulkan_shader(
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_RESOLVE].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_RESOLVE].count);
	if (!post.vertex || !post.pixel[XGPU_VULKAN_POST_DEFERRED_RESOLVE] || !sampler_make())
		return 0;
	pipeline = pipeline_of(XGPU_VULKAN_POST_DEFERRED_RESOLVE, description->format);
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex_shader = post.vertex;
	pipeline->pixel_shader = post.pixel[XGPU_VULKAN_POST_DEFERRED_RESOLVE];
	pipeline->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	pipeline->color_mask = 15;
	pipeline->color_format = description->format;
	pipeline->samples = 1;
	if (!images_fit(0, description->format, description->width, description->height))
		return 0;
	rectangle[0] = rectangle[1] = 0;
	rectangle[2] = (int32_t)description->width;
	rectangle[3] = (int32_t)description->height;
	xgpu_vulkan_image_copy_rectangle(target, 0, post.color, rectangle);
	memset(bounds, 0, sizeof(bounds));
	bounds[0] = (float)view;
	/* (the cleared depth, the farthest: reversed, 0) */
	bounds[1] = XGPU_VULKAN_REVERSED_DEPTH ? 0.0f : 1.0f;
	bounds[2] = gbuffer ? 1.0f : 0.0f;
	/* (a stand-in where there is none: its texels are never read) */
	if (!gbuffer && !post.gbuffer_stand_in)
	{
		struct xgpu_vulkan_image_description stand_in;

		memset(&stand_in, 0, sizeof(stand_in));
		stand_in.type = XGPU_VULKAN_IMAGE_2D;
		stand_in.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
		stand_in.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
		stand_in.width = stand_in.height = 1;
		stand_in.depth_or_layers = 1;
		stand_in.levels = 1;
		post.gbuffer_stand_in = xgpu_vulkan_image_create(&stand_in);
	}
	post.gbuffer = gbuffer ? gbuffer : post.gbuffer_stand_in;
	/* (the motion's and the emissive light's views read the second target's
	words in the G-buffer's place) */
	if ((view == XGPU_GBUFFER_VIEW_MOTION || view == XGPU_GBUFFER_VIEW_EMISSIVE) && gbuffer)
	{
		post.gbuffer = extras ? extras : post.gbuffer_stand_in;
		bounds[2] = extras ? 1.0f : 0.0f;
	}
	/* (display.lighting "raster": the light made of the copy by the bake) */
	if (relight)
		light = relight(post.color, base, additions, &whole, context);
	/* (PRISM Raster's frame whole: the resolve draws it as it is, with the
	additions' alpha, CE's destination alpha) */
	if (light && whole && view == 0)
		bounds[0] = 2.0f;
	pass_draw(XGPU_VULKAN_POST_DEFERRED_RESOLVE, target, 0, rectangle, light ? light : post.color, base,
		view == 8 && depth ? depth : additions, bounds);
	post.gbuffer = NULL;
	return 1;
}

/* ---------- display.lighting's v4 fog (xgpu_vulkan_stream.c's SCENE_FOG)

The window's fog as a pass over its G-buffer (DEFERRED_FOG; xgpu_stream.h's
SCENE_FOG), in place of CE's fog drawn over the level again and in the
objects' own draws: each opaque pixel's fog found from its distance and
blended into the target as CE's fog pass blends it (one, and one less the
source's alpha; the colour alone), or the fog screen's density written into
the target's alpha alone. Its words and its second target's fetched, never
filtered. */

int xgpu_vulkan_post_fog(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *words,
	struct xgpu_vulkan_image *extras, const struct xgpu_stream_scene_view *view,
	const struct xgpu_stream_scene_fog *fog)
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int screen, stage, axis;

	if (!target || !words || !extras || !view || !fog)
		return 0;
	screen = (fog->flags & XGPU_SCENE_FOG_FLAG_SCREEN) != 0;
	description = xgpu_vulkan_image_description(target);
	if ((description->format != XGPU_VULKAN_FORMAT_BGRA8 && description->format != XGPU_VULKAN_FORMAT_RGBA16F) ||
		description->samples > 1)
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_DEFERRED_FOG])
		post.pixel[XGPU_VULKAN_POST_DEFERRED_FOG] = xgpu_vulkan_shader(
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_FOG].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_FOG].count);
	if (!post.vertex || !post.pixel[XGPU_VULKAN_POST_DEFERRED_FOG] || !sampler_make())
		return 0;
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.vertex_shader = post.vertex;
	pipeline.pixel_shader = post.pixel[XGPU_VULKAN_POST_DEFERRED_FOG];
	pipeline.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	pipeline.color_format = description->format;
	pipeline.samples = 1;
	if (screen)
		pipeline.color_mask = 8;
	else
	{
		/* (one, and one less the source's alpha: xgpu_vulkan.c's factors 1
		and 5) */
		pipeline.color_mask = 7;
		pipeline.blend = 1;
		pipeline.blend_source = 1;
		pipeline.blend_destination = 5;
	}
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
	if (!draw.pipeline)
		return 0;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage == 2 ? extras : stage == 3 ? words : NULL;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.nearest;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return 0;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)description->width;
	uniforms->metrics[1] = 1.0f / (float)description->height;
	uniforms->metrics[2] = (float)description->width;
	uniforms->metrics[3] = (float)description->height;
	for (axis = 0; axis < 3; axis++)
	{
		uniforms->camera_eye[axis] = view->now[0][axis];
		uniforms->camera_axis[axis] = view->now[1][axis];
		uniforms->camera_x[axis] = view->now[2][axis];
		uniforms->camera_y[axis] = view->now[3][axis];
	}
	for (axis = 0; axis < 4; axis++)
		uniforms->view_corners[axis] = (float)view->corners[axis];
	memcpy(uniforms->fog_atmosphere, fog->atmosphere_color, sizeof(uniforms->fog_atmosphere));
	uniforms->fog_range[0] = fog->atmosphere_range[0];
	/* (a range of none: one so small that the atmosphere is whole past its
	start, as CE's tables saturate) */
	uniforms->fog_range[1] = fog->atmosphere_range[1] > fog->atmosphere_range[0] ? fog->atmosphere_range[1] :
		fog->atmosphere_range[0] + 1.0e-6f;
	uniforms->fog_range[2] = fog->atmosphere_range[2];
	uniforms->fog_range[3] = screen ? 1.0f : 0.0f;
	if (fog->plane_count)
	{
		memcpy(uniforms->fog_plane, fog->planes[0].plane, sizeof(uniforms->fog_plane));
		memcpy(uniforms->fog_planar, fog->planes[0].color, sizeof(uniforms->fog_planar));
		memcpy(uniforms->fog_shape, fog->planes[0].shape, sizeof(uniforms->fog_shape));
	}
	/* (no plane: its density none, its shape any that divides) */
	if (!fog->plane_count || !(uniforms->fog_shape[0] > 0.0f) || !(uniforms->fog_shape[1] > 0.0f))
	{
		uniforms->fog_planar[3] = 0.0f;
		uniforms->fog_shape[0] = uniforms->fog_shape[1] = 1.0f;
		uniforms->fog_plane[2] = 1.0f;
	}
	uniforms->fog_shape[3] = (float)(((fog->flags & XGPU_SCENE_FOG_FLAG_SCREEN_LEVEL) ? 1 : 0) |
		((fog->flags & XGPU_SCENE_FOG_FLAG_SCREEN_OBJECTS) ? 2 : 0));
	uniforms->fog_screen[0] = fog->screen[0];
	uniforms->fog_screen[1] = fog->screen[1] > fog->screen[0] ? fog->screen[1] : fog->screen[0] + 1.0e-6f;
	uniforms->fog_screen[2] = fog->screen[2];
	draw.constants = draw.uniforms;
	/* (over the window's view alone, as CE's fog pass and its fog screen's
	clear: another window's pixels of a split screen are not touched) */
	memset(&dynamic, 0, sizeof(dynamic));
	for (axis = 0; axis < 2; axis++)
	{
		int32_t size = (int32_t)(axis ? description->height : description->width);
		int32_t from = view->corners[axis] < 0 ? 0 : view->corners[axis] > size ? size : view->corners[axis];
		int32_t to = view->corners[axis + 2] < from ? from : view->corners[axis + 2] > size ? size :
			view->corners[axis + 2];

		dynamic.viewport[axis] = dynamic.scissor[axis] = from;
		dynamic.viewport[axis + 2] = dynamic.scissor[axis + 2] = to - from;
	}
	if (dynamic.viewport[2] <= 0 || dynamic.viewport[3] <= 0)
		return 1;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(target, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return 1;
}

/* ---------- display.lighting's v4 projected decals (xgpu_vulkan_stream.c's
SCENE_DECAL)

A decal projected by its box onto the level's pixels of the window's
G-buffer (DEFERRED_DECAL), over the rectangle of the screen the box covers,
into the target the window's draws go to (the base colour, or the light),
blended by CE's framebuffer blend function as its decal draws are: the
pipeline's factors and operation (xgpu_vulkan.c's numbers), and the
channels its draws write (every one where they multiply, else the colour). */

static const uint8_t decal_blends[XGPU_SCENE_DECAL_BLENDS][4] =
{
	/* source, destination, operation, channels */
	{ 4, 5, 0, 7 },     /* alpha: source alpha, one less it */
	{ 8, 0, 0, 15 },    /* multiply: destination colour, zero */
	{ 8, 2, 0, 15 },    /* double multiply: destination colour, source colour */
	{ 1, 1, 0, 7 },     /* add */
	{ 1, 1, 2, 7 },     /* subtract: the destination less the source */
	{ 1, 1, 3, 7 },     /* min */
	{ 1, 1, 4, 7 },     /* max */
	{ 1, 5, 0, 7 },     /* alpha multiply add: one, one less source alpha */
};

int xgpu_vulkan_post_decal(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *bitmap,
	struct xgpu_vulkan_image *words, struct xgpu_vulkan_image *extras, const struct xgpu_stream_scene_view *view,
	const struct xgpu_stream_scene_decal *decal)
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	double axis2 = 0.0, x2 = 0.0, y2 = 0.0, left, top, width, height, box[4];
	int stage, axis, corner, whole = 0;

	if (!target || !bitmap || !words || !extras || !view || !decal || decal->blend >= XGPU_SCENE_DECAL_BLENDS)
		return 0;
	/* (a bitmap of another kind than the 2D texture the pass samples: none,
	as a draw's stage takes none, a cube map's or a volume's view in a 2D
	sampler being undefined) */
	description = xgpu_vulkan_image_description(bitmap);
	if (description->type != XGPU_VULKAN_IMAGE_2D && description->type != XGPU_VULKAN_IMAGE_2D_ARRAY)
		return 0;
	description = xgpu_vulkan_image_description(target);
	if ((description->format != XGPU_VULKAN_FORMAT_BGRA8 && description->format != XGPU_VULKAN_FORMAT_RGBA16F) ||
		description->samples > 1)
		return 0;
	/* the rectangle of the screen the box covers: its corners as the camera
	sees them (the whole view where one is not in front of it), a pixel
	wider, inside the view */
	left = view->corners[0];
	top = view->corners[1];
	width = view->corners[2] - left;
	height = view->corners[3] - top;
	if (!(width > 0.0) || !(height > 0.0))
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		axis2 += (double)view->now[1][axis] * view->now[1][axis];
		x2 += (double)view->now[2][axis] * view->now[2][axis];
		y2 += (double)view->now[3][axis] * view->now[3][axis];
	}
	if (!(axis2 > 1.0e-12) || !(x2 > 1.0e-12) || !(y2 > 1.0e-12))
		return 0;
	box[0] = box[1] = 1.0e30;
	box[2] = box[3] = -1.0e30;
	for (corner = 0; corner < 8 && !whole; corner++)
	{
		double point[3], along = 0.0, across = 0.0, down = 0.0;

		for (axis = 0; axis < 3; axis++)
		{
			point[axis] = (double)decal->centre[axis] - view->now[0][axis] +
				(corner & 1 ? 1.0 : -1.0) * decal->axes[0][axis] + (corner & 2 ? 1.0 : -1.0) * decal->axes[1][axis] +
				(corner & 4 ? 1.0 : -1.0) * decal->axes[2][axis];
			along += point[axis] * view->now[1][axis];
			across += point[axis] * view->now[2][axis];
			down += point[axis] * view->now[3][axis];
		}
		along /= axis2;
		if (!(along > 1.0e-3))
		{
			whole = 1;
			break;
		}
		across = across / x2 / along;
		down = down / y2 / along;
		box[0] = across < box[0] ? across : box[0];
		box[1] = down < box[1] ? down : box[1];
		box[2] = across > box[2] ? across : box[2];
		box[3] = down > box[3] ? down : box[3];
	}
	{
		int32_t rectangle[4];

		if (whole)
		{
			rectangle[0] = view->corners[0];
			rectangle[1] = view->corners[1];
			rectangle[2] = view->corners[2];
			rectangle[3] = view->corners[3];
		}
		else
		{
			double x0 = left + (box[0] + 1.0) * 0.5 * width - 1.0, y0 = top + (box[1] + 1.0) * 0.5 * height - 1.0;
			double x1 = left + (box[2] + 1.0) * 0.5 * width + 1.0, y1 = top + (box[3] + 1.0) * 0.5 * height + 1.0;

			rectangle[0] = (int32_t)(x0 < left ? left : x0 > view->corners[2] ? view->corners[2] : x0);
			rectangle[1] = (int32_t)(y0 < top ? top : y0 > view->corners[3] ? view->corners[3] : y0);
			rectangle[2] = (int32_t)(x1 < left ? left : x1 > view->corners[2] ? view->corners[2] : x1 + 1.0);
			rectangle[3] = (int32_t)(y1 < top ? top : y1 > view->corners[3] ? view->corners[3] : y1 + 1.0);
		}
		if (rectangle[0] < 0)
			rectangle[0] = 0;
		if (rectangle[1] < 0)
			rectangle[1] = 0;
		if (rectangle[2] > (int32_t)description->width)
			rectangle[2] = (int32_t)description->width;
		if (rectangle[3] > (int32_t)description->height)
			rectangle[3] = (int32_t)description->height;
		if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
			return 1;
		if (!post.vertex)
			post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
		if (!post.pixel[XGPU_VULKAN_POST_DEFERRED_DECAL])
			post.pixel[XGPU_VULKAN_POST_DEFERRED_DECAL] = xgpu_vulkan_shader(
				xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_DECAL].words,
				xgpu_vulkan_post_programs[XGPU_VULKAN_POST_DEFERRED_DECAL].count);
		if (!post.vertex || !post.pixel[XGPU_VULKAN_POST_DEFERRED_DECAL] || !sampler_make())
			return 0;
		memset(&pipeline, 0, sizeof(pipeline));
		pipeline.vertex_shader = post.vertex;
		pipeline.pixel_shader = post.pixel[XGPU_VULKAN_POST_DEFERRED_DECAL];
		pipeline.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		pipeline.color_format = description->format;
		pipeline.samples = 1;
		pipeline.blend = 1;
		pipeline.blend_source = decal_blends[decal->blend][0];
		pipeline.blend_destination = decal_blends[decal->blend][1];
		pipeline.blend_operation = decal_blends[decal->blend][2];
		pipeline.color_mask = decal_blends[decal->blend][3];
		memset(&draw, 0, sizeof(draw));
		draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
		if (!draw.pipeline)
			return 0;
		for (stage = 0; stage < 4; stage++)
		{
			draw.textures[stage].image = stage == 0 ? bitmap : stage == 2 ? extras : stage == 3 ? words : NULL;
			draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
			draw.textures[stage].sampler = stage == 0 ? post.levels : post.nearest;
		}
		uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
		if (!uniforms)
			return 0;
		memset(uniforms, 0, sizeof(*uniforms));
		uniforms->metrics[0] = 1.0f / (float)description->width;
		uniforms->metrics[1] = 1.0f / (float)description->height;
		uniforms->metrics[2] = (float)description->width;
		uniforms->metrics[3] = (float)description->height;
		for (axis = 0; axis < 3; axis++)
		{
			uniforms->camera_eye[axis] = view->now[0][axis];
			uniforms->camera_axis[axis] = view->now[1][axis];
			uniforms->camera_x[axis] = view->now[2][axis];
			uniforms->camera_y[axis] = view->now[3][axis];
		}
		for (axis = 0; axis < 4; axis++)
			uniforms->view_corners[axis] = (float)view->corners[axis];
		{
			double lengths[3];
			int which;

			for (which = 0; which < 3; which++)
			{
				lengths[which] = 0.0;
				for (axis = 0; axis < 3; axis++)
					lengths[which] += (double)decal->axes[which][axis] * decal->axes[which][axis];
				if (!(lengths[which] > 1.0e-12))
					return 1;
			}
			for (axis = 0; axis < 3; axis++)
			{
				uniforms->decal_centre[axis] = decal->centre[axis];
				uniforms->decal_x[axis] = (float)(decal->axes[0][axis] / lengths[0]);
				uniforms->decal_y[axis] = (float)(decal->axes[1][axis] / lengths[1]);
				uniforms->decal_z[axis] = (float)(decal->axes[2][axis] / lengths[2]);
			}
			uniforms->decal_centre[3] = (float)decal->blend;
			uniforms->decal_x[3] = (decal->flags & XGPU_SCENE_DECAL_FLAG_ALPHA_TESTED) ? 1.0f : 0.0f;
			memcpy(uniforms->decal_color, decal->color, sizeof(uniforms->decal_color));
			memcpy(uniforms->decal_rectangle, decal->rectangle, sizeof(uniforms->decal_rectangle));
			/* (a texel across the box: its width over the sprite's texels; a
			pixel at a unit's distance: the view's width there over its pixels) */
			uniforms->decal_lod[0] = (float)(2.0 * sqrt(lengths[0]) / ((double)xgpu_vulkan_image_description(bitmap)->width *
				fabs((double)decal->rectangle[2] - decal->rectangle[0]) + 1.0e-6));
			uniforms->decal_lod[2] = (float)(2.0 * sqrt(x2) / width);
		}
		draw.constants = draw.uniforms;
		memset(&dynamic, 0, sizeof(dynamic));
		dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
		dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
		dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
		dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
		dynamic.depth_range[1] = 1.0f;
		draw.dynamic = &dynamic;
		draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
		draw.count = 3;
		xgpu_vulkan_set_target_views(target, NULL, 0, 1);
		xgpu_vulkan_draw(&draw);
	}
	return 1;
}

/* ---------- display.panini (port/linux/game/panini.c)

The window's 3D view drawn again through the Panini projection, a pass like
FXAA's: the target's pixels copied, then each pixel of the rectangle drawn
from the copy where the rectilinear view saw what the projection puts there
(tools/vulkan_post_shaders.py's PANINI_SOURCE). All four channels: the
game's own values in destination alpha belong to the pixels they are moved
with. With multisampling the copy is of the resolved pixels and the pass
draws into the multisampled attachment, which the HUD is drawn into after
and which is resolved over the target's pixels at the frame's end: drawn
into the resolved pixels alone, the pass would be lost to that resolve. */

int xgpu_vulkan_post_panini_prepare(uint8_t format, uint32_t samples, int preparing)
{
	struct xgpu_vulkan_pipeline_description *description, multisampled;

	if (format != XGPU_VULKAN_FORMAT_RGBA16F)
		format = XGPU_VULKAN_FORMAT_BGRA8;
	if (post.panini_failed)
		return 0;
	description = pipeline_of(XGPU_VULKAN_POST_PANINI, format);
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_PANINI])
		post.pixel[XGPU_VULKAN_POST_PANINI] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PANINI].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PANINI].count);
	if (post.vertex && post.pixel[XGPU_VULKAN_POST_PANINI] && sampler_make())
	{
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[XGPU_VULKAN_POST_PANINI];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		description->color_mask = 15;
		description->color_format = format;
		description->samples = 1;
		/* (the pipeline of the target's samples: pass_draw_with's) */
		multisampled = *description;
		multisampled.samples = (uint8_t)(samples > 1 ? samples : 1);
		if (preparing ? xgpu_vulkan_pipeline_prepare(&multisampled) :
			xgpu_vulkan_pipeline(&multisampled) != VK_NULL_HANDLE)
			return 1;
	}
	host_logf(HOST_LOG_WARN, "vulkan: the Panini projection's pass cannot be made, so the 3D view is drawn flat");
	post.panini_failed = 1;
	return 0;
}

int xgpu_vulkan_post_panini(struct xgpu_vulkan_image *source, struct xgpu_vulkan_image *into, uint32_t layer,
	const int32_t corners[4], const float projection[8])
{
	const struct xgpu_vulkan_image_description *description, *into_description;
	int32_t rectangle[4];
	float bounds[4], window[4];
	int index;

	if (!source || !into)
		return 0;
	description = xgpu_vulkan_image_description(source);
	into_description = xgpu_vulkan_image_description(into);
	if ((description->format != XGPU_VULKAN_FORMAT_BGRA8 && description->format != XGPU_VULKAN_FORMAT_RGBA16F) ||
		description->samples > 1 || into_description->format != description->format ||
		into_description->width != description->width || into_description->height != description->height)
		return 0;
	/* (made already, as the map was prepared, or now) */
	if (!xgpu_vulkan_post_panini_prepare(description->format, into_description->samples, 0))
		return 0;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	/* (the copy, FXAA's) */
	if (!images_fit(0, description->format, description->width, description->height))
		return 0;
	xgpu_vulkan_image_copy_rectangle(source, layer, post.color, rectangle);
	/* (split screen: no texel of the next window is read) */
	bounds[0] = ((float)rectangle[0] + 0.5f) / (float)post.width;
	bounds[1] = ((float)rectangle[1] + 0.5f) / (float)post.height;
	bounds[2] = ((float)rectangle[2] - 0.5f) / (float)post.width;
	bounds[3] = ((float)rectangle[3] - 0.5f) / (float)post.height;
	/* (the view's own corners, which the tangents are at) */
	window[0] = (float)corners[0];
	window[1] = (float)corners[1];
	window[2] = (float)(corners[2] - corners[0]);
	window[3] = (float)(corners[3] - corners[1]);
	pass_draw_with(XGPU_VULKAN_POST_PANINI, into, layer, rectangle, post.color, NULL, NULL, bounds, window,
		projection);
	return 1;
}

/* the turn's matrix for each count of quarter turns clockwise: rows from
the turned image's coordinates (about its centre) to the frame's */
static const float rotate_rows[4][4] =
{
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, -1.0f, 0.0f },
	{ -1.0f, 0.0f, 0.0f, -1.0f },
	{ 0.0f, -1.0f, 1.0f, 0.0f },
};

struct xgpu_vulkan_image *xgpu_vulkan_post_rotate(struct xgpu_vulkan_image *image, int quarter_turns)
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	uint32_t width, height;
	int stage;

	if (!image || !post.rotate || quarter_turns < 1 || quarter_turns > 3)
		return NULL;
	description = xgpu_vulkan_image_description(image);
	width = quarter_turns & 1 ? description->height : description->width;
	height = quarter_turns & 1 ? description->width : description->height;
	if (post.turned && (xgpu_vulkan_image_description(post.turned)->width != width ||
		xgpu_vulkan_image_description(post.turned)->height != height))
	{
		xgpu_vulkan_image_destroy(post.turned);
		post.turned = NULL;
	}
	if (!post.turned)
		post.turned = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
	if (!post.turned)
		return NULL;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = post.rotate;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : image;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return NULL;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)width;
	uniforms->metrics[1] = 1.0f / (float)height;
	uniforms->metrics[2] = (float)width;
	uniforms->metrics[3] = (float)height;
	memcpy(uniforms->bounds, rotate_rows[quarter_turns], sizeof(uniforms->bounds));
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.scissor[2] = (int32_t)width;
	dynamic.viewport[3] = dynamic.scissor[3] = (int32_t)height;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(post.turned, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return post.turned;
}

/* ---------- display.hdr (xgpu_hdr.c's shaders) */

/* the shader modules of a program and the passes' vertex shader: 1 if
they are made */
static int shaders_make(int program)
{
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[program])
		post.pixel[program] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[program].words,
			xgpu_vulkan_post_programs[program].count);
	return post.vertex && post.pixel[program];
}

/* the tone map's pipeline: into an RGBA16F target of its samples, with its
depth attachment's format (none tested or written), its colour alone,
reading the pixel it draws over */
static void tone_map_description(uint8_t depth_format, uint32_t samples,
	struct xgpu_vulkan_pipeline_description *description)
{
	int program = samples > 1 ? XGPU_VULKAN_POST_HDR_TONE_MAP_MS : XGPU_VULKAN_POST_HDR_TONE_MAP;

	memset(description, 0, sizeof(*description));
	description->vertex_shader = post.vertex;
	description->pixel_shader = post.pixel[program];
	description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	description->color_mask = 7;
	description->color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	description->depth_format = depth_format == XGPU_VULKAN_FORMAT_D24S8 ? XGPU_VULKAN_FORMAT_D24S8 :
		XGPU_VULKAN_FORMAT_NONE;
	description->samples = (uint8_t)(samples > 1 ? samples : 1);
	description->color_read = 1;
}

int xgpu_vulkan_post_prepare_hdr(uint8_t depth_format, uint32_t samples, int preparing)
{
	struct xgpu_vulkan_pipeline_description description;
	int program = samples > 1 ? XGPU_VULKAN_POST_HDR_TONE_MAP_MS : XGPU_VULKAN_POST_HDR_TONE_MAP;
	int made;

	if (post.tone_map_failed)
		return 0;
	made = shaders_make(program);
	if (made)
	{
		tone_map_description(depth_format, samples, &description);
		made = preparing ? xgpu_vulkan_pipeline_prepare(&description) :
			xgpu_vulkan_pipeline(&description) != VK_NULL_HANDLE;
	}
	if (!made)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.hdr's tone map cannot be made: highlights past the display's peak clip");
		post.tone_map_failed = 1;
	}
	return made;
}

void xgpu_vulkan_post_tone_map(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *depth,
	const int32_t corners[4], const float hdr[4])
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	uint8_t depth_format = depth ? XGPU_VULKAN_FORMAT_D24S8 : XGPU_VULKAN_FORMAT_NONE;
	int32_t rectangle[4];
	int index, stage;

	if (!target)
		return;
	description = xgpu_vulkan_image_description(target);
	if (description->format != XGPU_VULKAN_FORMAT_RGBA16F ||
		!xgpu_vulkan_post_prepare_hdr(depth_format, description->samples, 0))
		return;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	tone_map_description(depth_format, description->samples, &pipeline);
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)description->width;
	uniforms->metrics[1] = 1.0f / (float)description->height;
	uniforms->metrics[2] = (float)description->width;
	uniforms->metrics[3] = (float)description->height;
	memcpy(uniforms->hdr, hdr, sizeof(uniforms->hdr));
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	draw.reads_color = 1;
	/* (the pass that drew the view, where these are its targets) */
	xgpu_vulkan_set_target_views(target, depth, 0, 1);
	xgpu_vulkan_draw(&draw);
}

/* ---------- display.prism_bloom (xgpu_backend.h's XGPU_BLOOM_*)

The window's 3D view, in half floats before its tone map, glows where its
light is past paper white: the rectangle copied (FXAA's copy), halved into
the first of the bloom's images by the 13-tap filter with the threshold
(each tap's light past 1 alone), halved again into each next (as many as
the mode's levels), then each added back into the one above it through the
3 by 3 tent (the pipeline's blend: one and one), and the first, through
the tent again, into the target over the rectangle by the mode's strength,
its colour alone (the game keeps its own values in alpha). Images of a
half, a quarter... of the target's size, sampled at the target's
coordinates scaled; each pass reads no texel outside the rectangle's
(bounds). A multisampled view (MSAA) has none. */

#define BLOOM_LEVELS_MOST 6

static const struct
{
	int levels;
	float strength;
} bloom_modes[3] = { { 0, 0.0f }, { 4, 0.06f }, { 6, 0.14f } };

static struct
{
	struct xgpu_vulkan_image *levels[BLOOM_LEVELS_MOST];
	uint32_t width, height;
	struct xgpu_vulkan_pipeline_description down, up;
	int failed, multisampled_logged;
} bloom;

static int bloom_prepare(void)
{
	if (bloom.failed)
		return 0;
	if (bloom.down.pixel_shader && bloom.up.pixel_shader)
		return 1;
	if (!shaders_make(XGPU_VULKAN_POST_BLOOM_DOWN) || !shaders_make(XGPU_VULKAN_POST_BLOOM_UP) || !sampler_make())
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.prism_bloom's passes cannot be made: no bloom");
		bloom.failed = 1;
		return 0;
	}
	memset(&bloom.down, 0, sizeof(bloom.down));
	bloom.down.vertex_shader = post.vertex;
	bloom.down.pixel_shader = post.pixel[XGPU_VULKAN_POST_BLOOM_DOWN];
	bloom.down.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	bloom.down.color_mask = 15;
	bloom.down.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	bloom.down.samples = 1;
	bloom.up = bloom.down;
	bloom.up.pixel_shader = post.pixel[XGPU_VULKAN_POST_BLOOM_UP];
	/* (added: one and one, XGPU_BLEND_ONE; its colour alone) */
	bloom.up.color_mask = 7;
	bloom.up.blend = 1;
	bloom.up.blend_source = bloom.up.blend_destination = 1;
	bloom.up.blend_operation = 0;
	return 1;
}

/* the images for a target's size: 1 if they are made */
static int bloom_images_fit(uint32_t width, uint32_t height)
{
	int level;

	if (bloom.width != width || bloom.height != height)
	{
		for (level = 0; level < BLOOM_LEVELS_MOST; level++)
		{
			xgpu_vulkan_image_destroy(bloom.levels[level]);
			bloom.levels[level] = NULL;
		}
		bloom.width = width;
		bloom.height = height;
	}
	for (level = 0; level < BLOOM_LEVELS_MOST; level++)
	{
		uint32_t w = width >> (level + 1), h = height >> (level + 1);

		if (!bloom.levels[level])
			bloom.levels[level] = image_make(XGPU_VULKAN_FORMAT_RGBA16F,
				XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET, w ? w : 1, h ? h : 1);
		if (!bloom.levels[level])
			return 0;
	}
	return 1;
}

/* a pass: source sampled (its rectangle's texels alone) into a rectangle
of into, by the pipeline, with the threshold and strength */
static void bloom_draw(const struct xgpu_vulkan_pipeline_description *description, struct xgpu_vulkan_image *into,
	const int32_t rectangle[4], struct xgpu_vulkan_image *source, const int32_t source_rectangle[4], float threshold,
	float strength)
{
	const struct xgpu_vulkan_image_description *from = xgpu_vulkan_image_description(source);
	const struct xgpu_vulkan_image_description *to = xgpu_vulkan_image_description(into);
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage;

	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline(description);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : source;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)from->width;
	uniforms->metrics[1] = 1.0f / (float)from->height;
	uniforms->metrics[2] = (float)from->width;
	uniforms->metrics[3] = (float)from->height;
	uniforms->bounds[0] = ((float)source_rectangle[0] + 0.5f) / (float)from->width;
	uniforms->bounds[1] = ((float)source_rectangle[1] + 0.5f) / (float)from->height;
	uniforms->bounds[2] = ((float)source_rectangle[2] - 0.5f) / (float)from->width;
	uniforms->bounds[3] = ((float)source_rectangle[3] - 0.5f) / (float)from->height;
	uniforms->hdr[0] = 1.0f / (float)to->width;
	uniforms->hdr[1] = 1.0f / (float)to->height;
	uniforms->hdr[2] = threshold;
	uniforms->hdr[3] = strength;
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

int xgpu_vulkan_post_bloom(struct xgpu_vulkan_image *target, const int32_t corners[4], int mode)
{
	const struct xgpu_vulkan_image_description *description;
	int32_t rectangles[BLOOM_LEVELS_MOST + 1][4];
	int index, level, levels;

	if (!target || mode <= 0 || mode >= (int)(sizeof(bloom_modes) / sizeof(bloom_modes[0])))
		return 0;
	description = xgpu_vulkan_image_description(target);
	if (description->format != XGPU_VULKAN_FORMAT_RGBA16F)
		return 0;
	if (description->samples > 1)
	{
		if (!bloom.multisampled_logged)
			host_logf(HOST_LOG_INFO, "vulkan: display.prism_bloom: none over a multisampled view (MSAA)");
		bloom.multisampled_logged = 1;
		return 0;
	}
	if (!bloom_prepare())
		return 0;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangles[0][index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangles[0][2] <= rectangles[0][0] || rectangles[0][3] <= rectangles[0][1])
		return 1;
	if (!images_fit(0, description->format, description->width, description->height) ||
		!bloom_images_fit(description->width, description->height))
		return 0;
	levels = bloom_modes[mode].levels;
	for (level = 1; level <= levels; level++)
	{
		for (index = 0; index < 4; index++)
		{
			/* (the half's pixels, out to the ones the rectangle's last reach) */
			int32_t value = index < 2 ? rectangles[level - 1][index] / 2 : (rectangles[level - 1][index] + 1) / 2;

			rectangles[level][index] = value;
		}
		if (rectangles[level][2] <= rectangles[level][0])
			rectangles[level][2] = rectangles[level][0] + 1;
		if (rectangles[level][3] <= rectangles[level][1])
			rectangles[level][3] = rectangles[level][1] + 1;
	}
	/* the view's pixels, read while the target is drawn into */
	xgpu_vulkan_image_copy_rectangle(target, 0, post.color, rectangles[0]);
	bloom_draw(&bloom.down, bloom.levels[0], rectangles[1], post.color, rectangles[0], 1.0f, 1.0f);
	for (level = 1; level < levels; level++)
		bloom_draw(&bloom.down, bloom.levels[level], rectangles[level + 1], bloom.levels[level - 1], rectangles[level],
			0.0f, 1.0f);
	for (level = levels - 1; level > 0; level--)
		bloom_draw(&bloom.up, bloom.levels[level - 1], rectangles[level], bloom.levels[level], rectangles[level + 1],
			0.0f, 1.0f);
	bloom_draw(&bloom.up, target, rectangles[0], bloom.levels[0], rectangles[1], 0.0f, bloom_modes[mode].strength);
	return 1;
}

/* ---------- display.prism_tone (xgpu_backend.h's XGPU_PRISM_TONE_*)

The window's 3D view, in half floats before its bloom and tone map, graded:
the rectangle copied (FXAA's copy), halved by the bloom's 13-tap filter
without its threshold into the bloom's images down to a sixteenth, brought
back up through the 3 by 3 tent (no blend: each replacing the larger) to a
quarter, the local mean; then each pixel of the rectangle by its ratio to
the mean and the mean lifted (the PRISM_TONE program's: local contrast,
lift, saturation and exposure by the mode). A multisampled view has none. */

#define TONE_LEVELS 5
#define TONE_LOCAL_LEVEL 1
#define TONE_LIGHT_MOST 4.0f

/* each mode's local contrast, lift (the local mean's power under white),
saturation and exposure, and its exposure by the view's mean: the mean it
aims at, the power and the most */
static const struct
{
	float contrast, lift, saturation, exposure, aim, power, most;
} tone_modes[3] = {
	{ 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.30f, 0.94f, 1.06f, 1.0f, 0.28f, 0.5f, 1.25f },
	{ 0.45f, 0.90f, 1.12f, 1.04f, 0.32f, 0.5f, 1.4f },
};

static struct
{
	struct xgpu_vulkan_pipeline_description up, grade;
	int failed, multisampled_logged;
} tone;

static int tone_prepare(void)
{
	if (tone.failed)
		return 0;
	if (tone.up.pixel_shader && tone.grade.pixel_shader)
		return 1;
	if (!bloom_prepare() || !shaders_make(XGPU_VULKAN_POST_PRISM_TONE))
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.prism_tone's passes cannot be made: no grade");
		tone.failed = 1;
		return 0;
	}
	/* (the tent up replacing the larger, its colour alone) */
	tone.up = bloom.up;
	tone.up.blend = 0;
	tone.up.blend_source = tone.up.blend_destination = 0;
	tone.grade = bloom.down;
	tone.grade.pixel_shader = post.pixel[XGPU_VULKAN_POST_PRISM_TONE];
	tone.grade.color_mask = 7;
	return 1;
}

/* the grade: the view's copy and the local mean (its rectangle) into the
target's rectangle */
static void tone_grade(struct xgpu_vulkan_image *target, const int32_t rectangle[4], struct xgpu_vulkan_image *local,
	const int32_t local_rectangle[4], int mode)
{
	const struct xgpu_vulkan_image_description *to = xgpu_vulkan_image_description(target);
	const struct xgpu_vulkan_image_description *from = xgpu_vulkan_image_description(local);
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline(&tone.grade);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage == 0 ? post.color : stage == 1 ? local : NULL;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)to->width;
	uniforms->metrics[1] = 1.0f / (float)to->height;
	uniforms->metrics[2] = (float)to->width;
	uniforms->metrics[3] = (float)to->height;
	uniforms->bounds[0] = ((float)rectangle[0] + 0.5f) / (float)to->width;
	uniforms->bounds[1] = ((float)rectangle[1] + 0.5f) / (float)to->height;
	uniforms->bounds[2] = ((float)rectangle[2] - 0.5f) / (float)to->width;
	uniforms->bounds[3] = ((float)rectangle[3] - 0.5f) / (float)to->height;
	uniforms->hdr[0] = tone_modes[mode].contrast;
	uniforms->hdr[1] = tone_modes[mode].lift;
	uniforms->hdr[2] = tone_modes[mode].saturation;
	uniforms->hdr[3] = tone_modes[mode].exposure;
	uniforms->window[0] = ((float)local_rectangle[0] + 0.5f) / (float)from->width;
	uniforms->window[1] = ((float)local_rectangle[1] + 0.5f) / (float)from->height;
	uniforms->window[2] = ((float)local_rectangle[2] - 0.5f) / (float)from->width;
	uniforms->window[3] = ((float)local_rectangle[3] - 0.5f) / (float)from->height;
	uniforms->panini[0] = tone_modes[mode].aim;
	uniforms->panini[1] = tone_modes[mode].power;
	uniforms->panini[2] = tone_modes[mode].most;
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(target, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

int xgpu_vulkan_post_prism_tone(struct xgpu_vulkan_image *target, const int32_t corners[4], int mode)
{
	const struct xgpu_vulkan_image_description *description;
	int32_t rectangles[TONE_LEVELS + 1][4];
	int index, level;

	if (!target || mode <= 0 || mode >= (int)(sizeof(tone_modes) / sizeof(tone_modes[0])))
		return 0;
	description = xgpu_vulkan_image_description(target);
	if (description->format != XGPU_VULKAN_FORMAT_RGBA16F)
		return 0;
	if (description->samples > 1)
	{
		if (!tone.multisampled_logged)
			host_logf(HOST_LOG_INFO, "vulkan: display.prism_tone: none over a multisampled view (MSAA)");
		tone.multisampled_logged = 1;
		return 0;
	}
	if (!tone_prepare())
		return 0;
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangles[0][index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangles[0][2] <= rectangles[0][0] || rectangles[0][3] <= rectangles[0][1])
		return 1;
	if (!images_fit(0, description->format, description->width, description->height) ||
		!bloom_images_fit(description->width, description->height))
		return 0;
	for (level = 1; level <= TONE_LEVELS; level++)
	{
		for (index = 0; index < 4; index++)
			rectangles[level][index] = index < 2 ? rectangles[level - 1][index] / 2 :
				(rectangles[level - 1][index] + 1) / 2;
		if (rectangles[level][2] <= rectangles[level][0])
			rectangles[level][2] = rectangles[level][0] + 1;
		if (rectangles[level][3] <= rectangles[level][1])
			rectangles[level][3] = rectangles[level][1] + 1;
	}
	xgpu_vulkan_image_copy_rectangle(target, 0, post.color, rectangles[0]);
	/* (down without the threshold: the whole light, each tap within 0 and
	TONE_LIGHT_MOST, none where it is no number) */
	bloom_draw(&bloom.down, bloom.levels[0], rectangles[1], post.color, rectangles[0], -TONE_LIGHT_MOST, 1.0f);
	for (level = 1; level < TONE_LEVELS; level++)
		bloom_draw(&bloom.down, bloom.levels[level], rectangles[level + 1], bloom.levels[level - 1], rectangles[level],
			0.0f, 1.0f);
	/* (up, each replacing the larger: the tent's mean, a strength of 1) */
	for (level = TONE_LEVELS - 1; level > TONE_LOCAL_LEVEL; level--)
		bloom_draw(&tone.up, bloom.levels[level - 1], rectangles[level], bloom.levels[level], rectangles[level + 1],
			0.0f, 1.0f);
	tone_grade(target, rectangles[0], bloom.levels[TONE_LOCAL_LEVEL], rectangles[TONE_LOCAL_LEVEL + 1], mode);
	return 1;
}

/* the encoding's pipeline for a swapchain's format: the frame sampled,
every channel written (the swapchain's alpha is opaque) */
static void encode_description(uint8_t format, struct xgpu_vulkan_pipeline_description *description)
{
	memset(description, 0, sizeof(*description));
	description->vertex_shader = post.vertex;
	description->pixel_shader = post.pixel[XGPU_VULKAN_POST_HDR_ENCODE];
	description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	description->color_mask = 15;
	description->color_format = format;
	description->samples = 1;
}

int xgpu_vulkan_post_encode_prepare(void)
{
	struct xgpu_vulkan_pipeline_description description;
	int index;

	if (post.encode[0] && post.encode[1])
		return 1;
	if (shaders_make(XGPU_VULKAN_POST_HDR_ENCODE) && sampler_make())
	{
		for (index = 0; index < 2; index++)
		{
			encode_description(index ? XGPU_VULKAN_FORMAT_RGB10A2 : XGPU_VULKAN_FORMAT_RGBA16F, &description);
			post.encode[index] = xgpu_vulkan_pipeline(&description);
		}
	}
	if (!post.encode[0] || !post.encode[1])
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.hdr's encoding cannot be made: the window is SDR");
		return 0;
	}
	return 1;
}

struct xgpu_vulkan_image *xgpu_vulkan_post_encode(struct xgpu_vulkan_image *image, const struct xgpu_hdr_frame *frame,
	uint8_t format)
{
	const struct xgpu_vulkan_image_description *description, *encoded;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	VkPipeline pipeline;
	int stage;

	if (!image || (format != XGPU_VULKAN_FORMAT_RGBA16F && format != XGPU_VULKAN_FORMAT_RGB10A2))
		return NULL;
	pipeline = post.encode[format == XGPU_VULKAN_FORMAT_RGB10A2];
	if (!pipeline)
		return NULL;
	description = xgpu_vulkan_image_description(image);
	encoded = post.encoded ? xgpu_vulkan_image_description(post.encoded) : NULL;
	if (encoded && (encoded->width != description->width || encoded->height != description->height ||
		encoded->format != format))
	{
		xgpu_vulkan_image_destroy(post.encoded);
		post.encoded = NULL;
	}
	if (!post.encoded)
		post.encoded = image_make(format, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET, description->width,
			description->height);
	if (!post.encoded)
		return NULL;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = pipeline;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : image;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return NULL;
	/* (the viewport's corner, and its size's reciprocals: the frame over
	all of it) */
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[2] = 1.0f / (float)description->width;
	uniforms->metrics[3] = 1.0f / (float)description->height;
	uniforms->hdr[0] = frame->paper_white;
	uniforms->hdr[1] = frame->peak;
	uniforms->hdr[2] = (float)frame->output;
	uniforms->hdr[3] = frame->ui;
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.scissor[2] = (int32_t)description->width;
	dynamic.viewport[3] = dynamic.scissor[3] = (int32_t)description->height;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(post.encoded, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return post.encoded;
}

/* ---------- display.gamma */

int xgpu_vulkan_post_gamma_prepare(void)
{
	struct xgpu_vulkan_pipeline_description *description = &post.pipelines[XGPU_VULKAN_POST_GAMMA];

	if (post.gamma)
		return 1;
	if (shaders_make(XGPU_VULKAN_POST_GAMMA) && sampler_make())
	{
		/* (into BGRA8, every channel: the swapchain's alpha is opaque) */
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[XGPU_VULKAN_POST_GAMMA];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		description->color_mask = 15;
		description->color_format = XGPU_VULKAN_FORMAT_BGRA8;
		description->samples = 1;
		post.gamma = xgpu_vulkan_pipeline(description);
	}
	if (!post.gamma)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.gamma's pass cannot be made: the frame is shown as drawn");
		return 0;
	}
	return 1;
}

struct xgpu_vulkan_image *xgpu_vulkan_post_gamma(struct xgpu_vulkan_image *image, float power)
{
	const struct xgpu_vulkan_image_description *description, *curved;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage;

	if (!image || !post.gamma)
		return NULL;
	description = xgpu_vulkan_image_description(image);
	curved = post.curved ? xgpu_vulkan_image_description(post.curved) : NULL;
	if (curved && (curved->width != description->width || curved->height != description->height))
	{
		xgpu_vulkan_image_destroy(post.curved);
		post.curved = NULL;
	}
	if (!post.curved)
		post.curved = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			description->width, description->height);
	if (!post.curved)
		return NULL;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = post.gamma;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : image;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return NULL;
	/* (the viewport's corner, and its size's reciprocals: the frame over
	all of it, each pixel its own texel) */
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[2] = 1.0f / (float)description->width;
	uniforms->metrics[3] = 1.0f / (float)description->height;
	uniforms->bounds[0] = power;
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.scissor[2] = (int32_t)description->width;
	dynamic.viewport[3] = dynamic.scissor[3] = (int32_t)description->height;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(post.curved, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return post.curved;
}
