/*
FORGE_TAG_IMPORT.C

Forge's tags, copied into a Forge game's level from Forge's tag pack
(maps/openh1_forge.pack, port/assets/tag_packs/openh1_forge.toml) by
Watanoge's tag importer (tag_import.c, level_tag_import.c): the monitor of
The Maw (c40), which a builder flies as (forge_monitor.c), and some of The
Silent Cartographer's (b30) characters, which Forge's AI tab places
(forge_ai.c lists the actor variants the level has).

SaftiGlas's Forge brought the monitor in with an importer of its own, which
read The Maw itself as each Forge map loaded; Watanoge's reads a pack the
main menu wrote once from the player's maps instead, and copies the same
tags at the same indices on every machine that asks alike. What is copied
depends on the game's variant alone (game_variant_set_is_forge), never on a
setting of one machine, so every machine of a Forge game has the same tag
table; a machine without the pack does not join one
(network_client_manager.c's forge check).
*/

#include "cseries.h"

#include "halo_forge.h"
#include "tag_import.h"
#include "tag_pack.h"

/* ---------- constants */

#define FORGE_TAG_PACK "openh1_forge"
#define FORGE_MONITOR_MAP "c40"
#define FORGE_CHARACTERS_MAP "b30"

/* ---------- globals */

/* the monitor (forge_monitor.c's FORGE_MONITOR_TAG_NAME) */
static struct tag_import_root const forge_monitor_roots[] =
{
	{ 'bipd', "characters\\monitor\\monitor" },
};

/* the characters Forge's AI tab offers on a map without its own: actor
variants, each with its actor, biped, weapon and what they name. The
Hunters and the Flood are left to the campaign. */
static struct tag_import_root const forge_character_roots[] =
{
	{ 'actv', "characters\\elite\\elite minor\\elite minor plasma rifle" },
	{ 'actv', "characters\\elite\\elite major\\elite major plasma rifle" },
	{ 'actv', "characters\\grunt\\grunt minor plasma pistol" },
	{ 'actv', "characters\\grunt\\grunt major needler" },
	{ 'actv', "characters\\jackal\\jackal minor plasma pistol" },
	{ 'actv', "characters\\marine_armored\\marine_armored assault rifle" },
};

/* ---------- public code */

/* whether the loading level gets Forge's tags: a Forge game's, with the
pack written (level_tag_import.c) */
boolean forge_tag_import_wanted(
	void)
{
	return forge_monitor_import_wanted() && tag_pack_ready(FORGE_TAG_PACK);
}

/* Forge's roots, between the level's tag_import_begin and tag_import_run
(level_tag_import.c), in a fixed order */
void forge_tag_import_roots(
	void)
{
	tag_import_roots(FORGE_TAG_PACK, FORGE_MONITOR_MAP, forge_monitor_roots, (int)NUMBEROF(forge_monitor_roots));
	tag_import_roots(FORGE_TAG_PACK, FORGE_CHARACTERS_MAP, forge_character_roots,
		(int)NUMBEROF(forge_character_roots));

	return;
}

/* whether this machine has Forge's pack, to play a Forge game; asked for
when it has not (network_client_manager.c, menu_functions.c) */
boolean forge_tag_pack_ready(
	boolean request)
{
	if (tag_pack_ready(FORGE_TAG_PACK))
		return TRUE;
	if (request)
		tag_pack_request(FORGE_TAG_PACK);
	return FALSE;
}
