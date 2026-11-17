/*
GUEST_VULKAN.C

The guest's half of the Vulkan window (HALO_VULKAN): the host import
sdl_platform.c calls, declared for the import stubs
(port/linux/vulkan/host_imports.list; the host's half is host_vulkan.c).
*/

int host_vulkan_window_open(unsigned int window, unsigned int debug, const char *shader_folder,
	const char *texture_folder, int interval, int battery_saver);
/* display.vulkan_device, before the window opens (host_vulkan.c) */
int host_vulkan_device_choose(const char *device);
/* display.upscaler's and display.denoiser's plugins' names and their folder,
before the window opens (host_vulkan.c) */
int host_vulkan_plugins_choose(const char *upscaler, const char *denoiser, const char *folder);
