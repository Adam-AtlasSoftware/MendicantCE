/*
XGPU_VULKAN_TERRAIN_PRINTS.C

The terrain overhaul's prints, T4 (docs/prism_terrain.md, "T4: prints";
overhaul_research/prism_v4_renderer/terrain/design.md sections 2.3, 2.4 and
4.1): the ground pressed by feet, kept for the whole level in world space.

- The pages. A page is PRINT_PAGE_TEXELS squared texels of 2 cm (2.52 m of
  its own, PRINT_PAGE_INNER texels, and a texel of its neighbours' about it
  so that the pass's linear lookups meet across a page's edge), made where a
  foot first presses and kept until the generation changes (a new map or
  BSP, a revert: the beach clean again, as the game's state says) or the
  pool is full (the page farthest from the camera that is not being pressed
  is let go). Each texel: the displacement (world units, negative down, a
  rim positive), the compaction, the ground's height (over the page's own,
  so that a print belongs to its own ground: a bridge over it or a floor
  under it shows none) and the coverage.
- The stamps: our own, procedural (stamp_make), 64 squared, one for each
  species (the Chief and the Marines' boots with their lugs, the Elites' two
  splayed toes and heel spur, the Grunts' broad three-toed pads, the
  Jackals' bird-like three toes, the Hunters' great flat feet, the Flood's
  ragged soles), each a dent and the broad, low rim its pushed-aside sand
  makes (its height a share of the depth by the ground: dry sand a quarter,
  snow least, which packs). Scaled by the walk (a run digs its toe, a landing deepens and
  scuffs, a slide stretches), by the ground (dry sand 3.5 cm, wet sand 1.5
  cm and crisp, snow 10 cm and packed, dirt 0.8 cm, mud 3 cm: design 4.1),
  by the species' mass. A stamp's value is the lower of what is there and
  its dent, the higher where it raises a rim, so an event sent twice
  (sub-steps, co-op's prediction) makes the same print.
- Relax: a stamped region slumps toward the ground's angle of repose
  (thermal erosion, a few passes a frame for a second and a half: sand 42
  degrees (repose_step says why not loose sand's 34), wet sand 65, snow 55,
  dirt 45, mud 50), so dry prints round off and wet ones stay sharp.
- Infill: each page, about every two seconds of the game's time while it is
  near the camera, sinks toward flat and softens, by the ground and the
  wind (dry sand in b30's 0.8 world units a second of wind keeps about half
  its depth after two minutes; dirt and mud keep theirs; snow fills only
  while it snows, T12); the compaction outlives the displacement.
- Upload: a page's changed rectangle of its first level and its two
  smaller levels, as half floats; the page table (the window about the
  camera, toroidal: a page's x and y modulo PRINT_WINDOW) made again when
  the camera's page or the pages change.

Nothing here is read by the game. Its clock is FRAME's game time (0 while
paused: nothing moves), so a capture under a fixed step repeats.
*/

#include "xgpu_vulkan_terrain_prints.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_terrain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WORLD_UNIT_METRES 3.048f
#define TEXEL_WU (PRINT_TEXEL_METRES / WORLD_UNIT_METRES)
#define PAGE_WU (TEXEL_WU * (float)PRINT_PAGE_INNER)
#define PAGE_AREA (PRINT_PAGE_TEXELS * PRINT_PAGE_TEXELS)
/* (the pool: 384 pages of 128 KB at their first level, 64 MB with the
levels, 54 MB of the host's at most; b30's opening battle on the first beach
presses about 250 in a minute) */
#define POOL_PAGES 384u
#define STAMP_SIDE 64
#define SPECIES 8
/* (a stamped page relaxes for so many seconds; a page is filled in at most
every INFILL_SECONDS, and only within INFILL_RANGE of the camera) */
#define RELAX_SECONDS 1.5f
#define RELAX_PASSES 3
#define INFILL_SECONDS 2.0f
#define INFILL_RANGE 70.0f
/* (a foot is wet within this of a wader in water or out of it for less
than 10 s: its print is wet sand's) */
#define WET_REACH 1.5f
/* (the parallax's reach and the deepest a print goes: the pass's march) */
#define PARALLAX_RANGE 25.0f
#define DEPTH_MOST 0.06f
#define RIM_MOST 0.012f

enum
{
	GROUND_NONE,
	GROUND_SAND,
	GROUND_DIRT,
	GROUND_MUD,
	GROUND_SNOW,
};

struct print_page
{
	int32_t x, y;
	float z;
	int used;
	uint8_t ground;
	/* (game seconds: the last stamp, relaxing until, the last infill) */
	float stamped, relax_until, infilled;
	/* (the changed rectangle of the first level, x0 y0 x1 y1 inclusive, until
	it is sent; any change; the rectangle pressed while it relaxes) */
	int32_t dirty[4], pressed[4];
	int changed, whole;
	/* (the displacement in world units; the compaction and the coverage in
	255ths; the ground's height over the page's in 1024ths of a world unit;
	wet) */
	float *h;
	uint8_t *c, *cover, *wet;
	int16_t *g;
};

struct stamp
{
	/* the dent (0 to 1) and the rim about it (its highest 1) */
	float dent[STAMP_SIDE * STAMP_SIDE];
	float rim[STAMP_SIDE * STAMP_SIDE];
	/* the foot's length (metres), its box's side over it, the depth's scale
	(the species' mass) */
	float length, box, depth;
};

static struct
{
	struct print_page pages[POOL_PAGES];
	struct stamp stamps[SPECIES];
	int stamps_made;
	float clock, counted;
	float camera[3], wind;
	struct xgpu_stream_terrain_wader waders[XGPU_TERRAIN_WADERS_MOST];
	uint32_t wader_count;
	/* the images; the window's centre page, valid; the pool's pages since
	the image was made (a page's layer is its index) */
	struct xgpu_vulkan_image *pool, *table;
	int32_t window_x, window_y;
	int window_valid;
	uint32_t infill_next, stamp_count, logged;
	uint16_t scratch[PAGE_AREA * 4];
	float level[2][(PRINT_PAGE_TEXELS / 2) * (PRINT_PAGE_TEXELS / 2) * 4];
	uint32_t table_texels[PRINT_WINDOW * PRINT_WINDOW * 4];
} prints;

/* ---------- stamps (ours: procedural) */

static float clampf(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

static float smooth(float edge0, float edge1, float x)
{
	float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);

	return t * t * (3.0f - 2.0f * t);
}

/* an ellipse's inside, soft at its edge: (u, v) in the foot's metres, its
centre, half axes, the turn (radians) */
static float ellipse(float u, float v, float cu, float cv, float au, float av, float turn)
{
	float c = cosf(turn), s = sinf(turn);
	float du = u - cu, dv = v - cv;
	float x = (du * c + dv * s) / au, y = (-du * s + dv * c) / av;
	float r = sqrtf(x * x + y * y);

	return 1.0f - smooth(0.82f, 1.08f, r);
}

/* a hash of the stamp's texel, 0 to 1 (the Flood's ragged edge) */
static float stamp_noise(int x, int y, int seed)
{
	uint32_t n = (uint32_t)x * 1597334673u ^ (uint32_t)y * 3812015801u ^ (uint32_t)seed * 2654435761u;

	n ^= n >> 15;
	n *= 2246822519u;
	n ^= n >> 13;
	return (float)(n & 0xffffu) / 65535.0f;
}

/* a species' sole at (u along the foot, v across, metres; the right foot's,
the left mirrored when stamped): 0 to 1 */
static float sole(int species, float u, float v, float length)
{
	float a, b, toe, lug;

	switch (species)
	{
	case XGPU_TERRAIN_SPECIES_HUMAN:
		/* (a boot: heel and ball joined by the shank, its outer edge
		straighter; the lugs chevrons across it, 4.5 cm apart) */
		a = ellipse(u, v, -0.30f * length, 0.0f, 0.20f * length, 0.050f, 0.0f);
		b = ellipse(u, v, 0.17f * length, 0.006f, 0.33f * length, 0.062f, 0.06f);
		toe = ellipse(u, v, -0.06f * length, 0.012f, 0.22f * length, 0.044f, 0.0f);
		a = fmaxf(a, fmaxf(b, toe));
		lug = 0.5f + 0.5f * cosf((u * 22.0f + fabsf(v) * 9.0f) * 6.2831853f / 3.0f);
		return a * (0.80f + 0.20f * smooth(0.35f, 0.65f, lug));
	case XGPU_TERRAIN_SPECIES_ELITE:
		/* (two long toes splayed forward, the pad behind them, a heel spur) */
		a = ellipse(u, v, 0.18f * length, 0.045f, 0.28f * length, 0.034f, 0.26f);
		b = ellipse(u, v, 0.18f * length, -0.045f, 0.28f * length, 0.034f, -0.26f);
		toe = ellipse(u, v, -0.05f * length, 0.0f, 0.16f * length, 0.05f, 0.0f);
		lug = ellipse(u, v, -0.40f * length, 0.0f, 0.07f * length, 0.018f, 0.0f);
		return fmaxf(fmaxf(a, b), fmaxf(toe * 0.85f, lug * 0.9f));
	case XGPU_TERRAIN_SPECIES_GRUNT:
		/* (a broad pad and three stubby toes) */
		a = ellipse(u, v, -0.10f * length, 0.0f, 0.30f * length, 0.065f, 0.0f);
		b = ellipse(u, v, 0.30f * length, 0.0f, 0.13f * length, 0.026f, 0.0f);
		toe = fmaxf(ellipse(u, v, 0.24f * length, 0.052f, 0.12f * length, 0.024f, 0.45f),
			ellipse(u, v, 0.24f * length, -0.052f, 0.12f * length, 0.024f, -0.45f));
		return fmaxf(a, fmaxf(b, toe));
	case XGPU_TERRAIN_SPECIES_JACKAL:
	{
		/* (a bird's: three thin toes fanned forward, one behind) */
		float best = 0.0f;
		int k;

		for (k = -1; k <= 1; k++)
		{
			float turn = 0.5f * (float)k, cu = 0.20f * length * cosf(turn), cv = 0.20f * length * sinf(turn);

			best = fmaxf(best, ellipse(u, v, cu, cv, 0.26f * length, 0.014f, turn));
		}
		best = fmaxf(best, ellipse(u, v, -0.28f * length, 0.0f, 0.16f * length, 0.012f, 0.0f));
		return fmaxf(best, ellipse(u, v, 0.0f, 0.0f, 0.10f * length, 0.024f, 0.0f));
	}
	case XGPU_TERRAIN_SPECIES_HUNTER:
		/* (a great flat foot, its front edge ridged) */
		a = ellipse(u, v, 0.0f, 0.0f, 0.46f * length, 0.17f, 0.0f);
		lug = 0.5f + 0.5f * cosf(v * 6.2831853f / 0.07f);
		return a * (0.85f + 0.15f * smooth(0.3f, 0.7f, lug) * smooth(0.0f, 0.3f * length, u));
	case XGPU_TERRAIN_SPECIES_FLOOD:
		a = ellipse(u, v, -0.25f * length, 0.0f, 0.24f * length, 0.045f, 0.1f);
		b = ellipse(u, v, 0.15f * length, 0.0f, 0.33f * length, 0.06f, -0.08f);
		return fmaxf(a, b);
	case XGPU_TERRAIN_SPECIES_SENTINEL:
		return 0.0f;
	default:
		return ellipse(u, v, 0.0f, 0.0f, 0.45f * length, 0.055f, 0.0f);
	}
}

static void stamp_make(int species, struct stamp *stamp)
{
	/* (the foot's length (m) and the depth's scale by the species' mass) */
	static const float lengths[SPECIES] = { 0.28f, 0.33f, 0.42f, 0.24f, 0.22f, 0.62f, 0.30f, 0.30f };
	static const float depths[SPECIES] = { 1.0f, 1.15f, 1.3f, 0.85f, 0.7f, 1.7f, 0.95f, 0.0f };
	float blurred[STAMP_SIDE * STAMP_SIDE], dent_sum = 0.0f, rim_most = 0.0f;
	float box = lengths[species] * 1.6f;
	int x, y, k;

	stamp->length = lengths[species];
	stamp->box = box;
	stamp->depth = depths[species];
	for (y = 0; y < STAMP_SIDE; y++)
		for (x = 0; x < STAMP_SIDE; x++)
		{
			float u = ((float)x + 0.5f) / STAMP_SIDE * box - 0.5f * box;
			float v = ((float)y + 0.5f) / STAMP_SIDE * box - 0.5f * box;
			float d = 0.0f;
			int sx, sy;

			/* (4 samples a texel) */
			for (sy = 0; sy < 2; sy++)
				for (sx = 0; sx < 2; sx++)
					d += sole(species, u + ((float)sx - 0.5f) * 0.5f * box / STAMP_SIDE,
						v + ((float)sy - 0.5f) * 0.5f * box / STAMP_SIDE, lengths[species]);
			d *= 0.25f;
			if (species == XGPU_TERRAIN_SPECIES_FLOOD)
				d *= 0.7f + 0.3f * stamp_noise(x, y, 7);
			stamp->dent[y * STAMP_SIDE + x] = d;
			dent_sum += d;
		}
	/* (the rim: the dent spread about 6 cm, less the dent, the outside alone,
	its highest made 1: a print's rim is a low, broad ridge of what the foot
	pushed aside) */
	memcpy(blurred, stamp->dent, sizeof(blurred));
	for (k = 0; k < 8; k++)
	{
		float next[STAMP_SIDE * STAMP_SIDE];

		for (y = 0; y < STAMP_SIDE; y++)
			for (x = 0; x < STAMP_SIDE; x++)
			{
				float sum = 0.0f;
				int dx, dy, n = 0;

				for (dy = -1; dy <= 1; dy++)
					for (dx = -1; dx <= 1; dx++)
					{
						int xx = x + dx, yy = y + dy;

						if (xx < 0 || yy < 0 || xx >= STAMP_SIDE || yy >= STAMP_SIDE)
							continue;
						sum += blurred[yy * STAMP_SIDE + xx];
						n++;
					}
				next[y * STAMP_SIDE + x] = sum / (float)n;
			}
		memcpy(blurred, next, sizeof(blurred));
	}
	for (k = 0; k < STAMP_SIDE * STAMP_SIDE; k++)
	{
		float r = blurred[k] - stamp->dent[k];

		r = r > 0.0f ? r * (1.0f - stamp->dent[k]) : 0.0f;
		stamp->rim[k] = r;
		rim_most = r > rim_most ? r : rim_most;
	}
	for (k = 0; k < STAMP_SIDE * STAMP_SIDE && rim_most > 0.0f; k++)
		stamp->rim[k] /= rim_most;
	(void)dent_sum;
}

static void stamps_make(void)
{
	int species;

	if (prints.stamps_made)
		return;
	for (species = 0; species < SPECIES; species++)
		stamp_make(species, &prints.stamps[species]);
	prints.stamps_made = 1;
}

/* a stamp's value at (s, t) in its texels, linear; 0 outside */
static float stamp_at(const float *field, float s, float t)
{
	int x0 = (int)floorf(s - 0.5f), y0 = (int)floorf(t - 0.5f);
	float fx = s - 0.5f - (float)x0, fy = t - 0.5f - (float)y0, sum = 0.0f;
	int k;

	for (k = 0; k < 4; k++)
	{
		int x = x0 + (k & 1), y = y0 + (k >> 1);
		float w = (k & 1 ? fx : 1.0f - fx) * (k >> 1 ? fy : 1.0f - fy);

		if (x >= 0 && y >= 0 && x < STAMP_SIDE && y < STAMP_SIDE)
			sum += w * field[y * STAMP_SIDE + x];
	}
	return sum;
}

/* ---------- pages */

static void page_dirty(struct print_page *page, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
	if (!page->changed)
	{
		page->dirty[0] = x0;
		page->dirty[1] = y0;
		page->dirty[2] = x1;
		page->dirty[3] = y1;
	}
	else
	{
		page->dirty[0] = x0 < page->dirty[0] ? x0 : page->dirty[0];
		page->dirty[1] = y0 < page->dirty[1] ? y0 : page->dirty[1];
		page->dirty[2] = x1 > page->dirty[2] ? x1 : page->dirty[2];
		page->dirty[3] = y1 > page->dirty[3] ? y1 : page->dirty[3];
	}
	page->changed = 1;
}

static void page_clear(struct print_page *page)
{
	memset(page->h, 0, PAGE_AREA * sizeof(float));
	memset(page->c, 0, PAGE_AREA);
	memset(page->g, 0, PAGE_AREA * sizeof(int16_t));
	memset(page->cover, 0, PAGE_AREA);
	memset(page->wet, 0, PAGE_AREA);
}

static struct print_page *page_find(int32_t x, int32_t y)
{
	uint32_t index;

	for (index = 0; index < POOL_PAGES; index++)
		if (prints.pages[index].used && prints.pages[index].x == x && prints.pages[index].y == y)
			return &prints.pages[index];
	return NULL;
}

static float page_distance2(const struct print_page *page)
{
	float cx = ((float)page->x + 0.5f) * PAGE_WU - prints.camera[0];
	float cy = ((float)page->y + 0.5f) * PAGE_WU - prints.camera[1];

	return cx * cx + cy * cy;
}

/* a page at (x, y), made (its ground's height z) where there is none: a free
one, else the one farthest from the camera not pressed in the last 5
seconds; NULL where none can be */
static struct print_page *page_get(int32_t x, int32_t y, float z, uint8_t ground)
{
	struct print_page *page = page_find(x, y), *far = NULL;
	float far_distance = -1.0f;
	uint32_t index;

	if (page)
		return page;
	for (index = 0; index < POOL_PAGES && !page; index++)
		if (!prints.pages[index].used)
			page = &prints.pages[index];
	if (!page)
	{
		for (index = 0; index < POOL_PAGES; index++)
		{
			struct print_page *other = &prints.pages[index];
			float distance = page_distance2(other);

			if (prints.clock - other->stamped > 5.0f && distance > far_distance)
			{
				far = other;
				far_distance = distance;
			}
		}
		page = far;
		if (!page)
			return NULL;
	}
	if (!page->h)
	{
		page->h = malloc(PAGE_AREA * sizeof(float));
		page->c = malloc(PAGE_AREA);
		page->g = malloc(PAGE_AREA * sizeof(int16_t));
		page->cover = malloc(PAGE_AREA);
		page->wet = malloc(PAGE_AREA);
		if (!page->h || !page->c || !page->g || !page->cover || !page->wet)
		{
			free(page->h);
			free(page->c);
			free(page->g);
			free(page->cover);
			free(page->wet);
			page->h = NULL;
			page->c = page->cover = page->wet = NULL;
			page->g = NULL;
			return NULL;
		}
	}
	page_clear(page);
	page->used = 1;
	page->x = x;
	page->y = y;
	page->z = z;
	page->ground = ground;
	page->stamped = page->infilled = prints.clock;
	page->relax_until = 0.0f;
	page->changed = 0;
	page->whole = 1;
	page_dirty(page, 0, 0, PRINT_PAGE_TEXELS - 1, PRINT_PAGE_TEXELS - 1);
	prints.window_valid = 0;
	return page;
}

void terrain_prints_forget(void)
{
	uint32_t index;

	for (index = 0; index < POOL_PAGES; index++)
		prints.pages[index].used = 0;
	prints.window_valid = 0;
	prints.clock = prints.counted = 0.0f;
	prints.wader_count = 0;
}

void terrain_prints_counts(uint32_t *pages, uint32_t *stamps)
{
	uint32_t index, used = 0;

	for (index = 0; index < POOL_PAGES; index++)
		used += prints.pages[index].used ? 1u : 0u;
	if (pages)
		*pages = used;
	if (stamps)
		*stamps = prints.stamp_count;
}

/* ---------- stamping */

static uint8_t ground_of(uint32_t material_type)
{
	/* (CE's material types: dirt 0, sand 1, snow 3; stone, ice, metal and
	the rest take no print; under water (0x1c) neither) */
	switch (material_type)
	{
	case 0: return GROUND_DIRT;
	case 1: return GROUND_SAND;
	case 3: return GROUND_SNOW;
	default: return GROUND_NONE;
	}
}

static void unfold(uint32_t word, float out[3])
{
	float x = (float)(int16_t)(word & 0xffffu) / 32767.0f, y = (float)(int16_t)(word >> 16) / 32767.0f;
	float z = 1.0f - fabsf(x) - fabsf(y), t = z < 0.0f ? -z : 0.0f, length;

	x += x >= 0.0f ? -t : t;
	y += y >= 0.0f ? -t : t;
	length = sqrtf(x * x + y * y + z * z);
	out[0] = x / length;
	out[1] = y / length;
	out[2] = z / length;
}

static int foot_wet(const float position[3])
{
	uint32_t index;

	for (index = 0; index < prints.wader_count; index++)
	{
		const struct xgpu_stream_terrain_wader *w = &prints.waders[index];
		float dx = w->position[0] - position[0], dy = w->position[1] - position[1];

		if (!(w->kind & XGPU_TERRAIN_WADER_BIPED))
			continue;
		if (dx * dx + dy * dy < (WET_REACH + w->radius) * (WET_REACH + w->radius))
			return 1;
	}
	return 0;
}

void terrain_prints_waders(const struct xgpu_stream_terrain_wader *waders, uint32_t count)
{
	prints.wader_count = count < XGPU_TERRAIN_WADERS_MOST ? count : XGPU_TERRAIN_WADERS_MOST;
	memcpy(prints.waders, waders, prints.wader_count * sizeof(waders[0]));
}

void terrain_prints_event(const struct xgpu_stream_terrain_event *event)
{
	uint32_t kind = event->kind & 0xffu, species = event->kind >> 24, material = (event->kind >> 16) & 0xffu;
	uint8_t ground = ground_of(material);
	const struct stamp *stamp;
	float normal[3], forward[3], fx, fy, length, depth, rim_share, packing, half, reach, along, stretch, toe;
	int32_t px0, py0, px1, py1, px, py;
	int wet, right;

	if (kind < XGPU_TERRAIN_EVENT_WALK || kind > XGPU_TERRAIN_EVENT_LAND || ground == GROUND_NONE)
		return;
	if (species >= SPECIES)
		species = XGPU_TERRAIN_SPECIES_OTHER;
	/* (a Sentinel floats) */
	if (species == XGPU_TERRAIN_SPECIES_SENTINEL)
		return;
	if (!isfinite(event->position[0]) || !isfinite(event->position[1]) || !isfinite(event->position[2]))
		return;
	unfold(event->normal, normal);
	/* (no print on a slope past 40 degrees: the foot slides, the sand runs) */
	if (normal[2] < 0.76f)
		return;
	stamps_make();
	stamp = &prints.stamps[species];
	unfold(event->facing ? event->facing : 0x00007fffu, forward);
	fx = forward[0];
	fy = forward[1];
	length = sqrtf(fx * fx + fy * fy);
	if (length < 1.0e-3f)
	{
		fx = 1.0f;
		fy = 0.0f;
	}
	else
	{
		fx /= length;
		fy /= length;
	}
	wet = ground == GROUND_SAND && foot_wet(event->position);
	right = (event->kind & XGPU_TERRAIN_EVENT_RIGHT) != 0;
	/* (the ground's depth (m), its rim's height over the depth, the
	packing: design 4.1) */
	switch (ground)
	{
	case GROUND_SAND: depth = wet ? 0.015f : 0.035f; rim_share = wet ? 0.15f : 0.35f; packing = wet ? 0.6f : 0.35f;
		break;
	case GROUND_SNOW: depth = 0.10f; rim_share = 0.06f; packing = 1.0f; break;
	case GROUND_MUD: depth = 0.03f; rim_share = 0.35f; packing = 0.5f; break;
	default: depth = 0.008f; rim_share = 0.2f; packing = 0.7f; break;
	}
	/* (the walk's: a run digs the toe, a landing deepens and scuffs, a slide
	stretches, a shuffle barely presses) */
	stretch = 1.0f;
	toe = 0.0f;
	switch (kind)
	{
	case XGPU_TERRAIN_EVENT_RUN: depth *= 1.15f; toe = 0.35f; break;
	case XGPU_TERRAIN_EVENT_JUMP: depth *= 1.1f; toe = 0.6f; break;
	case XGPU_TERRAIN_EVENT_LAND: depth *= 1.5f; stretch = 1.2f; rim_share *= 1.3f; break;
	case XGPU_TERRAIN_EVENT_SLIDE: depth *= 0.8f; stretch = 2.0f; break;
	case XGPU_TERRAIN_EVENT_SHUFFLE: depth *= 0.55f; break;
	default: break;
	}
	depth *= stamp->depth / WORLD_UNIT_METRES;
	/* (the pass marches snow's prints to DEPTH_MOST, the rest's to half of it) */
	if (depth > DEPTH_MOST * (ground == GROUND_SNOW ? 0.9f : 0.45f))
		depth = DEPTH_MOST * (ground == GROUND_SNOW ? 0.9f : 0.45f);
	/* (the stamp's box: half its side along the foot, world units; its reach
	about the centre) */
	half = 0.5f * stamp->box / WORLD_UNIT_METRES;
	along = half * stretch;
	reach = sqrtf(along * along + half * half);
	px0 = (int32_t)floorf((event->position[0] - reach) / PAGE_WU - 1.0f / PRINT_PAGE_INNER);
	px1 = (int32_t)floorf((event->position[0] + reach) / PAGE_WU + 1.0f / PRINT_PAGE_INNER);
	py0 = (int32_t)floorf((event->position[1] - reach) / PAGE_WU - 1.0f / PRINT_PAGE_INNER);
	py1 = (int32_t)floorf((event->position[1] + reach) / PAGE_WU + 1.0f / PRINT_PAGE_INNER);
	prints.stamp_count++;
	if (prints.stamp_count <= 4 || getenv("HALO_TERRAIN_PRINTS_LOG"))
		host_logf(HOST_LOG_INFO, "prism terrain: print %u at %.3f %.3f %.3f (species %u, ground %u, %s, %s foot, "
			"%.1f mm deep)", prints.stamp_count, event->position[0], event->position[1], event->position[2], species,
			ground, wet ? "wet" : "dry", right ? "right" : "left", depth * WORLD_UNIT_METRES * 1000.0f);
	for (py = py0; py <= py1; py++)
		for (px = px0; px <= px1; px++)
		{
			struct print_page *page = page_get(px, py, event->position[2], ground);
			float origin_x = ((float)px * PRINT_PAGE_INNER - 1.0f) * TEXEL_WU;
			float origin_y = ((float)py * PRINT_PAGE_INNER - 1.0f) * TEXEL_WU;
			int32_t tx0, tx1, ty0, ty1, tx, ty, touched = 0;

			if (!page)
				continue;
			tx0 = (int32_t)floorf((event->position[0] - reach - origin_x) / TEXEL_WU);
			tx1 = (int32_t)ceilf((event->position[0] + reach - origin_x) / TEXEL_WU);
			ty0 = (int32_t)floorf((event->position[1] - reach - origin_y) / TEXEL_WU);
			ty1 = (int32_t)ceilf((event->position[1] + reach - origin_y) / TEXEL_WU);
			tx0 = tx0 < 0 ? 0 : tx0;
			ty0 = ty0 < 0 ? 0 : ty0;
			tx1 = tx1 > (int32_t)PRINT_PAGE_TEXELS - 1 ? (int32_t)PRINT_PAGE_TEXELS - 1 : tx1;
			ty1 = ty1 > (int32_t)PRINT_PAGE_TEXELS - 1 ? (int32_t)PRINT_PAGE_TEXELS - 1 : ty1;
			for (ty = ty0; ty <= ty1; ty++)
				for (tx = tx0; tx <= tx1; tx++)
				{
					float wx = origin_x + ((float)tx + 0.5f) * TEXEL_WU - event->position[0];
					float wy = origin_y + ((float)ty + 0.5f) * TEXEL_WU - event->position[1];
					/* (the foot's frame: u along it, v across, the right foot's
					side; a stretched stamp's u squeezed back) */
					float u = (wx * fx + wy * fy) / stretch, v = -wx * fy + wy * fx;
					float s, t, d, r, value, ground_z;
					int at = ty * (int32_t)PRINT_PAGE_TEXELS + tx;

					if (!right)
						v = -v;
					s = (u / half * 0.5f + 0.5f) * STAMP_SIDE;
					t = (v / half * 0.5f + 0.5f) * STAMP_SIDE;
					if (s < 0.0f || t < 0.0f || s >= STAMP_SIDE || t >= STAMP_SIDE)
						continue;
					d = stamp_at(stamp->dent, s, t);
					r = stamp_at(stamp->rim, s, t);
					if (d < 0.01f && r < 0.002f)
						continue;
					/* (the toe dug: deeper toward the front) */
					if (toe > 0.0f)
						d *= 1.0f + toe * clampf(u / half, -0.6f, 0.6f);
					value = -depth * d + depth * rim_share * r;
					if (value < 0.0f)
						page->h[at] = value < page->h[at] ? value : page->h[at];
					else if (page->h[at] >= 0.0f)
						page->h[at] = value > page->h[at] ? value : page->h[at];
					{
						uint8_t packed = (uint8_t)(255.0f * packing * clampf(d * 1.4f, 0.0f, 1.0f) + 0.5f);

						page->c[at] = packed > page->c[at] ? packed : page->c[at];
					}
					/* (the ground: the plane of the foot's ray's hit, over the
					page's height; a texel's first) */
					ground_z = event->position[2] - (normal[0] * wx + normal[1] * wy) /
						fmaxf(normal[2], 0.2f) - page->z;
					if (!page->cover[at])
						page->g[at] = (int16_t)clampf(floorf(ground_z * 1024.0f + 0.5f), -32767.0f, 32767.0f);
					page->cover[at] = 255;
					if (wet && d > 0.01f)
						page->wet[at] = 1;
					touched = 1;
				}
			if (touched)
			{
				page_dirty(page, tx0, ty0, tx1, ty1);
				if (page->relax_until <= prints.clock)
				{
					page->pressed[0] = tx0;
					page->pressed[1] = ty0;
					page->pressed[2] = tx1;
					page->pressed[3] = ty1;
				}
				else
				{
					page->pressed[0] = tx0 < page->pressed[0] ? tx0 : page->pressed[0];
					page->pressed[1] = ty0 < page->pressed[1] ? ty0 : page->pressed[1];
					page->pressed[2] = tx1 > page->pressed[2] ? tx1 : page->pressed[2];
					page->pressed[3] = ty1 > page->pressed[3] ? ty1 : page->pressed[3];
				}
				page->stamped = prints.clock;
				page->relax_until = prints.clock + RELAX_SECONDS;
			}
		}
}

/* ---------- relax and infill */

/* the most a texel may stand over its neighbour (world units): the angle
of repose's tangent over a texel. Sand's 56 degrees, not loose dry sand's
34: a beach's sand is damp under its surface and a boot packs it, so a
print's wall stands steep (at 34 every print rounded flat under b30's sun,
35 degrees up, and cast no shadow; at 42 a boot's 10 cm sole became a
V-shaped trough of 2 cm texels with no floor); wet sand's 72; snow packs
and its wall stands (68) */
static float repose_step(uint8_t ground, int wet)
{
	float degrees = ground == GROUND_SAND ? (wet ? 72.0f : 56.0f) : ground == GROUND_SNOW ? 68.0f :
		ground == GROUND_MUD ? 60.0f : 50.0f;

	return tanf(degrees * 0.017453293f) * TEXEL_WU;
}

/* (a page's relax over its pressed region; done (its relax ended) once
nothing moves more than a hundredth of a millimetre) */
static void page_relax(struct print_page *page)
{
	float most = 0.0f;
	int32_t x0 = page->pressed[0] - 2, y0 = page->pressed[1] - 2, x1 = page->pressed[2] + 2, y1 = page->pressed[3] + 2;
	float dry = repose_step(page->ground, 0), wetted = repose_step(page->ground, 1);
	int pass, x, y;

	x0 = x0 < 1 ? 1 : x0;
	y0 = y0 < 1 ? 1 : y0;
	x1 = x1 > (int32_t)PRINT_PAGE_TEXELS - 2 ? (int32_t)PRINT_PAGE_TEXELS - 2 : x1;
	y1 = y1 > (int32_t)PRINT_PAGE_TEXELS - 2 ? (int32_t)PRINT_PAGE_TEXELS - 2 : y1;
	for (pass = 0; pass < RELAX_PASSES; pass++)
		for (y = y0; y <= y1; y++)
			for (x = x0; x <= x1; x++)
			{
				int at = y * (int32_t)PRINT_PAGE_TEXELS + x, k;
				static const int offsets[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
				float limit = page->wet[at] ? wetted : dry;

				for (k = 0; k < 4; k++)
				{
					int other = (y + offsets[k][1]) * (int32_t)PRINT_PAGE_TEXELS + x + offsets[k][0];
					float drop = page->h[at] - page->h[other];

					if (drop > limit)
					{
						float moved = (drop - limit) * 0.2f;

						page->h[at] -= moved;
						page->h[other] += moved;
						most = moved > most ? moved : most;
					}
				}
			}
	page_dirty(page, x0 - 1, y0 - 1, x1 + 1, y1 + 1);
	if (most < 0.00001f / WORLD_UNIT_METRES)
		page->relax_until = prints.clock;
}

/* the rate (a second) a ground's print sinks toward flat, by the wind
(world units a second) */
static float infill_rate(uint8_t ground, float wind)
{
	float w = clampf(wind / 0.8f, 0.0f, 3.0f);

	switch (ground)
	{
	/* (b30's 0.8 of wind: about a third of the depth gone in two minutes,
	the walls softened: an old trail still reads as one) */
	case GROUND_SAND: return 0.0035f * (0.3f + 0.7f * w);
	case GROUND_SNOW: return 0.0006f;
	case GROUND_MUD: return 0.0002f;
	default: return 0.0004f * (0.5f + 0.5f * w);
	}
}

static void page_infill(struct print_page *page)
{
	float seconds = prints.clock - page->infilled;
	float rate = infill_rate(page->ground, prints.wind);
	float sink = expf(-seconds * rate), sink_wet = expf(-seconds * rate * 0.4f), pack = expf(-seconds * rate / 3.0f);
	/* (the softening: a share of a blur's step a second, wind-borne grains
	filling the edges first) */
	float spread = clampf(seconds * rate * 2.5f, 0.0f, 0.5f);
	float next[PRINT_PAGE_TEXELS];
	float above[PRINT_PAGE_TEXELS];
	int x, y;

	page->infilled = prints.clock;
	if (seconds <= 0.0f)
		return;
	for (x = 0; x < (int)PAGE_AREA; x++)
	{
		page->h[x] *= page->wet[x] ? sink_wet : sink;
		page->c[x] = (uint8_t)((float)page->c[x] * pack + 0.5f);
	}
	if (spread > 0.001f)
	{
		/* (a 5-point blur in place, a row behind) */
		memcpy(above, page->h, sizeof(above));
		for (y = 1; y < (int)PRINT_PAGE_TEXELS - 1; y++)
		{
			const float *row = page->h + y * PRINT_PAGE_TEXELS, *below = row + PRINT_PAGE_TEXELS;

			for (x = 1; x < (int)PRINT_PAGE_TEXELS - 1; x++)
			{
				float mean = 0.25f * (row[x - 1] + row[x + 1] + above[x] + below[x]);

				next[x] = row[x] + (mean - row[x]) * spread;
			}
			memcpy(above, row, sizeof(above));
			memcpy(page->h + y * PRINT_PAGE_TEXELS + 1, next + 1, (PRINT_PAGE_TEXELS - 2) * sizeof(float));
		}
	}
	page_dirty(page, 0, 0, PRINT_PAGE_TEXELS - 1, PRINT_PAGE_TEXELS - 1);
}

void terrain_prints_frame(float game_seconds, const float camera[3], const float wind[3])
{
	uint32_t index, looked;

	if (game_seconds > 0.0f && game_seconds < 1.0f)
		prints.clock += game_seconds;
	/* (every 20 seconds of the game's with prints: the pages and stamps, for
	the log) */
	if (prints.stamp_count && prints.clock - prints.counted >= 20.0f)
	{
		uint32_t pages;

		terrain_prints_counts(&pages, NULL);
		host_logf(HOST_LOG_INFO, "prism terrain: prints at %.0f s of the game: %u stamps, %u pages of %u, wind %.2f",
			prints.clock, prints.stamp_count, pages, POOL_PAGES, prints.wind);
		prints.counted = prints.clock;
	}
	memcpy(prints.camera, camera, sizeof(prints.camera));
	prints.wind = sqrtf(wind[0] * wind[0] + wind[1] * wind[1]);
	if (!isfinite(prints.wind))
		prints.wind = 0.0f;
	/* (relax: the pages pressed in the last second and a half, while the
	game runs) */
	for (index = 0; index < POOL_PAGES && game_seconds > 0.0f; index++)
	{
		struct print_page *page = &prints.pages[index];

		if (page->used && page->relax_until > prints.clock)
			page_relax(page);
	}
	/* (infill: one page a frame, the next due near the camera) */
	for (looked = 0; looked < POOL_PAGES && game_seconds > 0.0f; looked++)
	{
		struct print_page *page = &prints.pages[prints.infill_next];

		prints.infill_next = (prints.infill_next + 1) % POOL_PAGES;
		if (!page->used || page->relax_until > prints.clock || prints.clock - page->infilled < INFILL_SECONDS ||
			page_distance2(page) > INFILL_RANGE * INFILL_RANGE)
			continue;
		page_infill(page);
		break;
	}
}

/* ---------- upload */

static uint16_t half_of(float value)
{
	uint32_t bits, sign, exponent, mantissa;

	memcpy(&bits, &value, sizeof(bits));
	sign = (bits >> 16) & 0x8000u;
	exponent = (bits >> 23) & 0xffu;
	mantissa = bits & 0x7fffffu;
	if (exponent == 0xffu)
		return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0u));
	if (exponent > 142u)
		return (uint16_t)(sign | 0x7bffu);
	if (exponent < 113u)
	{
		/* (a subnormal half, rounded to nearest) */
		if (exponent < 102u)
			return (uint16_t)sign;
		mantissa |= 0x800000u;
		{
			uint32_t shift = 126u - exponent, value16 = mantissa >> shift;
			uint32_t rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);

			if (rest > halfway || (rest == halfway && (value16 & 1u)))
				value16++;
			return (uint16_t)(sign | value16);
		}
	}
	{
		uint32_t value16 = ((exponent - 112u) << 10) | (mantissa >> 13), rest = mantissa & 0x1fffu;

		if (rest > 0x1000u || (rest == 0x1000u && (value16 & 1u)))
			value16++;
		return (uint16_t)(sign | value16);
	}
}

static void texel_of(const struct print_page *page, int at, float out[4])
{
	out[0] = page->h[at];
	out[1] = (float)page->c[at] / 255.0f;
	out[3] = (float)page->cover[at] / 255.0f;
	out[2] = (float)page->g[at] / 1024.0f * out[3];
}

static int pool_make(void)
{
	struct xgpu_vulkan_image_description description;

	if (prints.pool && prints.table)
		return 1;
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D_ARRAY;
	description.format = XGPU_VULKAN_FORMAT_RGBA16F;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED;
	description.width = description.height = PRINT_PAGE_TEXELS;
	description.depth_or_layers = POOL_PAGES;
	description.levels = PRINT_LEVELS;
	if (!prints.pool)
		prints.pool = xgpu_vulkan_image_create(&description);
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	description.width = description.height = PRINT_WINDOW;
	description.depth_or_layers = 1;
	description.levels = 1;
	if (!prints.table)
		prints.table = xgpu_vulkan_image_create(&description);
	if (!prints.pool || !prints.table)
	{
		host_logf(HOST_LOG_WARN, "prism terrain: the prints' pool cannot be made: no prints");
		return 0;
	}
	/* (every page sent whole again into the new pool) */
	{
		uint32_t index;

		for (index = 0; index < POOL_PAGES; index++)
			if (prints.pages[index].used)
			{
				prints.pages[index].whole = 1;
				page_dirty(&prints.pages[index], 0, 0, PRINT_PAGE_TEXELS - 1, PRINT_PAGE_TEXELS - 1);
			}
	}
	prints.window_valid = 0;
	return 1;
}

static void page_upload(struct print_page *page, uint32_t layer)
{
	int32_t x0 = page->dirty[0], y0 = page->dirty[1], x1 = page->dirty[2], y1 = page->dirty[3];
	uint32_t width, height, side, level;
	int32_t x, y;
	float *from;

	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > (int32_t)PRINT_PAGE_TEXELS - 1 ? (int32_t)PRINT_PAGE_TEXELS - 1 : x1;
	y1 = y1 > (int32_t)PRINT_PAGE_TEXELS - 1 ? (int32_t)PRINT_PAGE_TEXELS - 1 : y1;
	width = (uint32_t)(x1 - x0 + 1);
	height = (uint32_t)(y1 - y0 + 1);
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
		{
			float texel[4];
			uint16_t *to = &prints.scratch[((uint32_t)(y - y0) * width + (uint32_t)(x - x0)) * 4];
			int k;

			texel_of(page, y * (int32_t)PRINT_PAGE_TEXELS + x, texel);
			for (k = 0; k < 4; k++)
				to[k] = half_of(texel[k]);
		}
	xgpu_vulkan_image_upload(prints.pool, 0, layer, (uint32_t)x0, (uint32_t)y0, 0, width, height, 1, prints.scratch,
		width * 8, width * height * 8);
	/* (the smaller levels, each the mean of four of the one before, whole) */
	side = PRINT_PAGE_TEXELS / 2;
	from = NULL;
	for (level = 1; level < PRINT_LEVELS; level++, side /= 2)
	{
		float *into = prints.level[(level - 1) & 1];
		uint32_t i, j;

		for (j = 0; j < side; j++)
			for (i = 0; i < side; i++)
			{
				float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				int k, c;

				for (k = 0; k < 4; k++)
				{
					uint32_t sx = i * 2 + (uint32_t)(k & 1), sy = j * 2 + (uint32_t)(k >> 1);
					float texel[4];

					if (!from)
						texel_of(page, (int)(sy * PRINT_PAGE_TEXELS + sx), texel);
					else
						memcpy(texel, from + (sy * side * 2 + sx) * 4, sizeof(texel));
					for (c = 0; c < 4; c++)
						sum[c] += texel[c];
				}
				for (c = 0; c < 4; c++)
				{
					into[(j * side + i) * 4 + c] = sum[c] * 0.25f;
					prints.scratch[(j * side + i) * 4 + c] = half_of(sum[c] * 0.25f);
				}
			}
		xgpu_vulkan_image_upload(prints.pool, level, layer, 0, 0, 0, side, side, 1, prints.scratch, side * 8,
			side * side * 8);
		from = into;
	}
	page->changed = 0;
	page->whole = 0;
}

static void window_make(int32_t centre_x, int32_t centre_y)
{
	uint32_t index;

	memset(prints.table_texels, 0, sizeof(prints.table_texels));
	for (index = 0; index < POOL_PAGES; index++)
	{
		const struct print_page *page = &prints.pages[index];
		uint32_t *to;

		if (!page->used || abs(page->x - centre_x) >= (int32_t)PRINT_WINDOW / 2 ||
			abs(page->y - centre_y) >= (int32_t)PRINT_WINDOW / 2)
			continue;
		to = &prints.table_texels[(((uint32_t)page->y & (PRINT_WINDOW - 1)) * PRINT_WINDOW +
			((uint32_t)page->x & (PRINT_WINDOW - 1))) * 4];
		to[0] = index + 1;
		to[1] = (uint32_t)page->x;
		to[2] = (uint32_t)page->y;
		memcpy(&to[3], &page->z, sizeof(float));
	}
	xgpu_vulkan_image_upload(prints.table, 0, 0, 0, 0, 0, PRINT_WINDOW, PRINT_WINDOW, 1, prints.table_texels,
		PRINT_WINDOW * 16, sizeof(prints.table_texels));
	prints.window_x = centre_x;
	prints.window_y = centre_y;
	prints.window_valid = 1;
}

int terrain_prints_prepare(const float eye[3], const float sun[4], struct terrain_prints_draw *draw)
{
	int32_t cx = (int32_t)floorf(eye[0] / PAGE_WU), cy = (int32_t)floorf(eye[1] / PAGE_WU);
	uint32_t index, near = 0, used = 0;

	memset(draw, 0, sizeof(*draw));
	for (index = 0; index < POOL_PAGES; index++)
	{
		const struct print_page *page = &prints.pages[index];

		if (!page->used)
			continue;
		used++;
		if (abs(page->x - cx) < (int32_t)PRINT_WINDOW / 2 && abs(page->y - cy) < (int32_t)PRINT_WINDOW / 2)
			near++;
	}
	if (!used || !pool_make())
		return 0;
	for (index = 0; index < POOL_PAGES; index++)
		if (prints.pages[index].used && prints.pages[index].changed)
			page_upload(&prints.pages[index], index);
	if (!prints.window_valid || cx != prints.window_x || cy != prints.window_y)
		window_make(cx, cy);
	if (!prints.logged)
	{
		host_logf(HOST_LOG_INFO, "prism terrain: prints drawn (a page %.3f world units of %u texels of %.0f mm, a "
			"pool of %u pages, a window of %u pages a side)", PAGE_WU, PRINT_PAGE_INNER,
			PRINT_TEXEL_METRES * 1000.0f, POOL_PAGES, PRINT_WINDOW);
		prints.logged = 1;
	}
	if (!near)
		return 0;
	draw->pool = prints.pool;
	draw->table = prints.table;
	draw->values[0][0] = 1.0f / PAGE_WU;
	draw->values[0][1] = TEXEL_WU;
	draw->values[0][2] = (float)PRINT_PAGE_INNER;
	draw->values[0][3] = (float)PRINT_WINDOW;
	memcpy(draw->values[1], sun, 4 * sizeof(float));
	draw->values[2][0] = PARALLAX_RANGE;
	draw->values[2][1] = 24.0f;
	draw->values[2][2] = 8.0f;
	draw->values[2][3] = 0.0f;
	draw->values[3][0] = DEPTH_MOST;
	draw->values[3][1] = RIM_MOST;
	draw->values[3][2] = (float)PRINT_PAGE_TEXELS;
	draw->values[3][3] = (float)(PRINT_LEVELS - 1);
	return 1;
}
