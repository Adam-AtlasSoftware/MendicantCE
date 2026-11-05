/*
XBOX_TEXTURES.C

The high-res replacements drawn in place of the Xbox's textures, and the
Custom Edition channel orders; the host caches the textures themselves
(port/linux/vulkan/xgpu_vulkan_textures.c, port/macos/xgpu_metal_textures.c),
decoded by xbox_texture_decode.c.

An Xbox texture is a Direct3D header - Common, Data (physical address),
Lock, Format and Size - over texels in guest memory. Power-of-two textures
are swizzled (Morton order, one level after another); textures with a Size
field are linear, with a pitch, and are addressed with texel coordinates.
DXT textures are stored as plain 4x4 blocks. Everything except DXT is
converted to 32-bit BGRA on upload.

A cached texture stays valid until any page it was read from is written;
memory_watch.c detects that by write-protecting the pages.

Here are the replacements, as images (xgpu_images.h), and the Custom
Edition channel orders, which the command stream hands the host.
*/

#include "xgpu_images.h"
#include "hud_hires.h"
#include "menu_files.h"
#include "menu_pictures.h"
#include "text_hires.h"
#include "../game/cache_file_formats.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---------- Custom Edition channel orders

Halo PC keeps what some textures hold in other channels than the game reads
it from (enum custom_edition_channel_order): a model shader's multipurpose
masks, and a HUD meter's shape and fill order. The Custom Edition map
loading says which texels hold which order as they arrive
(port/linux/game/custom_edition_bitmaps.c), and textures made of them are
sampled with each channel taken from where Halo PC keeps it, which leaves
them as compressed as they were. Addresses stay listed until other texels
arrive there, which the loading also says, or the map goes; the game and the
renderer share a thread. */

/* for each order, the channel (red, green, blue, alpha) of the texels each
channel is sampled from */
static const unsigned char custom_edition_channel_sources[NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS][4] =
{
	{ 0, 1, 2, 3 },
	/* specular, self-illumination, color change and the auxiliary mask */
	{ 2, 1, 3, 0 },
	/* the fill order in color, the shape in alpha */
	{ 3, 3, 3, 0 },
	/* a meter's, in the Xbox's order as its map has it (drawn as it is, by
	meters too) */
	{ 0, 1, 2, 3 },
};

struct custom_edition_texels
{
	unsigned long address;
	unsigned char channel_order;
};

static struct custom_edition_texels *custom_edition_texels;
static unsigned long custom_edition_texel_count;
static unsigned long custom_edition_texel_capacity;

/* whether a HUD meter of a Custom Edition map is being drawn (hud_draw.c):
a sheet that is a meter's and other pictures' too keeps the order it has for
the pictures, and its texels in the Xbox's order are sampled in the meters'
for the meter's draw alone (its first stage) */
static int custom_edition_hud_meter_active;

void halo_custom_edition_hud_meter(int active)
{
	custom_edition_hud_meter_active = active;
}

/* the order of the texels at address */
static unsigned char custom_edition_texels_order(unsigned long address)
{
	unsigned long index;

	for (index = 0; index < custom_edition_texel_count; index++)
	{
		if (custom_edition_texels[index].address == address)
			return custom_edition_texels[index].channel_order;
	}
	return _custom_edition_channels_xbox;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	unsigned long address = (unsigned long)texels;
	unsigned long index;

	if (channel_order >= NUMBER_OF_CUSTOM_EDITION_CHANNEL_ORDERS)
		channel_order = _custom_edition_channels_xbox;
	for (index = 0; index < custom_edition_texel_count && custom_edition_texels[index].address != address; index++)
	{
	}
	if (index < custom_edition_texel_count)
	{
		if (channel_order == _custom_edition_channels_xbox)
			custom_edition_texels[index] = custom_edition_texels[--custom_edition_texel_count];
		else
			custom_edition_texels[index].channel_order = channel_order;
	}
	else if (channel_order != _custom_edition_channels_xbox)
	{
		if (custom_edition_texel_count == custom_edition_texel_capacity)
		{
			unsigned long capacity = custom_edition_texel_capacity ? custom_edition_texel_capacity * 2 : 64;
			struct custom_edition_texels *grown = realloc(custom_edition_texels, capacity * sizeof(*grown));

			if (!grown)
			{
				platform_log("no memory to list the texels at %08lx: they are sampled in Halo PC's channel order",
					address);
				return;
			}
			custom_edition_texels = grown;
			custom_edition_texel_capacity = capacity;
		}
		custom_edition_texels[custom_edition_texel_count].address = address;
		custom_edition_texels[custom_edition_texel_count].channel_order = channel_order;
		custom_edition_texel_count++;
	}
}

void halo_custom_edition_texels_forget(void)
{
	custom_edition_hud_meter_active = 0;
	free(custom_edition_texels);
	custom_edition_texels = NULL;
	custom_edition_texel_count = 0;
	custom_edition_texel_capacity = 0;
}

unsigned long xgpu_texture_channels(unsigned long address, int stage)
{
	unsigned char order = custom_edition_texels_order(address);
	const unsigned char *sources;

	if (order == _custom_edition_channels_xbox && stage == 0 && custom_edition_hud_meter_active)
		order = _custom_edition_channels_hud_meter;
	if (order == _custom_edition_channels_xbox || order == _custom_edition_channels_hud_meter_xbox)
		return 0;
	sources = custom_edition_channel_sources[order];
	return 0x100 | sources[0] | (sources[1] << 2) | (sources[2] << 4) | ((unsigned long)sources[3] << 6);
}

/* ---------- the replacements, where the renderer is on the host

What the host's cache draws in a texture's place: the high-res text's
atlas, a menu's art, a high-res HUD texture for a placeholder bitmap, or
for the bitmap whose pixels these are. Which HUD texture stands for a
bitmap's pixels is found again whenever they change (the write tracking),
for each texture by its Data, Format and Size. */

#define REPLACEMENT_BUCKET_COUNT 1024
#define REPLACEMENT_IDLE_FRAMES 1800

/* A menu's picture of ui.map's drawn larger than it is (kind 5: a mission's
picture, a difficulty's badge) is upscaled on a thread of its own, the
original drawn until it is: upscaling one (256x128 to 1024x512) took a few
milliseconds of the frame that first drew it, four of them as Choose
Mission opened. */
struct upscale_job
{
	struct upscale_job *next;
	unsigned char *bgra, *pixels;
	unsigned long width, height;
	long kind;
	int done;
	/* its entry gone: freed by the thread when done */
	int abandoned;
};

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t work;
	int running;
	struct upscale_job *queue;
} upscaler = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void *upscaler_thread(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&upscaler.lock);
	for (;;)
	{
		struct upscale_job *job;
		unsigned char *pixels;

		while (!upscaler.queue)
			pthread_cond_wait(&upscaler.work, &upscaler.lock);
		job = upscaler.queue;
		upscaler.queue = job->next;
		pthread_mutex_unlock(&upscaler.lock);
		pixels = job->abandoned ? NULL :
			hud_hires_upscale_kind_pixels(job->bgra, job->width, job->height, job->kind);
		pthread_mutex_lock(&upscaler.lock);
		free(job->bgra);
		job->bgra = NULL;
		if (job->abandoned)
		{
			free(pixels);
			free(job);
			continue;
		}
		job->pixels = pixels;
		job->done = 1;
	}
	return NULL;
}

/* the texels (BGRA, the job's now) upscaled on the thread: the job, or NULL
if it cannot be */
static struct upscale_job *upscale_begin(unsigned char *bgra, unsigned long width, unsigned long height, long kind)
{
	struct upscale_job *job = calloc(1, sizeof(*job)), **end;

	if (!job)
	{
		free(bgra);
		return NULL;
	}
	job->bgra = bgra;
	job->width = width;
	job->height = height;
	job->kind = kind;
	pthread_mutex_lock(&upscaler.lock);
	if (!upscaler.running)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, upscaler_thread, NULL) != 0)
		{
			pthread_mutex_unlock(&upscaler.lock);
			free(bgra);
			free(job);
			return NULL;
		}
		pthread_detach(thread);
		upscaler.running = 1;
	}
	for (end = &upscaler.queue; *end; end = &(*end)->next)
		;
	*end = job;
	pthread_cond_signal(&upscaler.work);
	pthread_mutex_unlock(&upscaler.lock);
	return job;
}

/* the job's image, once it is done (the job freed); 0 until then, or if it
could not be made */
static unsigned int upscale_take(struct upscale_job **job, unsigned long *levels)
{
	unsigned char *pixels;
	unsigned long width, height;

	pthread_mutex_lock(&upscaler.lock);
	if (!(*job)->done)
	{
		pthread_mutex_unlock(&upscaler.lock);
		return 0;
	}
	pthread_mutex_unlock(&upscaler.lock);
	pixels = (*job)->pixels;
	width = (*job)->width;
	height = (*job)->height;
	free(*job);
	*job = NULL;
	return hud_hires_upscaled_texture(pixels, width, height, levels);
}

/* the job let go of (freed now if done, else by the thread) */
static void upscale_drop(struct upscale_job **job)
{
	if (!*job)
		return;
	pthread_mutex_lock(&upscaler.lock);
	if ((*job)->done)
	{
		free((*job)->pixels);
		free(*job);
	}
	else
	{
		(*job)->abandoned = 1;
	}
	pthread_mutex_unlock(&upscaler.lock);
	*job = NULL;
}

struct replacement_entry
{
	struct replacement_entry *next;
	DWORD data, format_word, size_word;
	/* the high-res HUD texture (hud_hires.h), or -1 */
	long override;
	unsigned int hud_upscale;
	unsigned long hud_upscale_levels, last_used_frame;
	BOOL hud_upscale_checked;
	/* its upscaling on the thread (a menu's picture's), until it is done */
	struct upscale_job *upscale_job;
	/* the menu picture's pose whose original this is (menu_pictures.h), -1
none, -2 not yet looked for */
	long picture;
	/* its pages' newest generation when it was found, and the memory
	watch serial as of which that is known (as texture_entry's) */
	unsigned long generation, watched_serial;
};

static struct replacement_entry *replacement_buckets[REPLACEMENT_BUCKET_COUNT];
static unsigned long replacement_frame;

void xgpu_texture_cache_begin_frame(void)
{
	unsigned long changed = hud_hires_settings_changed();
	unsigned long index;

	replacement_frame++;
	if (!changed && replacement_frame % 600)
		return;
	for (index = 0; index < REPLACEMENT_BUCKET_COUNT; index++)
	{
		struct replacement_entry **link = &replacement_buckets[index];

		while (*link)
		{
			struct replacement_entry *entry = *link;
			struct xgpu_texture_description description;
			unsigned long address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(entry->data);

			if (replacement_frame - entry->last_used_frame > REPLACEMENT_IDLE_FRAMES)
			{
				*link = entry->next;
				xgpu_image_delete(entry->hud_upscale);
				upscale_drop(&entry->upscale_job);
				free(entry);
				continue;
			}
			if (changed)
			{
				xgpu_texture_describe(entry->format_word, entry->size_word, &description);
				if (hud_hires_bitmap_affected(address, (long)description.width, (long)description.height, changed))
				{
					/* Find the replacement again even when its pixels have not
					changed. Keep the images used by frames still in flight. */
					entry->generation = 0;
					entry->watched_serial = 0;
				}
			}
			link = &entry->next;
		}
	}
}

static struct replacement_entry *replacement_get(const DWORD *resource, const struct xgpu_texture_description *description)
{
	DWORD data = resource[1], format_word = resource[3], size_word = resource[4];
	unsigned long hash = ((unsigned long)data * 2654435761UL ^ (unsigned long)format_word * 2246822519UL ^
		(unsigned long)size_word * 3266489917UL) >> 20;
	struct replacement_entry **bucket = &replacement_buckets[hash % REPLACEMENT_BUCKET_COUNT];
	struct replacement_entry *entry;
	unsigned long address, size, generation, watch_serial = memory_watch_serial();

	if (description->format == 0x0b || description->cube_map || description->depth > 1 ||
		!xgpu_texture_size_supported(description))
		return NULL;
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->data == data && entry->format_word == format_word && entry->size_word == size_word)
			break;
	}
	if (!entry)
	{
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return NULL;
		entry->data = data;
		entry->format_word = format_word;
		entry->size_word = size_word;
		entry->override = -1;
		entry->picture = -2;
		entry->next = *bucket;
		*bucket = entry;
	}
	entry->last_used_frame = replacement_frame;
	if (entry->watched_serial && entry->watched_serial == watch_serial)
		return entry;
	address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(data);
	size = xgpu_texture_face_size(description);
	generation = memory_watch_generation(address, size);
	if (!entry->generation || generation > entry->generation)
	{
		unsigned long levels;

		/* protect first, so a write racing with the search is noticed */
		memory_watch_protect(address, size);
		entry->generation = memory_watch_generation(address, size);
		if (!entry->generation)
			entry->generation = 1;
		xgpu_image_delete(entry->hud_upscale);
		entry->hud_upscale = 0;
		upscale_drop(&entry->upscale_job);
		entry->hud_upscale_checked = FALSE;
		entry->picture = -2;
		entry->override = hud_hires_override_find(address, description->width, description->height,
			description->levels > 1 ? xgpu_texture_level_offset(description, 1) : size);
		if (entry->override >= 0 && !hud_hires_override_texture(entry->override, &levels))
			entry->override = -1;
	}
	entry->watched_serial = watch_serial;
	return entry;
}

unsigned int xgpu_texture_replacement(DWORD resource, struct xgpu_texture_description *description)
{
	const DWORD *header = (const DWORD *)resource;
	unsigned long levels;
	unsigned int image;
	long override;
	struct replacement_entry *entry;

	/* (the high-res text's atlas, for its placeholder bitmap: text_hires.h) */
	if ((image = text_hires_atlas_texture(header[1])) != 0)
	{
		description->levels = 1;
		return image;
	}
	/* (a menu's bitmap: menu_files.h) */
	if ((image = menu_art_texture(header[1], &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		return image;
	}
	/* (a high-res texture drawn for some sprites, for the placeholder the
	game draws them from: hud_hires.h) */
	if ((image = hud_hires_placeholder_texture(header[1], &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		return image;
	}
	entry = replacement_get(header, description);
	if (!entry)
		return 0;
	/* (a menu's picture of a mission or a map, drawn again from the
player's map: menu_pictures.c) */
	if (entry->picture == -2)
	{
		entry->picture = description->width == MENU_PICTURE_FRAME_WIDTH &&
			description->height == MENU_PICTURE_FRAME_HEIGHT ?
			menu_picture_at((unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(header[1]), (long)description->width,
				(long)description->height) : -1;
	}
	if (entry->picture >= 0 && (image = menu_pictures_texture(entry->picture, &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		return image;
	}
	override = entry->override;
	if (override >= 0 && (image = hud_hires_override_texture(override, &levels)) != 0)
	{
		description->levels = levels;
		description->hires = TRUE;
		description->hires_coverage = hud_hires_override_coverage(override);
		description->hires_point_threshold = hud_hires_override_point_threshold(override);
		return image;
	}
	if (hud_hires_enabled())
	{
		if (!entry->hud_upscale_checked)
		{
			unsigned long address = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(header[1]);
			unsigned long size = xgpu_texture_face_size(description);
			long kind = hud_hires_upscale_at(address, description->width, description->height);

			/* (a Custom Edition map's stock sheet, kind 4, may be DXT and
			up to 512 square; a menu's picture of ui.map's, kind 5, DXT) */
			unsigned long largest = kind == 4 ? 512 : 256;

			entry->hud_upscale_checked = TRUE;
			if (kind && size && (!description->compressed || kind == 4 || kind == 5) &&
				description->width <= largest && description->height <= largest &&
				custom_edition_texels_order(address) == _custom_edition_channels_xbox &&
				platform_is_contiguous((void *)address) && platform_is_contiguous((void *)(address + size - 1)))
			{
				DWORD *pixels = malloc(description->width * description->height * 4);
				BOOL decoded = FALSE;

				if (pixels && description->compressed)
				{
					xgpu_texture_decode_dxt(description, 0, (const unsigned char *)address, pixels);
					decoded = TRUE;
				}
				else if (pixels)
				{
					decoded = xgpu_texture_decode_level(description, 0, (const unsigned char *)address, NULL, pixels);
				}
				/* (a menu's picture upscaled on the thread, the original drawn
				meanwhile; the HUD's sheets, as the HUD is first drawn, here) */
				if (decoded && kind == 5)
				{
					entry->upscale_job = upscale_begin((unsigned char *)pixels, description->width,
						description->height, kind);
					pixels = NULL;
				}
				else if (decoded)
				{
					entry->hud_upscale = hud_hires_upscale_texture((const unsigned char *)pixels,
						description->width, description->height, kind, &entry->hud_upscale_levels);
				}
				free(pixels);
			}
		}
		if (entry->upscale_job)
			entry->hud_upscale = upscale_take(&entry->upscale_job, &entry->hud_upscale_levels);
		if (entry->hud_upscale)
		{
			description->hires = TRUE;
			description->levels = entry->hud_upscale_levels;
			return entry->hud_upscale;
		}
	}
	return 0;
}
