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

/* ---------- the view (xgpu_vulkan_stream.c's UPSCALE)

BEGIN: the view's colour target (its image) drawn jittered from now on;
OPAQUE: its colour kept, before its transparent surfaces; END: its inputs
made (the G-buffer's second target, NULL where none was drawn for it), the
upscaler run, and its output and the view's depth stretched into the
output's targets' rectangle. The images are the stream's targets'. */

void xgpu_vulkan_upscale_begin(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view);
void xgpu_vulkan_upscale_opaque(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view);
void xgpu_vulkan_upscale_end(const struct xgpu_stream_upscale *record, struct xgpu_vulkan_image *view,
	struct xgpu_vulkan_image *view_depth, struct xgpu_vulkan_image *output, struct xgpu_vulkan_image *output_depth,
	struct xgpu_vulkan_image *gbuffer_extras, struct xgpu_vulkan_image *gbuffer_words,
	struct xgpu_vulkan_image *gbuffer_base);

/* ---------- display.denoiser's plugin (xgpu_vulkan_rt.c's path tracer)

Whether the view drawn now gives the path tracer's light to the denoiser's
plugin (display.denoiser "plugin:<name>", one ready): its own filter is then
off, the light traced raw. A plugin that denoises (PRISM_CAP_DENOISE) is
given the light then (xgpu_vulkan_upscale_denoise: the path pass's two raw
images, the light and the light added, RGBA16F, denoised in place, recorded
into commands outside a pass; 1 if it was); one that denoises and upscales
(PRISM_CAP_DENOISE_UPSCALE: DLSS Ray Reconstruction) the whole view at
END, with the G-buffer's guides, in the upscaler's place (the view must
have its G-buffer: display.lighting "ce_deferred" or "raster"). */
int xgpu_vulkan_upscale_raw_light(void);
int xgpu_vulkan_upscale_denoise(VkCommandBuffer commands, struct xgpu_vulkan_image *light,
	struct xgpu_vulkan_image *added, uint32_t width, uint32_t height);

#endif
