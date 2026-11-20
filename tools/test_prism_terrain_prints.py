"""The terrain overhaul's prints (T4: port/linux/vulkan/xgpu_vulkan_terrain_prints.c; docs/prism_terrain.md, "T4:
the prints"): the module compiled on its own, with stand-ins for the renderer's images (each upload kept), and
driven by footstep events as PRISM_TERRAIN brings them. Checked: each species' stamp is a dent with a broad rim about
it; a dry print in sand is as deep as the design says and points the way the foot faced; an event sent
twice makes the same print; the left foot is the right's mirror; relax brings the walls to the ground's angle of
repose (a wet print crisper); two minutes of b30's wind fill a dry print in by about a third, dirt and mud keep theirs,
nothing moves while the game is paused; the ground's height kept with the print (so a bridge over it shows none);
a new generation forgets every page; a full pool lets the farthest page go; Sentinels, stone and slopes past 40
degrees take none; the half floats the pool is sent as; the page table's window. No GPU and no game data."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
VULKAN = ROOT / "port/linux/vulkan"
CC = shutil.which("cc") or shutil.which("clang")

STUBS = {
    "host.h": """
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#include <stdio.h>
#include <stdarg.h>
static int host_log_lines;
static void host_logf(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
	host_log_lines++;
}
""",
    "xgpu_vulkan.h": """
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { XGPU_VULKAN_IMAGE_2D = 1, XGPU_VULKAN_IMAGE_2D_ARRAY = 2 };
enum { XGPU_VULKAN_FORMAT_RGBA16F = 1, XGPU_VULKAN_FORMAT_RGBA32_UINT = 2 };
enum { XGPU_VULKAN_USAGE_SAMPLED = 1 };
struct xgpu_vulkan_image_description
{
	uint8_t type, format, usage, samples;
	uint32_t width, height, depth_or_layers, levels, channels;
};
struct xgpu_vulkan_image
{
	struct xgpu_vulkan_image_description d;
	uint32_t uploads;
	/* (the first level of every layer, as sent) */
	uint16_t *halves;
	uint32_t *words;
};
static struct xgpu_vulkan_image *xgpu_vulkan_image_create(const struct xgpu_vulkan_image_description *d)
{
	struct xgpu_vulkan_image *image = calloc(1, sizeof(*image));
	image->d = *d;
	if (d->format == XGPU_VULKAN_FORMAT_RGBA16F)
		image->halves = calloc((size_t)d->width * d->height * d->depth_or_layers * 4, 2);
	else
		image->words = calloc((size_t)d->width * d->height * 4, 4);
	return image;
}
static void xgpu_vulkan_image_upload(struct xgpu_vulkan_image *image, uint32_t level, uint32_t layer, uint32_t x,
	uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth, const void *data, uint32_t bytes_per_row,
	uint32_t bytes_per_image)
{
	uint32_t row;
	(void)z; (void)depth; (void)bytes_per_image;
	image->uploads++;
	if (level != 0)
		return;
	for (row = 0; row < height; row++)
	{
		if (image->halves)
			memcpy(image->halves + (((size_t)layer * image->d.height + y + row) * image->d.width + x) * 4,
				(const char *)data + row * bytes_per_row, width * 8);
		else
			memcpy(image->words + ((size_t)(y + row) * image->d.width + x) * 4,
				(const char *)data + row * bytes_per_row, width * 16);
	}
}
""",
    "xgpu_vulkan_terrain.h": "",
}

CHECK = r"""
#include "xgpu_vulkan_terrain_prints.c"
#include <assert.h>
#include <stdio.h>

static float half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exponent = (h >> 10) & 31u, mantissa = h & 1023u, bits;
	float value;
	if (!exponent)
		value = (float)mantissa / 1024.0f / 16384.0f;
	else if (exponent == 31u)
		value = mantissa ? NAN : INFINITY;
	else
		value = ldexpf(1.0f + (float)mantissa / 1024.0f, (int)exponent - 15);
	memcpy(&bits, &value, 4);
	bits |= sign;
	memcpy(&value, &bits, 4);
	return value;
}

static uint32_t octahedral(float x, float y, float z)
{
	float length = fabsf(x) + fabsf(y) + fabsf(z), fx, fy;
	x /= length; y /= length;
	if (z < 0.0f)
	{
		fx = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f);
		fy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
		x = fx; y = fy;
	}
	return ((uint32_t)(int32_t)lrintf(x * 32767.0f) & 0xffffu) | (((uint32_t)(int32_t)lrintf(y * 32767.0f) & 0xffffu) << 16);
}

static struct xgpu_stream_terrain_event foot(float x, float y, float z, float fx, float fy, uint32_t kind, int right,
	uint32_t material, uint32_t species)
{
	struct xgpu_stream_terrain_event e;
	memset(&e, 0, sizeof(e));
	e.kind = kind | (right ? XGPU_TERRAIN_EVENT_RIGHT : 0u) | material << 16 | species << 24;
	e.position[0] = x; e.position[1] = y; e.position[2] = z;
	e.normal = octahedral(0.0f, 0.0f, 1.0f);
	e.facing = octahedral(fx, fy, 0.0f);
	e.magnitude = 1.0f;
	return e;
}

/* the field (displacement) at a world point: the page's texel there, 0 none */
static float field_at(float x, float y)
{
	int32_t px = (int32_t)floorf(x / PAGE_WU), py = (int32_t)floorf(y / PAGE_WU);
	struct print_page *page = page_find(px, py);
	int tx, ty;
	if (!page)
		return 0.0f;
	tx = (int)floorf(x / TEXEL_WU - (float)px * PRINT_PAGE_INNER + 1.0f);
	ty = (int)floorf(y / TEXEL_WU - (float)py * PRINT_PAGE_INNER + 1.0f);
	return page->h[ty * (int)PRINT_PAGE_TEXELS + tx];
}

/* the print's least and most about a point, within r world units, and the dent's and rim's volumes */
static void survey(float x, float y, float r, float *least, float *most, double *dent, double *rim)
{
	float u, v;
	*least = 0.0f; *most = 0.0f; *dent = 0.0; *rim = 0.0;
	for (v = -r; v <= r; v += TEXEL_WU)
		for (u = -r; u <= r; u += TEXEL_WU)
		{
			float h = field_at(x + u, y + v);
			*least = h < *least ? h : *least;
			*most = h > *most ? h : *most;
			if (h < 0.0f) *dent -= h; else *rim += h;
		}
}

/* the print's length along (dx, dy) through its centre: where it is deeper than a third of its depth */
static float extent(float x, float y, float dx, float dy, float depth)
{
	float t, first = 1.0e9f, last = -1.0e9f;
	for (t = -0.2f; t <= 0.2f; t += TEXEL_WU * 0.5f)
		if (field_at(x + dx * t, y + dy * t) < -depth / 3.0f)
		{
			first = t < first ? t : first;
			last = t > last ? t : last;
		}
	return last > first ? last - first : 0.0f;
}

static void frames(float seconds, float wind)
{
	float camera[3] = { 50.0f, 50.0f, 2.0f }, w[3] = { wind, 0.0f, 0.0f };
	float sun[4] = { 0.5f, 0.6f, 0.62f, 1.0f };
	struct terrain_prints_draw draw;
	int n = (int)(seconds * 30.0f + 0.5f), i;
	/* (each frame drawn too: the pages sent, as the renderer does) */
	for (i = 0; i < n; i++)
	{
		terrain_prints_frame(1.0f / 30.0f, camera, w);
		(void)terrain_prints_prepare(camera, sun, &draw);
	}
}

/* the steepest step between neighbours in a page about a point (world units over world units) */
static float steepest(float x, float y, float r)
{
	float u, v, most = 0.0f;
	for (v = -r; v <= r; v += TEXEL_WU)
		for (u = -r; u <= r; u += TEXEL_WU)
		{
			float a = field_at(x + u, y + v), b = field_at(x + u + TEXEL_WU, y + v), c = field_at(x + u, y + v + TEXEL_WU);
			float s = fmaxf(fabsf(a - b), fabsf(a - c)) / TEXEL_WU;
			most = s > most ? s : most;
		}
	return most;
}

int main(void)
{
	struct xgpu_stream_terrain_event e;
	float least, most, least2, most2;
	double dent, rim;
	int species;
	const float metres = 1.0f / 3.048f;

	setvbuf(stdout, NULL, _IONBF, 0);
	/* stamps: each a dent and a rim of its volume */
	stamps_make();
	for (species = 0; species < SPECIES; species++)
	{
		const struct stamp *s = &prints.stamps[species];
		double d = 0.0, r = 0.0, inside = 0.0;
		float top = 0.0f;
		int k;
		for (k = 0; k < STAMP_SIDE * STAMP_SIDE; k++)
		{
			d += s->dent[k];
			r += s->rim[k];
			inside += s->rim[k] * s->dent[k];
			top = s->rim[k] > top ? s->rim[k] : top;
			assert(s->dent[k] >= 0.0f && s->dent[k] <= 1.0f && s->rim[k] >= 0.0f);
		}
		if (species == XGPU_TERRAIN_SPECIES_SENTINEL)
			continue;
		printf("species %d: dent %.0f rim %.0f inside %.1f top %.3f\n", species, d, r, inside, top);
		/* (a dent; a rim about it, its highest 1, little of it inside the dent) */
		assert(d > 20.0);
		assert(fabsf(top - 1.0f) < 1.0e-5f && r > 0.15 * d && inside < 0.1 * r);
	}
	printf("stamps ok\n");

	/* a dry walk in sand (the Chief's: human), facing +y: about 3.5 cm deep, a rim, the volume pushed aside */
	terrain_prints_forget();
	prints.clock = 0.0f;
	e = foot(10.3f, 20.1f, 1.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	survey(10.3f, 20.1f, 0.15f, &least, &most, &dent, &rim);
	printf("walk: least %.1f mm most %.1f mm, rim/dent %.2f\n", least * 3048.0f, most * 3048.0f, rim / dent);
	assert(least < -0.030f * metres && least > -0.050f * metres);
	assert(most > 0.003f * metres && most < 0.015f * metres);
	assert(rim / dent > 0.08 && rim / dent < 0.6);
	/* (it points the way the foot faced: longer along y than across) */
	{
		float along = extent(10.3f, 20.1f, 0.0f, 1.0f, -least), across = extent(10.3f, 20.1f, 1.0f, 0.0f, -least);
		printf("walk: %.0f mm along, %.0f mm across\n", along * 3048.0f, across * 3048.0f);
		assert(along > 0.22f * metres && along < 0.40f * metres);
		assert(across > 0.04f * metres && across < 0.16f * metres);
	}
	/* (the same event again: the same print) */
	terrain_prints_event(&e);
	survey(10.3f, 20.1f, 0.15f, &least2, &most2, &dent, &rim);
	assert(least2 == least && most2 == most);
	/* (the ground's height kept: the texel's ground is the foot's) */
	{
		struct print_page *page = page_find((int32_t)floorf(10.3f / PAGE_WU), (int32_t)floorf(20.1f / PAGE_WU));
		float texel[4];
		int tx = (int)floorf(10.3f / TEXEL_WU - (float)page->x * PRINT_PAGE_INNER + 1.0f);
		int ty = (int)floorf(20.1f / TEXEL_WU - (float)page->y * PRINT_PAGE_INNER + 1.0f);
		texel_of(page, ty * (int)PRINT_PAGE_TEXELS + tx, texel);
		assert(texel[3] == 1.0f && fabsf(page->z + texel[2] / texel[3] - 1.0f) < 0.002f);
	}

	/* relax: dry sand's walls to its angle of repose; then nothing more moves */
	frames(2.0f, 0.0f);
	{
		float s = steepest(10.3f, 20.1f, 0.15f);
		printf("relaxed: steepest %.2f (repose %.2f)\n", s, repose_step(GROUND_SAND, 0) / TEXEL_WU);
		assert(s < repose_step(GROUND_SAND, 0) / TEXEL_WU * 1.3f);
	}
	survey(10.3f, 20.1f, 0.15f, &least, &most, &dent, &rim);
	assert(least < -0.02f * metres);

	/* the left foot: the right's mirror across the foot's axis (each on a texel's centre, apart in two
	generations, so that both meet the texels alike) */
	{
		float x0 = (4572.0f + 0.5f) * TEXEL_WU, y0 = (4572.0f + 0.5f) * TEXEL_WU, u, v, worst = 0.0f;
		static float right[41][41];
		int i, j;
		terrain_prints_forget();
		e = foot(x0, y0, 0.0f, 1.0f, 0.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
		terrain_prints_event(&e);
		for (j = 0; j < 41; j++)
			for (i = 0; i < 41; i++)
				right[j][i] = field_at(x0 + (float)(i - 20) * 2.0f * TEXEL_WU, y0 + (float)(j - 20) * TEXEL_WU);
		terrain_prints_forget();
		e = foot(x0, y0, 0.0f, 1.0f, 0.0f, XGPU_TERRAIN_EVENT_WALK, 0, 1, XGPU_TERRAIN_SPECIES_HUMAN);
		terrain_prints_event(&e);
		for (j = 0; j < 41; j++)
			for (i = 0; i < 41; i++)
				worst = fmaxf(worst, fabsf(right[j][i] -
					field_at(x0 + (float)(i - 20) * 2.0f * TEXEL_WU, y0 - (float)(j - 20) * TEXEL_WU)));
		(void)u; (void)v;
		printf("mirror: worst %.2f mm\n", worst * 3048.0f);
		assert(worst < 0.002f * metres);
	}

	/* infill: two minutes of b30's wind (0.8) take about a third of a dry print's depth; dirt keeps its own; a pause
	none */
	terrain_prints_forget();
	e = foot(50.0f, 50.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	e = foot(52.0f, 50.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 0, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	frames(2.0f, 0.8f);
	{
		float sand0, dirt0, sand1, dirt1, x;
		survey(50.0f, 50.0f, 0.15f, &sand0, &most, &dent, &rim);
		survey(52.0f, 50.0f, 0.15f, &dirt0, &most, &dent, &rim);
		{
			float camera[3] = { 0.0f, 0.0f, 0.0f }, w[3] = { 0.8f, 0.0f, 0.0f };
			int i;
			for (i = 0; i < 300; i++)
				terrain_prints_frame(0.0f, camera, w);
		}
		survey(50.0f, 50.0f, 0.15f, &x, &most, &dent, &rim);
		assert(x == sand0);
		frames(120.0f, 0.8f);
		survey(50.0f, 50.0f, 0.15f, &sand1, &most, &dent, &rim);
		survey(52.0f, 50.0f, 0.15f, &dirt1, &most, &dent, &rim);
		printf("two minutes: sand %.1f -> %.1f mm, dirt %.1f -> %.1f mm\n", sand0 * 3048.0f, sand1 * 3048.0f,
			dirt0 * 3048.0f, dirt1 * 3048.0f);
		assert(sand1 / sand0 > 0.5f && sand1 / sand0 < 0.8f);
		assert(dirt1 / dirt0 > 0.9f);
	}

	/* wet sand: shallower and steeper */
	terrain_prints_forget();
	{
		struct xgpu_stream_terrain_wader w;
		memset(&w, 0, sizeof(w));
		w.kind = XGPU_TERRAIN_WADER_BIPED | XGPU_TERRAIN_WADER_PARTIALLY;
		w.position[0] = 70.0f; w.position[1] = 70.0f; w.radius = 0.3f;
		terrain_prints_waders(&w, 1);
	}
	e = foot(70.0f, 70.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	e = foot(80.0f, 70.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	frames(2.0f, 0.0f);
	{
		float wet, dry, swet = steepest(70.0f, 70.0f, 0.15f), sdry = steepest(80.0f, 70.0f, 0.15f);
		survey(70.0f, 70.0f, 0.15f, &wet, &most, &dent, &rim);
		survey(80.0f, 70.0f, 0.15f, &dry, &most, &dent, &rim);
		printf("wet %.1f mm (steepest %.2f), dry %.1f mm (steepest %.2f)\n", wet * 3048.0f, swet, dry * 3048.0f, sdry);
		assert(wet > dry && wet < -0.008f * metres);
		/* (its walls the steeper for its depth: crisp) */
		assert(swet / -wet > sdry / -dry);
	}

	/* none: a Sentinel, stone, a steep slope */
	terrain_prints_forget();
	e = foot(90.0f, 90.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_SENTINEL);
	terrain_prints_event(&e);
	e = foot(92.0f, 90.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 2, XGPU_TERRAIN_SPECIES_HUMAN);
	terrain_prints_event(&e);
	e = foot(94.0f, 90.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
	e.normal = octahedral(0.7f, 0.0f, 0.7f);
	terrain_prints_event(&e);
	{
		uint32_t pages;
		terrain_prints_counts(&pages, NULL);
		assert(pages == 0);
	}

	/* every species prints, the Hunter's the deepest; a landing deeper than a walk */
	{
		float depth[SPECIES];
		for (species = 0; species < SPECIES; species++)
		{
			if (species == XGPU_TERRAIN_SPECIES_SENTINEL)
				continue;
			terrain_prints_forget();
			e = foot(100.0f, 100.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, (uint32_t)species);
			terrain_prints_event(&e);
			survey(100.0f, 100.0f, 0.4f, &depth[species], &most, &dent, &rim);
			assert(depth[species] < 0.0f);
		}
		assert(depth[XGPU_TERRAIN_SPECIES_HUNTER] < depth[XGPU_TERRAIN_SPECIES_HUMAN]);
		assert(depth[XGPU_TERRAIN_SPECIES_JACKAL] > depth[XGPU_TERRAIN_SPECIES_HUMAN]);
		terrain_prints_forget();
		e = foot(100.0f, 100.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_LAND, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
		terrain_prints_event(&e);
		survey(100.0f, 100.0f, 0.4f, &least, &most, &dent, &rim);
		assert(least < depth[XGPU_TERRAIN_SPECIES_HUMAN]);
	}

	/* the upload: the pool's halves, the window's table */
	{
		float sun[4] = { 0.5f, 0.6f, 0.6f, 1.0f }, eye[3] = { 100.0f, 100.0f, 2.0f };
		struct terrain_prints_draw draw;
		struct print_page *page = page_find((int32_t)floorf(100.0f / PAGE_WU), (int32_t)floorf(100.0f / PAGE_WU));
		uint32_t layer = (uint32_t)(page - prints.pages), at;
		int tx = (int)floorf(100.0f / TEXEL_WU - (float)page->x * PRINT_PAGE_INNER + 1.0f);
		int ty = (int)floorf(100.0f / TEXEL_WU - (float)page->y * PRINT_PAGE_INNER + 1.0f);
		const uint32_t *entry;
		assert(terrain_prints_prepare(eye, sun, &draw) == 1);
		at = ((layer * PRINT_PAGE_TEXELS + (uint32_t)ty) * PRINT_PAGE_TEXELS + (uint32_t)tx) * 4;
		assert(fabsf(half_to_float(draw.pool->halves[at]) - page->h[ty * (int)PRINT_PAGE_TEXELS + tx]) < 2.0e-5f);
		entry = &draw.table->words[(((uint32_t)page->y & (PRINT_WINDOW - 1)) * PRINT_WINDOW +
			((uint32_t)page->x & (PRINT_WINDOW - 1))) * 4];
		assert(entry[0] == layer + 1 && (int32_t)entry[1] == page->x && (int32_t)entry[2] == page->y);
		assert(draw.values[1][3] == 1.0f && fabsf(draw.values[0][1] - TEXEL_WU) < 1.0e-9f);
		/* (far from every page: nothing to draw) */
		eye[0] = 1000.0f;
		assert(terrain_prints_prepare(eye, sun, &draw) == 0);
	}
	/* (the halves: a few values both ways) */
	{
		const float values[] = { 0.0f, -0.0123f, 0.0049f, 1.0f, -3.5f, 1.0e-6f, 70000.0f };
		int k;
		for (k = 0; k < 7; k++)
		{
			float back = half_to_float(half_of(values[k]));
			assert(fabsf(back - (values[k] > 65504.0f ? 65504.0f : values[k])) <= fabsf(values[k]) * 1.0e-3f + 1.0e-7f);
		}
	}

	/* a full pool lets go of the page farthest from the camera, not one being pressed */
	terrain_prints_forget();
	{
		uint32_t index, pages;
		float camera[3] = { 0.0f, 0.0f, 0.0f }, w[3] = { 0.0f, 0.0f, 0.0f };
		for (index = 0; index < POOL_PAGES; index++)
		{
			e = foot(5.0f + (float)index * 2.0f, 5.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1,
				XGPU_TERRAIN_SPECIES_HUMAN);
			terrain_prints_event(&e);
		}
		terrain_prints_counts(&pages, NULL);
		assert(pages == POOL_PAGES);
		terrain_prints_frame(0.0f, camera, w);
		prints.clock += 10.0f;
		e = foot(-40.0f, -40.0f, 0.0f, 0.0f, 1.0f, XGPU_TERRAIN_EVENT_WALK, 1, 1, XGPU_TERRAIN_SPECIES_HUMAN);
		terrain_prints_event(&e);
		assert(field_at(-40.0f, -40.0f) < 0.0f);
		assert(field_at(5.0f, 5.0f) < 0.0f);
	}
	/* a new generation: nothing left */
	terrain_prints_forget();
	assert(field_at(5.0f, 5.0f) == 0.0f);
	printf("ok\n");
	return 0;
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    work = tmp_path_factory.mktemp("prints")
    (work / "vk").mkdir()
    (work / "src").mkdir()
    for name in ("xgpu_vulkan_terrain_prints.c", "xgpu_vulkan_terrain_prints.h"):
        shutil.copy(VULKAN / name, work / "vk" / name)
    shutil.copy(ROOT / "port/linux/src/xgpu_stream.h", work / "src" / "xgpu_stream.h")
    for name, text in STUBS.items():
        (work / "vk" / name).write_text(text)
    (work / "vk" / "check.c").write_text(CHECK)
    executable = work / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", "-Wno-unused-function", "-o", str(executable),
                    str(work / "vk" / "check.c"), "-lm"], check=True)
    return executable


def test_prints(built):
    result = subprocess.run([str(built)], capture_output=True, text=True)
    print(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip().endswith("ok")
