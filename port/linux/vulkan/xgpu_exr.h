/*
XGPU_EXR.H

A linear image written as an OpenEXR file: scan lines, no compression,
32-bit float channels (debug.pt_reference, xgpu_vulkan_rt.c). Enough of the
format for any OpenEXR reader (tools/prism_gate/exr.py reads it back).
*/

#ifndef __HALO_LINUX_XGPU_EXR_H
#define __HALO_LINUX_XGPU_EXR_H

#include <stdint.h>

#define XGPU_EXR_CHANNELS_MOST 16

/* an integer attribute written into the header (a name of at most 31
bytes) */
struct xgpu_exr_attribute
{
	const char *name;
	int32_t value;
};

/* writes width x height pixels of channel_count channels (names: each at
most 31 bytes; the file lists them sorted by name, as the format asks),
pixels[y * width * channel_count + x * channel_count + c], the top row
first; with attribute_count integer attributes. Written to "<path>.part"
and renamed over path, so that a reader never sees half a file; the
path's folder is made if it is missing (one level). 1, or 0 with the
reason in error (error_size bytes) */
int xgpu_exr_write(const char *path, uint32_t width, uint32_t height, uint32_t channel_count,
	const char *const *channel_names, const float *pixels, const struct xgpu_exr_attribute *attributes,
	uint32_t attribute_count, char *error, uint32_t error_size);

#endif
