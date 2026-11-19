/*
XGPU_PLUGIN.C

PRISM's renderer plugins opened and checked (xgpu_plugin.h). The checks,
in order: the name a plain file name; the library opened; its one symbol
(PRISM_PLUGIN_ENTRY_SYMBOL) found; its table given for the host's ABI; the
table's info of that ABI and no smaller than the host's; its sizes of the
Vulkan handover, an image and a frame no larger than the host's (a plugin
that expects fields the host does not fill is refused); every call
present; its strings present. A failure closes the library and logs one
line.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "xgpu_plugin.h"

#ifdef _WIN32
#define PLUGIN_SUFFIX ".dll"
#else
#define PLUGIN_SUFFIX ".so"
#endif

static void logf_line(xgpu_plugin_log log, void *data, const char *format, const char *a, const char *b)
{
	char line[1024];

	if (!log)
		return;
	snprintf(line, sizeof(line), format, a ? a : "", b ? b : "");
	log(data, line);
}

/* a name of letters, digits, '_', '-' and '.' (no folder, no "..") */
static int plain_name(const char *name)
{
	const char *c;

	if (!name[0] || name[0] == '.' || strlen(name) > 64)
		return 0;
	for (c = name; *c; c++)
	{
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
			*c == '-' || *c == '.'))
			return 0;
	}
	return 1;
}

static void *library_open(const char *path, int *missing)
{
#ifdef _WIN32
	wchar_t wide[1024];
	HMODULE module;

	*missing = 0;
	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
		return NULL;
	if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES)
	{
		*missing = 1;
		return NULL;
	}
	module = LoadLibraryW(wide);
	return (void *)module;
#else
	FILE *file = fopen(path, "rb");

	*missing = 0;
	if (!file)
	{
		*missing = 1;
		return NULL;
	}
	fclose(file);
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *library_symbol(void *library, const char *name)
{
#ifdef _WIN32
	return (void *)GetProcAddress((HMODULE)library, name);
#else
	return dlsym(library, name);
#endif
}

static void library_close(void *library)
{
	if (!library)
		return;
#ifdef _WIN32
	FreeLibrary((HMODULE)library);
#else
	dlclose(library);
#endif
}

static const char *library_error(void)
{
#ifdef _WIN32
	return "LoadLibraryW failed";
#else
	const char *error = dlerror();

	return error ? error : "dlopen failed";
#endif
}

/* why a table is refused, or NULL if it is not */
static const char *table_refused(const struct prism_plugin *table, uint32_t host_abi)
{
	const struct prism_plugin_info *info = &table->info;

	if (info->abi != host_abi || host_abi != PRISM_PLUGIN_ABI)
		return "it was built for another ABI";
	if (info->size < sizeof(struct prism_plugin_info))
		return "its info is smaller than this ABI's";
	if (info->vulkan_size > sizeof(struct prism_plugin_vulkan) || info->image_size > sizeof(struct prism_plugin_image) ||
		info->frame_size > sizeof(struct prism_plugin_frame) || !info->vulkan_size || !info->image_size ||
		!info->frame_size)
		return "it expects structures this host does not give";
	if (!info->name || !info->vendor || !info->version || !info->licence_notice || !info->credits_text)
		return "its info lacks a name, vendor, version, licence notice or credits";
	if (!table->instance_extensions || !table->device_extensions || !table->supported || !table->create ||
		!table->resize || !table->evaluate || !table->destroy || !table->last_error)
		return "its table lacks a call";
	if (!(info->capabilities & (PRISM_CAP_UPSCALE | PRISM_CAP_DENOISE | PRISM_CAP_DENOISE_UPSCALE |
		PRISM_CAP_FRAME_GENERATE)))
		return "it names no capability";
	return NULL;
}

struct xgpu_plugin *xgpu_plugin_open_abi(const char *folder, const char *name, uint32_t host_abi,
	xgpu_plugin_log log, void *log_data)
{
	prism_plugin_entry_function entry;
	const struct prism_plugin *table;
	struct xgpu_plugin *plugin;
	const char *refused;
	char path[1024];
	void *library, *symbol;
	int missing;

	if (!name || !name[0])
		return NULL;
	if (!plain_name(name))
	{
		logf_line(log, log_data, "prism plugin: \"%s\" is not a plugin's name (letters, digits, _ - . only)%s", name,
			NULL);
		return NULL;
	}
	if (!folder || !folder[0])
		folder = ".";
	if (snprintf(path, sizeof(path), "%s/%s%s", folder, name, PLUGIN_SUFFIX) >= (int)sizeof(path))
	{
		logf_line(log, log_data, "prism plugin %s: its path is too long%s", name, NULL);
		return NULL;
	}
	library = library_open(path, &missing);
	if (!library)
	{
		if (missing)
			logf_line(log, log_data, "prism plugin %s: none at %s; the renderer's own path is drawn", name, path);
		else
			logf_line(log, log_data, "prism plugin %s: cannot be opened (%s)", name, library_error());
		return NULL;
	}
	symbol = library_symbol(library, PRISM_PLUGIN_ENTRY_SYMBOL);
	if (!symbol)
	{
		logf_line(log, log_data, "prism plugin %s: refused: it has no %s", name, PRISM_PLUGIN_ENTRY_SYMBOL);
		library_close(library);
		return NULL;
	}
	memcpy(&entry, &symbol, sizeof(entry));
	table = entry(host_abi);
	refused = !table ? "it serves no table for this ABI" : table_refused(table, host_abi);
	if (refused)
	{
		logf_line(log, log_data, "prism plugin %s: refused: %s", name, refused);
		library_close(library);
		return NULL;
	}
	plugin = calloc(1, sizeof(*plugin));
	if (!plugin)
	{
		library_close(library);
		return NULL;
	}
	plugin->library = library;
	plugin->table = table;
	logf_line(log, log_data, "prism plugin %s: opened (%s)", name, table->info.version);
	return plugin;
}

struct xgpu_plugin *xgpu_plugin_open(const char *folder, const char *name, xgpu_plugin_log log, void *log_data)
{
	return xgpu_plugin_open_abi(folder, name, PRISM_PLUGIN_ABI, log, log_data);
}

void xgpu_plugin_close(struct xgpu_plugin *plugin)
{
	if (!plugin)
		return;
	library_close(plugin->library);
	free(plugin);
}
