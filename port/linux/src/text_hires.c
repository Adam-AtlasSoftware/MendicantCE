/*
TEXT_HIRES.C

The game's text drawn with fonts at the display's resolution (text_hires.h).

Each font tag the fonts draw (port/assets/fonts/fonts.json) gets its font
sized so that its capitals are as tall as the tag's, and its fallback, the
font that draws a character the first has not (Overpass, for the few that
Cyborg Interface Gothic lacks), sized and fitted the same way: the game's layout,
from the tag's character widths, is kept, and the fonts were chosen to fit
the Xbox maps' widths. A tag whose letters are narrower (the fonts of a
Custom Edition loc.map) gets its font narrowed until its letters are, on
the whole, as wide as the tag's, so that they do not run into each other;
the Xbox maps' fonts are narrowed by a few hundredths.

The glyphs are rasterized on a thread of their own, never on the game's,
at the display's pixels per unit of the 480 lines, into an 8-bit atlas: a
font's letters, digits and punctuation (ASCII's) as the font is made, which
the maps' fonts are as each map loads (rasterizer_text.c's
rasterizer_text_fonts_ahead), before the menus or the HUD draw them, and any
other character as it is first drawn. Until a string's every glyph is in
the atlas it is drawn as the game draws it, from its font tag's bitmaps,
and from the atlas once they are (a frame or two later: rasterizing a
glyph takes some tens of microseconds, the ASCII of the maps' five fonts
some milliseconds, of the thread's time). The image the game's placeholder
bitmap stands for (xbox_textures.c) is the atlas, uploaded again when
glyphs have been added to it and the placeholder is bound. A change of the
display's scale (a window made fullscreen) starts it over, every font's
ASCII rasterized again at once, on the thread; a full atlas too, its glyphs
rasterized again as they are drawn.

The atlas is not kept on disk: the thread rasterizes the maps' fonts'
ASCII (some 470 glyphs) in tens of milliseconds of its own time, as the
loading screen shows, and reading it back would save nothing a player sees.
*/

#include "text_hires.h"
#include "embedded_assets.h"
#include "platform.h"
#include "port_config.h"
/* (the renderer's backend is on the host: the pictures are images) */
#include "xgpu_images.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

/* (the Xbox maps' tags and a Custom Edition loc.map's, which share names,
each at its oversamples) */
#define MAXIMUM_FONTS 16
#define ATLAS_SIZE 2048
/* clear texels around each glyph, so that filtering never reaches the next */
#define ATLAS_PADDING 2
#define GLYPH_CACHE_SIZE 4096

/* a font file as it draws a tag (none where scale is 0) */
struct font_face
{
	stbtt_fontinfo info;
	/* font units to units of the 480 lines */
	float scale;
	/* the glyphs' widths, against their heights, to fit the tag's letters */
	float narrowing;
};

/* the font, and the fallback that draws the characters it has not */
#define FONT_FACES 2

static struct
{
	char tag[64];
	/* the tag's measures it was made for */
	float cap_height;
	long letters_width;
	struct font_face faces[FONT_FACES];
	/* how many times the display's pixels its glyphs are rasterized with */
	float oversample;
} fonts[MAXIMUM_FONTS];
static long font_count;

/* a glyph's place in the cache (the game thread's) */
enum
{
	_glyph_free,
	/* asked of the thread */
	_glyph_wanted,
	/* in the atlas (or one with no ink: none) */
	_glyph_ready,
	/* the font has none */
	_glyph_missing,
};

static struct
{
	short state;
	long font;
	unsigned long code;
	short x, y, width, height;
	short left, top;    /* the glyph's corner, in pixels from the pen on the baseline */
	float advance;      /* in units of the 480 lines */
} glyphs[GLYPH_CACHE_SIZE];

/* the glyphs asked of the thread, and those it has rasterized: rings of
GLYPH_CACHE_SIZE (no more can be asked than the cache has room for) */
struct glyph_job
{
	long font;
	/* (the font's face that has it: font_glyph_face) */
	int face;
	unsigned long code;
	/* the atlas it is for (its generation) and the pixels it is drawn at */
	unsigned long generation;
	float pixels, pixels_x;
	/* (done) where it is, its corner and advance; full: the atlas is */
	short x, y, width, height, left, top;
	float advance;
	int full;
};

static struct
{
	pthread_mutex_t lock;
	pthread_cond_t work;
	int running;
	struct glyph_job wanted[GLYPH_CACHE_SIZE];
	unsigned long wanted_first, wanted_count;
	struct glyph_job done[GLYPH_CACHE_SIZE];
	unsigned long done_first, done_count;
	/* (the atlas's, the lock's) where the next glyph goes */
	int pack_x, pack_y, pack_row_height;
	unsigned long pack_generation;
} rasterizer = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

/* (the game thread's) glyphs rasterized, waiting to be taken: read without
the lock, to take none when there are none */
static volatile unsigned long rasterizer_done;

static unsigned char *atlas;
static float atlas_scale;   /* the pixels per unit its glyphs have */
/* changed each time it is emptied: the glyphs rasterized for one before are
let go */
static unsigned long atlas_generation;
static int atlas_changed;
static unsigned long placeholder_data, placeholder_width, placeholder_height;
static unsigned int atlas_texture;

/* tag names compared as the game does, ignoring case (and without the POSIX
strcasecmp, which the Windows build has not) */
static int names_differ(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return tolower((unsigned char)*a) != tolower((unsigned char)*b);
}

int text_hires_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.high_res_text");
	}
	return enabled;
}

static void (*reset_hook)(void);

void text_hires_set_reset_hook(void (*hook)(void))
{
	reset_hook = hook;
}

/* the atlas emptied, for a display of this scale: every glyph to be
rasterized again (those asked of the thread for the atlas before are let
go) */
static void atlas_reset(float scale)
{
	long index;

	/* (text batched from the glyphs there drawn first) */
	if (reset_hook && atlas)
		reset_hook();
	pthread_mutex_lock(&rasterizer.lock);
	if (!atlas)
		atlas = malloc(ATLAS_SIZE * ATLAS_SIZE);
	if (atlas)
		memset(atlas, 0, ATLAS_SIZE * ATLAS_SIZE);
	atlas_generation++;
	rasterizer.pack_x = rasterizer.pack_y = rasterizer.pack_row_height = 0;
	rasterizer.pack_generation = atlas_generation;
	/* (those waiting are dropped by their generation, as they are taken) */
	pthread_mutex_unlock(&rasterizer.lock);
	for (index = 0; index < GLYPH_CACHE_SIZE; index++)
		glyphs[index].state = _glyph_free;
	atlas_scale = scale;
	atlas_changed = 1;
}

/* (the thread's, the lock held) a glyph rasterized: placed in the atlas, its
pixels copied there; 0 if the atlas is full */
static int glyph_place(struct glyph_job *job, const unsigned char *pixels)
{
	int width = job->width, height = job->height;

	if (width <= 0 || height <= 0)
		return 1;
	if (rasterizer.pack_x + width + 2 * ATLAS_PADDING > ATLAS_SIZE)
	{
		rasterizer.pack_x = 0;
		rasterizer.pack_y += rasterizer.pack_row_height;
		rasterizer.pack_row_height = 0;
	}
	if (rasterizer.pack_y + height + 2 * ATLAS_PADDING > ATLAS_SIZE)
		return 0;
	job->x = (short)(rasterizer.pack_x + ATLAS_PADDING);
	job->y = (short)(rasterizer.pack_y + ATLAS_PADDING);
	{
		int row;

		for (row = 0; row < height; row++)
			memcpy(atlas + (size_t)(job->y + row) * ATLAS_SIZE + job->x, pixels + (size_t)row * width, (size_t)width);
	}
	rasterizer.pack_x += width + 2 * ATLAS_PADDING;
	if (height + 2 * ATLAS_PADDING > rasterizer.pack_row_height)
		rasterizer.pack_row_height = height + 2 * ATLAS_PADDING;
	return 1;
}

/* the thread: each glyph asked for rasterized (outside the lock: the
fonts it reads do not change once made), then placed in the atlas */
static void *rasterizer_thread(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&rasterizer.lock);
	for (;;)
	{
		struct glyph_job job;
		struct font_face *face;
		int index, x0 = 0, y0 = 0, x1 = 0, y1 = 0, advance = 0, bearing = 0;
		unsigned char *pixels = NULL;

		while (!rasterizer.wanted_count)
			pthread_cond_wait(&rasterizer.work, &rasterizer.lock);
		job = rasterizer.wanted[rasterizer.wanted_first];
		rasterizer.wanted_first = (rasterizer.wanted_first + 1) % GLYPH_CACHE_SIZE;
		rasterizer.wanted_count--;
		/* (for an atlas emptied since: let go) */
		if (job.generation != rasterizer.pack_generation)
			continue;
		pthread_mutex_unlock(&rasterizer.lock);
		face = &fonts[job.font].faces[job.face];
		index = stbtt_FindGlyphIndex(&face->info, (int)job.code);
		if (index)
		{
			stbtt_GetGlyphBitmapBox(&face->info, index, job.pixels_x, job.pixels, &x0, &y0, &x1, &y1);
			stbtt_GetGlyphHMetrics(&face->info, index, &advance, &bearing);
		}
		job.width = (short)(x1 - x0);
		job.height = (short)(y1 - y0);
		if (job.width <= 0 || job.height <= 0 ||
			job.width + 2 * ATLAS_PADDING > ATLAS_SIZE || job.height + 2 * ATLAS_PADDING > ATLAS_SIZE)
		{
			job.width = job.height = 0;
		}
		else if ((pixels = malloc((size_t)job.width * job.height)) != NULL)
		{
			stbtt_MakeGlyphBitmap(&face->info, pixels, job.width, job.height, job.width,
				job.pixels_x, job.pixels, index);
		}
		else
		{
			job.width = job.height = 0;
		}
		job.left = (short)x0;
		job.top = (short)y0;
		job.advance = advance * face->scale * face->narrowing;
		job.x = job.y = 0;
		pthread_mutex_lock(&rasterizer.lock);
		if (job.generation == rasterizer.pack_generation)
		{
			job.full = !glyph_place(&job, pixels);
			rasterizer.done[(rasterizer.done_first + rasterizer.done_count) % GLYPH_CACHE_SIZE] = job;
			rasterizer.done_count++;
			rasterizer_done = rasterizer.done_count;
		}
		free(pixels);
	}
	return NULL;
}

/* the glyph's slot in the cache: its own, else the free one it would take,
else -1 (the cache is full) */
static long glyph_slot(long font, unsigned long code)
{
	unsigned long slot = ((unsigned long)font * 2654435761UL ^ code * 40503UL) & (GLYPH_CACHE_SIZE - 1);
	long probe;

	for (probe = 0; probe < GLYPH_CACHE_SIZE; probe++, slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1))
	{
		if (glyphs[slot].state == _glyph_free || (glyphs[slot].font == font && glyphs[slot].code == code))
			return (long)slot;
	}
	return -1;
}

/* the glyphs the thread has rasterized taken into the cache (the atlas
started over if it is full) */
static void glyphs_take(void)
{
	int full = 0;

	if (!rasterizer_done)
		return;
	pthread_mutex_lock(&rasterizer.lock);
	while (rasterizer.done_count)
	{
		struct glyph_job *job = &rasterizer.done[rasterizer.done_first];
		long slot;

		rasterizer.done_first = (rasterizer.done_first + 1) % GLYPH_CACHE_SIZE;
		rasterizer.done_count--;
		if (job->generation != atlas_generation)
			continue;
		if (job->full)
		{
			full = 1;
			continue;
		}
		slot = glyph_slot(job->font, job->code);
		if (slot < 0 || glyphs[slot].state != _glyph_wanted)
			continue;
		glyphs[slot].x = job->x;
		glyphs[slot].y = job->y;
		glyphs[slot].width = job->width;
		glyphs[slot].height = job->height;
		glyphs[slot].left = job->left;
		glyphs[slot].top = job->top;
		glyphs[slot].advance = job->advance;
		glyphs[slot].state = _glyph_ready;
		atlas_changed = 1;
	}
	rasterizer_done = 0;
	pthread_mutex_unlock(&rasterizer.lock);
	/* (its glyphs asked for again as they are drawn) */
	if (full)
		atlas_reset(atlas_scale);
}

/* the face of a font that draws a character: the font's own, else its
fallback's; -1 if neither has it */
static int font_glyph_face(long font, unsigned long code)
{
	int face;

	for (face = 0; face < FONT_FACES; face++)
	{
		if (fonts[font].faces[face].scale > 0.0f && stbtt_FindGlyphIndex(&fonts[font].faces[face].info, (int)code))
			return face;
	}
	return -1;
}

/* the atlas, for the display's scale now (emptied if it was another's);
0 if there is none */
static long glyph_want(long font, unsigned long code);

static int atlas_ready(void)
{
	float scale = halo_screen_pixel_scale();

	if (scale <= 0.0f)
		return 0;
	if (!atlas || scale != atlas_scale)
	{
		long font;
		unsigned long code;

		atlas_reset(scale);
		/* (every font's letters, digits and punctuation rasterized again
		at once, for this scale, on the thread: the text is drawn from its
		tags' bitmaps until they are) */
		for (font = 0; atlas && font < font_count; font++)
		{
			for (code = '!'; fonts[font].faces[0].scale > 0.0f && code <= '~'; code++)
				glyph_want(font, code);
		}
	}
	if (atlas)
		glyphs_take();
	return atlas != NULL;
}

/* the glyph asked of the thread (once): its slot, else -1 */
static long glyph_want(long font, unsigned long code)
{
	long slot = glyph_slot(font, code);
	struct glyph_job *job;
	int face;

	if (slot < 0)
	{
		atlas_reset(atlas_scale);
		slot = glyph_slot(font, code);
		if (slot < 0)
			return -1;
	}
	if (glyphs[slot].state != _glyph_free)
		return slot;
	glyphs[slot].font = font;
	glyphs[slot].code = code;
	face = font_glyph_face(font, code);
	if (face < 0)
	{
		glyphs[slot].state = _glyph_missing;
		return slot;
	}
	pthread_mutex_lock(&rasterizer.lock);
	if (!rasterizer.running)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, rasterizer_thread, NULL) != 0)
		{
			pthread_mutex_unlock(&rasterizer.lock);
			return -1;
		}
		pthread_detach(thread);
		rasterizer.running = 1;
	}
	/* (every glyph asked for has a slot of its own: no more than the
	ring holds) */
	job = &rasterizer.wanted[(rasterizer.wanted_first + rasterizer.wanted_count) % GLYPH_CACHE_SIZE];
	if (rasterizer.wanted_count == GLYPH_CACHE_SIZE)
	{
		pthread_mutex_unlock(&rasterizer.lock);
		return -1;
	}
	memset(job, 0, sizeof(*job));
	job->font = font;
	job->face = face;
	job->code = code;
	job->generation = atlas_generation;
	job->pixels = fonts[font].faces[face].scale * atlas_scale * fonts[font].oversample;
	job->pixels_x = job->pixels * fonts[font].faces[face].narrowing;
	rasterizer.wanted_count++;
	pthread_cond_signal(&rasterizer.work);
	pthread_mutex_unlock(&rasterizer.lock);
	glyphs[slot].state = _glyph_wanted;
	return slot;
}

/* a font's letters, digits and punctuation asked of the thread as it is
made, before they are drawn */
static void font_ahead(long font)
{
	unsigned long code;

	if (!atlas_ready())
		return;
	for (code = '!'; code <= '~'; code++)
		glyph_want(font, code);
}

/* a font file made a face that draws a tag: sized so that its capitals are
cap_height units tall, narrowed where its letters are wider than the tag's
(letter_widths); FALSE (and no face) if it cannot be read */
static int face_make(struct font_face *face, const unsigned char *data, float cap_height, short const *letter_widths)
{
	float tag_width = 0.0f;
	float font_width = 0.0f;
	int x0, y0, x1, y1;
	int letter;

	face->scale = 0.0f;
	face->narrowing = 1.0f;
	if (!data || !stbtt_InitFont(&face->info, data, stbtt_GetFontOffsetForIndex(data, 0)) ||
		!stbtt_GetCodepointBox(&face->info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
	{
		return 0;
	}
	face->scale = cap_height / (float)(y1 - y0);
	for (letter = 0; letter < TEXT_HIRES_FIT_LETTERS; letter++)
	{
		int glyph = stbtt_FindGlyphIndex(&face->info, letter < 26 ? 'A' + letter : 'a' + letter - 26);
		int advance = 0, bearing = 0;

		if (letter_widths[letter] <= 0 || !glyph)
			continue;
		stbtt_GetGlyphHMetrics(&face->info, glyph, &advance, &bearing);
		tag_width += letter_widths[letter];
		font_width += advance * face->scale;
	}
	if (tag_width > 0.0f && font_width > tag_width)
		face->narrowing = tag_width / font_width;
	return 1;
}

long text_hires_font(char const *tag_name, float cap_height, short const *letter_widths, float oversample)
{
	unsigned int index;
	long font;
	long letters_width = 0;
	int letter;

	if (!text_hires_enabled() || !tag_name || cap_height <= 0.0f || !letter_widths)
		return -1;
	/* (text is never drawn with fewer pixels than the display's) */
	if (oversample < 1.0f)
		oversample = 1.0f;
	for (letter = 0; letter < TEXT_HIRES_FIT_LETTERS; letter++)
		letters_width += letter_widths[letter] > 0 ? letter_widths[letter] : 0;
	for (font = 0; font < font_count; font++)
	{
		if (!names_differ(fonts[font].tag, tag_name) && fonts[font].cap_height == cap_height &&
			fonts[font].letters_width == letters_width && fonts[font].oversample == oversample)
		{
			return fonts[font].faces[0].scale > 0.0f ? font : -1;
		}
	}
	if (font_count >= MAXIMUM_FONTS)
		return -1;
	font = font_count++;
	snprintf(fonts[font].tag, sizeof(fonts[font].tag), "%s", tag_name);
	fonts[font].cap_height = cap_height;
	fonts[font].letters_width = letters_width;
	memset(fonts[font].faces, 0, sizeof(fonts[font].faces));
	fonts[font].oversample = oversample;
	for (index = 0; index < text_hires_embedded_count; index++)
	{
		const struct text_hires_embedded *embedded = &text_hires_embedded[index];

		if (!embedded->tag || names_differ(embedded->tag, tag_name))
			continue;
		if (!face_make(&fonts[font].faces[0],
			(const unsigned char *)embedded_assets_data(embedded->data_offset, embedded->size), cap_height,
			letter_widths))
		{
			platform_log("high-res text: could not read %s for %s", embedded->file, tag_name);
			break;
		}
		/* (its fallback: a font that cannot be read draws nothing, the
		string with a character the font lacks drawn from the tag's bitmaps,
		as without one) */
		if (embedded->fallback_file && !face_make(&fonts[font].faces[1],
			(const unsigned char *)embedded_assets_data(embedded->fallback_data_offset, embedded->fallback_size),
			cap_height, letter_widths))
		{
			platform_log("high-res text: could not read %s for %s", embedded->fallback_file, tag_name);
		}
		platform_log("high-res text: %s (capitals %.0f units) drawn with %s, %.0f%% wide%s%s",
			tag_name, cap_height, embedded->file, fonts[font].faces[0].narrowing * 100.0f,
			fonts[font].faces[1].scale > 0.0f ? ", what it lacks with " : "",
			fonts[font].faces[1].scale > 0.0f ? embedded->fallback_file : "");
		break;
	}
	if (fonts[font].faces[0].scale <= 0.0f)
		return -1;
	font_ahead(font);
	return font;
}

int text_hires_covers(long font, unsigned long code)
{
	long slot;

	if (font < 0 || font >= font_count || fonts[font].faces[0].scale <= 0.0f)
		return 0;
	/* (control characters are never drawn) */
	if (code < 32 || code == ' ')
		return 1;
	if (!placeholder_data || !atlas_ready())
		return 0;
	/* (one not yet rasterized: asked for, the string drawn from the tag's
	bitmaps meanwhile) */
	slot = glyph_want(font, code);
	return slot >= 0 && glyphs[slot].state == _glyph_ready;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	float pixel_scale;
	long slot;

	if (font < 0 || font >= font_count || fonts[font].faces[0].scale <= 0.0f || !placeholder_data || !atlas)
		return 0;
	slot = glyph_slot(font, code);
	if (slot < 0 || glyphs[slot].state != _glyph_ready || !glyphs[slot].width || !glyphs[slot].height)
		return 0;
	pixel_scale = atlas_scale * fonts[font].oversample;
	glyph->left = glyphs[slot].left / pixel_scale;
	glyph->top = glyphs[slot].top / pixel_scale;
	glyph->right = (glyphs[slot].left + glyphs[slot].width) / pixel_scale;
	glyph->bottom = (glyphs[slot].top + glyphs[slot].height) / pixel_scale;
	glyph->advance = glyphs[slot].advance;
	glyph->u0 = (float)glyphs[slot].x * placeholder_width / ATLAS_SIZE;
	glyph->v0 = (float)glyphs[slot].y * placeholder_height / ATLAS_SIZE;
	glyph->u1 = (float)(glyphs[slot].x + glyphs[slot].width) * placeholder_width / ATLAS_SIZE;
	glyph->v1 = (float)(glyphs[slot].y + glyphs[slot].height) * placeholder_height / ATLAS_SIZE;
	return 1;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	placeholder_data = texture ? texture[1] : 0;
	placeholder_width = width;
	placeholder_height = height;
}

unsigned int text_hires_atlas_texture(unsigned long data)
{
	if (!data || data != placeholder_data || !atlas)
		return 0;
	/* (the host reads the atlas where it is, again once it has changed) */
	if (!atlas_texture)
		atlas_texture = xgpu_image_create(atlas, ATLAS_SIZE, ATLAS_SIZE, 1, XGPU_IMAGE_COVERAGE, FALSE);
	if (atlas_changed)
	{
		xgpu_image_changed(atlas_texture);
		atlas_changed = 0;
	}
	return atlas_texture;
}

unsigned char *text_hires_line(char const *tag_name, char const *text, float cap_pixels,
	int *width, int *height)
{
	stbtt_fontinfo info;
	const unsigned char *data = NULL;
	unsigned char *pixels;
	unsigned int index;
	int x0, y0, x1, y1, pass, w = 0, h, pen, baseline;
	float scale;

	*width = *height = 0;
	if (!tag_name || !text || cap_pixels <= 0.0f || cap_pixels > 512.0f)
		return NULL;
	for (index = 0; index < text_hires_embedded_count && !data; index++)
		if (text_hires_embedded[index].tag && !names_differ(text_hires_embedded[index].tag, tag_name))
			data = (const unsigned char *)embedded_assets_data(text_hires_embedded[index].data_offset,
				text_hires_embedded[index].size);
	if (!data || !stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) ||
		!stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
		return NULL;
	scale = cap_pixels / (float)(y1 - y0);
	/* (room above the capitals for accents, below the baseline for
	descenders: the font's own ascent and descent are not to be trusted
	for it, OpenCE's being smaller than its capitals) */
	baseline = (int)ceilf(cap_pixels * 1.3f);
	h = baseline + (int)ceilf(cap_pixels * 0.4f) + 1;
	/* (twice: the width, then the glyphs) */
	pixels = NULL;
	for (pass = 0; pass < 2; pass++)
	{
		const unsigned char *p = (const unsigned char *)text;
		float x = 1.0f;
		int previous = 0;

		while (*p)
		{
			int code = *p++, advance, bearing;

			/* (UTF-8's two- and three-byte letters; others as Latin-1) */
			if ((code & 0xe0) == 0xc0 && (p[0] & 0xc0) == 0x80)
				code = ((code & 0x1f) << 6) | (*p++ & 0x3f);
			else if ((code & 0xf0) == 0xe0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80)
			{
				code = ((code & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
				p += 2;
			}
			if (previous)
				x += (float)stbtt_GetCodepointKernAdvance(&info, previous, code) * scale;
			stbtt_GetCodepointHMetrics(&info, code, &advance, &bearing);
			if (pass)
			{
				int left, top, right, bottom;
				float shift = x - floorf(x);

				unsigned char *glyph;

				stbtt_GetCodepointBitmapBoxSubpixel(&info, code, scale, scale, shift, 0.0f, &left, &top, &right, &bottom);
				pen = (int)floorf(x) + left;
				glyph = right > left && bottom > top ? calloc((size_t)(right - left) * (bottom - top), 1) : NULL;
				if (glyph)
				{
					int gx, gy;

					stbtt_MakeCodepointBitmapSubpixel(&info, glyph, right - left, bottom - top, right - left,
						scale, scale, shift, 0.0f, code);
					/* (each pixel the most of the glyphs over it, the line's
					edges clipping them) */
					for (gy = 0; gy < bottom - top; gy++)
						for (gx = 0; gx < right - left; gx++)
						{
							int px = pen + gx, py = baseline + top + gy;
							unsigned char value = glyph[gy * (right - left) + gx];

							if (px >= 0 && px < w && py >= 0 && py < h && value > pixels[py * w + px])
								pixels[py * w + px] = value;
						}
					free(glyph);
				}
			}
			x += (float)advance * scale;
			previous = code;
		}
		if (!pass)
		{
			w = (int)ceilf(x) + 2;
			if (w <= 2 || w > 8192)
				return NULL;
			pixels = calloc((size_t)w * (size_t)h, 1);
			if (!pixels)
				return NULL;
		}
	}
	*width = w;
	*height = h;
	return pixels;
}
