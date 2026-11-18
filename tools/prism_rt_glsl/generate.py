#!/usr/bin/env python3
"""Writes port/linux/vulkan/xgpu_vulkan_rt_glsl.inc (display.prism_rt's GLSL as C string literals) from the
shaders below, which share their Pass block and helpers, and the text around them in this folder (header.txt,
path_comment.txt, tail.txt: the comments and the object passes' shaders, kept as written). Edit this file, not
the .inc; then run tools/vulkan_rt_shaders.py, which compiles the .inc to SPIR-V:

    python3 tools/prism_rt_glsl/generate.py           # writes the .inc
    python3 tools/prism_rt_glsl/generate.py --check   # fails if the .inc is not what this writes
    python3 tools/vulkan_rt_shaders.py && python3 tools/vulkan_rt_shaders.py --check
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TAIL = (HERE / "tail.txt").read_text()

PASS = """layout(set = 0, binding = @B@, std140) uniform Pass
{
    vec4 camera[4];
    ivec4 corners;
    vec4 previous[4];
    ivec4 previous_corners;
    vec4 sun;
    vec4 strength;
    vec4 rays;
    uvec4 info;
    ivec4 size;
    vec4 probe_origin;
    ivec4 probe_count;
    uvec4 probe_info;
    vec4 sky;
    vec4 sun_radiance;
    uvec4 addresses;
    uvec4 addresses2;
    vec4 pt;
    vec4 pt2;
    vec4 lights[192];
    uvec4 addresses3;
    uvec4 addresses4;
    uvec4 counts;
    vec4 pt3;
    vec4 rig[16];
    vec4 motion[64];
    uvec4 addresses5;
    vec4 grid;
    ivec4 grid_size;
    vec4 reference;
} pass;
"""

BASIC = """
vec3 unfold(vec2 f)
{
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

float luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 eye_ray(ivec2 at)
{
    vec2 s = 2.0 * (vec2(at) + 0.5 - vec2(pass.corners.xy)) / vec2(pass.corners.zw - pass.corners.xy) - 1.0;
    return normalize(pass.camera[1].xyz + s.x * pass.camera[2].xyz + s.y * pass.camera[3].xyz);
}

/* the last frame's pixel of a point (its camera), or -1 */
ivec2 reproject(vec3 p, uint flags, out float old_distance)
{
    /* (an object's point where its object was the frame before: its
    owner's motion, pass.motion, the inputs' low byte the owner + 2) */
    int owner = int(flags & 0xffu) - 2;
    if ((flags & 0x100u) == 0u && owner >= 0 && owner < 64)
        p -= pass.motion[owner].xyz;
    vec3 v = p - pass.previous[0].xyz;
    old_distance = length(v);
    float along = dot(v, pass.previous[1].xyz);
    if (along <= 0.0)
        return ivec2(-1);
    vec3 u = v / along - pass.previous[1].xyz;
    vec2 ps = vec2(dot(u, pass.previous[2].xyz) / dot(pass.previous[2].xyz, pass.previous[2].xyz),
        dot(u, pass.previous[3].xyz) / dot(pass.previous[3].xyz, pass.previous[3].xyz));
    vec2 pixel = vec2(pass.previous_corners.xy) + (ps + 1.0) * 0.5 *
        vec2(pass.previous_corners.zw - pass.previous_corners.xy);
    ivec2 q = ivec2(floor(pixel));
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, pass.size.xy)))
        return ivec2(-1);
    return q;
}

/* the light in CE's encoding: the rig's linear light to 1 / the power its
pages were fitted in (pass.rig[1].w) */
vec3 encoded(vec3 light)
{
    float g = pass.rig[1].w;
    return g > 1.001 ? pow(max(light, vec3(0.0)), vec3(1.0 / g)) : max(light, vec3(0.0));
}
"""

# the helpers of the shaders that trace: the triangles' corners, materials and alpha maps, the rays (with the
# alpha-tested structure's candidates tested against their alpha maps), the rig's lights
TRACE = """
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Floats { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Uints { uint v[]; };

/* the structure's alpha-tested triangles' instance (the opaque's is 0, the
objects' 1 on) */
const uint ALPHA_CUSTOM = 0xffffffu;

uint seed;

/* a 32-bit hash (PCG's output permutation on one LCG step, Jarzynski and
Olano 2020): the seeds of neighbouring pixels and frames scattered, so that
their sequences share nothing (a seed linear in x, y and the frame let
neighbours' first numbers run together, and the filters made their shared
error into patches) */
uint hash_u(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint seed_of(ivec2 texel, uint frame, uint salt)
{
    return hash_u(uint(texel.x) ^ hash_u(uint(texel.y) ^ hash_u(frame * 4u + salt)));
}

float rnd()
{
    seed = seed * 747796405u + 2891336453u;
    uint w = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    w = (w >> 22u) ^ w;
    return float(w & 0xffffffu) / 16777216.0;
}

/* the path tracer's world-space radiance cache (a hashed grid, after the
path-space filtering of Binder et al. 2019): each cell (a world-space cube
whose side doubles with the distance from the eye, by the surface's facing
axis) gathers the light less the sun's and the lamps' (the domes, the sky,
the bounces) its pixels' paths brought this frame, and the resolve pass
blends that into the cell's light over 256 samples; a level's pixel takes
its cell's light in place of its own path's (one path a pixel: raw, a sky
seen or not), so that what the filters see is already the mean of a
cell's hundreds. Entry: its check (0: empty), the frame it was last
written, the frame's sum (red, green, blue, fixed point by CACHE_FIXED)
and count, the resolved light (floats) and its weight */
layout(buffer_reference, std430, buffer_reference_align = 4) buffer CacheWords { uint v[]; };
const uint CACHE_ENTRIES = 524288u;
const uint CACHE_WORDS = 10u;
const float CACHE_FIXED = 256.0;
/* (a cell's weight from which its light stands alone) */
const float CACHE_SETTLED = 64.0;

/* the cell of a point (normal n, distance from the eye): its entry's
index, found or made (make), or -1 */
int cache_entry(vec3 p, vec3 n, float distance, bool make)
{
    if (pass.addresses5.z == 0u && pass.addresses5.w == 0u)
        return -1;
    CacheWords c = CacheWords(pass.addresses5.zw);
    float side = max(0.125, exp2(ceil(log2(max(distance * 0.01, 1.0e-4)))));
    int level = int(round(log2(side / 0.125)));
    ivec3 cell = ivec3(floor(p / side));
    vec3 a = abs(n);
    uint face = a.x >= a.y && a.x >= a.z ? (n.x > 0.0 ? 0u : 1u) : a.y >= a.z ? (n.y > 0.0 ? 2u : 3u) :
        (n.z > 0.0 ? 4u : 5u);
    uint h = hash_u(uint(cell.x) ^ hash_u(uint(cell.y) ^ hash_u(uint(cell.z) ^ hash_u(uint(level) * 8u + face))));
    uint check = hash_u(h ^ 0x5bd1e995u) | 1u;
    for (uint probe = 0u; probe < 8u; probe++)
    {
        uint index = (h + probe) & (CACHE_ENTRIES - 1u);
        uint at = index * CACHE_WORDS;
        uint was = c.v[at];
        if (was == check)
            return int(index);
        if (was == 0u)
        {
            if (!make)
                return -1;
            uint got = atomicCompSwap(c.v[at], 0u, check);
            if (got == 0u || got == check)
                return int(index);
        }
    }
    return -1;
}

void cache_add(int entry, vec3 light)
{
    CacheWords c = CacheWords(pass.addresses5.zw);
    uint at = uint(entry) * CACHE_WORDS;
    uvec3 fixed_ = uvec3(clamp(light, vec3(0.0), vec3(1.0e4)) * CACHE_FIXED + 0.5);
    atomicAdd(c.v[at + 2u], fixed_.x);
    atomicAdd(c.v[at + 3u], fixed_.y);
    atomicAdd(c.v[at + 4u], fixed_.z);
    atomicAdd(c.v[at + 5u], 1u);
    c.v[at + 1u] = pass.info.x;
}

/* the cell's resolved light and its weight (0: none yet) */
vec4 cache_read(int entry)
{
    CacheWords c = CacheWords(pass.addresses5.zw);
    uint at = uint(entry) * CACHE_WORDS;
    return vec4(uintBitsToFloat(c.v[at + 6u]), uintBitsToFloat(c.v[at + 7u]), uintBitsToFloat(c.v[at + 8u]),
        uintBitsToFloat(c.v[at + 9u]));
}

/* a structure triangle's place in the traced set (the alpha-tested after
the opaque) */
uint structure_triangle(uint custom, uint primitive)
{
    return custom == 0u ? primitive : primitive + pass.counts.x;
}

vec3 corner(uint custom, uint primitive, uint k)
{
    if (custom == 0u || custom == ALPHA_CUSTOM)
    {
        Floats f = Floats(pass.addresses.xy);
        uint b = (structure_triangle(custom, primitive) * 3u + k) * 3u;
        return vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);
    }
    Uints t = Uints(pass.addresses.zw);
    Floats f = Floats(uvec2(t.v[(custom - 1u) * 4u], t.v[(custom - 1u) * 4u + 1u]));
    uint b = (primitive * 3u + k) * 3u;
    return vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);
}

/* a structure triangle's material (the materials' count where none) */
uint material_of(uint triangle)
{
    if (pass.counts.w == 0u || (pass.addresses3.x == 0u && pass.addresses3.y == 0u))
        return pass.counts.w;
    uint m = Uints(pass.addresses3.xy).v[triangle * 8u] & 0xffffu;
    return min(m, pass.counts.w);
}

float material_value(uint m, uint k)
{
    return Floats(pass.addresses3.zw).v[m * 16u + k];
}

/* a candidate kept: the alpha-tested structure's where its alpha map's
texel at the point (nearest) is past the material's reference; a cutout
object's (foliage: mask 8) by the chance of its share of texels drawn */
/* (the cutouts a ray passes within this distance of its start (5 world
units: a tree's crown): a ray from an object's pixel, a leaf's among them,
through its own canopy, whose
leaves' holes are not traced one by one: CE lit a tree's leaves whole) */
float cutout_skip = 0.0;

bool alpha_kept(uint custom, uint primitive, vec2 b, float distance)
{
    if (custom != ALPHA_CUSTOM)
    {
        if (distance < cutout_skip)
            return false;
        /* (a leaf lets a quarter of the light through: kept by three
        quarters of its share) */
        float coverage = uintBitsToFloat(Uints(pass.addresses.zw).v[(custom - 1u) * 4u + 3u]);
        return rnd() < 0.75 * coverage;
    }
    uint t = primitive + pass.counts.x;
    uint m = material_of(t);
    if (m >= pass.counts.w || (uint(material_value(m, 3u)) & 2u) == 0u)
        return true;
    Uints a = Uints(pass.addresses3.xy);
    vec2 uv0 = uintBitsToFloat(uvec2(a.v[t * 8u + 1u], a.v[t * 8u + 2u]));
    vec2 uv1 = uintBitsToFloat(uvec2(a.v[t * 8u + 3u], a.v[t * 8u + 4u]));
    vec2 uv2 = uintBitsToFloat(uvec2(a.v[t * 8u + 5u], a.v[t * 8u + 6u]));
    vec2 uv = uv0 * (1.0 - b.x - b.y) + uv1 * b.x + uv2 * b.y;
    float w = material_value(m, 9u), h = material_value(m, 10u);
    uvec2 texel = uvec2(clamp(fract(uv) * vec2(w, h), vec2(0.0), vec2(w, h) - 1.0));
    uint at = uint(material_value(m, 8u)) + texel.y * uint(w) + texel.x;
    uint alpha = (Uints(pass.addresses4.xy).v[at >> 2u] >> ((at & 3u) * 8u)) & 0xffu;
    return float(alpha) * (1.0 / 255.0) > material_value(m, 7u);
}

/* the ray's flags: opaque at once where neither the alpha-tested
structure (mask 2) nor the cutouts (mask 8) are in its mask (their
candidates are tested, ALPHA_PROCEED) */
uint ray_flags(uint mask, bool first)
{
    return (first ? gl_RayFlagsTerminateOnFirstHitEXT : 0u) | ((mask & 10u) == 0u ? gl_RayFlagsOpaqueEXT : 0u);
}

#define ALPHA_PROCEED(q) while (rayQueryProceedEXT(q)) { \\
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && \\
            alpha_kept(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false), \\
            uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), \\
            rayQueryGetIntersectionBarycentricsEXT(q, false), rayQueryGetIntersectionTEXT(q, false))) \\
            rayQueryConfirmIntersectionEXT(q); }

bool blocked(vec3 origin, vec3 direction, float t_max, uint mask)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, ray_flags(mask, true), mask, origin, 0.0, direction, t_max);
    ALPHA_PROCEED(q)
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

float nearest(vec3 origin, vec3 direction, float t_max, uint mask)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, ray_flags(mask, false), mask, origin, 0.0, direction, t_max);
    ALPHA_PROCEED(q)
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return t_max;
    return rayQueryGetIntersectionTEXT(q, true);
}

/* the nearest hit: t (negative none), its face normal toward the ray (0
where it is the level's back: inside it), its instance's custom index and
its triangle */
float hit(vec3 origin, vec3 direction, float t_max, uint mask, out vec3 nh, out uint custom, out uint primitive)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, ray_flags(mask, false), mask, origin, 0.0, direction, t_max);
    ALPHA_PROCEED(q)
    nh = vec3(0.0);
    custom = 0u;
    primitive = 0u;
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return -1.0;
    custom = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    vec3 a0 = corner(custom, primitive, 0u), a1 = corner(custom, primitive, 1u), a2 = corner(custom, primitive, 2u);
    vec3 n = cross(a1 - a0, a2 - a0);
    n = dot(n, n) > 1.0e-20 ? normalize(n) : -direction;
    bool level = custom == 0u || custom == ALPHA_CUSTOM;
    /* (the level's opaque faces are one-sided: its back seen is inside it,
    no light, where the level is the map's (pass.pt3.w); a remodel's are
    wound either way, two-sided as an object's and a leaf's card) */
    if (dot(n, direction) > 0.0)
        n = level && custom == 0u && pass.pt3.w < 0.5 ? vec3(0.0) : -n;
    nh = n;
    return rayQueryGetIntersectionTEXT(q, true);
}

/* a hit's albedo (display.prism_rt_materials: "grey" 0.45; "average" the
material's mean, an object's base map's mean) */
vec3 albedo_of(uint custom, uint primitive)
{
    if (pass.rig[11].y < 0.5)
        return vec3(0.45);
    if (custom == 0u || custom == ALPHA_CUSTOM)
    {
        uint m = material_of(structure_triangle(custom, primitive));
        if (m >= pass.counts.w)
            return vec3(0.35);
        return vec3(material_value(m, 0u), material_value(m, 1u), material_value(m, 2u));
    }
    uint packed = Uints(pass.addresses.zw).v[(custom - 1u) * 4u + 2u];
    if ((packed >> 24u) == 0u)
        return vec3(0.35);
    return clamp(unpackUnorm4x8(packed).rgb, vec3(0.03), vec3(0.85));
}

/* a hit's glow (display.prism_rt_emissive "all": the self-illumination
maps' light, by display.prism_rt_emissive_scale; not the lamps', which are
sampled as lights) */
vec3 glow_of(uint custom, uint primitive)
{
    if (pass.rig[10].w < 1.5 || (custom != 0u && custom != ALPHA_CUSTOM))
        return vec3(0.0);
    uint m = material_of(structure_triangle(custom, primitive));
    if (m >= pass.counts.w || (uint(material_value(m, 3u)) & 4u) == 0u)
        return vec3(0.0);
    return vec3(material_value(m, 11u), material_value(m, 12u), material_value(m, 13u)) * pass.rig[11].x;
}

void basis(vec3 n, out vec3 t, out vec3 b)
{
    t = normalize(abs(n.z) < 0.95 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    b = cross(n, t);
}

vec3 cosine_dir(vec3 n)
{
    vec3 t, b;
    basis(n, t, b);
    float u = rnd(), a = rnd() * 6.2831853;
    return normalize(sqrt(u) * (cos(a) * t + sin(a) * b) + sqrt(max(1.0 - u, 0.0)) * n);
}

/* a direction in the cone about axis whose half angle's cosine is
cos_most, uniformly */
vec3 cone_dir(vec3 axis, float cos_most)
{
    vec3 t, b;
    basis(axis, t, b);
    float c = mix(1.0, cos_most, rnd()), s = sqrt(max(1.0 - c * c, 0.0)), a = rnd() * 6.2831853;
    return normalize(c * axis + s * (cos(a) * t + sin(a) * b));
}

/* the sun's visibility from o (its disc, display.prism_sun_size: one ray
toward a point of it) through mask */
float sun_visible(vec3 o, uint mask)
{
    if (pass.sun.w <= 0.0)
        return 0.0;
    vec3 d = cone_dir(pass.sun.xyz, cos(0.5 * pass.rig[0].w));
    return blocked(o, d, 2000.0, mask) ? 0.0 : 1.0;
}

/* the sun's light at a point (the rig's: its radiance by the facing, where
it sees the sun) */
vec3 sun_at(vec3 o, vec3 n)
{
    float c = dot(n, pass.sun.xyz);
    if (pass.sun.w <= 0.0 || c <= 0.0)
        return vec3(0.0);
    return pass.rig[0].rgb * c * sun_visible(o, 15u);
}

/* the domes' light at a point: each a direction of its cone (the bake's
sun lamp of that angle: its strength the light facing it squarely) */
vec3 domes_at(vec3 o, vec3 n)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; i++)
    {
        vec4 a = pass.rig[2 + 2 * i], r = pass.rig[3 + 2 * i];
        if (dot(r.rgb, vec3(1.0)) <= 0.0)
            continue;
        vec3 d = cone_dir(normalize(a.xyz), a.w);
        float c = dot(n, d);
        if (c > 0.0 && !blocked(o, d, 2000.0, 15u))
            sum += r.rgb * c;
    }
    return sum;
}

/* the sky's light along a direction past the level: its radiance, and
where the direction is in a dome's cone the dome's (its light facing it
squarely, by pi over its solid angle) unless the domes were sampled as
lights */
vec3 sky_along(vec3 d, bool domes)
{
    vec3 l = pass.rig[1].rgb;
    if (!domes)
        return l;
    for (int i = 0; i < 4; i++)
    {
        vec4 a = pass.rig[2 + 2 * i], r = pass.rig[3 + 2 * i];
        if (dot(r.rgb, vec3(1.0)) > 0.0 && dot(d, normalize(a.xyz)) >= a.w)
            l += r.rgb * 3.14159265 / max(6.2831853 * (1.0 - a.w), 1.0e-4);
    }
    return l;
}

/* whether a lamp's point at distance d along l is seen from o: nothing
before it, or first another lamp's surface (a lamp of several triangles,
a strip in its housing: the light reaches as the bake's emitters' does,
whichever of them a ray meets) */
bool lamp_visible(vec3 o, vec3 l, float d)
{
    /* (most rays meet nothing: the first hit's test settles them; only a
    ray that meets something looks for the nearest) */
    if (!blocked(o, l, max(d * 0.995 - 0.03, 0.0), 15u))
        return true;
    vec3 nh;
    uint custom, primitive;
    float t = hit(o, l, max(d * 0.995 - 0.03, 0.0), 15u, nh, custom, primitive);
    if (t < 0.0)
        return true;
    if (custom != 0u && custom != ALPHA_CUSTOM)
        return false;
    uint m = material_of(structure_triangle(custom, primitive));
    return m < pass.counts.w && (uint(material_value(m, 3u)) & 1u) != 0u;
}

/* a lamp's point (its triangle, two barycentrics): its light at p
(normal n), unshadowed (the bake's one-sided emitter), the direction and
distance to it, and the density its pick by power and place gave it */
/* the lamps' grid (xgpu_vulkan_rt.c's lamp_grid_build): the cell about p
(its first word's index), or -1 outside it or none made */
int lamp_cell(vec3 p)
{
    if (pass.grid_size.x <= 0)
        return -1;
    ivec3 c = ivec3(floor((p - pass.grid.xyz) / pass.grid.w));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, pass.grid_size.xyz)))
        return -1;
    return ((c.z * pass.grid_size.y + c.y) * pass.grid_size.x + c.x) * 33;
}

/* the share of the picks from the cell's lamps (the rest from the level's
by power: a lamp no cell lists is still found) */
const float LAMP_CELL_SHARE = 0.85;

/* a lamp's pick's probability at p: the cell's share by its weight there
(where the cell lists it), the rest by its power */
float lamp_probability(uint lamp, vec3 p, float power)
{
    float global_ = power / max(pass.pt3.x, 1.0e-20);
    int cell = lamp_cell(p);
    if (cell < 0)
        return global_;
    Uints g = Uints(pass.addresses5.xy);
    uint count = g.v[cell];
    if (count == 0u)
        return global_;
    float total = uintBitsToFloat(g.v[cell + 2 * int(count)]);
    float mine = 0.0, below = 0.0;
    for (uint k = 0u; k < count; k++)
    {
        float cumulative = uintBitsToFloat(g.v[cell + 2 + 2 * int(k)]);
        if (g.v[cell + 1 + 2 * int(k)] == lamp)
            mine = cumulative - below;
        below = cumulative;
    }
    return LAMP_CELL_SHARE * mine / max(total, 1.0e-20) + (1.0 - LAMP_CELL_SHARE) * global_;
}

vec3 lamp_point(uint lamp, vec2 b, vec3 p, vec3 n, out vec3 l, out float d, out float density)
{
    Floats f = Floats(pass.addresses4.zw);
    uint at = lamp * 16u;
    vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
    vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);
    vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);
    vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);
    float area = f.v[at + 12u];
    /* (the point's density: its lamp's pick's probability over its area) */
    density = lamp_probability(lamp, p, luma(radiance) * area) / max(area, 1.0e-20);
    radiance *= pass.rig[11].x;
    vec3 q = (1.0 - b.x - b.y) * a0 + b.x * a1 + b.y * a2;
    vec3 ne = normalize(cross(a1 - a0, a2 - a0));
    vec3 to = q - p;
    float d2 = max(dot(to, to), 1.0e-6);
    d = sqrt(d2);
    l = to / d;
    float cr = dot(n, l), ce = dot(ne, -l);
    if (cr <= 0.0 || ce <= 0.0 || area <= 0.0)
        return vec3(0.0);
    return radiance * cr * ce / (d2 * 3.14159265);
}

/* a lamp picked by its power (the cumulative power's search) and a point
of it, uniformly: its triangle and barycentrics */
uint lamp_pick(out vec2 b)
{
    Floats f = Floats(pass.addresses4.zw);
    uint count = pass.counts.z;
    float u = rnd() * pass.pt3.x;
    uint lo = 0u, hi = count - 1u;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (f.v[mid * 16u + 13u] < u)
            lo = mid + 1u;
        else
            hi = mid;
    }
    float r1 = sqrt(rnd()), r2 = rnd();
    b = vec2(r1 * (1.0 - r2), r1 * r2);
    return lo;
}

/* a lamp picked at p: from its grid cell's lamps by their weights there
(LAMP_CELL_SHARE of the picks), else by its power (the cumulative power's
search); a point of it, uniformly: its triangle and barycentrics */
uint lamp_pick_at(vec3 p, out vec2 b)
{
    int cell = lamp_cell(p);
    if (cell >= 0 && rnd() < LAMP_CELL_SHARE)
    {
        Uints g = Uints(pass.addresses5.xy);
        uint count = g.v[cell];
        if (count > 0u)
        {
            float u = rnd() * uintBitsToFloat(g.v[cell + 2 * int(count)]);
            uint k = 0u;
            while (k + 1u < count && uintBitsToFloat(g.v[cell + 2 + 2 * int(k)]) < u)
                k++;
            float r1 = sqrt(rnd()), r2 = rnd();
            b = vec2(r1 * (1.0 - r2), r1 * r2);
            return g.v[cell + 1 + 2 * int(k)];
        }
    }
    return lamp_pick(b);
}


/* a lamp (the light cards, the lamps' surfaces): picked by its power (the
cumulative power's search), a point of it; the light it gives at p (normal
n), unshadowed, as the bake's one-sided emitter, over the pick's density;
the direction and distance to the point */
vec3 lamp_sample(vec3 p, vec3 n, out vec3 l, out float d, out float weight)
{
    weight = 0.0;
    l = vec3(0.0, 0.0, 1.0);
    d = 0.0;
    uint count = pass.counts.z;
    float total = pass.pt3.x;
    if (count == 0u || total <= 0.0 || pass.rig[10].w < 0.5)
        return vec3(0.0);
    Floats f = Floats(pass.addresses4.zw);
    float u = rnd() * total;
    uint lo = 0u, hi = count - 1u;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (f.v[mid * 16u + 13u] < u)
            lo = mid + 1u;
        else
            hi = mid;
    }
    uint b = lo * 16u;
    vec3 a0 = vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);
    vec3 a1 = vec3(f.v[b + 3u], f.v[b + 4u], f.v[b + 5u]);
    vec3 a2 = vec3(f.v[b + 6u], f.v[b + 7u], f.v[b + 8u]);
    vec3 radiance = vec3(f.v[b + 9u], f.v[b + 10u], f.v[b + 11u]) * pass.rig[11].x;
    float area = f.v[b + 12u];
    float power = luma(radiance / max(pass.rig[11].x, 1.0e-6)) * area;
    float r1 = sqrt(rnd()), r2 = rnd();
    vec3 q = (1.0 - r1) * a0 + r1 * (1.0 - r2) * a1 + r1 * r2 * a2;
    vec3 ne = normalize(cross(a1 - a0, a2 - a0));
    vec3 to = q - p;
    float d2 = max(dot(to, to), 1.0e-6);
    d = sqrt(d2);
    l = to / d;
    float cr = dot(n, l), ce = dot(ne, -l);
    if (cr <= 0.0 || ce <= 0.0 || power <= 0.0)
        return vec3(0.0);
    /* (the light's estimate over the pick's density: its power's share,
    one over its area) */
    float density = power / total / area;
    weight = 1.0 / density;
    return radiance * cr * ce / (d2 * 3.14159265);
}

/* the lamps' light at a point: candidates picked by power, one kept by its
light there (resampled importance), its shadow traced */
vec3 lamps_at(vec3 p, vec3 n, vec3 o, int candidates)
{
    vec3 kept = vec3(0.0), kept_l = vec3(0.0);
    float kept_d = 0.0, kept_target = 0.0, sum = 0.0;
    for (int c = 0; c < candidates; c++)
    {
        vec3 l;
        float d, weight;
        vec3 e = lamp_sample(p, n, l, d, weight);
        float target = luma(e);
        float w = target * weight;
        sum += w;
        if (w > 0.0 && rnd() * sum <= w)
        {
            kept = e;
            kept_l = l;
            kept_d = d;
            kept_target = target;
        }
    }
    if (kept_target <= 0.0 || sum <= 0.0)
        return vec3(0.0);
    /* (at a path's hits and the probes': the first hit's test alone) */
    if (blocked(o, kept_l, max(kept_d * 0.995 - 0.03, 0.0), 15u))
        return vec3(0.0);
    return kept / kept_target * (sum / float(candidates));
}
"""


def literal(glsl):
    out = []
    for line in glsl.split("\n"):
        out.append('\t"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"')
    return "\n".join(out)


def source(name, head, body, bindings_pass):
    glsl = head + PASS.replace("@B@", str(bindings_pass)) + body
    return "static const char %s_source[] =\n%s;\n" % (name, literal(glsl.rstrip("\n")))


# ---------------------------------------------------------------- trace ("shadows", "gi")
TRACE_HEAD = """#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D raw;
layout(set = 0, binding = 4, std430) readonly buffer Probes { vec4 probes[]; };
"""

PROBE_IRRADIANCE = """
/* the probes' irradiance (L1 spherical harmonics, a channel each) at a
point, trilinear over the 8 probes about it that are inside the volume and
valid: rgb along n, and its mean over the sphere */
void probe_irradiance(vec3 p, vec3 n, out vec3 along, out vec3 mean)
{
    vec3 f = p / pass.probe_origin.w;
    ivec3 g0 = ivec3(floor(f));
    vec3 t = f - vec3(g0);
    ivec3 o = ivec3(floor(pass.probe_origin.xyz / pass.probe_origin.w + 0.5));
    ivec3 n3 = pass.probe_count.xyz;
    vec4 sum[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
    float weights = 0.0;
    for (int c = 0; c < 8; c++)
    {
        ivec3 d = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 g = g0 + d;
        if (any(lessThan(g, o)) || any(greaterThanEqual(g, o + n3)))
            continue;
        ivec3 slot = ((g % n3) + n3) % n3;
        int index = (slot.z * n3.y + slot.y) * n3.x + slot.x;
        vec4 where = probes[index * 4 + 3];
        if (where.w <= 0.0 || any(greaterThan(abs(where.xyz - vec3(g) * pass.probe_origin.w), vec3(0.01))))
            continue;
        vec3 w3 = mix(1.0 - t, t, vec3(d));
        float w = w3.x * w3.y * w3.z;
        sum[0] += w * probes[index * 4];
        sum[1] += w * probes[index * 4 + 1];
        sum[2] += w * probes[index * 4 + 2];
        weights += w;
    }
    along = vec3(0.0);
    mean = vec3(0.0);
    if (weights <= 1.0e-4)
        return;
    vec4 y = vec4(0.886227, 1.023327 * n.y, 1.023327 * n.z, 1.023327 * n.x);
    for (int k = 0; k < 3; k++)
    {
        vec4 c = sum[k] / weights;
        along[k] = max(dot(c, y), 0.0);
        mean[k] = c.x * 0.886227;
    }
}
"""

TRACE_BODY = PROBE_IRRADIANCE + """
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    int scale = pass.size.z;
    ivec2 full = pass.size.xy;
    if (texel.x >= (full.x + scale - 1) / scale || texel.y >= (full.y + scale - 1) / scale)
        return;
    uvec4 best = uvec4(0u);
    ivec2 at = texel * scale;
    float best_distance = 3.0e38;
    for (int j = 0; j < scale; j++)
        for (int i = 0; i < scale; i++)
        {
            ivec2 p = min(texel * scale + ivec2(i, j), full - 1);
            uvec4 w = texelFetch(inputs, p, 0);
            float d = uintBitsToFloat(w.z);
            if ((w.y & 0x400u) != 0u && d > 0.0 && d < best_distance)
            {
                best = w;
                best_distance = d;
                at = p;
            }
        }
    if ((best.y & 0x400u) == 0u)
    {
        imageStore(raw, texel, vec4(1.0, 1.0, 1.0, 0.0));
        return;
    }
    vec3 n = unfold(unpackSnorm2x16(best.x));
    bool level = (best.y & 0x100u) != 0u;
    vec3 ray = eye_ray(at);
    vec3 p = pass.camera[0].xyz + best_distance * ray;
    /* (a surface seen from behind: its normal toward the eye) */
    if (dot(n, ray) > 0.0)
        n = -n;
    /* (an object's own coarse facets shadow it where its smooth normal
    leans past them: pushed further off its surface) */
    float bias = (level ? 0.02 : 0.05) + 0.0015 * best_distance;
    /* (and toward the eye where the level traced is a remodel: the level
    drawn may lie under the one traced) */
    vec3 origin = p + n * bias - ray * (pass.pt3.w > 0.5 ? 0.04 + 0.004 * best_distance : 0.0);
    uint frame = pass.info.x;
    seed = seed_of(at, frame, 1u);
    cutout_skip = level ? 0.0 : 5.0;
    bool whole = pass.rig[10].z > 0.5;
    uint everything = pass.rig[13].y > 0.5 ? 15u : 5u;
    uint structure = pass.rig[13].y > 0.5 ? 3u : 1u;
    uint objects = pass.rig[13].y > 0.5 ? 12u : 4u;
    float v_a = 1.0, v_b = 1.0;
    int sun_rays = int(pass.rays.x);
    if (pass.sun.w > 0.0 && dot(n, pass.sun.xyz) > 0.0)
    {
        float a = 0.0, b = 0.0;
        vec3 t0, b0;
        basis(n, t0, b0);
        for (int r = 0; r < sun_rays; r++)
        {
            if (whole)
            {
                /* (display.prism_rt_sun "all": the sun through everything,
                the level and the objects in one ray; and as the lightmap's
                texel holds it, through the structure from about the point
                over a texel's width) */
                a += sun_visible(origin, everything);
                float u = sqrt(rnd()) * pass.rig[13].x, w = rnd() * 6.2831853;
                /* (lifted off the surface by half the step: a bend in it
                within the texel never puts the start under it) */
                vec3 o2 = origin + u * (cos(w) * t0 + sin(w) * b0) + n * (0.5 * u);
                b += sun_visible(o2, structure);
            }
            else
            {
                /* ("objects": a ray takes the sun only where an object blocks
                it and the structure does not) */
                vec3 d = cone_dir(pass.sun.xyz, cos(0.5 * pass.rig[0].w));
                bool s = blocked(origin, d, pass.rays.w, structure);
                a += !s && blocked(origin, d, pass.rays.z, objects) ? 0.0 : 1.0;
                b += s ? 0.0 : 1.0;
            }
        }
        v_a = a / float(max(sun_rays, 1));
        v_b = b / float(max(sun_rays, 1));
    }
    else if (whole)
    {
        v_a = v_b = 0.0;
    }

    /* the ambient occlusion: cosine-weighted rays about the normal, a hit at
    t counting 1 - (t / reach)^2, as world AO's traced objects count */
    int ao_rays = int(pass.rays.y);
    float reach = level ? 2.0 : 1.0;
    float occluded = 0.0;
    vec3 t0, b0;
    basis(n, t0, b0);
    for (int r = 0; r < ao_rays; r++)
    {
        float u = (float(r) + rnd()) / float(ao_rays);
        float a = rnd() * 6.2831853;
        vec3 d = normalize(sqrt(u) * (cos(a) * t0 + sin(a) * b0) + sqrt(max(1.0 - u, 0.0)) * n);
        float hit_t = nearest(origin, d, reach, objects);
        /* (an object's: the structure close by too, its half a world unit
        counted over the whole reach) */
        if (!level)
        {
            float near_structure = nearest(origin, d, 0.5, structure);
            if (near_structure < 0.5)
                hit_t = min(hit_t, reach * near_structure / 0.5);
        }
        if (hit_t < reach)
            occluded += 1.0 - (hit_t / reach) * (hit_t / reach);
    }
    float ao = ao_rays > 0 ? 1.0 - occluded / float(ao_rays) : 1.0;
    /* (gi: an object's light shaded by the probes' indirect light along its
    normal against their mean, so its brightness stays CE's and only how it
    falls across the object changes: within 1/k to k, k by the mood) */
    if (pass.probe_info.w != 0u && !level)
    {
        vec3 along, mean;
        probe_irradiance(p + n * 0.25, n, along, mean);
        float y_along = luma(along);
        float y_mean = luma(mean);
        float k = 1.0 + 0.25 * float(pass.probe_info.z) / 100.0;
        if (y_mean > 1.0e-5)
            ao *= clamp(y_along / y_mean, 1.0 / k, k);
        /* (debug.prism_rt_view probes: the probes found (white), none (grey),
        and their ratio's reach) */
        if (pass.info.w == 4u)
            v_a = isnan(y_mean) || isnan(y_along) ? 0.3 : y_mean > 1.0e-5 ?
                clamp(0.5 + 2.0 * (y_along / y_mean - 1.0), 0.0, 1.0) : 0.1;
    }
    imageStore(raw, texel, vec4(v_a, v_b, ao, best_distance));
}
"""

RESOLVE_HEAD = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform sampler2D raw;
layout(set = 0, binding = 2) uniform sampler2D history;
layout(set = 0, binding = 3, rgba32f) uniform writeonly image2D state;
layout(set = 0, binding = 4, rgba32f) uniform writeonly image2D result;
"""

RESOLVE_BODY = """
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    int scale = pass.size.z;
    ivec2 full = pass.size.xy;
    ivec2 extent = (full + scale - 1) / scale;
    if (texel.x >= extent.x || texel.y >= extent.y)
        return;
    vec4 centre = texelFetch(raw, texel, 0);
    /* (the texel's pixel again: the 2x2 block's nearest, its inputs) */
    uvec4 best = uvec4(0u);
    ivec2 at = texel * scale;
    float best_distance = 3.0e38;
    for (int j = 0; j < scale; j++)
        for (int i = 0; i < scale; i++)
        {
            ivec2 p = min(texel * scale + ivec2(i, j), full - 1);
            uvec4 w = texelFetch(inputs, p, 0);
            float d = uintBitsToFloat(w.z);
            if ((w.y & 0x400u) != 0u && d > 0.0 && d < best_distance)
            {
                best = w;
                best_distance = d;
                at = p;
            }
        }
    /* (the raw texel's distance is a half float: the inputs' is whole) */
    centre.w = (best.y & 0x400u) != 0u ? best_distance : 0.0;
    bool keyed = pass.pt3.y > 0.5;
    if (centre.w <= 0.0)
    {
        imageStore(state, texel, vec4(1.0, 1.0, 1.0, 0.0));
        imageStore(result, texel, vec4(1.0, 0.0, 0.0, 0.0));
        return;
    }
    vec2 octahedral = unpackSnorm2x16(best.x);
    vec3 n = unfold(octahedral);
    bool level = (best.y & 0x100u) != 0u;
    vec4 value = centre;
    /* (spatial: a 3x3 cross-bilateral by the neighbours' distances and
    normals, as world AO's upsample weighs them; and the neighbourhood's
    least and most, which the history is held within) */
    vec3 least = centre.xyz, most = centre.xyz;
    if (pass.info.y == 0u)
    {
        vec3 sum = vec3(0.0);
        float weights = 0.0;
        vec3 p0 = pass.camera[0].xyz + centre.w * eye_ray(at);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
            {
                ivec2 q = clamp(texel + ivec2(i, j), ivec2(0), extent - 1);
                vec4 other = texelFetch(raw, q, 0);
                if (other.w <= 0.0)
                    continue;
                uvec4 w = texelFetch(inputs, min(q * scale, full - 1), 0);
                /* (never across a level's edge with an object: far off, an
                object's and the ground's behind it are within a few hundredths
                of each other's distance, and its shadowed side would ring it) */
                if ((w.y & 0x100u) != (best.y & 0x100u))
                    continue;
                vec3 on = unfold(unpackSnorm2x16(w.x));
                /* (the neighbour's distance off the pixel's plane, against a
                reach that grows with the distance) */
                vec3 po = pass.camera[0].xyz + other.w * eye_ray(min(q * scale, full - 1));
                float relative = abs(dot(n, po - p0)) / (0.003 * centre.w + 0.01);
                float along = max(dot(n, on), 0.0);
                along *= along; along *= along;
                float weight = along / (1.0 + relative * relative) * exp(-0.18 * float(i * i + j * j));
                sum += weight * other.xyz;
                weights += weight;
                if (weight > 0.05)
                {
                    least = min(least, other.xyz);
                    most = max(most, other.xyz);
                }
            }
        if (weights > 1.0e-4)
            value.xyz = sum / weights;
    }
    /* (temporal: the last frame's at the point, reprojected by its camera,
    held within the neighbourhood's reach (a moving caster's shadow does not
    trail), blended by the history's length) */
    if (pass.info.z != 0u && pass.info.y != 2u)
    {
        vec3 p = pass.camera[0].xyz + centre.w * eye_ray(at);
        float old_distance;
        ivec2 q = reproject(p, best.y, old_distance);
        if (q.x >= 0)
        {
            vec4 old = texelFetch(history, q / scale, 0);
            /* (the history's distance is negative where it was an object's:
            an object's never taken for the ground's behind it) */
            if (old.w != 0.0 && (old.w > 0.0) == level && abs(abs(old.w) - old_distance) < 0.05 * old_distance)
            {
                vec3 held = clamp(old.xyz, least - 0.15, most + 0.15);
                float alpha = pass.strength.w;
                value.xyz = mix(held, value.xyz, alpha);
            }
        }
    }
    imageStore(state, texel, vec4(value.xyz, level ? centre.w : -centre.w));
    float ao = level ? mix(1.0, max(value.z, 0.2), 0.7) : mix(1.0, max(value.z, 0.3), 0.6);
    uint view = pass.info.w;
    float facing = max(dot(n, pass.sun.xyz), 0.0) * pass.sun.w;
    vec3 s = pass.rig[0].rgb;
    bool whole = pass.rig[10].z > 0.5;
    if (keyed)
    {
        /* the surfaces' light by the occlusion, and the sun's light the
        traces change, added (xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT): "all"
        the sun as the rays see it, less as the lightmap's texel held it (the
        level's shadows as sharp as the traces, an object's shadows with
        them; an object's own light only taken from, CE lit it by one sample
        under it); "objects" the sun's light the objects take where the
        structure lets it through */
        vec3 sun_ce = encoded(s) * facing;
        vec3 change = whole ? (level ? sun_ce * (value.x - value.y) : -sun_ce * max(value.y - value.x, 0.0)) :
            -sun_ce * (1.0 - value.x) * value.y;
        /* (a level's pixel, where the pre-pass gave CE's light: never taken
        below the share of it that is not the sun's, the rig's sky and domes
        over theirs and the sun's) */
        if (level && (best.w >> 24u) == 255u)
        {
            vec3 ce = unpackUnorm4x8(best.w).rgb;
            ce *= ce;
            float sky_light = luma(pass.rig[1].rgb);
            for (int i = 0; i < 4; i++)
                sky_light += luma(pass.rig[3 + 2 * i].rgb) * max(dot(n, normalize(pass.rig[2 + 2 * i].xyz +
                    vec3(0.0, 0.0, 1.0e-6))), 0.0);
            float kept = clamp(sky_light / max(sky_light + luma(s) * facing, 1.0e-4), 0.15, 1.0);
            change = max(change, -(1.0 - kept) * ce);
        }
        if (view == 1u)
            imageStore(result, texel, vec4(0.0, vec3(0.5) + change));
        else if (view == 2u)
            imageStore(result, texel, vec4(0.0, vec3(ao)));
        else if (view == 3u || view == 4u)
            imageStore(result, texel, vec4(0.0, vec3(view == 3u ? value.y : value.x)));
        else
            imageStore(result, texel, vec4(ao, change * ao));
        return;
    }
    /* the factor alone (half resolution): "all" the light as the rays see
    it over the light as the texel held it, the sky's and domes' light (A)
    and the sun's (S) of the rig, (A + S v) / (A + S v'): the bake's sharp
    mask's form; "objects" the share of the sun's light the objects take
    where the structure does not (the rig's, or where the BSP has none
    fitted strength.x and strength.y of a sunlit light) */
    float a = luma(pass.rig[1].rgb);
    for (int i = 0; i < 4; i++)
        a += luma(pass.rig[3 + 2 * i].rgb) * max(dot(n, normalize(pass.rig[2 + 2 * i].xyz + vec3(0.0, 0.0, 1.0e-6))), 0.0);
    float sl = luma(s) * facing;
    float shadow;
    if (whole)
        shadow = (a + sl * value.x) / max(a + sl * value.y, 1.0e-4);
    else
    {
        float fitted = pass.rig[13].z;
        float share = fitted > 0.5 ? sl / max(a + sl, 1.0e-4) : (level ? pass.strength.x : pass.strength.y) *
            (level ? smoothstep(0.0, 0.25, facing) : smoothstep(0.1, 0.4, facing));
        shadow = 1.0 - share * (1.0 - value.x) * value.y;
    }
    float light = view == 1u ? shadow : view == 2u ? ao : view == 3u ? value.y : view == 4u ? value.x :
        shadow * ao;
    imageStore(result, texel, vec4(light, centre.w, octahedral));
}
"""

# ---------------------------------------------------------------- probes (gi's, and "pathtraced"'s cache)
PROBES_HEAD = """#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 1, std430) buffer Probes { vec4 probes[]; };
"""

PROBES_BODY = """shared vec4 sh[64][3];
shared float backs[64];

/* the last irradiance about a point along n (this buffer's: the probes
updated meanwhile as they are) */
vec3 irradiance(vec3 p, vec3 n)
{
    vec3 f = p / pass.probe_origin.w;
    ivec3 g0 = ivec3(floor(f));
    vec3 t = f - vec3(g0);
    ivec3 o = ivec3(floor(pass.probe_origin.xyz / pass.probe_origin.w + 0.5));
    ivec3 n3 = pass.probe_count.xyz;
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    vec4 y = vec4(0.886227, 1.023327 * n.y, 1.023327 * n.z, 1.023327 * n.x);
    for (int c = 0; c < 8; c++)
    {
        ivec3 d = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 g = g0 + d;
        if (any(lessThan(g, o)) || any(greaterThanEqual(g, o + n3)))
            continue;
        ivec3 slot = ((g % n3) + n3) % n3;
        int index = (slot.z * n3.y + slot.y) * n3.x + slot.x;
        vec4 where = probes[index * 4 + 3];
        if (where.w <= 0.0 || any(greaterThan(abs(where.xyz - vec3(g) * pass.probe_origin.w), vec3(0.01))))
            continue;
        vec3 w3 = mix(1.0 - t, t, vec3(d));
        float w = w3.x * w3.y * w3.z;
        sum += w * vec3(max(dot(probes[index * 4], y), 0.0), max(dot(probes[index * 4 + 1], y), 0.0),
            max(dot(probes[index * 4 + 2], y), 0.0));
        weights += w;
    }
    return weights > 1.0e-4 ? sum / weights : vec3(0.0);
}

/* each probe's rays (spherical Fibonacci, turned each frame) in the rig's
light: past the level the sky's (and a dome's within its cone), at a hit
the hit's albedo by the light reaching it (the sun, a lamp, the probes'
last irradiance there: every bounce, kept from frame to frame) and its
glow; projected on L1 spherical harmonics, a channel each, blended into the
probe's last */
void main()
{
    uint total = uint(pass.probe_count.w);
    uint index = (pass.probe_info.x + gl_WorkGroupID.x) % total;
    uint lane = gl_LocalInvocationID.x;
    ivec3 n3 = pass.probe_count.xyz;
    ivec3 slot = ivec3(int(index) % n3.x, (int(index) / n3.x) % n3.y, int(index) / (n3.x * n3.y));
    ivec3 o = ivec3(floor(pass.probe_origin.xyz / pass.probe_origin.w + 0.5));
    /* (the cell this slot holds now: the one in the volume it stands for) */
    ivec3 g = o + (((slot - o) % n3) + n3) % n3;
    vec3 p = vec3(g) * pass.probe_origin.w;
    uint ray_count = pass.probe_info.y;
    vec4 acc[3] = vec4[3](vec4(0.0), vec4(0.0), vec4(0.0));
    float back = 0.0;
    seed = hash_u(index ^ hash_u(pass.info.x * 4u + 3u) ^ hash_u(lane + 0x9e3779b9u));
    float spin = rnd() * 6.2831853;
    for (uint r = lane; r < ray_count; r += 64u)
    {
        float i = float(r) + 0.5;
        float z = 1.0 - 2.0 * i / float(ray_count);
        float a = 2.3999632 * i + spin;
        float rr = sqrt(max(1.0 - z * z, 0.0));
        vec3 d = vec3(rr * cos(a), rr * sin(a), z);
        vec3 radiance = sky_along(d, true);
        vec3 nh;
        uint custom, primitive;
        float t = hit(p, d, 64.0, 15u, nh, custom, primitive);
        if (t >= 0.0)
        {
            if (dot(nh, nh) <= 0.0)
            {
                /* (the level's back: the probe is inside it) */
                back += 1.0;
                radiance = vec3(0.0);
            }
            else
            {
                vec3 h = p + d * t + nh * 0.03;
                vec3 lit = sun_at(h, nh);
                if (pass.pt3.z > 0.5)
                    lit += lamps_at(h, nh, h, 1);
                lit += irradiance(h, nh) * 0.318310;
                radiance = albedo_of(custom, primitive) * lit + glow_of(custom, primitive);
            }
        }
        vec4 y = vec4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x);
        acc[0] += radiance.r * y;
        acc[1] += radiance.g * y;
        acc[2] += radiance.b * y;
    }
    sh[lane][0] = acc[0];
    sh[lane][1] = acc[1];
    sh[lane][2] = acc[2];
    backs[lane] = back;
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            sh[lane][0] += sh[lane + stride][0];
            sh[lane][1] += sh[lane + stride][1];
            sh[lane][2] += sh[lane + stride][2];
            backs[lane] += backs[lane + stride];
        }
        barrier();
    }
    if (lane != 0u)
        return;
    float scale = 12.566371 / float(ray_count);
    vec4 where = probes[index * 4u + 3u];
    bool fresh = where.w <= 0.0 || any(greaterThan(abs(where.xyz - p), vec3(0.01)));
    /* (inside the level: a quarter of its rays or more saw a face's back) */
    bool inside = backs[0] > 0.25 * float(ray_count);
    float keep = fresh ? 0.0 : 0.9;
    /* (a sample gone wrong never written: one NaN would spread to every
    probe through the bounces) */
    for (int k = 0; k < 3; k++)
    {
        vec4 old = probes[index * 4u + uint(k)];
        vec4 value = sh[0][k] * scale;
        if (any(isnan(value)) || any(isinf(value)))
            value = vec4(0.0);
        if (any(isnan(old)) || any(isinf(old)))
            keep = 0.0;
        probes[index * 4u + uint(k)] = mix(value, old, keep);
    }
    probes[index * 4u + 3u] = vec4(p, inside ? 0.0 : 1.0);
}
"""

# ---------------------------------------------------------------- path ("pathtraced")
PATH_HEAD = """#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D raw0;
layout(set = 0, binding = 4, std430) readonly buffer Probes { vec4 probes[]; };
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D raw1;
layout(set = 0, binding = 6) uniform sampler2D reservoirs_last;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D reservoirs;
layout(set = 0, binding = 8) uniform sampler2D lamp_last;
layout(set = 0, binding = 9, rgba32f) uniform writeonly image2D lamp_now;
"""


# the path tracer's copy of TRACE: its own edits (the sky lights' reach, the light's units' floor, a lamp
# blocked by another lamp's surface), not the probes' and the trace pass's
PATH_TRACE = TRACE
for _old, _new in (
        ("""}

/* the sun's visibility from o (its disc, display.prism_sun_size: one ray
toward a point of it) through mask */
float sun_visible(vec3 o, uint mask)
{
    if (pass.sun.w <= 0.0)
        return 0.0;
    vec3 d = cone_dir(pass.sun.xyz, cos(0.5 * pass.rig[0].w));
    return blocked(o, d, 2000.0, mask) ? 0.0 : 1.0;
}

""",
         """}

/* port: a sky light's rays (the sun's, a dome's) tested as Tool's radiosity
tests them, the bake's alike: as far as the light's test distance (the
rig's: rig[15].w the sun's, a dome's its colour's w; 0 the whole level),
blocked by a face met from its front or a two-sided one, not by the
level's one-sided backs */
float sun_reach() { return pass.rig[15].w > 0.0 ? pass.rig[15].w : 2000.0; }
float dome_reach(vec4 r) { return r.w > 0.0 ? r.w : 2000.0; }
bool sky_blocked(vec3 o, vec3 d, float reach, uint mask)
{
    for (int k = 0; k < 4; k++)
    {
        vec3 nh;
        uint custom, primitive;
        float h = hit(o, d, reach, mask, nh, custom, primitive);
        if (h < 0.0)
            return false;
        if (dot(nh, nh) > 0.0)
            return true;
        o += d * (h + 0.01);
        reach -= h + 0.01;
        if (reach <= 0.0)
            return false;
    }
    return true;
}
/* the sun's visibility from o (its disc, display.prism_sun_size: one ray
toward a point of it) through mask */
float sun_visible(vec3 o, uint mask)
{
    if (pass.sun.w <= 0.0)
        return 0.0;
    vec3 d = cone_dir(pass.sun.xyz, cos(0.5 * pass.rig[0].w));
    return sky_blocked(o, d, sun_reach(), mask) ? 0.0 : 1.0;
}

"""),
        ("""        vec3 d = cone_dir(normalize(a.xyz), a.w);
        float c = dot(n, d);
        if (c > 0.0 && !blocked(o, d, 2000.0, 15u))
            sum += r.rgb * c;
    }
    return sum;
}

/* the sky's light along a direction past the level: its radiance, and
where the direction is in a dome's cone the dome's (its light facing it
squarely, by pi over its solid angle) unless the domes were sampled as
lights */
vec3 sky_along(vec3 d, bool domes)
{
    vec3 l = pass.rig[1].rgb;
    if (!domes)
        return l;
""",
         """        vec3 d = cone_dir(normalize(a.xyz), a.w);
        float c = dot(n, d);
        if (c > 0.0 && !sky_blocked(o, d, dome_reach(r), 15u))
            sum += r.rgb * c;
    }
    return sum;
}

/* the sky's light along a direction past the level: its radiance, and
where the direction is in a dome's cone the dome's (its light facing it
squarely, by pi over its solid angle) unless the domes were sampled as
lights */
/* port: the rig in the light's units (pass.rig[15].y, 1 or 2): with the
sky a light (2) the sky's ambient is the open sky's radiance where a path
leaves the level (sky_along), passed on by the bounces, and no floor;
with Tool's fill (1) CE's floor of light, unoccluded, at every point seen
(ambient()), not a radiance from the open sky nor passed on by the
bounces (the bake's alike) */
bool flat_floor() { return pass.rig[15].y > 0.5 && pass.rig[15].y < 1.5; }
vec3 ambient() { return flat_floor() ? pass.rig[1].rgb : vec3(0.0); }
/* port: ... and sky 0's indoor floor (rig[14].yzw) where the structure's
surface the pixel shows (an object's: the surface below it) is in one of
its indoor clusters, one with no sky (word 0's bit 18: xgpu_stream.h's
XGPU_SCENE_INDOOR), as the bake's texels and probes have it: floor_here 0 */
vec3 ambient_indoor() { return flat_floor() ? pass.rig[14].yzw : vec3(0.0); }
float floor_here(vec3 eye, vec3 ray, float distance, vec3 origin, bool level)
{
    if (!flat_floor() || (pass.addresses3.x == 0u && pass.addresses3.y == 0u))
        return 1.0;
    vec3 nh;
    uint custom, primitive;
    float t = level ? hit(eye, ray, distance + 0.1, 3u, nh, custom, primitive) :
        hit(origin, vec3(0.0, 0.0, -1.0), 64.0, 3u, nh, custom, primitive);
    if (t < 0.0 || (custom != 0u && custom != ALPHA_CUSTOM))
        return 1.0;
    return (Uints(pass.addresses3.xy).v[structure_triangle(custom, primitive) * 8u] & 0x40000u) != 0u ? 0.0 : 1.0;
}
vec3 sky_along(vec3 d, bool domes)
{
    vec3 l = flat_floor() ? vec3(0.0) : pass.rig[1].rgb;
    if (!domes)
        return l;
"""),
        ("""bool lamp_visible(vec3 o, vec3 l, float d)
{
    /* (most rays meet nothing: the first hit's test settles them; only a
    ray that meets something looks for the nearest) */
    if (!blocked(o, l, max(d * 0.995 - 0.03, 0.0), 15u))
        return true;
    vec3 nh;
    uint custom, primitive;
    float t = hit(o, l, max(d * 0.995 - 0.03, 0.0), 15u, nh, custom, primitive);
    if (t < 0.0)
        return true;
    if (custom != 0u && custom != ALPHA_CUSTOM)
        return false;
    uint m = material_of(structure_triangle(custom, primitive));
    return m < pass.counts.w && (uint(material_value(m, 3u)) & 1u) != 0u;
}

""",
         """bool lamp_visible(vec3 o, vec3 l, float d)
{
    /* (port: another lamp's surface before the point blocks it, as the
    paths' bounces have it (lamps_at): its own light is its own pick's;
    let through, it was counted twice; the bake's alike) */
    return !blocked(o, l, max(d * 0.995 - 0.03, 0.0), 15u);
}

"""),
):
    assert PATH_TRACE.count(_old) == 1, _old
    PATH_TRACE = PATH_TRACE.replace(_old, _new)


PATH_BODY = """
/* pass.pt: x bounces, y paths a pixel, z ReSTIR's candidates, w the lights;
pass.pt2: x mood (0..1), y the dynamic lights' shadows' strength, z the
lamps' candidates, w the spatial neighbours (negative: half the pixels a
frame). A light: lights[3i] its place and radius, [3i+1] its colour by its
intensity, [3i+2] its direction and cone cosine (2 none). */

vec3 irradiance(vec3 p, vec3 n)
{
    vec3 f = p / pass.probe_origin.w;
    ivec3 g0 = ivec3(floor(f));
    vec3 t = f - vec3(g0);
    ivec3 o = ivec3(floor(pass.probe_origin.xyz / pass.probe_origin.w + 0.5));
    ivec3 n3 = pass.probe_count.xyz;
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    vec4 y = vec4(0.886227, 1.023327 * n.y, 1.023327 * n.z, 1.023327 * n.x);
    for (int c = 0; c < 8; c++)
    {
        ivec3 d = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 g = g0 + d;
        if (any(lessThan(g, o)) || any(greaterThanEqual(g, o + n3)))
            continue;
        ivec3 slot = ((g % n3) + n3) % n3;
        int index = (slot.z * n3.y + slot.y) * n3.x + slot.x;
        vec4 where = probes[index * 4 + 3];
        if (where.w <= 0.0 || any(greaterThan(abs(where.xyz - vec3(g) * pass.probe_origin.w), vec3(0.01))))
            continue;
        vec3 w3 = mix(1.0 - t, t, vec3(d));
        float w = w3.x * w3.y * w3.z;
        sum += w * vec3(max(dot(probes[index * 4], y), 0.0), max(dot(probes[index * 4 + 1], y), 0.0),
            max(dot(probes[index * 4 + 2], y), 0.0));
        weights += w;
    }
    return weights > 1.0e-4 ? sum / weights : vec3(-1.0);
}

/* a dynamic light's light at p (normal n), unshadowed: CE's point light's
falloff (max(1 - d^2 / r^2, 0)), the facing, a spot's cone; and the
direction and distance to it */
vec3 light_at(int i, vec3 p, vec3 n, out vec3 l, out float d)
{
    vec4 a = pass.lights[3 * i], c = pass.lights[3 * i + 1], s = pass.lights[3 * i + 2];
    vec3 to = a.xyz - p;
    float d2 = dot(to, to);
    d = sqrt(max(d2, 1.0e-12));
    l = to / d;
    float r = max(a.w, 1.0e-3);
    float fall = max(1.0 - d2 / (r * r), 0.0);
    float facing = max(dot(n, l), 0.0);
    float cone = s.w > 1.5 ? 1.0 : smoothstep(s.w, mix(s.w, 1.0, 0.2), dot(-l, s.xyz));
    return c.rgb * c.w * fall * facing * cone;
}

float target(int i, vec3 p, vec3 n)
{
    vec3 l;
    float d;
    return i >= 0 ? luma(light_at(i, p, n, l, d)) : 0.0;
}

struct Reservoir
{
    int light;
    float wsum;
    float m;
    float w;
};

bool reservoir_add(inout Reservoir r, int light, float weight, float m)
{
    r.wsum += weight;
    r.m += m;
    if (weight > 0.0 && rnd() * r.wsum <= weight)
    {
        r.light = light;
        return true;
    }
    return false;
}

/* port: the light fixtures (the rig in the light's units: the bake's
fixtures, xgpu_vulkan_bake_glsl.inc's fixtures_ris): addresses2's 16
floats each, addresses2.z of them; one picked by its unshadowed light
among all (16 or fewer) or 16 candidates, its shadow traced */
vec3 fixtures_at(vec3 p, vec3 n, vec3 o)
{
    uint count = pass.addresses2.z;
    if (count == 0u)
        return vec3(0.0);
    Floats f = Floats(pass.addresses2.xy);
    uint candidates = min(count, 16u);
    float sum = 0.0, kept_w = 0.0, kept_d = 0.0;
    vec3 kept = vec3(0.0), kept_l = vec3(0.0, 0.0, 1.0);
    for (uint c = 0u; c < candidates; c++)
    {
        uint i = count <= 16u ? c : min(uint(rnd() * float(count)), count - 1u);
        uint at = i * 16u;
        vec3 to = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]) - p;
        float d2 = dot(to, to), d = sqrt(d2);
        vec3 l = to / max(d, 1.0e-6);
        float cs = dot(n, l) / 3.14159265;
        if (cs <= 0.0 || d < 1.0e-4)
            continue;
        vec4 axis = vec4(f.v[at + 8u], f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);
        float cone = axis.w < 1.5 ? smoothstep(axis.w, max(f.v[at + 12u], axis.w + 1.0e-4), dot(-l, axis.xyz)) : 1.0;
        vec3 e = vec3(f.v[at + 4u], f.v[at + 5u], f.v[at + 6u]) * (cs * cone / (d2 + 0.01));
        float w = luma(e);
        if (!(w > 0.0) || isinf(w))
            continue;
        sum += w;
        if (rnd() * sum <= w)
        {
            kept = e;
            kept_w = w;
            kept_l = l;
            kept_d = d;
        }
    }
    if (kept_w <= 0.0 || blocked(o, kept_l, max(kept_d - 0.05, 0.0), 15u))
        return vec3(0.0);
    return kept / kept_w * sum * (float(count) / float(candidates));
}

/* the light reaching a hit (the rig's: the sun, a lamp; the dynamic
lights' apart, CE's units), its bounces past it to come from the path */
void hit_light(vec3 h, vec3 nh, out vec3 ls, out vec3 ld)
{
    vec3 o = h + nh * 0.03;
    ls = sun_at(o, nh) + fixtures_at(h, nh, o);
    if (pass.counts.z > 0u)
        ls += lamps_at(h, nh, o, 1);
    ld = vec3(0.0);
    int count = int(pass.pt.w);
    if (count > 0)
    {
        int i = min(int(rnd() * float(count)), count - 1);
        vec3 l;
        float d;
        vec3 e = light_at(i, h, nh, l, d);
        if (luma(e) > 1.0e-5 && !blocked(o, l, max(d - 0.05, 0.0), 15u))
            ld = e * float(count);
    }
}

/* at each pixel of the pre-pass: the rig's light reaching it (the paths'
estimate, linear, in the rig's units): the sun's visibility apart (its
light is the rig's sun by the facing, found where it is denoised), the
domes and the lamps sampled as lights, the sky and every bounce by paths
(a cosine ray, each hit's albedo by the light reaching it: the sun, a
lamp, its glow, and the next bounce or, at the last, the probes' irradiance:
their cache of every bounce), the dynamic lights' bounce and their traced
shadows' share of the light CE adds unshadowed. Written: raw0 the light
less the sun's, and the sun's visibility; raw1 the dynamic lights' change
(CE's units), and whether the pixel was traced this frame; the reservoirs */
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 full = pass.size.xy;
    if (texel.x >= full.x || texel.y >= full.y)
        return;
    uvec4 w = texelFetch(inputs, texel, 0);
    float distance = uintBitsToFloat(w.z);
    if ((w.y & 0x400u) == 0u || !(distance > 0.0))
    {
        imageStore(raw0, texel, vec4(0.0));
        imageStore(raw1, texel, vec4(0.0));
        imageStore(reservoirs, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        imageStore(lamp_now, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        return;
    }
    /* (display.prism_rt_rate "half", or quality low: half the pixels a
    frame, in a checkerboard that turns each frame; the others keep their
    history, the spatial passes fill them where it has none) */
    /* (the traced light's rate (pass.grid_size.w's bits 1 and 2: 1 half,
    2 a quarter, the pixels of a checkerboard or of each 2x2 block in turn):
    a pixel not traced this frame takes its light from the radiance cache
    at its own place (a level's pixel, the cache on), the sun's visibility
    and the dynamic lights traced as at every pixel; the paths and the
    lamps' picks, the cost, at the traced ones alone, which fill the cache
    for all. The light is the cache's at full resolution, not a smaller
    image's stretched) */
    int rate = (pass.grid_size.w >> 1) & 3;
    bool light_only = rate == 1 ? ((texel.x + texel.y + int(pass.info.x)) & 1) != 0 :
        rate == 2 ? (((texel.x & 1) | ((texel.y & 1) << 1)) != int(pass.info.x & 3u)) : false;
    if (light_only && (pass.grid_size.w & 1) == 0)
        light_only = false;
    if (pass.pt2.w < 0.0 && ((texel.x + texel.y + int(pass.info.x)) & 1) != 0)
    {
        imageStore(raw0, texel, vec4(0.0));
        imageStore(raw1, texel, vec4(0.0));
        imageStore(reservoirs, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        imageStore(lamp_now, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        return;
    }
    seed = seed_of(texel, pass.info.x, 2u);
    vec3 n = unfold(unpackSnorm2x16(w.x));
    bool level = (w.y & 0x100u) != 0u;
    vec3 ray = eye_ray(texel);
    vec3 p = pass.camera[0].xyz + distance * ray;
    if (dot(n, ray) > 0.0)
        n = -n;
    float bias = (level ? 0.02 : 0.05) + 0.0015 * distance;
    /* (and toward the eye where the level traced is a remodel: the level
    drawn, a coarser level of it far off, may lie under the one traced) */
    vec3 origin = p + n * bias - ray * (pass.pt3.w > 0.5 ? 0.04 + 0.004 * distance : 0.0);
    int bounces = int(pass.pt.x), paths = int(pass.pt.y);
    cutout_skip = level ? 0.0 : 5.0;

    /* the sun's visibility (its disc), through everything: a level's
    surface facing it alone; an object's whichever way its normal leans (its
    own light, CE's, has the sun's angle: a leaf's normal, bent out from its
    tree, turned away from the sun is lit through) */
    float v = 0.0, v_structure = 0.0;
    int sun_rays = int(pass.rays.x);
    if (pass.sun.w > 0.0 && (!level || dot(n, pass.sun.xyz) > 0.0))
    {
        /* (through the structure, then the objects where it lets the sun
        through: the structure's alone kept too, the anchor's measure, which
        CE's lightmap has: the objects' shadows are not its to undo) */
        for (int r = 0; r < sun_rays; r++)
        {
            vec3 d = cone_dir(pass.sun.xyz, cos(0.5 * pass.rig[0].w));
            if (!sky_blocked(origin, d, sun_reach(), 3u))
            {
                v_structure += 1.0;
                v += blocked(origin, d, sun_reach(), 12u) ? 0.0 : 1.0;
            }
        }
        v /= float(max(sun_rays, 1));
        v_structure /= float(max(sun_rays, 1));
    }

    /* the domes, sampled as lights */
    vec3 light = vec3(0.0);
    vec3 indirect = light_only ? vec3(0.0) : domes_at(origin, n);

    /* the lamps: ReSTIR DI (Bitterli et al. 2020) over their points:
    candidates picked mostly from the lamps' grid cell about the point (by
    their light at its middle), the rest by power, one kept by its light here (resampled
    importance), dropped where its shadow ray finds it hidden; the last
    frame's reservoir at the point and about it (each a lamp's point, its
    weight and how many samples it stands for), their lights weighed again
    here; the one kept traced for its shadow. A reservoir: its lamp and
    count (lamp + count x 2^18), the point's barycentrics, its weight */
    vec4 lamp_store = vec4(-1.0, 0.0, 0.0, 0.0);
    if (pass.counts.z > 0u && pass.rig[10].w >= 0.5 && !light_only)
    {
        uint y = 0u;
        vec2 yb = vec2(0.0);
        float wsum = 0.0, m = 0.0, y_target = 0.0;
        int candidates = int(pass.pt2.z);
        for (int c = 0; c < candidates; c++)
        {
            vec2 b;
            uint lamp = lamp_pick_at(p, b);
            vec3 l;
            float d, density;
            float t = luma(lamp_point(lamp, b, p, n, l, d, density));
            float w = density > 0.0 ? t / density : 0.0;
            wsum += w;
            m += 1.0;
            if (w > 0.0 && rnd() * wsum <= w)
            {
                y = lamp;
                yb = b;
                y_target = t;
            }
        }
        if (y_target > 0.0)
        {
            vec3 l;
            float d, density;
            lamp_point(y, yb, p, n, l, d, density);
            if (!lamp_visible(origin, l, d))
            {
                y_target = 0.0;
                wsum = 0.0;
            }
        }
        float old_distance;
        ivec2 q = reproject(p, w.y, old_distance);
        if (q.x >= 0 && pass.info.z != 0u)
        {
            int neighbours = int(abs(pass.pt2.w));
            for (int s = 0; s <= neighbours; s++)
            {
                ivec2 o = s == 0 ? q : q + ivec2((rnd() * 2.0 - 1.0) * 6.0, (rnd() * 2.0 - 1.0) * 6.0);
                o = clamp(o, ivec2(0), pass.size.xy - 1);
                /* (a neighbour's reservoir only from a surface like this one:
                its normal within 25 degrees, on its plane within 2% of the
                distance (the
                inputs there now standing for last frame's); a lamp another
                surface saw, reused here, lit or darkened whole patches) */
                if (s > 0)
                {
                    uvec4 wo = texelFetch(inputs, o, 0);
                    float d_o = uintBitsToFloat(wo.z);
                    vec3 p_o = pass.camera[0].xyz + d_o * eye_ray(o);
                    if ((wo.y & 0x400u) == 0u || (wo.y & 0x100u) != (w.y & 0x100u) ||
                        abs(dot(n, p_o - p)) > 0.02 * distance + 0.05 ||
                        abs(dot(unfold(unpackSnorm2x16(wo.x)), n)) < 0.9)
                        continue;
                }
                vec4 last = texelFetch(lamp_last, o, 0);
                if (last.x < 0.0)
                    continue;
                uint lamp = uint(last.x) % 262144u;
                /* (the history's weight at most four frames' candidates: a
                longer one, its neighbours' reused with it, holds a lamp's
                choice over patches tens of pixels across, which then boil as
                the camera moves) */
                float count = min(floor(last.x / 262144.0), 4.0 * float(candidates));
                if (lamp >= pass.counts.z || count <= 0.0)
                    continue;
                /* (a reservoir whose lamp was hidden (its weight 0) still
                counts its candidates: dropped, only the lit ones' counts
                were summed, and the lamps' light came out brighter the more
                was reused, a moving camera's most) */
                if (last.w <= 0.0)
                {
                    m += count;
                    continue;
                }
                vec3 l;
                float d, density;
                float t = luma(lamp_point(lamp, last.yz, p, n, l, d, density));
                float w = t * last.w * count;
                wsum += w;
                m += count;
                if (w > 0.0 && rnd() * wsum <= w)
                {
                    y = lamp;
                    yb = last.yz;
                    y_target = t;
                }
            }
        }
        float weight = y_target > 0.0 ? min(wsum / (max(m, 1.0) * y_target), 1.0e6) : 0.0;
        if (weight > 0.0)
        {
            vec3 l;
            float d, density;
            vec3 e = lamp_point(y, yb, p, n, l, d, density);
            if (lamp_visible(origin, l, d))
                light += e * weight;
            else
                weight = 0.0;
        }
        lamp_store = vec4(float(y) + min(m, 63.0) * 262144.0, yb, weight);
    }
    imageStore(lamp_now, texel, lamp_store);
    light += fixtures_at(p, n, origin) +
        mix(ambient_indoor(), ambient(), floor_here(pass.camera[0].xyz, ray, distance, origin, level));

    /* the sky and the bounces, by paths */
    vec3 dynamic_bounce = vec3(0.0);
    for (int k = 0; k < (light_only ? 0 : paths); k++)
    {
        vec3 d = cosine_dir(n);
        vec3 nh;
        uint custom, primitive;
        float t = hit(origin, d, 300.0, 15u, nh, custom, primitive);
        if (t < 0.0)
        {
            indirect += sky_along(d, false) / float(paths);
            continue;
        }
        vec3 h = origin + d * t;
        vec3 throughput = vec3(1.0);
        cutout_skip = 0.0;
        vec3 path = vec3(0.0), path_dynamic = vec3(0.0);
        for (int bounce = 0; bounce < bounces && dot(nh, nh) > 0.0; bounce++)
        {
            vec3 albedo = albedo_of(custom, primitive);
            path += throughput * glow_of(custom, primitive);
            vec3 ls, ld;
            hit_light(h, nh, ls, ld);
            throughput *= albedo;
            path += throughput * ls;
            path_dynamic += throughput * ld;
            /* (the last bounce ends in the probes' irradiance: every bounce
            past it; debug.pt_reference's paths end there, unbiased up to
            their bounces, no cache and no probes) */
            if (bounce == bounces - 1)
            {
                if (pass.reference.x > 0.5)
                    break;
                /* (the radiance cache's cell at the hit first, where a pixel
                has seen it: in world space, settled; else the probes about
                the camera, whose grid moves with it and holds another
                place's light a while) */
                int hit_entry = pass.grid_size.w != 0 ?
                    cache_entry(h, nh, length(h - pass.camera[0].xyz), false) : -1;
                vec4 hit_cached = hit_entry >= 0 ? cache_read(hit_entry) : vec4(0.0);
                if (hit_cached.w >= 8.0)
                {
                    path += throughput * hit_cached.rgb;
                    break;
                }
                vec3 pr = irradiance(h + nh * 0.1, nh);
                path += throughput * (pr.x >= 0.0 ? pr * 0.318310 : 0.3 * pass.rig[1].rgb);
                break;
            }
            /* (Russian roulette past the second bounce) */
            if (bounce >= 1)
            {
                float keep = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
                if (rnd() > keep)
                    break;
                throughput /= keep;
            }
            vec3 o = h + nh * 0.03;
            vec3 wi = cosine_dir(nh);
            float t2 = hit(o, wi, 300.0, 15u, nh, custom, primitive);
            if (t2 < 0.0)
            {
                path += throughput * sky_along(wi, true);
                break;
            }
            h = o + wi * t2;
        }
        indirect += path / float(paths);
        dynamic_bounce += path_dynamic / float(paths);
        cutout_skip = level ? 0.0 : 5.0;
    }

    /* the radiance cache: a level's pixel adds its path's light to its
    cell (at a point jittered within it, so that the cells' edges blur) and
    takes the cell's resolved light once it has a few samples (an object's,
    which moves, keeps its own) */
    if (level && pass.grid_size.w != 0)
    {
        float side = max(0.125, exp2(ceil(log2(max(distance * 0.01, 1.0e-4)))));
        vec3 t_, b_;
        basis(n, t_, b_);
        vec3 jittered = p + side * ((rnd() - 0.5) * t_ + (rnd() - 0.5) * b_);
        int entry = cache_entry(jittered, n, distance, true);
        /* (and the cell a level up, twice the side, written alike: a cell
        new to the eye (the camera come nearer, its level changed) is lit
        by its parent's settled light until it has its own) */
        vec3 jittered2 = p + 2.0 * side * ((rnd() - 0.5) * t_ + (rnd() - 0.5) * b_);
        int parent = cache_entry(jittered2, n, distance * 2.0, true);
        /* (the lamps' light with it: a lamp's pick, one of hundreds, most of
        them hidden, is the noisiest of the light, and the cell's, in world
        space, keeps its samples as the camera moves where a pixel's history
        is let go of) */
        vec3 total = indirect + light;
        if (entry >= 0)
        {
            bool good = !light_only && !(any(isnan(total)) || any(isinf(total)));
            if (good)
                cache_add(entry, total);
            if (good && parent >= 0)
                cache_add(parent, total);
            vec4 cached = cache_read(entry);
            vec4 up = parent >= 0 ? cache_read(parent) : vec4(0.0);
            if (cached.w < CACHE_SETTLED && up.w >= CACHE_SETTLED)
            {
                float k = cached.w / CACHE_SETTLED;
                cached = vec4(mix(up.rgb, cached.rgb, k), CACHE_SETTLED);
            }
            if (cached.w >= 8.0)
            {
                indirect = cached.rgb;
                light = vec3(0.0);
                light_only = false;
            }
        }
    }
    /* (a pixel not traced this frame that the cache could not light (an
    object's, a cell not yet settled): its history alone, as the
    checkerboard's) */
    if (light_only)
    {
        imageStore(raw0, texel, vec4(0.0));
        imageStore(raw1, texel, vec4(0.0));
        imageStore(reservoirs, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        imageStore(lamp_now, texel, vec4(-1.0, 0.0, 0.0, 0.0));
        return;
    }
    light += indirect;

    /* ReSTIR DI over the dynamic lights at the pixel: candidates by their
    count, the last frame's reservoir at the point and its neighbours
    there, their light weighed again here; the one kept is traced for its
    shadow. CE adds every light unshadowed: the shadows found take that
    light away (by pt2.y) */
    int count = int(pass.pt.w);
    vec3 shadowed = vec3(0.0);
    Reservoir r = Reservoir(-1, 0.0, 0.0, 0.0);
    if (count > 0)
    {
        int candidates = int(pass.pt.z);
        for (int c = 0; c < candidates; c++)
        {
            int i = min(int(rnd() * float(count)), count - 1);
            reservoir_add(r, i, target(i, p, n) * float(count), 1.0);
        }
        float old_distance;
        ivec2 q = reproject(p, w.y, old_distance);
        if (q.x >= 0 && pass.info.z != 0u)
        {
            int neighbours = int(abs(pass.pt2.w));
            for (int s = 0; s <= neighbours; s++)
            {
                ivec2 o = s == 0 ? q : q + ivec2((rnd() * 2.0 - 1.0) * 10.0, (rnd() * 2.0 - 1.0) * 10.0);
                o = clamp(o, ivec2(0), full - 1);
                vec4 last = texelFetch(reservoirs_last, o, 0);
                int li = int(last.x + 0.5);
                if (last.x < -0.5 || li >= count || last.z <= 0.0)
                    continue;
                float m = min(last.z, 20.0 * float(candidates));
                reservoir_add(r, li, target(li, p, n) * last.w * m, m);
            }
        }
        float chosen = target(r.light, p, n);
        r.w = chosen > 0.0 ? min(r.wsum / (max(r.m, 1.0) * chosen), 1.0e4) : 0.0;
        if (r.light >= 0 && r.w > 0.0)
        {
            vec3 l;
            float d;
            vec3 e = light_at(r.light, p, n, l, d);
            /* (the unshadowed sum, every light's, as CE draws them) */
            vec3 all = vec3(0.0);
            for (int i = 0; i < count; i++)
            {
                vec3 l2;
                float d2;
                all += light_at(i, p, n, l2, d2);
            }
            vec3 lit = blocked(origin, l, max(d - 0.05, 0.0), 15u) ? vec3(0.0) : e * r.w;
            shadowed = all - lit;
        }
    }
    imageStore(reservoirs, texel, vec4(float(r.light), r.wsum, r.m, r.w));
    vec3 add = dynamic_bounce - pass.pt2.y * shadowed;
    if (any(isnan(add)) || any(isinf(add)))
        add = vec3(0.0);
    if (any(isnan(light)) || any(isinf(light)))
        light = vec3(0.0);
    imageStore(raw0, texel, vec4(min(light, vec3(6.0e4)), v));
    /* (w: 1 (sampled this frame) and the structure's visibility) */
    imageStore(raw1, texel, vec4(add, 1.0 + v_structure));
}
"""

# ---------------------------------------------------------------- accumulate
ACCUMULATE_HEAD = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform sampler2D raw0;
layout(set = 0, binding = 2) uniform sampler2D raw1;
layout(set = 0, binding = 4) uniform sampler2D history0;
layout(set = 0, binding = 5) uniform sampler2D history1;
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2D state0;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D state1;
layout(set = 0, binding = 8) uniform sampler2D history2;
layout(set = 0, binding = 9, rgba16f) uniform writeonly image2D state2;
layout(set = 0, binding = 10) uniform sampler2D history3;
layout(set = 0, binding = 11, rgba16f) uniform writeonly image2D state3;
"""

ACCUMULATE_BODY = """
/* the path tracer's temporal pass: each texel's new samples blended into
its history at the point the last camera saw it (kept where its distance is
within 3%), by 1 / its length (the light at most 32 frames, 12 with
"temporal"; the sun's visibility at most 8, held within its neighbours'
reach this frame, so that a moving caster's shadow does not trail); the
visibility's second moment kept for the spatial passes' weights. state0:
the light less the sun's and the distance; state1: the dynamic lights'
change and the length; state2: the sun's visibility, its square's mean,
its length */
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 full = pass.size.xy;
    if (texel.x >= full.x || texel.y >= full.y)
        return;
    vec4 a = texelFetch(raw0, texel, 0), b = texelFetch(raw1, texel, 0);
    uvec4 w = texelFetch(inputs, texel, 0);
    float distance = uintBitsToFloat(w.z);
    if ((w.y & 0x400u) == 0u || !(distance > 0.0))
    {
        imageStore(state0, texel, vec4(0.0));
        imageStore(state1, texel, vec4(0.0));
        imageStore(state2, texel, vec4(0.0));
        imageStore(state3, texel, vec4(0.0));
        return;
    }
    /* (a texel the checkerboard passed over this frame: its history alone,
    length 0 where it has none) */
    bool sampled = b.w > 0.5;
    float length_ = sampled ? 1.0 : 0.0, v_length = sampled ? 1.0 : 0.0;
    vec3 light = sampled ? a.rgb : vec3(0.0), add = sampled ? b.rgb : vec3(0.0);
    float v = sampled ? a.w : 0.0, v2 = v * v;
    float vs = sampled ? b.w - 1.0 : 0.0;
    /* (the fast history (four frames) and the light's luminance's second
    moment, ReLAX's (the published talks') and A-SVGF's (Schied et al.
    2018): the slow history clamped to the fast one's reach, so that an
    error a short history let in is not held for 32 frames; the moment, the
    temporal variance the spatial passes weigh the light by) */
    vec3 fast = light;
    float y0 = luma(light), m2 = y0 * y0;
    /* (a new sample past four times its neighbours' brightest, a lamp's or
    the sky's rare path: held to that, the fireflies the filters would
    spread into patches) */
    if (sampled)
    {
        float brightest = 0.0;
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
            {
                if (i == 0 && j == 0)
                    continue;
                ivec2 q = clamp(texel + ivec2(i, j), ivec2(0), full - 1);
                brightest = max(brightest, luma(texelFetch(raw0, q, 0).rgb));
            }
        float y_here = luma(light);
        if (y_here > 4.0 * brightest + 0.02)
        {
            light *= (4.0 * brightest + 0.02) / y_here;
            fast = light;
            y0 = luma(light);
            m2 = y0 * y0;
        }
    }
    /* (the visibility's neighbourhood this frame) */
    float least = 1.0, most = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
        {
            ivec2 q = clamp(texel + ivec2(i, j), ivec2(0), full - 1);
            vec4 o = texelFetch(raw0, q, 0);
            if (texelFetch(raw1, q, 0).w > 0.5)
            {
                least = min(least, o.w);
                most = max(most, o.w);
            }
        }
    if (pass.info.z != 0u && pass.info.y != 2u)
    {
        vec3 p = pass.camera[0].xyz + distance * eye_ray(texel);
        float old_distance;
        ivec2 q = reproject(p, w.y, old_distance);
        /* (the history's texel of the point and its four neighbours, the
        nearest in distance; held within 3% of it, more where the surface is
        seen edge on (a floor's distance changes by more than that from one
        texel to the next): the floors' history had been let go of every few
        frames as the camera moved, and their light left a sample or two) */
        vec3 nh = unfold(unpackSnorm2x16(w.x));
        float grazing = max(abs(dot(nh, eye_ray(texel))), 0.15);
        if (q.x >= 0)
        {
            ivec2 best_q = q;
            float best_gap = 3.0e38;
            for (int k = 0; k < 5; k++)
            {
                ivec2 c = clamp(q + (k == 0 ? ivec2(0) : k == 1 ? ivec2(1, 0) : k == 2 ? ivec2(-1, 0) :
                    k == 3 ? ivec2(0, 1) : ivec2(0, -1)), ivec2(0), pass.size.xy - 1);
                float hw = texelFetch(history0, c, 0).w;
                if (hw == 0.0 || (hw > 0.0) != ((w.y & 0x100u) != 0u))
                    continue;
                float gap = abs(abs(hw) - old_distance);
                if (gap < best_gap)
                {
                    best_gap = gap;
                    best_q = c;
                }
            }
            q = best_q;
        }
        if (q.x >= 0)
        {
            vec4 oa = texelFetch(history0, q, 0), ob = texelFetch(history1, q, 0), oc = texelFetch(history2, q, 0);
            /* (the history's distance negative where it was an object's: an
            object's never taken for the ground's behind it) */
            if (oa.w != 0.0 && (oa.w > 0.0) == ((w.y & 0x100u) != 0u) &&
                abs(abs(oa.w) - old_distance) < 0.03 * old_distance / grazing && !any(isnan(oa)) &&
                !any(isnan(ob)) && !any(isnan(oc)))
            {
                float most_length = pass.info.y == 1u ? 12.0 : 32.0;
                float held = least <= most ? clamp(oc.x, least, most) : oc.x;
                vec4 od = texelFetch(history3, q, 0);
                if (any(isnan(od)))
                    od = vec4(oa.rgb, luma(oa.rgb) * luma(oa.rgb));
                if (!sampled)
                {
                    length_ = ob.w;
                    v_length = oc.z;
                    light = oa.rgb;
                    add = ob.rgb;
                    v = oc.x;
                    v2 = oc.y;
                    vs = oc.w;
                    fast = od.rgb;
                    m2 = od.w;
                }
                else
                {
                    /* (a new sample far brighter than the history, a lamp's
                    rare path, held to 32 times it once the history is long
                    enough to say) */
                    float y_new = luma(light), y_old = luma(oa.rgb);
                    if (ob.w >= 8.0 && y_new > 32.0 * y_old + 0.05)
                        light *= (32.0 * y_old + 0.05) / y_new;
                    length_ = min(ob.w + 1.0, most_length);
                    float alpha = 1.0 / max(length_, 1.0);
                    vec3 sample_ = light;
                    light = mix(oa.rgb, light, alpha);
                    fast = mix(od.rgb, sample_, max(alpha, 0.25));
                    float ys = luma(sample_);
                    m2 = mix(od.w, ys * ys, max(alpha, 0.1));
                    /* (the slow history held within the fast one's reach: its
                    luminance within two of the moment's deviations of the
                    fast one's (and a hundredth of it); where it is not, its
                    length cut, so the next frames move it) */
                    float y_slow = luma(light), y_fast = luma(fast);
                    float sigma = sqrt(max(m2 - y_slow * y_slow, 0.0));
                    float reach = 2.0 * sigma + 0.01 * y_fast + 1.0e-4;
                    float y_held = clamp(y_slow, y_fast - reach, y_fast + reach);
                    if (y_held != y_slow)
                    {
                        light *= y_slow > 1.0e-6 ? y_held / y_slow : 1.0;
                        if (y_slow <= 1.0e-6)
                            light = fast;
                        length_ = min(length_, 8.0);
                    }
                    /* (the dynamic lights' change at most three frames
                    long: a plasma bolt's light and its shadows move with it,
                    and a longer history left their glow on the ground behind) */
                    add = mix(ob.rgb, add, max(alpha, 1.0 / 3.0));
                    v_length = min(oc.z + 1.0, 8.0);
                    float beta = 1.0 / max(v_length, 1.0);
                    v2 = mix(oc.y + (held * held - oc.x * oc.x), v2, beta);
                    v = mix(held, v, beta);
                    vs = mix(oc.w, vs, beta);
                }
            }
        }
    }
    imageStore(state0, texel, vec4(light, (w.y & 0x100u) != 0u ? distance : -distance));
    imageStore(state1, texel, vec4(add, length_));
    imageStore(state2, texel, vec4(v, max(v2, v * v), v_length, vs));
    imageStore(state3, texel, vec4(fast, max(m2, luma(light) * luma(light))));
}
"""


# ---------------------------------------------------------------- debug.pt_reference
REFERENCE_HEAD = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform sampler2D raw0;
layout(set = 0, binding = 2) uniform sampler2D raw1;
layout(set = 0, binding = 4, std430) buffer Sums { vec4 sums[]; };
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D state0;
layout(set = 0, binding = 6, rgba16f) uniform writeonly image2D state1;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D state2;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D state3;
"""

REFERENCE_BODY = """
/* debug.pt_reference, in the temporal pass's place: each texel's samples
summed over the frames in 32-bit floats (sums: three vec4s a texel: the
light less the sun's and the sun's visibility; the dynamic lights' change
and the structure's visibility; the visibility's square, the distance, the
samples) with no clamp, no reprojection and no history but its own (the
camera does not move: xgpu_vulkan_rt.c starts again where it does), and
their mean written as the temporal pass writes its state, for the spatial
passes (the denoiser off: they pass it through) and the anchor's tiles.
pass.reference: x 1, y this frame's number in the sum (1 the first: the
sums started again), z the frames, w 1 where this frame's samples are added
(0: the sum is whole, held) */
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 full = pass.size.xy;
    if (texel.x >= full.x || texel.y >= full.y)
        return;
    uint at = uint(texel.y * full.x + texel.x) * 3u;
    uvec4 w = texelFetch(inputs, texel, 0);
    float distance = uintBitsToFloat(w.z);
    vec4 s0 = vec4(0.0), s1 = vec4(0.0), s2 = vec4(0.0);
    if (pass.reference.y > 1.5 || pass.reference.w < 0.5)
    {
        s0 = sums[at];
        s1 = sums[at + 1u];
        s2 = sums[at + 2u];
    }
    bool present = (w.y & 0x400u) != 0u && distance > 0.0;
    if (present && pass.reference.w > 0.5)
    {
        vec4 a = texelFetch(raw0, texel, 0), b = texelFetch(raw1, texel, 0);
        if (b.w > 0.5 && !any(isnan(a)) && !any(isinf(a)) && !any(isnan(b)) && !any(isinf(b)))
        {
            s0 += a;
            s1 += vec4(b.rgb, b.w - 1.0);
            s2 += vec4(a.w * a.w, distance, 1.0, 0.0);
        }
    }
    if (pass.reference.w > 0.5)
    {
        sums[at] = s0;
        sums[at + 1u] = s1;
        sums[at + 2u] = s2;
    }
    if (!present || s2.z < 0.5)
    {
        imageStore(state0, texel, vec4(0.0));
        imageStore(state1, texel, vec4(0.0));
        imageStore(state2, texel, vec4(0.0));
        imageStore(state3, texel, vec4(0.0));
        return;
    }
    float n = s2.z;
    vec3 light = s0.rgb / n;
    float v = s0.w / n;
    imageStore(state0, texel, vec4(light, (w.y & 0x100u) != 0u ? distance : -distance));
    imageStore(state1, texel, vec4(s1.rgb / n, 32.0));
    imageStore(state2, texel, vec4(v, max(s2.x / n, v * v), 8.0, s1.w / n));
    imageStore(state3, texel, vec4(light, luma(light) * luma(light)));
}
"""


# ---------------------------------------------------------------- the radiance cache's resolve
CACHE_RESOLVE = """#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(local_size_x = 64) in;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer CacheWords { uint v[]; };
layout(push_constant) uniform Push { uvec2 cache; uint frame; uint entries; } push;

/* the radiance cache's entries (xgpu_vulkan_rt_glsl.inc's path): each
written this frame has its sum's mean blended into its light, its weight
at most 256 samples (a cell a few hundred pixels see takes it over a few
frames, kept in world space as the camera moves; a far one's few samples
each frame over many); its sum cleared. An
entry no pixel wrote for 240 frames let go of */
void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= push.entries)
        return;
    CacheWords c = CacheWords(push.cache);
    uint at = index * 10u;
    if (c.v[at] == 0u)
        return;
    uint count = c.v[at + 5u];
    if (count > 0u)
    {
        vec3 mean_ = vec3(c.v[at + 2u], c.v[at + 3u], c.v[at + 4u]) / (256.0 * float(count));
        vec3 old = vec3(uintBitsToFloat(c.v[at + 6u]), uintBitsToFloat(c.v[at + 7u]), uintBitsToFloat(c.v[at + 8u]));
        float weight = uintBitsToFloat(c.v[at + 9u]);
        float total = min(weight + float(count), 256.0);
        vec3 now = weight > 0.0 ? mix(old, mean_, min(float(count) / total, 1.0)) : mean_;
        c.v[at + 6u] = floatBitsToUint(now.r);
        c.v[at + 7u] = floatBitsToUint(now.g);
        c.v[at + 8u] = floatBitsToUint(now.b);
        c.v[at + 9u] = floatBitsToUint(total);
        c.v[at + 2u] = 0u;
        c.v[at + 3u] = 0u;
        c.v[at + 4u] = 0u;
        c.v[at + 5u] = 0u;
    }
    else if (push.frame - c.v[at + 1u] > 240u)
    {
        for (uint w = 0u; w < 10u; w++)
            c.v[at + w] = 0u;
    }
}
"""

# ---------------------------------------------------------------- tiles and exposure
TILES_HEAD = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform sampler2D state0;
layout(set = 0, binding = 2) uniform sampler2D state2;
layout(set = 0, binding = 4, rgba32f) uniform writeonly image2D tiles;
layout(set = 0, binding = 5, std430) buffer Exposure { uint histogram[128]; float exposure; uint made; uint pad[2]; } meter;
"""

TILES_BODY = """shared float sum_ce[64], sum_traced[64], sum_count[64];
shared uint bins[128];

/* the light CE's lightmap gave the pixel (the pre-pass's lightmap twin's
fourth word: its light's square root, 8 bits a channel, 255 alpha where
written), its luminance; -1 none */
float ce_light(uvec4 w)
{
    if ((w.w >> 24u) != 255u)
        return -1.0;
    vec3 c = unpackUnorm4x8(w.w).rgb;
    return luma(c * c);
}

/* the sun's angle at a pixel: a level's surface lit on its face alone; an
object's back by half (a leaf lit through, a tree's normals bent out from
it; a closed object's back is in its own shadow, the visibility's) */
float sun_facing(vec3 n, uint flags)
{
    float c = dot(n, pass.sun.xyz);
    return ((flags & 0x100u) != 0u ? max(c, 0.0) : max(c, -0.5 * c)) * pass.sun.w;
}

/* the pixel's traced light (CE's encoding, before the anchor and the
exposure) */
vec3 traced_light(ivec2 texel, uvec4 w, bool structure)
{
    vec4 a = texelFetch(state0, texel, 0);
    vec4 c = texelFetch(state2, texel, 0);
    float v = structure ? c.w : c.x;
    vec3 n = unfold(unpackSnorm2x16(w.x));
    if (dot(n, eye_ray(texel)) > 0.0)
        n = -n;
    float facing = sun_facing(n, w.y);
    return encoded(a.rgb + pass.rig[0].rgb * facing * v);
}

/* each 8x8 tile: the level's light as CE's lightmap gave it and as traced
(their luminances' sums over the pixels with both, and their count), the
anchor's regions; and the exposure's meter: both lights' luminances'
histograms (log2 from -12 to 4, a quarter EV a bin) */
void main()
{
    uint lane = gl_LocalInvocationIndex;
    bins[lane] = 0u;
    bins[lane + 64u] = 0u;
    barrier();
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 full = pass.size.xy;
    float ce = 0.0, traced = 0.0, count = 0.0;
    if (texel.x < full.x && texel.y < full.y)
    {
        uvec4 w = texelFetch(inputs, texel, 0);
        float distance = uintBitsToFloat(w.z);
        float l = (w.y & 0x400u) != 0u && distance > 0.0 ? ce_light(w) : -1.0;
        if (l >= 0.0)
        {
            float e = luma(traced_light(texel, w, false));
            /* (the anchor's measure: the sun through the structure alone, as
            CE's lightmap has it; every pixel's (the light less the sun's is
            the radiance cache's, settled whether or not the pixel's own
            history is: a hold of the settled pixels alone let go as the
            camera moved, and the light pumped between motion and rest) */
            ce = l;
            traced = luma(traced_light(texel, w, true));
            count = 1.0;
            uint be = uint(clamp((log2(max(e, 1.0e-6)) + 12.0) * 4.0, 0.0, 63.0));
            uint bl = uint(clamp((log2(max(l, 1.0e-6)) + 12.0) * 4.0, 0.0, 63.0));
            atomicAdd(bins[be], 1u);
            atomicAdd(bins[64u + bl], 1u);
        }
    }
    sum_ce[lane] = ce;
    sum_traced[lane] = traced;
    sum_count[lane] = count;
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            sum_ce[lane] += sum_ce[lane + stride];
            sum_traced[lane] += sum_traced[lane + stride];
            sum_count[lane] += sum_count[lane + stride];
        }
        barrier();
    }
    if (bins[lane] != 0u)
        atomicAdd(meter.histogram[lane], bins[lane]);
    if (bins[lane + 64u] != 0u)
        atomicAdd(meter.histogram[lane + 64u], bins[lane + 64u]);
    if (lane == 0u)
        imageStore(tiles, ivec2(gl_WorkGroupID.xy), vec4(sum_ce[0], sum_traced[0], sum_count[0], 0.0));
}
"""

EXPOSURE_HEAD = """#version 460
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer Exposure { uint histogram[128]; float exposure; uint made; uint pad[2]; } meter;
"""

EXPOSURE_BODY = """
/* the median bin's luminance (log2) of a histogram, or 99 where empty */
float median_of(uint first)
{
    uint total = 0u;
    for (uint i = 0u; i < 64u; i++)
        total += meter.histogram[first + i];
    if (total < 16u)
        return 99.0;
    uint half_ = total / 2u, sum = 0u;
    for (uint i = 0u; i < 64u; i++)
    {
        sum += meter.histogram[first + i];
        if (sum >= half_)
            return (float(i) + 0.5) / 4.0 - 12.0;
    }
    return 99.0;
}

/* display.prism_exposure: "ce" the traced light's median pinned to CE's
light's over the same pixels (within the range), "auto" to a mid grey of
CE's light (0.45: EV -2 to 5 about it), "fixed" none; the bias added;
moved toward at the speed (EV a second), at once the first time. Its log2
kept, and the histograms emptied for the next frame */
void main()
{
    float mode = pass.rig[11].w, range = pass.rig[12].x, speed = pass.rig[12].y, bias = pass.rig[12].z;
    float traced = median_of(0u), ce = median_of(64u);
    float aim = 0.0;
    if (mode > 1.5 && traced < 90.0)
        aim = clamp(log2(0.45) - traced, -2.0, 5.0) + bias;
    else if (mode > 0.5 && traced < 90.0 && ce < 90.0)
        aim = clamp(ce - traced, -range, range) + bias;
    float now = meter.made == 1u ? meter.exposure : aim;
    if (mode < 0.5)
        now = 0.0;
    else if (isnan(now) || isinf(now))
        now = aim;
    float step_ = speed * pass.rig[14].x;
    meter.exposure = now + clamp(aim - now, -step_, step_);
    meter.made = 1u;
    for (uint i = 0u; i < 128u; i++)
        meter.histogram[i] = 0u;
}
"""

# ---------------------------------------------------------------- atrous
ATROUS_HEAD = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D inputs;
layout(set = 0, binding = 1) uniform sampler2D in0;
layout(set = 0, binding = 2) uniform sampler2D in1;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D out0;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D out1;
layout(set = 0, binding = 6, rgba32f) uniform writeonly image2D result;
layout(set = 0, binding = 7) uniform sampler2D in2;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D out2;
layout(set = 0, binding = 9) uniform sampler2D tiles;
layout(set = 0, binding = 10, std430) readonly buffer Exposure { uint histogram[128]; float exposure; uint made; uint pad[2]; } meter;
layout(push_constant) uniform Step { int step; int last; } step;
layout(set = 0, binding = 11) uniform sampler2D in3;
"""

ATROUS_BODY = """
/* the path tracer's spatial passes, an edge-avoiding a-trous wavelet (5x5,
B3 spline, steps 1, 2, 4, 8 and 16) weighed by the neighbours' distances,
normals and the light's luminance against its variance; the sun's visibility also by its own nearness over its spread
(the second moment's: a converged edge kept sharp). The last
writes what the surfaces read (xgpu.h's XGPU_AMBIENT_OCCLUSION_RT_LIGHT):
x the factor of CE's light (1 - the mood), yzw the light (the rig's sun
by the facing and the visibility, with the rest, in CE's encoding; held to
CE's light over its region by display.prism_rt_anchor, within 1/1.5 to
1.5; by display.prism_exposure's exposure) with the dynamic lights' change,
by the mood */
void main()
{
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 full = pass.size.xy;
    if (texel.x >= full.x || texel.y >= full.y)
        return;
    vec4 c0 = texelFetch(in0, texel, 0), c1 = texelFetch(in1, texel, 0), c2 = texelFetch(in2, texel, 0);
    uvec4 w = texelFetch(inputs, texel, 0);
    float distance = uintBitsToFloat(w.z);
    bool present = (w.y & 0x400u) != 0u && distance > 0.0;
    vec3 n = unfold(unpackSnorm2x16(w.x));
    if (present && pass.info.y != 2u)
    {
        float spread = sqrt(max(c2.y - c2.x * c2.x, 0.0));
        /* (the light's luminance variance, SVGF's (Schied et al., 2017)
        edge-stopping by it: the first pass finds it over the 7x7 of the
        accumulated light about the pixel (its noise once the history is
        short, near none once converged), the later ones carry it in w,
        filtered as the light is, by the weights squared) */
        float l_p = luma(c0.rgb);
        float var_p;
        if (step.step == 1)
        {
            float m1 = 0.0, m2 = 0.0, n_ = 0.0;
            /* (over 7x7 pixels, every third one: the path tracer's noise,
            its lamps' reservoirs shared with their neighbours, comes in
            patches a 3x3 cannot see) */
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++)
                {
                    ivec2 q = clamp(texel + 3 * ivec2(i, j), ivec2(0), full - 1);
                    uvec4 wq = texelFetch(inputs, q, 0);
                    if ((wq.y & 0x400u) == 0u || (wq.y & 0x100u) != (w.y & 0x100u))
                        continue;
                    float l = luma(texelFetch(in0, q, 0).rgb);
                    m1 += l;
                    m2 += l * l;
                    n_ += 1.0;
                }
            m1 /= max(n_, 1.0);
            var_p = max(m2 / max(n_, 1.0) - m1 * m1, 0.0);
            /* (a short history's more: its samples few) */
            var_p *= 1.0 + 4.0 / max(c1.w, 1.0);
            /* (once the history is four frames or more, its own temporal
            variance (the moment's, over its length), the larger of the two:
            a patch of error that is smooth across the 7x7 is still caught) */
            if (c1.w >= 4.0)
            {
                float m2t = texelFetch(in3, texel, 0).w;
                var_p = max(var_p, max(m2t - l_p * l_p, 0.0) / max(c1.w, 1.0));
            }
        }
        else
            var_p = max(c0.w, 0.0);
        float sigma_l = 4.0 * sqrt(var_p) + 0.02 * l_p + 1.0e-4;
        float var_sum = 0.0;
        const float kernel[3] = float[3](0.375, 0.25, 0.0625);
        vec4 s0 = vec4(0.0), s1 = vec4(0.0);
        float s2 = 0.0, s2m = 0.0;
        float weights = 0.0, weights_v = 0.0;
        for (int j = -2; j <= 2; j++)
            for (int i = -2; i <= 2; i++)
            {
                ivec2 q = texel + ivec2(i, j) * step.step;
                if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, full)))
                    continue;
                uvec4 wq = texelFetch(inputs, q, 0);
                float dq = uintBitsToFloat(wq.z);
                if ((wq.y & 0x400u) == 0u || !(dq > 0.0) || (wq.y & 0x100u) != (w.y & 0x100u))
                    continue;
                vec4 q0 = texelFetch(in0, q, 0), q1 = texelFetch(in1, q, 0), q2 = texelFetch(in2, q, 0);
                /* (no samples yet: none to give) */
                if (q1.w <= 0.0)
                    continue;
                vec3 nq = unfold(unpackSnorm2x16(wq.x));
                float along = max(dot(n, nq), 0.0);
                along *= along; along *= along; along *= along;
                /* (the neighbour's distance off the pixel's plane, against a
                reach that grows with the distance: far off, an object's and
                the ground's behind it are within a few hundredths) */
                vec3 pq = pass.camera[0].xyz + dq * eye_ray(q);
                vec3 pp = pass.camera[0].xyz + distance * eye_ray(texel);
                float relative = abs(dot(n, pq - pp)) / (0.003 * distance + 0.01);
                float geometry = kernel[abs(i)] * kernel[abs(j)] * along / (1.0 + relative * relative);
                /* (the light less the sun's is the surfaces' light before
                their textures: smooth but for the geometry's edges, which
                the weights keep; weighing it by its own brightness too
                would favour the dark side of its noise, a lamp's, and
                darken it) */
                /* (by the luminance only once the pixel's history has a
                few frames: a newly seen pixel's sample, a lamp's hit or none,
                is spread by the geometry alone, else its dots stay) */
                float weight = geometry * mix(1.0, exp(-abs(luma(q0.rgb) - l_p) / sigma_l),
                    clamp((c1.w - 1.0) / 3.0, 0.0, 1.0));
                var_sum += weight * weight * (step.step == 1 ? var_p : max(q0.w, 0.0));
                s0 += weight * vec4(q0.rgb, 0.0);
                s1 += weight * q1;
                weights += weight;
                float wv = geometry * (c2.z <= 0.0 ? 1.0 : exp(-abs(q2.x - c2.x) / (4.0 * spread + 0.05)));
                s2 += wv * q2.x;
                s2m += wv * q2.y;
                weights_v += wv;
            }
        if (weights > 1.0e-6)
        {
            c0.rgb = s0.rgb / weights;
            c1.rgb = s1.rgb / weights;
            c1.w = max(c1.w, 1.0);
            /* (w, in the temporary images alone: the variance carried) */
            c0.w = var_sum / (weights * weights);
        }
        /* (the sun's visibility over steps 1 to 4 alone: past them its
        shadows' edges, which converge, would spread) */
        if (weights_v > 1.0e-6 && step.step <= 4)
        {
            c2.x = s2 / weights_v;
            c2.y = s2m / weights_v;
            c2.z = max(c2.z, 1.0);
        }
    }
    if (step.last == 0)
    {
        imageStore(out0, texel, c0);
        imageStore(out1, texel, c1);
        imageStore(out2, texel, c2);
        return;
    }
    if (!present)
    {
        imageStore(result, texel, vec4(1.0, 0.0, 0.0, 0.0));
        return;
    }
    if (dot(n, eye_ray(texel)) > 0.0)
        n = -n;
    /* (an object's back by half: tiles' sun_facing) */
    float facing_c = dot(n, pass.sun.xyz);
    float facing = ((w.y & 0x100u) != 0u ? max(facing_c, 0.0) : max(facing_c, -0.5 * facing_c)) * pass.sun.w;
    vec3 sun = pass.rig[0].rgb * facing * clamp(c2.x, 0.0, 1.0);
    vec3 light = encoded(c0.rgb + sun);
    /* (display.prism_rt_anchor: the region's light held to CE's, its tiles'
    luminances' sums about it (a tent 40 pixels wide), within 1/1.5 to 1.5,
    to the anchor's power) */
    float anchor = pass.rig[10].y;
    float ratio = 1.0;
    vec3 fill = vec3(0.0);
    float region_ce = -1.0;
    if (anchor > 0.0)
    {
        /* (a tent five tiles wide about the pixel's place among them: the
        regions' sums with no edges between tiles) */
        vec2 place = (vec2(texel) + 0.5) / 8.0 - 0.5;
        ivec2 tile = ivec2(floor(place)), tiles_size = textureSize(tiles, 0);
        float ce = 0.0, traced = 0.0, count = 0.0;
        for (int j = -2; j <= 3; j++)
            for (int i = -2; i <= 3; i++)
            {
                ivec2 q = tile + ivec2(i, j);
                vec2 d = abs(vec2(q) - place);
                float tent = max(3.0 - d.x, 0.0) * max(3.0 - d.y, 0.0);
                vec4 t = texelFetch(tiles, clamp(q, ivec2(0), tiles_size - 1), 0);
                ce += tent * t.x;
                traced += tent * t.y;
                count += tent * t.z;
            }
        if (ce > 1.0e-4 && traced > 1.0e-4)
            ratio = pow(clamp(ce / traced, 1.0 / 1.5, 1.5), anchor);
        if (count > 0.5)
            region_ce = ce / count;
        /* (and the light the region's CE light has past that, a lamp the rig
        lacks (a fixture CE baked), added evenly in the pixel's own CE light's
        colour where it has it, by the anchor) */
        if (count > 0.5)
        {
            float missing = max(ce / count - traced / count * ratio, 0.0) * anchor;
            vec3 ce_rgb = unpackUnorm4x8(w.w).rgb;
            vec3 hue = (w.w >> 24u) == 255u ? ce_rgb * ce_rgb : light;
            float hue_luma = luma(hue);
            fill = hue_luma > 1.0e-4 ? hue * (missing / hue_luma) : vec3(missing);
        }
    }
    light = light * ratio + fill;
    /* (and a level pixel's light never under half of CE's there, by the
    anchor: a crease of the remodel CE's texel never saw, a ray started under
    the level drawn, kept from going black) */
    if (anchor > 0.0 && (w.w >> 24u) == 255u)
    {
        vec3 ce_here = unpackUnorm4x8(w.w).rgb;
        /* (less the share of the light an object's shadow takes, which CE's
        lightmap never had: an object's shadow kept) */
        vec3 sun_struct = pass.rig[0].rgb * facing;
        float whole_light = luma(c0.rgb + sun_struct * clamp(c2.w, 0.0, 1.0));
        float taken = luma(sun_struct) * max(clamp(c2.w, 0.0, 1.0) - clamp(c2.x, 0.0, 1.0), 0.0);
        float keep = 1.0 - clamp(taken / max(whole_light, 1.0e-4), 0.0, 1.0);
        light = max(light, ce_here * ce_here * (0.5 * anchor * keep));
    }
    /* (an object's (CE lit it from the level's light under it, no shadow of
    its own: a trunk under its canopy, a Marine's back): never under half of
    the level's CE light about it, by the anchor, less its share in an
    object's shadow, as the level's are held) */
    else if (anchor > 0.0 && (region_ce > 0.0 || (w.w >> 24u) == 254u))
    {
        vec3 sun_struct = pass.rig[0].rgb * facing;
        float whole_light = luma(c0.rgb + sun_struct * clamp(c2.w, 0.0, 1.0));
        float taken = luma(sun_struct) * max(clamp(c2.w, 0.0, 1.0) - clamp(c2.x, 0.0, 1.0), 0.0);
        float keep = 1.0 - clamp(taken / max(whole_light, 1.0e-4), 0.0, 1.0);
        float least = region_ce * 0.5 * anchor * keep;
        /* (where the model's twin gave its own light as CE lit it (254 in
        the fourth word's top byte): half of that, an object's shadow on it
        taking at most half of the hold) */
        if ((w.w >> 24u) == 254u)
        {
            vec3 own = unpackUnorm4x8(w.w).rgb;
            least = luma(own * own) * 0.5 * anchor * (0.5 + 0.5 * keep);
        }
        float y = luma(light);
        if (y < least)
            light = y > 1.0e-4 ? light * (least / y) : vec3(least);
    }
    float exposure = exp2(clamp(meter.exposure, -8.0, 8.0));
    light *= exposure;
    float mood = pass.pt2.x;
    vec3 add = (light + c1.rgb) * mood;
    float factor = 1.0 - mood;
    if (any(isnan(add)) || isnan(factor))
    {
        factor = 1.0;
        add = vec3(0.0);
    }
    /* (debug.prism_rt_view: 1 the sun's visibility, 2 the light less the
    sun's (the sky, domes, lamps and bounces), 3 the anchor's ratio about
    a half grey, 5 CE's light where the pre-pass gave it) */
    uint view = pass.info.w;
    if (view == 1u)
        imageStore(result, texel, vec4(0.0, vec3(clamp(c2.x, 0.0, 1.0))));
    else if (view == 2u)
        imageStore(result, texel, vec4(0.0, encoded(c0.rgb) * exposure));
    else if (view == 3u)
        imageStore(result, texel, vec4(0.0, vec3(0.5 * ratio)));
    else if (view == 5u)
    {
        vec3 c = (w.w >> 24u) == 255u ? unpackUnorm4x8(w.w).rgb : vec3(1.0, 0.0, 1.0);
        imageStore(result, texel, vec4(0.0, (w.w >> 24u) == 255u ? c * c : c));
    }
    else
        imageStore(result, texel, vec4(factor, add));
}
"""


# ---------------------------------------------------------------- the tone curve (display.prism_tone_curve)
CURVE_MEAN = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D view;
layout(set = 0, binding = 1, rgba32f) uniform writeonly image2D means;
layout(push_constant) uniform Curve { ivec4 rectangle; vec4 mode; } curve;
shared float sums[64];

/* each 8x8 block of the view's rectangle: its pixels' mean log2
luminance (the local operator's base, before its blur) */
void main()
{
    uint lane = gl_LocalInvocationIndex;
    ivec2 at = curve.rectangle.xy + ivec2(gl_GlobalInvocationID.xy);
    float l = 0.0;
    if (all(lessThan(at, curve.rectangle.zw)))
        l = log2(max(dot(texelFetch(view, at, 0).rgb, vec3(0.2126, 0.7152, 0.0722)), 1.0e-4));
    else
        l = log2(0.18);
    sums[lane] = l;
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            sums[lane] += sums[lane + stride];
        barrier();
    }
    if (lane == 0u)
        imageStore(means, ivec2(gl_WorkGroupID.xy), vec4(sums[0] / 64.0));
}
"""

CURVE = """#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D view;
layout(set = 0, binding = 1) uniform sampler2D means;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D curved;
layout(push_constant) uniform Curve { ivec4 rectangle; vec4 mode; } curve;

/* the filmic shoulder: CE's light as it is up to 0.6 of white, past it
rolled off toward white (never past it: the display's own tone map then
has nothing to clip) */
float shoulder(float x)
{
    const float k = 0.6;
    return x <= k ? x : k + (1.0 - k) * (1.0 - exp(-(x - k) / (1.0 - k)));
}

/* a colour through a curve of its brightest channel (its hue kept) */
vec3 by_brightest(vec3 c, float mapped, float brightest)
{
    return brightest > 1.0e-5 ? c * (mapped / brightest) : c;
}

/* display.prism_tone_curve over the 3D view (CE's encoding, 1 white,
past it the traced light): filmic (1) the shoulder; aces (2) Narkowicz's
fit of ACES's; local (3) each region's light (the 8x8 blocks' mean log
luminance, a 3x3 tent of them) compressed past half white (by 0.6) and
lifted a little under an eighth (to 0.85), its detail about it kept, then
the shoulder */
void main()
{
    ivec2 at = curve.rectangle.xy + ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(at, curve.rectangle.zw)))
        return;
    vec3 c = max(texelFetch(view, at, 0).rgb, vec3(0.0));
    float brightest = max(c.r, max(c.g, c.b));
    int mode = int(curve.mode.x);
    vec3 outcome = c;
    if (mode == 1)
        outcome = by_brightest(c, shoulder(brightest), brightest);
    else if (mode == 2)
    {
        vec3 x = c;
        outcome = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
    }
    else if (mode == 3)
    {
        vec2 block = (vec2(at - curve.rectangle.xy) + 0.5) / 8.0 - 0.5;
        ivec2 size = textureSize(means, 0);
        float base = 0.0, weights = 0.0;
        for (int j = -1; j <= 2; j++)
            for (int i = -1; i <= 2; i++)
            {
                ivec2 q = clamp(ivec2(floor(block)) + ivec2(i, j), ivec2(0), size - 1);
                vec2 d = abs(vec2(q) - block);
                float w = max(1.5 - d.x, 0.0) * max(1.5 - d.y, 0.0);
                base += w * texelFetch(means, q, 0).r;
                weights += w;
            }
        base = weights > 0.0 ? base / weights : -2.5;
        float l = log2(max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1.0e-4));
        float detail = l - base;
        float compressed = base > -1.0 ? -1.0 + (base + 1.0) * 0.6 : base < -3.0 ? -3.0 + (base + 3.0) * 0.85 : base;
        float scale = exp2(compressed + detail - l);
        vec3 local = c * scale;
        float b = max(local.r, max(local.g, local.b));
        outcome = by_brightest(local, shoulder(b), b);
    }
    imageStore(curved, at, vec4(outcome, 1.0));
}
"""

HEADER = (HERE / "header.txt").read_text()

parts = [HEADER,
         source("trace", TRACE_HEAD, BASIC + TRACE + TRACE_BODY, 3),
         "\n",
         source("resolve", RESOLVE_HEAD, BASIC + RESOLVE_BODY, 5),
         "\n",
         source("probes", PROBES_HEAD, BASIC + TRACE + PROBES_BODY, 2),
         "\n",
         (HERE / "path_comment.txt").read_text(),
         source("path", PATH_HEAD, BASIC + PATH_TRACE + PATH_BODY, 3),
         "\n",
         source("accumulate", ACCUMULATE_HEAD, BASIC + ACCUMULATE_BODY, 3),
         "\n",
         "/* debug.pt_reference's sums (xgpu_vulkan_rt.c's reference pass), in the temporal pass's place */\n\n",
         source("reference", REFERENCE_HEAD, BASIC + REFERENCE_BODY, 3),
         "\n",
         source("tiles", TILES_HEAD, BASIC + TILES_BODY, 3),
         "\n",
         source("exposure", EXPOSURE_HEAD, EXPOSURE_BODY, 1),
         "\n",
         "static const char cache_resolve_source[] =\n%s;\n" % literal(CACHE_RESOLVE.rstrip("\n")),
         "\n",
         source("atrous", ATROUS_HEAD, BASIC + ATROUS_BODY, 3),
         "\n",
         "/* display.prism_tone_curve (xgpu_vulkan_rt.c's xgpu_vulkan_rt_tone_curve): the 3D view's light past\n"
         "white rolled off, over a copy of the view's rectangle. curve_mean: its 8x8 blocks' mean log luminance;\n"
         "curve: the curve, into an image copied back over the view. */\n\n",
         "static const char curve_mean_source[] =\n%s;\n\n" % literal(CURVE_MEAN.rstrip("\n")),
         "static const char curve_source[] =\n%s;\n" % literal(CURVE.rstrip("\n")),
         "\n",
         TAIL]
TARGET = ROOT / "port/linux/vulkan/xgpu_vulkan_rt_glsl.inc"
if "--check" in sys.argv[1:]:
    if TARGET.read_text() != "".join(parts):
        raise SystemExit("%s is not what tools/prism_rt_glsl/generate.py writes: run it" % TARGET)
else:
    TARGET.write_text("".join(parts))
