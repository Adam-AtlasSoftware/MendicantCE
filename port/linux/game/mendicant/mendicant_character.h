/* mendicant_character.h: the soldier character's types (MENDICANT AI,
port/linux/game/mendicant; docs/mendicant_port.md, "The soldier character"):
the traits, kinds, quirks, moods and settings every behaviour names when it
asks mendicant_character.c (mendicant_internal.h declares the calls). Plain
C, included by mendicant_traits.h and tools/mendicant/character_test.c too. */
#ifndef HALO_MENDICANT_CHARACTER_H
#define HALO_MENDICANT_CHARACTER_H

/* ---------- traits */

enum
{
	MENDICANT_TRAIT_COURAGE = 0, /* nerve under fire, panic resistance */
	MENDICANT_TRAIT_AGGRESSION,  /* closes distance, keeps firing on the move */
	MENDICANT_TRAIT_SKILL,       /* hand-eye: aim, grenade arm, driving */
	MENDICANT_TRAIT_DISCIPLINE,  /* follows orders, controls bursts */
	MENDICANT_TRAIT_CAUTION,     /* values cover and distance */
	MENDICANT_TRAIT_GRENADE,     /* appetite for throwing */
	MENDICANT_TRAIT_INITIATIVE,  /* how fast he acts on a new order or chance */
	MENDICANT_TRAIT_VOCAL,       /* how much he talks */
	MENDICANT_TRAIT_LOYALTY,     /* bond to the Chief or his leader */
	MENDICANT_TRAIT_REFLEX,      /* perception and reaction speed */
	MENDICANT_TRAITS
};

/* ---------- kinds: species by rank, from the actor's type and its variant's name */

enum
{
	MENDICANT_KIND_NONE = 0, /* no character: Hunters, Flood, Sentinels, Engineers, turrets */
	MENDICANT_KIND_GRUNT_MINOR,
	MENDICANT_KIND_GRUNT_MAJOR,
	MENDICANT_KIND_GRUNT_SPECOPS,
	MENDICANT_KIND_GRUNT_FLEE, /* c10's scripted cowards: locked, never braver */
	MENDICANT_KIND_JACKAL_MINOR,
	MENDICANT_KIND_JACKAL_MAJOR,
	MENDICANT_KIND_ELITE_MINOR,
	MENDICANT_KIND_ELITE_MAJOR,
	MENDICANT_KIND_ELITE_COMMANDER,
	MENDICANT_KIND_ELITE_SPECOPS,
	MENDICANT_KIND_ELITE_STEALTH,
	MENDICANT_KIND_MARINE,
	MENDICANT_KIND_MARINE_ARMORED, /* armored Marines and Marine majors */
	MENDICANT_KIND_SERGEANT,       /* a Marine squad's unique leader */
	MENDICANT_KINDS
};

/* the quirks' groups */
enum
{
	MENDICANT_GROUP_NONE = 0,
	MENDICANT_GROUP_GRUNT,
	MENDICANT_GROUP_JACKAL,
	MENDICANT_GROUP_ELITE,
	MENDICANT_GROUP_MARINE,
	MENDICANT_GROUPS
};

/* ---------- quirks (archetypes), each with its signature beat */

enum
{
	MENDICANT_QUIRK_NONE = 0,
	/* Grunts */
	MENDICANT_QUIRK_COWARD,        /* breaks first, screaming the flee line */
	MENDICANT_QUIRK_BRAVE,         /* holds when his Elite falls, lobs a grenade, shouts */
	MENDICANT_QUIRK_PANIC_THROWER, /* a wild grenade when shaken */
	/* Jackals */
	MENDICANT_QUIRK_SHARPSHOOTER,  /* holds his aim on the corner you left by */
	MENDICANT_QUIRK_SKITTISH,      /* backs off with his shield up at the first hit */
	MENDICANT_QUIRK_CHARGER,       /* rushes in behind his shield */
	/* Elites */
	MENDICANT_QUIRK_DUELIST,       /* comes for the Chief alone, no grenade at a lone foe */
	MENDICANT_QUIRK_TACTICIAN,     /* a flushing grenade, then the flank call */
	MENDICANT_QUIRK_BERSERKER,     /* berserks when a fellow Elite dies */
	/* Marines */
	MENDICANT_QUIRK_VETERAN,       /* a calm callout and one well-placed grenade */
	MENDICANT_QUIRK_ROOKIE,        /* pinned and yelling at first contact */
	MENDICANT_QUIRK_HOTHEAD,       /* pushes ahead of the squad, shouting */
	MENDICANT_QUIRK_JOKER,         /* the banter after a fight */
	MENDICANT_QUIRK_PACKRAT,       /* grabs a dropped weapon after a fight */
	MENDICANT_QUIRKS
};

/* ---------- the fight's mood: one per encounter fight */

enum
{
	MENDICANT_MOOD_STEADY = 0,
	MENDICANT_MOOD_JITTERY,
	MENDICANT_MOOD_FIRED_UP,
	MENDICANT_MOOD_SPARING,
	MENDICANT_MOOD_LIBERAL,
	MENDICANT_MOODS
};

/* what a mood scales: aim error, the grenade lull, spacing, reaction time */
enum
{
	MENDICANT_MOOD_AIM = 0,
	MENDICANT_MOOD_GRENADE_LULL,
	MENDICANT_MOOD_SPACING,
	MENDICANT_MOOD_REACTION,
	MENDICANT_MOOD_SCALES
};

/* quirk visibility (ai.mendicant.quirks) */
enum
{
	MENDICANT_QUIRKS_OFF = 0,
	MENDICANT_QUIRKS_SUBTLE,
	MENDICANT_QUIRKS_NORMAL,
	MENDICANT_QUIRKS_LOUD,
	MENDICANT_QUIRK_VISIBILITIES
};

/* the per-soldier constants */
enum { MENDICANT_SIDE_LEFT = 0, MENDICANT_SIDE_RIGHT = 1 };
enum { MENDICANT_FAVOURITE_NONE = 0, MENDICANT_FAVOURITE_SHOTGUN, MENDICANT_FAVOURITE_SNIPER };
enum { MENDICANT_SEAT_ANY = 0, MENDICANT_SEAT_DRIVER, MENDICANT_SEAT_GUNNER };

/* a drawn character: what the store keeps per soldier besides its identity */
struct mendicant_traits
{
	unsigned char kind, quirk;
	unsigned char trait[MENDICANT_TRAITS]; /* 0..255 for 0..1 */
	unsigned char peek_side, favourite, seat;
	float lead_bias;  /* 0.6..1.25: how far ahead he leads; about 1 in 10 over-leads (> 1.1) */
	float buddy_pref; /* 0..1: how much he likes working in a pair */
};

#endif
