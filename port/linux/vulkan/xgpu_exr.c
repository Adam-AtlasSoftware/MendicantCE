/*
XGPU_EXR.C

xgpu_exr.h's OpenEXR writer: the format's magic and version 2 (single
part, scan lines), a header of the attributes every reader needs
(channels, compression none, the data and display windows, the line
order, the pixel aspect ratio, the screen window) and the integer ones the
caller adds, then the offsets of the lines and the lines, each a chunk of
its y, its size and each channel's row of 32-bit floats in the channels'
order. Little-endian throughout, as the format is.
*/

#include "xgpu_exr.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

struct exr_writer
{
	FILE *file;
	int failed;
};

static void say(char *error, uint32_t error_size, const char *format, ...)
{
	va_list arguments;

	if (!error || !error_size)
		return;
	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);
	va_end(arguments);
}

static void put_bytes(struct exr_writer *writer, const void *bytes, size_t count)
{
	if (!writer->failed && fwrite(bytes, 1, count, writer->file) != count)
		writer->failed = 1;
}

static void put_u32(struct exr_writer *writer, uint32_t value)
{
	unsigned char bytes[4];

	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
	put_bytes(writer, bytes, 4);
}

static void put_u64(struct exr_writer *writer, uint64_t value)
{
	put_u32(writer, (uint32_t)value);
	put_u32(writer, (uint32_t)(value >> 32));
}

static void put_float(struct exr_writer *writer, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	put_u32(writer, bits);
}

static void put_string(struct exr_writer *writer, const char *text)
{
	put_bytes(writer, text, strlen(text) + 1);
}

/* an attribute's name, type and size; its value follows */
static void put_attribute(struct exr_writer *writer, const char *name, const char *type, uint32_t size)
{
	put_string(writer, name);
	put_string(writer, type);
	put_u32(writer, size);
}

static int name_valid(const char *name)
{
	size_t length = name ? strlen(name) : 0;

	return length >= 1 && length <= 31;
}

/* the path's folder made, where it has one and it is missing */
static void folder_make(const char *path)
{
	char folder[1024];
	char *slash, *backslash;

	if (snprintf(folder, sizeof(folder), "%s", path) >= (int)sizeof(folder))
		return;
	slash = strrchr(folder, '/');
	backslash = strrchr(folder, '\\');
	if (backslash && (!slash || backslash > slash))
		slash = backslash;
	if (!slash || slash == folder)
		return;
	*slash = 0;
#ifdef _WIN32
	_mkdir(folder);
#else
	mkdir(folder, 0755);
#endif
}

int xgpu_exr_write(const char *path, uint32_t width, uint32_t height, uint32_t channel_count,
	const char *const *channel_names, const float *pixels, const struct xgpu_exr_attribute *attributes,
	uint32_t attribute_count, char *error, uint32_t error_size)
{
	struct exr_writer writer;
	uint32_t order[XGPU_EXR_CHANNELS_MOST];
	uint32_t index, other, list_size, y, x;
	uint64_t header_size, line_bytes, offset;
	char part[1040];

	if (!path || !path[0] || !width || !height || !channel_count || channel_count > XGPU_EXR_CHANNELS_MOST ||
		!channel_names || !pixels)
	{
		say(error, error_size, "nothing to write");
		return 0;
	}
	if ((uint64_t)width * channel_count * 4 > 0x7fffffffu || height > 0x7fffffffu)
	{
		say(error, error_size, "%ux%u is too large", width, height);
		return 0;
	}
	for (index = 0; index < channel_count; index++)
	{
		if (!name_valid(channel_names[index]))
		{
			say(error, error_size, "channel %u's name", index);
			return 0;
		}
		order[index] = index;
	}
	for (index = 0; index < attribute_count; index++)
	{
		if (!name_valid(attributes[index].name))
		{
			say(error, error_size, "attribute %u's name", index);
			return 0;
		}
	}
	/* (the channels sorted by name, as the format lists them; a name twice
	refused) */
	for (index = 1; index < channel_count; index++)
	{
		uint32_t moved = order[index];

		for (other = index; other > 0 && strcmp(channel_names[order[other - 1]], channel_names[moved]) > 0; other--)
			order[other] = order[other - 1];
		order[other] = moved;
	}
	for (index = 1; index < channel_count; index++)
	{
		if (!strcmp(channel_names[order[index - 1]], channel_names[order[index]]))
		{
			say(error, error_size, "channel %s twice", channel_names[order[index]]);
			return 0;
		}
	}
	if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part))
	{
		say(error, error_size, "the path is too long");
		return 0;
	}
	folder_make(path);
	memset(&writer, 0, sizeof(writer));
	writer.file = fopen(part, "wb");
	if (!writer.file)
	{
		say(error, error_size, "%s cannot be made: %s", part, strerror(errno));
		return 0;
	}
	/* the magic and version (2: single part, scan lines, short names) */
	put_u32(&writer, 20000630u);
	put_u32(&writer, 2u);
	header_size = 8;
	list_size = 1;
	for (index = 0; index < channel_count; index++)
		list_size += (uint32_t)strlen(channel_names[index]) + 1 + 16;
	put_attribute(&writer, "channels", "chlist", list_size);
	for (index = 0; index < channel_count; index++)
	{
		/* (32-bit float, perceptually linear 0, three reserved bytes, x and
		y sampling 1) */
		put_string(&writer, channel_names[order[index]]);
		put_u32(&writer, 2u);
		put_u32(&writer, 0u);
		put_u32(&writer, 1u);
		put_u32(&writer, 1u);
	}
	put_bytes(&writer, "", 1);
	put_attribute(&writer, "compression", "compression", 1);
	put_bytes(&writer, "", 1);
	put_attribute(&writer, "dataWindow", "box2i", 16);
	put_u32(&writer, 0u);
	put_u32(&writer, 0u);
	put_u32(&writer, width - 1);
	put_u32(&writer, height - 1);
	put_attribute(&writer, "displayWindow", "box2i", 16);
	put_u32(&writer, 0u);
	put_u32(&writer, 0u);
	put_u32(&writer, width - 1);
	put_u32(&writer, height - 1);
	put_attribute(&writer, "lineOrder", "lineOrder", 1);
	put_bytes(&writer, "", 1);
	put_attribute(&writer, "pixelAspectRatio", "float", 4);
	put_float(&writer, 1.0f);
	put_attribute(&writer, "screenWindowCenter", "v2f", 8);
	put_float(&writer, 0.0f);
	put_float(&writer, 0.0f);
	put_attribute(&writer, "screenWindowWidth", "float", 4);
	put_float(&writer, 1.0f);
	for (index = 0; index < attribute_count; index++)
	{
		put_attribute(&writer, attributes[index].name, "int", 4);
		put_u32(&writer, (uint32_t)attributes[index].value);
	}
	put_bytes(&writer, "", 1);
	if (!writer.failed)
	{
		long at = ftell(writer.file);

		writer.failed = at < 0;
		header_size = at < 0 ? 0 : (uint64_t)at;
	}
	/* the lines' offsets, then the lines */
	line_bytes = (uint64_t)width * channel_count * 4;
	offset = header_size + (uint64_t)height * 8;
	for (y = 0; y < height; y++)
		put_u64(&writer, offset + (uint64_t)y * (8 + line_bytes));
	for (y = 0; y < height && !writer.failed; y++)
	{
		put_u32(&writer, y);
		put_u32(&writer, (uint32_t)line_bytes);
		for (index = 0; index < channel_count; index++)
		{
			const float *from = pixels + (size_t)y * width * channel_count + order[index];

			for (x = 0; x < width; x++)
				put_float(&writer, from[(size_t)x * channel_count]);
		}
	}
	if (fclose(writer.file) != 0)
		writer.failed = 1;
	if (writer.failed)
	{
		say(error, error_size, "%s cannot be written", part);
		remove(part);
		return 0;
	}
#ifdef _WIN32
	remove(path);
#endif
	if (rename(part, path) != 0)
	{
		say(error, error_size, "%s cannot be renamed to %s: %s", part, path, strerror(errno));
		remove(part);
		return 0;
	}
	return 1;
}
