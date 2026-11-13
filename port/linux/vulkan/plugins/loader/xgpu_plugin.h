/*
XGPU_PLUGIN.H

The host's loader of PRISM's renderer plugins (prism_plugin.h,
docs/prism_plugin.md): a plugin's shared library opened by name from a
folder, its entry called, its ABI and sizes checked; on any failure the
library closed, the reason logged once, and nothing returned, so that the
renderer draws its own path. Nothing is opened unless a name is given. Not
yet called by the renderer (PRISM v4's M5 wires display.upscaler and
display.denoiser "plugin:<name>" to it).
*/

#ifndef __HALO_LINUX_XGPU_PLUGIN_H
#define __HALO_LINUX_XGPU_PLUGIN_H

#include <stdint.h>

#include "prism_plugin.h"

/* a line for the log (the host's logger, or a test's) */
typedef void (*xgpu_plugin_log)(void *data, const char *line);

struct xgpu_plugin
{
	/* the library (dlopen's handle, or Windows's HMODULE) */
	void *library;
	/* its table, as its entry gave it */
	const struct prism_plugin *table;
};

/* <folder>/<name>.so (.dll on Windows) opened and checked; NULL if the
name is NULL or empty (nothing touched), not a plain file name, or the
plugin is missing or refused (one line logged saying why) */
struct xgpu_plugin *xgpu_plugin_open(const char *folder, const char *name, xgpu_plugin_log log, void *log_data);
/* the same, asking the plugin for another ABI than the host's (tests) */
struct xgpu_plugin *xgpu_plugin_open_abi(const char *folder, const char *name, uint32_t host_abi,
	xgpu_plugin_log log, void *log_data);
/* the library closed (the plugin's own state destroyed before) */
void xgpu_plugin_close(struct xgpu_plugin *plugin);

#endif
