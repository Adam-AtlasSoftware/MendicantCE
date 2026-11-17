/* display.upscaler's FSR 3.1 host half (xgpu_vulkan_fsr3.c) without a GPU
(tools/test_upscale.py): its jitter's sequence, and a dispatch with no device
refused (the view then stretched into the window as it is).

  upscale_test jitter <render width> <output width>   the sequence's length,
                                                      then its offsets
  upscale_test nodevice                               the dispatch refused */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xgpu_vulkan_fsr3.h"
#include "xgpu_vulkan_upscale_shaders.h"

/* ---------- the core's functions, stubbed: no device */

int xgpu_vulkan_handles(struct xgpu_vulkan_handles *handles)
{
	memset(handles, 0, sizeof(*handles));
	return 0;
}

uint64_t xgpu_vulkan_completed(void) { return 0; }
void xgpu_vulkan_wait(uint64_t fence) { (void)fence; }
uint64_t xgpu_vulkan_serial(void) { return 1; }
void *xgpu_vulkan_upload(uint32_t size, uint32_t alignment, struct xgpu_vulkan_range *range)
{
	(void)size; (void)alignment; (void)range;
	return NULL;
}
const struct xgpu_vulkan_upscale_program xgpu_vulkan_upscale_programs[XGPU_VULKAN_UPSCALE_PROGRAM_COUNT];

void host_logf(int level, const char *format, ...)
{
	(void)level; (void)format;
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
	return 2;
}
