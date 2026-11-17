/*
XGPU_HDR.H

display.hdr: the renderer's high dynamic range, one contract that every
backend implements (docs/hdr.md tells it at length). The front end
(d3d8_device.c) decides everything that is the game's: whether the frame is
drawn in HDR, which draws may go past the display's SDR white ("paper
white") and how far, and when the 3D view is tone mapped. A backend draws
what it is told: it makes the screen's target floating point, carries out
each draw's tag (below), runs the tone map and encodes the frame for its
display, filling in only what its display says (the headroom, the output's
encoding).

The frame. The back buffer holds the game's own values, blended by the
game's own maths, in half floats: 1.0 is paper white, the encoding is the
game's (sRGB's curve, extended past 1 as sRGB's extended spaces extend it),
the primaries sRGB's. Nothing is made linear but by the tone map and the
encoding. A value past 1 can only come of blending (every pixel shader's
result is clamped to [0, 1], as the NV2A's is), so a frame whose draws add
no light past white is the SDR frame, value for value.

The draws' tags (nv2a_pixel_shader_key.hdr, decided by xgpu_hdr_classify):

- none: as the game blends it, unclamped. Light the game adds (additive
  effects, plasma, lens flares, the sun, the dynamic lights) keeps what it
  adds past white: the highlights.
- a ceiling (XGPU_HDR_CEILING_*): the draw may not take a channel past
  max(what the pixel had, the ceiling), nor below 0. Paper white for
  surfaces and the HUD whose blend could go past white (a double
  multiply, the specular passes, the fog screens, the HUD's additive art),
  so that they stay where the SDR frame has them without dimming the light
  already there; a glow's ceiling (XGPU_HDR_GLOW) for the deliberate glows
  and the surfaces of light. The backend
  blends such a draw itself in its pixel shader (reading the pixel it
  draws over), with the key's blend (XGPU_HDR_BLEND_*), and its
  fixed-function blending off. The destination's colour as a blend factor
  is the pixel's value, but one less it is never below 0.
- a glow's ceiling (XGPU_HDR_GLOW: the first-person weapon's displays and
  glows; the HUD's flashes and the motion tracker's blips; the surfaces
  that are light, the light of the world the game draws in [0, 1]: a
  teleporter's field, a light fixture, a Covenant energy shield, a plasma
  bolt's light volume) brightened toward the ceiling where it is bright, by
  more of the light the draw added (its own colour: what was under it is
  not brightened): xgpu_hdr_expand.
- XGPU_HDR_ALPHA_LIMIT: destination alpha, which the game reads back as a
  blend factor, stays at most 1 (blended in the shader as above, its colour
  unlimited where no ceiling is set).
- XGPU_HDR_LIGHTS: the enhanced dynamic lights' shoulder (a structure's
  lightmap pass) goes on past white: the same curve below white, and what
  the SDR shoulder squeezed under it rolled off toward
  XGPU_HDR_LIGHTS_WHITE instead (xgpu_hdr_lights).
- XGPU_HDR_UNCLAMPED: the pixel stage carries values past 1 (no clamp
  above, where the NV2A's are: the combiners' results and inputs, the
  pixel's colour), for the screen effects' passes, which copy the tone
  mapped view through their target and back (a scope's warp and mask): they
  keep its light past white instead of cutting it at paper white.
- XGPU_HDR_SELF_ILLUMINATION: a surface whose own pass adds its
  self-illumination to its light (a model's; a structure's lightmap pass,
  which the textures' pass multiplies after) draws as the SDR frame has it
  but for its self-illumination (a door's panel, a computer's screen, a
  Forerunner glyph): the pixel less the same pixel without it (the
  constants of the combiner stages the tag names, its colours, made 0) is
  brightened toward XGPU_HDR_SELF_ILLUMINATION_VALUE as a glow is, where
  it is bright (xgpu_hdr_self_illuminate).

The tone map (xgpu_backend_tone_map) brings each window's 3D view into the
display's range before the HUD is drawn over it: XGPU_HDR_MODE_FAITHFUL
leaves every pixel no channel of which is past 1 as it is; one a little
past it keeps its hue, rolled off softly toward the display's peak, and one
far past it (a core of light the game adds up: an explosion's, a plasma
bolt's) takes the SDR frame's colour (its channels clipped, white-hot as
the SDR frame has it) brightened by the light that was clipped;
XGPU_HDR_MODE_PUNCHY does the same with the whole scene's light lifted and
its bright parts more, under a shoulder that starts lower, so that what is
under white keeps its hue, brighter, and the highlights go further toward
the peak. The encoding (the presentation) makes the frame linear, at most
the peak, times paper white, in the display's encoding. Their shaders are
one GLSL source (xgpu_hdr.c), compiled for each backend
(tools/metal_post_shaders.py, tools/vulkan_post_shaders.py); the C below
is the same maths, the reference the tests hold the shaders to.
*/

#ifndef __HALO_LINUX_XGPU_HDR_H
#define __HALO_LINUX_XGPU_HDR_H

/* ---------- settings */

/* display.hdr */
enum
{
	XGPU_HDR_OFF,
	XGPU_HDR_AUTO,
	XGPU_HDR_ON,
};

/* display.hdr_mode */
enum
{
	XGPU_HDR_MODE_FAITHFUL,
	XGPU_HDR_MODE_PUNCHY,
};

/* display.hdr_paper_white_nits: paper white in nits, these at most and
least (0: the system's SDR white); display.hdr_paper_white, its percent of
the system's SDR white while that is 0 (the setting before nits), these at
most and least */
#define XGPU_HDR_PAPER_WHITE_NITS_LEAST 80
#define XGPU_HDR_PAPER_WHITE_NITS_MOST 500
#define XGPU_HDR_PAPER_WHITE_LEAST 50
#define XGPU_HDR_PAPER_WHITE_MOST 250
/* display.hdr_ui_nits: the brightness of what is drawn over the tone mapped
view (the HUD, the menus, the screen flashes) in nits, these at most and
least (0: paper white's) */
#define XGPU_HDR_UI_NITS_LEAST 80
#define XGPU_HDR_UI_NITS_MOST 500

/* display.hdr_sdr_curve: the curve the encoding takes the frame's values
(SDR codes) to light by, the one an SDR display of the platform shows them
with, so that HDR's shadows are as dark as SDR's (xgpu_hdr_frame.sdr_curve:
SRGB or GAMMA_2_2; the setting's AUTO, the platform's:
xgpu_hdr_sdr_curve_of). sRGB's piecewise curve is the Mac's (its colour
management shows SDR so); most PC monitors and TVs show SDR at gamma 2.2,
which is darker near black (code 1 sixty times, code 10 four times). The
2.2 curve is gamma 2.2 to XGPU_HDR_SDR_TOE_START and sRGB's from
XGPU_HDR_SDR_TOE_END (code 51) up, joined smoothly between them, so that
only the shadows move. */
enum
{
	XGPU_HDR_SDR_CURVE_SRGB,
	XGPU_HDR_SDR_CURVE_GAMMA_2_2,
	XGPU_HDR_SDR_CURVE_AUTO,
};
#define XGPU_HDR_SDR_TOE_START 0.1f
#define XGPU_HDR_SDR_TOE_END 0.2f

/* the curve of a setting (XGPU_HDR_SDR_CURVE_*) on a platform (apple: macOS
or iOS, which show SDR with sRGB's curve) */
static inline int xgpu_hdr_sdr_curve_of(int setting, int apple)
{
	if (setting == XGPU_HDR_SDR_CURVE_SRGB || setting == XGPU_HDR_SDR_CURVE_GAMMA_2_2)
		return setting;
	return apple ? XGPU_HDR_SDR_CURVE_SRGB : XGPU_HDR_SDR_CURVE_GAMMA_2_2;
}
/* the system's SDR white in nits where it gives none: BT.2408's reference
white (Vulkan's, without SDL's), and EDR's 1.0 as macOS's reference modes
show it (Metal's: macOS and iOS give no nits, their SDR white following the
display's brightness) */
#define XGPU_HDR_REFERENCE_WHITE_NITS 203.0f
#define XGPU_HDR_EDR_WHITE_NITS 100.0f

/* paper white in nits from the settings (xgpu_backend_hdr's) and the
system's SDR white in nits */
static inline float xgpu_hdr_paper_white_nits(int nits, int percent, float system_white)
{
	if (nits > 0)
		return (float)nits;
	return system_white * (float)(percent > 0 ? percent : 100) / 100.0f;
}

/* the UI's white over paper white (xgpu_hdr_frame.ui), in any one
unit: display.hdr_ui_nits's (ui, 0 for paper white's), paper white's as
the encoding has it (no brighter than the display), and the display's
peak; the UI no brighter than the peak either */
static inline float xgpu_hdr_ui_white(float ui, float paper_white, float peak)
{
	if (!(ui > 0.0f) || !(paper_white > 0.0f))
		return 1.0f;
	if (peak > 0.0f && ui > peak)
		ui = peak;
	return ui / paper_white;
}

/* a display shows HDR (display.hdr "auto" turns it on) when its potential
headroom is more than this: macOS gives an SDR panel up to twice by dimming
its SDR white below the backlight's (a MacBook Air: 2.0), which is not an
HDR display */
#define XGPU_HDR_DISPLAY_HEADROOM 2.0f

/* ---------- the draws' tags (nv2a_pixel_shader_key.hdr: 16 bits) */

/* the draw's blend (XGPU_BLEND_* source and destination factors,
XGPU_BLEND_ADD... operation), where the shader blends it: with a ceiling or
XGPU_HDR_ALPHA_LIMIT */
#define XGPU_HDR_BLEND(source, destination, operation) \
	((unsigned short)(((source) & 15) | (((destination) & 15) << 4) | (((operation) & 7) << 8)))
#define XGPU_HDR_BLEND_SOURCE(hdr) ((hdr) & 15)
#define XGPU_HDR_BLEND_DESTINATION(hdr) (((hdr) >> 4) & 15)
#define XGPU_HDR_BLEND_OPERATION(hdr) (((hdr) >> 8) & 7)
#define XGPU_HDR_CEILING(ceiling) ((unsigned short)(((ceiling) & 7) << 11))
#define XGPU_HDR_CEILING_OF(hdr) (((hdr) >> 11) & 7)
#define XGPU_HDR_ALPHA_LIMIT 0x4000
#define XGPU_HDR_LIGHTS 0x8000
/* the shader blends the draw itself (its fixed-function blend is off; a
blend operation a draw has: the operations past them, 6 and 7, are the
tags below, whose ceiling's bits are a highlight boost's level) */
#define XGPU_HDR_SHADER_BLENDS(hdr) (XGPU_HDR_BLEND_OPERATION(hdr) < 6 && \
	(XGPU_HDR_CEILING_OF(hdr) != 0 || ((hdr) & XGPU_HDR_ALPHA_LIMIT) != 0))
/* the draw is a glow, brightened toward its ceiling (xgpu_hdr_expand) */
#define XGPU_HDR_GLOW(hdr) (XGPU_HDR_BLEND_OPERATION(hdr) < 6 && XGPU_HDR_CEILING_OF(hdr) >= XGPU_HDR_CEILING_WEAPON)
/* the pixel stage unclamped above (a blend operation no draw has, without
a ceiling or the alpha limit: no draw the shader blends) */
#define XGPU_HDR_UNCLAMPED XGPU_HDR_BLEND(0, 0, 7)
#define XGPU_HDR_IS_UNCLAMPED(hdr) ((hdr) == XGPU_HDR_UNCLAMPED)
/* a self-illuminated surface's pass (another blend operation no draw has,
without a ceiling or the alpha limit; XGPU_HDR_LIGHTS may be set too): the
combiner stages whose constants are the self-illumination's colours, a bit
each (stage n's 1 << n) */
#define XGPU_HDR_SELF_ILLUMINATION(stages) ((unsigned short)(XGPU_HDR_BLEND(0, 0, 6) | ((stages) & 0xff)))
#define XGPU_HDR_IS_SELF_ILLUMINATED(hdr) (XGPU_HDR_BLEND_OPERATION(hdr) == 6 && !((hdr) & XGPU_HDR_ALPHA_LIMIT))
#define XGPU_HDR_SELF_ILLUMINATION_STAGES(hdr) ((hdr) & 0xff)
/* display.hdr_highlight_boost (#1123): its level (0 none, the picture as
it was; at most XGPU_HDR_BOOST_MOST), in the ceiling's bits of a
self-illuminated surface's tag (XGPU_HDR_SELF_ILLUMINATION's) and of an
emitter's. An emitter's draw (an additive self-illuminated particle, a
glow, a sun's lens flare and glow, blended by the fixed function) has its
pixel's colour gained before the blend, its bound (XGPU_HDR_CEILING_LIGHT,
or XGPU_HDR_CEILING_WEAPON for the first-person weapon's effects) in the
blend's source bits: xgpu_hdr_boost_emitter */
#define XGPU_HDR_BOOST(level) XGPU_HDR_CEILING(level)
#define XGPU_HDR_BOOST_OF(hdr) (XGPU_HDR_BLEND_OPERATION(hdr) >= 6 ? XGPU_HDR_CEILING_OF(hdr) : 0)
#define XGPU_HDR_EMITTER(level, bound) ((unsigned short)(XGPU_HDR_BLEND((bound), 0, 7) | XGPU_HDR_BOOST(level)))
#define XGPU_HDR_IS_EMITTER(hdr) (XGPU_HDR_BLEND_OPERATION(hdr) == 7 && !((hdr) & XGPU_HDR_ALPHA_LIMIT) && \
	XGPU_HDR_BLEND_SOURCE(hdr) != 0 && XGPU_HDR_CEILING_OF(hdr) != 0)
#define XGPU_HDR_EMITTER_BOUND(hdr) XGPU_HDR_BLEND_SOURCE(hdr)
#define XGPU_HDR_BOOST_MOST 4
/* a level's gain on the emitter's light, in the frame's values: 1 and a
quarter more a level (twice at the most: about 5 times its light where
nothing bounds it) */
#define XGPU_HDR_BOOST_STEP 0.25f

/* the ceilings, in the frame's values (sRGB's curve, extended): paper
white; then the glows': the first-person weapon's, 1.75 times paper
white's light; the HUD's, 1.5 times; the light of the world's, 3 times
(a teleporter's field fills much of the screen: well past the surfaces,
short of an explosion's core) */
enum
{
	XGPU_HDR_CEILING_NONE,
	XGPU_HDR_CEILING_PAPER_WHITE,
	XGPU_HDR_CEILING_WEAPON,
	XGPU_HDR_CEILING_HUD,
	XGPU_HDR_CEILING_LIGHT,
};
#define XGPU_HDR_CEILING_PAPER_WHITE_VALUE 1.0f
#define XGPU_HDR_CEILING_WEAPON_VALUE 1.277043f
#define XGPU_HDR_CEILING_HUD_VALUE 1.194177f
#define XGPU_HDR_CEILING_LIGHT_VALUE 1.612451f

/* a glow's expansion: from this brightest channel of the blended pixel
(the frame's values) up, smoothly to the ceiling at 1; the light of the
world's from lower (a teleporter's field, a bolt's volume are drawn
dimmer than the weapon's displays) */
#define XGPU_HDR_EXPAND_KNEE 0.6f
#define XGPU_HDR_EXPAND_LIGHT_KNEE 0.4f
/* ... the light the draw added at most this many times more */
#define XGPU_HDR_EXPAND_MOST 2.0f

/* a self-illuminated surface's glow (XGPU_HDR_SELF_ILLUMINATION): toward 2
times paper white's light, from the light of the world's knee (a screen's
light is drawn as dim as a fixture's): well past the surfaces, short of the
lights and fixtures (a panel lights no room) */
#define XGPU_HDR_SELF_ILLUMINATION_VALUE 1.353256f
#define XGPU_HDR_SELF_ILLUMINATION_KNEE XGPU_HDR_EXPAND_LIGHT_KNEE

/* the enhanced dynamic lights' white with XGPU_HDR_LIGHTS: the linear light
(the lights' own gamma, 2.2) their shoulder rolls off toward, 8 times paper
white's */
#define XGPU_HDR_LIGHTS_WHITE 8.0f
/* (the same, as the shaders' text has it: the room past white, white less
1; and white in the lights' gamma, 8 to the 1 / 2.2) */
#define XGPU_HDR_LIGHTS_ROOM "7.0"
#define XGPU_HDR_LIGHTS_WHITE_VALUE 2.573330f

/* ---------- the tone map and the encoding */

/* Punchy's lift of the whole scene (linear light), its bright parts'
further lift (from SDR luminance PUNCHY_BRIGHT_KNEE to 1, up to 1 +
PUNCHY_BRIGHT), and its shoulder's knee (of the peak, at most paper white) */
#define XGPU_HDR_PUNCHY_LIFT 1.2f
#define XGPU_HDR_PUNCHY_BRIGHT 0.6f
#define XGPU_HDR_PUNCHY_BRIGHT_KNEE 0.5f
#define XGPU_HDR_PUNCHY_KNEE 0.75f
/* Faithful's colour past white: its own hue at 1 (the frame's values), the
SDR frame's clipped colour from this far past it */
#define XGPU_HDR_FAITHFUL_CLIPPED 2.0f

/* the display's encodings (xgpu_hdr_frame.output) */
enum
{
	/* SDR: the frame's values, clamped to [0, 1] (the window's 8 bits) */
	XGPU_HDR_OUTPUT_SDR,
	/* extended linear sRGB, 1.0 the system's SDR white: macOS's EDR */
	XGPU_HDR_OUTPUT_EXTENDED_LINEAR,
	/* scRGB: extended linear sRGB, 1.0 80 nits (Windows') */
	XGPU_HDR_OUTPUT_SCRGB,
	/* HDR10: BT.2020 primaries, SMPTE ST 2084 (PQ), 1.0 10 000 nits */
	XGPU_HDR_OUTPUT_PQ,
};

/* what the tone map and the encoding are given: the front end's settings,
and what the backend's display says. Paper white and the peak are linear
light: paper white in the output's units (over the system's SDR white for
EDR, xgpu_hdr_paper_white_nits over XGPU_HDR_EDR_WHITE_NITS; nits over 80
for scRGB; nits over 10 000 for PQ), the peak in paper whites (the display's headroom over paper
white: 1 for SDR). ui is the UI's white over paper white
(display.hdr_ui_nits: 1, or 0, for paper white's; 1 for SDR): the frame's
1.0 is the UI's white, the tone map leaves the 3D view in it (its light
over ui), and the encoding takes the frame's light times it. sdr_curve is
the curve the encoding takes the frame's values to light by
(XGPU_HDR_SDR_CURVE_SRGB or _GAMMA_2_2: display.hdr_sdr_curve's).
full_frame and black are the display's own (#1122: Windows' DXGI reports
them), given only with a peak of the player's (display.hdr_peak_nits), 0
otherwise, so that the System peak's picture is as it always was:
full_frame the most the display holds over the whole screen, in paper
whites as the peak is (the tone map's shoulder holds the bright parts
near it, xgpu_hdr_highlight); black the least it shows, in the output's
units as paper white is (the encoding lifts what is darker into its
range, xgpu_hdr_black). */
struct xgpu_hdr_frame
{
	int mode;
	int output;
	float paper_white;
	float peak;
	float ui;
	int sdr_curve;
	float full_frame;
	float black;
	/* display.hdr_gamut (#1124): "expanded" (1) or "srgb" (0, the picture
	as it was); and the gamut the highlights are expanded into
	(xgpu_hdr_gamut_target's: -1 sRGB's, 0 P3's, 1 BT.2020's) */
	int gamut;
	float gamut_target;
};

/* display.hdr_gamut "expanded" (#1124): what is past paper white (from
it, smoothly to XGPU_HDR_GAMUT_REACH times it) has its chroma in Oklab
scaled out, by up to 1 + XGPU_HDR_GAMUT_CHROMA, its hue and lightness
kept, softly compressed (a shoulder from XGPU_HDR_GAMUT_KNEE of the room
on) into the target's gamut and the peak; the room found by
XGPU_HDR_GAMUT_STEPS halvings */
#define XGPU_HDR_GAMUT_CHROMA 0.5f
#define XGPU_HDR_GAMUT_REACH 4.0f
#define XGPU_HDR_GAMUT_KNEE 0.75f
#define XGPU_HDR_GAMUT_STEPS 12

/* the gamut target (xgpu_hdr_frame.gamut_target) of a display's
primaries (red, green, blue, white: x and y each; DXGI's), by the area of
their triangle: sRGB's -1, P3's 0, BT.2020's 1, between them linearly; 0
(P3: DCI-P3 at D65, Display P3's) where they are none or make no sense */
static inline float xgpu_hdr_gamut_target(const float primaries[8])
{
	const float srgb = 0.11205f, p3 = 0.15200f, bt2020 = 0.21187f;
	float area;

	if (!primaries)
		return 0.0f;
	area = 0.5f * ((primaries[2] - primaries[0]) * (primaries[5] - primaries[1]) -
		(primaries[4] - primaries[0]) * (primaries[3] - primaries[1]));
	area = area < 0.0f ? -area : area;
	if (!(area > 0.05f && area < 0.3f))
		return 0.0f;
	if (area <= p3)
	{
		float t = (area - p3) / (p3 - srgb);

		return t < -1.0f ? -1.0f : t;
	}
	return area >= bt2020 ? 1.0f : (area - p3) / (bt2020 - p3);
}

/* display.hdr_peak_nits (#1122): the display's peak in nits, of the
player's, these at most and least (0: the system's) */
#define XGPU_HDR_PEAK_NITS_LEAST 200
#define XGPU_HDR_PEAK_NITS_MOST 10000

/* the display's headroom over its SDR white (system_white, in nits) as the
encoding takes it: the system's (system_headroom), or the player's peak in
nits over that white (display.hdr_peak_nits; 0 the system's), at least 1 */
static inline float xgpu_hdr_peak_headroom(int peak_nits, float system_headroom, float system_white)
{
	float headroom = system_headroom;

	if (peak_nits > 0 && system_white > 0.0f)
		headroom = (float)peak_nits / system_white;
	return headroom >= 1.0f ? headroom : 1.0f;
}

/* the black lift's reach (xgpu_hdr_black): light under this many times
the display's black is lifted into its range, the rest is as it was */
#define XGPU_HDR_BLACK_REACH 4.0f


/* ---------- the reference (host code and tests: the system's maths) */

#ifdef XGPU_HDR_REFERENCE
#include <math.h>

static inline float xgpu_hdr_linear(float value)
{
	if (value <= 0.0f)
		return 0.0f;
	return value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
}

static inline float xgpu_hdr_encoded(float linear)
{
	if (linear <= 0.0f)
		return 0.0f;
	return linear <= 0.0031308f ? linear * 12.92f : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

/* a value of the frame (an SDR code, extended past 1) as light by an SDR
curve (XGPU_HDR_SDR_CURVE_*): sRGB's, or gamma 2.2 joined to sRGB's by a
smoothstep from XGPU_HDR_SDR_TOE_START to XGPU_HDR_SDR_TOE_END (where sRGB's
is the brighter, so the join never falls) */
static inline float xgpu_hdr_sdr_light(float value, int curve)
{
	float srgb = xgpu_hdr_linear(value), t;

	if (curve != XGPU_HDR_SDR_CURVE_GAMMA_2_2 || value >= XGPU_HDR_SDR_TOE_END)
		return srgb;
	if (value <= 0.0f)
		return 0.0f;
	t = (value - XGPU_HDR_SDR_TOE_START) / (XGPU_HDR_SDR_TOE_END - XGPU_HDR_SDR_TOE_START);
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	t = t * t * (3.0f - 2.0f * t);
	return powf(value, 2.2f) * (1.0f - t) + srgb * t;
}

static inline float xgpu_hdr_ceiling_value(int ceiling)
{
	return ceiling == XGPU_HDR_CEILING_WEAPON ? XGPU_HDR_CEILING_WEAPON_VALUE :
		ceiling == XGPU_HDR_CEILING_HUD ? XGPU_HDR_CEILING_HUD_VALUE :
		ceiling == XGPU_HDR_CEILING_LIGHT ? XGPU_HDR_CEILING_LIGHT_VALUE : XGPU_HDR_CEILING_PAPER_WHITE_VALUE;
}

static inline float xgpu_hdr_expand_knee(int ceiling)
{
	return ceiling == XGPU_HDR_CEILING_LIGHT ? XGPU_HDR_EXPAND_LIGHT_KNEE : XGPU_HDR_EXPAND_KNEE;
}

static inline float xgpu_hdr_smoothstep(float edge0, float edge1, float x)
{
	float t = (x - edge0) / (edge1 - edge0);

	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

/* a pixel (rgb) brightened toward a ceiling (its value): its brightest
channel by up to (ceiling - 1) times itself, where it is bright (from the
knee), by more of the light added to what the pixel had (had), at most
XGPU_HDR_EXPAND_MOST times more */
static inline void xgpu_hdr_expand_toward(float rgb[3], const float had[3], float ceiling, float knee)
{
	float added[3], most = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])), added_most, more;
	int channel;

	for (channel = 0; channel < 3; channel++)
		added[channel] = fmaxf(rgb[channel] - had[channel], 0.0f);
	added_most = fmaxf(added[0], fmaxf(added[1], added[2]));
	if (added_most <= 0.0f)
		return;
	more = fminf((ceiling - 1.0f) * xgpu_hdr_smoothstep(knee, 1.0f, most) * most / added_most, XGPU_HDR_EXPAND_MOST);
	for (channel = 0; channel < 3; channel++)
		rgb[channel] += added[channel] * more;
}

/* a glow's blended pixel (rgb) brightened toward its ceiling, by more of
the light the draw added to what the pixel had (had) */
static inline void xgpu_hdr_expand(float rgb[3], const float had[3], int ceiling)
{
	xgpu_hdr_expand_toward(rgb, had, xgpu_hdr_ceiling_value(ceiling), xgpu_hdr_expand_knee(ceiling));
}

/* a self-illuminated surface's pixel (rgb, as its combiners leave it, in
[0, 1]) brightened toward XGPU_HDR_SELF_ILLUMINATION_VALUE by more of its
self-illumination: what it has past the same pixel without it (base) */
static inline void xgpu_hdr_self_illuminate(float rgb[3], const float base[3])
{
	xgpu_hdr_expand_toward(rgb, base, XGPU_HDR_SELF_ILLUMINATION_VALUE, XGPU_HDR_SELF_ILLUMINATION_KNEE);
}

/* display.hdr_highlight_boost (#1123): a gain (1 + XGPU_HDR_BOOST_STEP a
level) on the light an emitter adds to what was under it (rgb, after
today's: what it had, base), bounded: its brightest channel taken no
further than max(ceiling, its brightest as it was), so the boost never
passes the ceiling, nor takes away what the pixel had past it (the
emitter's light scaled, its hue kept). Level 0: as it was, exactly */
static inline void xgpu_hdr_boost_toward(float rgb[3], const float base[3], int level, float ceiling)
{
	float extra[3], extra_most, most = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])), share;
	int channel;

	if (level <= 0)
		return;
	for (channel = 0; channel < 3; channel++)
		extra[channel] = fmaxf(rgb[channel] - base[channel], 0.0f) * (XGPU_HDR_BOOST_STEP * (float)level);
	extra_most = fmaxf(extra[0], fmaxf(extra[1], extra[2]));
	if (!(extra_most > 0.0f))
		return;
	share = (ceiling - most) / extra_most;
	share = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
	for (channel = 0; channel < 3; channel++)
		rgb[channel] += extra[channel] * share;
}

/* a self-illuminated surface's pixel as xgpu_hdr_self_illuminate leaves
it, with a boost's level (XGPU_HDR_BOOST_OF), toward its own ceiling */
static inline void xgpu_hdr_self_illuminate_boost(float rgb[3], const float base[3], int level)
{
	xgpu_hdr_self_illuminate(rgb, base);
	xgpu_hdr_boost_toward(rgb, base, level, XGPU_HDR_SELF_ILLUMINATION_VALUE);
}

/* an emitter's pixel (rgb, the shader's colour in [0, 1], before the
fixed-function blend adds it) gained by a boost's level, its brightest
no further than its bound's ceiling (XGPU_HDR_CEILING_LIGHT, _WEAPON) */
static inline void xgpu_hdr_boost_emitter(float rgb[3], int level, int bound)
{
	static const float none[3] = { 0.0f, 0.0f, 0.0f };

	xgpu_hdr_boost_toward(rgb, none, level, xgpu_hdr_ceiling_value(bound));
}

/* the soft shoulder: identity to the knee, then toward the peak, never
reaching it (Reinhard's, meeting the line at its slope) */
static inline float xgpu_hdr_shoulder(float x, float knee, float peak)
{
	float range = peak - knee, t;

	if (x <= knee)
		return x;
	if (range <= 0.0f)
		return peak;
	t = (x - knee) / range;
	return knee + range * t / (1.0f + t);
}

/* the highlights' curve (#1122): the shoulder toward the peak, or, with
a full-frame peak between the knee and the peak (a display that cannot
hold its peak over the whole screen), two shoulders summed: one toward the
full-frame peak, at the knee's slope (bright surfaces, the sky, held near
what the display holds over the screen), and one toward the rest of the
peak that rises from the knee flat (a small core of light still reaching
for the peak). Identity to the knee either way, rising, never reaching the
peak; without a full-frame peak (0, or at the peak) the shoulder itself. */
static inline float xgpu_hdr_highlight(float x, float knee, float peak, float full_frame)
{
	float body, rest, u, v;

	if (!(full_frame > knee && full_frame < peak))
		return xgpu_hdr_shoulder(x, knee, peak);
	if (x <= knee)
		return x;
	body = full_frame - knee;
	rest = peak - full_frame;
	u = (x - knee) / body;
	v = (x - knee) / rest;
	return knee + body * u / (1.0f + u) + rest * v * v / (1.0f + v * v);
}

/* the tone map of one pixel's colour, in place: the light (made linear,
lifted in Punchy) keeps its hue under the shoulder, and toward the SDR
frame's colour, white-hot, brightened by the light the SDR frame clipped,
as it is further past white. Faithful leaves a pixel no channel of which is
past white as it is. full_frame: the display's full-frame peak in paper
whites (xgpu_hdr_frame's, 0 none: xgpu_hdr_highlight) */
static inline void xgpu_hdr_tone_map_full(float rgb[3], int mode, float peak, float full_frame)
{
	static const float luma[3] = { 0.2126f, 0.7152f, 0.0722f };
	float sdr[3], light[3], sdr_luminance = 0.0f, luminance = 0.0f, most, gain = 1.0f, knee = 1.0f, clipped, hue,
		brightened, value_most;
	int channel;

	for (channel = 0; channel < 3; channel++)
	{
		float value = rgb[channel] < 0.0f ? 0.0f : rgb[channel];

		sdr[channel] = xgpu_hdr_linear(value > 1.0f ? 1.0f : value);
		light[channel] = xgpu_hdr_linear(value);
		sdr_luminance += luma[channel] * sdr[channel];
		luminance += luma[channel] * light[channel];
	}
	if (mode == XGPU_HDR_MODE_PUNCHY)
	{
		gain = XGPU_HDR_PUNCHY_LIFT * (1.0f + XGPU_HDR_PUNCHY_BRIGHT *
			xgpu_hdr_smoothstep(XGPU_HDR_PUNCHY_BRIGHT_KNEE, 1.0f, sdr_luminance));
		knee = fminf(1.0f, XGPU_HDR_PUNCHY_KNEE * peak);
	}
	else if (luminance <= sdr_luminance)
	{
		for (channel = 0; channel < 3; channel++)
			rgb[channel] = rgb[channel] < 0.0f ? 0.0f : rgb[channel];
		return;
	}
	value_most = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
	clipped = xgpu_hdr_smoothstep(1.0f, XGPU_HDR_FAITHFUL_CLIPPED, value_most);
	most = fmaxf(light[0], fmaxf(light[1], light[2])) * gain;
	hue = most > 0.0f ? gain * xgpu_hdr_highlight(most, knee, peak, full_frame) / most : 0.0f;
	brightened = sdr_luminance > 0.0f ?
		xgpu_hdr_highlight(gain * luminance / sdr_luminance, knee, peak, full_frame) : 0.0f;
	for (channel = 0; channel < 3; channel++)
		rgb[channel] = xgpu_hdr_encoded(light[channel] * hue * (1.0f - clipped) + sdr[channel] * brightened * clipped);
}

/* (no full-frame peak: the shoulder toward the peak alone) */
static inline void xgpu_hdr_tone_map(float rgb[3], int mode, float peak)
{
	xgpu_hdr_tone_map_full(rgb, mode, peak, 0.0f);
}

/* the display's black (#1122; xgpu_hdr_frame.black, the output's units):
light under XGPU_HDR_BLACK_REACH times it lifted smoothly into its range
(0 to the black, the reach to itself, rising at half the slope or more,
meeting the line at its slope), so that a display that cannot show less
than its black shows the shadows' detail rather than one black; the rest,
and all of it without a black (0), as it was */
static inline float xgpu_hdr_black(float light, float black)
{
	float reach = XGPU_HDR_BLACK_REACH * black, t;

	if (!(black > 0.0f) || light >= reach)
		return light;
	t = 1.0f - (light > 0.0f ? light : 0.0f) / reach;
	return (light > 0.0f ? light : 0.0f) + black * t * t;
}

/* the 3D view's tone mapped colour (xgpu_hdr_tone_map's) in the frame's
values, in place, where the UI's white is not paper white: its light over
the UI's white (xgpu_hdr_frame.ui; 0 or 1, as it is) */
static inline void xgpu_hdr_to_ui(float rgb[3], float ui)
{
	int channel;

	if (!(ui > 0.0f) || ui == 1.0f)
		return;
	for (channel = 0; channel < 3; channel++)
		rgb[channel] = xgpu_hdr_encoded(xgpu_hdr_linear(rgb[channel]) / ui);
}

/* Oklab (#1124), of linear sRGB and back */
static inline void xgpu_hdr_oklab(const float rgb[3], float lab[3])
{
	float l = cbrtf(fmaxf(0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2], 0.0f));
	float m = cbrtf(fmaxf(0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2], 0.0f));
	float s = cbrtf(fmaxf(0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2], 0.0f));

	lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
	lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
	lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static inline void xgpu_hdr_linear_of_oklab(const float lab[3], float rgb[3])
{
	float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
	float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
	float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

	l = l * l * l;
	m = m * m * m;
	s = s * s * s;
	rgb[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
	rgb[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
	rgb[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

/* the matrix of linear sRGB to the gamut target's primaries (the target:
xgpu_hdr_gamut_target's), rows; sRGB's itself to P3's, P3's to BT.2020's
interpolated */
static inline void xgpu_hdr_gamut_matrix(float target, float matrix[3][3])
{
	static const float p3[3][3] =
	{
		{ 0.822462f, 0.177538f, 0.0f },
		{ 0.033194f, 0.966806f, 0.0f },
		{ 0.017083f, 0.072397f, 0.910520f },
	};
	static const float wide[3][3] =
	{
		{ 0.627404f, 0.329283f, 0.043313f },
		{ 0.069097f, 0.919540f, 0.011362f },
		{ 0.016391f, 0.088013f, 0.895595f },
	};
	int row, column;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			float identity = row == column ? 1.0f : 0.0f;

			matrix[row][column] = target < 0.0f ? identity + (p3[row][column] - identity) * (1.0f + target) :
				p3[row][column] + (wide[row][column] - p3[row][column]) * target;
		}
	}
}

/* a colour of Oklab within the target's gamut (its channels from 0 to the
peak, to a hair) */
static inline int xgpu_hdr_in_gamut(const float lab[3], float matrix[3][3], float peak)
{
	float rgb[3];
	int row;

	xgpu_hdr_linear_of_oklab(lab, rgb);
	for (row = 0; row < 3; row++)
	{
		float value = matrix[row][0] * rgb[0] + matrix[row][1] * rgb[1] + matrix[row][2] * rgb[2];

		if (value < -1.0e-5f * peak || value > peak * (1.0f + 1.0e-5f))
			return 0;
	}
	return 1;
}

/* display.hdr_gamut "expanded" (#1124): one pixel's light (linear sRGB,
over paper white, at most the peak), in place, past white (white: the
UI's white or paper white, the brighter) its chroma scaled out in Oklab,
its hue and lightness kept, softly into the target's gamut; under white
exactly as it was */
static inline void xgpu_hdr_gamut(float rgb[3], float peak, float white, float target)
{
	float most = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])), lab[3], matrix[3][3], low = 1.0f,
		high = 1.0f + XGPU_HDR_GAMUT_CHROMA, room, scale, weight, tried[3];
	int step;

	weight = xgpu_hdr_smoothstep(white, XGPU_HDR_GAMUT_REACH * white, most);
	if (!(weight > 0.0f))
		return;
	xgpu_hdr_oklab(rgb, lab);
	if (!(sqrtf(lab[1] * lab[1] + lab[2] * lab[2]) >= 1.0e-6f))
		return;
	xgpu_hdr_gamut_matrix(target, matrix);
	tried[0] = lab[0];
	tried[1] = lab[1] * high;
	tried[2] = lab[2] * high;
	if (!xgpu_hdr_in_gamut(tried, matrix, peak))
	{
		for (step = 0; step < XGPU_HDR_GAMUT_STEPS; step++)
		{
			float middle = 0.5f * (low + high);

			tried[1] = lab[1] * middle;
			tried[2] = lab[2] * middle;
			if (xgpu_hdr_in_gamut(tried, matrix, peak))
				low = middle;
			else
				high = middle;
		}
		room = low;
	}
	else
		room = high;
	scale = xgpu_hdr_shoulder(1.0f + XGPU_HDR_GAMUT_CHROMA * weight, 1.0f + XGPU_HDR_GAMUT_KNEE * (room - 1.0f), room);
	tried[1] = lab[1] * scale;
	tried[2] = lab[2] * scale;
	xgpu_hdr_linear_of_oklab(tried, rgb);
}

/* the encoding of one pixel's colour (the frame's values after the tone
map and the HUD) for the display, in place: linear (by the SDR curve),
times the UI's white, at most the peak, times paper white (and PQ's primaries and curve) */
static inline void xgpu_hdr_encode(float rgb[3], const struct xgpu_hdr_frame *frame)
{
	float light[3], ui = frame->ui > 0.0f ? frame->ui : 1.0f;
	int channel;

	for (channel = 0; channel < 3; channel++)
	{
		if (frame->output == XGPU_HDR_OUTPUT_SDR)
		{
			rgb[channel] = rgb[channel] < 0.0f ? 0.0f : rgb[channel] > 1.0f ? 1.0f : rgb[channel];
			continue;
		}
		light[channel] = fminf(xgpu_hdr_sdr_light(rgb[channel], frame->sdr_curve) * ui, frame->peak);
	}
	if (frame->output == XGPU_HDR_OUTPUT_SDR)
		return;
	/* (display.hdr_gamut "expanded": past white, #1124) */
	if (frame->gamut)
		xgpu_hdr_gamut(light, frame->peak, ui > 1.0f ? ui : 1.0f, frame->gamut_target);
	for (channel = 0; channel < 3; channel++)
		light[channel] = xgpu_hdr_black(light[channel] * frame->paper_white, frame->black);
	if (frame->output != XGPU_HDR_OUTPUT_PQ)
	{
		for (channel = 0; channel < 3; channel++)
			rgb[channel] = light[channel];
		return;
	}
	{
		/* BT.709 to BT.2020 (the same colours: no saturation is added) */
		static const float to2020[3][3] =
		{
			{ 0.627404f, 0.329283f, 0.043313f },
			{ 0.069097f, 0.919540f, 0.011362f },
			{ 0.016391f, 0.088013f, 0.895595f },
		};
		const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;

		for (channel = 0; channel < 3; channel++)
		{
			float y = to2020[channel][0] * light[0] + to2020[channel][1] * light[1] + to2020[channel][2] * light[2];
			float p = powf(fmaxf(y, 0.0f), m1);

			rgb[channel] = powf((c1 + c2 * p) / (1.0f + c3 * p), m2);
		}
	}
}

/* the final write's dither (#1108; the encoding's pass, xgpu_hdr.c): a
pixel's static noise, triangular in [-1, 1] (interleaved gradient noise,
a high-frequency uniform pattern, through the triangle's inverse
distribution), and an output code (0 to 1) dithered to levels (255 for 8
bits, 1023 for 10; 0 none) by up to one level either way. Within half a
level of black and of white the noise narrows to what never rounds past
them (no clipping: a curve's code a fraction of a level up is shown on
average, about half of it, not black), and at black and white themselves
it is none, so that they stay exact */
static inline float xgpu_hdr_noise(float x, float y)
{
	float d = floorf(x) * 0.06711056f + floorf(y) * 0.00583715f, u;

	u = 52.9829189f * (d - floorf(d));
	u -= floorf(u);
	return u < 0.5f ? sqrtf(2.0f * u) - 1.0f : 1.0f - sqrtf(2.0f - 2.0f * u);
}

static inline float xgpu_hdr_dither(float code, float x, float y, float levels)
{
	float room;

	if (!(levels > 0.0f))
		return code;
	if (!(code > 0.0f && code < 1.0f))
		return code;
	room = fminf(code, 1.0f - code) * levels + 0.49f;
	room = room > 1.0f ? 1.0f : room;
	return code + xgpu_hdr_noise(x, y) * room / levels;
}

/* the enhanced dynamic lights' shoulder (xgpu_spirv.c, xgpu_msl.c):
the light a pixel had (base, the lights' gamma 2.2) with the dynamic light
added (linear); with lights (XGPU_HDR_LIGHTS) on past white */
static inline float xgpu_hdr_lights(float base, float light, int lights)
{
	float had = powf(base < 0.0f ? 0.0f : base > 1.0f ? 1.0f : base, 2.2f);
	float room = fmaxf(1.0f - had, 1.0e-3f);
	float result = had + room * (1.0f - expf(-light / room));

	if (lights)
	{
		float past = fmaxf(had + light - 1.0f, 0.0f);

		result += (XGPU_HDR_LIGHTS_WHITE - 1.0f) * (1.0f - expf(-past / (XGPU_HDR_LIGHTS_WHITE - 1.0f)));
	}
	return powf(result, 1.0f / 2.2f);
}

/* a pixel of the frame (its values, RGBA) for an SDR picture, as BGRA8:
tone mapped at a peak of 1, as an SDR display shows HDR (Faithful's, which
leaves a pixel no channel of which is past white as it is), for the
screenshot key's PNG */
static inline void xgpu_hdr_sdr_pixel(const float rgba[4], unsigned char bgra[4])
{
	float rgb[3] = { rgba[0], rgba[1], rgba[2] };
	int channel;

	/* (Faithful leaves a pixel no channel of which is past white as it is:
	most of a frame, whose six powf a pixel took a 4K frame more than a
	second on the render thread) */
	if (rgb[0] > 1.0f || rgb[1] > 1.0f || rgb[2] > 1.0f)
		xgpu_hdr_tone_map(rgb, XGPU_HDR_MODE_FAITHFUL, 1.0f);
	for (channel = 0; channel < 4; channel++)
	{
		float value = channel == 3 ? rgba[3] : rgb[channel];

		bgra[channel == 3 ? 3 : 2 - channel] =
			(unsigned char)(value <= 0.0f ? 0 : value >= 1.0f ? 255 : (int)(value * 255.0f + 0.5f));
	}
}
#endif

/* ---------- the classification (xgpu_hdr.c) */

/* what the front end knows of a draw into the HDR back buffer */
struct xgpu_hdr_draw
{
	/* the blend: enabled, XGPU_BLEND_* factors and operation; the channels
	written (XGPU_WRITE_*) */
	int blend, blend_source, blend_destination, blend_operation;
	int color_mask;
	/* the game's renderer's category (rasterizer.h's _rasterizer_profile_*,
	-1 outside one: halo_hdr_category) */
	int category;
	/* drawn over the tone mapped 3D view: the HUD and menus */
	int interface;
	/* what the game's code says of the draw (halo_hdr_draw_kind:
	HALO_HDR_KIND_*) */
	int kind;
	/* a structure's lightmap pass lit by the enhanced dynamic lights */
	int dynamic_lights;
	int mode;
	/* display.hdr_highlight_boost's level (0 none: XGPU_HDR_BOOST_MOST at
	most) */
	int boost;
};

/* halo_hdr_draw_kind's (port/linux/include/halo_linux_source_fixups.h has
them too) */
#ifndef HALO_HDR_KIND_NONE
#define HALO_HDR_KIND_NONE 0
/* a transparent part of the first-person weapon (and the bits below) */
#define HALO_HDR_KIND_FIRST_PERSON 1
/* ... its shader draws a number (the assault rifle's ammunition) */
#define HALO_HDR_KIND_NUMERIC 2
/* ... its shader is a meter (a plasma weapon's heat) */
#define HALO_HDR_KIND_METER 4
/* a glass or water shader's reflection: a surface's specular light */
#define HALO_HDR_KIND_SPECULAR 8
/* a deliberate glow of the HUD's: a meter flashing, the motion tracker */
#define HALO_HDR_KIND_HUD_GLOW 16
/* a surface that is light (in the 3D view): a transparent shader the map
says emits light (its radiosity power: a teleporter's field, a light
fixture), a plasma shader (an energy shield), a light's volume (a plasma
bolt's) */
#define HALO_HDR_KIND_LIGHT 32
/* a surface's pass that adds its self-illumination to its light (a
model's, a structure's lightmap pass), in the 3D view: the combiner stages
whose constants are its colours, a bit each, in the bits above 8
(HALO_HDR_KIND_SELF_ILLUMINATION_STAGES) */
#define HALO_HDR_KIND_SELF_ILLUMINATION 64
#define HALO_HDR_KIND_SELF_ILLUMINATION_STAGES(stages) ((stages) << 8)
/* an emitter of light the boost gains (#1123): a self-illuminated
particle, a glow, a sun's lens flare and its glow; ... the first-person
weapon's (its effects: a muzzle flash), toward the weapon's ceiling */
#define HALO_HDR_KIND_EMITTER 128
#define HALO_HDR_KIND_EMITTER_FIRST_PERSON (1 << 16)
#endif

/* ---------- the display, as the menus and the log show it (xgpu_hdr.c) */

/* what the backend says of its display (xgpu_backend_display_hdr): whether
it shows HDR, its headroom over its SDR white now, and that white in nits:
the system's (reported), or, where it gives none, the one the encoding
takes (XGPU_HDR_REFERENCE_WHITE_NITS on Vulkan, XGPU_HDR_EDR_WHITE_NITS on
Metal) */
struct xgpu_hdr_display
{
	int hdr;
	float headroom;
	float white_nits;
	int white_reported;
};

/* xgpu_hdr_display_text's texts: Video Setup's DISPLAY PEAK row's value
("800 NITS"), its panel's words, the log's line */
enum
{
	XGPU_HDR_DISPLAY_VALUE,
	XGPU_HDR_DISPLAY_WORDS,
	XGPU_HDR_DISPLAY_LOG,
};

/* the display's peak (its headroom times its SDR white) and SDR white, as
text of a kind (XGPU_HDR_DISPLAY_*) */
void xgpu_hdr_display_text(const struct xgpu_hdr_display *display, int kind, char *text, unsigned int size);

/* the tag (nv2a_pixel_shader_key.hdr) of a draw into the HDR back buffer */
unsigned short xgpu_hdr_classify(const struct xgpu_hdr_draw *draw);

/* the GLSL the tone map's and the encoding's shaders are made from
(tools/metal_post_shaders.py, tools/vulkan_post_shaders.py read them out of
xgpu_hdr.c) */
extern const char xgpu_hdr_functions_source[];
/* the tone map's (in place: the pixel drawn over read as the pass's input)
and the encoding's main functions */
extern const char *const xgpu_hdr_programs[2];

#endif
