/*
HOST_SCREENSHOT_TEST.C

The screenshot key's files (host_screenshot.c), with SDL: the names, the
folder made, the PNG's pixels (the frame's alpha made opaque, the caller's
pixels free to be reused once it answers: the file is written on a thread
of its own, and no .part is left), a second picture in the same second
kept beside the first, and a folder that cannot be made. argv[1]: an empty folder to write in.
*/

#include "host_render_stream.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { failures++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

#define WIDTH 37
#define HEIGHT 21

static void pattern(unsigned char *bgra)
{
	int index;

	for (index = 0; index < WIDTH * HEIGHT * 4; index++)
		bgra[index] = (unsigned char)(index * 13 + 5);
}

/* the name is YYYY-MM-DD_HH.MM.SS.png, or with _<n> before .png */
static int name_valid(const char *name)
{
	static const char shape[] = "dddd-dd-dd_dd.dd.dd";
	size_t index, length = strlen(name);

	if (length < sizeof(shape) - 1 + 4 || strcmp(name + length - 4, ".png"))
		return 0;
	for (index = 0; index < sizeof(shape) - 1; index++)
	{
		if (shape[index] == 'd' ? !(name[index] >= '0' && name[index] <= '9') : name[index] != shape[index])
			return 0;
	}
	return length == sizeof(shape) - 1 + 4 || name[sizeof(shape) - 1] == '_';
}

int main(int argc, char **argv)
{
	unsigned char bgra[WIDTH * HEIGHT * 4];
	char directory[1024], name[64], first[64], path[1200];
	SDL_Surface *read, *converted;
	int attempt, row, column;

	if (argc < 2)
		return 2;
	snprintf(directory, sizeof(directory), "%s/screenshots", argv[1]);

	host_screenshot_name(name, sizeof(name), 2026, 1, 2, 3, 4, 5, 0);
	CHECK(!strcmp(name, "2026-01-02_03.04.05.png"));
	host_screenshot_name(name, sizeof(name), 2026, 12, 31, 23, 59, 9, 1);
	CHECK(!strcmp(name, "2026-12-31_23.59.09_2.png"));
	host_screenshot_name(name, sizeof(name), 2026, 12, 31, 23, 59, 9, 9);
	CHECK(!strcmp(name, "2026-12-31_23.59.09_10.png"));

	/* written, in a folder made for it */
	pattern(bgra);
	CHECK(host_screenshot_write(bgra, WIDTH, HEIGHT, directory, name, sizeof(name)));
	CHECK(name_valid(name));
	/* (the caller's pixels its own again: the thread writes its copy) */
	memset(bgra, 0, sizeof(bgra));
	host_screenshot_finish();
	snprintf(path, sizeof(path), "%s/%s.part", directory, name);
	CHECK(!SDL_GetPathInfo(path, NULL));
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	read = SDL_LoadPNG(path);
	CHECK(read != NULL);
	if (read)
	{
		converted = SDL_ConvertSurface(read, SDL_PIXELFORMAT_BGRA32);
		CHECK(converted && converted->w == WIDTH && converted->h == HEIGHT);
		pattern(bgra);
		for (row = 0; converted && row < HEIGHT; row++)
		{
			const unsigned char *pixels = (const unsigned char *)converted->pixels + row * converted->pitch;

			for (column = 0; column < WIDTH * 4; column++)
			{
				unsigned char want = (column & 3) == 3 ? 0xff : bgra[row * WIDTH * 4 + column];

				if (pixels[column] != want)
				{
					CHECK(pixels[column] == want);
					row = HEIGHT;
					break;
				}
			}
		}
		SDL_DestroySurface(converted);
		SDL_DestroySurface(read);
	}

	/* a second in the same second is kept beside the first (tried until
	two fall in one second) */
	for (attempt = 0; attempt < 5; attempt++)
	{
		pattern(bgra);
		CHECK(host_screenshot_write(bgra, WIDTH, HEIGHT, directory, first, sizeof(first)));
		pattern(bgra);
		CHECK(host_screenshot_write(bgra, WIDTH, HEIGHT, directory, name, sizeof(name)));
		host_screenshot_finish();
		CHECK(name_valid(first) && name_valid(name) && strcmp(first, name));
		if (!strncmp(first, name, 19))
		{
			CHECK(!strcmp(name + 19, first[19] == '.' ? "_2.png" : name + 19));
			break;
		}
	}
	CHECK(attempt < 5);

	/* a folder that cannot be made: its reason */
	snprintf(path, sizeof(path), "%s/%s/below", directory, name);
	CHECK(!host_screenshot_write(bgra, WIDTH, HEIGHT, path, name, sizeof(name)));
	CHECK(!strcmp(name, "the screenshots folder cannot be made"));

	if (failures)
	{
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
