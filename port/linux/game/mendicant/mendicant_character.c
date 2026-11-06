/*
MENDICANT_CHARACTER.C

MENDICANT AI's soldier character (the authentic soldier layer's M0): a
character per soldier, drawn once from his kind's distributions, that every
later behaviour reads instead of keeping its own numbers (the API is in
mendicant_internal.h; the tables and the draw in mendicant_traits.h).

- Ten core traits per soldier (courage, aggression, skill, discipline,
  caution, grenade, initiative, vocal, loyalty, reflex), each 0..1 around his
  kind's mean (species by rank), a few constants (lead bias, peek side, buddy
  preference, favourite weapon, seat), and at most one quirk: a loud
  archetype with a signature beat (Coward, Brave, Sharpshooter, Duelist,
  Veteran, Joker...). Hunters and the Flood have none.
- One mood per encounter fight (steady, jittery, fired up, sparing,
  liberal), drawn when the encounter goes into combat after 20 seconds of
  quiet, plus a small per-soldier swing of courage, aggression and vocal for
  that fight: the same squad plays differently on a replay.
- Seeding: a session nonce (64 random bits at the first map, or
  ai.mendicant.seed when set, for tests) and an epoch that moves on at every
  map start and loaded game state. A Covenant soldier's seed has the epoch in
  it, so a checkpoint revert meets new Covenant; a Marine's has not (it is
  his datum index, which the revert restores, the map, his unit's tag and its
  face and voice variant), so the squad that travels with the Chief keeps its
  men.
- The store is one entry per actor slot, outside game state (a revert must
  not bring the old draws back), checked against the full actor index and the
  epoch, and drawn lazily on first read; each decision touches its actor, so
  every soldier is drawn and logged (ai.mendicant.character_log) when he first
  decides.
- Its random numbers are its own (splitmix64): the game's seeds are never
  touched, so turning the layer on or off changes no stock roll by itself.

M0 changes no behaviour: nothing reads the traits yet. Under training the
layer is off unless ai.mendicant.character is set for the run
(mendicant_config.c), as the models were trained on stock soldiers.
*/

#include "cseries.h"
#include "memory/data.h"
#include "math/real_math.h"
#include "main/main.h"
#include "ai/ai.h"
#include "ai/actors.h"
#include "ai/encounters.h"
#include "objects/objects.h"
#include "units/units.h"
#include "tag_files/tag_files.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mendicant_internal.h"
#include "mendicant_traits.h"

void platform_log(char const *format, ...);
unsigned long system_milliseconds(void);
/* port/linux/src/posix_net.c, port/windows/src/win32_net.c, port/web/src/web_net.c */
void posix_random_bytes(void *buffer, unsigned int size);

/* ---------- constants */

#define CHARACTER_FIGHT_QUIET_TICKS 600 /* 20 s without combat: the next contact is a new fight */
#define CHARACTER_FIGHT_CHECK_TICKS 15  /* the encounters are looked at twice a second */
#define CHARACTER_SWING 0.08f           /* the per-fight swing of courage, aggression, vocal */
#define CHARACTER_COHESION_COMMANDED 0.6f

/* the per-fight swings' traits */
static int const character_swing_traits[3] = { MENDICANT_TRAIT_COURAGE, MENDICANT_TRAIT_AGGRESSION, MENDICANT_TRAIT_VOCAL };

/* ---------- types */

struct character_soldier
{
	long actor;            /* the full datum index it was drawn for, NONE empty */
	unsigned long epoch;   /* the epoch it was drawn in */
	struct mendicant_traits traits;
	unsigned long long seed;
	long fight;            /* the fight its swing and signature belong to (encounter << 16 | counter), -1 none */
	unsigned long long fight_seed;
	float swing[3];
	boolean signature_due, signature_spent;
};

struct character_fight
{
	unsigned short counter; /* fights this encounter has had this epoch; 0 none yet */
	boolean in_combat;
	long quiet_since;       /* when its last combat ended; -1 never in combat */
	unsigned char mood;
	unsigned long long seed;
};

/* ---------- globals */

static struct character_soldier character_soldiers[MENDICANT_MAX_ACTORS];
static struct character_fight character_fights[MENDICANT_MAX_ENCOUNTERS];
static float character_population_mean[MENDICANT_KINDS][MENDICANT_TRAITS];
static unsigned long long character_nonce = 0;
static boolean character_nonce_ready = FALSE;
static long character_nonce_pinned = 0; /* the ai.mendicant.seed the nonce came from, 0 random */
static unsigned long character_epoch = 0;
static unsigned long character_maps = 0;
static unsigned long character_map_hash = 0; /* this map's name (the Marines' seeds: one level's men are not another's) */
static long character_next_fight_check = 0;
static float character_strength = 1.0f; /* the settings the store and the means were made with */
static int character_visibility = MENDICANT_QUIRKS_NORMAL;
static long character_drawn = 0, character_quirked = 0; /* this map's, for the log line */

/* ---------- private code */

static boolean character_on(
	void)
{
	return mendicant_active && mendicant_config.character;
}

static struct actor_datum *character_actor_get(
	long actor_index)
{
	if (!actor_data || !actor_data->valid || actor_index == NONE)
		return NULL;
	return (struct actor_datum *)datum_try_and_get(actor_data, actor_index);
}

/* the actor's slot in the store, or -1 */
static int character_slot(
	long actor_index)
{
	int slot = (int)(unsigned short)actor_index;

	return actor_index == NONE || slot >= MENDICANT_MAX_ACTORS ? -1 : slot;
}

/* a tag's name in lower case, "" for none */
static void character_tag_name(
	long tag_index,
	char *name,
	size_t size)
{
	char const *text = tag_index != NONE ? tag_get_name(tag_index) : NULL;
	size_t index;

	name[0] = 0;
	if (!text)
		return;
	for (index = 0; text[index] && index + 1 < size; index++)
		name[index] = (char)tolower((unsigned char)text[index]);
	name[index] = 0;

	return;
}

/* FNV-1a of a text */
static unsigned long character_text_hash(
	char const *text)
{
	unsigned long hash = 0x811C9DC5UL;

	for (; text && *text; text++)
		hash = ((hash ^ (unsigned char)*text) * 0x01000193UL) & 0xFFFFFFFFUL;
	return hash;
}

/* species by rank, from the actor's type, its variant's name (characters\grunt\grunt major)
and its unique leader flag */
static int character_kind(
	struct actor_datum *actor)
{
	short type = actor->meta.type;
	char name[256];

	character_tag_name(actor->meta.variant_definition_index, name, sizeof(name));
	if (!name[0])
		character_tag_name(actor->meta.definition_index, name, sizeof(name));
	if (ACTOR_IS_GRUNT(type))
	{
		if (strstr(name, "flee"))
			return MENDICANT_KIND_GRUNT_FLEE;
		if (strstr(name, "specops") || strstr(name, "spec ops") || strstr(name, "spec_ops"))
			return MENDICANT_KIND_GRUNT_SPECOPS;
		return strstr(name, "major") ? MENDICANT_KIND_GRUNT_MAJOR : MENDICANT_KIND_GRUNT_MINOR;
	}
	if (ACTOR_IS_JACKAL(type))
		return strstr(name, "major") ? MENDICANT_KIND_JACKAL_MAJOR : MENDICANT_KIND_JACKAL_MINOR;
	if (ACTOR_IS_ELITE(type))
	{
		if (type == 5 || strstr(name, "stealth"))
			return MENDICANT_KIND_ELITE_STEALTH;
		if (strstr(name, "commander") || strstr(name, "zealot") || strstr(name, "ultra"))
			return MENDICANT_KIND_ELITE_COMMANDER;
		if (strstr(name, "specops") || strstr(name, "spec ops") || strstr(name, "spec_ops"))
			return MENDICANT_KIND_ELITE_SPECOPS;
		return strstr(name, "major") ? MENDICANT_KIND_ELITE_MAJOR : MENDICANT_KIND_ELITE_MINOR;
	}
	if (ACTOR_IS_MARINE(type))
	{
		if (actor->meta.unique_leader)
			return MENDICANT_KIND_SERGEANT;
		if (strstr(name, "armored") || strstr(name, "major"))
			return MENDICANT_KIND_MARINE_ARMORED;
		return MENDICANT_KIND_MARINE;
	}
	/* Hunters, the Flood, Sentinels, Engineers, turrets: unchanged */
	return MENDICANT_KIND_NONE;
}

static boolean character_kind_keeps(
	int kind)
{
	return kind == MENDICANT_KIND_MARINE || kind == MENDICANT_KIND_MARINE_ARMORED || kind == MENDICANT_KIND_SERGEANT;
}

/* who a soldier is: a Covenant soldier is new each attempt (the epoch); a
Marine is the same man after a revert (his datum index, which the revert
restores, his unit's tag and its face and voice variant) */
static unsigned long long character_seed(
	long actor_index,
	struct actor_datum *actor,
	int kind)
{
	if (character_kind_keeps(kind))
	{
		struct unit_datum *unit = actor->meta.unit_index != NONE ?
			(struct unit_datum *)object_try_and_get_and_verify_type(actor->meta.unit_index, _object_mask_all) : NULL;

		return mendicant_seed_marine(mendicant_mix64(character_nonce, character_map_hash), (unsigned long)actor_index,
			unit ? (unsigned long)unit->definition_index : 0, unit ? (unsigned short)unit->object.variant_number : 0);
	}
	return mendicant_seed_covenant(character_nonce, character_epoch, (unsigned long)actor_index,
		(unsigned long)actor->meta.variant_definition_index);
}

static void character_population_update(
	void)
{
	int kind;

	for (kind = 0; kind < MENDICANT_KINDS; kind++)
		mendicant_traits_population_means(kind, character_strength, character_visibility, character_population_mean[kind]);

	return;
}

/* the settings the draws use now */
static boolean character_settings_take(
	void)
{
	float strength = mendicant_config.personality;
	int visibility = (int)mendicant_config.quirks;
	boolean changed;

	if (!(strength >= 0.0f))
		strength = 0.0f;
	if (strength > 2.0f)
		strength = 2.0f;
	if (visibility < MENDICANT_QUIRKS_OFF || visibility > MENDICANT_QUIRKS_LOUD)
		visibility = MENDICANT_QUIRKS_NORMAL;
	changed = strength != character_strength || visibility != character_visibility;
	character_strength = strength;
	character_visibility = visibility;
	return changed;
}

static void character_store_clear(
	void)
{
	int slot;

	for (slot = 0; slot < MENDICANT_MAX_ACTORS; slot++)
	{
		character_soldiers[slot].actor = NONE;
		character_soldiers[slot].fight = -1;
	}

	return;
}

static void character_fights_clear(
	void)
{
	int index;

	memset(character_fights, 0, sizeof(character_fights));
	for (index = 0; index < MENDICANT_MAX_ENCOUNTERS; index++)
		character_fights[index].quiet_since = -1;
	character_next_fight_check = 0;

	return;
}

static void character_log_soldier(
	long actor_index,
	struct actor_datum *actor,
	struct character_soldier *soldier)
{
	FILE *file = mendicant_character_file;
	int trait;

	if (!file)
		return;
	fprintf(file, "%ld,%lu,soldier,%ld,%d,%d,%s,%s", mendicant_time(), character_epoch, actor_index,
		actor->meta.encounter_index == NONE ? -1 : (int)(short)actor->meta.encounter_index, actor->meta.team_index,
		mendicant_kind_names[soldier->traits.kind], mendicant_quirk_definitions[soldier->traits.quirk].name);
	for (trait = 0; trait < MENDICANT_TRAITS; trait++)
		fprintf(file, ",%.3f", soldier->traits.trait[trait] / 255.0f);
	fprintf(file, ",%.3f,%c,%.3f,%s,%s,,,%016llx\n", soldier->traits.lead_bias,
		soldier->traits.peek_side == MENDICANT_SIDE_LEFT ? 'L' : 'R', soldier->traits.buddy_pref,
		soldier->traits.favourite == MENDICANT_FAVOURITE_SHOTGUN ? "shotgun" :
		soldier->traits.favourite == MENDICANT_FAVOURITE_SNIPER ? "sniper" : "none",
		soldier->traits.seat == MENDICANT_SEAT_DRIVER ? "driver" : soldier->traits.seat == MENDICANT_SEAT_GUNNER ? "gunner" : "any",
		soldier->seed);
	fflush(file);

	return;
}

/* a row with only time, epoch, record, encounter, team, mood, fight and seed */
static void character_log_row(
	char const *record,
	int encounter,
	int team,
	char const *mood,
	int fight,
	unsigned long long seed)
{
	FILE *file = mendicant_character_file;

	if (!file)
		return;
	fprintf(file, "%ld,%lu,%s,,%d,%d,,,,,,,,,,,,,,,,,,%s,%d,%016llx\n", mendicant_time(), character_epoch, record,
		encounter, team, mood, fight, seed);
	fflush(file);

	return;
}

/* the soldier's entry, drawn now if it is not his or is from another epoch;
NULL for no character (or the layer off) */
static struct character_soldier *character_get(
	long actor_index)
{
	struct character_soldier *soldier;
	struct actor_datum *actor;
	int slot = character_slot(actor_index);
	int kind;

	if (slot < 0 || !character_on())
		return NULL;
	soldier = &character_soldiers[slot];
	if (soldier->actor != actor_index || soldier->epoch != character_epoch)
	{
		actor = character_actor_get(actor_index);
		if (!actor)
			return NULL;
		kind = character_kind(actor);
		soldier->actor = actor_index;
		soldier->epoch = character_epoch;
		soldier->seed = character_seed(actor_index, actor, kind);
		mendicant_traits_draw(&soldier->traits, kind, soldier->seed, character_strength, character_visibility);
		soldier->fight = -1;
		soldier->fight_seed = 0;
		memset(soldier->swing, 0, sizeof(soldier->swing));
		soldier->signature_due = FALSE;
		soldier->signature_spent = FALSE;
		if (kind != MENDICANT_KIND_NONE)
		{
			character_drawn++;
			if (soldier->traits.quirk != MENDICANT_QUIRK_NONE)
				character_quirked++;
			character_log_soldier(actor_index, actor, soldier);
		}
	}
	return soldier->traits.kind != MENDICANT_KIND_NONE ? soldier : NULL;
}

/* his encounter's fight, or NULL (no encounter, or none yet) */
static struct character_fight *character_actor_fight(
	long actor_index,
	int *encounter_slot)
{
	struct actor_datum *actor = character_actor_get(actor_index);
	int slot;

	if (!actor || actor->meta.encounter_index == NONE)
		return NULL;
	slot = (int)(unsigned short)actor->meta.encounter_index;
	if (slot >= MENDICANT_MAX_ENCOUNTERS || character_fights[slot].counter == 0)
		return NULL;
	*encounter_slot = slot;
	return &character_fights[slot];
}

/* the soldier's swing and signature for his encounter's current fight */
static void character_fight_refresh(
	long actor_index,
	struct character_soldier *soldier)
{
	int encounter = 0, index;
	struct character_fight *fight = character_actor_fight(actor_index, &encounter);
	long id = fight ? (long)encounter << 16 | fight->counter : 0;
	unsigned long long state;

	if (id == soldier->fight)
		return;
	soldier->fight = id;
	memset(soldier->swing, 0, sizeof(soldier->swing));
	soldier->signature_due = FALSE;
	soldier->signature_spent = FALSE;
	soldier->fight_seed = fight ? fight->seed : 0;
	if (!fight)
		return;
	state = mendicant_mix64(soldier->seed, fight->seed);
	for (index = 0; index < 3; index++)
		soldier->swing[index] = CHARACTER_SWING * character_strength * mendicant_random_triangle(&state);
	soldier->signature_due = soldier->traits.quirk != MENDICANT_QUIRK_NONE &&
		mendicant_random_unit(&state) < mendicant_visibility_signature[character_visibility];

	return;
}

/* the soldier with his fight brought up to date, or NULL */
static struct character_soldier *character_get_fight(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	if (soldier)
		character_fight_refresh(actor_index, soldier);
	return soldier;
}

/* the session's nonce: 64 random bits once, or ai.mendicant.seed's */
static void character_nonce_update(
	void)
{
	long pinned = mendicant_config.seed;

	if (character_nonce_ready && pinned == character_nonce_pinned)
		return;
	if (pinned)
	{
		character_nonce = mendicant_mix64(0xC0FFEE5EED000000ULL, (unsigned long long)(unsigned long)pinned);
	}
	else
	{
		unsigned long long bytes = 0;

		posix_random_bytes(&bytes, (unsigned int)sizeof(bytes));
		/* (never only the clock, never zero) */
		character_nonce = mendicant_mix64(bytes, ((unsigned long long)system_milliseconds() << 16) ^
			(unsigned long long)(unsigned long)(size_t)&bytes);
	}
	character_nonce_ready = TRUE;
	character_nonce_pinned = pinned;

	return;
}

/* ---------- public code: the platform's calls */

void mendicant_character_map_start(
	void)
{
	character_nonce_update();
	character_map_hash = character_text_hash(main_get_map_name());
	character_maps++;
	character_epoch++;
	character_settings_take();
	character_population_update();
	character_store_clear();
	character_fights_clear();
	character_drawn = 0;
	character_quirked = 0;
	/* (the nonce goes in the log, so that a playtest's bug can be replayed:
	ai.mendicant.seed pins it) */
	platform_log("mendicant: character %s, nonce %016llx (%s), epoch %lu, personality %.2f, quirks %s",
		mendicant_config.character ? "on" : "off", character_nonce,
		character_nonce_pinned ? "ai.mendicant.seed" : "random", character_epoch, character_strength,
		character_visibility == MENDICANT_QUIRKS_OFF ? "off" : character_visibility == MENDICANT_QUIRKS_SUBTLE ? "subtle" :
		character_visibility == MENDICANT_QUIRKS_LOUD ? "loud" : "normal");
	if (mendicant_character_file)
		character_log_row("session", -1, -1, mendicant_config.character ? "on" : "off", (int)character_maps, character_nonce);

	/* BUG A: the drill's random numbers started from one constant, so every
	launch played the same "random" drills. In play they now start from the
	session's nonce and the map; training keeps the trainer's seeding. A
	pinned seed gives the same numbers on each start of a map. */
	if (!mendicant_training)
	{
		unsigned long long seed = mendicant_mix64(character_nonce, character_map_hash);

		if (!character_nonce_pinned)
			seed = mendicant_mix64(seed, character_maps);
		mendicant_drill_seed(seed);
		platform_log("mendicant: drill random numbers from %08lx (the session's nonce and %s)",
			(unsigned long)((seed ^ (seed >> 32)) & 0xFFFFFFFFUL), main_get_map_name());
	}

	return;
}

void mendicant_character_revert(
	void)
{
	if (character_drawn && character_on())
		platform_log("mendicant: character epoch %lu: %ld soldiers drawn, %ld with a quirk", character_epoch,
			character_drawn, character_quirked);
	character_epoch++;
	character_fights_clear();
	character_drawn = 0;
	character_quirked = 0;
	if (mendicant_character_file)
		character_log_row("revert", -1, -1, "", 0, character_nonce);

	return;
}

void mendicant_character_settings(
	void)
{
	if (!character_settings_take())
		return;
	/* new spreads or quirks: every soldier is drawn again with them (the
	Marines from the same seeds) */
	character_population_update();
	character_store_clear();

	return;
}

void mendicant_character_dispose(
	void)
{
	if (character_drawn)
		platform_log("mendicant: character epoch %lu: %ld soldiers drawn, %ld with a quirk", character_epoch,
			character_drawn, character_quirked);
	character_store_clear();
	character_fights_clear();
	character_drawn = 0;
	character_quirked = 0;

	return;
}

void mendicant_character_touch(
	long actor_index)
{
	character_get(actor_index);

	return;
}

void mendicant_character_update(
	long time)
{
	short index;

	if (!character_on() || !encounter_data || !encounter_data->valid || time < character_next_fight_check)
		return;
	character_next_fight_check = time + CHARACTER_FIGHT_CHECK_TICKS;
	for (index = 0; index < encounter_data->count && index < MENDICANT_MAX_ENCOUNTERS; index++)
	{
		struct encounter_datum *encounter = (struct encounter_datum *)((byte *)encounter_data->data + index * encounter_data->size);
		struct character_fight *fight = &character_fights[index];
		boolean in_combat = encounter->identifier && encounter->current_in_combat_count > 0;

		if (in_combat && !fight->in_combat)
		{
			/* contact: a new fight unless the last one ended under 20 s ago */
			if (fight->counter == 0 || fight->quiet_since < 0 || time - fight->quiet_since >= CHARACTER_FIGHT_QUIET_TICKS)
			{
				fight->counter++;
				fight->seed = mendicant_mix64(mendicant_mix64(character_nonce, (unsigned long long)character_epoch << 20 | 0xF16u),
					(unsigned long long)index << 16 | fight->counter);
				fight->mood = (unsigned char)(character_strength > 0.0f ? mendicant_mood_draw(fight->seed) : MENDICANT_MOOD_STEADY);
				character_log_row("fight", index, encounter->team_index, mendicant_mood_names[fight->mood], fight->counter,
					fight->seed);
			}
		}
		else if (!in_combat && fight->in_combat)
		{
			fight->quiet_since = time;
		}
		fight->in_combat = in_combat;
	}

	return;
}

unsigned long long mendicant_character_nonce(
	void)
{
	return character_nonce;
}

/* ---------- public code: the API every behaviour reads */

boolean mendicant_soldier_has(
	long actor_index)
{
	return character_get(actor_index) != NULL;
}

int mendicant_soldier_kind(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.kind : MENDICANT_KIND_NONE;
}

int mendicant_soldier_quirk(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.quirk : MENDICANT_QUIRK_NONE;
}

real mendicant_soldier_base(
	long actor_index,
	int trait)
{
	struct character_soldier *soldier = character_get(actor_index);

	if (!soldier || trait < 0 || trait >= MENDICANT_TRAITS)
		return 0.5f;
	return soldier->traits.trait[trait] / 255.0f;
}

real mendicant_soldier_trait(
	long actor_index,
	int trait)
{
	struct character_soldier *soldier = character_get_fight(actor_index);
	real value;
	int index;

	if (!soldier || trait < 0 || trait >= MENDICANT_TRAITS)
		return 0.5f;
	value = soldier->traits.trait[trait] / 255.0f;
	for (index = 0; index < 3; index++)
	{
		if (character_swing_traits[index] == trait)
			value += soldier->swing[index];
	}
	return mendicant_clamp_unit(value);
}

real mendicant_soldier_mult(
	long actor_index,
	int trait,
	real lo,
	real hi)
{
	struct character_soldier *soldier = character_get(actor_index);

	if (!soldier || trait < 0 || trait >= MENDICANT_TRAITS)
		return 1.0f;
	return mendicant_traits_mult(soldier->traits.trait[trait] / 255.0f,
		character_population_mean[soldier->traits.kind][trait], lo, hi);
}

real mendicant_soldier_lead_bias(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.lead_bias : 1.0f;
}

int mendicant_soldier_peek_side(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.peek_side : MENDICANT_SIDE_LEFT;
}

real mendicant_soldier_buddy(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.buddy_pref : 0.5f;
}

int mendicant_soldier_favourite(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.favourite : MENDICANT_FAVOURITE_NONE;
}

int mendicant_soldier_seat(
	long actor_index)
{
	struct character_soldier *soldier = character_get(actor_index);

	return soldier ? soldier->traits.seat : MENDICANT_SEAT_ANY;
}

unsigned long mendicant_soldier_roll(
	long actor_index,
	unsigned long salt)
{
	struct character_soldier *soldier = character_get_fight(actor_index);
	unsigned long long seed;

	if (!soldier)
	{
		/* no character: still the character's own numbers, from the actor alone */
		seed = mendicant_mix64(character_nonce ^ (unsigned long long)(unsigned long)actor_index, salt);
	}
	else
	{
		seed = mendicant_mix64(soldier->seed ^ soldier->fight_seed, salt);
	}
	seed = mendicant_mix64(seed, (unsigned long long)(mendicant_time() / 15));
	return (unsigned long)(seed >> 32);
}

real mendicant_soldier_rollf(
	long actor_index,
	unsigned long salt)
{
	return (real)(mendicant_soldier_roll(actor_index, salt) >> 8) * (1.0f / 16777216.0f);
}

int mendicant_soldier_mood(
	long actor_index)
{
	int encounter = 0;
	struct character_fight *fight;

	if (!character_get(actor_index))
		return MENDICANT_MOOD_STEADY;
	fight = character_actor_fight(actor_index, &encounter);
	return fight ? fight->mood : MENDICANT_MOOD_STEADY;
}

real mendicant_soldier_mood_scale(
	long actor_index,
	int scale)
{
	int mood = mendicant_soldier_mood(actor_index);

	if (scale < 0 || scale >= MENDICANT_MOOD_SCALES)
		return 1.0f;
	/* the personality strength scales the mood's effect too */
	return 1.0f + (mendicant_mood_scale[mood][scale] - 1.0f) * (character_strength < 1.0f ? character_strength : 1.0f);
}

boolean mendicant_soldier_signature_due(
	long actor_index)
{
	struct character_soldier *soldier = character_get_fight(actor_index);

	return soldier && soldier->signature_due && !soldier->signature_spent;
}

void mendicant_soldier_signature_spent(
	long actor_index)
{
	struct character_soldier *soldier = character_get_fight(actor_index);

	if (soldier)
		soldier->signature_spent = TRUE;

	return;
}

real mendicant_soldier_cohesion(
	long actor_index)
{
	int slot = character_slot(actor_index);

	if (slot < 0 || !character_get(actor_index))
		return 1.0f;
	return commanded_time[slot] == mendicant_time() ? CHARACTER_COHESION_COMMANDED : 1.0f;
}
