"""display.hdr's shared contract (port/linux/src/xgpu_hdr.h, xgpu_hdr.c,
docs/hdr.md): the classification of the draws into the HDR back buffer, the
reference tone map and encoding the backends' shaders are held to (the Metal
backend's on the GPU: port/macos/tests/xgpu_metal_test.m, test_stream_hdr),
and the shaders' GLSL against the reference's constants. Compiled from the
sources with the host's compiler, beside a stand-in for the platform header,
as the macOS host builds the shared renderer files (tools/macos_build.py)."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src"
COMPILER = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")

PLATFORM = """#include <stddef.h>
#include <stdint.h>
typedef int BOOL;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef DWORD D3DCOLOR;
#define TRUE 1
#define FALSE 0
#define D3DRS_PS_MAX 47
"""

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "xgpu_backend.h"
#define XGPU_HDR_REFERENCE
#include "xgpu_hdr.h"

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { failures++; printf("FAIL line %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned short classify(int blend, int source, int destination, int operation, int mask, int category,
	int interface, int kind, int mode)
{
	struct xgpu_hdr_draw draw;

	memset(&draw, 0, sizeof(draw));
	draw.blend = blend;
	draw.blend_source = source;
	draw.blend_destination = destination;
	draw.blend_operation = operation;
	draw.color_mask = mask;
	draw.category = category;
	draw.interface = interface;
	draw.kind = kind;
	draw.mode = mode;
	return xgpu_hdr_classify(&draw);
}

#define RGB (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE)
#define ALL (RGB | XGPU_WRITE_ALPHA)
#define ADD XGPU_BLEND_ADD
#define F XGPU_HDR_MODE_FAITHFUL
#define P XGPU_HDR_MODE_PUNCHY

int main(void)
{
	static const float peaks[4] = { 1.0f, 1.6f, 4.0f, 16.0f };
	unsigned short tag;
	int mode, peak, step;

	/* the classification: what keeps its light past white */
	CHECK(classify(0, 0, 0, 0, ALL, 2, 0, 0, F) == 0, "an opaque draw is untagged");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0, 0, F) == 0,
		"an alpha blend cannot leave [0, 1]: untagged");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 22, 0, 0, F) == 0,
		"an additive effect keeps its light (no ceiling)");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 24, 0, 0, F) == 0, "a lens flare's light too");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 5, 0, 0, F) == 0, "the dynamic lights' too");
	tag = classify(1, XGPU_BLEND_DESTINATION_COLOR, XGPU_BLEND_SOURCE_COLOR, ADD, ALL, 9, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE && (tag & XGPU_HDR_ALPHA_LIMIT) &&
		XGPU_HDR_BLEND_SOURCE(tag) == XGPU_BLEND_DESTINATION_COLOR &&
		XGPU_HDR_BLEND_DESTINATION(tag) == XGPU_BLEND_SOURCE_COLOR, "a double multiply decal stops at paper white, "
		"its alpha at 1, blended in the shader: %04x", tag);
	tag = classify(1, XGPU_BLEND_DESTINATION_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 15, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "Faithful: the reflections stop at paper white");
	CHECK(classify(1, XGPU_BLEND_DESTINATION_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 15, 0, 0, P) == 0,
		"Punchy: the reflections keep their light");
	tag = classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 22, 0, HALO_HDR_KIND_SPECULAR, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "Faithful: glass's reflection stops at paper white");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 27, 1, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE, "the HUD's additive art stops at paper white");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, -1, 1, 0, F) == 0,
		"the menus' alpha blend is untagged");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 2, 0, HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_NUMERIC, F);
	CHECK(XGPU_HDR_GLOW(tag) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_WEAPON,
		"the assault rifle's ammunition counter glows to the weapon's ceiling: %04x", tag);
	tag = classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0,
		HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_NUMERIC, F);
	CHECK(XGPU_HDR_GLOW(tag) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_WEAPON, "... whatever its blend");
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 22, 0,
		HALO_HDR_KIND_FIRST_PERSON, F) == 0, "a first-person part's alpha blend is no glow");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 27, 1, HALO_HDR_KIND_HUD_GLOW, F);
	CHECK(XGPU_HDR_GLOW(tag) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_HUD, "the motion tracker glows to "
		"the HUD's ceiling: %04x", tag);
	/* the light of the world: a surface that is light (a teleporter's
	field, a light fixture, an energy shield, a bolt's volume) glows to its
	ceiling where it is additive, in Faithful and Punchy alike */
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 22, 0, HALO_HDR_KIND_LIGHT, F);
	CHECK(XGPU_HDR_GLOW(tag) && XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_LIGHT &&
		XGPU_HDR_BLEND_SOURCE(tag) == XGPU_BLEND_ONE && XGPU_HDR_BLEND_DESTINATION(tag) == XGPU_BLEND_ONE,
		"a teleporter's field glows to the light's ceiling, blended in the shader: %04x", tag);
	tag = classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE, ADD, RGB, 22, 0, HALO_HDR_KIND_LIGHT, P);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_LIGHT, "a plasma shield's or a bolt's volume too: %04x", tag);
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, ALL, 16, 0, HALO_HDR_KIND_LIGHT, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_LIGHT && !(tag & XGPU_HDR_ALPHA_LIMIT),
		"an alpha multiply add too, its alpha (the over operator's) under 1 as it was: %04x", tag);
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 16, 0, HALO_HDR_KIND_LIGHT,
		F) == 0, "a light's alpha blend is no glow (it cannot leave [0, 1])");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_MAX, RGB, 16, 0, HALO_HDR_KIND_LIGHT, F) == 0,
		"nor its maximum (a hologram's)");
	CHECK(XGPU_HDR_CEILING_OF(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 27, 1, HALO_HDR_KIND_LIGHT, F)) ==
		XGPU_HDR_CEILING_PAPER_WHITE, "nor anything over the tone mapped view");
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, 0, 22, 0, HALO_HDR_KIND_LIGHT, F) == 0,
		"nor a draw writing no colour");
	CHECK(XGPU_HDR_CEILING_LIGHT_VALUE > XGPU_HDR_CEILING_WEAPON_VALUE && XGPU_HDR_CEILING(XGPU_HDR_CEILING_LIGHT) <
		XGPU_HDR_ALPHA_LIMIT && !(XGPU_HDR_CEILING(7) & (XGPU_HDR_BLEND(15, 15, 7) | XGPU_HDR_ALPHA_LIMIT |
		XGPU_HDR_LIGHTS)), "the ceilings' field is its own bits");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, XGPU_BLEND_REVERSE_SUBTRACT, RGB, 22, 0, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) != 0, "a subtraction is kept from going below 0");
	tag = classify(1, XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR, XGPU_BLEND_ZERO, ADD, RGB, 28, 1, 0, F);
	CHECK(XGPU_HDR_CEILING_OF(tag) != 0, "one less a destination past white is kept from going below 0");
	tag = classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, ALL, 22, 0, 0, F);
	CHECK(tag == (XGPU_HDR_ALPHA_LIMIT | XGPU_HDR_BLEND(XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD)),
		"an additive draw writing alpha keeps alpha at most 1, its colour unlimited: %04x", tag);
	CHECK(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, ALL, 22, 0, 0, F) == 0,
		"the over operator's alpha cannot pass 1");
	/* the screen effects' passes copy the tone mapped view through their
	target and back (a scope's warp and mask): its light past white kept */
	CHECK(classify(0, 0, 0, 0, RGB, 26, 1, 0, F) == XGPU_HDR_UNCLAMPED,
		"a screen effect's copy of the view is unclamped");
	CHECK(classify(1, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, XGPU_BLEND_ZERO, ADD, RGB, 26, 1, 0, F) == XGPU_HDR_UNCLAMPED,
		"... and its last pass, blended by its mask");
	CHECK(XGPU_HDR_CEILING_OF(classify(1, XGPU_BLEND_ONE, XGPU_BLEND_ONE, ADD, RGB, 26, 1, 0, F)) ==
		XGPU_HDR_CEILING_PAPER_WHITE, "a screen effect adding light stops at paper white");
	CHECK(classify(0, 0, 0, 0, RGB, 27, 1, 0, F) == 0, "the HUD's opaque draws are untagged");
	CHECK(!XGPU_HDR_SHADER_BLENDS(XGPU_HDR_UNCLAMPED) && !XGPU_HDR_GLOW(XGPU_HDR_UNCLAMPED) &&
		XGPU_HDR_BLEND_OPERATION(XGPU_HDR_UNCLAMPED) > XGPU_BLEND_MAX, "unclamped is no blend a draw has, "
		"blended by the fixed function");
	/* a self-illuminated surface's pass (a door's panel, a screen: a
	model's, a structure's lightmap pass): its self-illumination glows, in
	Faithful and Punchy alike, the rest as the SDR frame has it */
	tag = classify(0, 0, 0, 0, RGB, 2, 0, HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(1 << 1), F);
	CHECK(tag == XGPU_HDR_SELF_ILLUMINATION(1 << 1) && XGPU_HDR_IS_SELF_ILLUMINATED(tag) &&
		XGPU_HDR_SELF_ILLUMINATION_STAGES(tag) == (1 << 1), "a model's self-illumination (stage 1's colour): %04x", tag);
	CHECK(!XGPU_HDR_SHADER_BLENDS(tag) && !XGPU_HDR_GLOW(tag) && !XGPU_HDR_IS_UNCLAMPED(tag),
		"blended by the fixed function, no glow's ceiling, not unclamped");
	CHECK(classify(0, 0, 0, 0, ALL, 1, 0, HALO_HDR_KIND_SELF_ILLUMINATION |
		HALO_HDR_KIND_SELF_ILLUMINATION_STAGES((1 << 3) | (1 << 4)), P) == XGPU_HDR_SELF_ILLUMINATION((1 << 3) | (1 << 4)),
		"a structure's lightmap pass (stages 3 and 4's colours), in Punchy too");
	{
		struct xgpu_hdr_draw draw;

		memset(&draw, 0, sizeof(draw));
		draw.color_mask = ALL;
		draw.category = 1;
		draw.kind = HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(0x18);
		draw.dynamic_lights = 1;
		tag = xgpu_hdr_classify(&draw);
		CHECK(tag == (XGPU_HDR_SELF_ILLUMINATION(0x18) | XGPU_HDR_LIGHTS) && XGPU_HDR_IS_SELF_ILLUMINATED(tag),
			"with the enhanced dynamic lights' shoulder too: %04x", tag);
	}
	CHECK(classify(1, XGPU_BLEND_SOURCE_ALPHA, XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA, ADD, RGB, 2, 0,
		HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(2), F) == XGPU_HDR_SELF_ILLUMINATION(2),
		"an alpha blended model's too");
	tag = classify(1, XGPU_BLEND_DESTINATION_COLOR, XGPU_BLEND_SOURCE_COLOR, ADD, RGB, 2, 0,
		HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(2), F);
	CHECK(XGPU_HDR_CEILING_OF(tag) == XGPU_HDR_CEILING_PAPER_WHITE && !XGPU_HDR_IS_SELF_ILLUMINATED(tag),
		"a double multiply keeps its ceiling: %04x", tag);
	CHECK(classify(0, 0, 0, 0, RGB, 27, 1, HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(2), F) ==
		0, "not over the tone mapped view");
	CHECK(classify(0, 0, 0, 0, XGPU_WRITE_ALPHA, 2, 0, HALO_HDR_KIND_SELF_ILLUMINATION |
		HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(2), F) == 0, "nor a draw writing no colour");
	CHECK(XGPU_HDR_BLEND_OPERATION(XGPU_HDR_SELF_ILLUMINATION(0xff)) > XGPU_BLEND_MAX &&
		XGPU_HDR_SELF_ILLUMINATION(0xff) != XGPU_HDR_UNCLAMPED &&
		!(XGPU_HDR_SELF_ILLUMINATION(0xff) & (XGPU_HDR_CEILING(7) | XGPU_HDR_ALPHA_LIMIT | XGPU_HDR_LIGHTS)),
		"self-illumination is no blend a draw has, its stages their own bits");

	{
		/* display.hdr_highlight_boost (#1123): at 0 (the default) every tag
		as it was, an emitter's draw untagged and a self-illuminated pass's
		tag its own; above 0 the level in the self-illumination's tag, and an
		emitter's additive draw tagged with it and its bound (the lights',
		the first-person weapon's), blended by the fixed function; never the
		HUD's or the menus', nor an emitter's alpha blend */
		struct xgpu_hdr_draw draw;
		int level, si_kind = HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(1 << 1);

		memset(&draw, 0, sizeof(draw));
		draw.color_mask = RGB;
		draw.category = 2;
		draw.kind = si_kind;
		draw.boost = 0;
		CHECK(xgpu_hdr_classify(&draw) == XGPU_HDR_SELF_ILLUMINATION(1 << 1), "boost 0: self-illumination's tag as it was");
		draw.blend = 1;
		draw.blend_source = XGPU_BLEND_SOURCE_ALPHA;
		draw.blend_destination = XGPU_BLEND_ONE;
		draw.blend_operation = ADD;
		draw.category = 22;
		draw.kind = HALO_HDR_KIND_EMITTER;
		CHECK(xgpu_hdr_classify(&draw) == 0, "boost 0: an emitter untagged");
		draw.kind = HALO_HDR_KIND_EMITTER_FIRST_PERSON;
		CHECK(xgpu_hdr_classify(&draw) == 0, "boost 0: a muzzle flash untagged");
		for (level = 1; level <= XGPU_HDR_BOOST_MOST; level++)
		{
			unsigned short tag;

			draw.boost = level;
			draw.kind = HALO_HDR_KIND_EMITTER;
			tag = xgpu_hdr_classify(&draw);
			CHECK(tag == XGPU_HDR_EMITTER(level, XGPU_HDR_CEILING_LIGHT) && XGPU_HDR_IS_EMITTER(tag) &&
				XGPU_HDR_BOOST_OF(tag) == level && XGPU_HDR_EMITTER_BOUND(tag) == XGPU_HDR_CEILING_LIGHT &&
				!XGPU_HDR_SHADER_BLENDS(tag) && !XGPU_HDR_GLOW(tag) && !XGPU_HDR_IS_UNCLAMPED(tag) &&
				!XGPU_HDR_IS_SELF_ILLUMINATED(tag), "level %d: an emitter's tag %04x", level, tag);
			draw.kind = HALO_HDR_KIND_EMITTER_FIRST_PERSON;
			tag = xgpu_hdr_classify(&draw);
			CHECK(XGPU_HDR_IS_EMITTER(tag) && XGPU_HDR_EMITTER_BOUND(tag) == XGPU_HDR_CEILING_WEAPON,
				"level %d: a muzzle flash bound by the weapon's ceiling: %04x", level, tag);
			draw.interface = 1;
			draw.category = 27;
			CHECK(!XGPU_HDR_IS_EMITTER(xgpu_hdr_classify(&draw)) && !XGPU_HDR_BOOST_OF(xgpu_hdr_classify(&draw)),
				"level %d: not over the tone mapped view", level);
			draw.interface = 0;
			draw.category = 22;
			draw.blend_destination = XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA;
			CHECK(xgpu_hdr_classify(&draw) == 0, "level %d: an emitter's alpha blend untagged", level);
			draw.blend_destination = XGPU_BLEND_ONE;
		}
		draw.blend = 0;
		draw.category = 2;
		draw.kind = si_kind;
		draw.boost = 3;
		CHECK(xgpu_hdr_classify(&draw) == (XGPU_HDR_SELF_ILLUMINATION(1 << 1) | XGPU_HDR_BOOST(3)) &&
			XGPU_HDR_IS_SELF_ILLUMINATED(xgpu_hdr_classify(&draw)) && !XGPU_HDR_SHADER_BLENDS(xgpu_hdr_classify(&draw)) &&
			XGPU_HDR_SELF_ILLUMINATION_STAGES(xgpu_hdr_classify(&draw)) == (1 << 1) &&
			XGPU_HDR_BOOST_OF(xgpu_hdr_classify(&draw)) == 3, "level 3: self-illumination's tag carries it");
		draw.boost = 9;
		CHECK(XGPU_HDR_BOOST_OF(xgpu_hdr_classify(&draw)) == XGPU_HDR_BOOST_MOST, "a level past the most, the most");
		CHECK(!XGPU_HDR_SHADER_BLENDS(XGPU_HDR_EMITTER(7, 7)) && XGPU_HDR_EMITTER(7, 7) != XGPU_HDR_UNCLAMPED &&
			!(XGPU_HDR_EMITTER(7, 7) & (XGPU_HDR_ALPHA_LIMIT | XGPU_HDR_LIGHTS)) && XGPU_HDR_BOOST_OF(XGPU_HDR_UNCLAMPED) == 0,
			"the emitter's tag no blend a draw has, apart from unclamped's");
	}
	{
		/* (#1123's macros: XGPU_HDR_SHADER_BLENDS, _GLOW and
		_IS_SELF_ILLUMINATED, which now leave out the blend operations 6 and
		7, give what they gave before, as before's macros below, for every
		tag the classification makes at level 0: each blend (on, off; every
		factor and operation), colour mask, category, kind, mode, the 3D
		view and the HUD's, the dynamic lights; and with HDR off (SDR) no
		draw is tagged at all: d3d8_device.c's hdr_tag) */
		static const int categories[] = { -1, 1, 2, 5, 9, 12, 13, 14, 15, 16, 17, 18, 19, 22, 24, 26, 27, 28 };
		static const int kinds[] = { 0, HALO_HDR_KIND_FIRST_PERSON, HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_NUMERIC,
			HALO_HDR_KIND_FIRST_PERSON | HALO_HDR_KIND_METER, HALO_HDR_KIND_SPECULAR, HALO_HDR_KIND_HUD_GLOW,
			HALO_HDR_KIND_LIGHT, HALO_HDR_KIND_SELF_ILLUMINATION | HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(0xff),
			HALO_HDR_KIND_EMITTER, HALO_HDR_KIND_EMITTER_FIRST_PERSON };
		static const int masks[] = { 0, RGB, XGPU_WRITE_ALPHA, ALL };
		struct xgpu_hdr_draw draw;
		long tags = 0, differ = 0;
		int on, source, destination, operation, mask, category, kind, interface, mode, lights;

#define OLD_SHADER_BLENDS(hdr) (XGPU_HDR_CEILING_OF(hdr) != 0 || ((hdr) & XGPU_HDR_ALPHA_LIMIT) != 0)
#define OLD_GLOW(hdr) (XGPU_HDR_CEILING_OF(hdr) >= XGPU_HDR_CEILING_WEAPON)
#define OLD_IS_SELF_ILLUMINATED(hdr) (!OLD_SHADER_BLENDS(hdr) && XGPU_HDR_BLEND_OPERATION(hdr) == 6)
		memset(&draw, 0, sizeof(draw));
		for (on = 0; on < 2; on++)
		for (source = 0; source <= XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA; source++)
		for (destination = 0; destination <= XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA; destination++)
		for (operation = XGPU_BLEND_ADD; operation <= XGPU_BLEND_MAX; operation++)
		for (mask = 0; mask < 4; mask++)
		for (category = 0; category < (int)(sizeof(categories) / sizeof(categories[0])); category++)
		for (kind = 0; kind < (int)(sizeof(kinds) / sizeof(kinds[0])); kind++)
		for (interface = 0; interface < 2; interface++)
		for (mode = F; mode <= P; mode++)
		for (lights = 0; lights < 2; lights++)
		{
			unsigned short tag;

			draw.blend = on;
			draw.blend_source = source;
			draw.blend_destination = destination;
			draw.blend_operation = operation;
			draw.color_mask = masks[mask];
			draw.category = categories[category];
			draw.kind = kinds[kind];
			draw.interface = interface;
			draw.mode = mode;
			draw.dynamic_lights = lights;
			draw.boost = 0;
			tag = xgpu_hdr_classify(&draw);
			tags++;
			if (XGPU_HDR_SHADER_BLENDS(tag) != OLD_SHADER_BLENDS(tag) || XGPU_HDR_GLOW(tag) != OLD_GLOW(tag) ||
				XGPU_HDR_IS_SELF_ILLUMINATED(tag) != OLD_IS_SELF_ILLUMINATED(tag) || XGPU_HDR_BOOST_OF(tag) ||
				XGPU_HDR_IS_EMITTER(tag))
				differ++;
		}
		CHECK(!differ && tags > 1000000, "level 0: %ld of %ld tags read otherwise by the new macros", differ, tags);
	}
	{
		/* the gain: level 0 nothing, bit for bit; each level a quarter more
		of the emitter's light, its hue kept, its brightest no further than
		its ceiling (the lights' 3 times paper white's light, the weapon's
		1.75), nor below what it was where it is past it already; and a
		self-illuminated panel's light past paper white, toward its own
		ceiling, its lit surface as it was */
		float dim[3] = { 0.4f, 0.2f, 0.1f }, same[3] = { 0.4f, 0.2f, 0.1f }, bright[3] = { 1.0f, 0.5f, 0.2f },
			flash[3] = { 1.0f, 0.5f, 0.2f }, past[3] = { 2.0f, 1.0f, 0.5f };
		float base[3] = { 0.25f, 0.25f, 0.28f }, screen[3] = { 0.45f, 0.95f, 1.0f }, plain[3] = { 0.45f, 0.95f, 1.0f },
			glyph[3] = { 0.30f, 0.42f, 0.35f }, glyph_plain[3] = { 0.30f, 0.42f, 0.35f },
			wall[3] = { 0.25f, 0.25f, 0.28f };
		int level;

		xgpu_hdr_boost_emitter(same, 0, XGPU_HDR_CEILING_LIGHT);
		CHECK(same[0] == 0.4f && same[1] == 0.2f && same[2] == 0.1f, "level 0: nothing");
		xgpu_hdr_boost_emitter(dim, 2, XGPU_HDR_CEILING_LIGHT);
		CHECK(fabsf(dim[0] - 0.6f) < 1e-6f && fabsf(dim[1] - 0.3f) < 1e-6f && fabsf(dim[2] - 0.15f) < 1e-6f,
			"level 2: half again: %.4f %.4f %.4f", dim[0], dim[1], dim[2]);
		xgpu_hdr_boost_emitter(bright, 4, XGPU_HDR_CEILING_LIGHT);
		CHECK(fabsf(bright[0] - XGPU_HDR_CEILING_LIGHT_VALUE) < 1e-5f && fabsf(bright[1] / bright[0] - 0.5f) < 1e-5f,
			"level 4: to the lights' ceiling, its hue kept: %.4f %.4f", bright[0], bright[1]);
		xgpu_hdr_boost_emitter(flash, 4, XGPU_HDR_CEILING_WEAPON);
		CHECK(fabsf(flash[0] - XGPU_HDR_CEILING_WEAPON_VALUE) < 1e-5f, "a muzzle flash to the weapon's: %.4f", flash[0]);
		xgpu_hdr_boost_toward(past, (const float[3]){ 0.0f, 0.0f, 0.0f }, 4, XGPU_HDR_CEILING_LIGHT_VALUE);
		CHECK(past[0] == 2.0f && past[1] == 1.0f, "past the ceiling already: as it was");
		xgpu_hdr_self_illuminate(plain, base);
		xgpu_hdr_self_illuminate_boost(screen, base, 0);
		CHECK(!memcmp(screen, plain, sizeof(screen)), "self-illumination at level 0 as it was");
		xgpu_hdr_self_illuminate_boost(glyph, base, 4);
		xgpu_hdr_self_illuminate(glyph_plain, base);
		CHECK(glyph[1] > glyph_plain[1], "a dim glyph brighter: %.4f, %.4f", glyph[1], glyph_plain[1]);
		for (level = 1; level <= XGPU_HDR_BOOST_MOST; level++)
		{
			float rgb[3] = { 0.45f, 0.95f, 1.0f }, lit[3] = { 0.25f, 0.25f, 0.28f };

			xgpu_hdr_self_illuminate_boost(rgb, base, level);
			xgpu_hdr_self_illuminate_boost(lit, base, level);
			CHECK(fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])) <= XGPU_HDR_SELF_ILLUMINATION_VALUE + 1e-5f &&
				rgb[2] >= plain[2], "level %d: a screen within its ceiling: %.4f", level, rgb[2]);
			CHECK(lit[0] == wall[0] && lit[1] == wall[1] && lit[2] == wall[2], "level %d: the lit wall as it was", level);
		}
	}
	/* the reference: Faithful leaves under white as it is, and stays
	under the peak, rising with the light, past it */
	for (mode = F; mode <= P; mode++)
	{
		for (peak = 0; peak < 4; peak++)
		{
			float last = 0.0f;

			for (step = 0; step <= 400; step++)
			{
				float value = (float)step / 100.0f;
				float rgb[3] = { value, value * 0.8f, value * 0.5f };
				float most;

				xgpu_hdr_tone_map(rgb, mode, peaks[peak]);
				most = xgpu_hdr_linear(rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]));
				CHECK(most <= peaks[peak] * 1.0001f, "%s at peak %.1f: %.3f stays under the peak (%.4f)",
					mode == F ? "faithful" : "punchy", peaks[peak], value, most);
				CHECK(most >= last - 1.0e-4f, "%s at peak %.1f: brighter with more light (%.3f: %.4f after %.4f)",
					mode == F ? "faithful" : "punchy", peaks[peak], value, most, last);
				if (mode == F && value <= 1.0f)
					CHECK(rgb[0] == value && rgb[1] == value * 0.8f && rgb[2] == value * 0.5f,
						"faithful at peak %.1f leaves %.3f as it is", peaks[peak], value);
				last = most;
			}
		}
	}
	{
		/* Punchy lifts the scene: a mid grey brighter, keeping its hue */
		float grey[3] = { 0.5f, 0.5f, 0.5f }, colour[3] = { 0.2f, 0.4f, 0.8f };

		xgpu_hdr_tone_map(grey, P, 4.0f);
		xgpu_hdr_tone_map(colour, P, 4.0f);
		CHECK(grey[0] > 0.5f && grey[0] == grey[1] && grey[1] == grey[2], "punchy lifts a grey: %.4f", grey[0]);
		CHECK(xgpu_hdr_linear(colour[1]) / xgpu_hdr_linear(colour[2]) > 0.99f * xgpu_hdr_linear(0.4f) / xgpu_hdr_linear(0.8f) &&
			xgpu_hdr_linear(colour[1]) / xgpu_hdr_linear(colour[2]) < 1.01f * xgpu_hdr_linear(0.4f) / xgpu_hdr_linear(0.8f),
			"punchy keeps a colour's hue");
	}
	{
		/* a light far past white is white-hot, as the SDR frame has it */
		float core[3] = { 3.0f, 2.5f, 1.5f };

		xgpu_hdr_tone_map(core, F, 4.0f);
		CHECK(core[0] > 1.0f && core[1] > 1.0f && core[2] > 1.0f && core[0] == core[1] && core[1] == core[2],
			"faithful: an explosion's core white-hot, brighter than paper white: %.3f %.3f %.3f", core[0], core[1],
			core[2]);
	}
	{
		/* the encoding: paper white times the light, at most the peak; PQ's
		paper white at its nits */
		struct xgpu_hdr_frame edr = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 1.25f, 2.0f };
		struct xgpu_hdr_frame pq = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 4.0f };
		float white[3] = { 1.0f, 1.0f, 1.0f }, bright[3] = { 4.0f, 4.0f, 4.0f }, pq_white[3] = { 1.0f, 1.0f, 1.0f };

		xgpu_hdr_encode(white, &edr);
		xgpu_hdr_encode(bright, &edr);
		xgpu_hdr_encode(pq_white, &pq);
		CHECK(white[0] > 1.2499f && white[0] < 1.2501f, "EDR: paper white at its scale: %.5f", white[0]);
		CHECK(bright[0] > 2.4999f && bright[0] < 2.5001f, "EDR: at most the peak times paper white: %.5f", bright[0]);
		CHECK(pq_white[0] > 0.5805f && pq_white[0] < 0.5809f, "PQ: 203 nits is 0.5807: %.5f", pq_white[0]);
	}
	{
		/* the UI's white (display.hdr_ui_nits): paper white's by default (0),
		exactly, the encoding as it was; in nits over paper white's; never
		past the display's peak; the 3D view's light over it, so that the
		view is shown as it was and the UI at its own nits */
		struct xgpu_hdr_frame same = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 2.0f, 4.0f, 0.0f };
		struct xgpu_hdr_frame one = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 2.0f, 4.0f, 1.0f };
		struct xgpu_hdr_frame dim = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 2.0f, 4.0f, 0.6f };
		struct xgpu_hdr_frame pq = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 4.0f, 2.0f };
		float a[3] = { 1.0f, 0.5f, 0.2f }, b[3] = { 1.0f, 0.5f, 0.2f }, hud[3] = { 1.0f, 1.0f, 1.0f },
			view[3] = { 0.8f, 0.4f, 1.0f }, shown[3], glow[3] = { XGPU_HDR_CEILING_HUD_VALUE, 1.0f, 1.0f },
			pq_hud[3] = { 1.0f, 1.0f, 1.0f }, pq_glow[3] = { XGPU_HDR_CEILING_HUD_VALUE, 1.0f, 1.0f };
		int channel;

		CHECK(xgpu_hdr_ui_white(0.0f, 2.0f, 8.0f) == 1.0f, "0: paper white's");
		CHECK(xgpu_hdr_ui_white(2.0f, 2.0f, 8.0f) == 1.0f, "the UI's nits paper white's: 1");
		CHECK(xgpu_hdr_ui_white(1.2f, 2.0f, 8.0f) == 0.6f, "120 nits over 200: %.3f", xgpu_hdr_ui_white(1.2f, 2.0f, 8.0f));
		CHECK(xgpu_hdr_ui_white(5.0f, 2.0f, 4.0f) == 2.0f, "the UI's ceiling the display's peak: %.3f",
			xgpu_hdr_ui_white(5.0f, 2.0f, 4.0f));
		CHECK(xgpu_hdr_ui_white(500.0f / 203.0f, 1.0f, 8.0f) * 203.0f > 499.9f, "500 nits over a 203-nit paper white");
		xgpu_hdr_encode(a, &same);
		xgpu_hdr_encode(b, &one);
		CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "the default (0) is paper white's, exactly");
		xgpu_hdr_encode(hud, &dim);
		CHECK(fabsf(hud[0] - 1.2f) < 1e-5f, "the HUD's white at 120 nits (EDR 1.2): %.5f", hud[0]);
		/* (the view: tone mapped, put over the UI's white, encoded, as it
		would be at paper white's) */
		for (channel = 0; channel < 3; channel++)
			shown[channel] = view[channel];
		xgpu_hdr_encode(shown, &one);
		xgpu_hdr_to_ui(view, dim.ui);
		CHECK(view[2] > 1.0f, "the view's white past the frame's 1.0 under a dimmer UI: %.4f", view[2]);
		xgpu_hdr_encode(view, &dim);
		for (channel = 0; channel < 3; channel++)
			CHECK(fabsf(view[channel] - shown[channel]) < 1e-4f, "the view as it was, channel %d: %.5f, %.5f", channel,
				view[channel], shown[channel]);
		/* (the HUD's glows keep their ratio above the UI's white; nothing
		past the peak) */
		xgpu_hdr_encode(glow, &dim);
		CHECK(fabsf(glow[0] / 1.2f - 1.5f) < 1e-3f, "a HUD glow 1.5 times the UI's white: %.4f", glow[0] / 1.2f);
		xgpu_hdr_encode(pq_hud, &pq);
		xgpu_hdr_encode(pq_glow, &pq);
		CHECK(pq_hud[0] > 0.6537f && pq_hud[0] < 0.6547f, "PQ: the UI at 406 nits: %.5f", pq_hud[0]);
		CHECK(pq_glow[0] > pq_hud[0], "PQ: its glow past it, under the peak: %.5f", pq_glow[0]);
	}
	{
		/* display.hdr_sdr_curve (#1120): at 203-nit paper white, gamma 2.2's
		dark codes are as dark as a gamma-2.2 SDR display shows them (code 1
		0.001 nit, 10 0.16, 20 0.75; sRGB's 0.062, 0.62, 1.42), joined to
		sRGB's by code 51, from which up nothing changes; never falling. The
		negative control: sRGB's (the frame's 0 value), exactly the
		piecewise curve the encoding had. */
		struct xgpu_hdr_frame srgb = { F, XGPU_HDR_OUTPUT_EXTENDED_LINEAR, 203.0f, 8.0f, 1.0f };
		struct xgpu_hdr_frame power = srgb;
		static const int codes[3] = { 1, 10, 20 };
		static const float power_nits[3] = { 0.001f, 0.163f, 0.746f }, srgb_nits[3] = { 0.0615f, 0.615f, 1.415f };
		float previous = -1.0f;
		int index, code;

		power.sdr_curve = XGPU_HDR_SDR_CURVE_GAMMA_2_2;
		for (index = 0; index < 3; index++)
		{
			float value = (float)codes[index] / 255.0f;
			float a[3] = { value, value, value }, b[3] = { value, value, value };

			xgpu_hdr_encode(a, &power);
			xgpu_hdr_encode(b, &srgb);
			CHECK(fabsf(a[0] - power_nits[index]) <= 0.02f * power_nits[index] + 1e-4f,
				"2.2: code %d at %.4f nits, not %.4f", codes[index], a[0], power_nits[index]);
			CHECK(fabsf(b[0] - srgb_nits[index]) <= 0.01f * srgb_nits[index],
				"sRGB: code %d at %.4f nits, not %.4f", codes[index], b[0], srgb_nits[index]);
			CHECK(b[0] == xgpu_hdr_linear(value) * 203.0f, "sRGB: code %d not the piecewise curve's", codes[index]);
		}
		for (code = 0; code <= 255 * 4; code++)
		{
			float value = (float)code / 255.0f;
			float a[3] = { value, value, value }, b[3] = { value, value, value };

			xgpu_hdr_encode(a, &power);
			xgpu_hdr_encode(b, &srgb);
			CHECK(a[0] >= previous, "2.2: code %d darker than the one before it: %.6f", code, a[0]);
			previous = a[0];
			if (code >= 51)
				CHECK(a[0] == b[0], "2.2: code %d (past 51) changed: %.6f, sRGB's %.6f", code, a[0], b[0]);
			else if (code)
				CHECK(a[0] < b[0], "2.2: code %d no darker than sRGB's: %.6f, %.6f", code, a[0], b[0]);
			else
				CHECK(a[0] == 0.0f && b[0] == 0.0f, "black not black");
		}
		CHECK(xgpu_hdr_sdr_curve_of(XGPU_HDR_SDR_CURVE_AUTO, 1) == XGPU_HDR_SDR_CURVE_SRGB &&
			xgpu_hdr_sdr_curve_of(XGPU_HDR_SDR_CURVE_AUTO, 0) == XGPU_HDR_SDR_CURVE_GAMMA_2_2 &&
			xgpu_hdr_sdr_curve_of(XGPU_HDR_SDR_CURVE_SRGB, 0) == XGPU_HDR_SDR_CURVE_SRGB &&
			xgpu_hdr_sdr_curve_of(XGPU_HDR_SDR_CURVE_GAMMA_2_2, 1) == XGPU_HDR_SDR_CURVE_GAMMA_2_2,
			"auto: sRGB on Apple's platforms, 2.2 elsewhere; a choice kept");
	}
	{
		/* #1122: the default (no full-frame peak, no black, the System
		peak) is today's picture bit for bit: the tone map with a full-frame
		peak of 0, or one at or past the peak, the shoulder exactly, and the
		encoding with a black of 0 as it was. A peak of the player's changes
		only what is past paper white. */
		static const float probes[6][3] = { { 0.0f, 0.0f, 0.0f }, { 0.1f, 0.2f, 0.3f }, { 0.9f, 0.6f, 0.3f },
			{ 1.0f, 1.0f, 1.0f }, { 1.4f, 1.1f, 0.6f }, { 3.0f, 2.5f, 1.5f } };
		struct xgpu_hdr_frame was = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 4.0f, 1.0f, XGPU_HDR_SDR_CURVE_GAMMA_2_2 };
		struct xgpu_hdr_frame now = was;
		int index, channel, mode_index;
		float x;

		now.full_frame = 0.0f;
		now.black = 0.0f;
		for (mode_index = F; mode_index <= P; mode_index++)
		{
			for (index = 0; index < 6; index++)
			{
				float a[3], b[3], c[3], e[3], f[3];

				for (channel = 0; channel < 3; channel++)
					a[channel] = b[channel] = c[channel] = e[channel] = f[channel] = probes[index][channel];
				xgpu_hdr_tone_map(a, mode_index, 4.0f);
				xgpu_hdr_tone_map_full(b, mode_index, 4.0f, 0.0f);
				xgpu_hdr_tone_map_full(c, mode_index, 4.0f, 4.0f);
				CHECK(!memcmp(a, b, sizeof(a)) && !memcmp(a, c, sizeof(a)),
					"no full-frame peak (0, or the peak): the tone map bit for bit (probe %d)", index);
				xgpu_hdr_encode(e, &was);
				xgpu_hdr_encode(f, &now);
				CHECK(!memcmp(e, f, sizeof(e)), "no black: the encoding bit for bit (probe %d)", index);
			}
		}
		for (x = 0.0f; x <= 50.0f; x += 0.01f)
			CHECK(xgpu_hdr_highlight(x, 1.0f, 8.0f, 0.0f) == xgpu_hdr_shoulder(x, 1.0f, 8.0f) &&
				xgpu_hdr_highlight(x, 1.0f, 8.0f, 9.0f) == xgpu_hdr_shoulder(x, 1.0f, 8.0f) &&
				xgpu_hdr_highlight(x, 1.0f, 8.0f, 0.5f) == xgpu_hdr_shoulder(x, 1.0f, 8.0f),
				"a full-frame peak outside (knee, peak) is the shoulder itself: %.2f", x);
		/* (a peak of the player's: the System peak's headroom, or the
		player's nits over the SDR white; never under 1) */
		CHECK(xgpu_hdr_peak_headroom(0, 3.5f, 240.0f) == 3.5f, "0: the system's headroom");
		CHECK(xgpu_hdr_peak_headroom(1000, 3.5f, 250.0f) == 4.0f, "1000 nits over a 250-nit white: 4");
		CHECK(xgpu_hdr_peak_headroom(200, 3.5f, 300.0f) == 1.0f, "a peak under the white: 1");
		/* a manual peak (1000 nits for the system's 600, at 203-nit paper
		white) changes only highlights: every value at or under paper white
		the same in the frame and the encoding, the highlights brighter */
		{
			float system_peak = 600.0f / 203.0f, player_peak = 1000.0f / 203.0f;
			struct xgpu_hdr_frame low = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 0.0f, 1.0f, XGPU_HDR_SDR_CURVE_GAMMA_2_2 };
			struct xgpu_hdr_frame high = low;
			int step;

			low.peak = system_peak;
			high.peak = player_peak;
			for (mode_index = F; mode_index <= P; mode_index++)
			{
				for (step = 0; step <= 400; step++)
				{
					float value = (float)step / 100.0f;
					float a[3] = { value, value * 0.7f, value * 0.4f }, b[3] = { value, value * 0.7f, value * 0.4f };
					float most_a, most_b;

					xgpu_hdr_tone_map(a, mode_index, system_peak);
					xgpu_hdr_tone_map(b, mode_index, player_peak);
					most_a = fmaxf(a[0], fmaxf(a[1], a[2]));
					most_b = fmaxf(b[0], fmaxf(b[1], b[2]));
					if (mode_index == F && value <= 1.0f)
						CHECK(!memcmp(a, b, sizeof(a)), "faithful: %.2f under paper white moved with the peak", value);
					if (most_a <= 1.0f && most_b <= 1.0f)
					{
						xgpu_hdr_encode(a, &low);
						xgpu_hdr_encode(b, &high);
						CHECK(!memcmp(a, b, sizeof(a)), "%s: %.2f, under paper white, encoded otherwise",
							mode_index == F ? "faithful" : "punchy", value);
					}
					else
						CHECK(most_b >= most_a - 1.0e-6f, "%s: %.2f's highlight dimmer with the higher peak: %.4f, %.4f",
							mode_index == F ? "faithful" : "punchy", value, most_b, most_a);
				}
			}
			{
				float a[3] = { 3.0f, 3.0f, 3.0f }, b[3] = { 3.0f, 3.0f, 3.0f };

				xgpu_hdr_tone_map(a, F, system_peak);
				xgpu_hdr_tone_map(b, F, player_peak);
				CHECK(xgpu_hdr_linear(b[0]) > 1.05f * xgpu_hdr_linear(a[0]), "a core brighter with the higher peak: "
					"%.3f, %.3f", xgpu_hdr_linear(b[0]), xgpu_hdr_linear(a[0]));
			}
		}
		/* the full-frame peak shapes the shoulder as meant: the same to the
		knee at its slope, rising, under the peak; the bright parts (2 to 4
		times paper white) held under the shoulder, to 3 under the
		full-frame peak, a core far past it still reaching for the
		peak; a lower full-frame peak holds them lower */
		{
			float last = 0.0f, core;

			for (x = 0.0f; x <= 400.0f; x += 0.05f)
			{
				float y = xgpu_hdr_highlight(x, 1.0f, 10.0f, 2.5f);

				CHECK(y >= last - 1.0e-5f && y < 10.0f, "the curve at %.2f: %.4f after %.4f", x, y, last);
				if (x <= 1.0f)
					CHECK(y == x, "identity to the knee: %.2f", x);
				last = y;
			}
			CHECK(fabsf((xgpu_hdr_highlight(1.001f, 1.0f, 10.0f, 2.5f) - 1.0f) / 0.001f - 1.0f) < 0.01f,
				"the knee's slope 1");
			for (x = 2.0f; x <= 4.0f; x += 0.5f)
			{
				float held = xgpu_hdr_highlight(x, 1.0f, 10.0f, 2.5f), lower = xgpu_hdr_highlight(x, 1.0f, 10.0f, 1.8f);

				CHECK(held < xgpu_hdr_shoulder(x, 1.0f, 10.0f) && (x > 3.0f || held < 2.5f), "%.1f held under the "
					"shoulder (to 3, under the full-frame peak): %.3f (the shoulder's %.3f)", x, held,
					xgpu_hdr_shoulder(x, 1.0f, 10.0f));
				CHECK(lower < held, "a lower full-frame peak holds %.1f lower: %.3f, %.3f", x, lower, held);
			}
			core = xgpu_hdr_highlight(1000.0f, 1.0f, 10.0f, 2.5f);
			CHECK(core > 9.0f, "a core far past white still near the peak: %.3f", core);
			/* (the tone map with it: under paper white as it was, a highlight
			held) */
			{
				float under[3] = { 0.9f, 0.5f, 0.2f }, bright[3] = { 1.5f, 1.4f, 1.3f }, plain[3] = { 1.5f, 1.4f, 1.3f };

				xgpu_hdr_tone_map_full(under, F, 10.0f, 2.5f);
				xgpu_hdr_tone_map_full(bright, F, 10.0f, 2.5f);
				xgpu_hdr_tone_map(plain, F, 10.0f);
				CHECK(under[0] == 0.9f && under[1] == 0.5f && under[2] == 0.2f, "faithful under white as it is");
				CHECK(bright[0] < plain[0] && bright[0] > 1.0f, "a highlight held by the full-frame peak: %.4f, %.4f",
					bright[0], plain[0]);
			}
		}
		/* the black lifts only what is under XGPU_HDR_BLACK_REACH times it:
		0 to the black, rising, meeting the line at the reach; and in the
		encoding (scRGB, 0.05 nits), code 0 at the black, paper white as it
		was. The negative control: no black, nothing lifted. */
		{
			float black = 0.05f / 80.0f, last = -1.0f, l;
			struct xgpu_hdr_frame lifted = { F, XGPU_HDR_OUTPUT_SCRGB, 203.0f / 80.0f, 4.0f, 1.0f,
				XGPU_HDR_SDR_CURVE_GAMMA_2_2, 0.0f, 0.05f / 80.0f };
			struct xgpu_hdr_frame plain = lifted;
			float zero[3] = { 0.0f, 0.0f, 0.0f }, zero_plain[3] = { 0.0f, 0.0f, 0.0f }, white[3] = { 1.0f, 1.0f, 1.0f },
				white_plain[3] = { 1.0f, 1.0f, 1.0f };

			plain.black = 0.0f;
			CHECK(xgpu_hdr_black(0.0f, black) == black, "black's light at the display's black");
			for (l = 0.0f; l <= 8.0f * black; l += black / 64.0f)
			{
				float y = xgpu_hdr_black(l, black);

				CHECK(y >= last && y >= l, "the lift at %.6f: %.6f after %.6f", l, y, last);
				if (l >= XGPU_HDR_BLACK_REACH * black)
					CHECK(y == l, "past the reach as it was: %.6f", l);
				CHECK(xgpu_hdr_black(l, 0.0f) == l, "no black: %.6f lifted", l);
				last = y;
			}
			xgpu_hdr_encode(zero, &lifted);
			xgpu_hdr_encode(zero_plain, &plain);
			xgpu_hdr_encode(white, &lifted);
			xgpu_hdr_encode(white_plain, &plain);
			CHECK(fabsf(zero[0] * 80.0f - 0.05f) < 1e-6f && zero_plain[0] == 0.0f, "code 0 at 0.05 nits: %.6f",
				zero[0] * 80.0f);
			CHECK(!memcmp(white, white_plain, sizeof(white)), "paper white as it was");
		}
	}
	{
		/* display.hdr_gamut (#1124): "srgb" (0) the encoding as it was; and
		"expanded" leaves every colour at or under paper white bit for bit,
		expands a highlight's chroma keeping its Oklab hue and lightness,
		stays within the target's gamut and the peak (never past BT.2020),
		the more chroma the brighter, and a target of the primaries */
		static const float srgb_primaries[8] = { 0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.329f };
		static const float p3_primaries[8] = { 0.68f, 0.32f, 0.265f, 0.69f, 0.15f, 0.06f, 0.3127f, 0.329f };
		static const float bt2020_primaries[8] = { 0.708f, 0.292f, 0.17f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.329f };
		static const float none[8] = { 0.0f };
		static const float targets[3] = { -1.0f, 0.0f, 1.0f };
		struct xgpu_hdr_frame plain = { F, XGPU_HDR_OUTPUT_PQ, 203.0f / 10000.0f, 8.0f, 1.0f, XGPU_HDR_SDR_CURVE_GAMMA_2_2 };
		struct xgpu_hdr_frame wide = plain;
		unsigned seed = 7;
		int index, channel, outside = 0, hue_moved = 0, target;
		float last_chroma = 0.0f;

		wide.gamut = 1;
		wide.gamut_target = 1.0f;
		CHECK(fabsf(xgpu_hdr_gamut_target(srgb_primaries) + 1.0f) < 0.01f &&
			fabsf(xgpu_hdr_gamut_target(p3_primaries)) < 0.01f &&
			fabsf(xgpu_hdr_gamut_target(bt2020_primaries) - 1.0f) < 0.01f && xgpu_hdr_gamut_target(none) == 0.0f &&
			xgpu_hdr_gamut_target(NULL) == 0.0f, "the targets of sRGB's, P3's, BT.2020's primaries and of none");
		for (index = 0; index <= 255; index++)
		{
			float value = (float)index / 255.0f;
			float a[3] = { value, value * 0.3f, value * 0.9f }, b[3] = { value, value * 0.3f, value * 0.9f };

			xgpu_hdr_encode(a, &plain);
			xgpu_hdr_encode(b, &wide);
			CHECK(!memcmp(a, b, sizeof(a)), "expanded: %.3f, under paper white, encoded otherwise", value);
		}
		for (target = 0; target < 3; target++)
		{
			for (index = 0; index < 4000; index++)
			{
				float rgb[3], before[3], lab_before[3], lab_after[3], matrix[3][3], peak = 8.0f;

				for (channel = 0; channel < 3; channel++)
				{
					seed = seed * 1103515245u + 12345u;
					rgb[channel] = (float)((seed >> 8) & 0xffff) / 65535.0f * peak;
					before[channel] = rgb[channel];
				}
				xgpu_hdr_gamut(rgb, peak, 1.0f, targets[target]);
				xgpu_hdr_gamut_matrix(targets[target], matrix);
				for (channel = 0; channel < 3; channel++)
				{
					float value = matrix[channel][0] * rgb[0] + matrix[channel][1] * rgb[1] + matrix[channel][2] * rgb[2];

					outside += value < -1.0e-4f * peak || value > peak * 1.0001f;
				}
				xgpu_hdr_oklab(before, lab_before);
				xgpu_hdr_oklab(rgb, lab_after);
				if (sqrtf(lab_before[1] * lab_before[1] + lab_before[2] * lab_before[2]) > 0.02f)
					hue_moved += fabsf(atan2f(lab_after[2], lab_after[1]) - atan2f(lab_before[2], lab_before[1])) > 1.0e-3f ||
						fabsf(lab_after[0] - lab_before[0]) > 1.0e-3f * lab_before[0];
			}
		}
		CHECK(!outside, "%d channels out of the target's gamut or past the peak", outside);
		CHECK(!hue_moved, "%d hues or lightnesses moved", hue_moved);
		for (index = 0; index <= 40; index++)
		{
			/* (a plasma bolt's blue, brighter: its chroma rising, more than
			sRGB's past white) */
			float scale = 1.0f + (float)index * 0.1f;
			float rgb[3] = { 0.02f * scale, 0.15f * scale, 1.0f * scale }, lab[3], chroma;

			xgpu_hdr_gamut(rgb, 8.0f, 1.0f, 0.0f);
			xgpu_hdr_oklab(rgb, lab);
			chroma = sqrtf(lab[1] * lab[1] + lab[2] * lab[2]) / lab[0];
			CHECK(chroma >= last_chroma - 1.0e-4f, "a bolt at %.1f: its chroma %.4f fell from %.4f", scale, chroma,
				last_chroma);
			if (index == 40)
			{
				float plain_rgb[3] = { 0.02f * scale, 0.15f * scale, 1.0f * scale }, plain_lab[3];

				xgpu_hdr_oklab(plain_rgb, plain_lab);
				CHECK(chroma > 1.1f * sqrtf(plain_lab[1] * plain_lab[1] + plain_lab[2] * plain_lab[2]) / plain_lab[0],
					"a bright bolt more vivid: %.4f", chroma);
				CHECK(fminf(rgb[0], fminf(rgb[1], rgb[2])) < 0.0f, "past sRGB (a channel under 0): %.4f %.4f %.4f",
					rgb[0], rgb[1], rgb[2]);
			}
			last_chroma = chroma;
		}
	}
	{
		/* the final write's dither (#1108): its noise within [-1, 1],
		about 0 on average and triangular (variance 1/6) over a screen's
		pixels; a code moved by at most a level, black and white exact; no
		levels (the negative control) nothing */
		double sum = 0.0, squares = 0.0;
		float most = 0.0f;
		int x, y;

		for (y = 0; y < 512; y++)
		{
			for (x = 0; x < 512; x++)
			{
				float n = xgpu_hdr_noise((float)x + 0.5f, (float)y + 0.5f);

				sum += n;
				squares += (double)n * n;
				most = fmaxf(most, fabsf(n));
			}
		}
		sum /= 512.0 * 512.0;
		squares /= 512.0 * 512.0;
		CHECK(most <= 1.0f && fabs(sum) < 0.005 && fabs(squares - 1.0 / 6.0) < 0.01,
			"the noise: most %.4f, mean %.5f, variance %.4f", most, sum, squares);
		CHECK(xgpu_hdr_dither(0.0f, 3.5f, 7.5f, 255.0f) == 0.0f && xgpu_hdr_dither(1.0f, 3.5f, 7.5f, 255.0f) == 1.0f,
			"black and white exact");
		for (x = 0; x < 64; x++)
		{
			float code = 100.3f / 255.0f, dithered = xgpu_hdr_dither(code, (float)x, 5.0f, 255.0f);

			CHECK(fabsf(dithered - code) * 255.0f <= 1.0001f, "a code moved by more than a level: %.4f",
				(dithered - code) * 255.0f);
			CHECK(xgpu_hdr_dither(code, (float)x, 5.0f, 0.0f) == code, "no levels: dithered");
		}
		/* (a fraction of a level above black, as display.gamma 0.5 makes
		codes 1 to 11: shown on average within 0.06 of a level, not crushed
		to black, and never below it) */
		for (x = 1; x < 10; x++)
		{
			float code = (float)x * 0.1f / 255.0f;
			double shown = 0.0;

			for (y = 0; y < 4096; y++)
			{
				float level = roundf(xgpu_hdr_dither(code, (float)(y % 64), (float)(y / 64), 255.0f) * 255.0f);

				CHECK(level >= 0.0f, "below black");
				shown += level;
			}
			shown /= 4096.0;
			CHECK(shown > 0.0 && fabs(shown - x * 0.1) < 0.06, "%.1f of a level above black shown as %.3f", x * 0.1,
				shown);
		}
	}
	{
		/* the display as Video Setup and the log show it */
		struct xgpu_hdr_display reported = { 1, 800.0f / 203.0f, 203.0f, 1 };
		struct xgpu_hdr_display edr = { 1, 16.0f, XGPU_HDR_EDR_WHITE_NITS, 0 };
		struct xgpu_hdr_display sdr = { 0, 1.0f, 203.0f, 1 };
		char text[200];

		xgpu_hdr_display_text(&reported, XGPU_HDR_DISPLAY_VALUE, text, sizeof(text));
		CHECK(!strcmp(text, "800 NITS"), "the row's value: %s", text);
		xgpu_hdr_display_text(&reported, XGPU_HDR_DISPLAY_WORDS, text, sizeof(text));
		CHECK(!strncmp(text, "Display peak 800 nits, SDR white 203 nits, as the system reports them.", 70),
			"the panel's words: %s", text);
		xgpu_hdr_display_text(&reported, XGPU_HDR_DISPLAY_LOG, text, sizeof(text));
		CHECK(!strcmp(text, "hdr: display peak 800 nits, SDR white 203 nits, from the system"), "the log: %s", text);
		xgpu_hdr_display_text(&edr, XGPU_HDR_DISPLAY_VALUE, text, sizeof(text));
		CHECK(!strcmp(text, "1600 NITS"), "EDR's value: %s", text);
		xgpu_hdr_display_text(&edr, XGPU_HDR_DISPLAY_WORDS, text, sizeof(text));
		CHECK(!strncmp(text, "Display peak 1600 nits. SDR white not reported: using 100 nits.", 63),
			"EDR's words: %s", text);
		xgpu_hdr_display_text(&edr, XGPU_HDR_DISPLAY_LOG, text, sizeof(text));
		CHECK(!strcmp(text, "hdr: display peak 1600 nits, SDR white not reported, using 100 nits (headroom 16.00)"),
			"EDR's log: %s", text);
		xgpu_hdr_display_text(&sdr, XGPU_HDR_DISPLAY_VALUE, text, sizeof(text));
		CHECK(!strcmp(text, "SDR"), "an SDR display's value: %s", text);
		xgpu_hdr_display_text(&sdr, XGPU_HDR_DISPLAY_LOG, text, sizeof(text));
		CHECK(!strcmp(text, "hdr: the display reports no HDR"), "an SDR display's log: %s", text);
		xgpu_hdr_display_text(&reported, XGPU_HDR_DISPLAY_VALUE, text, 4);
		CHECK(!strcmp(text, "800"), "cut to its room: %s", text);
	}
	/* paper white: display.hdr_paper_white_nits, else the system's white
	times display.hdr_paper_white's percent (a config before the nits) */
	CHECK(xgpu_hdr_paper_white_nits(300, 50, 203.0f) == 300.0f, "paper white in nits, the percent not read");
	CHECK(xgpu_hdr_paper_white_nits(0, 100, 240.0f) == 240.0f, "0: the system's white");
	CHECK(xgpu_hdr_paper_white_nits(0, 150, XGPU_HDR_EDR_WHITE_NITS) == 150.0f, "0: the percent of the system's");
	CHECK(xgpu_hdr_paper_white_nits(0, 0, 203.0f) == 203.0f, "no percent: the system's white");
	{
		/* a glow brightened toward its ceiling by its own light, at most to it */
		float had[3] = { 0.1f, 0.2f, 0.3f }, rgb[3] = { 0.6f, 0.95f, 1.0f };

		xgpu_hdr_expand(rgb, had, XGPU_HDR_CEILING_WEAPON);
		CHECK(rgb[2] > 1.0f, "the counter's digits past paper white: %.3f", rgb[2]);
		CHECK(rgb[0] - 0.6f < rgb[2] - 1.0f, "by its own light (its colour)");
	}
	{
		/* a teleporter's field: its bright swirls past paper white, toward
		the light's ceiling and no further, its hue kept; its dim body as it
		was; nothing added where the draw added nothing */
		float had[3] = { 0.05f, 0.08f, 0.05f }, swirl[3] = { 0.30f, 1.05f, 0.25f }, body[3] = { 0.10f, 0.35f, 0.08f },
			dark[3] = { 0.05f, 0.08f, 0.05f }, weapon[3] = { 0.30f, 1.05f, 0.25f };
		float most;

		xgpu_hdr_expand(swirl, had, XGPU_HDR_CEILING_LIGHT);
		xgpu_hdr_expand(weapon, had, XGPU_HDR_CEILING_WEAPON);
		xgpu_hdr_expand(body, had, XGPU_HDR_CEILING_LIGHT);
		xgpu_hdr_expand(dark, had, XGPU_HDR_CEILING_LIGHT);
		most = swirl[1];
		CHECK(xgpu_hdr_linear(most) > 2.0f && weapon[1] < most, "a swirl well past paper white, past a weapon's glow: "
			"%.3f (%.2f times), the weapon's %.3f", most, xgpu_hdr_linear(most), weapon[1]);
		CHECK(swirl[0] / swirl[1] < 0.30f / 1.05f && swirl[0] > 0.30f, "its green kept: %.3f %.3f %.3f", swirl[0],
			swirl[1], swirl[2]);
		CHECK(body[1] < 0.36f, "its dim body nearly as it was: %.3f", body[1]);
		CHECK(dark[0] == 0.05f && dark[1] == 0.08f && dark[2] == 0.05f, "nothing where it added nothing");
	}
	{
		/* a self-illuminated panel: its bright self-illumination past paper
		white, toward 2 times its light and no further, its hue kept; its
		dim glyphs nearly as they were; its lit surface, where nothing is
		self-illuminated, as it was */
		float base[3] = { 0.25f, 0.25f, 0.28f }, screen[3] = { 0.45f, 0.95f, 1.0f }, glyph[3] = { 0.30f, 0.42f, 0.35f },
			wall[3] = { 0.25f, 0.25f, 0.28f }, light[3] = { 0.45f, 0.95f, 1.0f };
		float most;

		xgpu_hdr_self_illuminate(screen, base);
		xgpu_hdr_self_illuminate(glyph, base);
		xgpu_hdr_self_illuminate(wall, base);
		xgpu_hdr_expand(light, base, XGPU_HDR_CEILING_LIGHT);
		most = fmaxf(screen[0], fmaxf(screen[1], screen[2]));
		CHECK(most > 1.2f && most <= XGPU_HDR_SELF_ILLUMINATION_VALUE + 1.0e-5f && most < light[2],
			"a screen past paper white, short of its ceiling and a light's: %.3f (%.2f times), a light's %.3f", most,
			xgpu_hdr_linear(most), light[2]);
		CHECK(screen[0] / screen[2] < 0.45f && screen[1] / screen[2] < 0.96f, "its hue kept: %.3f %.3f %.3f", screen[0],
			screen[1], screen[2]);
		CHECK(glyph[1] < 0.43f, "a dim glyph nearly as it was: %.3f", glyph[1]);
		CHECK(wall[0] == 0.25f && wall[1] == 0.25f && wall[2] == 0.28f, "nothing where it has no self-illumination");
		CHECK(fabsf(xgpu_hdr_linear(XGPU_HDR_SELF_ILLUMINATION_VALUE) - 2.0f) < 1.0e-3f &&
			XGPU_HDR_SELF_ILLUMINATION_VALUE < XGPU_HDR_CEILING_LIGHT_VALUE &&
			XGPU_HDR_SELF_ILLUMINATION_VALUE > XGPU_HDR_CEILING_WEAPON_VALUE,
			"its ceiling 2 times paper white's light, under the lights'");
	}
	{
		/* the dynamic lights: the same under white, more past it */
		CHECK(xgpu_hdr_lights(0.2f, 0.1f, 1) == xgpu_hdr_lights(0.2f, 0.1f, 0), "the lights' shoulder under white");
		CHECK(xgpu_hdr_lights(0.8f, 3.0f, 1) > 1.2f && xgpu_hdr_lights(0.8f, 3.0f, 0) <= 1.0f,
			"the lights' core past white with HDR: %.3f", xgpu_hdr_lights(0.8f, 3.0f, 1));
	}
	{
		/* the screenshot key's picture: the SDR frame's pixels as they are, a
		highlight inside white keeping its hue, a core white */
		float sdr[4] = { 0.5f, 0.25f, 1.0f, 0.3f }, glow[4] = { 1.6f, 0.8f, 0.4f, 1.0f },
			core[4] = { 3.0f, 2.5f, 1.5f, 1.0f };
		unsigned char bgra[4];

		xgpu_hdr_sdr_pixel(sdr, bgra);
		CHECK(bgra[0] == 255 && bgra[1] == 64 && bgra[2] == 128 && bgra[3] == 77,
			"an SDR pixel as it is, as BGRA: %u %u %u %u", bgra[0], bgra[1], bgra[2], bgra[3]);
		xgpu_hdr_sdr_pixel(glow, bgra);
		CHECK(bgra[2] == 255 && bgra[1] > 64 && bgra[1] < 255 && bgra[0] < bgra[1],
			"a glow past white in SDR, its hue kept: %u %u %u", bgra[2], bgra[1], bgra[0]);
		xgpu_hdr_sdr_pixel(core, bgra);
		CHECK(bgra[0] == 255 && bgra[1] == 255 && bgra[2] == 255, "a core of light white: %u %u %u", bgra[2], bgra[1],
			bgra[0]);
	}
	printf("%d failed\n", failures);
	return failures != 0;
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    if not COMPILER:
        pytest.skip("needs a C compiler")
    folder = tmp_path_factory.mktemp("hdr")
    for name in ("xgpu.h", "xgpu_backend.h", "xgpu_hdr.h", "xgpu_hdr.c"):
        shutil.copy(SOURCE / name, folder / name)
    (folder / "platform.h").write_text(PLATFORM)
    (folder / "driver.c").write_text(DRIVER)
    binary = folder / "driver"
    subprocess.run([COMPILER, "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-I", str(folder),
                    str(folder / "driver.c"), str(folder / "xgpu_hdr.c"), "-lm", "-o", str(binary)], check=True)
    return binary


def test_contract(built):
    result = subprocess.run([str(built)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr


def test_glsl_constants():
    """the shaders' curves are the reference's: Punchy's lift, bright lift and
    knees, and Faithful's clipped colour's start, written into the GLSL as
    numbers"""
    header = (SOURCE / "xgpu_hdr.h").read_text()
    glsl = (SOURCE / "xgpu_hdr.c").read_text()

    def macro(name):
        return float(re.search(rf"#define {name} ([\d.]+)f", header)[1])

    assert f"{macro('XGPU_HDR_PUNCHY_LIFT'):g} * (1.0 + {macro('XGPU_HDR_PUNCHY_BRIGHT'):g} * smoothstep(" \
           f"{macro('XGPU_HDR_PUNCHY_BRIGHT_KNEE'):g}, 1.0, sdr_luminance))" in glsl
    assert f"min(1.0, {macro('XGPU_HDR_PUNCHY_KNEE'):g} * peak)" in glsl
    assert f"smoothstep(1.0, {macro('XGPU_HDR_FAITHFUL_CLIPPED'):.1f}, max(v.r, max(v.g, v.b)))" in glsl
    assert f"smoothstep(vec3({macro('XGPU_HDR_SDR_TOE_START'):g}), vec3({macro('XGPU_HDR_SDR_TOE_END'):g}), v)" in glsl
    # (the dither's noise: the same interleaved gradient noise)
    for constant in ("52.9829189", "0.06711056", "0.00583715"):
        assert constant in glsl and constant in header, constant
    # (#1124: the gamut's chroma, reach, knee and halvings)
    assert f"1.0 + {macro('XGPU_HDR_GAMUT_CHROMA'):g} * weight" in glsl
    assert f"smoothstep(white, {macro('XGPU_HDR_GAMUT_REACH'):.1f} * white" in glsl
    assert f"1.0 + {macro('XGPU_HDR_GAMUT_KNEE'):g} * (room - 1.0)" in glsl
    assert re.search(r"#define XGPU_HDR_GAMUT_STEPS 12\b", header) and "step < 12;" in glsl
    # (#1122: the black lift's reach)
    assert f"{macro('XGPU_HDR_BLACK_REACH'):.1f} * black" in glsl
