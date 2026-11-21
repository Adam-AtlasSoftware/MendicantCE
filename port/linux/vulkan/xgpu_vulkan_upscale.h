/*
XGPU_VULKAN_UPSCALE.H

display.upscaler and display.denoiser in the Vulkan backend (PRISM v4 M5,
docs/prism_plugin.md; xgpu_vulkan_upscale.c): PRISM's renderer plugins
opened as the device is made, a view drawn smaller jittered and upscaled
(FSR 3.1, xgpu_vulkan_fsr3.c, or a plugin), and the path tracer's light
denoised by a plugin.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_UPSCALE_H
#define __HALO_LINUX_XGPU_VULKAN_UPSCALE_H

#include "xgpu_vulkan.h"

struct xgpu_stream_upscale;

/* ---------- the plugins (host_vulkan.c)

Before the device: the plugins display.upscaler and display.denoiser name
("plugin:<name>"'s name; NULL or empty none) opened from a folder (the data
folder's plugins), and what their instance and device need (the core's
options' plugin_*). After it: each one's state made. At the end: let go of. */

void xgpu_vulkan_upscale_plugins_open(const char *folder, const char *upscaler, const char *denoiser);
/* the instance extensions they ask for (static strings) and their count */
uint32_t xgpu_vulkan_upscale_instance_extensions(const char *const **names);
/* the device extensions they ask for of a device (at most most), each
plugin then asked whether it works on it (supported); a plugin that does
not is turned off and asks for none */
uint32_t xgpu_vulkan_upscale_device_extensions(VkPhysicalDevice physical, const char **names, uint32_t most);
/* an extension the loader (device 0) or the device (1) lacks, or one the
driver made no device with (2): its plugin turned off */
void xgpu_vulkan_upscale_extension_missing(const char *extension, int device);
void xgpu_vulkan_upscale_plugins_create(void);
void xgpu_vulkan_upscale_shutdown(void);
/* the credits' text of the plugins in use, one line each (NULL: none) */
const char *xgpu_vulkan_upscale_credits(void);
/* whether the plugin of that name is ready for Settings to offer (its
PRISM_CAP_* where it is, else 0): in use this run (made, not turned off),
or, where none of that name was opened, found in the folder, its ABI
accepted and its supported passing on the device made (asked once a name,
then remembered; the library closed again). 0 before the device is made */
uint32_t xgpu_vulkan_upscale_plugin_ready(const char *folder, const char *name);

/* ---------- the view (xgpu_vulkan_stream.c's UPSCALE)

BEGIN: the view's colour target (its image) drawn jittered from now on;
OPAQUE: its colour kept, before its transparent surfaces; END: its inputs
made (the G-buffer's second target, NULL where none was drawn for it), the
upscaler run, and its output and the view's depth stretched into the
output's targets' rectangle. The images are the stream's targets'. */

void xgpu_vulkan_upscale_begin(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view);
/* the view begun's pixels over the output's each way while an upscaler (or
a denoiser that upscales) brings it to the screen's (the last END's sizes:
the game's ratio, a frame behind where it changes), 1 outside such a view:
the passes that sample textures by their own footprints (the terrain's HQ
layers) sample them as sharp as the screen's pixels would, as the game's
mip bias samples CE's maps */
float xgpu_vulkan_upscale_view_ratio(void);
/* the view begun's jitter (the draws' offset in its target's pixels, right and
down), 1 while its draws are jittered; 0 and none outside such a view: the
passes that find a pixel's point again from its distance (the path tracer)
find it along the jittered pixel's ray, where the G-buffer drew it */
int xgpu_vulkan_upscale_jitter(float jitter[2]);
void xgpu_vulkan_upscale_opaque(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view);
void xgpu_vulkan_upscale_end(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view,
	struct xgpu_vulkan_image *view_depth, struct xgpu_vulkan_image *output, struct xgpu_vulkan_image *output_depth,
	struct xgpu_vulkan_image *gbuffer_extras, struct xgpu_vulkan_image *gbuffer_words,
	struct xgpu_vulkan_image *gbuffer_base);

/* ---------- display.denoiser's plugin (xgpu_vulkan_rt.c's path tracer)

Whether the view drawn now gives the path tracer's light to the denoiser's
plugin (display.denoiser "plugin:<name>", one ready, between the view's
BEGIN and END; never outside a view): its own filter is then off, the light
traced raw. A plugin that denoises (PRISM_CAP_DENOISE) is given the light
then (xgpu_vulkan_upscale_denoise: the path pass's two raw images, the
light and the light added, RGBA16F, denoised in place, recorded into
commands outside a pass; 1 if it was); one that denoises and upscales
(PRISM_CAP_DENOISE_UPSCALE: DLSS Ray Reconstruction) the whole view at
END, with the G-buffer's guides, in the upscaler's place, where the view
has its G-buffer (display.lighting "ce_deferred" or "raster"; the stream
tells it, xgpu_vulkan_upscale_gbuffer, before the trace): without it the
tracer's own filter is kept and FSR 3.1 resolves the view. Each change of
the answer is logged where a denoiser's plugin is named. */
/* the G-buffer drawn this frame was the target's (its number; 0 none) */
void xgpu_vulkan_upscale_gbuffer(uint32_t target);
int xgpu_vulkan_upscale_raw_light(void);
int xgpu_vulkan_upscale_denoise(VkCommandBuffer commands, struct xgpu_vulkan_image *light,
	struct xgpu_vulkan_image *added, uint32_t width, uint32_t height);

/* ---------- PRISM Path Traced's frame (xgpu_vulkan_pt.c, display.lighting
"pathtraced")

Whether the view drawn now gives the path tracer's noisy light to the
denoiser's plugin that denoises and upscales (display.denoiser "dlss_rr" or
"plugin:<name>", one ready, between the view's BEGIN and END): the tracer's
own filter is then off and it gives its light raw (pt_frame's radiance and
guides). Each change of the answer is logged where a denoiser's plugin is
named. */
int xgpu_vulkan_upscale_pt_raw(void);
/* the path tracer's frame drawn in the view now, each one an image of its
(the view's target's size): its exposure (1x1, RGBA16F: x the exposure, y
the curve, written by the frame's commands) and the curve (display.
prism_tone_curve: 0 clipped, 1 filmic, 2 aces, 3 local), so that a plugin
upscales the view as linear HDR light before the tone mapping and END tone
maps its output again; and where pt_raw answered 1, its noisy light before
the tone mapping (RGBA16F, alpha 1 where it lit the pixel, else 0) and its
guides (the diffuse and specular albedo it demodulated with, the shading
normal and roughness; hit_distance's x the reflection ray's hit distance, y
the diffuse bounce's, zw the reflected image's last place less the
surface's, in pixels), else NULL */
void xgpu_vulkan_upscale_pt_frame(struct xgpu_vulkan_image *exposure, uint32_t curve,
	struct xgpu_vulkan_image *radiance, struct xgpu_vulkan_image *diffuse_albedo,
	struct xgpu_vulkan_image *specular_albedo, struct xgpu_vulkan_image *normal_roughness,
	struct xgpu_vulkan_image *hit_distance);

#endif
