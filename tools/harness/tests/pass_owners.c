/*
PASS_OWNERS.C (test)

#1151: the Vulkan backend's passes labeled by the graphics setting that
owns them (port/linux/vulkan/xgpu_vulkan.c's xgpu_vulkan_pass_owner,
pass_times_label, pass_times_groups_log), and the performance overlay's
reading of the groups' line (port/linux/game/perf_overlay.c's
perf_overlay_pass_groups). test_pass_owners.py takes the code.
*/

#include "harness.h"

#include <stdint.h>

#define HOST_LOG_INFO 4
#define PERF_OVERLAY_LINE_SIZE 32

struct xgpu_vulkan_image
{
	struct
	{
		uint32_t samples;
	} description;
};

struct pass_attachment
{
	struct xgpu_vulkan_image *image;
};

struct segment
{
	const char *ended_by;
	const char *owner;
	struct
	{
		struct
		{
			uint32_t width, height;
		} extent;
	} area;
	struct pass_attachment color, depth;
};

static int host_pass_times;

static struct
{
	const char *pass_owner;
	struct
	{
		uint32_t frames, draws;
	} statistics;
} vk;

/* the host's last line */
static char logged[2048];

static void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vsnprintf(logged, sizeof(logged), format, arguments);
	va_end(arguments);
}

#include "under_test.inc"

static void sum_add(const char *label, double milliseconds)
{
	snprintf(pass_times.sums[pass_times.sum_count].label, PASS_TIMES_LABEL, "%s", label);
	pass_times.sums[pass_times.sum_count].milliseconds = milliseconds;
	pass_times.sums[pass_times.sum_count].count = 1.0;
	pass_times.sum_count++;
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	if (!strcmp(test, "off"))
	{
		/* (the passes not timed: no owner kept, the label what the pass
		draws into, as before) */
		struct xgpu_vulkan_image image = { { 1 } };
		struct segment segment;

		host_pass_times = 0;
		xgpu_vulkan_pass_owner("SMAA");
		CHECK(vk.pass_owner == NULL, "an owner kept with the passes not timed");
		memset(&segment, 0, sizeof(segment));
		segment.area.extent.width = 1920;
		segment.area.extent.height = 1080;
		segment.color.image = &image;
		segment.ended_by = "xgpu_vulkan_set_target_views";
		CHECK(!strcmp(pass_times_label(&segment, 0), "1920x1080 colour, ended by xgpu_vulkan_set_target_views"),
			"label %s", pass_times_label(&segment, 0));
		return 0;
	}
	if (!strcmp(test, "owned"))
	{
		struct xgpu_vulkan_image image = { { 1 } };
		struct segment segment;

		host_pass_times = 10;
		xgpu_vulkan_pass_owner("SMAA");
		CHECK(vk.pass_owner && !strcmp(vk.pass_owner, "SMAA"), "the owner not kept");
		memset(&segment, 0, sizeof(segment));
		segment.owner = vk.pass_owner;
		segment.color.image = &image;
		segment.ended_by = "xgpu_vulkan_set_target_views";
		CHECK(!strcmp(pass_times_label(&segment, 0), "SMAA"), "label %s", pass_times_label(&segment, 0));
		xgpu_vulkan_pass_owner(NULL);
		CHECK(vk.pass_owner == NULL, "the owner kept past its work");
		CHECK(pass_times_owned("SMAA") && pass_times_owned("MSAA RESOLVE") && !pass_times_owned("outside passes") &&
			!pass_times_owned("1920x1080 colour, ended by x") && !pass_times_owned("aside 64x64 colour"),
			"owned labels told apart wrongly");
		return 0;
	}
	if (!strcmp(test, "groups"))
	{
		char labels[4][PERF_OVERLAY_LINE_SIZE];
		real milliseconds[4];
		long draws = -1;
		short count;

		/* (two timed frames: the summary's sums, then their groups) */
		pass_times.frames = 2.0;
		sum_add("1920x1080 colour depth, ended by xgpu_vulkan_set_target_views", 4.0);
		sum_add("SMAA", 0.186);
		sum_add("outside passes", 0.2);
		sum_add("PANINI", 0.5);
		sum_add("MSAA RESOLVE", 0.04);
		sum_add("FINAL PASS", 0.1);
		vk.statistics.frames = 300;
		vk.statistics.draws = 300 * 812;
		pass_times_groups_log();
		CHECK(strstr(logged, "vulkan: pass groups: draws 812; OTHER 2.100; PANINI 0.250; SMAA 0.093; FINAL PASS 0.050; "
			"MSAA RESOLVE 0.020") == logged, "logged %s", logged);
		/* (as the host's recent lines hold it, among others) */
		{
			char log[4096];

			snprintf(log, sizeof(log), "vulkan: pass groups: draws 1; OLD 9.000\nvulkan: something else\n%s\n"
				"vulkan: pass times, 2.4 ms a frame\n", logged);
			count = perf_overlay_pass_groups(log, labels, milliseconds, 4, &draws);
		}
		CHECK(count == 4, "%d groups read", count);
		CHECK(draws == 812, "%ld draws", draws);
		CHECK(!strcmp(labels[0], "OTHER") && !strcmp(labels[1], "PANINI") && !strcmp(labels[2], "SMAA") &&
			!strcmp(labels[3], "FINAL PASS"), "labels %s, %s, %s, %s", labels[0], labels[1], labels[2], labels[3]);
		CHECK(milliseconds[2] > 0.0925f && milliseconds[2] < 0.0935f, "SMAA %f ms", milliseconds[2]);
		CHECK(perf_overlay_pass_groups("vulkan: pass times, 2.4 ms\n", labels, milliseconds, 4, &draws) == 0,
			"groups read where none was logged");
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
