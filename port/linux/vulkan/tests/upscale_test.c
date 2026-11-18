/* display.upscaler's and display.denoiser's host halves (xgpu_vulkan_fsr3.c,
xgpu_vulkan_upscale.c) without a GPU (tools/test_upscale.py): FSR 3.1's
jitter's sequence, a dispatch with no device refused (the view then
stretched into the window as it is), and the path tracer's light given to a
denoiser's plugin only in a view begun for it, over stubs of the core and
of the plugin loader (fake plugins).

  upscale_test jitter <render width> <output width>   the sequence's length,
                                                      then its offsets
  upscale_test nodevice                               the dispatch refused
  upscale_test denoiser denoise|rr                    each step's raw light
                                                      (a plugin that denoises,
                                                      or one that denoises and
                                                      upscales)
The core's log lines are printed as "log: <line>". */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgpu_vulkan_fsr3.h"
#include "xgpu_vulkan_upscale.h"
#include "xgpu_vulkan_upscale_shaders.h"
#include "xgpu_plugin.h"
#include "../src/xgpu_stream.h"

/* ---------- the core's functions, stubbed: a device only where asked (no
GPU work is ever recorded) */

static int stub_device;

int xgpu_vulkan_handles(struct xgpu_vulkan_handles *handles)
{
	memset(handles, 0, sizeof(*handles));
	if (!stub_device)
		return 0;
	handles->device = (VkDevice)(uintptr_t)1;
	return 1;
}

uint64_t xgpu_vulkan_completed(void) { return 0; }
void xgpu_vulkan_wait(uint64_t fence) { (void)fence; }
uint64_t xgpu_vulkan_serial(void) { return 1; }
uint64_t xgpu_vulkan_fence(void) { return 1; }
void *xgpu_vulkan_upload(uint32_t size, uint32_t alignment, struct xgpu_vulkan_range *range)
{
	(void)size; (void)alignment; (void)range;
	return NULL;
}
const struct xgpu_vulkan_upscale_program xgpu_vulkan_upscale_programs[XGPU_VULKAN_UPSCALE_PROGRAM_COUNT];

void xgpu_vulkan_set_jitter(float x, float y, uint32_t width, uint32_t height)
{
	(void)x; (void)y; (void)width; (void)height;
}

const struct xgpu_vulkan_image_description *xgpu_vulkan_image_description(const struct xgpu_vulkan_image *image)
{
	static struct xgpu_vulkan_image_description description;

	(void)image;
	description.width = 64;
	description.height = 32;
	return &description;
}

struct xgpu_vulkan_image *xgpu_vulkan_image_create(const struct xgpu_vulkan_image_description *description)
{
	(void)description;
	return NULL;
}

void xgpu_vulkan_image_destroy(struct xgpu_vulkan_image *image) { (void)image; }
void xgpu_vulkan_image_copy_rectangle(struct xgpu_vulkan_image *source, uint32_t source_layer,
	struct xgpu_vulkan_image *destination, const int32_t rectangle[4])
{
	(void)source; (void)source_layer; (void)destination; (void)rectangle;
}
void xgpu_vulkan_image_compute_use(struct xgpu_vulkan_image *image, int written) { (void)image; (void)written; }
VkCommandBuffer xgpu_vulkan_compute_commands(void) { return VK_NULL_HANDLE; }
void xgpu_vulkan_gpu_time_mark(int which) { (void)which; }
int xgpu_vulkan_image_blit(struct xgpu_vulkan_image *source, const int32_t from[4],
	struct xgpu_vulkan_image *destination, const int32_t to[4], int linear)
{
	(void)source; (void)from; (void)destination; (void)to; (void)linear;
	return 0;
}
VkImage xgpu_vulkan_image_handle(const struct xgpu_vulkan_image *image) { (void)image; return VK_NULL_HANDLE; }
VkImageView xgpu_vulkan_image_view(const struct xgpu_vulkan_image *image) { (void)image; return VK_NULL_HANDLE; }
VkFormat xgpu_vulkan_depth_format(void) { return VK_FORMAT_D24_UNORM_S8_UINT; }

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	printf("log: ");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

/* ---------- fake plugins (xgpu_plugin_open's), by name: "denoise"
(PRISM_CAP_DENOISE) and "rr" (PRISM_CAP_DENOISE_UPSCALE) */

static int evaluations;

static int no_extensions(const char **names, uint32_t *count) { (void)names; *count = 0; return 1; }
static int no_device_extensions(VkPhysicalDevice physical, const char **names, uint32_t *count)
{
	(void)physical; (void)names;
	*count = 0;
	return 1;
}

static int supported(VkPhysicalDevice physical) { (void)physical; return 1; }
static int create(const struct prism_plugin_vulkan *vulkan, void **context)
{
	(void)vulkan;
	*context = &evaluations;
	return 1;
}
static int resize(void *context, uint32_t render_width, uint32_t render_height, uint32_t output_width,
	uint32_t output_height, uint32_t quality)
{
	(void)context; (void)render_width; (void)render_height; (void)output_width; (void)output_height; (void)quality;
	return 1;
}
static int evaluate(void *context, const struct prism_plugin_frame *frame)
{
	(void)context; (void)frame;
	evaluations++;
	return 1;
}
static void destroy(void *context) { (void)context; }
static const char *last_error(void *context) { (void)context; return "none"; }

#define FAKE(name, capabilities, instance, device) \
	{ { PRISM_PLUGIN_ABI, sizeof(struct prism_plugin_info), name, "test", "1", "", "", capabilities, \
		sizeof(struct prism_plugin_vulkan), sizeof(struct prism_plugin_image), sizeof(struct prism_plugin_frame) }, \
		instance, device, supported, create, resize, evaluate, destroy, last_error }

static const struct prism_plugin fakes[] =
{
	FAKE("denoise", PRISM_CAP_DENOISE, no_extensions, no_device_extensions),
	FAKE("rr", PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE_UPSCALE, no_extensions, no_device_extensions),
};
static struct xgpu_plugin libraries[sizeof(fakes) / sizeof(fakes[0])];

struct xgpu_plugin *xgpu_plugin_open(const char *folder, const char *name, xgpu_plugin_log log, void *log_data)
{
	size_t index;

	(void)folder; (void)log; (void)log_data;
	for (index = 0; name && index < sizeof(fakes) / sizeof(fakes[0]); index++)
	{
		if (!strcmp(name, fakes[index].info.name))
		{
			libraries[index].table = &fakes[index];
			return &libraries[index];
		}
	}
	return NULL;
}

void xgpu_plugin_close(struct xgpu_plugin *plugin) { (void)plugin; }

/* ---------- the tests */

static void phase(uint32_t which, uint32_t denoiser, uint32_t view)
{
	struct xgpu_stream_upscale record;

	memset(&record, 0, sizeof(record));
	record.phase = which;
	record.upscaler = XGPU_UPSCALE_UPSCALER_NONE;
	record.denoiser = denoiser;
	record.view = view;
	/* (no images: nothing recorded, only the view's state kept) */
	if (which == XGPU_UPSCALE_BEGIN)
		xgpu_vulkan_upscale_begin(&record, NULL);
	else
		xgpu_vulkan_upscale_end(&record, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* the raw light's answer now, and whether the plugin denoised the light */
static void step(const char *what)
{
	/* (stand-ins for the path pass's two images, never read) */
	static int light, added;
	int raw = xgpu_vulkan_upscale_raw_light();
	int denoised = xgpu_vulkan_upscale_denoise(VK_NULL_HANDLE, (struct xgpu_vulkan_image *)&light,
		(struct xgpu_vulkan_image *)&added, 64, 32);

	printf("%s: raw %d denoised %d\n", what, raw, denoised);
}

static int denoiser_test(const char *name)
{
	stub_device = 1;
	xgpu_vulkan_upscale_plugins_open("plugins", "", name);
	xgpu_vulkan_upscale_plugins_create();
	step("no view yet");
	phase(XGPU_UPSCALE_BEGIN, XGPU_UPSCALE_DENOISER_PLUGIN, 5);
	step("view begun");
	xgpu_vulkan_upscale_gbuffer(9);
	step("another target's G-buffer");
	xgpu_vulkan_upscale_gbuffer(5);
	step("the view's G-buffer");
	phase(XGPU_UPSCALE_END, XGPU_UPSCALE_DENOISER_PLUGIN, 5);
	step("view ended");
	step("a frame with no view");
	xgpu_vulkan_upscale_gbuffer(5);
	step("a frame with no view, its G-buffer");
	phase(XGPU_UPSCALE_BEGIN, XGPU_UPSCALE_DENOISER_PRISM, 5);
	xgpu_vulkan_upscale_gbuffer(5);
	step("a view for PRISM's filter");
	phase(XGPU_UPSCALE_END, XGPU_UPSCALE_DENOISER_PRISM, 5);
	phase(XGPU_UPSCALE_BEGIN, XGPU_UPSCALE_DENOISER_PLUGIN, 6);
	xgpu_vulkan_upscale_gbuffer(6);
	step("a view for the plugin again");
	phase(XGPU_UPSCALE_END, XGPU_UPSCALE_DENOISER_PLUGIN, 6);
	step("ended again");
	xgpu_vulkan_upscale_shutdown();
	printf("evaluations %d\n", evaluations);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "jitter"))
	{
		uint32_t phases = xgpu_vulkan_fsr3_jitter_phases((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3])), index;

		printf("%u\n", phases);
		for (index = 0; index < phases + 2; index++)
		{
			float jitter[2];

			xgpu_vulkan_fsr3_jitter(index, phases, jitter);
			printf("%.7f %.7f\n", jitter[0], jitter[1]);
		}
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "nodevice"))
	{
		struct xgpu_vulkan_fsr3_frame frame;

		memset(&frame, 0, sizeof(frame));
		frame.render_width = frame.output_width = 64;
		frame.render_height = frame.output_height = 32;
		printf("dispatch %d\n", xgpu_vulkan_fsr3_dispatch(&frame));
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "denoiser"))
		return denoiser_test(argv[2]);
	return 2;
}
