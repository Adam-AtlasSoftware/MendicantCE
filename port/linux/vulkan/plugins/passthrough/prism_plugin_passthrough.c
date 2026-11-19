/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 Adam Wiley */
/*
PRISM_PLUGIN_PASSTHROUGH.C

The tree's own plugin (prism_plugin.h, docs/prism_plugin.md): an
"upscaler" that copies the frame's colour to the output, byte for byte
where the sizes are the same (vkCmdCopyImage), else stretched to the
nearest texel (vkCmdBlitImage); and a "denoiser" that leaves the light as
it is given (a frame with no colour, its radiance denoised in place: none
of it changed). It exists to exercise the ABI: with it selected the picture
must be the one the upscaler off draws (at the screen's size, the jitter
off), and the one the path tracer's own filter off draws. Two diagnostics,
to see what the host gives a plugin: PRISM_PASSTHROUGH_CAPABILITIES="rr"
makes it an upscaler and a denoiser that upscales (DLSS Ray
Reconstruction's kind), so that the host makes the G-buffer's guides and
hands them over; PRISM_PASSTHROUGH_SHOW names an input put in the output in
the colour's place (depth, motion, reactive, diffuse, specular, normal,
hit: the reflection rays' lengths, diffuse_hit: the diffuse bounces',
motion_reflections: the reflected objects' motion), stretched; and the first frame's flags,
tone mapper and which images it has are written to standard error once.
Built as a shared library on its own:

  cc -shared -fPIC -fvisibility=hidden -I<vulkan headers> -I port/linux/vulkan \
      port/linux/vulkan/plugins/passthrough/prism_plugin_passthrough.c -o passthrough.so
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prism_plugin.h"

struct passthrough
{
	VkDevice device;
	PFN_vkCmdPipelineBarrier barrier;
	PFN_vkCmdCopyImage copy;
	PFN_vkCmdBlitImage blit;
	const char *error;
};

static const char *last_error_static = "";

static int instance_extensions(const char **names, uint32_t *count)
{
	(void)names;
	*count = 0;
	return 1;
}

static int device_extensions(VkPhysicalDevice physical, const char **names, uint32_t *count)
{
	(void)physical;
	(void)names;
	*count = 0;
	return 1;
}

static int supported(VkPhysicalDevice physical)
{
	(void)physical;
	return 1;
}

static int create(const struct prism_plugin_vulkan *vulkan, void **context)
{
	struct passthrough *p;

	*context = NULL;
	if (!vulkan || vulkan->size < sizeof(*vulkan) || !vulkan->get_device_proc)
	{
		last_error_static = "passthrough: no device handed over";
		return 0;
	}
	p = calloc(1, sizeof(*p));
	if (!p)
	{
		last_error_static = "passthrough: out of memory";
		return 0;
	}
	p->device = vulkan->device;
	p->barrier = (PFN_vkCmdPipelineBarrier)vulkan->get_device_proc(vulkan->device, "vkCmdPipelineBarrier");
	p->copy = (PFN_vkCmdCopyImage)vulkan->get_device_proc(vulkan->device, "vkCmdCopyImage");
	p->blit = (PFN_vkCmdBlitImage)vulkan->get_device_proc(vulkan->device, "vkCmdBlitImage");
	if (!p->barrier || !p->copy || !p->blit)
	{
		free(p);
		last_error_static = "passthrough: the device has no vkCmdCopyImage, vkCmdBlitImage or vkCmdPipelineBarrier";
		return 0;
	}
	p->error = "";
	*context = p;
	return 1;
}

static int resize(void *context, uint32_t render_width, uint32_t render_height, uint32_t output_width,
	uint32_t output_height, uint32_t quality)
{
	(void)context;
	(void)render_width;
	(void)render_height;
	(void)output_width;
	(void)output_height;
	(void)quality;
	return 1;
}

/* an image moved from one layout to another, every earlier write finished
and visible to what comes after (a transfer, or the host's work) */
static void transition(struct passthrough *p, VkCommandBuffer commands, VkImage image, VkImageLayout from,
	VkImageLayout to)
{
	VkImageMemoryBarrier b;

	if (from == to)
		return;
	memset(&b, 0, sizeof(b));
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	p->barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
		1, &b);
}

/* the transfer's layout for an image: its own where that one serves */
static VkImageLayout transfer_layout(VkImageLayout layout, VkImageLayout wanted)
{
	return layout == VK_IMAGE_LAYOUT_GENERAL || layout == wanted ? layout : wanted;
}

static int evaluate(void *context, const struct prism_plugin_frame *frame)
{
	struct passthrough *p = context;
	const struct prism_plugin_image *in, *out;
	VkImageLayout in_layout, out_layout;

	if (!p || !frame || frame->size < sizeof(*frame))
	{
		if (p)
			p->error = "passthrough: no frame, or a frame of an older ABI";
		return 0;
	}
	in = &frame->color_in;
	out = &frame->color_out;
	/* (what the host gives, the first frame and each time it changes: the
	frame's flags, its curve, the images it has) */
	{
		static uint32_t told;
		uint32_t key = 0x80000000u | frame->flags | (frame->specular_hit_distance.image != VK_NULL_HANDLE) << 8 |
			(frame->diffuse_hit_distance.image != VK_NULL_HANDLE) << 9 |
			(frame->motion_reflections.image != VK_NULL_HANDLE) << 10 |
			(frame->diffuse_albedo.image != VK_NULL_HANDLE) << 11 | (frame->render_width != frame->output_width) << 12;

		if (told != key)
		{
			fprintf(stderr, "passthrough: %s: flags", told ? "frame" : "first frame");
			told = key;
			fprintf(stderr, " 0x%x (hdr %d, depth reversed %d, noisy %d), tone mapper "
				"%u, exposure %g, pre-exposure %g, %ux%u to %ux%u; images: colour %d, depth %d, motion %d, normal %d, "
				"diffuse %d, specular %d, hit %d, diffuse hit %d, reflection motion %d, reactive %d, exposure image %d\n",
				frame->flags,
				(frame->flags & PRISM_FRAME_HDR) != 0, (frame->flags & PRISM_FRAME_DEPTH_REVERSED) != 0,
				(frame->flags & PRISM_FRAME_NOISY) != 0, frame->tone_mapper, frame->exposure, frame->pre_exposure,
				frame->render_width, frame->render_height, frame->output_width, frame->output_height,
				frame->color_in.image != VK_NULL_HANDLE, frame->depth.image != VK_NULL_HANDLE,
				frame->motion.image != VK_NULL_HANDLE, frame->normal_roughness.image != VK_NULL_HANDLE,
				frame->diffuse_albedo.image != VK_NULL_HANDLE, frame->specular_albedo.image != VK_NULL_HANDLE,
				frame->specular_hit_distance.image != VK_NULL_HANDLE, frame->diffuse_hit_distance.image != VK_NULL_HANDLE,
				frame->motion_reflections.image != VK_NULL_HANDLE, frame->reactive_mask.image != VK_NULL_HANDLE,
				frame->exposure_image.image != VK_NULL_HANDLE);
		}
	}
	/* (a denoiser's frame: the light left as it is) */
	if (!in->image && !out->image && frame->diffuse_radiance.image)
		return 1;
	/* (PRISM_PASSTHROUGH_SHOW: another input in the colour's place, where
	the frame has it) */
	{
		const char *show = getenv("PRISM_PASSTHROUGH_SHOW");
		const struct prism_plugin_image *shown = NULL;

		if (show && !strcmp(show, "depth"))
			shown = &frame->depth;
		else if (show && !strcmp(show, "motion"))
			shown = &frame->motion;
		else if (show && !strcmp(show, "reactive"))
			shown = &frame->reactive_mask;
		else if (show && !strcmp(show, "diffuse"))
			shown = &frame->diffuse_albedo;
		else if (show && !strcmp(show, "specular"))
			shown = &frame->specular_albedo;
		else if (show && !strcmp(show, "normal"))
			shown = &frame->normal_roughness;
		else if (show && !strcmp(show, "hit"))
			shown = &frame->specular_hit_distance;
		else if (show && !strcmp(show, "diffuse_hit"))
			shown = &frame->diffuse_hit_distance;
		else if (show && !strcmp(show, "motion_reflections"))
			shown = &frame->motion_reflections;
		if (shown && shown->image && shown->width && shown->height)
			in = shown;
	}
	if (!in->image || !out->image || !in->width || !in->height || !out->width || !out->height)
	{
		p->error = "passthrough: no colour in or out";
		return 0;
	}
	in_layout = transfer_layout(in->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	out_layout = transfer_layout(out->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	transition(p, frame->commands, in->image, in->layout, in_layout);
	transition(p, frame->commands, out->image, out->layout, out_layout);
	if (in->width == out->width && in->height == out->height && in->format == out->format)
	{
		VkImageCopy region;

		memset(&region, 0, sizeof(region));
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource = region.srcSubresource;
		region.extent.width = in->width;
		region.extent.height = in->height;
		region.extent.depth = 1;
		p->copy(frame->commands, in->image, in_layout, out->image, out_layout, 1, &region);
	}
	else
	{
		VkImageBlit region;

		memset(&region, 0, sizeof(region));
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource = region.srcSubresource;
		region.srcOffsets[1].x = (int32_t)in->width;
		region.srcOffsets[1].y = (int32_t)in->height;
		region.srcOffsets[1].z = 1;
		region.dstOffsets[1].x = (int32_t)out->width;
		region.dstOffsets[1].y = (int32_t)out->height;
		region.dstOffsets[1].z = 1;
		p->blit(frame->commands, in->image, in_layout, out->image, out_layout, 1, &region, VK_FILTER_NEAREST);
	}
	/* (each image left in the layout it came in) */
	transition(p, frame->commands, in->image, in_layout, in->layout);
	transition(p, frame->commands, out->image, out_layout, out->layout);
	return 1;
}

static void destroy(void *context)
{
	free(context);
}

static const char *last_error(void *context)
{
	struct passthrough *p = context;

	return p && p->error ? p->error : last_error_static;
}

static struct prism_plugin plugin =
{
	{
		PRISM_PLUGIN_ABI, sizeof(struct prism_plugin_info),
		"passthrough", "PRISM", "1", "AGPL-3.0-or-later (the game's own code)", "",
		PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE,
		sizeof(struct prism_plugin_vulkan), sizeof(struct prism_plugin_image), sizeof(struct prism_plugin_frame),
	},
	instance_extensions,
	device_extensions,
	supported,
	create,
	resize,
	evaluate,
	destroy,
	last_error,
};

PRISM_PLUGIN_EXPORT const struct prism_plugin *prism_plugin_entry(uint32_t host_abi)
{
	const char *capabilities = getenv("PRISM_PASSTHROUGH_CAPABILITIES");

	/* (PRISM_PASSTHROUGH_CAPABILITIES "rr": Ray Reconstruction's kind of
	denoiser in the light's) */
	plugin.info.capabilities = capabilities && !strcmp(capabilities, "rr") ?
		PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE_UPSCALE : PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE;
	return host_abi == PRISM_PLUGIN_ABI ? &plugin : NULL;
}
