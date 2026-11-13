/*
MENDICANT_SHIPPED.C

The build's own models file found and read (mendicant_shipped.h), as
prepared_shipped.c finds prepared.records.
*/

#include "mendicant_shipped.h"
#include "install_layout.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
	char *file;
	size_t size;
	char path[1024];
} shipped;

unsigned long mendicant_shipped_models_size(unsigned long maximum_size, char *path, int path_size)
{
	if (!shipped.file)
	{
		size_t length;

		config_folder(shipped.path, sizeof(shipped.path));
		length = strlen(shipped.path);
		snprintf(shipped.path + length, sizeof(shipped.path) - length, "%s", MENDICANT_SHIPPED_FILE);
		shipped.file = config_file_read(shipped.path, &shipped.size);
#if !defined(HALO_WINDOWS) && !defined(HALO_ANDROID) && !defined(HALO_MACOS)
		if (!shipped.file)
		{
			install_shared_file(MENDICANT_SHIPPED_FILE, shipped.path, sizeof(shipped.path));
			shipped.file = config_file_read(shipped.path, &shipped.size);
		}
#endif
	}
	if (path && path_size > 0)
		snprintf(path, (size_t)path_size, "%s", shipped.path);
	if (!shipped.file || !shipped.size || shipped.size > maximum_size)
	{
		mendicant_shipped_models_release();
		return 0;
	}
	return (unsigned long)shipped.size;
}

int mendicant_shipped_models_copy(void *into, unsigned long size)
{
	if (!shipped.file || size != (unsigned long)shipped.size)
		return 0;
	memcpy(into, shipped.file, shipped.size);
	return 1;
}

void mendicant_shipped_models_release(void)
{
	free(shipped.file);
	shipped.file = NULL;
	shipped.size = 0;
}
