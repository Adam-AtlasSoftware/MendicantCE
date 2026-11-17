/*
XGPU_HDR.C

display.hdr's shared half (xgpu_hdr.h): which draws into the HDR back
buffer may go past paper white, and how far (xgpu_hdr_classify, the front
end's: d3d8_device.c), and the GLSL of the tone map and the encoding, which
tools/metal_post_shaders.py and tools/vulkan_post_shaders.py compile for the
backends, so that every backend runs the same curves.

Which draws. A float target clips nothing, so the classification is of the
draws whose blend could take a pixel out of [0, 1] from values in it (no
pixel shader's result is outside it: the NV2A's clamp), from the blend
alone: an add whose two factors are not one and its complement, nor either
zero (an add, an alpha multiply add, a double multiply, a meter's constant
blend), and a subtraction or a factor of one less the destination's colour
(which a pixel past white makes negative). Of those, what the game draws as
light keeps what it adds: its additive effects, plasma, lens flares, the
sun's glare, the dynamic lights and their glows, a sky's additive layers.
What is a surface's look (a double multiply anywhere; in Faithful the
structure's specular light and reflections and glass's and water's; the
fog screens) and everything drawn over the tone mapped view (the HUD, the
menus, screen flashes) stops at paper white, or where the pixel already
was, as the SDR frame has it. The screen effects' passes that cannot blend
past white (a scope's warp and mask, which copy the view through their
target and back) carry the view's light past white through their pixel
stage unclamped (XGPU_HDR_UNCLAMPED). The deliberate glows the game's code names
(halo_hdr_draw_kind: the first-person weapon's displays, meters and
additive parts; the HUD's flashing meters and the motion tracker; the
additive surfaces that are light, which the game draws in [0, 1] though
they stand for light brighter than any surface: a transparent shader the
map gives radiosity power, as a teleporter's field and the light fixtures,
a plasma shader, a light's volume, as a plasma bolt's) are brightened
toward their ceilings and stop there. A surface's pass that adds its
self-illumination to its light (halo_hdr_draw_kind: a model's, a
structure's lightmap pass; a door's panel, a computer's screen) has that
self-illumination brightened past white, toward a lower ceiling than the
lights' (XGPU_HDR_SELF_ILLUMINATION), the rest as the SDR frame has it.
Destination alpha, which
the game reads as a blend factor (the specular passes, the fog screens), is
never let past 1.
*/

#include <stdio.h>

#include "xgpu_backend.h"
#include "xgpu_hdr.h"

/* ---------- the classification */

/* rasterizer.h's _rasterizer_profile_* (the categories halo_hdr_category
gives) */
enum
{
	_category_environment_specular_lightmaps = 12,
	_category_environment_reflection_lightmap_masks,
	_category_environment_reflection_mirrors,
	_category_environment_reflections,
	_category_environment_transparents,
	_category_environment_fog,
	_category_environment_fog_screen,
	_category_water,
	_category_screen_effect = 26,
	_category_hud,
	_category_screen_flash,
};

/* a factor as it acts on the alpha channel (the colour's factors take
alpha there) */
static int alpha_factor(int factor)
{
	switch (factor)
	{
	case XGPU_BLEND_SOURCE_COLOR: return XGPU_BLEND_SOURCE_ALPHA;
	case XGPU_BLEND_ONE_MINUS_SOURCE_COLOR: return XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case XGPU_BLEND_DESTINATION_COLOR: return XGPU_BLEND_DESTINATION_ALPHA;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case XGPU_BLEND_CONSTANT_COLOR: return XGPU_BLEND_CONSTANT_ALPHA;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA;
	default: return factor;
	}
}

/* one less the factor (-1: none of the factors) */
static int complement(int factor)
{
	switch (factor)
	{
	case XGPU_BLEND_ZERO: return XGPU_BLEND_ONE;
	case XGPU_BLEND_ONE: return XGPU_BLEND_ZERO;
	case XGPU_BLEND_SOURCE_COLOR: return XGPU_BLEND_ONE_MINUS_SOURCE_COLOR;
	case XGPU_BLEND_ONE_MINUS_SOURCE_COLOR: return XGPU_BLEND_SOURCE_COLOR;
	case XGPU_BLEND_SOURCE_ALPHA: return XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return XGPU_BLEND_SOURCE_ALPHA;
	case XGPU_BLEND_DESTINATION_ALPHA: return XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_ALPHA: return XGPU_BLEND_DESTINATION_ALPHA;
	case XGPU_BLEND_DESTINATION_COLOR: return XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR;
	case XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return XGPU_BLEND_DESTINATION_COLOR;
	case XGPU_BLEND_CONSTANT_COLOR: return XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return XGPU_BLEND_CONSTANT_COLOR;
	case XGPU_BLEND_CONSTANT_ALPHA: return XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA;
	case XGPU_BLEND_ONE_MINUS_CONSTANT_ALPHA: return XGPU_BLEND_CONSTANT_ALPHA;
	default: return -1;
	}
}

/* whether a channel (alpha's, or the colour's) can be blended past 1 from
a source and destination in [0, 1]: an add, unless a factor is zero (the
other term is at most 1), the factors are one and its complement (a mix),
or, for alpha, the source is whole and the destination kept by one less
its alpha (the "over" operator: a + d (1 - a)) */
static BOOL overflows(int source, int destination, int operation, BOOL alpha)
{
	if (operation != XGPU_BLEND_ADD || source == XGPU_BLEND_ZERO || destination == XGPU_BLEND_ZERO)
		return FALSE;
	if (alpha)
	{
		source = alpha_factor(source);
		destination = alpha_factor(destination);
		if (source == XGPU_BLEND_ONE && destination == XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA)
			return FALSE;
	}
	return complement(source) != destination;
}

/* whether the colour can be blended below 0: a subtraction, or one less
the destination's colour past white */
static BOOL underflows(int source, int destination, int operation)
{
	return operation == XGPU_BLEND_SUBTRACT || operation == XGPU_BLEND_REVERSE_SUBTRACT ||
		source == XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR || destination == XGPU_BLEND_ONE_MINUS_DESTINATION_COLOR;
}

/* a surface's look rather than light: kept under paper white */
static BOOL surface(const struct xgpu_hdr_draw *draw)
{
	BOOL multiply = (draw->blend_source == XGPU_BLEND_DESTINATION_COLOR &&
		draw->blend_destination == XGPU_BLEND_SOURCE_COLOR) ||
		(draw->blend_source == XGPU_BLEND_SOURCE_COLOR && draw->blend_destination == XGPU_BLEND_DESTINATION_COLOR);

	if (multiply || draw->interface)
		return TRUE;
	switch (draw->category)
	{
	case _category_environment_fog:
	case _category_environment_fog_screen:
	case _category_screen_effect:
	case _category_hud:
	case _category_screen_flash:
		return TRUE;
	case _category_environment_specular_lightmaps:
	case _category_environment_reflection_lightmap_masks:
	case _category_environment_reflection_mirrors:
	case _category_environment_reflections:
	case _category_water:
		return draw->mode == XGPU_HDR_MODE_FAITHFUL;
	default:
		return (draw->kind & HALO_HDR_KIND_SPECULAR) && draw->mode == XGPU_HDR_MODE_FAITHFUL;
	}
}

unsigned short xgpu_hdr_classify(const struct xgpu_hdr_draw *draw)
{
	int source = draw->blend ? draw->blend_source : XGPU_BLEND_ONE;
	int destination = draw->blend ? draw->blend_destination : XGPU_BLEND_ZERO;
	int operation = draw->blend ? draw->blend_operation : XGPU_BLEND_ADD;
	BOOL colour = (draw->color_mask & (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE)) != 0;
	BOOL additive = operation == XGPU_BLEND_ADD && (destination == XGPU_BLEND_ONE ||
		(source == XGPU_BLEND_ONE && destination == XGPU_BLEND_ONE_MINUS_SOURCE_ALPHA));
	BOOL out_of_range = colour && (overflows(source, destination, operation, FALSE) ||
		underflows(source, destination, operation));
	unsigned short hdr = 0;
	int glow = XGPU_HDR_CEILING_NONE;

	if (draw->kind & HALO_HDR_KIND_HUD_GLOW)
		glow = XGPU_HDR_CEILING_HUD;
	else if ((draw->kind & HALO_HDR_KIND_FIRST_PERSON) &&
		((draw->kind & (HALO_HDR_KIND_NUMERIC | HALO_HDR_KIND_METER)) || additive))
		glow = XGPU_HDR_CEILING_WEAPON;
	else if ((draw->kind & HALO_HDR_KIND_LIGHT) && additive && !draw->interface)
		glow = XGPU_HDR_CEILING_LIGHT;
	if (glow != XGPU_HDR_CEILING_NONE && colour)
		hdr = XGPU_HDR_CEILING(glow);
	else if (out_of_range && (surface(draw) || underflows(source, destination, operation)))
		hdr = XGPU_HDR_CEILING(XGPU_HDR_CEILING_PAPER_WHITE);
	else if (draw->category == _category_screen_effect && colour)
		return XGPU_HDR_UNCLAMPED;
	if ((draw->color_mask & XGPU_WRITE_ALPHA) && overflows(source, destination, operation, TRUE))
		hdr |= XGPU_HDR_ALPHA_LIMIT;
	if (XGPU_HDR_SHADER_BLENDS(hdr))
		hdr |= XGPU_HDR_BLEND(source, destination, operation);
	/* (a self-illuminated surface's pass: what its own blend leaves past
	white is its own, a ceiling or the alpha limit first) */
	else if ((draw->kind & HALO_HDR_KIND_SELF_ILLUMINATION) && colour && !draw->interface)
		hdr = XGPU_HDR_SELF_ILLUMINATION(draw->kind >> 8);
	/* (display.hdr_highlight_boost, #1123: a self-illuminated surface's
	pass, and an emitter's additive draw the fixed function blends, gained
	by its level; none of the HUD's or the menus') */
	if (draw->boost > 0 && colour && !draw->interface)
	{
		int level = draw->boost < XGPU_HDR_BOOST_MOST ? draw->boost : XGPU_HDR_BOOST_MOST;

		if (XGPU_HDR_IS_SELF_ILLUMINATED(hdr))
			hdr |= XGPU_HDR_BOOST(level);
		else if (!hdr && additive && (draw->kind & (HALO_HDR_KIND_EMITTER | HALO_HDR_KIND_EMITTER_FIRST_PERSON)))
			hdr = XGPU_HDR_EMITTER(level, draw->kind & HALO_HDR_KIND_EMITTER_FIRST_PERSON ? XGPU_HDR_CEILING_WEAPON :
				XGPU_HDR_CEILING_LIGHT);
	}
	if (draw->dynamic_lights)
		hdr |= XGPU_HDR_LIGHTS;
	return hdr;
}

/* ---------- the shaders' GLSL

The functions both shaders share: the frame's values made linear and
encoded again (sRGB's curve, extended past 1), the shoulder and the
highlights' curve, the tone map (the mode, the peak, the full-frame peak)
and the black lift, the encoding (hdr: paper white, the peak, the
output: xgpu_hdr.h's XGPU_HDR_OUTPUT_*; curve: the SDR curve, its toe
XGPU_HDR_SDR_TOE_START to _END), and the final write's dither (its static
triangular noise of a pixel, and a code dithered to levels), as
xgpu_hdr.h's reference has them. */
const char xgpu_hdr_functions_source[] =
	"const vec3 HDR_LUMA = vec3(0.2126, 0.7152, 0.0722);\n"
	"\n"
	"vec3 hdr_linear(vec3 value)\n"
	"{\n"
	"\tvec3 v = max(value, vec3(0.0));\n"
	"\treturn mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, lessThanEqual(v, vec3(0.04045)));\n"
	"}\n"
	"\n"
	"vec3 hdr_encoded(vec3 light)\n"
	"{\n"
	"\tvec3 l = max(light, vec3(0.0));\n"
	"\treturn mix(1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, l * 12.92, lessThanEqual(l, vec3(0.0031308)));\n"
	"}\n"
	"\n"
	"float hdr_shoulder(float x, float knee, float peak)\n"
	"{\n"
	"\tfloat range = peak - knee;\n"
	"\tif (x <= knee)\n"
	"\t\treturn x;\n"
	"\tif (range <= 0.0)\n"
	"\t\treturn peak;\n"
	"\tfloat t = (x - knee) / range;\n"
	"\treturn knee + range * t / (1.0 + t);\n"
	"}\n"
	"\n"
	"float hdr_highlight(float x, float knee, float peak, float full)\n"
	"{\n"
	"\tif (!(full > knee && full < peak))\n"
	"\t\treturn hdr_shoulder(x, knee, peak);\n"
	"\tif (x <= knee)\n"
	"\t\treturn x;\n"
	"\tfloat body = full - knee, rest = peak - full;\n"
	"\tfloat u = (x - knee) / body, v = (x - knee) / rest;\n"
	"\treturn knee + body * u / (1.0 + u) + rest * v * v / (1.0 + v * v);\n"
	"}\n"
	"\n"
	"vec3 hdr_tone_map(vec3 value, float mode, float peak, float full)\n"
	"{\n"
	"\tvec3 v = max(value, vec3(0.0));\n"
	"\tvec3 sdr = hdr_linear(min(v, vec3(1.0)));\n"
	"\tvec3 light = hdr_linear(v);\n"
	"\tfloat sdr_luminance = dot(sdr, HDR_LUMA);\n"
	"\tfloat luminance = dot(light, HDR_LUMA);\n"
	"\tfloat gain = 1.0, knee = 1.0;\n"
	"\tif (mode > 0.5)\n"
	"\t{\n"
	"\t\tgain = 1.2 * (1.0 + 0.6 * smoothstep(0.5, 1.0, sdr_luminance));\n"
	"\t\tknee = min(1.0, 0.75 * peak);\n"
	"\t}\n"
	"\telse if (luminance <= sdr_luminance)\n"
	"\t\treturn v;\n"
	"\tfloat clipped = smoothstep(1.0, 2.0, max(v.r, max(v.g, v.b)));\n"
	"\tfloat most = max(light.r, max(light.g, light.b)) * gain;\n"
	"\tfloat hue = most > 0.0 ? gain * hdr_highlight(most, knee, peak, full) / most : 0.0;\n"
	"\tfloat brightened = sdr_luminance > 0.0 ? hdr_highlight(gain * luminance / sdr_luminance, knee, peak, full) : 0.0;\n"
	"\treturn hdr_encoded(mix(light * hue, sdr * brightened, clipped));\n"
	"}\n"
	"\n"
	"vec3 hdr_sdr_light(vec3 value, float curve)\n"
	"{\n"
	"\tvec3 srgb = hdr_linear(value);\n"
	"\tif (curve < 0.5)\n"
	"\t\treturn srgb;\n"
	"\tvec3 v = max(value, vec3(0.0));\n"
	"\treturn mix(pow(v, vec3(2.2)), srgb, smoothstep(vec3(0.1), vec3(0.2), v));\n"
	"}\n"
	"\n"
	"float hdr_noise(vec2 pixel)\n"
	"{\n"
	"\tfloat u = fract(52.9829189 * fract(dot(floor(pixel), vec2(0.06711056, 0.00583715))));\n"
	"\treturn u < 0.5 ? sqrt(2.0 * u) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * u);\n"
	"}\n"
	"\n"
	"vec3 hdr_dither(vec3 code, vec2 pixel, float levels)\n"
	"{\n"
	"\tif (levels <= 0.0)\n"
	"\t\treturn code;\n"
	"\tvec3 room = clamp(min(code, 1.0 - code) * levels + 0.49, 0.0, 1.0);\n"
	"\troom *= vec3(greaterThan(code, vec3(0.0))) * vec3(lessThan(code, vec3(1.0)));\n"
	"\treturn code + hdr_noise(pixel) * room / levels;\n"
	"}\n"
	"\n"
	"vec3 hdr_black(vec3 light, float black)\n"
	"{\n"
	"\tif (!(black > 0.0))\n"
	"\t\treturn light;\n"
	"\tvec3 l = max(light, vec3(0.0));\n"
	"\tvec3 t = 1.0 - l / (4.0 * black);\n"
	"\treturn mix(light, l + black * t * t, lessThan(light, vec3(4.0 * black)));\n"
	"}\n"
	"\n"
	"vec3 hdr_oklab(vec3 c)\n"
	"{\n"
	"\tvec3 lms = mat3(0.4122214708, 0.2119034982, 0.0883024619, 0.5363325363, 0.6806995451, 0.2817188376,\n"
	"\t\t0.0514459929, 0.1073969566, 0.6299787005) * c;\n"
	"\tlms = pow(max(lms, vec3(0.0)), vec3(1.0 / 3.0));\n"
	"\treturn mat3(0.2104542553, 1.9779984951, 0.0259040371, 0.7936177850, -2.4285922050, 0.7827717662,\n"
	"\t\t-0.0040720468, 0.4505937099, -0.8086757660) * lms;\n"
	"}\n"
	"\n"
	"vec3 hdr_linear_of_oklab(vec3 lab)\n"
	"{\n"
	"\tvec3 lms = mat3(1.0, 1.0, 1.0, 0.3963377774, -0.1055613458, -0.0894841775, 0.2158037573, -0.0638541728,\n"
	"\t\t-1.2914855480) * lab;\n"
	"\tlms = lms * lms * lms;\n"
	"\treturn mat3(4.0767416621, -1.2684380046, -0.0041960863, -3.3077115913, 2.6097574011, -0.7034186147,\n"
	"\t\t0.2309699292, -0.3413193965, 1.7076147010) * lms;\n"
	"}\n"
	"\n"
	"mat3 hdr_gamut_matrix(float target)\n"
	"{\n"
	"\tmat3 p3 = mat3(0.822462, 0.033194, 0.017083, 0.177538, 0.966806, 0.072397, 0.0, 0.0, 0.910520);\n"
	"\tmat3 wide = mat3(0.627404, 0.069097, 0.016391, 0.329283, 0.919540, 0.088013, 0.043313, 0.011362, 0.895595);\n"
	"\tmat3 identity = mat3(1.0);\n"
	"\treturn target < 0.0 ? identity + (p3 - identity) * (1.0 + target) : p3 + (wide - p3) * target;\n"
	"}\n"
	"\n"
	"bool hdr_in_gamut(vec3 lab, mat3 to, float peak)\n"
	"{\n"
	"\tvec3 t = to * hdr_linear_of_oklab(lab);\n"
	"\treturn all(greaterThanEqual(t, vec3(-1.0e-5 * peak))) && all(lessThanEqual(t, vec3(peak * (1.0 + 1.0e-5))));\n"
	"}\n"
	"\n"
	"vec3 hdr_gamut(vec3 light, float peak, float white, float target)\n"
	"{\n"
	"\tfloat weight = smoothstep(white, 4.0 * white, max(light.r, max(light.g, light.b)));\n"
	"\tif (!(weight > 0.0))\n"
	"\t\treturn light;\n"
	"\tvec3 lab = hdr_oklab(light);\n"
	"\tif (!(length(lab.yz) >= 1.0e-6))\n"
	"\t\treturn light;\n"
	"\tmat3 to = hdr_gamut_matrix(target);\n"
	"\tfloat low = 1.0, high = 1.0 + 0.5, room = high;\n"
	"\tif (!hdr_in_gamut(vec3(lab.x, lab.yz * high), to, peak))\n"
	"\t{\n"
	"\t\tfor (int step = 0; step < 12; step++)\n"
	"\t\t{\n"
	"\t\t\tfloat middle = 0.5 * (low + high);\n"
	"\t\t\tif (hdr_in_gamut(vec3(lab.x, lab.yz * middle), to, peak))\n"
	"\t\t\t\tlow = middle;\n"
	"\t\t\telse\n"
	"\t\t\t\thigh = middle;\n"
	"\t\t}\n"
	"\t\troom = low;\n"
	"\t}\n"
	"\tfloat scale = hdr_shoulder(1.0 + 0.5 * weight, 1.0 + 0.75 * (room - 1.0), room);\n"
	"\treturn hdr_linear_of_oklab(vec3(lab.x, lab.yz * scale));\n"
	"}\n"
	"\n"
	"vec3 hdr_encode(vec3 value, vec4 hdr, float curve, float black, vec4 gamut)\n"
	"{\n"
	"\tif (hdr.z < 0.5)\n"
	"\t\treturn clamp(value, 0.0, 1.0);\n"
	"\tvec3 light = min(hdr_sdr_light(value, curve) * (hdr.w > 0.0 ? hdr.w : 1.0), vec3(hdr.y));\n"
	"\tif (gamut.x > 0.5)\n"
	"\t\tlight = hdr_gamut(light, hdr.y, max(hdr.w, 1.0), gamut.y);\n"
	"\tlight = hdr_black(light * hdr.x, black);\n"
	"\tif (hdr.z < 2.5)\n"
	"\t\treturn light;\n"
	"\tvec3 wide = mat3(0.627404, 0.069097, 0.016391, 0.329283, 0.919540, 0.088013, 0.043313, 0.011362, 0.895595) * light;\n"
	"\tvec3 p = pow(max(wide, vec3(0.0)), vec3(0.1593017578125));\n"
	"\treturn pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));\n"
	"}\n";

/* the tone map over a window's 3D view, in place: the pixel it draws over
read as the pass's input (Metal's programmable blending), its colour
written (the game's destination alpha kept), its light over the UI's
white (over paper white's: xgpu_hdr_to_ui) */
static const char tone_map_source[] =
	"layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput frame_input;\n"
	"/* the mode, the peak, the UI's white, the full-frame peak */\n"
	"uniform vec4 hdr;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec3 frame = subpassLoad(frame_input).rgb;\n"
	"\t/* (finite: a channel not a number as 0, an infinite one, the half\n"
	"\tfloats' sum past their most, as their most) */\n"
	"\tuvec3 bits = floatBitsToUint(frame);\n"
	"\tbvec3 special = equal(bits & uvec3(0x7f800000u), uvec3(0x7f800000u));\n"
	"\tbvec3 not_number = notEqual(bits & uvec3(0x007fffffu), uvec3(0u));\n"
	"\tbvec3 negative = notEqual(bits & uvec3(0x80000000u), uvec3(0u));\n"
	"\tframe = mix(frame, mix(mix(vec3(65504.0), vec3(-65504.0), negative), vec3(0.0), not_number), special);\n"
	"\tvec3 mapped = hdr_tone_map(frame, hdr.x, hdr.y, hdr.w);\n"
	"\tif (hdr.z > 0.0 && hdr.z != 1.0)\n"
	"\t\tmapped = hdr_encoded(hdr_linear(mapped) / hdr.z);\n"
	"\tresult = vec4(mapped, 1.0);\n"
	"}\n";

/* the frame's last pass, as it is drawn into the display's image (#1108):
the frame scaled over the viewport, encoded for the display (an SDR
output's values curved by display.gamma's power), then dithered to the
output's levels. metrics: the viewport's rectangle (x, y, 1 / width, 1 /
height); bounds: the frame's width and height and the sharp upscale's
whole factors across and down (xgpu_vulkan.c's present_sharp, in one tap:
the frame scaled up by them by nearest, then linearly to the viewport; 1, 1
linearly alone); window: the SDR curve (XGPU_HDR_SDR_CURVE_*), the levels
a channel is dithered to (255, 1023; 0 none), the power (1 none), the
display's black (xgpu_hdr_frame.black: 0 none); gamut: display.hdr_gamut
"expanded" (x: 1) and its target (y: xgpu_hdr_frame.gamut_target) */
static const char encode_source[] =
	"uniform sampler2D color_texture;\n"
	"/* the viewport: x, y, 1 / width, 1 / height */\n"
	"uniform vec4 metrics;\n"
	"/* the frame's width and height, the sharp upscale's factors */\n"
	"uniform vec4 bounds;\n"
	"/* paper white, the peak, the output, the UI's white */\n"
	"uniform vec4 hdr;\n"
	"/* the SDR curve, the levels dithered to, display.gamma's power, the\n"
	"display's black */\n"
	"uniform vec4 window;\n"
	"/* display.hdr_gamut: expanded (1), its target */\n"
	"uniform vec4 gamut;\n"
	"out vec4 result;\n"
	"\n"
	"/* the texture coordinates of a point of the viewport (its pixels from\n"
	"its corner): where it falls in the frame scaled up by the factors\n"
	"(between the scaled texels' centres), the two texels of the frame\n"
	"those are, blended only where they differ */\n"
	"vec2 present_position(vec2 at)\n"
	"{\n"
	"\tvec2 t = at * bounds.xy * bounds.zw * metrics.zw - 0.5;\n"
	"\tvec2 j = floor(t);\n"
	"\tvec2 a = floor(j / bounds.zw);\n"
	"\tvec2 b = floor((j + 1.0) / bounds.zw);\n"
	"\treturn (a + 0.5 + (t - j) * vec2(notEqual(a, b))) / bounds.xy;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec3 frame = textureLod(color_texture, present_position(gl_FragCoord.xy - metrics.xy), 0.0).rgb;\n"
	"\tvec3 code = hdr_encode(frame, hdr, window.x, window.w, gamut);\n"
	"\tif (hdr.z < 0.5 && window.z > 0.0 && window.z != 1.0)\n"
	"\t\tcode = pow(code, vec3(window.z));\n"
	"\tresult = vec4(hdr_dither(code, gl_FragCoord.xy, window.y), 1.0);\n"
	"}\n";

/* ---------- the display */

void xgpu_hdr_display_text(const struct xgpu_hdr_display *display, int kind, char *text, unsigned int size)
{
	float headroom = display->headroom >= 1.0f ? display->headroom : 1.0f;
	float white = display->white_nits > 0.0f ? display->white_nits : XGPU_HDR_REFERENCE_WHITE_NITS;
	int peak = (int)(headroom * white + 0.5f), nits = (int)(white + 0.5f);

	if (!size)
		return;
	if (!display->hdr)
	{
		snprintf(text, size, "%s", kind == XGPU_HDR_DISPLAY_VALUE ? "SDR" :
			kind == XGPU_HDR_DISPLAY_WORDS ? "The display reports no HDR: it shows the game in SDR." :
			"hdr: the display reports no HDR");
		return;
	}
	switch (kind)
	{
	case XGPU_HDR_DISPLAY_VALUE:
		snprintf(text, size, "%d NITS", peak);
		break;
	case XGPU_HDR_DISPLAY_WORDS:
		if (display->white_reported)
			snprintf(text, size, "Display peak %d nits, SDR white %d nits, as the system reports them. Paper white "
				"and UI brightness go no brighter than the peak.", peak, nits);
		else
			snprintf(text, size, "Display peak %d nits. SDR white not reported: using %d nits. Paper white and UI "
				"brightness go no brighter than the peak.", peak, nits);
		break;
	default:
		if (display->white_reported)
			snprintf(text, size, "hdr: display peak %d nits, SDR white %d nits, from the system", peak, nits);
		else
			snprintf(text, size, "hdr: display peak %d nits, SDR white not reported, using %d nits (headroom "
				"%.2f)", peak, nits, (double)headroom);
		break;
	}
}

/* (the generators read them out of this file's text; nothing here draws
with them) */
const char *const xgpu_hdr_programs[2] = { tone_map_source, encode_source };
