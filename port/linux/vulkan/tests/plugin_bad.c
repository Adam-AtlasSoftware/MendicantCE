/* A plugin the loader must refuse (tools/test_prism_plugin.py), by
BAD_KIND: 1 no entry symbol, 2 an info of another ABI, 3 an info smaller
than the ABI's, 4 a table without evaluate, 5 a frame larger than the
host's, 6 no table at all */

#include <stddef.h>

#include "prism_plugin.h"

static int extensions(const char **names, uint32_t *count) { (void)names; *count = 0; return 1; }
static int device_extensions(VkPhysicalDevice physical, const char **names, uint32_t *count)
{ (void)physical; (void)names; *count = 0; return 1; }
static int supported(VkPhysicalDevice physical) { (void)physical; return 1; }
static int create(const struct prism_plugin_vulkan *vulkan, void **context) { (void)vulkan; *context = NULL; return 1; }
static int resize(void *c, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t q)
{ (void)c; (void)a; (void)b; (void)d; (void)e; (void)q; return 1; }
static int evaluate(void *c, const struct prism_plugin_frame *f) { (void)c; (void)f; return 1; }
static void destroy(void *c) { (void)c; }
static const char *last_error(void *c) { (void)c; return ""; }

static const struct prism_plugin table =
{
	{
		BAD_KIND == 2 ? PRISM_PLUGIN_ABI + 1 : PRISM_PLUGIN_ABI,
		BAD_KIND == 3 ? (uint32_t)offsetof(struct prism_plugin_info, capabilities) : sizeof(struct prism_plugin_info),
		"bad", "test", "0", "", "", PRISM_CAP_UPSCALE,
		sizeof(struct prism_plugin_vulkan), sizeof(struct prism_plugin_image),
		BAD_KIND == 5 ? sizeof(struct prism_plugin_frame) + 64 : sizeof(struct prism_plugin_frame),
	},
	extensions, device_extensions, supported, create, resize, BAD_KIND == 4 ? NULL : evaluate, destroy, last_error,
};

#if BAD_KIND == 1
PRISM_PLUGIN_EXPORT const struct prism_plugin *prism_plugin_entry_misnamed(uint32_t host_abi)
#else
PRISM_PLUGIN_EXPORT const struct prism_plugin *prism_plugin_entry(uint32_t host_abi)
#endif
{
	(void)host_abi;
	return BAD_KIND == 6 ? NULL : &table;
}
