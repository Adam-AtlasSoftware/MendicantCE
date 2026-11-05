/*
EMBEDDED_ASSETS.C

The assets' file read and checked, once (embedded_assets.h).
*/

#include "embedded_assets.h"
#include "install_layout.h"
#include "platform.h"
#include "port_config.h"

#include "zlib_prefixed.h"

#ifndef HALO_ANDROID
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBEDDED_ASSETS_MAGIC 0x4131484fu /* "OH1A" */
#define EMBEDDED_ASSETS_FORM 1
#define EMBEDDED_ASSETS_HEADER_SIZE 16

static const unsigned char *assets;
static size_t assets_size;

static unsigned int assets_word(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8 | (unsigned int)bytes[2] << 16 |
		(unsigned int)bytes[3] << 24;
}

/* beside config.toml, else the Linux release's (share/openh1,
install_layout.c), as brokers.txt is found (p2p_signal.c) */
static void assets_read(void)
{
	char path[1024];
	size_t size = 0, length;
	char *file;

	if (embedded_assets_size <= EMBEDDED_ASSETS_HEADER_SIZE)
		return;
	config_folder(path, sizeof(path));
	length = strlen(path);
	snprintf(path + length, sizeof(path) - length, "%s", EMBEDDED_ASSETS_FILE);
	file = config_file_read(path, &size);
#if !defined(HALO_WINDOWS) && !defined(HALO_ANDROID) && !defined(HALO_MACOS)
	if (!file)
	{
		install_shared_file(EMBEDDED_ASSETS_FILE, path, sizeof(path));
		file = config_file_read(path, &size);
	}
#endif
	if (!file)
	{
		platform_log("assets: %s cannot be read: no high-res HUD, text or menus", path);
		return;
	}
	if (size != embedded_assets_size || assets_word((const unsigned char *)file) != EMBEDDED_ASSETS_MAGIC ||
		assets_word((const unsigned char *)file + 4) != EMBEDDED_ASSETS_FORM ||
		crc32(0L, (const Bytef *)file, (uInt)size) != embedded_assets_crc)
	{
		platform_log("assets: %s is not this build's (%lu bytes, not %u): no high-res HUD, text or menus", path,
			(unsigned long)size, embedded_assets_size);
		free(file);
		return;
	}
	assets = (const unsigned char *)file;
	assets_size = size;
}

#ifndef HALO_ANDROID
static pthread_mutex_t assets_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static int assets_tried;

const void *embedded_assets_data(unsigned int offset, unsigned int size)
{
	/* (the high-res HUD's decoders ask from their threads too: the guest's C
	library has no pthread_once) */
#ifndef HALO_ANDROID
	pthread_mutex_lock(&assets_lock);
#endif
	if (!assets_tried)
	{
		assets_tried = 1;
		assets_read();
	}
#ifndef HALO_ANDROID
	pthread_mutex_unlock(&assets_lock);
#endif
	if (!assets || offset < EMBEDDED_ASSETS_HEADER_SIZE || offset > assets_size || size > assets_size - offset)
		return NULL;
	return assets + offset;
}
