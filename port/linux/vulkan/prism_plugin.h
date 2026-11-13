/* SPDX-License-Identifier: MIT OR AGPL-3.0-or-later */
/* Copyright (c) 2026 Adam Wiley */
/*
PRISM_PLUGIN.H

The PRISM renderer's plugin interface (docs/prism_plugin.md): a stable C
ABI through which a separately built shared library (.so on Linux, .dll on
Windows) upscales or denoises the frame: DLSS, NRD, XeSS or FSR 4 glue,
or the tree's own passthrough plugin. The host opens a plugin only when a
setting names one (xgpu_plugin.c).

Licence: this one file is offered under the MIT licence or the AGPL 3.0 or
later, as the reader chooses, so that plugin glue built against it may be
MIT and never needs to be AGPL. The rest of the game is AGPL-3.0-or-later.
No vendor's code or header is in this file or in the tree; a vendor's
plugin is built by the player, outside it.

It includes only <stdint.h> and Vulkan's own headers. Every structure
starts with its size (or carries it in its info), so that a host and a
plugin built at different times can check what the other has.

The order of calls: before the host makes its Vulkan instance and device
it calls instance_extensions and device_extensions (a vendor may need
extensions enabled at creation), then supported for the device chosen;
after it makes the device, create; resize whenever the render or output
size or the quality changes (and once before the first evaluate); evaluate
once a frame; destroy at the end. Any call that returns 0 (or a plugin
that fails a check) is turned off for the session, with last_error's text
in the log, and the host draws its own path in its place.
*/

#ifndef __PRISM_PLUGIN_H
#define __PRISM_PLUGIN_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the ABI's version: raised with any change to a structure or a call
below. A plugin built for another refuses (or the host refuses it) */
#define PRISM_PLUGIN_ABI 1u

/* the one symbol a plugin exports, and its type: given the host's ABI, it
returns the plugin's table (static, alive while the library is open), or
NULL where it cannot serve that ABI */
#define PRISM_PLUGIN_ENTRY_SYMBOL "prism_plugin_entry"

#if defined(_WIN32)
#define PRISM_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PRISM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* what a plugin can do (prism_plugin_info's capabilities, a mask) */
enum prism_plugin_capability
{
	/* the frame upscaled: color_in at the render size to color_out at the
	output size (DLSS SR, XeSS SR, FSR 4) */
	PRISM_CAP_UPSCALE = 1u << 0,
	/* the traced light denoised: the *_radiance and shadow_visibility
	images, in place (NRD) */
	PRISM_CAP_DENOISE = 1u << 1,
	/* both at once from the noisy frame and its guides (DLSS RR) */
	PRISM_CAP_DENOISE_UPSCALE = 1u << 2,
	/* frames generated between the host's (not used before M9, if ever) */
	PRISM_CAP_FRAME_GENERATE = 1u << 3,
};

/* resize's quality, the share of the output the render size is: the
host's choice, the plugin's to use as its vendor names them */
enum prism_plugin_quality
{
	PRISM_QUALITY_NATIVE,
	PRISM_QUALITY_QUALITY,
	PRISM_QUALITY_BALANCED,
	PRISM_QUALITY_PERFORMANCE,
	PRISM_QUALITY_ULTRA_PERFORMANCE,
};

/* who the plugin is */
struct prism_plugin_info
{
	/* PRISM_PLUGIN_ABI as the plugin was built; sizeof(struct
	prism_plugin_info) as the plugin was built */
	uint32_t abi, size;
	/* its name (the setting's "plugin:<name>"), its vendor, its own version,
	a notice of the licences of what it contains (shown with the game's
	licences), and the text the credits screen shows for it (a vendor may
	require its marks there); UTF-8, never NULL (empty where none) */
	const char *name, *vendor, *version, *licence_notice, *credits_text;
	/* PRISM_CAP_* */
	uint32_t capabilities;
	/* the sizes of struct prism_plugin_vulkan, _image and _frame as the
	plugin was built: the host refuses a plugin that expects more than it
	gives */
	uint32_t vulkan_size, image_size, frame_size;
};

/* the host's Vulkan objects, handed over at create (after the device is
made); the plugin must not destroy any of them */
struct prism_plugin_vulkan
{
	/* sizeof(struct prism_plugin_vulkan) */
	uint32_t size;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	/* the loader's functions, from which the plugin takes every other */
	PFN_vkGetInstanceProcAddr get_instance_proc;
	PFN_vkGetDeviceProcAddr get_device_proc;
	/* the queue the host submits evaluate's commands on, and its family */
	uint32_t queue_family;
	VkQueue queue;
};

/* an image of the frame: the plugin reads or writes it in the commands it
records; a missing one is all VK_NULL_HANDLE and 0 */
struct prism_plugin_image
{
	VkImage image;
	VkImageView view;
	VkFormat format;
	/* the layout the image is in when evaluate's commands start; the
	plugin's commands must leave it in the same layout (any transition the
	plugin makes it makes back) */
	VkImageLayout layout;
	uint32_t width, height;
};

/* a frame's inputs and output (evaluate) */
struct prism_plugin_frame
{
	/* sizeof(struct prism_plugin_frame) */
	uint32_t size;
	/* the commands evaluate records into; the host has begun it and ends
	and submits it on the queue it named. Every image below was written by
	commands before these, with the barrier that makes it readable in a
	compute or transfer stage; the host puts its own barrier after */
	VkCommandBuffer commands;
	/* the frame's colour: HDR, linear, pre-exposed (by pre_exposure), at the
	render size; and the plugin's output, at the output size, in a layout
	it can be written in (VK_IMAGE_LAYOUT_GENERAL or
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) */
	struct prism_plugin_image color_in, color_out;
	/* the guides, at the render size: depth (reversed or not as the
	projection says), screen motion in pixels (this frame's position less
	the last frame's), the shading normal (xyz, world space) and roughness
	(w) */
	struct prism_plugin_image depth, motion, normal_roughness;
	/* the surfaces' diffuse and specular albedo, and each pixel's specular
	ray's hit distance (world units) */
	struct prism_plugin_image diffuse_albedo, specular_albedo, specular_hit_distance;
	/* PRISM_CAP_DENOISE's: the diffuse and specular light, demodulated
	(divided by their albedos), and the sun's visibility; denoised in place */
	struct prism_plugin_image diffuse_radiance, specular_radiance, shadow_visibility;
	/* where history is less to be trusted (particles, animated textures),
	and where transparent surfaces were drawn: 0 to 1 */
	struct prism_plugin_image reactive_mask, transparency_mask;
	/* the projection's sub-pixel jitter (pixels), the exposure (a factor),
	the pre-exposure color_in was drawn with, the sharpening asked for (0 to
	1) and the frame's time (milliseconds) */
	float jitter[2], exposure, pre_exposure, sharpness, frame_time_ms;
	/* the camera's matrices, this frame's and the last's, column-major,
	world to view and view to clip */
	float view[16], projection[16], view_prev[16], projection_prev[16];
	/* reset: 1 where the history must be dropped (a cut, a teleport, a map
	loaded); the render and output sizes */
	uint32_t reset, render_width, render_height, output_width, output_height;
};

/* the plugin's table. Every call returns 1 on success and 0 on failure
(then last_error says why) unless it says otherwise */
struct prism_plugin
{
	struct prism_plugin_info info;
	/* before vkCreateInstance: the instance extensions the plugin needs.
	Called first with names NULL, *count set to how many; then with room
	for *count names (static strings) */
	int (*instance_extensions)(const char **names, uint32_t *count);
	/* before vkCreateDevice, likewise for the device's extensions */
	int (*device_extensions)(VkPhysicalDevice physical, const char **names, uint32_t *count);
	/* whether the plugin works on this device and driver: 0 not */
	int (*supported)(VkPhysicalDevice physical);
	/* after the device is made: the plugin's state, in *context */
	int (*create)(const struct prism_plugin_vulkan *vulkan, void **context);
	/* the sizes and quality (PRISM_QUALITY_*) changed, or before the first
	evaluate; the host waits for its queue to be idle before calling */
	int (*resize)(void *context, uint32_t render_width, uint32_t render_height, uint32_t output_width,
		uint32_t output_height, uint32_t quality);
	/* the frame's commands recorded into frame->commands */
	int (*evaluate)(void *context, const struct prism_plugin_frame *frame);
	/* the state released; the host's queue is idle */
	void (*destroy)(void *context);
	/* why the last call failed (context may be NULL: a failure before or in
	create); a static or context-owned string, never NULL */
	const char *(*last_error)(void *context);
};

/* the entry's type (PRISM_PLUGIN_ENTRY_SYMBOL) */
typedef const struct prism_plugin *(*prism_plugin_entry_function)(uint32_t host_abi);

#ifdef __cplusplus
}
#endif

#endif
