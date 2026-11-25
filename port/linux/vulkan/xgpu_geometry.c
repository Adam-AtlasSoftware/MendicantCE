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
	uint32_t object, mesh;
	/* the decision */
	int variant, lod_known;
	uint32_t lod, traced_lod, drawable_last, lod_count;
	uint32_t triangles[XGPU_GEOMETRY_LODS];
	float cutoff[XGPU_GEOMETRY_LODS];
	uint64_t seen_frame, decided_frame, view_frame;
	/* its size in the view times its distance then (its distance unknown:
	the size alone), and its distance this frame (-1 unknown) */
	float view_measure;
	int measure_has_distance;
	float distance;
	struct channel_note notes[XGPU_GEOMETRY_CHANNELS];
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

static uint64_t key_of(uint32_t object, uint32_t mesh)
{
	uint64_t key = ((uint64_t)object << 32) | mesh;

	return key == EMPTY_KEY ? 1ull << 63 : key;
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
static struct entry *entry_find(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int make)
{
	uint64_t key = key_of(object, mesh);
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
		return entry_find(geometry, object, mesh, make);
	}
	memset(&geometry->entries[at], 0, sizeof(geometry->entries[at]));
	geometry->entries[at].key = key;
	geometry->entries[at].object = object;
	geometry->entries[at].mesh = mesh;
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

/* the most coarse level an entry may be traced at */
static uint32_t traced_most(const struct xgpu_geometry *geometry, const struct entry *entry)
{
	if (entry->lod_count && entry->distance >= 0.0f && entry->distance > geometry->settings.coarse_reach)
		return entry->lod_count - 1u;
	return entry->drawable_last;
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

				if (entry->traced_lod <= entry->lod)
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
		if (entry->seen_frame + XGPU_GEOMETRY_AGE_FRAMES < geometry->frame)
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
	budget_share(geometry);
	geometry->statistics.entries = geometry->count;
	if (geometry->log_level >= 1 && geometry->frame % 600u == 0)
	{
		const struct xgpu_geometry_statistics *s = &geometry->statistics;

		say(geometry, 1, "geometry: %llu frames, %u objects; switches view %llu shadow %llu traced %llu; flips view %llu "
			"shadow %llu traced %llu; stock to remodel %llu, remodel to stock %llu; shadow not the view's %llu, traced "
			"stock under a remodel %llu, traced another level near %llu; traced levels made coarser %llu, frames over "
			"budget %llu, levels the floor held %llu; traced triangles %llu",
			(unsigned long long)s->frames, s->entries, (unsigned long long)s->switches[0],
			(unsigned long long)s->switches[1], (unsigned long long)s->switches[2], (unsigned long long)s->flips[0],
			(unsigned long long)s->flips[1], (unsigned long long)s->flips[2], (unsigned long long)s->to_remodel,
			(unsigned long long)s->to_stock, (unsigned long long)s->shadow_mismatch,
			(unsigned long long)s->traced_stock, (unsigned long long)s->traced_mismatch,
			(unsigned long long)s->traced_degraded, (unsigned long long)s->over_budget,
			(unsigned long long)s->floor_held, (unsigned long long)s->traced_triangles_last);
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

void xgpu_geometry_resolve(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int shadow,
	const struct xgpu_geometry_ladder *ladder, float lod_pixels, const float origin[3],
	struct xgpu_geometry_choice *choice)
{
	struct entry *entry = entry_find(geometry, object, mesh, 1);
	float distance = -1.0f, pixels;
	uint32_t lod;

	memset(choice, 0, sizeof(*choice));
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
		if (lod > entry->drawable_last)
		{
			geometry->statistics.floor_held++;
			lod = entry->drawable_last;
		}
		if (entry->lod_known && lod != entry->lod)
			say(geometry, 2, "geometry: frame %llu object %08x mesh %x level %u -> %u (%.1f px%s, %.1f wu)",
				(unsigned long long)geometry->frame, object, mesh, entry->lod, lod, (double)pixels,
				shadow ? " estimated" : "", (double)distance);
		entry->lod = lod;
		entry->lod_known = 1;
		entry->decided_frame = geometry->frame;
		if (entry->traced_lod < lod)
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

void xgpu_geometry_note(struct xgpu_geometry *geometry, uint32_t object, uint32_t mesh, int channel, int variant,
	uint32_t lod)
{
	struct entry *entry;
	uint32_t value;
	int flipped;

	if (channel < 0 || channel >= (int)XGPU_GEOMETRY_CHANNELS)
		return;
	entry = entry_find(geometry, object, mesh, 1);
	if (!entry)
		return;
	entry->seen_frame = geometry->frame;
	value = (((uint32_t)variant << 8) | (lod & 0xffu)) + 1u;
	/* (a pass's draws of the frame after its first: the first's kept; the
	shadow's cascades and the view's chunks draw the one choice) */
	if (entry->notes[channel].value && entry->notes[channel].frame == geometry->frame)
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

void xgpu_geometry_statistics(const struct xgpu_geometry *geometry, struct xgpu_geometry_statistics *statistics)
{
	*statistics = geometry->statistics;
	statistics->entries = geometry->count;
}

uint64_t xgpu_geometry_frame_number(const struct xgpu_geometry *geometry)
{
	return geometry->frame;
}
