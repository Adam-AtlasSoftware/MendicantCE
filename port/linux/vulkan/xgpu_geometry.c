/*
XGPU_GEOMETRY.C

PRISM's resolved geometry of the drawn objects (xgpu_geometry.h).
*/

#include "xgpu_geometry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMPTY_KEY 0ull
#define FIRST_CAPACITY 1024u
/* the traced budget's band: levels made finer again only while the total
stays under this share of the budget */
#define BUDGET_BAND 0.85f
/* ...and a level a budget made coarser is held at least this many frames
before it may be made finer again (an object panned in and out of view
changes the totals from frame to frame) */
#define BUDGET_HOLD_FRAMES 150u
/* an object of no datum (the game's 0xffffffff: a ghost, the sky, a
cinematic's) is keyed by its mesh and the cell of its place, so that two of
them do not share one decision; one moved to a cell beside its last takes
its decision along. Such entries are let go of sooner */
#define DATUMLESS_CELL 2.0f
#define DATUMLESS_AGE_FRAMES 120u

struct channel_note
{
	/* (variant << 8 | level) + 1; 0 none yet */
	uint32_t value;
	uint64_t frame, change_frame;
	int direction;
};

struct entry
{
	uint64_t key;
	/* the object (0xffffffff: none), its mesh, and for an object of no
	datum the cell of its place (DATUMLESS_CELL world units; 0 unknown) */
	uint32_t object, mesh, cell;
	/* the decision */
	int variant, lod_known;
	uint32_t lod, traced_lod, drawable_last, lod_count;
	/* the level the skinned vertices' budget holds it to at least */
	uint32_t budget_lod;
	/* the frames the budgets last made its drawn and traced levels coarser */
	uint64_t budget_frame, traced_frame;
	/* the frame its drawn level last changed (by its size or a budget) */
	uint64_t level_frame;
	/* skinned each frame (a rig, or moving: its skinning not kept), and the
	frames its skinning has been kept in a row */
	int per_frame;
	uint32_t kept_frames;
	uint32_t vertices[XGPU_GEOMETRY_LODS];
	uint32_t triangles[XGPU_GEOMETRY_LODS];
	float cutoff[XGPU_GEOMETRY_LODS];
	uint64_t seen_frame, decided_frame, view_frame;
	/* its size in the view times its distance then (its distance unknown:
	the size alone), and its distance this frame (-1 unknown) */
	float view_measure;
	int measure_has_distance;
	float distance;
	struct channel_note notes[XGPU_GEOMETRY_CHANNELS];
	/* the frame a channel's stock fallback was last counted */
	uint64_t fallback_frame[XGPU_GEOMETRY_CHANNELS];
	/* the decision's own changes (for the log's flips of the traced level) */
	struct channel_note traced_note;
};

struct xgpu_geometry
{
	struct entry *entries;
	uint32_t capacity, count;
	uint64_t frame;
	float eye[3];
	int eye_known;
	struct xgpu_geometry_settings settings;
	struct xgpu_geometry_statistics statistics;
	int log_level;
	xgpu_geometry_log_function log;
	void *log_context;
	uint32_t *order;
	uint32_t order_capacity;
};

static const char *const channel_names[XGPU_GEOMETRY_CHANNELS] = { "view", "shadow", "traced" };

static void say(struct xgpu_geometry *geometry, int level, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

#include <stdarg.h>

static void say(struct xgpu_geometry *geometry, int level, const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (!geometry->log || geometry->log_level < level)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	geometry->log(geometry->log_context, line);
}

static uint64_t key_of(uint32_t object, uint32_t mesh, uint32_t cell)
{
	uint64_t key = ((uint64_t)object << 32) | mesh;

	/* (an object of no datum: its cell mixed in, the top bit kept clear of
	a datum's keys by the mix) */
	if (cell)
	{
		key ^= (uint64_t)cell * 0x9e3779b97f4a7c15ull;
		key ^= key >> 29;
		key *= 0xbf58476d1ce4e5b9ull;
		key ^= key >> 32;
	}
	return key == EMPTY_KEY ? 1ull << 63 : key;
}

uint32_t xgpu_geometry_cell(uint32_t object, const float origin[3])
{
	int32_t c[3];
	uint32_t hash = 2166136261u;
	int axis;

	if (object != 0xffffffffu || !origin)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		float q = floorf(origin[axis] / DATUMLESS_CELL);

		c[axis] = q > 1.0e6f ? 1000000 : q < -1.0e6f ? -1000000 : (int32_t)q;
		hash = (hash ^ (uint32_t)c[axis]) * 16777619u;
	}
	return hash ? hash : 1u;
}

static uint32_t hash_of(uint64_t key)
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdull;
	key ^= key >> 33;
	key *= 0xc4ceb9fe1a85ec53ull;
	key ^= key >> 33;
	return (uint32_t)key;
}

static int table_grow(struct xgpu_geometry *geometry)
{
	uint32_t capacity = geometry->capacity ? geometry->capacity * 2u : FIRST_CAPACITY, index;
	struct entry *entries = calloc(capacity, sizeof(*entries)), *old = geometry->entries;

	if (!entries)
		return 0;
	for (index = 0; old && index < geometry->capacity; index++)
	{
		uint32_t at;

		if (old[index].key == EMPTY_KEY)
			continue;
		for (at = hash_of(old[index].key) & (capacity - 1u); entries[at].key != EMPTY_KEY; at = (at + 1u) & (capacity - 1u))
		{
		}
		entries[at] = old[index];
	}
	free(old);
	geometry->entries = entries;
	geometry->capacity = capacity;
	return 1;
}

/* the object's entry, made where new (NULL: no memory) */
static struct entry *entry_find(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, uint32_t cell,
	int make)
{
	uint64_t key = key_of(object, mesh, cell);
	uint32_t at;

	if (!geometry->capacity)
	{
		if (!make || !table_grow(geometry))
			return NULL;
	}
	for (at = hash_of(key) & (geometry->capacity - 1u); geometry->entries[at].key != EMPTY_KEY;
		at = (at + 1u) & (geometry->capacity - 1u))
	{
		if (geometry->entries[at].key == key)
			return &geometry->entries[at];
	}
	if (!make)
		return NULL;
	if ((geometry->count + 1u) * 10u > geometry->capacity * 7u)
	{
		if (!table_grow(geometry))
			return NULL;
		return entry_find(geometry, object, mesh, cell, make);
	}
	memset(&geometry->entries[at], 0, sizeof(geometry->entries[at]));
	geometry->entries[at].key = key;
	geometry->entries[at].object = object;
	geometry->entries[at].mesh = mesh;
	geometry->entries[at].cell = cell;
	geometry->entries[at].distance = -1.0f;
	geometry->count++;
	geometry->statistics.objects++;
	return &geometry->entries[at];
}

/* an entry let go of (linear probing's backward shift: no tombstones) */
static void entry_remove(struct xgpu_geometry *geometry, uint32_t at)
{
	uint32_t mask = geometry->capacity - 1u, next;

	geometry->entries[at].key = EMPTY_KEY;
	geometry->count--;
	for (next = (at + 1u) & mask; geometry->entries[next].key != EMPTY_KEY; next = (next + 1u) & mask)
	{
		uint32_t home = hash_of(geometry->entries[next].key) & mask;

		/* (the entry at next may move to at where at lies between its home
		and next, cyclically) */
		if ((next > at && (home <= at || home > next)) || (next < at && home <= at && home > next))
		{
			geometry->entries[at] = geometry->entries[next];
			geometry->entries[next].key = EMPTY_KEY;
			at = next;
		}
	}
}

struct xgpu_geometry *xgpu_geometry_create(void)
{
	struct xgpu_geometry *geometry = calloc(1, sizeof(*geometry));

	if (!geometry)
		return NULL;
	geometry->settings.hysteresis = 0.3f;
	geometry->settings.coarse_reach = 40.0f;
	return geometry;
}

void xgpu_geometry_destroy(struct xgpu_geometry *geometry)
{
	if (!geometry)
		return;
	free(geometry->entries);
	free(geometry->order);
	free(geometry);
}

void xgpu_geometry_log(struct xgpu_geometry *geometry, int level, xgpu_geometry_log_function log, void *context)
{
	geometry->log_level = level;
	geometry->log = log;
	geometry->log_context = context;
}

void xgpu_geometry_settings_set(struct xgpu_geometry *geometry, const struct xgpu_geometry_settings *settings)
{
	geometry->settings = *settings;
	if (!(geometry->settings.hysteresis >= 0.0f))
		geometry->settings.hysteresis = 0.0f;
	if (geometry->settings.hysteresis > 0.5f)
		geometry->settings.hysteresis = 0.5f;
	if (!(geometry->settings.coarse_reach >= 0.0f))
		geometry->settings.coarse_reach = 0.0f;
}

void xgpu_geometry_map(struct xgpu_geometry *geometry)
{
	if (geometry->entries)
		memset(geometry->entries, 0, geometry->capacity * sizeof(*geometry->entries));
	geometry->count = 0;
}

uint32_t xgpu_geometry_drawable_last(const struct xgpu_geometry_ladder *ladder)
{
	uint32_t last = 0, lod;

	if (!ladder || !ladder->lod_count)
		return 0;
	for (lod = 1; lod < ladder->lod_count && lod < XGPU_GEOMETRY_LODS; lod++)
	{
		if ((uint64_t)ladder->triangles[lod] * XGPU_GEOMETRY_LADDER_RATIO < (uint64_t)ladder->triangles[last])
			break;
		last = lod;
	}
	return last;
}

/* the most coarse level an entry may be traced at: beyond the coarse reach
any level of its remodel; within it (or its distance unknown) the level
drawn, never coarser, so that a ray leaving the drawn surface near the eye
meets that surface (the budget then left over, counted) */
static uint32_t traced_most(const struct xgpu_geometry *geometry, const struct entry *entry)
{
	if (entry->lod_count && entry->distance >= 0.0f && entry->distance > geometry->settings.coarse_reach)
		return entry->lod_count - 1u;
	return entry->lod;
}

/* a change of a value noted, its flips counted */
static int note_change(struct channel_note *note, uint32_t value, uint64_t frame, int *flipped)
{
	int direction;

	*flipped = 0;
	if (note->value == value)
	{
		note->frame = frame;
		return 0;
	}
	if (!note->value)
	{
		note->value = value;
		note->frame = note->change_frame = frame;
		note->direction = 0;
		return 0;
	}
	direction = value > note->value ? 1 : -1;
	if (note->direction && direction == -note->direction && frame - note->change_frame <= XGPU_GEOMETRY_FLIP_FRAMES)
		*flipped = 1;
	note->value = value;
	note->direction = direction;
	note->frame = note->change_frame = frame;
	return 1;
}

/* the order of the entries by distance, for the budget (qsort_r is not
everywhere: an insertion sort, a few hundred objects) */
static void order_sort(const struct xgpu_geometry *geometry, uint32_t *order, uint32_t count, int far_first)
{
	uint32_t i, j;

	for (i = 1; i < count; i++)
	{
		uint32_t value = order[i];
		float d = geometry->entries[value].distance;

		for (j = i; j > 0; j--)
		{
			float other = geometry->entries[order[j - 1]].distance;

			if (far_first ? other >= d : other <= d)
				break;
			order[j] = order[j - 1];
		}
		order[j] = value;
	}
}

/* the traced levels of the objects drawn last frame shared out within the
budget: the farthest made coarser first, the nearest made finer first while
the total stays within the band */
static void budget_share(struct xgpu_geometry *geometry)
{
	uint64_t total = 0, budget = geometry->settings.traced_budget, band;
	uint32_t index, count = 0, pass;
	int changed;

	if (geometry->order_capacity < geometry->capacity)
	{
		uint32_t *grown = realloc(geometry->order, geometry->capacity * sizeof(*grown));

		if (!grown)
			return;
		geometry->order = grown;
		geometry->order_capacity = geometry->capacity;
	}
	for (index = 0; index < geometry->capacity; index++)
	{
		struct entry *entry = &geometry->entries[index];
		uint32_t most;

		if (entry->key == EMPTY_KEY || entry->variant != XGPU_GEOMETRY_REMODEL || !entry->lod_count ||
			entry->seen_frame + 1u < geometry->frame)
			continue;
		most = traced_most(geometry, entry);
		if (most < entry->lod)
			most = entry->lod;
		if (entry->traced_lod < entry->lod)
			entry->traced_lod = entry->lod;
		if (entry->traced_lod > most)
			entry->traced_lod = most;
		total += entry->triangles[entry->traced_lod];
		geometry->order[count++] = index;
	}
	if (!budget || !count)
	{
		/* (no budget: traced as drawn) */
		for (index = 0; index < count; index++)
			geometry->entries[geometry->order[index]].traced_lod = geometry->entries[geometry->order[index]].lod;
		geometry->statistics.traced_triangles_last = total;
		return;
	}
	band = (uint64_t)((float)budget * BUDGET_BAND);
	if (total > budget)
	{
		order_sort(geometry, geometry->order, count, 1);
		for (pass = 0, changed = 1; changed && total > budget && pass < XGPU_GEOMETRY_LODS; pass++)
		{
			changed = 0;
			for (index = 0; index < count && total > budget; index++)
			{
				struct entry *entry = &geometry->entries[geometry->order[index]];
				uint32_t most = traced_most(geometry, entry);

				if (entry->traced_lod >= most)
					continue;
				total -= entry->triangles[entry->traced_lod];
				entry->traced_lod++;
				entry->traced_frame = geometry->frame;
				total += entry->triangles[entry->traced_lod];
				changed = 1;
			}
		}
	}
	else if (total < band)
	{
		order_sort(geometry, geometry->order, count, 0);
		for (pass = 0, changed = 1; changed && pass < XGPU_GEOMETRY_LODS; pass++)
		{
			changed = 0;
			for (index = 0; index < count; index++)
			{
				struct entry *entry = &geometry->entries[geometry->order[index]];
				uint64_t finer;

				if (entry->traced_lod <= entry->lod || geometry->frame - entry->traced_frame < BUDGET_HOLD_FRAMES)
					continue;
				finer = total - entry->triangles[entry->traced_lod] + entry->triangles[entry->traced_lod - 1u];
				if (finer > band)
					continue;
				total = finer;
				entry->traced_lod--;
				changed = 1;
			}
		}
	}
	if (total > budget)
		geometry->statistics.over_budget++;
	for (index = 0; index < count; index++)
	{
		struct entry *entry = &geometry->entries[geometry->order[index]];
		int flipped;

		if (entry->traced_lod > entry->lod)
			geometry->statistics.traced_degraded++;
		if (note_change(&entry->traced_note, entry->traced_lod + 1u, geometry->frame, &flipped))
			say(geometry, 2, "geometry: frame %llu object %08x mesh %x traced level %u (drawn %u, %.1f wu)%s",
				(unsigned long long)geometry->frame, entry->object, entry->mesh, entry->traced_lod, entry->lod,
				(double)entry->distance, flipped ? " (budget flip)" : "");
	}
	geometry->statistics.traced_triangles_last = total;
}

/* the drawn levels of the objects skinned each frame (drawn the frame
before) held within the skinned vertices' budget: the farthest held to a
coarser level of their drawable ladder first, the nearest let go first
while the total stays within the band; never stock */
static void skinned_share(struct xgpu_geometry *geometry)
{
	uint64_t total = 0, budget = geometry->settings.skinned_budget, band;
	uint32_t index, count = 0, pass;
	int changed;

	if (geometry->order_capacity < geometry->capacity)
	{
		uint32_t *grown = realloc(geometry->order, geometry->capacity * sizeof(*grown));

		if (!grown)
			return;
		geometry->order = grown;
		geometry->order_capacity = geometry->capacity;
	}
	for (index = 0; index < geometry->capacity; index++)
	{
		struct entry *entry = &geometry->entries[index];

		if (entry->key == EMPTY_KEY || entry->variant != XGPU_GEOMETRY_REMODEL || !entry->per_frame ||
			!entry->lod_count || entry->seen_frame + 1u < geometry->frame)
			continue;
		if (!budget)
			entry->budget_lod = 0;
		total += entry->vertices[entry->lod];
		geometry->order[count++] = index;
	}
	geometry->statistics.skinned_vertices_last = total;
	if (!budget || !count)
		return;
	band = (uint64_t)((float)budget * BUDGET_BAND);
	if (total > budget)
	{
		int settled;

		order_sort(geometry, geometry->order, count, 1);
		/* (the objects whose level has not changed lately first, so that a
		level just made finer by its size is not undone at once: a flip;
		then any) */
		for (settled = 1; settled >= 0 && total > budget; settled--)
		for (pass = 0, changed = 1; changed && total > budget && pass < XGPU_GEOMETRY_LODS; pass++)
		{
			changed = 0;
			for (index = 0; index < count && total > budget; index++)
			{
				struct entry *entry = &geometry->entries[geometry->order[index]];

				if (entry->lod >= entry->drawable_last ||
					(settled && entry->level_frame + XGPU_GEOMETRY_FLIP_FRAMES >= geometry->frame))
					continue;
				entry->level_frame = geometry->frame;
				total -= entry->vertices[entry->lod];
				entry->lod++;
				entry->budget_lod = entry->lod;
				entry->budget_frame = geometry->frame;
				total += entry->vertices[entry->lod];
				changed = 1;
			}
		}
		if (total > budget)
			geometry->statistics.skinned_over_budget++;
	}
	else if (total < band)
	{
		order_sort(geometry, geometry->order, count, 0);
		for (index = 0; index < count; index++)
		{
			struct entry *entry = &geometry->entries[geometry->order[index]];
			uint64_t finer;

			if (!entry->budget_lod || geometry->frame - entry->budget_frame < BUDGET_HOLD_FRAMES ||
				entry->level_frame + 2u * XGPU_GEOMETRY_FLIP_FRAMES >= geometry->frame)
				continue;
			/* (held at its budget's level: one level finer where the total
			stays in the band; held coarser by its own size: let go) */
			if (entry->lod > entry->budget_lod)
			{
				entry->budget_lod = 0;
				continue;
			}
			finer = total - entry->vertices[entry->lod] + entry->vertices[entry->lod - 1u];
			if (finer > band)
				continue;
			total = finer;
			entry->budget_lod--;
			entry->level_frame = geometry->frame;
		}
	}
	for (index = 0; index < count; index++)
		if (geometry->entries[geometry->order[index]].budget_lod)
			geometry->statistics.drawn_degraded++;
}

/* last frame's notes compared: the shadows' and the traced shape against
the view's */
static void notes_compare(struct xgpu_geometry *geometry, struct entry *entry)
{
	uint64_t last = geometry->frame - 1u;
	const struct channel_note *view = &entry->notes[XGPU_GEOMETRY_VIEW];
	const struct channel_note *shadow = &entry->notes[XGPU_GEOMETRY_SHADOW];
	const struct channel_note *traced = &entry->notes[XGPU_GEOMETRY_TRACED];

	if (!view->value || view->frame != last)
		return;
	if (shadow->value && shadow->frame == last && shadow->value != view->value)
		geometry->statistics.shadow_mismatch++;
	if (traced->value && traced->frame == last && traced->value != view->value)
	{
		uint32_t view_variant = (view->value - 1u) >> 8, traced_variant = (traced->value - 1u) >> 8;

		if (view_variant == XGPU_GEOMETRY_REMODEL && traced_variant != XGPU_GEOMETRY_REMODEL)
			geometry->statistics.traced_stock++;
		else if (entry->distance < 0.0f || entry->distance <= geometry->settings.coarse_reach)
			geometry->statistics.traced_mismatch++;
	}
}

void xgpu_geometry_frame(struct xgpu_geometry *geometry, const float eye[3])
{
	uint32_t index;

	geometry->frame++;
	geometry->statistics.frames++;
	geometry->eye_known = eye != NULL;
	if (eye)
		memcpy(geometry->eye, eye, sizeof(geometry->eye));
	/* (the objects not drawn lately let go of; the shift moves a later
	entry into the place freed, which is looked at again) */
	for (index = 0; index < geometry->capacity; index++)
	{
		struct entry *entry = &geometry->entries[index];

		if (entry->key == EMPTY_KEY)
			continue;
		if (entry->seen_frame + (entry->cell ? DATUMLESS_AGE_FRAMES : XGPU_GEOMETRY_AGE_FRAMES) < geometry->frame)
		{
			entry_remove(geometry, index);
			index--;
		}
	}
	for (index = 0; index < geometry->capacity; index++)
	{
		if (geometry->entries[index].key != EMPTY_KEY)
			notes_compare(geometry, &geometry->entries[index]);
	}
	skinned_share(geometry);
	budget_share(geometry);
	geometry->statistics.entries = geometry->count;
	if (geometry->log_level >= 1 && geometry->frame % 600u == 0)
	{
		const struct xgpu_geometry_statistics *s = &geometry->statistics;

		say(geometry, 1, "geometry: %llu frames, %u objects; switches view %llu shadow %llu traced %llu; flips view %llu "
			"shadow %llu traced %llu; stock to remodel %llu, remodel to stock %llu; shadow not the view's %llu, traced "
			"stock under a remodel %llu, traced another level near %llu; traced levels made coarser %llu, frames over "
			"budget %llu, levels the floor held %llu; traced triangles %llu; drawn levels made coarser for the skinned vertices "
			"%llu, frames over %llu, skinned vertices %llu; stock fallbacks view %llu shadow %llu traced %llu",
			(unsigned long long)s->frames, s->entries, (unsigned long long)s->switches[0],
			(unsigned long long)s->switches[1], (unsigned long long)s->switches[2], (unsigned long long)s->flips[0],
			(unsigned long long)s->flips[1], (unsigned long long)s->flips[2], (unsigned long long)s->to_remodel,
			(unsigned long long)s->to_stock, (unsigned long long)s->shadow_mismatch,
			(unsigned long long)s->traced_stock, (unsigned long long)s->traced_mismatch,
			(unsigned long long)s->traced_degraded, (unsigned long long)s->over_budget,
			(unsigned long long)s->floor_held, (unsigned long long)s->traced_triangles_last,
			(unsigned long long)s->drawn_degraded, (unsigned long long)s->skinned_over_budget,
			(unsigned long long)s->skinned_vertices_last, (unsigned long long)s->fallbacks[0],
			(unsigned long long)s->fallbacks[1], (unsigned long long)s->fallbacks[2]);
	}
}

/* the level a size draws, with the hysteresis against the level had */
static uint32_t level_for(const struct xgpu_geometry *geometry, const struct entry *entry, float pixels)
{
	uint32_t lod;

	for (lod = 0; lod + 1u < entry->lod_count; lod++)
	{
		float cutoff = entry->cutoff[lod];

		if (entry->lod_known && lod >= entry->lod && geometry->settings.hysteresis > 0.0f)
			cutoff *= 1.0f - geometry->settings.hysteresis;
		if (pixels >= cutoff)
			break;
	}
	return lod;
}

/* an object of no datum's entry: its cell's, else one of its mesh seen in
a cell beside it last frame and not yet decided this frame, moved to its
cell (it moved), else a new one */
static struct entry *datumless_find(struct xgpu_geometry *geometry, uint32_t mesh, uint32_t cell,
	const float origin[3])
{
	struct entry *entry = entry_find(geometry, 0xffffffffu, mesh, cell, 0);
	int dx, dy, dz;

	if (entry)
		return entry;
	for (dx = -1; dx <= 1; dx++)
	for (dy = -1; dy <= 1; dy++)
	for (dz = -1; dz <= 1; dz++)
	{
		float near[3];
		uint32_t other;
		struct entry *found, moved;

		if (!dx && !dy && !dz)
			continue;
		near[0] = origin[0] + (float)dx * DATUMLESS_CELL;
		near[1] = origin[1] + (float)dy * DATUMLESS_CELL;
		near[2] = origin[2] + (float)dz * DATUMLESS_CELL;
		other = xgpu_geometry_cell(0xffffffffu, near);
		found = entry_find(geometry, 0xffffffffu, mesh, other, 0);
		if (!found || found->seen_frame + 1u < geometry->frame || found->decided_frame == geometry->frame)
			continue;
		moved = *found;
		entry_remove(geometry, (uint32_t)(found - geometry->entries));
		entry = entry_find(geometry, 0xffffffffu, mesh, cell, 1);
		if (!entry)
			return NULL;
		moved.key = entry->key;
		moved.cell = cell;
		*entry = moved;
		geometry->statistics.objects--;
		return entry;
	}
	return entry_find(geometry, 0xffffffffu, mesh, cell, 1);
}

void xgpu_geometry_resolve(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int shadow,
	const struct xgpu_geometry_ladder *ladder, float lod_pixels, const float origin[3],
	struct xgpu_geometry_choice *choice)
{
	uint32_t cell = xgpu_geometry_cell(object, origin);
	struct entry *entry = cell ? datumless_find(geometry, mesh, cell, origin) :
		entry_find(geometry, object, mesh, 0, 1);
	float distance = -1.0f, pixels;
	uint32_t lod;

	memset(choice, 0, sizeof(*choice));
	choice->cell = cell;
	if (!entry)
	{
		/* (no memory: as the store would have it, without hysteresis) */
		if (ladder)
		{
			choice->variant = XGPU_GEOMETRY_REMODEL;
			choice->drawable_last = xgpu_geometry_drawable_last(ladder);
			for (lod = 0; lod + 1u < ladder->lod_count && lod_pixels < ladder->cutoff[lod]; lod++)
			{
			}
			choice->lod = choice->traced_lod = lod > choice->drawable_last ? choice->drawable_last : lod;
		}
		return;
	}
	entry->seen_frame = geometry->frame;
	if (origin && geometry->eye_known)
	{
		float d[3];

		d[0] = origin[0] - geometry->eye[0];
		d[1] = origin[1] - geometry->eye[1];
		d[2] = origin[2] - geometry->eye[2];
		distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		entry->distance = distance;
	}
	/* the variant: stock until the mesh is in, then the remodel */
	if (!ladder || !ladder->lod_count)
	{
		if (entry->variant == XGPU_GEOMETRY_REMODEL)
		{
			geometry->statistics.to_stock++;
			say(geometry, 2, "geometry: frame %llu object %08x mesh %x remodel lost: drawn stock",
				(unsigned long long)geometry->frame, object, mesh);
		}
		entry->variant = XGPU_GEOMETRY_STOCK;
		entry->lod_known = 0;
		return;
	}
	if (entry->variant != XGPU_GEOMETRY_REMODEL || entry->lod_count != ladder->lod_count)
	{
		if (entry->variant != XGPU_GEOMETRY_REMODEL)
		{
			geometry->statistics.to_remodel++;
			say(geometry, 2, "geometry: frame %llu object %08x mesh %x the remodel in", (unsigned long long)geometry->frame,
				object, mesh);
		}
		entry->variant = XGPU_GEOMETRY_REMODEL;
		entry->lod_count = ladder->lod_count < XGPU_GEOMETRY_LODS ? ladder->lod_count : XGPU_GEOMETRY_LODS;
		memcpy(entry->triangles, ladder->triangles, sizeof(entry->triangles));
		memcpy(entry->cutoff, ladder->cutoff, sizeof(entry->cutoff));
		memcpy(entry->vertices, ladder->vertices, sizeof(entry->vertices));
		entry->per_frame = ladder->per_frame || entry->per_frame;
		entry->budget_lod = 0;
		entry->drawable_last = xgpu_geometry_drawable_last(ladder);
		entry->lod_known = 0;
	}
	/* (the view's size kept, times its distance where known) */
	if (!shadow)
	{
		entry->view_frame = geometry->frame;
		entry->measure_has_distance = distance > 0.0f;
		entry->view_measure = distance > 0.0f ? lod_pixels * distance : lod_pixels;
	}
	/* the level, once a frame: the first draw's (the view's own size; a
	shadow's the view's size where it has been seen, else its own) */
	if (entry->decided_frame != geometry->frame || !entry->lod_known)
	{
		if (shadow && entry->view_frame)
			pixels = entry->measure_has_distance && distance > 0.0f ? entry->view_measure / distance :
				entry->view_measure;
		else
			pixels = lod_pixels;
		lod = level_for(geometry, entry, pixels);
		if (lod < entry->budget_lod)
			lod = entry->budget_lod;
		if (lod > entry->drawable_last)
		{
			geometry->statistics.floor_held++;
			lod = entry->drawable_last;
		}
		if (entry->lod_known && lod != entry->lod)
			say(geometry, 2, "geometry: frame %llu object %08x mesh %x level %u -> %u (%.1f px%s, %.1f wu)",
				(unsigned long long)geometry->frame, object, mesh, entry->lod, lod, (double)pixels,
				shadow ? " estimated" : "", (double)distance);
		if (entry->lod_known && lod != entry->lod)
			entry->level_frame = geometry->frame;
		entry->lod = lod;
		entry->lod_known = 1;
		entry->decided_frame = geometry->frame;
		/* (within the coarse reach, or its distance unknown, traced as drawn
		on the frame its level changes too: the budget's share was of last
		frame's level; beyond it never finer than drawn) */
		if (entry->traced_lod < lod || entry->distance < 0.0f || entry->distance <= geometry->settings.coarse_reach)
			entry->traced_lod = lod;
		geometry->statistics.decisions++;
	}
	choice->variant = XGPU_GEOMETRY_REMODEL;
	choice->lod = entry->lod;
	choice->traced_lod = entry->traced_lod < entry->lod ? entry->lod : entry->traced_lod;
	if (choice->traced_lod >= entry->lod_count)
		choice->traced_lod = entry->lod_count - 1u;
	choice->drawable_last = entry->drawable_last;
}

void xgpu_geometry_note(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, uint32_t cell, int channel,
	int variant, uint32_t lod)
{
	struct entry *entry;
	uint32_t value;
	int flipped;

	if (channel < 0 || channel >= (int)XGPU_GEOMETRY_CHANNELS)
		return;
	entry = entry_find(geometry, object, mesh, cell, 1);
	if (!entry)
		return;
	entry->seen_frame = geometry->frame;
	value = (((uint32_t)variant << 8) | (lod & 0xffu)) + 1u;
	/* (a remodel drawn, shadowed or traced as CE's model after all, or left
	out: a stock fallback, counted once a frame however many draws) */
	if (variant != XGPU_GEOMETRY_REMODEL && entry->variant == XGPU_GEOMETRY_REMODEL &&
		entry->fallback_frame[channel] != geometry->frame)
	{
		entry->fallback_frame[channel] = geometry->frame;
		geometry->statistics.fallbacks[channel]++;
		say(geometry, 2, "geometry: frame %llu object %08x mesh %x %s stock fallback (%s)",
			(unsigned long long)geometry->frame, object, mesh, channel_names[channel],
			variant == XGPU_GEOMETRY_STOCK ? "CE's model" : "nothing");
	}
	/* (a pass's draws of the frame after its first: the first's kept, the
	shadow's cascades and the view's chunks drawing the one choice; but a
	draw of the frame made stock under a remodel is the frame's) */
	if (entry->notes[channel].value && entry->notes[channel].frame == geometry->frame &&
		!(variant == XGPU_GEOMETRY_STOCK && ((entry->notes[channel].value - 1u) >> 8) == XGPU_GEOMETRY_REMODEL))
		return;
	if (note_change(&entry->notes[channel], value, geometry->frame, &flipped))
	{
		geometry->statistics.switches[channel]++;
		if (flipped)
			geometry->statistics.flips[channel]++;
		say(geometry, 2, "geometry: frame %llu object %08x mesh %x %s drew %s %u%s", (unsigned long long)geometry->frame,
			object, mesh, channel_names[channel], variant == XGPU_GEOMETRY_REMODEL ? "remodel" :
			variant == XGPU_GEOMETRY_STOCK ? "stock" : "nothing", lod, flipped ? " (flip)" : "");
	}
}

void xgpu_geometry_skinned(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, uint32_t cell, int kept)
{
	struct entry *entry = entry_find(geometry, object, mesh, cell, 0);

	if (!entry)
		return;
	if (!kept)
	{
		entry->per_frame = 1;
		entry->kept_frames = 0;
	}
	else if (entry->per_frame && ++entry->kept_frames > 30u)
		entry->per_frame = 0;
}

void xgpu_geometry_statistics(const struct xgpu_geometry *geometry, struct xgpu_geometry_statistics *statistics)
{
	*statistics = geometry->statistics;
	statistics->entries = geometry->count;
}

uint64_t xgpu_geometry_frame_number(const struct xgpu_geometry *geometry)
{
	return geometry->frame;
}
