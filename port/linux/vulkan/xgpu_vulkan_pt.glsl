/* PRISM Path Traced's compute passes (display.lighting "pathtraced", M3;
xgpu_vulkan_pt.c; docs/prism_pathtraced.md), in Vulkan's GLSL. Not compiled
into the game: tools/vulkan_pt_shaders.py makes xgpu_vulkan_pt_shaders.c of
it. Each pass is the baker's BAKE_COMMON (xgpu_vulkan_bake_glsl.inc: the
bake's scene, its materials, lamps, light-importance grid, fixtures and rig,
and the estimator's light functions), then this file's common part, then
the pass's own part (the sections "//@common" and "//@pass <name>").

The path tracer reads the scene the bake was made from, by the bake's own
functions, so that what it traces and what the bake holds are one
estimator's: nothing is counted twice, and its reference (the pass
"reference") is that estimator unbiased, with no bake, at every pixel.

At each opaque pixel of the G-buffer (raster primary visibility):

  the sun: one ray toward a point of its disc within settings.x world units
    of the eye (and at every object's pixel), else the baked sun mask (the
    level's);
  the domes: one direction of each one's cone, its shadow traced (the
    bake's domes_at);
  the lamps (the emitting triangles), the fixtures and the dynamic lights:
    resampled importance (RIS) over candidates whose lamps come from the
    light-importance grid (the bake's lamp_pick: its cell's seen and near
    lists, the rest by power), the target each one's unshadowed diffuse and
    specular light at the pixel, one kept and its shadow traced;
  the sky's ambient floor (the units' rig): a constant, as the bake has it;
  the indirect light: the bake's indirect layer at the pixel ("performance"
    where the level has its texel), else one bounce, its hit lit from the
    bake (the hit's texel or the irradiance volume, the sun there by the
    mask or the volume's visibility, its glow), the sky's domes where it
    leaves;
  the reflections: below a roughness, one GGX ray (the visible normals'
    distribution) whose hit is lit from the bake as the bounce's; above
    it, the bake's reflection probes.

The noisy parts (the direct light, the traced indirect light, the traced
reflections, the sun's visibility) go through the denoiser (temporal, fix,
atrous); the rest (the floor, the bake's indirect light at the pixel, the
probes' reflections) is added after it. composite puts the frame together,
in Raster's form: the diffuse light by the base colour less the specular's
share, the specular light, the emissive light. */

//@common
#extension GL_KHR_shader_subgroup_arithmetic : require

/* the passes' options fixed when their pipelines are made (specialization
constants, xgpu_vulkan_pt.c's spec_options: the same variables the job's
fields take), so that an option that is off leaves no code in the light
pass: its registers held whether or not it runs cost the pass about 0.8 ms
of the RTX 3070 Ti's at G4 (round 7: the radiance cache's, the clamp's, the
spatial reuse's). The job's fields still give each option's values */
layout(constant_id = 0) const bool SPEC_CACHE = false;
layout(constant_id = 1) const bool SPEC_CLAMP = false;
layout(constant_id = 2) const bool SPEC_CONES = false;
layout(constant_id = 3) const int SPEC_SPATIAL = 0;
layout(constant_id = 4) const bool SPEC_DIAG = false;
layout(constant_id = 5) const bool SPEC_BOIL = false;
layout(constant_id = 6) const int SPEC_BOUNCE_HALF = 0;
layout(constant_id = 7) const bool SPEC_SUN_TILES = false;

const float PI = 3.14159265;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Job
{
    uvec2 pages, charts, vpages, table, tiles, indirect, sun, masks;
    uvec2 bricks, coarse, probes, lightmaps, reflections, reflection_table, reflection_data, lights;
    uvec2 sums, stats, meter, counters, tree, lamps_out, objects, object_table;
    /* the radiance cache's cells (0: none); ReSTIR DI's reuse flags
    (REUSE_*); a diagnosis's parts left out (PARTS_*, 0 in play) */
    uvec2 cache;
    uint reuse_flags, parts_off;
    vec4 camera[4];
    vec4 last_camera[4];
    ivec4 corners;
    ivec4 last_corners;
    uvec4 counts;
    uvec4 more;
    vec4 volume;
    uvec4 frame;
    vec4 settings;
    vec4 denoise;
    vec4 extra;
    /* display.prism_exposure (0 fixed, 1 ce, 2 auto), display.prism_tone_curve
    (0 ce, 1 filmic, 2 aces, 3 local), 1 where the frame meters (job.meter) */
    vec4 exposure;
    /* the indirect light's clamp (a bounce's luminance, 0 none), the radiance
    cache's cell at a unit's distance, the samples a cell needs before it is
    read, the most it keeps */
    vec4 cache_settings;
};
Job job;
/* (the light pass's lamps' specular light shared by multiple importance
sampling between ReSTIR DI and the reflection's GGX ray (lamp_mis_nee): a
pixel whose reflection is traced) */
bool lamps_mis = false;
/* (hit_light_n's last triangle met on the level, else 0xffffffff) */
uint hit_light_triangle = 0xffffffffu;

/* the frame's counters (job.counters, in the job's own block, zero at its
start; the host reads them once the frame is done): [0] the level pixels
whose history was looked for (one in 16), [1] those whose last frame's place
was found on its own surface there, [2] those whose place was in the last
frame's view, [3] those whose history was taken */
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Counters
{
    uint v[4];
};

/* job.reuse_flags: the spatial reservoirs kept for the next frame (round
4's, which spread a sample from pixel to pixel over the frames: the blobs in
motion); the temporal reuse by a permutation of the 4 x 4 about the place; the spatial disc's radius in
pixels in bits 16 to 23 (0: 16);
the boiling filter's strength in hundredths (bits 8 to 15, 0 none) */
const uint REUSE_FEEDBACK = 1u;
const uint REUSE_PERMUTE = 2u;
/* job.parts_off (a diagnosis of the light pass's cost, HALO_PT_PARTS_OFF):
the sun's ray, the domes, ReSTIR DI's spatial reuse, the bounce, the
reflection's ray, ReSTIR GI's reuse, the spatial neighbours' traced Z left
out */
const uint PARTS_SUN = 1u;
const uint PARTS_DOMES = 2u;
const uint PARTS_SPATIAL = 4u;
const uint PARTS_BOUNCE = 8u;
const uint PARTS_REFLECTION = 16u;
const uint PARTS_GI_REUSE = 32u;
const uint PARTS_Z = 64u;
bool part_off(uint part) { return SPEC_DIAG && (job.parts_off & part) != 0u; }
/* job.frame.y's flags */
const uint FLAG_HISTORY = 1u;
const uint FLAG_PERFORMANCE = 2u;
const uint FLAG_DENOISER_OFF = 4u;
const uint FLAG_REFERENCE = 8u;
const uint FLAG_TILES = 16u;
/* the reference's sums: floats a pixel (the diffuse light's mean, the
specular's, the luminance's mean and squared deviations, the distance, then
the means and squared deviations of its parts' luminances: the direct, the
diffuse bounce's, the reflection's) */
const uint SUMS = 16u;

layout(set = 0, binding = 0) uniform usampler2D words;
layout(set = 0, binding = 1) uniform usampler2D extras;
layout(set = 0, binding = 2) uniform sampler2D base_in;
layout(set = 0, binding = 3) uniform sampler2D additions_in;
layout(set = 0, binding = 4, rgba16f) uniform image2D noisy_diffuse;
layout(set = 0, binding = 5, rgba16f) uniform image2D noisy_specular;
layout(set = 0, binding = 6, rgba16f) uniform image2D clean_diffuse;
layout(set = 0, binding = 7, rgba16f) uniform image2D clean_specular;
layout(set = 0, binding = 8, rgba16f) uniform image2D sun_raw;
layout(set = 0, binding = 9, rgba32ui) uniform uimage2D guide_now;
layout(set = 0, binding = 10, rgba32ui) uniform uimage2D guide_last;
layout(set = 0, binding = 11, rgba16f) uniform image2D diff_last;
layout(set = 0, binding = 12, rgba16f) uniform image2D diff_now;
layout(set = 0, binding = 13, rgba16f) uniform image2D spec_last;
layout(set = 0, binding = 14, rgba16f) uniform image2D spec_now;
layout(set = 0, binding = 15, rgba16f) uniform image2D moments_last;
layout(set = 0, binding = 16, rgba16f) uniform image2D moments_now;
layout(set = 0, binding = 17, rgba16f) uniform image2D sun_last;
layout(set = 0, binding = 18, rgba16f) uniform image2D sun_now;
layout(set = 0, binding = 19, rgba16f) uniform image2D diff_a;
layout(set = 0, binding = 20, rgba16f) uniform image2D diff_b;
layout(set = 0, binding = 21, rgba16f) uniform image2D spec_a;
layout(set = 0, binding = 22, rgba16f) uniform image2D spec_b;
layout(set = 0, binding = 23, rgba16f) uniform image2D sun_a;
layout(set = 0, binding = 24, rgba16f) uniform image2D sun_b;
layout(set = 0, binding = 25, rgba16f) uniform image2D frame_out;
layout(set = 0, binding = 26) uniform sampler2D light_in;
layout(set = 0, binding = 27, rgba16f) uniform image2D spec_moments_last;
layout(set = 0, binding = 28, rgba16f) uniform image2D spec_moments_now;
/* ReSTIR DI's reservoirs (the last frame's, this frame's): the kept
sample's point (a lamp's point, a fixture's place) and its weight W; its
kind and index, its confidence M (frames) */
layout(set = 0, binding = 29, rgba32f) uniform image2D reservoir_point_last;
layout(set = 0, binding = 30, rgba32f) uniform image2D reservoir_point_now;
layout(set = 0, binding = 31, rgba32ui) uniform uimage2D reservoir_info_last;
layout(set = 0, binding = 32, rgba32ui) uniform uimage2D reservoir_info_now;
/* ReSTIR GI's reservoirs (the last frame's, this frame's): the kept bounce
sample's hit point and the hit's normal (folded, its bits); its radiance
toward the pixel's surface (halves), its confidence M (a half) and its
weight W */
layout(set = 0, binding = 33, rgba32f) uniform image2D gi_point_last;
layout(set = 0, binding = 34, rgba32f) uniform image2D gi_point_now;
layout(set = 0, binding = 35, rgba32f) uniform image2D gi_data_last;
layout(set = 0, binding = 36, rgba32f) uniform image2D gi_data_now;

ivec2 size_of() { return ivec2(job.counts.y, job.counts.z); }
bool inside(ivec2 p) { return p.x >= 0 && p.y >= 0 && p.x < int(job.counts.y) && p.y < int(job.counts.z); }

vec3 unfold(vec2 f)
{
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
vec2 fold(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 f = n.xy;
    if (n.z < 0.0)
        f = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return f;
}
float power16(float x)
{
    x *= x;
    x *= x;
    x *= x;
    return x * x;
}
vec3 rgb9e5(uint v)
{
    float e = exp2(float(int(v >> 27u) - 15 - 9));
    return vec3(float(v & 511u), float((v >> 9u) & 511u), float((v >> 18u) & 511u)) * e;
}

/* a pixel of the G-buffer: its point, normal (toward the eye where the level
is one-sided), the eye's direction, the material, the instance */
struct Surface
{
    bool present;
    uint instance;
    vec3 point;
    vec3 normal;
    vec3 eye;
    float distance;
    vec3 albedo;
    float roughness;
    float metal;
    float specular;
    uint page;
    vec2 lightmap;
    vec3 emissive;
    bool has_history;
    vec2 motion;
};

vec3 ray_of(vec4 camera[4], ivec4 corners, vec2 pixel)
{
    vec4 c = vec4(corners);
    vec2 sxy = vec2(2.0 * (pixel.x + 0.5 - c.x) / max(c.z - c.x, 1.0) - 1.0,
        2.0 * (pixel.y + 0.5 - c.y) / max(c.w - c.y, 1.0) - 1.0);
    return camera[1].xyz + sxy.x * camera[2].xyz + sxy.y * camera[3].xyz;
}

/* where a pixel's surface was the frame before: its pixel there (the
motion) and the point the last frame's camera saw at a pixel of it */
vec3 last_point(ivec2 at, float distance)
{
    vec3 ray = ray_of(job.last_camera, job.last_corners, vec2(at));
    return job.last_camera[0].xyz + distance * ray;
}

/* where a pixel's surface was in the last frame's target: the level stands
still, so its point is found in the last frame's camera by its geometry
(the camera's rays solved for the point: no motion vector trusted); an
object by its screen motion (the G-buffer's) */
vec2 last_pixel(vec3 point, vec2 pixel, vec2 motion, bool level)
{
    if (!level)
        return pixel + motion;
    mat3 rays = mat3(job.last_camera[1].xyz, job.last_camera[2].xyz, job.last_camera[3].xyz);
    if (abs(determinant(rays)) < 1.0e-12)
        return pixel + motion;
    vec3 u = inverse(rays) * (point - job.last_camera[0].xyz);
    if (u.x <= 1.0e-6)
        return vec2(-1.0e6);
    vec2 sxy = u.yz / u.x;
    vec4 c = vec4(job.last_corners);
    return vec2((sxy.x + 1.0) * 0.5 * (c.z - c.x) + c.x - 0.5, (sxy.y + 1.0) * 0.5 * (c.w - c.y) + c.y - 0.5);
}

/* (port: the objects' instances a ray sees: 4 and 8, and 16 the player's own
body in first person (XGPU_GBUFFER_FLAG_TRACE_ONLY: a shadow caster never
drawn), which the rays of the first-person view's pixels (the weapon and
arms, inside or before that body) leave out; surface_at sets it a pixel) */
uint object_cull = 28u;
Surface surface_at(ivec2 pixel)
{
    Surface g;
    uvec4 w = texelFetch(words, pixel, 0);
    uvec4 x = texelFetch(extras, pixel, 0);
    /* (port: a distance that is not finite, a stray word, no surface: never
    a ray's origin) */
    g.present = (w.w & 0x8000u) != 0u && uintBitsToFloat(x.w) > 0.0 && (x.w & 0x7f800000u) != 0x7f800000u;
    g.instance = w.w >> 16;
    object_cull = g.instance == 0xffffu ? 12u : 28u;
    g.page = (x.z >> 17u) & 0x7fu;
    g.lightmap = unpackUnorm2x16(w.x);
    g.distance = uintBitsToFloat(x.w);
    vec3 ray = ray_of(job.camera, job.corners, vec2(pixel));
    g.point = job.camera[0].xyz + g.distance * ray;
    g.eye = -normalize(ray);
    bool has_normal = ((w.z >> 28u) & 8u) != 0u;
    vec3 n = has_normal ? unfold(unpackSnorm2x16(w.y)) : g.eye;
    if (dot(n, ray) > 0.0 && g.instance == 0u)
        n = -n;
    /* (an object's normal turned from the eye (its vertices' normal at a
    silhouette, a two-sided card seen from behind) mirrored about the plane
    square to the eye: its sideways part kept, its part along the eye made
    toward it. Left turned away, the GGX lobe's terms go negative (the
    reference summed negative reflections: round 1's black fringes about
    b30's rocks and over its trees' leaves) */
    float toward = dot(n, g.eye);
    if (toward < 0.0 && g.instance != 0u)
        n = normalize(n - 2.0 * toward * g.eye);
    g.normal = n;
    vec4 m = unpackUnorm4x8(w.z);
    g.roughness = clamp(m.x, 0.04, 1.0);
    g.metal = m.y;
    g.specular = m.z;
    g.albedo = g.instance == 0u ? texelFetch(base_in, pixel, 0).rgb : unpackUnorm4x8(w.x).rgb;
    vec3 em = vec3(unpackHalf2x16(x.y), unpackHalf2x16(x.z & 0xffffu).x);
    g.emissive = g.instance == 0u ? em * g.albedo : em;
    g.has_history = (x.z & 0x10000u) != 0u;
    g.motion = unpackHalf2x16(x.x);
    return g;
}

/* the material's specular colour and the split sum's share (Raster's: CE's
base maps are diffuse colours, metal or not, so metalness tints the
reflection half way to the base colour and the diffuse keeps the rest) */
vec3 f0_of(Surface g) { return mix(vec3(0.08 * g.specular), g.albedo, 0.5 * g.metal); }
vec2 environment_brdf(float roughness, float nv)
{
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a + r.zw;
}
vec3 specular_share(Surface g)
{
    vec2 ab = environment_brdf(g.roughness, clamp(dot(g.normal, g.eye), 1.0e-3, 1.0));
    return f0_of(g) * ab.x + ab.y;
}
/* GGX's specular reflectance times the cosine toward l (no pi: the light's
radiance in, the radiance out), Smith's height-correlated visibility,
Schlick's Fresnel */
vec3 ggx(Surface g, vec3 l)
{
    float nl = dot(g.normal, l);
    float nv = clamp(dot(g.normal, g.eye), 1.0e-3, 1.0);
    if (nl <= 0.0)
        return vec3(0.0);
    vec3 h = normalize(l + g.eye);
    float nh = max(dot(g.normal, h), 0.0), vh = max(dot(g.eye, h), 0.0);
    float a = g.roughness * g.roughness, a2 = a * a;
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float distribution = a2 / (PI * dd * dd);
    float visibility = 0.5 / max(nl * (nv * (1.0 - a) + a) + nv * (nl * (1.0 - a) + a), 1.0e-4);
    vec3 f0 = f0_of(g);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    return min(distribution * visibility, 64.0) * fresnel * nl;
}

/* the density over directions of ggx_sample's reflection l (GGX's visible
normals: G1(v) D(m) / (4 v.n), Heitz 2018), its alpha as ggx_sample's */
float ggx_pdf(Surface g, vec3 l)
{
    float nv = dot(g.normal, g.eye), nl = dot(g.normal, l);
    if (nv <= 1.0e-4 || nl <= 0.0)
        return 0.0;
    vec3 h = normalize(l + g.eye);
    float nh = max(dot(g.normal, h), 0.0);
    float alpha = max(g.roughness * g.roughness, 1.0e-3), a2 = alpha * alpha;
    float dd = nh * nh * (a2 - 1.0) + 1.0;
    float distribution = a2 / (PI * dd * dd);
    float g1v = 2.0 * nv / (nv + sqrt(a2 + (1.0 - a2) * nv * nv));
    return g1v * distribution / (4.0 * nv);
}
/* a lamp's specular light's share taken by next-event estimation (ReSTIR
DI) where the reflection's ray is traced too (lamps_mis): the power
heuristic between the GGX ray's density toward the lamp and the lamp's own
density were it picked alone (1 over the solid angle it subtends, its area
by its cosine over the distance squared). The reflection's ray takes the
rest (1 minus it) where it meets the lamp's face: any such pair of weights
that sum to 1 at every direction keeps the light unbiased, and this one
gives a lamp wider than the lobe to the ray (a mirror's highlight, which a
point on the lamp rarely finds) and a lamp narrower than the lobe to the
lamp's point (a rough floor's, which the ray rarely meets) */
float lamp_mis_nee(Surface g, vec3 l, float solid)
{
    if (!lamps_mis)
        return 1.0;
    float pb = ggx_pdf(g, l), q = 1.0 / max(solid, 1.0e-8);
    return q * q / (q * q + pb * pb);
}

/* ---------- the bake's lookups (Raster's, xgpu_vulkan_bake_glsl.inc's raster pass) */

bool texel_of(uint vp, ivec2 x, out uvec2 w, out uint indirect_word)
{
    Uints v = Uints(job.vpages);
    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];
    w = uvec2(0u);
    indirect_word = 0u;
    if (x.x < 0 || x.y < 0)
        return false;
    uvec2 t = uvec2(x) / 16u;
    if (t.x >= tiles_x || t.y >= tiles_y)
        return false;
    uint physical = Uints(job.table).v[v.v[vp * 8u + 4u] + t.y * tiles_x + t.x];
    if (physical == 0u)
        return false;
    uvec2 l = uvec2(x) % 16u;
    uint at = (physical - 1u) * 512u + (l.y * 16u + l.x) * 2u;
    Uints tl = Uints(job.tiles);
    w = uvec2(tl.v[at], tl.v[at + 1u]);
    if (job.indirect != uvec2(0u))
        indirect_word = Uints(job.indirect).v[(physical - 1u) * 256u + l.y * 16u + l.x];
    return (w.y & 0xff000000u) != 0u || w.x != 0u || w.y != 0u;
}
/* (sun_of's last call: whether its tile is all in the sun's shadow, the
bake's state 1: none of its 16 x 16 texels' samples saw the sun) */
bool sun_tile_dark = false;
float sun_of(uint vp, vec2 x)
{
    sun_tile_dark = false;
    Uints v = Uints(job.vpages);
    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];
    ivec2 t = ivec2(floor(x / 16.0));
    if (any(lessThan(t, ivec2(0))) || t.x >= int(tiles_x) || t.y >= int(tiles_y))
        return -1.0;
    uint state = Uints(job.sun).v[v.v[vp * 8u + 4u] + uint(t.y) * tiles_x + uint(t.x)];
    if (state == 0u)
        return -1.0;
    if (state < 3u)
    {
        sun_tile_dark = state == 1u;
        return state == 2u ? 1.0 : 0.0;
    }
    vec2 m = clamp((x - vec2(t) * 16.0) * 4.0 - 0.5, vec2(0.0), vec2(63.0));
    ivec2 a = ivec2(floor(m));
    ivec2 b = min(a + 1, ivec2(63));
    vec2 f = m - vec2(a);
    Uints mk = Uints(job.masks);
    uint base = (state - 3u) * 4096u;
    uint i00 = base + uint(a.y * 64 + a.x), i10 = base + uint(a.y * 64 + b.x);
    uint i01 = base + uint(b.y * 64 + a.x), i11 = base + uint(b.y * 64 + b.x);
    float v00 = float((mk.v[i00 >> 2u] >> ((i00 & 3u) * 8u)) & 255u);
    float v10 = float((mk.v[i10 >> 2u] >> ((i10 & 3u) * 8u)) & 255u);
    float v01 = float((mk.v[i01 >> 2u] >> ((i01 & 3u) * 8u)) & 255u);
    float v11 = float((mk.v[i11 >> 2u] >> ((i11 & 3u) * 8u)) & 255u);
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y) / 255.0;
}

/* the bake at a level point: its CE lightmap page (1 on) and coordinates
(0 to 1 on the page), the normal it is read at: the light less the sun's
(the bake's total: direct and indirect), the indirect light alone, the sun's
visibility (the mask; -1 unknown); false where no texel is baked about it */
bool bake_at(uint page, vec2 uv, vec3 n, out vec3 total, out vec3 indirect_light, out float sun_v)
{
    total = vec3(0.0);
    indirect_light = vec3(0.0);
    sun_v = -1.0;
    if (page == 0u || page > job.counts.x || job.pages == uvec2(0u))
        return false;
    Uints pg = Uints(job.pages);
    uint offset = pg.v[(page - 1u) * 4u], pw = pg.v[(page - 1u) * 4u + 1u], ph = pg.v[(page - 1u) * 4u + 2u];
    vec2 ce = uv * vec2(pw, ph);
    ivec2 cell = clamp(ivec2(floor(ce)), ivec2(0), ivec2(pw, ph) - 1);
    Uints cm = Uints(job.charts);
    uint ci = offset + uint(cell.y) * pw + uint(cell.x);
    uint vp1 = (cm.v[ci >> 1u] >> ((ci & 1u) * 16u)) & 0xffffu;
    if (vp1 == 0u)
        return false;
    uint vp = vp1 - 1u;
    float scale = float(Uints(job.vpages).v[vp * 8u + 1u]);
    vec2 tx = ce * scale - 0.5;
    ivec2 a = ivec2(floor(tx));
    vec2 f = tx - vec2(a);
    float weights = 0.0;
    for (int k = 0; k < 4; k++)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        uvec2 tw;
        uint iw;
        if (!texel_of(vp, a + o, tw, iw))
            continue;
        vec4 dir = unpackUnorm4x8(tw.y);
        if (dir.w <= 0.0)
            continue;
        vec3 kk = (dir.xyz - 0.5) * 4.0;
        float a0 = dir.w * 4.0 - 1.0;
        float factor = clamp(a0 + dot(kk, n), 0.0, 4.0);
        float wk = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        total += wk * rgb9e5(tw.x) * factor;
        indirect_light += wk * rgb9e5(iw) * factor;
        weights += wk;
    }
    if (weights <= 1.0e-4)
        return false;
    total /= weights;
    indirect_light /= weights;
    sun_v = sun_of(vp, ce * scale);
    return true;
}

/* a cascade of the irradiance volume at p: the light over pi at the normal n
(rgb) and the sun's visibility (w); false none */
bool cascade_at(uvec2 table, uint entries, float spacing, vec3 p, vec3 n, out vec4 at_n)
{
    at_n = vec4(-1.0);
    if (entries == 0u || table == uvec2(0u))
        return false;
    vec3 f = p / spacing - 0.5;
    ivec3 g0 = ivec3(floor(f));
    vec3 t = f - vec3(g0);
    vec3 sum = vec3(0.0);
    float sun = 0.0, weights = 0.0;
    Uints bt = Uints(table);
    Floats pr = Floats(job.probes);
    vec4 yn = vec4(0.282095 * 3.14159265, 0.488603 * 2.0943951 * n.y, 0.488603 * 2.0943951 * n.z,
        0.488603 * 2.0943951 * n.x);
    for (int c = 0; c < 8; c++)
    {
        ivec3 gg = g0 + ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        ivec3 brick = ivec3(floor(vec3(gg) / 4.0));
        ivec3 local = gg - brick * 4;
        ivec3 key3 = brick + ivec3(32768);
        uint key = (uint(key3.x) & 2047u) | ((uint(key3.y) & 2047u) << 11u) | ((uint(key3.z) & 1023u) << 22u);
        uint h = hash_u(key ^ 0x68e31da4u) & (entries - 1u);
        int first = -1;
        for (uint probe = 0u; probe < 16u; probe++)
        {
            uint at = ((h + probe) & (entries - 1u)) * 2u;
            uint k = bt.v[at];
            if (k == 0u)
                break;
            if (k == key + 1u)
            {
                first = int(bt.v[at + 1u]);
                break;
            }
        }
        if (first < 0)
            continue;
        uint index = uint(first) + uint(local.z * 16 + local.y * 4 + local.x);
        uint b = index * 8u;
        float valid = pr.v[b + 7u];
        if (valid <= 0.0)
            continue;
        vec3 w3 = mix(1.0 - t, t, vec3(c & 1, (c >> 1) & 1, (c >> 2) & 1));
        float w = w3.x * w3.y * w3.z;
        vec4 cr = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b])), unpackHalf2x16(floatBitsToUint(pr.v[b + 1u])));
        vec4 cg = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b + 2u])), unpackHalf2x16(floatBitsToUint(pr.v[b + 3u])));
        vec4 cb = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b + 4u])), unpackHalf2x16(floatBitsToUint(pr.v[b + 5u])));
        sum += w * max(vec3(dot(cr, yn), dot(cg, yn), dot(cb, yn)) / 3.14159265, vec3(0.0));
        sun += w * pr.v[b + 6u];
        weights += w;
    }
    if (weights <= 1.0e-3)
        return false;
    at_n = vec4(sum / weights, sun / weights);
    return true;
}
bool volume_at(vec3 p, vec3 n, out vec4 at_n)
{
    if (cascade_at(job.bricks, job.counts.w, job.volume.x, p, n, at_n))
        return true;
    return cascade_at(job.coarse, job.more.x, job.volume.y, p, n, at_n);
}

/* the reflection probes: the nearest few to p whose boxes hold it,
parallax-corrected along r, at the roughness's level; found 0 none */
vec3 reflection_at(vec3 p, vec3 r, float roughness, out float found)
{
    found = 0.0;
    uint count = job.more.y;
    if (count == 0u || job.reflections == uvec2(0u))
        return vec3(0.0);
    Floats pr = Floats(job.reflections);
    ivec3 cell = ivec3(floor(p / 8.0));
    uint entries = job.more.z;
    uint key = (uint(cell.x + 32768) & 2047u) | ((uint(cell.y + 32768) & 2047u) << 11u) |
        ((uint(cell.z + 32768) & 1023u) << 22u);
    uint h = hash_u(key ^ 0x2c1b3c6du) & (entries - 1u);
    Uints rtab = Uints(job.reflection_table);
    int list = -1;
    for (uint probe = 0u; probe < 32u; probe++)
    {
        uint at = ((h + probe) & (entries - 1u)) * 10u;
        uint k = rtab.v[at];
        if (k == 0u)
            break;
        if (k == key + 1u)
        {
            list = int(at);
            break;
        }
    }
    if (list < 0)
        return vec3(0.0);
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    uint levels = job.more.w & 0xffffu, side = job.more.w >> 16;
    float level = clamp(sqrt(roughness) * float(levels - 1u), 0.0, float(levels - 1u));
    for (uint k = 0u; k < 8u; k++)
    {
        uint i = rtab.v[uint(list) + 2u + k];
        if (i == 0xffffffffu)
            break;
        uint at = i * 12u;
        vec3 c = vec3(pr.v[at], pr.v[at + 1u], pr.v[at + 2u]);
        vec3 lo = vec3(pr.v[at + 4u], pr.v[at + 5u], pr.v[at + 6u]);
        vec3 hi = vec3(pr.v[at + 8u], pr.v[at + 9u], pr.v[at + 10u]);
        if (any(lessThan(p, lo - 0.5)) || any(greaterThan(p, hi + 0.5)))
            continue;
        vec3 rr = mix(r, vec3(1.0e-5), lessThan(abs(r), vec3(1.0e-5)));
        vec3 t1 = (hi - p) / rr, t2 = (lo - p) / rr;
        float tw = max(min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z)), 0.0);
        vec3 d = normalize(p + r * tw - c);
        vec3 a = abs(d);
        uint face;
        vec2 uv;
        if (a.x >= a.y && a.x >= a.z)
        {
            face = d.x > 0.0 ? 0u : 1u;
            uv = vec2(d.x > 0.0 ? -d.z : d.z, -d.y) / a.x;
        }
        else if (a.y >= a.z)
        {
            face = d.y > 0.0 ? 2u : 3u;
            uv = vec2(d.x, d.y > 0.0 ? d.z : -d.z) / a.y;
        }
        else
        {
            face = d.z > 0.0 ? 4u : 5u;
            uv = vec2(d.z > 0.0 ? d.x : -d.x, -d.y) / a.z;
        }
        vec3 l2 = vec3(0.0);
        for (int lv = 0; lv < 2; lv++)
        {
            uint li = min(uint(level) + uint(lv), levels - 1u);
            uint s_ = max(side >> li, 1u);
            uint first = floatBitsToUint(pr.v[at + 3u]);
            uint offset = 0u;
            for (uint q = 0u; q < li; q++)
                offset += 6u * max(side >> q, 1u) * max(side >> q, 1u);
            vec2 fx = clamp((uv * 0.5 + 0.5) * float(s_) - 0.5, vec2(0.0), vec2(float(s_) - 1.0));
            ivec2 t0 = ivec2(floor(fx)), t1i = min(t0 + 1, ivec2(int(s_) - 1));
            vec2 ff = fx - vec2(t0);
            uint fb = first + offset + face * s_ * s_;
            Uints rd = Uints(job.reflection_data);
            vec3 value = mix(mix(rgb9e5(rd.v[fb + uint(t0.y) * s_ + uint(t0.x)]), rgb9e5(rd.v[fb + uint(t0.y) * s_ + uint(t1i.x)]), ff.x),
                mix(rgb9e5(rd.v[fb + uint(t1i.y) * s_ + uint(t0.x)]), rgb9e5(rd.v[fb + uint(t1i.y) * s_ + uint(t1i.x)]), ff.x), ff.y);
            l2 += (lv == 0 ? 1.0 - fract(level) : fract(level)) * value;
        }
        float w = 1.0 / (0.25 + dot(p - c, p - c));
        sum += w * l2;
        weights += w;
    }
    if (weights <= 0.0)
        return vec3(0.0);
    found = 1.0;
    return sum / weights;
}

/* ---------- the lights at a surface

The dynamic lights (SCENE_LIGHTS' dynamic class: the flashlight, plasma,
muzzle flashes, grenades, explosions, the vehicles' and weapons' lights):
DYNAMIC_FLOATS each (the place and reach r, the colour by its intensity, a
spot's inner cone's cosine (2 none), its axis and outer cone's cosine (2 a
point light)), job.volume.z of them, then the recent ones (job.volume.w,
the temporal pass's: xgpu_vulkan_pt.c's recent_lights_take). A point
light's inverse square, windowed to none at its reach ((1 - (d/r)^4)^2, as
Karis 2013), its intensity 0.3 r^2 x the colour (the one whose light over
the sphere of radius r is what CE's 1 - d^2/r^2 gives, 2/15 r^3 x 4 pi),
and never more than CE's own 1 - d^2/r^2 at that distance: CE's within
0.6 r, the inverse square's tail past it (CE's reach is a light's whole
sphere of light, not its size: held at 1.9 x CE's within 0.4 r, a wall
1 to 2 units from the flashlight's mouth was a white disc, and it stayed
white through most of CE's 24 frames' fade of the colour; tools/
test_prism_pt.py). A
spot's cone is CE's (rasterizer_set_model_lighting_point_light's): whole
inside the inner cone, to none at the outer, linear in the cosine. Their
shadows traced (light_seen). */
const uint DYNAMIC_FLOATS = 12u;
vec3 dynamic_light_at(uint at, vec3 p, out vec3 l, out float d)
{
    Floats f = Floats(job.lights);
    vec3 to = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]) - p;
    float r = f.v[at + 3u], d2 = dot(to, to);
    d = sqrt(d2);
    l = to / max(d, 1.0e-6);
    if (r <= 0.0 || d2 >= r * r)
        return vec3(0.0);
    float cone = 1.0;
    vec4 axis = vec4(f.v[at + 8u], f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);
    if (axis.w < 1.5)
    {
        float c = dot(-l, axis.xyz), inner = f.v[at + 7u];
        cone = inner < 1.5 && inner > axis.w + 1.0e-4 ? clamp((c - axis.w) / (inner - axis.w), 0.0, 1.0) :
            smoothstep(axis.w, mix(axis.w, 1.0, 0.05), c);
    }
    float x2 = d2 / (r * r);
    float window = 1.0 - x2 * x2;
    return vec3(f.v[at + 4u], f.v[at + 5u], f.v[at + 6u]) * (cone * min(window * window * 0.3 / max(x2, 1.0e-4), 1.0 - x2));
}
vec3 dynamic_light(uint i, vec3 p, out vec3 l, out float d)
{
    return dynamic_light_at(i * DYNAMIC_FLOATS, p, l, d);
}
/* a dynamic light's specular light: a light has a size (a plasma bolt, a
muzzle flash, a grenade's glow are not points), DYNAMIC_SIZE of its reach
and DYNAMIC_SIZE_MOST at most, so its highlight is a sphere light's: the
lobe widened by the size over the distance (Karis 2013's representative
point's roughness, its energy kept by (a / a')^2). As a point, a bolt over a
polished floor drew a highlight as bright as the sun's in a pixel or two.
At most a small one: the flashlight's reach (CE's whole sphere of its light,
12 units) made it a sphere of 1.2 units, its highlight on the wall it
pointed at spread to a seventh of itself, the wall hardly lit */
const float DYNAMIC_SIZE = 0.1;
const float DYNAMIC_SIZE_MOST = 0.08;
vec3 dynamic_specular(Surface g, vec3 l, float d, uint i)
{
    float r = Floats(job.lights).v[i * DYNAMIC_FLOATS + 3u];
    float a = g.roughness * g.roughness;
    float a2 = min(a + 0.5 * min(DYNAMIC_SIZE * r, DYNAMIC_SIZE_MOST) / max(d, 1.0e-3), 1.0);
    Surface w = g;
    w.roughness = sqrt(a2);
    return ggx(w, l) * (a / a2) * (a / a2) * PI;
}

/* ---------- the moving objects (port: the G-buffer pass's object draws of
this frame, every one but the first-person view's, skinned as drawn and
built by xgpu_vulkan_rt.c's entries: job.objects their top level, instances
mask 4, a cutout's 8 (taken by its share of texels drawn); job.object_table
a row of four words an object, its custom index - 1: its triangle soup's
address (3 vec3 a triangle), its albedo (RGBA8, its base map's mean), its
coverage). Every visibility test of BAKE_COMMON's the path tracer makes
(blocked, sky_blocked: tools/vulkan_pt_shaders.py wraps them) takes them;
the sun's ray, a bounce's hit and the reference's paths below too. */
const uint OBJECT_HIT = 0xfffffffeu;
float sun_ray(vec3 o, out float blocker);
const float OBJECT_NEAR = 0.01;
float object_coverage(uint custom)
{
    return custom >= 1u ? uintBitsToFloat(Uints(job.object_table).v[(custom - 1u) * 4u + 3u]) : 1.0;
}
bool objects_blocked(vec3 o, vec3 d, float t_max)
{
    if (job.objects == uvec2(0u) || !(t_max > OBJECT_NEAR) || !ray_ok(o, d, OBJECT_NEAR, t_max))
        return false;
    uint candidates = 0u;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(job.objects), gl_RayFlagsTerminateOnFirstHitEXT, object_cull, o,
        OBJECT_NEAR, d, t_max);
    while (rayQueryProceedEXT(q))
    {
        if (++candidates > CANDIDATES_MOST)
        {
            rayQueryTerminateEXT(q);
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            rnd() < object_coverage(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}
/* the nearest object along the ray within t_max: its distance (negative
none), its face's normal toward the ray and its albedo */
float objects_hit(vec3 o, vec3 d, float t_max, out vec3 nh, out vec3 albedo)
{
    nh = vec3(0.0);
    albedo = vec3(0.0);
    if (job.objects == uvec2(0u) || !(t_max > OBJECT_NEAR) || !ray_ok(o, d, OBJECT_NEAR, t_max))
        return -1.0;
    uint candidates = 0u;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(job.objects), 0u, object_cull, o, OBJECT_NEAR, d, t_max);
    while (rayQueryProceedEXT(q))
    {
        if (++candidates > CANDIDATES_MOST)
        {
            rayQueryTerminateEXT(q);
            break;
        }
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            rnd() < object_coverage(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)))
            rayQueryConfirmIntersectionEXT(q);
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return -1.0;
    uint row = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)) - 1u;
    uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    Uints table = Uints(job.object_table);
    Floats soup = Floats(uvec2(table.v[row * 4u], table.v[row * 4u + 1u]));
    uint at = primitive * 9u;
    vec3 a0 = vec3(soup.v[at], soup.v[at + 1u], soup.v[at + 2u]);
    vec3 a1 = vec3(soup.v[at + 3u], soup.v[at + 4u], soup.v[at + 5u]);
    vec3 a2 = vec3(soup.v[at + 6u], soup.v[at + 7u], soup.v[at + 8u]);
    vec3 ne = cross(a1 - a0, a2 - a0);
    nh = dot(ne, ne) > 1.0e-20 ? normalize(ne) : -d;
    if (dot(nh, d) > 0.0)
        nh = -nh;
    uint packed = table.v[row * 4u + 2u];
    /* (no mean known: 0.45, as the old tracer's grey) */
    albedo = (packed >> 24) != 0u ? unpackUnorm4x8(packed).rgb : vec3(0.45);
    return rayQueryGetIntersectionTEXT(q, true);
}

/* the units rig's floor of light at a pixel (prism-v4-m2's rule, the bake's
texels' and probes': its outdoor value in the structure's clusters with a
sky, sky 0's indoor value in those with none, xgpu_stream.h's
XGPU_SCENE_INDOOR): the flag of the structure's triangle the pixel shows (a
short stretch of the eye's ray about its point), an object's the structure
below it (a ray down past the object itself); nothing found, outdoor */
bool structure_flag_hit(vec3 o, vec3 d, float t_max, out bool inside)
{
    inside = false;
    vec3 nh;
    uint t;
    vec2 b;
    for (int k = 0; k < 2; k++)
    {
        float h = hit(o, d, t_max, nh, t, b);
        if (h < 0.0)
            return false;
        if (t < s.counts.x + s.counts.y)
        {
            inside = indoor(t);
            return true;
        }
        o += d * (h + 0.01);
        t_max -= h + 0.01;
        if (t_max <= 0.0)
            return false;
    }
    return false;
}
vec3 floor_light(Surface g)
{
    vec3 outdoor = ambient(), inside_light = ambient_indoor();
    if (!units_rig() || all(equal(outdoor, inside_light)))
        return outdoor;
    bool inside = false;
    if (g.instance == 0u)
    {
        vec3 d = normalize(g.point - job.camera[0].xyz);
        structure_flag_hit(g.point - d * 0.1, d, 0.15, inside);
    }
    else
        structure_flag_hit(g.point + vec3(0.0, 0.0, 0.01), vec3(0.0, 0.0, -1.0), 64.0, inside);
    return inside ? inside_light : outdoor;
}
/* the floor the bake added at a ray's hit: its texel's triangle's (flag 8 in
the bake's setup), or the volume's probes' (where they stand: the bake's
floor_at) */
vec3 floor_of_triangle(uint t)
{
    return indoor(t) ? ambient_indoor() : ambient();
}
vec3 floor_of_volume(vec3 p)
{
    vec3 outdoor = ambient(), inside_light = ambient_indoor();
    if (!units_rig() || all(equal(outdoor, inside_light)))
        return outdoor;
    return mix(inside_light, outdoor, floor_at(p));
}

/* a light sample's parts at the surface: its diffuse light (the bake's
unit, the irradiance over pi: the albedo multiplies it later) and its
specular light, both unshadowed; a lamp's point given by its direction
and distance and its radiance over the candidate's density over directions
(its inverse pdf folded in by the caller) */
void parts(Surface g, vec3 l, vec3 radiance_cos, bool diffuse_only, out vec3 dif, out vec3 spe)
{
    float c = dot(g.normal, l);
    dif = c > 0.0 ? radiance_cos / PI : vec3(0.0);
    spe = diffuse_only || c <= 0.0 ? vec3(0.0) : ggx(g, l) / max(c, 1.0e-4) * radiance_cos;
}


/* ---------- ReSTIR DI (Bitterli et al. 2020): a reservoir a pixel

A light sample: its kind (0 a lamp's point, 1 a fixture, 2 a dynamic light,
3 none), its index (the lamp's triangle, the fixture's, the light's), its
point. Its unshadowed light at a surface in its own measure (a lamp's by its
area: the cosine at the lamp over the distance squared folded in; a
fixture's and a dynamic light's as they are), the diffuse and specular
parts, their luminance the target. */
const int KIND_LAMP = 0, KIND_FIXTURE = 1, KIND_DYNAMIC = 2, KIND_NONE = 3;
bool sample_light(Surface g, int kind, uint index, vec3 y, out vec3 dif, out vec3 spe, out vec3 l, out float dist)
{
    dif = vec3(0.0);
    spe = vec3(0.0);
    l = vec3(0.0, 0.0, 1.0);
    dist = 0.0;
    vec3 to = y - g.point;
    float d2 = dot(to, to);
    if (d2 < 1.0e-8)
        return false;
    dist = sqrt(d2);
    l = to / dist;
    float c = dot(g.normal, l);
    if (c <= 0.0)
        return false;
    if (kind == KIND_LAMP)
    {
        if (index >= s.counts.z)
            return false;
        Floats f = Floats(s.lamps);
        uint at = index * 16u;
        vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
        vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);
        vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);
        vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x;
        vec3 ne = normalize(cross(a1 - a0, a2 - a0));
        float ce = dot(ne, -l);
        if (ce <= 0.0)
            return false;
        parts(g, l, radiance * c, false, dif, spe);
        spe *= lamp_mis_nee(g, l, 0.5 * length(cross(a1 - a0, a2 - a0)) * ce / d2);
        float geometry = ce / d2;
        dif *= geometry;
        spe *= geometry;
        return true;
    }
    if (kind == KIND_FIXTURE)
    {
        if (index >= uint(s.lamp.z))
            return false;
        vec3 lf;
        float df;
        vec3 e = fixture_light(index, g.point, vec3(0.0), lf, df);
        parts(g, l, e * c, false, dif, spe);
        return dot(e, e) > 0.0;
    }
    if (kind == KIND_DYNAMIC)
    {
        if (index >= uint(job.volume.z) || job.lights == uvec2(0u))
            return false;
        vec3 ld;
        float dd;
        vec3 e = dynamic_light(index, g.point, ld, dd);
        dif = e * c;
        spe = dynamic_specular(g, l, dd, index) * e;
        return dot(e, e) > 0.0;
    }
    return false;
}

/* (below: a lamp from the grid's mixture or the light tree) */
uint lamp_pick_mixed(vec3 p, vec3 n, out float probability);
uint lamp_pick_live(vec3 p, vec3 n, int c, int count, out float probability);

/* the initial reservoir: resampled importance over the candidates (as
lights_ris: each kind its own domain, its candidates' weight over its
count), no shadow ray: the kept sample, its target in its own measure and
the weights' sum (W = sum / target) */
void lights_initial(Surface g, int lamp_count, int fixture_count, int dynamic_count, out int kind, out uint index,
    out vec3 y, out float target_own, out float sum)
{
    kind = KIND_NONE;
    index = 0u;
    y = vec3(0.0);
    target_own = 0.0;
    sum = 0.0;
    vec3 p = g.point;
    bool lamps_on = s.counts.z > 0u && s.lamp.x > 0.0 && s.rig[10].w >= 0.5 && !term_off(2u);
    if (lamps_on)
    {
        Floats f = Floats(s.lamps);
        for (int c = 0; c < lamp_count; c++)
        {
            float probability;
            uint lamp = lamp_pick_live(p, g.normal, c, lamp_count, probability);
            if (probability <= 0.0)
                continue;
            uint at = lamp * 16u;
            vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
            vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);
            vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);
            vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x;
            vec3 ne = normalize(cross(a1 - a0, a2 - a0));
            vec3 l;
            float d, inverse_pdf;
            if (!lamp_direction(p, a0, a1, a2, ne, l, d, inverse_pdf))
                continue;
            float c_ = dot(g.normal, l);
            if (c_ <= 0.0)
                continue;
            vec3 dif, spe;
            parts(g, l, radiance * c_, false, dif, spe);
            spe *= lamp_mis_nee(g, l, 0.5 * length(cross(a1 - a0, a2 - a0)) * max(dot(ne, -l), 0.0) / max(d * d, 1.0e-8));
            float target = luma(dif) + luma(spe);
            float w = target * inverse_pdf / probability / float(lamp_count);
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kind = KIND_LAMP;
                index = lamp;
                y = p + l * d;
                /* (its target by area: the cosine at the lamp over d^2) */
                target_own = target * max(dot(ne, -l), 0.0) / max(d * d, 1.0e-8);
            }
        }
    }
    uint fixtures = uint(s.lamp.z);
    if (fixtures > 0u && !term_off(16u) && fixture_count > 0)
    {
        uint candidates = min(fixtures, uint(fixture_count));
        Floats ff = Floats(s.unused);
        for (uint c = 0u; c < candidates; c++)
        {
            uint i = fixtures <= uint(fixture_count) ? c : min(uint(rnd() * float(fixtures)), fixtures - 1u);
            vec3 l;
            float d;
            vec3 e = fixture_light(i, p, vec3(0.0), l, d);
            if (dot(g.normal, l) <= 0.0 || dot(e, e) <= 0.0)
                continue;
            vec3 dif, spe;
            parts(g, l, e * dot(g.normal, l), false, dif, spe);
            float target = luma(dif) + luma(spe);
            float w = target * (float(fixtures) / float(candidates));
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kind = KIND_FIXTURE;
                index = i;
                y = vec3(ff.v[i * 16u], ff.v[i * 16u + 1u], ff.v[i * 16u + 2u]);
                target_own = target;
            }
        }
    }
    uint dynamics = uint(job.volume.z);
    if (dynamics > 0u && dynamic_count > 0 && job.lights != uvec2(0u))
    {
        uint candidates = min(dynamics, uint(dynamic_count));
        Floats fl = Floats(job.lights);
        for (uint c = 0u; c < candidates; c++)
        {
            uint i = dynamics <= uint(dynamic_count) ? c : min(uint(rnd() * float(dynamics)), dynamics - 1u);
            vec3 l;
            float d;
            vec3 e = dynamic_light(i, p, l, d);
            float nl = dot(g.normal, l);
            if (nl <= 0.0 || dot(e, e) <= 0.0)
                continue;
            float target = luma(e * nl) + luma(dynamic_specular(g, l, d, i) * e);
            float w = target * (float(dynamics) / float(candidates));
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kind = KIND_DYNAMIC;
                index = i;
                y = vec3(fl.v[i * 12u], fl.v[i * 12u + 1u], fl.v[i * 12u + 2u]);
                target_own = target;
            }
        }
    }
}

/* the pixel's lights by ReSTIR DI: its initial reservoir, then the last
frame's at its surface's place there (the motion's pixel, its guide this
surface's: no reuse across an edge or an opening), its sample's target
evaluated again here, its confidence capped at denoise_m frames (a dynamic
light's sample is not reused: their order changes from frame to frame), one
kept by its weight. The two are combined by a target that holds the
sample's shadow (its light at this pixel times its visibility from here: a
shadow ray for the initial sample and one for the last frame's), the
generalized RIS of Lin et al. 2022 with both targets this pixel's, so the
balance heuristic is the confidences' share and the light stays unbiased;
the kept sample's shadow is known, its light given with no further ray, and
the reservoir kept for the next frame with the weight of that target.
Round 2's reservoirs held the unshadowed target alone: in a room of many
lamps most of a pixel's picks were hidden, so its reservoir held a hidden
lamp most frames and a seen one in a few, each frame's light 0 or several
times its mean (G4's and G5's speckled walls, which the firefly hold then
darkened). With the shadow in the target the reservoir keeps to the lamps
this pixel sees. (A hidden sample's weight 0 with its confidence kept is
not the same thing: there the weight is set after a choice made by the
unshadowed target, which takes a ninth of a sparse light (round 1's G5);
here the choice itself is by the shadowed target, and an empty reservoir is
a valid estimate of 0 at its confidence) */
/* (a dynamic light's shadow by the level to its place, by the moving
objects to DYNAMIC_OWNER short of it: a light is often on or in the object
it belongs to (a vehicle's lamp, a weapon's glow, an elite's), which does
not shadow it) */
const float DYNAMIC_OWNER = 0.15;
bool light_seen(vec3 o, int kind, vec3 l, float d)
{
    if (kind == KIND_DYNAMIC)
        return !blocked_structure(o, l, max(d - 0.05, 0.0)) && !objects_blocked(o, l, d - DYNAMIC_OWNER);
    return kind == KIND_LAMP ? lamp_visible(o, l, d) : !blocked(o, l, max(d - 0.05, 0.0));
}
/* one of the last frame's reservoirs (at pixel q) offered to this pixel's:
taken only where q held this surface (the same instance, the normal, the
plane, an object's distance), its sample's target evaluated here and its
shadow traced where it would weigh, its confidence capped at denoise_m
frames; kept by its weight (the generalized RIS's step, both targets this
pixel's) */
/* (restir_reuse's last call: whether the reservoir's sample lit this pixel,
in front of it and seen) */
bool reuse_valid = false;
/* (whether the last frame's pixel q held this surface: the same instance,
the normal, the plane (the level), an object's distance) */
bool restir_same(ivec2 q, Surface g)
{
    if (!inside(q))
        return false;
    uvec4 gl = imageLoad(guide_last, q);
    vec3 nl = unfold(unpackSnorm2x16(gl.y));
    float dl = uintBitsToFloat(gl.z);
    bool same = gl.x == (g.instance | 0x10000u) && dot(nl, g.normal) >= 0.9;
    if (same && g.instance == 0u)
        same = abs(dot(last_point(q, dl) - g.point, g.normal)) <= 0.02 + 0.004 * g.distance;
    else if (same)
        same = abs(dl - g.distance) <= 0.05 * g.distance + 0.05;
    return same;
}
void restir_reuse(ivec2 q, Surface g, vec3 o, inout int kind, inout uint index, inout vec3 y, inout float target_kept,
    inout vec3 kept_d, inout vec3 kept_s, inout float w_total, inout float m_total)
{
    reuse_valid = false;
    if (!restir_same(q, g))
        return;
    vec4 rp = imageLoad(reservoir_point_last, q);
    uvec4 ri = imageLoad(reservoir_info_last, q);
    int k_last = int(ri.x & 3u);
    uint i_last = ri.x >> 2;
    float m_last = min(uintBitsToFloat(ri.y), job.extra.z);
    vec3 dif, spe, l;
    float d;
    if (k_last != KIND_DYNAMIC && k_last != KIND_NONE && rp.w > 0.0 && m_last > 0.0 &&
        sample_light(g, k_last, i_last, rp.xyz, dif, spe, l, d))
    {
        float target = luma(dif) + luma(spe);
        float w_last = target * rp.w * m_last;
        /* (its shadow traced only where it would weigh) */
        if (w_last > 0.0 && !isinf(w_last) && light_seen(o, k_last, l, d))
        {
            reuse_valid = true;
            w_total += w_last;
            if (rnd() * w_total <= w_last)
            {
                kind = k_last;
                index = i_last;
                y = rp.xyz;
                target_kept = target;
                kept_d = dif;
                kept_s = spe;
            }
        }
    }
    m_total += max(m_last, 0.0);
}
/* the reservoir kept for the next frame (its sample, W, its confidence),
after the boiling filter (RTXDI's): a reservoir whose W is far over the
mean of the others' in its subgroup (32 pixels about it) is let go: a
runaway reservoir, a reused sample whose target here fell far under its
weight. (W alone: a lucky sample of a bright light has a large target too,
so its W is not an outlier; comparing the estimate, W by its target, takes
such samples away and with them the light they find: tools/test_prism_pt.py)
Off by default (HALO_PT_RESTIR_BOIL): at G4 it let go of nothing */
void restir_store(ivec2 pixel, int t_kind, uint t_index, vec3 t_y, float t_target, float t_w, float t_m)
{
    float t_big_w = t_kind != KIND_NONE && t_target > 0.0 ? t_w / (t_target * t_m) : 0.0;
    uint boil = (job.reuse_flags >> 8) & 255u;
    if (SPEC_BOIL && boil != 0u)
    {
        float strength = clamp(float(boil) / 100.0, 1.0e-3, 1.0);
        float most = 10.0 / strength - 9.0;
        float wv = t_big_w > 0.0 && !isinf(t_big_w) ? t_big_w : 0.0;
        float sum_w = subgroupAdd(wv);
        float count = subgroupAdd(wv > 0.0 ? 1.0 : 0.0);
        /* (against the others' mean: in a subgroup of 32 a reservoir is at
        most 32 times a mean that holds it, under the filter's 41 at its
        usual strength 0.2, which RTXDI takes over a group of 256) */
        if (wv > 0.0 && count > 1.5 && wv > most * (sum_w - wv) / (count - 1.0))
        {
            t_kind = KIND_NONE;
            t_big_w = 0.0;
            t_m = 0.0;
        }
    }
    imageStore(reservoir_point_now, pixel, vec4(t_y, t_big_w));
    imageStore(reservoir_info_now, pixel, uvec4(uint(t_kind) | (t_index << 2), floatBitsToUint(t_m), 0u, 0u));
}
void lights_restir(ivec2 pixel, Surface g, vec3 o, bool reuse, out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    int kind;
    uint index;
    vec3 y;
    float target_own, sum;
    lights_initial(g, int(job.extra.x), 8, 0, kind, index, y, target_own, sum);
    /* (the initial sample's shadow: hidden, its weight in the shadowed
    target 0; seen, its light kept for the shading) */
    vec3 kept_d = vec3(0.0), kept_s = vec3(0.0);
    float w_cur = 0.0;
    float target_kept = 0.0;
    if (kind != KIND_NONE && target_own > 0.0)
    {
        vec3 dif, spe, l;
        float d;
        if (sample_light(g, kind, index, y, dif, spe, l, d) && light_seen(o, kind, l, d))
        {
            w_cur = sum;
            target_kept = target_own;
            kept_d = dif;
            kept_s = spe;
        }
    }
    if (w_cur <= 0.0)
        kind = KIND_NONE;
    float m_total = 1.0;
    /* (the kept sample's weight sum, its target here) */
    float w_total = w_cur;
    /* (whether the reservoir for the next frame is stored: the temporal
    one, before the spatial neighbours, unless REUSE_FEEDBACK) */
    bool stored = false;
    if (reuse && g.has_history && (job.frame.y & FLAG_HISTORY) != 0u)
    {
        /* (the last frame's pixel one of the 2 x 2 about the place, chosen
        by its bilinear weight, not the nearest: where the eye moves toward a
        surface several pixels map to one of the last frame's, and the
        nearest gave them all its reservoir, so a lucky sample was copied
        into blocks that grew along the move (round 2's and 3's G4 floor
        streaking toward the point the eye moved to)) */
        vec2 at_last = last_pixel(g.point, vec2(pixel), g.motion, g.instance == 0u);
        vec2 fl = floor(at_last), fr = at_last - fl;
        ivec2 q = ivec2(fl) + ivec2(rnd() < fr.x ? 1 : 0, rnd() < fr.y ? 1 : 0);
        /* (permutation sampling (RTXDI's, after Wyman and Panteleev 2021):
        the last frame's reservoir taken from a pixel of the 4 x 4 about the
        place by a permutation the same over the frame and new each frame
        (an offset, then each coordinate's low two bits flipped), so each of
        the last frame's reservoirs goes to one pixel still, but a
        reservoir's sample does not stay at one pixel frame after frame:
        its run of frames is spread over the block and the histories take
        it as noise to average, not a fixed spot; where that pixel is not
        this surface's, the place's own) */
        if ((job.reuse_flags & REUSE_PERMUTE) != 0u)
        {
            uint r = hash_u(job.frame.x * 0x9e3779b9u + 0x632be5abu);
            ivec2 off = ivec2(int(r & 3u), int((r >> 2) & 3u));
            ivec2 qp = ((q + off) ^ ivec2(3)) - off;
            if (restir_same(qp, g))
                q = qp;
        }
        restir_reuse(q, g, o, kind, index, y, target_kept, kept_d, kept_s, w_total, m_total);
        /* (the temporal reservoir, kept for the next frame unless
        REUSE_FEEDBACK: the spatial neighbours' samples are this frame's
        light alone, never passed on, so no sample walks from pixel to
        pixel over the frames) */
        if ((job.reuse_flags & REUSE_FEEDBACK) == 0u)
        {
            restir_store(pixel, kind, index, y, target_kept, w_total, m_total);
            stored = true;
        }
        /* (the spatiotemporal reuse, job.settings.z of them: the last
        frame's reservoirs at points of a disc of 2 to 16 pixels about the
        place, each this surface's by the same test, taken alike: a lamp
        that one pixel's candidates found is offered to its neighbours, so a
        light few candidates reach (a semi-glossy wall's highlight of a lamp
        the grid's lists rank low) is not one pixel's luck) */
        /* (job.settings.z: the neighbours, 16 more where their confidence
        is counted by the kept sample's support traced from them) */
        bool traced_z = job.settings.z >= 16.0 && !part_off(PARTS_Z);
        int spatial = part_off(PARTS_SPATIAL) ? 0 : min(int(mod(job.settings.z, 16.0)), min(SPEC_SPATIAL, 4));
        /* (the disc's radius: reuse_flags' bits 16 to 23, 16 pixels where 0) */
        uint disc_bits = (job.reuse_flags >> 16) & 255u;
        float disc = disc_bits >= 3u ? float(disc_bits) : 16.0;
        vec3 n_points[4], n_normals[4];
        float n_m[4];
        bool n_kept[4];
        for (int k = 0; k < spatial; k++)
        {
            float a = 6.2831853 * rnd(), r = 2.0 + (disc - 2.0) * sqrt(rnd());
            ivec2 qs = ivec2(floor(at_last + r * vec2(cos(a), sin(a)) + 0.5));
            /* (a neighbour's confidence counted below, only where the kept
            sample could have been its: its m_total apart) */
            float m_k = 0.0, w_before = w_total;
            vec3 y_before = y;
            restir_reuse(qs, g, o, kind, index, y, target_kept, kept_d, kept_s, w_total, m_k);
            n_m[k] = m_k;
            /* (whether this neighbour's sample is the one kept so far: its
            support there known, no ray) */
            n_kept[k] = w_total > w_before && y != y_before;
            for (int j = 0; j < k; j++)
                n_kept[j] = n_kept[j] && !n_kept[k];
            /* (untraced: a neighbour counts where its own sample lit this
            pixel, the shadow restir_reuse traced; a slight brightening where
            the kept sample lights a neighbour whose own did not light here) */
            if (!traced_z)
            {
                if (reuse_valid)
                    m_total += m_k;
                n_m[k] = 0.0;
            }
            n_points[k] = vec3(0.0);
            n_normals[k] = vec3(0.0);
            if (m_k > 0.0)
            {
                uvec4 gq = imageLoad(guide_last, qs);
                n_points[k] = last_point(qs, uintBitsToFloat(gq.z));
                n_normals[k] = unfold(unpackSnorm2x16(gq.y));
            }
        }
        /* (the spatial neighbours' confidences by the kept sample's support
        there (Bitterli et al. 2020's Z, the unbiased normalisation): a
        neighbour counts only where the kept sample lights its point (in
        front of it, seen from it), else 1/M took the light of every pixel
        whose neighbours see lamps it does not: round 4's spatial reuse,
        G4's floor in motion a third darker than R) */
        for (int k = 0; k < spatial; k++)
        {
            if (n_m[k] <= 0.0 || kind == KIND_NONE)
                continue;
            if (n_kept[k])
            {
                m_total += n_m[k];
                continue;
            }
            Surface gk = g;
            gk.point = n_points[k];
            gk.normal = n_normals[k];
            vec3 dif, spe, l;
            float d;
            if (sample_light(gk, kind, index, y, dif, spe, l, d) && luma(dif) + luma(spe) > 0.0 &&
                light_seen(gk.point + gk.normal * (0.01 + 0.0005 * g.distance), kind, l, d))
                m_total += n_m[k];
        }
    }
    /* (W: the weights' sum over the kept target, by the confidences' sum:
    the initial reservoir's weights are already its candidates' mean) */
    float big_w = kind != KIND_NONE && target_kept > 0.0 ? w_total / (target_kept * m_total) : 0.0;
    if (big_w > 0.0)
    {
        diffuse = kept_d * big_w;
        specular = kept_s * big_w;
    }
    if (!stored)
        restir_store(pixel, kind, index, y, target_kept, w_total, m_total);
}

/* the dynamic lights at the pixel, apart from ReSTIR DI's reservoir:
resampled importance over every one of them (DYNAMIC_ALL at most: a
light's test is its reach's, most are out of reach) by their unshadowed
diffuse and specular light, the kept one's shadow traced. Every one, not
4 at random: in a fight (thirty plasma bolts, muzzle flashes and glows
across the room) 4 at random missed the one light near the pixel most
frames, a bolt's pool on the floor a sparkle.
Apart, since the last frame's reservoirs never hold one (their order
changes from frame to frame) and the reuse's confidences (the canonical
reservoir's 1 of up to 9) took the rest of a dynamic light's weight: a
flashlight came in over many frames at a ninth of itself */
const uint DYNAMIC_ALL = 64u;
void dynamic_direct(Surface g, vec3 o, out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    uint n = uint(job.volume.z);
    if (n == 0u || job.lights == uvec2(0u))
        return;
    uint candidates = min(n, DYNAMIC_ALL);
    float sum = 0.0, target_kept = 0.0, d_kept = 0.0;
    vec3 kept_d = vec3(0.0), kept_s = vec3(0.0), l_kept = vec3(0.0);
    for (uint c = 0u; c < candidates; c++)
    {
        uint i = n <= DYNAMIC_ALL ? c : min(uint(rnd() * float(n)), n - 1u);
        vec3 l;
        float d;
        vec3 e = dynamic_light(i, g.point, l, d);
        float nl = dot(g.normal, l);
        if (nl <= 0.0 || dot(e, e) <= 0.0)
            continue;
        vec3 dif = e * nl, spe = dynamic_specular(g, l, d, i) * e;
        float target = luma(dif) + luma(spe);
        float w = target * (float(n) / float(candidates));
        if (!(w > 0.0) || isinf(w))
            continue;
        sum += w;
        if (rnd() * sum <= w)
        {
            target_kept = target;
            kept_d = dif;
            kept_s = spe;
            l_kept = l;
            d_kept = d;
        }
    }
    if (target_kept <= 0.0 || !light_seen(o, KIND_DYNAMIC, l_kept, d_kept))
        return;
    diffuse = kept_d * (sum / target_kept);
    specular = kept_s * (sum / target_kept);
}

/* ---------- the lamps by a light tree as well as the grid (the live
frames' candidates and the reference's)

job.tree: a binary tree of the bake's lamps, built on the host
(xgpu_vulkan_pt.c's tree_node), job.extra.w its nodes; each node 16 floats:
its box's corners (6), its power (its lamps' luminance by area), its two
children (float bits; 0 a leaf), its parent, a leaf's lamp, the leaf's
triangle's normal and plane (the normal by a corner); after the nodes, each
lamp's leaf. A node's importance at a point p facing n (after Conty Estevez
and Kulla 2018's for a cluster of lights, without its emitters' cone): its
power by the receiver's cosine bound (the best cosine to n over the cone
from p to the node's bounding sphere) over the distance squared to its
middle, never nearer than the sphere's radius; a leaf's nought where p is
clearly behind its triangle. A lamp whose light can reach p (in front of
it, above p's surface) has every ancestor's importance above nought, so its
probability is too. A lamp is taken from the grid's mixture (lamp_pick) or
from the tree, half and half, its probability the two's mean: one sample of
a mixture, unbiased, and near the better of the two where the other misses a
lamp. The grid's lists, made from a cell's few points, can miss a lamp near
a surface of the cell and leave it to the level's power (one pick in
thousands): round 1's references in a10's corridors were a dim wall under
dense fireflies, and its moving frames there the noisiest (G5's flicker). The
grid's lists know which lamps a cell's points see; the tree knows which are
near: ReSTIR DI's initial candidates and the reference's both take the
mixture (HALO_PT_LAMP_TREE=0: the grid alone in the live frames). */
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Tree
{
    float v[];
};
bool tree_on() { return job.tree != uvec2(0u) && job.extra.w >= 1.0; }
float tree_importance(uint node, vec3 p, vec3 n)
{
    Tree t = Tree(job.tree);
    uint b = node * 16u;
    vec3 lo = vec3(t.v[b], t.v[b + 1u], t.v[b + 2u]), hi = vec3(t.v[b + 3u], t.v[b + 4u], t.v[b + 5u]);
    if (floatBitsToUint(t.v[b + 7u]) == 0u &&
        dot(vec3(t.v[b + 11u], t.v[b + 12u], t.v[b + 13u]), p) - t.v[b + 14u] < -1.0e-4)
        return 0.0;
    vec3 d = 0.5 * (lo + hi) - p;
    float r2 = 0.25 * dot(hi - lo, hi - lo), dist2 = dot(d, d), bound = 1.0;
    if (dist2 > r2)
    {
        float cos_u = sqrt(max(1.0 - r2 / dist2, 0.0)), sin_u = sqrt(min(r2 / dist2, 1.0));
        float cos_i = dot(n, d) * inversesqrt(dist2), sin_i = sqrt(max(1.0 - cos_i * cos_i, 0.0));
        bound = cos_i >= cos_u ? 1.0 : max(cos_i * cos_u + sin_i * sin_u, 0.0);
        /* (the emitters' side, HALO_PT_TREE_CONES=1 (job.volume.w past 16): the best
        cosine of a lamp of the node toward p, its normals' cone (axis,
        half angle) widened by the sphere's angle from p; one-sided
        Lambertian lamps, nought past a right angle: Conty Estevez and Kulla
        2018's theta prime) */
        float cos_o = t.v[b + 15u];
        if (SPEC_CONES && cos_o > -0.99 && job.volume.w >= 16.0)
        {
            /* (cos(theta - theta_o - theta_u) by the angles' sums, no
            inverse trigonometry: theta past the two, else 1) */
            vec3 axis = vec3(t.v[b + 11u], t.v[b + 12u], t.v[b + 13u]);
            float cos_t = clamp(dot(axis, -d) * inversesqrt(dist2), -1.0, 1.0), sin_t = sqrt(1.0 - cos_t * cos_t);
            float sin_o = sqrt(max(1.0 - cos_o * cos_o, 0.0));
            /* (theta - theta_o, where theta is past theta_o) */
            if (cos_t < cos_o)
            {
                float cos_a = cos_t * cos_o + sin_t * sin_o, sin_a = sin_t * cos_o - cos_t * sin_o;
                /* (less theta_u, where past it) */
                if (cos_a < cos_u)
                {
                    float cos_b = cos_a * cos_u + sin_a * sin_u;
                    float sin_b = sin_a * cos_u - cos_a * sin_u;
                    bound *= sin_b > 0.0 && cos_b <= 0.0 ? 0.0 : max(cos_b, 0.0);
                }
            }
        }
    }
    return t.v[b + 6u] * bound / max(dist2, max(r2, 1.0e-8));
}
/* a node's chance of its left child (by their importances; by their powers
where both are nought: none of their lamps reaches p, any choice is right) */
float tree_left(uint left, uint right, vec3 p, vec3 n)
{
    Tree t = Tree(job.tree);
    float il = tree_importance(left, p, n), ir = tree_importance(right, p, n);
    if (il + ir > 0.0)
        return il / (il + ir);
    return t.v[left * 16u + 6u] / max(t.v[left * 16u + 6u] + t.v[right * 16u + 6u], 1.0e-30);
}
uint tree_pick(vec3 p, vec3 n, out float probability)
{
    Tree t = Tree(job.tree);
    uint node = 0u;
    probability = 1.0;
    for (int depth = 0; depth < 48; depth++)
    {
        uint b = node * 16u, left = floatBitsToUint(t.v[b + 7u]);
        if (left == 0u)
            return floatBitsToUint(t.v[b + 10u]);
        uint right = floatBitsToUint(t.v[b + 8u]);
        float pl = tree_left(left, right, p, n);
        if (rnd() < pl)
        {
            node = left;
            probability *= pl;
        }
        else
        {
            node = right;
            probability *= 1.0 - pl;
        }
    }
    probability = 0.0;
    return 0u;
}
float tree_probability(uint lamp, vec3 p, vec3 n)
{
    Tree t = Tree(job.tree);
    uint node = floatBitsToUint(t.v[uint(job.extra.w) * 16u + lamp]);
    float probability = 1.0;
    for (int depth = 0; depth < 48 && node != 0u; depth++)
    {
        uint parent = floatBitsToUint(t.v[node * 16u + 9u]);
        uint left = floatBitsToUint(t.v[parent * 16u + 7u]), right = floatBitsToUint(t.v[parent * 16u + 8u]);
        float pl = tree_left(left, right, p, n);
        probability *= node == left ? pl : 1.0 - pl;
        node = parent;
    }
    return probability;
}
/* lamp_pick's probability of a given lamp at p (its mixture: the cell's seen
list by GRID_SEEN, its near by GRID_NEAR, the rest by power; the same
arithmetic, tools/test_prism_pt.py checks it against lamp_pick's text) */
float grid_probability(vec3 p, uint l)
{
    int cell = grid_cell(p);
    Uints g = Uints(s.grid);
    uint seen_at = uint(max(cell, 0)), seen = cell >= 0 ? min(g.v[seen_at], 64u) : 0u;
    uint near_at = seen_at + 1u + 2u * seen, near = cell >= 0 ? min(g.v[near_at], 64u) : 0u;
    float share_seen = seen > 0u && !term_off(32u) ? GRID_SEEN : 0.0;
    float share_near = near > 0u && !term_off(32u) ? (seen > 0u ? GRID_NEAR : GRID_SEEN + GRID_NEAR) : 0.0;
    float probability = (1.0 - share_seen - share_near) * lamp_power(l) / max(s.lamp.x, 1.0e-20);
    if (seen > 0u)
        probability += share_seen * list_probability(seen_at, seen, l);
    if (near > 0u)
        probability += share_near * list_probability(near_at, near, l);
    return probability;
}
/* the live frames' candidate c of count: the last from the light tree, the
others from the grid, each weighed by its own source's probability (its
candidate's weight target / (count x probability)). Both sources reach every
lamp of power (the grid by its power share, the tree by its importance), so
the sum stays unbiased (each candidate's term an unbiased estimate of the
lamps' light over count); the tree's one candidate finds the lamps the
grid's lists miss near a cell's surfaces (round 1's a10 fireflies) at one
tree descent a pixel. The mixture for every candidate (lamp_pick_mixed, the
reference's) cost two descents a candidate: 12 ms of the RTX 3070 Ti's
light pass at 1080p, G2 and G5 alike. */
uint lamp_pick_live(vec3 p, vec3 n, int c, int count, out float probability)
{
    if (!tree_on() || c < count - int(max(mod(job.volume.w, 16.0), 1.0)))
        return lamp_pick(p, probability);
    return tree_pick(p, n, probability);
}
uint lamp_pick_mixed(vec3 p, vec3 n, out float probability)
{
    if (!tree_on())
        return lamp_pick(p, probability);
    uint l;
    float from_grid, from_tree;
    if (rnd() < 0.5)
    {
        l = lamp_pick(p, from_grid);
        from_tree = tree_probability(l, p, n);
    }
    else
    {
        l = tree_pick(p, n, from_tree);
        from_grid = grid_probability(p, l);
    }
    probability = 0.5 * from_grid + 0.5 * from_tree;
    return l;
}

/* the reference's lights at the pixel: resampled importance as lights_ris,
but each candidate's shadow traced and its visibility in its target, so the
kept sample is a seen one and a pixel that sees a lamp through a gap is not
lit by one sample in many: unbiased (the target is the integrand's own where
the light is seen), its noise the candidates' alone */
void lights_ris_seen(Surface g, vec3 o, int lamp_count, int fixture_count, int dynamic_count, bool lamps_specular,
    out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    vec3 kept_d = vec3(0.0), kept_s = vec3(0.0);
    float kept_t = 0.0, sum = 0.0;
    vec3 p = g.point;
    bool lamps_on = s.counts.z > 0u && s.lamp.x > 0.0 && s.rig[10].w >= 0.5 && !term_off(2u);
    if (lamps_on)
    {
        Floats f = Floats(s.lamps);
        for (int c = 0; c < lamp_count; c++)
        {
            float probability;
            uint lamp = lamp_pick_mixed(p, g.normal, probability);
            if (probability <= 0.0)
                continue;
            uint at = lamp * 16u;
            vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
            vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);
            vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);
            vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x;
            vec3 ne = normalize(cross(a1 - a0, a2 - a0));
            vec3 l;
            float d, inverse_pdf;
            if (!lamp_direction(p, a0, a1, a2, ne, l, d, inverse_pdf))
                continue;
            if (dot(g.normal, l) <= 0.0 || !lamp_visible(o, l, d))
                continue;
            vec3 dif, spe;
            parts(g, l, radiance * dot(g.normal, l), !lamps_specular, dif, spe);
            float target = luma(dif) + luma(spe);
            float w = target * inverse_pdf / probability / float(lamp_count);
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kept_d = dif;
                kept_s = spe;
                kept_t = target;
            }
        }
    }
    uint fixtures = uint(s.lamp.z);
    if (fixtures > 0u && !term_off(16u) && fixture_count > 0)
    {
        uint candidates = min(fixtures, uint(fixture_count));
        for (uint c = 0u; c < candidates; c++)
        {
            uint i = fixtures <= uint(fixture_count) ? c : min(uint(rnd() * float(fixtures)), fixtures - 1u);
            vec3 l;
            float d;
            vec3 e = fixture_light(i, p, vec3(0.0), l, d);
            if (dot(g.normal, l) <= 0.0 || dot(e, e) <= 0.0 || blocked(o, l, max(d - 0.05, 0.0)))
                continue;
            vec3 dif, spe;
            parts(g, l, e * dot(g.normal, l), false, dif, spe);
            float target = luma(dif) + luma(spe);
            float w = target * (float(fixtures) / float(candidates));
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kept_d = dif;
                kept_s = spe;
                kept_t = target;
            }
        }
    }
    uint dynamics = uint(job.volume.z);
    if (dynamics > 0u && dynamic_count > 0 && job.lights != uvec2(0u))
    {
        uint candidates = min(dynamics, uint(dynamic_count));
        for (uint c = 0u; c < candidates; c++)
        {
            uint i = dynamics <= uint(dynamic_count) ? c : min(uint(rnd() * float(dynamics)), dynamics - 1u);
            vec3 l;
            float d;
            vec3 e = dynamic_light(i, p, l, d);
            float nl = dot(g.normal, l);
            if (nl <= 0.0 || dot(e, e) <= 0.0 || !light_seen(o, KIND_DYNAMIC, l, d))
                continue;
            vec3 dif = e * nl;
            vec3 spe = dynamic_specular(g, l, d, i) * e;
            float target = luma(dif) + luma(spe);
            float w = target * (float(dynamics) / float(candidates));
            if (!(w > 0.0) || isinf(w))
                continue;
            sum += w;
            if (rnd() * sum <= w)
            {
                kept_d = dif;
                kept_s = spe;
                kept_t = target;
            }
        }
    }
    if (kept_t <= 0.0 || sum <= 0.0)
        return;
    diffuse = kept_d / kept_t * sum;
    specular = kept_s / kept_t * sum;
}

/* a point's sun: its visibility from o (a direction of the disc), its
diffuse light (the rig's sun by the cosine) and specular light */
void sun_parts(Surface g, out vec3 dif, out vec3 spe)
{
    float c = dot(g.normal, s.sun.xyz);
    dif = vec3(0.0);
    spe = vec3(0.0);
    if (s.sun.w <= 0.0 || c <= 0.0)
        return;
    dif = s.rig[0].rgb * c;
    spe = ggx(g, s.sun.xyz) * s.rig[0].rgb * PI;
}

/* the light a ray's hit gives back toward the ray (the bake's unit): its
glow and its albedo by the bake's light there (its texel's light less the
sun's and less the ambient floor, which the estimator does not pass on; else
the irradiance volume's) and the sun's by the mask (else the volume's
visibility); a miss the sky's, the domes with it where the point did not
take them as lights (domes: a reflection's ray, not a diffuse bounce's) */
vec3 hit_light_n(vec3 o, vec3 d, float t_max, bool domes, out float hit_distance, out vec3 nh)
{
    uint t;
    vec2 b;
    hit_distance = -1.0;
    hit_light_triangle = 0xffffffffu;
    float th = hit(o, d, t_max, nh, t, b);
    /* (a moving object nearer: lit by the irradiance volume and the sun's
    ray from its point) */
    {
        vec3 no, ao;
        float ho = objects_hit(o, d, th < 0.0 ? t_max : th, no, ao);
        if (ho >= 0.0)
        {
            hit_distance = ho;
            nh = no;
            vec3 h = o + d * ho, p = h + no * 0.05;
            vec4 v;
            vec3 total = volume_at(p, no, v) ? max(v.rgb - floor_of_volume(p), vec3(0.0)) : vec3(0.0);
            float c = dot(no, s.sun.xyz), blocker;
            vec3 sunlight = s.sun.w > 0.0 && c > 0.0 ? s.rig[0].rgb * c * sun_ray(h + no * 0.02, blocker) : vec3(0.0);
            return ao * (total + sunlight);
        }
    }
    if (th < 0.0)
        return sky_along(d, domes);
    hit_distance = th;
    hit_light_triangle = t;
    if (dot(nh, nh) <= 0.0)
        return vec3(0.0);
    vec3 h = o + d * th;
    vec3 light = glow_of(t);
    vec3 albedo = albedo_of(t);
    vec3 total = vec3(-1.0), indirect_light;
    float sun_v = -1.0;
    /* (the level's triangles: their texel, by their lightmap page and
    coordinates; the info's word 7 the triangle's kept index) */
    uint kept = Uints(s.info).v[t * 8u + 7u];
    if (t < s.counts.x + s.counts.y && kept != 0xffffffffu && job.lightmaps != uvec2(0u))
    {
        Uints lm = Uints(job.lightmaps);
        uint page = lm.v[kept * 4u];
        if (page > 0u)
        {
            vec2 uv0 = unpackUnorm2x16(lm.v[kept * 4u + 1u]);
            vec2 uv1 = unpackUnorm2x16(lm.v[kept * 4u + 2u]);
            vec2 uv2 = unpackUnorm2x16(lm.v[kept * 4u + 3u]);
            vec2 uv = uv0 * (1.0 - b.x - b.y) + uv1 * b.x + uv2 * b.y;
            vec3 tot;
            float sv;
            if (bake_at(page, uv, nh, tot, indirect_light, sv))
            {
                total = max(tot - floor_of_triangle(t), vec3(0.0));
                sun_v = sv;
            }
        }
    }
    if (total.x < 0.0)
    {
        vec4 v;
        if (volume_at(h + nh * 0.05, nh, v))
        {
            total = max(v.rgb - floor_of_volume(h + nh * 0.05), vec3(0.0));
            if (sun_v < 0.0)
                sun_v = v.w;
        }
        else
            total = vec3(0.0);
    }
    float c = dot(nh, s.sun.xyz);
    vec3 sunlight = s.sun.w > 0.0 && c > 0.0 ? s.rig[0].rgb * c * max(sun_v, 0.0) : vec3(0.0);
    return light + albedo * (total + sunlight);
}
vec3 hit_light(vec3 o, vec3 d, float t_max, bool domes, out float hit_distance)
{
    vec3 nh;
    return hit_light_n(o, d, t_max, domes, hit_distance, nh);
}


/* ---------- the radiance cache: a world-space hash grid of the diffuse
bounce's light (the idea of a spatially hashed radiance cache, as Binder et
al.'s "Massively Parallel Path Space Filtering" (2019) and NVIDIA's SHaRC
describe it; this implementation is our own)

A cell is a cube of the world, its side the cache's cell at a unit's distance
(job.cache_settings.y) by the power of two at or under the hit's distance
from the eye (a far hit's cell is larger, as a pixel's footprint there is),
and one of six normal directions (the hit normal's largest axis and its
sign). Its key (a hash of the cube, its level and its direction; never 0)
finds it among CACHE_CELLS by open addressing (8 places). A cell's words: its
key, this frame's sums (fixed point, by CACHE_FIXED) and count, the frame it
was last updated, its mean light (floats) and the samples it holds.

Each frame the light pass adds every fourth pixel's diffuse bounce as
traced (the hit's light: the bake's there, the moving objects' by the
volume and the sun's ray: the estimator's own value, so the cache starts
from the bake and follows what the live hits see) into the hit's cell, and
the bounce reads the cell's mean in place of its own hit's light where the
cell holds job.cache_settings.z samples. The cache pass then folds the
frame's sums into each cell's mean (a running mean over at most
job.cache_settings.w samples, so it follows a change in about that many),
and lets a cell no frame has updated for CACHE_IDLE frames go. What it
does to the light: a bounce meeting a small bright patch (a lamp's glow, the
pool under it) takes the cell's mean over its cube instead of the patch's
value: the same light spread over the cube, so the expected light of the
pixels about it holds where the cube is small beside the distance; the
sharp detail of the bounce's light at its hit (never the pixel's own) is
lost. The reference never reads it. */
const uint CACHE_WORDS = 12u;
const uint CACHE_CELLS = 1u << 19;
const float CACHE_FIXED = 256.0;
const uint CACHE_IDLE = 600u;
layout(buffer_reference, std430, buffer_reference_align = 16) coherent buffer RadianceCache
{
    uint v[];
};
uint cache_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
/* the cell's key and its first place */
uint cache_key(vec3 p, vec3 n, float distance_, out uint place)
{
    int level = clamp(int(floor(log2(max(distance_, 1.0)))), 0, 15);
    float side = job.cache_settings.y * exp2(float(level));
    ivec3 q = ivec3(floor(p / side));
    vec3 a = abs(n);
    uint direction = a.x >= a.y && a.x >= a.z ? (n.x >= 0.0 ? 0u : 1u) :
        a.y >= a.z ? (n.y >= 0.0 ? 2u : 3u) : (n.z >= 0.0 ? 4u : 5u);
    uint h = cache_hash(uint(q.x) ^ cache_hash(uint(q.y) ^ cache_hash(uint(q.z) ^ cache_hash(uint(level) * 6u + direction))));
    place = h & (CACHE_CELLS - 1u);
    uint key = cache_hash(h ^ 0x5bd1e995u) | 1u;
    return key;
}
void cache_add(vec3 p, vec3 n, float distance_, vec3 light)
{
    RadianceCache c = RadianceCache(job.cache);
    uint place, key = cache_key(p, n, distance_, place);
    for (uint k = 0u; k < 8u; k++)
    {
        uint at = ((place + k) & (CACHE_CELLS - 1u)) * CACHE_WORDS;
        uint was = atomicCompSwap(c.v[at], 0u, key);
        if (was == 0u || was == key)
        {
            uvec3 add = uvec3(clamp(light, vec3(0.0), vec3(4096.0)) * CACHE_FIXED + 0.5);
            atomicAdd(c.v[at + 1u], add.r);
            atomicAdd(c.v[at + 2u], add.g);
            atomicAdd(c.v[at + 3u], add.b);
            atomicAdd(c.v[at + 4u], 1u);
            return;
        }
    }
}
/* the cell's mean light, and its samples (0: no cell) */
vec4 cache_get(vec3 p, vec3 n, float distance_)
{
    RadianceCache c = RadianceCache(job.cache);
    uint place, key = cache_key(p, n, distance_, place);
    for (uint k = 0u; k < 8u; k++)
    {
        uint at = ((place + k) & (CACHE_CELLS - 1u)) * CACHE_WORDS;
        uint was = c.v[at];
        if (was == key)
            return vec4(uintBitsToFloat(c.v[at + 6u]), uintBitsToFloat(c.v[at + 7u]), uintBitsToFloat(c.v[at + 8u]),
                uintBitsToFloat(c.v[at + 9u]));
        if (was == 0u)
            break;
    }
    return vec4(0.0);
}
/* the indirect light's clamp (job.cache_settings.x: a bounce's luminance;
0 none): the light past it taken away, its hue kept */
vec3 indirect_clamped(vec3 light)
{
    float most = job.cache_settings.x, y = luma(light);
    return most > 0.0 && y > most ? light * (most / y) : light;
}

/* ---------- ReSTIR GI (Ouyang et al. 2021), temporal reuse: the quality
profile's bounce

A bounce sample is its hit point (far along its direction where it leaves
the level), the hit's normal and the radiance it sends back (hit_light's:
the bake at the hit, which is diffuse, so the same toward any point near the
pixel). A pixel's reservoir: its own sample (one candidate from the cosine
density, its target the radiance's luminance by the cosine at the pixel)
and the last frame's reservoir at the surface's place there (the denoiser's
guide test: the same instance, the normal, the plane; its confidence capped
at job.denoise.z frames), merged by generalized resampled importance (Lin et
al. 2022): each sample's weight its MIS weight (the generalized balance
heuristic over the two points, each by its confidence and its target there,
a reconnection's Jacobian taking a sample from one point's measure to the
other's: the cosine at the hit over the other's, by the distances squared
the other way) by its target here by its contribution weight W. The last
reservoir's confidence counts whatever its sample (a miss's, a dark one's),
or the light would grow as it is reused (tools/test_prism_pt.py: the merge
unbiased across a moved point). A last sample whose Jacobian is outside 1/10
to 10 is not taken. The kept sample's light by the cosine over pi by W is
the pixel's bounce. Its segment to the pixel is not traced again: the point
moves by a pixel's footprint on its own surface between frames, so its
visibility seldom changes */
float gi_target(vec3 x, vec3 n, vec3 p_x, vec3 p_l)
{
    vec3 to = p_x - x;
    float d2 = max(dot(to, to), 1.0e-8);
    return luma(p_l) * max(dot(n, to) * inversesqrt(d2), 0.0);
}
float gi_jacobian(vec3 p_x, vec3 p_n, vec3 from_x, vec3 to_x)
{
    vec3 a = p_x - from_x, b = p_x - to_x;
    float da = max(dot(a, a), 1.0e-8), db = max(dot(b, b), 1.0e-8);
    float ca = abs(dot(p_n, a)) * inversesqrt(da), cb = abs(dot(p_n, b)) * inversesqrt(db);
    return ca > 1.0e-4 ? (cb / ca) * (da / db) : 0.0;
}
vec3 restir_gi(ivec2 pixel, Surface g, vec3 o, vec3 d, float hd, vec3 hn, vec3 radiance)
{
    vec3 y_x = hd > 0.0 ? o + d * hd : o + d * 1.0e4;
    vec3 y_n = hd > 0.0 && dot(hn, hn) > 0.0 ? hn : -d;
    vec3 y_l = max(radiance, vec3(0.0));
    float t_c = gi_target(g.point, g.normal, y_x, y_l);
    float w_c = t_c > 0.0 ? luma(y_l) * PI : 0.0;
    float w_sum = w_c, m_total = 1.0, y_t = t_c;
    if (job.denoise.z > 0.0 && g.has_history && (job.frame.y & FLAG_HISTORY) != 0u)
    {
        /* (one of the 2 x 2 by its bilinear weight, as ReSTIR DI's) */
        vec2 at_last = last_pixel(g.point, vec2(pixel), g.motion, g.instance == 0u);
        vec2 fl = floor(at_last), fr = at_last - fl;
        ivec2 q = ivec2(fl) + ivec2(rnd() < fr.x ? 1 : 0, rnd() < fr.y ? 1 : 0);
        if (inside(q))
        {
            uvec4 gl = imageLoad(guide_last, q);
            vec3 nl = unfold(unpackSnorm2x16(gl.y));
            float dl = uintBitsToFloat(gl.z);
            bool same = gl.x == (g.instance | 0x10000u) && dot(nl, g.normal) >= 0.9;
            vec3 x_last = last_point(q, dl);
            if (same && g.instance == 0u)
                same = abs(dot(x_last - g.point, g.normal)) <= 0.02 + 0.004 * g.distance;
            else if (same)
                same = abs(dl - g.distance) <= 0.05 * g.distance + 0.05;
            vec4 pa = imageLoad(gi_point_last, q), pb = imageLoad(gi_data_last, q);
            vec2 l_rg = unpackHalf2x16(floatBitsToUint(pb.x)), l_bm = unpackHalf2x16(floatBitsToUint(pb.y));
            float m_last = min(l_bm.y, job.denoise.z), w_last = pb.z;
            if (same && m_last > 0.0)
            {
                /* (this frame's sample: its MIS weight, the last point's target
                for it by the move's Jacobian) */
                float t_cl = gi_target(x_last, nl, y_x, y_l), j_cl = gi_jacobian(y_x, y_n, g.point, x_last);
                float m_c = t_c > 0.0 ? t_c / (t_c + m_last * t_cl * j_cl) : 0.0;
                w_sum = m_c * w_c;
                /* (the last frame's sample: its target here, by its Jacobian from
                there to here) */
                vec3 p_x = pa.xyz, p_n = unfold(unpackSnorm2x16(floatBitsToUint(pa.w)));
                vec3 p_l = vec3(l_rg, l_bm.x);
                float j_lc = gi_jacobian(p_x, p_n, x_last, g.point);
                float t_here = gi_target(g.point, g.normal, p_x, p_l), t_ll = gi_target(x_last, nl, p_x, p_l);
                if (w_last > 0.0 && !isinf(w_last) && j_lc > 0.1 && j_lc < 10.0 && t_here > 0.0)
                {
                    float m_l = m_last * t_ll / (t_here * j_lc + m_last * t_ll);
                    float w = m_l * t_here * w_last * j_lc;
                    if (w > 0.0 && !isinf(w))
                    {
                        w_sum += w;
                        if (rnd() * w_sum <= w)
                        {
                            y_x = p_x;
                            y_n = p_n;
                            y_l = p_l;
                            y_t = t_here;
                        }
                    }
                }
                m_total += m_last;
            }
        }
    }
    float big_w = y_t > 0.0 ? w_sum / y_t : 0.0;
    imageStore(gi_point_now, pixel, vec4(y_x, uintBitsToFloat(packSnorm2x16(fold(y_n)))));
    imageStore(gi_data_now, pixel, vec4(uintBitsToFloat(packHalf2x16(y_l.rg)),
        uintBitsToFloat(packHalf2x16(vec2(y_l.b, min(m_total, job.denoise.z)))), big_w, 0.0));
    float cos_y = max(dot(g.normal, normalize(y_x - g.point)), 0.0);
    return y_l * cos_y / PI * big_w;
}

/* a direction of GGX's visible normals (Heitz 2018) about n for the eye v at
roughness a (alpha = a * a): the reflected direction and the estimator's
weight (Fresnel by G2 / G1) */
vec3 ggx_sample(Surface g, out vec3 weight)
{
    vec3 t, bt;
    basis(g.normal, t, bt);
    vec3 v = vec3(dot(g.eye, t), dot(g.eye, bt), dot(g.eye, g.normal));
    float alpha = max(g.roughness * g.roughness, 1.0e-3);
    vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, max(v.z, 1.0e-4)));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float u1 = rnd(), u2 = rnd();
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    float p1 = r * cos(phi), p2 = r * sin(phi);
    float sh = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - sh) * sqrt(max(1.0 - p1 * p1, 0.0)) + sh * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    vec3 m = normalize(vec3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
    vec3 l = reflect(-v, m);
    weight = vec3(0.0);
    /* (the eye at or below the surface: no reflection, never a negative one) */
    if (l.z <= 0.0 || v.z <= 1.0e-4)
        return g.normal;
    float a2 = alpha * alpha;
    float g1v = 2.0 * v.z / (v.z + sqrt(a2 + (1.0 - a2) * v.z * v.z));
    float g1l = 2.0 * l.z / (l.z + sqrt(a2 + (1.0 - a2) * l.z * l.z));
    float g2 = g1v * g1l / max(g1v + g1l - g1v * g1l, 1.0e-4);
    vec3 f0 = f0_of(g);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(dot(v, m), 0.0, 1.0), 5.0);
    weight = fresnel * g2 / max(g1v, 1.0e-4);
    return normalize(l.x * t + l.y * bt + l.z * g.normal);
}


/* the sun's ray from o toward a direction of its disc, tested as the bake's
sky_blocked tests it (prism-v4-m2 61370f51f: Tool's rule, as far as the sun's
test distance, sun_reach, and a one-sided face met from its back letting it
through, up to 4 of them): 1 seen, 0 blocked, and where blocked the distance
to the blocker (the penumbra's width for the filter) */
float sun_ray_structure(vec3 o, vec3 d, out float blocker)
{
    float reach = sun_reach(), travelled = 0.0;
    blocker = 0.0;
    for (int k = 0; k < 4; k++)
    {
        vec3 nh;
        uint t;
        vec2 b;
        float h = hit(o, d, reach, nh, t, b);
        if (h < 0.0)
            return 1.0;
        if (dot(nh, nh) > 0.0)
        {
            blocker = travelled + h;
            return 0.0;
        }
        o += d * (h + 0.01);
        travelled += h + 0.01;
        reach -= h + 0.01;
        if (reach <= 0.0)
            return 1.0;
    }
    blocker = travelled;
    return 0.0;
}
/* ... and the moving objects' (the nearer blocker's distance kept) */
float sun_ray(vec3 o, out float blocker)
{
    vec3 d = cone_dir(s.sun.xyz, cos(0.5 * s.rig[0].w));
    float seen = sun_ray_structure(o, d, blocker);
    vec3 no, ao;
    float ho = objects_hit(o, d, seen > 0.5 ? sun_reach() : blocker, no, ao);
    if (ho >= 0.0)
    {
        blocker = ho;
        return 0.0;
    }
    return seen;
}

/* the lamps' light at a path's hit (the bake's lamps_ris for a texel's
normal: the same candidates, picks and estimate) */
vec3 hit_lamps(vec3 p, vec3 n, vec3 o, int candidates)
{
    if (s.counts.z == 0u || s.lamp.x <= 0.0 || s.rig[10].w < 0.5 || term_off(2u))
        return vec3(0.0);
    Floats f = Floats(s.lamps);
    vec3 kept = vec3(0.0), kept_l = vec3(0.0, 0.0, 1.0);
    float kept_target = 0.0, kept_d = 0.0, sum = 0.0;
    for (int c = 0; c < candidates; c++)
    {
        float probability;
        uint lamp = lamp_pick_mixed(p, n, probability);
        if (probability <= 0.0)
            continue;
        uint at = lamp * 16u;
        vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
        vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);
        vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);
        vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x;
        vec3 ne = normalize(cross(a1 - a0, a2 - a0));
        vec3 l;
        float d, inverse_pdf;
        if (!lamp_direction(p, a0, a1, a2, ne, l, d, inverse_pdf))
            continue;
        float cr = dot(n, l);
        if (cr <= 0.0)
            continue;
        vec3 integrand = radiance * cr / 3.14159265;
        float target = luma(integrand);
        float w = target * inverse_pdf / probability;
        if (!(w > 0.0) || isinf(w))
            continue;
        sum += w;
        if (rnd() * sum <= w)
        {
            kept = integrand;
            kept_target = target;
            kept_l = l;
            kept_d = d;
        }
    }
    if (kept_target <= 0.0 || sum <= 0.0 || !lamp_visible(o, kept_l, kept_d))
        return vec3(0.0);
    return kept / kept_target * (sum / float(candidates));
}

/* the domes at a path's hit by multiple importance sampling (Veach's
power heuristic) of two directions: one of each dome's cone (the next-event
estimate, its shadow traced, as domes_at), weighed by its cone's density
squared over the two's squares, and the path's own cosine direction where
it leaves the level into the cone (domes_missed), weighed by the cosine's.
The two weights sum to 1 at every direction of a cone, so the light is the
bake's estimator's (which meets the domes only where its cosine paths
leave), its noise within a few percent of the better one's however wide the
dome (tools/test_prism_pt.py): a narrow cone is met by one cosine direction
in hundreds (fireflies), a wide one fills the hemisphere the cosine already
samples */
vec3 domes_mis_at(vec3 o, vec3 n)
{
    vec3 sum = vec3(0.0);
    if (term_off(1u))
        return sum;
    for (int i = 0; i < 4; i++)
    {
        vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];
        if (dot(r.rgb, vec3(1.0)) <= 0.0)
            continue;
        float solid = max(6.2831853 * (1.0 - a.w), 1.0e-4);
        vec3 d = cone_dir(normalize(a.xyz), a.w);
        float c = dot(n, d);
        if (c > 0.0 && !sky_blocked(o, d, dome_reach(r)))
        {
            float pl = 1.0 / solid, pc = c / PI;
            sum += r.rgb * c * (pl * pl / (pl * pl + pc * pc));
        }
    }
    return sum;
}
vec3 domes_missed(vec3 d, vec3 n)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; i++)
    {
        vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];
        if (dot(r.rgb, vec3(1.0)) > 0.0 && dot(d, normalize(a.xyz)) >= a.w)
        {
            float solid = max(6.2831853 * (1.0 - a.w), 1.0e-4);
            float pl = 1.0 / solid, pc = max(dot(n, d), 0.0) / PI;
            sum += r.rgb * PI / solid * (term_off(1u) ? 1.0 : pc * pc / max(pl * pl + pc * pc, 1.0e-30));
        }
    }
    return sum;
}

/* the reference's path (the bake's path(), but the domes taken at the first
ray's miss where the point did not take them as lights: a reflection's).
At each hit the domes are taken by multiple importance sampling (above) with
the sun's, the lamps' and the fixtures' next-event estimates, so a ray that
leaves the level from there takes the sky's rest and the domes by their
weights */
/* a lamp's radiance toward a ray that meets its triangle's face from the
front (its material's emission, as s.lamps holds it; the same front as
lamp_direction's), else nothing: the level's triangles alone, as the bake's
lamps are */
vec3 lamp_met(uint t, vec3 d)
{
    if (t >= s.counts.x + s.counts.y)
        return vec3(0.0);
    uint m = material_of(t);
    if (m >= s.counts.w || (uint(material_value(m, 3u)) & 1u) == 0u || term_off(2u) || s.rig[10].w < 0.5 ||
        s.counts.z == 0u || s.lamp.x <= 0.0)
        return vec3(0.0);
    vec3 a0 = corner(t, 0u), a1 = corner(t, 1u), a2 = corner(t, 2u);
    vec3 ne = cross(a1 - a0, a2 - a0);
    if (dot(ne, ne) <= 1.0e-20 || dot(ne, -d) <= 0.0)
        return vec3(0.0);
    return vec3(material_value(m, 4u), material_value(m, 5u), material_value(m, 6u)) * s.rig[11].x;
}

/* the nearest hit, the structure's or a moving object's (t OBJECT_HIT, its
albedo in object_albedo) */
float scene_hit(vec3 o, vec3 d, float t_max, out vec3 nh, out uint t, out vec2 b, out vec3 object_albedo)
{
    float th = hit(o, d, t_max, nh, t, b);
    vec3 no;
    float ho = objects_hit(o, d, th < 0.0 ? t_max : th, no, object_albedo);
    if (ho < 0.0)
        return th;
    nh = no;
    t = OBJECT_HIT;
    return ho;
}

vec3 reference_path(vec3 o, vec3 d, int bounces, bool domes_first, bool lamps_first)
{
    vec3 nh, object_albedo;
    uint t;
    vec2 b;
    float th = scene_hit(o, d, 300.0, nh, t, b, object_albedo);
    if (th < 0.0)
        return sky_along(d, domes_first);
    if (dot(nh, nh) <= 0.0)
        return vec3(0.0);
    vec3 h = o + d * th;
    vec3 throughput = vec3(1.0), light = lamps_first && t != OBJECT_HIT ? lamp_met(t, d) : vec3(0.0);
    for (int bounce = 0; bounce < bounces && dot(nh, nh) > 0.0; bounce++)
    {
        if (t != OBJECT_HIT)
            light += throughput * glow_of(t);
        vec3 oh = h + nh * 0.03;
        vec3 lh;
        vec3 ls = sun_at(oh, nh) + hit_lamps(h, nh, oh, 4) + fixtures_ris(h, nh, oh, lh) + domes_mis_at(oh, nh);
        throughput *= t == OBJECT_HIT ? object_albedo : albedo_of(t);
        light += throughput * ls;
        if (bounce == bounces - 1)
            break;
        if (bounce >= 1)
        {
            float keep = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
            if (rnd() > keep)
                break;
            throughput /= keep;
        }
        vec3 wi = cosine_dir(nh);
        vec3 n_from = nh;
        float t2 = scene_hit(oh, wi, 300.0, nh, t, b, object_albedo);
        if (t2 < 0.0)
        {
            light += throughput * (sky_along(wi, false) + domes_missed(wi, n_from));
            break;
        }
        h = oh + wi * t2;
    }
    return light;
}

/* the reference's diffuse bounce, its direction resampled (Talbot et al.'s
RIS) from GUIDE cosine-distributed candidates, each traced to its hit and
weighed by the light the bake says that hit gives back (hit_light's) plus a
floor: the kept direction's path traced unbiased (reference_path) and its
light over its weight by the candidates' mean weight. The bake only steers
which direction is traced: the estimate's expectation is the path's,
whatever the bake holds (the floor keeps every direction's chance above 0),
so the reference stays the unbiased, bake-free estimate, its noise far
less where a dark pixel's light comes from a lit place few directions see */
const int GUIDE = 8;
/* the roughness under which the reference takes a lamp's specular light by
the reflection's own direction, not by next-event estimation */
const float REFERENCE_GLOSSY = 0.3;
/* the reference's paths' bounces at most (xgpu_stream.h's
XGPU_RT_REFERENCE_BOUNCES, the bake's paths' alike since prism-v4-m2
bb2a6b1a4: 8 cut a room with walls of albedo 0.85 short), Russian roulette
past the second */
const int REFERENCE_BOUNCES = 32;
const float GUIDE_FLOOR = 0.002;
vec3 guided_bounce(Surface g, vec3 o)
{
    vec3 kept = g.normal;
    float kept_w = 0.0, sum = 0.0;
    for (int k = 0; k < GUIDE; k++)
    {
        vec3 d = cosine_dir(g.normal);
        float hd;
        float w = luma(max(hit_light(o, d, 300.0, false, hd), vec3(0.0))) + GUIDE_FLOOR;
        sum += w;
        if (rnd() * sum <= w)
        {
            kept = d;
            kept_w = w;
        }
    }
    if (kept_w <= 0.0)
        return vec3(0.0);
    return reference_path(o, kept, REFERENCE_BOUNCES, false, false) / kept_w * (sum / float(GUIDE));
}

/* a pixel's seed for one use (decorrelated uses: the sun, the lights, the
bounce, the reflection each their own) */
void seed_for(ivec2 pixel, uint frame, uint use)
{
    seed = hash_u(uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + 0x2545f491u) ^
        hash_u(frame * 26699u + use * 0x9e3779b9u);
}

bool direct_on_of(uint view) { return view != 24u; }

/* the surface's guide for the frames after (its instance and present bit,
its normal, its distance, its roughness) */
uvec4 guide_of(Surface g)
{
    if (!g.present)
        return uvec4(0u);
    return uvec4(g.instance | 0x10000u, packSnorm2x16(fold(g.normal)), floatBitsToUint(g.distance),
        packUnorm2x16(vec2(g.roughness, 0.0)));
}

//@pass light
layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    uint frame = job.frame.x;
    uint view = job.frame.z;
    Surface g = surface_at(pixel);
    imageStore(guide_now, pixel, guide_of(g));
    if (!g.present || !direct_on_of(job.frame.z))
    {
        imageStore(reservoir_point_now, pixel, vec4(0.0));
        imageStore(reservoir_info_now, pixel, uvec4(uint(KIND_NONE), 0u, 0u, 0u));
    }
    /* (no bounce traced this frame, none kept: the GI reservoir empty) */
    if (!g.present || view == 23u)
        imageStore(gi_data_now, pixel, vec4(0.0));
    if (!g.present)
    {
        imageStore(noisy_diffuse, pixel, vec4(0.0));
        imageStore(noisy_specular, pixel, vec4(0.0));
        imageStore(clean_diffuse, pixel, vec4(0.0));
        imageStore(clean_specular, pixel, vec4(0.0));
        imageStore(sun_raw, pixel, vec4(0.0));
        return;
    }
    float offset = 0.01 + 0.0005 * g.distance;
    vec3 o = g.point + g.normal * offset;
    bool level = g.instance == 0u;
    bool performance = (job.frame.y & FLAG_PERFORMANCE) != 0u;
    /* (the views of the direct light alone, the indirect light alone) */
    bool direct_on = view != 24u, indirect_on = view != 23u;

    /* the bake at the pixel */
    vec3 bake_total, bake_indirect;
    float bake_sun;
    bool texel = level && bake_at(g.page, g.lightmap, g.normal, bake_total, bake_indirect, bake_sun);

    /* the sun: a ray near the eye and on the objects, the mask beyond */
    float v_sun = 0.0, blocker = 0.0, traced = 0.0;
    if (s.sun.w > 0.0 && dot(g.normal, s.sun.xyz) > 0.0 && !part_off(PARTS_SUN))
    {
        /* (the ray where the mask is not read (the performance profile's
        far pixels read it), except in a tile the bake found all in shadow:
        none of its texels' samples saw the sun, so the ray, the same
        structure's to the same length, would not either; a moving object
        only blocks. Round 7: most of an interior's sun rays, 0.85 ms of the
        RTX 3070 Ti's light pass at G4 (HALO_PT_SUN_TILES=0: traced)) */
        bool dark_tile = SPEC_SUN_TILES && texel && sun_tile_dark && bake_sun == 0.0;
        if ((!texel || bake_sun < 0.0 || g.distance < job.settings.x) && !dark_tile)
        {
            seed_for(pixel, frame, 1u);
            v_sun = sun_ray(o, blocker);
            traced = 1.0;
        }
        else
        {
            v_sun = bake_sun;
            /* (the moving objects' shadow over the mask, the sun's centre) */
            seed_for(pixel, frame, 1u);
            vec3 no, ao;
            float ho = v_sun > 0.0 ? objects_hit(o, s.sun.xyz, sun_reach(), no, ao) : -1.0;
            if (ho >= 0.0)
            {
                v_sun = 0.0;
                blocker = ho;
                traced = 1.0;
            }
        }
    }
    imageStore(sun_raw, pixel, vec4(direct_on ? v_sun : 0.0, blocker, traced, 0.0));

    vec3 noisy_d = vec3(0.0), noisy_s = vec3(0.0), clean_d = vec3(0.0), clean_s = vec3(0.0);
    vec3 share = specular_share(g);
    /* (a pixel whose reflection is traced (under the profile's ray
    roughness): the lamps' specular light shared between ReSTIR DI and that
    ray meeting their faces by multiple importance sampling (lamp_mis_nee).
    By next-event estimation alone a narrow lobe's lamp highlight is found by
    few candidate points and then held by the firefly clamp: round 2's a10
    corridor floor beyond the door windows lacked R's lamp strips'
    reflections (G5's second pose); by the ray alone a semi-glossy wall's
    pool of a small lamp's light is met by few rays (G4's second pose)) */
    bool glossy = g.roughness <= job.settings.y && indirect_on;
    /* (the reflection ray's length, for the specular history's virtual
    motion: 0 where none is traced, far where it leaves the level) */
    float spec_hit = 0.0;
    if (direct_on)
    {
        /* the domes, the lamps, the fixtures, the dynamic lights */
        vec3 m = vec3(0.0);
        seed_for(pixel, frame, 2u);
        if (!part_off(PARTS_DOMES))
            noisy_d += domes_at(o, g.normal, m);
        vec3 ld, ls;
        seed_for(pixel, frame, 3u);
        lamps_mis = glossy;
        lights_restir(pixel, g, o, job.extra.z > 0.0, ld, ls);
        lamps_mis = false;
        noisy_d += ld;
        noisy_s += ls;
        seed_for(pixel, frame, 9u);
        dynamic_direct(g, o, ld, ls);
        noisy_d += ld;
        noisy_s += ls;
        /* the sky's floor (the units' rig: unoccluded, as the bake adds it;
        the indoor one in an indoor cluster) */
        clean_d += floor_light(g);
    }
    if (indirect_on)
    {
        /* the indirect light: the bake's at the pixel, else a bounce (its
        sample reused by ReSTIR GI where job.denoise.z gives it frames) */
        /* (SPEC_BOUNCE_HALF: on half the level's 8 x 8 blocks (1) or half
        its pixels (2), a checkerboard of them turning each frame, a pixel with its texel takes
        the bake's indirect light as its sample of the bounce, through the
        denoiser like the traced one: the two estimate the same light (the
        bounce's hit is lit from the same bake), so the histories average
        them; by blocks whole subgroups leave the bounce's ray out) */
        int cell = SPEC_BOUNCE_HALF == 2 ? 0 : 3;
        bool bake_bounce = SPEC_BOUNCE_HALF != 0 && texel && !performance &&
            (((pixel.x >> cell) + (pixel.y >> cell) + int(frame)) & 1) != 0;
        if ((performance && texel) || part_off(PARTS_BOUNCE))
        {
            clean_d += texel ? bake_indirect : vec3(0.0);
            imageStore(gi_data_now, pixel, vec4(0.0));
        }
        else if (bake_bounce)
        {
            noisy_d += bake_indirect;
            imageStore(gi_data_now, pixel, vec4(0.0));
        }
        else
        {
            float hd;
            vec3 hn;
            seed_for(pixel, frame, 4u);
            vec3 d = cosine_dir(g.normal);
            vec3 bounce = hit_light_n(o, d, 300.0, false, hd, hn);
            /* (the radiance cache: every fourth pixel's hit added to its
            cell; the cell's mean in the hit's place where it has its
            samples) */
            if (SPEC_CACHE && job.cache != uvec2(0u) && hd > 0.0 && dot(hn, hn) > 0.0)
            {
                vec3 hp = o + d * hd;
                float from_eye = length(hp - (g.point + g.eye * g.distance));
                if (((uint(pixel.x) + uint(pixel.y) * 3u + frame) & 3u) == 0u)
                    cache_add(hp, hn, from_eye, bounce);
                vec4 cell = cache_get(hp, hn, from_eye);
                if (cell.w >= job.cache_settings.z)
                    bounce = cell.rgb;
            }
            if (SPEC_CLAMP)
                bounce = indirect_clamped(bounce);
            seed_for(pixel, frame, 8u);
            noisy_d += job.denoise.z > 0.0 && !part_off(PARTS_GI_REUSE) ?
                restir_gi(pixel, g, o, d, hd, hn, bounce) : bounce;
        }
        /* the reflections: a GGX ray below the roughness, else the probes */
        if (g.roughness <= job.settings.y && !part_off(PARTS_REFLECTION))
        {
            vec3 weight;
            seed_for(pixel, frame, 5u);
            vec3 r = ggx_sample(g, weight);
            float hd;
            if (dot(weight, weight) > 0.0)
            {
                vec3 met = hit_light(o, r, 300.0, true, hd);
                spec_hit = hd > 0.0 ? hd : 1000.0;
                if (glossy && direct_on && hit_light_triangle != 0xffffffffu && hd > 0.0)
                {
                    vec3 lamp = lamp_met(hit_light_triangle, r);
                    if (dot(lamp, lamp) > 0.0)
                    {
                        uint t = hit_light_triangle;
                        vec3 e = cross(corner(t, 1u) - corner(t, 0u), corner(t, 2u) - corner(t, 0u));
                        float area = 0.5 * length(e);
                        float ce = abs(dot(normalize(e), r));
                        /* (the solid angle from the pixel's point, as the
                        lamp's sample measures it) */
                        vec3 to = o + r * hd - g.point;
                        lamps_mis = true;
                        met += lamp * (1.0 - lamp_mis_nee(g, r, area * ce / max(dot(to, to), 1.0e-8)));
                        lamps_mis = false;
                    }
                }
                noisy_s += weight * met;
            }
        }
        else
        {
            float found;
            vec3 r = reflect(-g.eye, g.normal);
            vec3 probe = reflection_at(o, r, g.roughness, found);
            if (found <= 0.0)
            {
                vec4 v;
                probe = volume_at(o, r, v) ? v.rgb : texel ? bake_total : vec3(0.0);
            }
            clean_s += probe * share;
        }
    }
    if (any(isnan(noisy_d)) || any(isinf(noisy_d)))
        noisy_d = vec3(0.0);
    if (any(isnan(noisy_s)) || any(isinf(noisy_s)))
        noisy_s = vec3(0.0);
    /* (the specular light kept as the light the reflection takes in: its
    split sum's share out, put back by composite) */
    vec3 demodulated = noisy_s / max(share, vec3(1.0e-3));
    imageStore(noisy_diffuse, pixel, vec4(noisy_d, luma(noisy_d)));
    imageStore(noisy_specular, pixel, vec4(demodulated, luma(demodulated)));
    imageStore(clean_diffuse, pixel, vec4(clean_d, 0.0));
    imageStore(clean_specular, pixel, vec4(clean_s, 0.0));
    if (spec_hit > 0.0)
    {
        uvec4 guide = guide_of(g);
        guide.w = (guide.w & 0xffffu) | (packHalf2x16(vec2(0.0, min(spec_hit, 60000.0))) & 0xffff0000u);
        imageStore(guide_now, pixel, guide);
    }
}

//@pass temporal
layout(local_size_x = 8, local_size_y = 8) in;
/* the dynamic lights' change at a surface: their unshadowed diffuse light
there this frame and the recent ones' (job.volume.z and .w of them, the
recent ones after LIGHTS_MOST: each where it was last frame, its colour the
most the history may still hold of it), the change's share of the light the
history holds besides theirs (0 none, 1 all of it). A light switched off
or fading keeps cutting the history until what it may hold of it is gone (a
flashlight's spot was a ring on the wall for 200 frames after it) */
const uint LIGHTS_MOST = 64u;
const float DYNAMIC_RESET = 0.1;
const float DYNAMIC_LAG = 0.1;
/* (each light tested once a subgroup: its lanes take a light each, its
sphere against the box of the subgroup's points; only those that reach the
box are evaluated, by every lane. Every light at every pixel, a fight's
forty and their recent forty, cost the pass 1.8 ms at 1080p) */
float dynamic_lights_luma(Surface g, uint first, uint n, vec3 least, vec3 most)
{
    Floats f = Floats(job.lights);
    float sum = 0.0;
    for (uint base = 0u; base < n; base += gl_SubgroupSize)
    {
        uint i = base + gl_SubgroupInvocationID;
        bool reaches = false;
        if (i < n)
        {
            uint at = (first + i) * DYNAMIC_FLOATS;
            vec3 c = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);
            float r = f.v[at + 3u];
            vec3 e = max(max(least - c, c - most), vec3(0.0));
            reaches = r > 0.0 && dot(e, e) < r * r;
        }
        uvec4 mask = subgroupBallot(reaches);
        while (mask != uvec4(0u))
        {
            uint lane = subgroupBallotFindLSB(mask);
            mask[lane >> 5] &= ~(1u << (lane & 31u));
            vec3 l;
            float d;
            sum += luma(dynamic_light_at((first + base + lane) * DYNAMIC_FLOATS, g.point, l, d)) *
                max(dot(g.normal, l), 0.0);
        }
    }
    return sum;
}
float dynamic_change(Surface g, float history_luma)
{
    uint now_n = uint(job.volume.z), last_n = uint(job.volume.w);
    if (job.lights == uvec2(0u) || now_n + last_n == 0u)
        return 0.0;
    vec3 least = subgroupMin(g.point), most = subgroupMax(g.point);
    float a = dynamic_lights_luma(g, 0u, now_n, least, most);
    float b = dynamic_lights_luma(g, LIGHTS_MOST, last_n, least, most);
    float change = abs(a - b);
    return change / max(max(a, b) + max(history_luma - b, 0.0), 1.0e-4);
}
/* the last frame's histories at a pixel of this frame: the motion's point
there, its 2 x 2 taps each kept where its guide is the surface's (the same
instance, the normal within 25 degrees, the plane through it near the
point: no history across an edge, nor through an opening onto a surface
behind), bilinear over the kept ones */
/* whether the last frame's pixel q held this pixel's surface (the temporal
pass's guide test: the instance, the normal, and the level's point on this
surface's plane, an object's distance loosely) */
bool history_tap(ivec2 q, Surface g, float tolerance)
{
    if (!inside(q))
        return false;
    uvec4 gl = imageLoad(guide_last, q);
    if (gl.x != (g.instance | 0x10000u))
        return false;
    vec3 nl = unfold(unpackSnorm2x16(gl.y));
    if (dot(nl, g.normal) < 0.9)
        return false;
    float dl = uintBitsToFloat(gl.z);
    if (g.instance == 0u)
        return abs(dot(last_point(q, dl) - g.point, g.normal)) <= tolerance;
    return abs(dl - g.distance) <= 0.05 * g.distance + 0.05;
}
/* Catmull-Rom's weights for a tap 1, 0, 1, 2 away (the offset f in [0, 1)) */
vec4 catmull_rom(float f)
{
    float f2 = f * f, f3 = f2 * f;
    return vec4(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}
bool reproject(ivec2 pixel, Surface g, out vec4 d, out vec4 sp, out vec4 mo, out vec4 ms, out vec4 su,
    out float length_d)
{
    d = vec4(0.0);
    sp = vec4(0.0);
    mo = vec4(0.0);
    ms = vec4(0.0);
    su = vec4(0.0);
    length_d = 0.0;
    if ((job.frame.y & FLAG_HISTORY) == 0u || !g.has_history)
        return false;
    vec2 at = last_pixel(g.point, vec2(pixel), g.motion, g.instance == 0u);
    ivec2 a = ivec2(floor(at));
    vec2 f = at - vec2(a);
    float weights = 0.0;
    float tolerance = 0.02 + 0.004 * g.distance;
    for (int k = 0; k < 4; k++)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = a + o;
        if (!inside(q))
            continue;
        uvec4 gl = imageLoad(guide_last, q);
        if (gl.x != (g.instance | 0x10000u))
            continue;
        vec3 nl = unfold(unpackSnorm2x16(gl.y));
        if (dot(nl, g.normal) < 0.9)
            continue;
        float dl = uintBitsToFloat(gl.z);
        /* (the level stands still: its point there must lie on this
        surface's plane; an object moved, so its distance alone, loosely) */
        if (g.instance == 0u)
        {
            vec3 pl = last_point(q, dl);
            if (abs(dot(pl - g.point, g.normal)) > tolerance)
                continue;
        }
        else if (abs(dl - g.distance) > 0.05 * g.distance + 0.05)
            continue;
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        if (w <= 1.0e-4)
            continue;
        d += w * imageLoad(diff_last, q);
        sp += w * imageLoad(spec_last, q);
        mo += w * imageLoad(moments_last, q);
        ms += w * imageLoad(spec_moments_last, q);
        su += w * imageLoad(sun_last, q);
        weights += w;
    }
    if (weights < 0.05)
        return false;
    d /= weights;
    sp /= weights;
    mo /= weights;
    ms /= weights;
    su /= weights;
    length_d = d.a;
    /* (the light's histories resampled by Catmull-Rom where the whole 4 x 4
    about the place is this surface's, as TAA and ReLAX resample theirs:
    bilinear taps at every move blur the history along the move, so a moving
    eye's histories smeared a lamp's sparse samples into streaks toward the
    point it moves to (round 2's G4 floor) and the lit structure's light into
    the ground beside it (G6); the result held to the range of the 2 x 2
    taps, so it never rings past them. The sun's visibility (its slow and
    fast means) alike: G6's moonlit ground in motion brighter than R where
    the lit side's history bled over the shadows' edges. The moments and the
    lengths keep the bilinear taps) */
    if (weights > 0.999)
    {
        bool whole = true;
        for (int k = 0; k < 16 && whole; k++)
        {
            ivec2 o = ivec2(k & 3, k >> 2) - 1;
            if (o.x >= 0 && o.x <= 1 && o.y >= 0 && o.y <= 1)
                continue;
            whole = history_tap(a + o, g, tolerance);
        }
        if (whole)
        {
            vec4 wx = catmull_rom(f.x), wy = catmull_rom(f.y);
            vec3 cd = vec3(0.0), cs = vec3(0.0);
            vec2 cv = vec2(0.0), lo_v = vec2(1.0e30), hi_v = vec2(-1.0e30);
            vec3 lo_d = vec3(1.0e30), hi_d = vec3(-1.0e30), lo_s = vec3(1.0e30), hi_s = vec3(-1.0e30);
            for (int k = 0; k < 16; k++)
            {
                ivec2 o = ivec2(k & 3, k >> 2);
                ivec2 q = a + o - 1;
                float w = wx[o.x] * wy[o.y];
                vec3 vd = imageLoad(diff_last, q).rgb, vs = imageLoad(spec_last, q).rgb;
                vec2 vv = imageLoad(sun_last, q).xy;
                cd += w * vd;
                cs += w * vs;
                cv += w * vv;
                if (o.x >= 1 && o.x <= 2 && o.y >= 1 && o.y <= 2)
                {
                    lo_d = min(lo_d, vd);
                    hi_d = max(hi_d, vd);
                    lo_s = min(lo_s, vs);
                    hi_s = max(hi_s, vs);
                    lo_v = min(lo_v, vv);
                    hi_v = max(hi_v, vv);
                }
            }
            d.rgb = clamp(cd, lo_d, hi_d);
            sp.rgb = clamp(cs, lo_s, hi_s);
            su.xy = clamp(cv, lo_v, hi_v);
        }
    }
    return true;
}
/* the specular's history by its virtual motion (ReLAX's, after Zhdan's
talks on NVIDIA's denoisers, from their published descriptions): a
reflection moves with the reflected point's image behind the surface (the
point along the eye's ray past the surface by the reflection's length,
exact for a plane mirror), not with the surface, so a glossy floor's
history taken by the surface's motion smears the reflection along the
move (round 3's G4 and G5 floors streaking toward the point the eye moved
to). The reflection's length the mean of this pixel's and its 3 x 3
neighbours' rays on the same surface (the guide's w, the light pass's);
the last frame's place of the image found as the surface's is, its 2 x 2
taps each this surface's (a plane's image falls on the plane), bilinear.
amount: the share the virtual motion takes, 1 for a mirror, none past a
roughness of 0.45 (a wide lobe's reflection moves with the surface); 0
where no ray was traced. False where the image's place holds no history
of this surface (the caller then shortens the specular's) */
bool specular_virtual(ivec2 pixel, Surface g, out vec4 sp, out vec4 ms, out float amount)
{
    sp = vec4(0.0);
    ms = vec4(0.0);
    amount = 0.0;
    if (g.instance != 0u)
        return false;
    uint here = imageLoad(guide_now, pixel).x;
    float sum = 0.0, count = 0.0;
    for (int k = 0; k < 9; k++)
    {
        ivec2 q = pixel + ivec2(k % 3 - 1, k / 3 - 1);
        if (!inside(q))
            continue;
        uvec4 gq = imageLoad(guide_now, q);
        float h = unpackHalf2x16(gq.w).y;
        if (gq.x != here || !(h > 0.0))
            continue;
        sum += h;
        count += 1.0;
    }
    if (count < 1.0)
        return false;
    amount = 1.0 - smoothstep(0.1, 0.45, g.roughness);
    if (amount <= 0.0)
        return false;
    vec3 image = g.point - g.eye * (sum / count);
    vec2 at = last_pixel(image, vec2(pixel), g.motion, true);
    ivec2 a = ivec2(floor(at));
    vec2 f = at - vec2(a);
    float tolerance = 0.02 + 0.004 * g.distance, weights = 0.0;
    for (int k = 0; k < 4; k++)
    {
        ivec2 o = ivec2(k & 1, k >> 1);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        if (w <= 1.0e-4 || !history_tap(a + o, g, tolerance))
            continue;
        sp += w * imageLoad(spec_last, a + o);
        ms += w * imageLoad(spec_moments_last, a + o);
        weights += w;
    }
    if (weights < 0.5)
        return false;
    sp /= weights;
    ms /= weights;
    return true;
}
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    Surface g = surface_at(pixel);
    if (!g.present)
    {
        imageStore(diff_now, pixel, vec4(0.0));
        imageStore(spec_now, pixel, vec4(0.0));
        imageStore(moments_now, pixel, vec4(0.0));
        imageStore(spec_moments_now, pixel, vec4(0.0));
        imageStore(sun_now, pixel, vec4(0.0));
        imageStore(diff_a, pixel, vec4(0.0));
        imageStore(spec_a, pixel, vec4(0.0));
        imageStore(sun_a, pixel, vec4(0.0));
        return;
    }
    /* (this frame's samples: pre-blurred by the fix pass's pre-blur into
    diff_b and spec_b, parameters.x 0; else the light as traced, 1) */
    bool raw = push.parameters.x == 1u;
    vec4 nd = raw ? imageLoad(noisy_diffuse, pixel) : imageLoad(diff_b, pixel);
    vec4 ns = raw ? imageLoad(noisy_specular, pixel) : imageLoad(spec_b, pixel);
    vec4 sr = imageLoad(sun_raw, pixel);
    vec4 hd, hs, hm, hms, hsun;
    float length_d;
    /* (the share of the history the dynamic lights' change leaves: below
    DYNAMIC_RESET, none) */
    float dynamic_keep = 1.0;
    bool history = reproject(pixel, g, hd, hs, hm, hms, hsun, length_d);
    if (history)
    {
        /* (the specular's history by the reflection's image where it moves
        with it; where the image's place holds none, the surface's kept
        short, in proportion) */
        vec4 vs, vms;
        float amount;
        if (specular_virtual(pixel, g, vs, vms, amount))
        {
            hs = vec4(mix(hs.rgb, vs.rgb, amount), mix(hs.a, min(hs.a, vs.a), amount));
            hms = mix(hms, vms, amount);
        }
        else if (amount > 0.0)
            hs.a = mix(hs.a, min(hs.a, 2.0), amount);
        /* (the histories cut by the dynamic lights' change here: a light
        switched on (the flashlight, a muzzle flash, a bolt going by) is
        whole in its first frame, not a firefly held to the history nor a
        32nd of itself, one switched off or moved on leaves no trail, and
        one fading or rising (CE ramps the flashlight's colour over 6 frames
        on, 24 off) follows it: the history at most as long as keeps its lag
        under DYNAMIC_LAG of the light (a change of that share of it a frame
        held over that many frames; a history cut by (1 - 2 x share)^2 kept
        about 21 frames of 27 as the flashlight came on, and the spot came
        in over 12 frames and stayed whole through CE's fade)) */
        float cut = dynamic_change(g, hm.x);
        if (cut * job.denoise.x > DYNAMIC_LAG)
        {
            float most = DYNAMIC_LAG / cut;
            dynamic_keep = most / job.denoise.x;
            length_d = min(length_d, most);
            hs.a = min(hs.a, most);
        }
        /* (the hold, push.parameters.y (HALO_PT_SPEC_HOLD, on by default
        since round 8): while the eye moves, a glossy surface's specular history no brighter than the
        brightest of this frame's samples over the 3 x 3 on the same
        surface, as a TAA's clamp: a glint the history carries where nothing
        about it now reflects one is a reflection that moved on, which the
        reprojection's taps smeared out (round 7's radial streaks of wall
        glints as the eye moves forward). A lamp's highlight still there is
        among this frame's samples (ReSTIR finds the lamp most frames) */
        vec3 moved = job.camera[0].xyz - job.last_camera[0].xyz;
        if (push.parameters.y != 0u && g.roughness < 0.6 && dot(moved, moved) > 1.0e-8)
        {
            uint here = imageLoad(guide_now, pixel).x;
            float top = ns.a;
            for (int k = 0; k < 9; k++)
            {
                ivec2 q = pixel + ivec2(k % 3 - 1, k / 3 - 1);
                if (k == 4 || !inside(q) || imageLoad(guide_now, q).x != here)
                    continue;
                top = max(top, raw ? imageLoad(noisy_specular, q).a : imageLoad(spec_b, q).a);
            }
            float lh = luma(hs.rgb);
            if (lh > top)
                hs.rgb *= top / lh;
        }
    }
    /* (the counters, a level pixel in 16 that looks for its history: its
    last frame's place in the view, the distance the last frame saw there its
    point's in the last frame's camera (within 2%: the place is its own
    surface's), its history taken) */
    if (job.counters != uvec2(0u) && (pixel.x & 3) == 0 && (pixel.y & 3) == 0 && g.instance == 0u &&
        (job.frame.y & FLAG_HISTORY) != 0u && g.has_history)
    {
        Counters c = Counters(job.counters);
        atomicAdd(c.v[0], 1u);
        vec2 at = last_pixel(g.point, vec2(pixel), g.motion, true);
        ivec2 q = ivec2(floor(at + 0.5));
        mat3 rays = mat3(job.last_camera[1].xyz, job.last_camera[2].xyz, job.last_camera[3].xyz);
        float depth = abs(determinant(rays)) > 1.0e-12 ? (inverse(rays) * (g.point - job.last_camera[0].xyz)).x : 0.0;
        if (inside(q))
        {
            atomicAdd(c.v[2], 1u);
            float dl = uintBitsToFloat(imageLoad(guide_last, q).z);
            if (abs(dl - depth) <= 0.02 * depth + 0.01)
                atomicAdd(c.v[1], 1u);
        }
        if (history)
            atomicAdd(c.v[3], 1u);
    }
    float most_d = job.denoise.x, most_fast = job.denoise.y;
    /* (the specular's history shorter where the surface is glossy: a
    reflection moves over it as the eye does; by how far it moves, the
    parallax (the angle the eye turned about the point since the last
    frame) against the reflection lobe's width (about twice alpha), as ReLAX's specular
    accumulation: a still eye takes the whole history whatever the gloss,
    since its reflection does not move. Round 2's 4 frames for a mirror
    left a still frame's glossy floors and walls sparkling (G4, G5)) */
    float most_s = mix(4.0, most_d, clamp(g.roughness * 2.0, 0.0, 1.0));
    if (g.instance == 0u)
    {
        vec3 eye_last = job.last_camera[0].xyz - g.point;
        float parallax = acos(clamp(dot(g.eye, eye_last * inversesqrt(max(dot(eye_last, eye_last), 1.0e-12))), -1.0, 1.0));
        float alpha = max(g.roughness * g.roughness, 0.01);
        most_s = max(most_s, clamp(2.0 * alpha / max(parallax, 1.0e-6), 4.0, most_d));
    }

    /* fireflies: a new sample's luminance held, once the history has 16
    frames, to the history's mean and 8 times its samples' spread (the slow
    moments'); before, while the history is short (the camera moving, a
    disocclusion, the first frames), to 4 times the brightest of its
    neighbours on the same surface (eight, two pixels away), so that one
    sample of a small bright light (a keypad's glow met by the bounce, a lamp
    one candidate in thousands finds) is not spread by the fix's wide gather
    and the a-trous steps into a blob (round 1's moving frames in a10: green
    dots over a dark wall, smears where the history was short). Once the
    history is long, not to the neighbours: where few pixels see a light (a
    lamp through a gap) a sample far above them is the light itself, and a
    still camera's frames take it whole after its first 16 */
    float ld = nd.a, ls = ns.a;
    float cap_d = 1.0e30, cap_s = 1.0e30;
    if (history && length_d >= 16.0)
    {
        float sigma = sqrt(max(hm.y - hm.x * hm.x, 0.0));
        cap_d = max(hm.x + 8.0 * sigma, 1.0e-4);
    }
    if (history && hs.a >= 16.0)
    {
        float sigma = sqrt(max(hms.w - hms.z * hms.z, 0.0));
        cap_s = max(hms.z + 8.0 * sigma, 1.0e-4);
    }
    bool short_d = !history || length_d < 16.0, short_s = !history || hs.a < 16.0;
    if (short_d || short_s)
    {
        uint here = imageLoad(guide_now, pixel).x;
        float most_d = 0.0, most_s = 0.0;
        bool any_ = false;
        for (int k = 0; k < 8; k++)
        {
            ivec2 q = pixel + 2 * ivec2(k < 3 ? k - 1 : k == 3 ? -1 : k == 4 ? 1 : k - 6, k < 3 ? -1 : k < 5 ? 0 : 1);
            if (!inside(q) || imageLoad(guide_now, q).x != here)
                continue;
            most_d = max(most_d, raw ? imageLoad(noisy_diffuse, q).a : imageLoad(diff_b, q).a);
            most_s = max(most_s, raw ? imageLoad(noisy_specular, q).a : imageLoad(spec_b, q).a);
            any_ = true;
        }
        if (any_ && short_d)
            cap_d = min(cap_d, max(4.0 * most_d, 1.0e-3));
        /* (the specular's at 16 times: a glossy floor's reflection of a lamp
        strip is a few rays in many that meet it, its neighbours' samples
        dark, and 4 times them took it away wherever a moving eye's history
        was short: round 3's G5, the corridor's floor through the door
        windows without the strips R reflects) */
        if (any_ && short_s)
            cap_s = min(cap_s, max(16.0 * most_s, 1.0e-3));
    }
    vec3 cd = ld > cap_d ? nd.rgb * (cap_d / ld) : nd.rgb;
    vec3 cs = ls > cap_s ? ns.rgb * (cap_s / ls) : ns.rgb;
    /* (the moments take the sample as traced, the light's history the held
    one: moments of held samples hold the next ones lower still, so a light
    few frames see (a lamp one sample in twenty finds) was held to a half
    and less of itself while the eye stood still, and a moving eye's
    resampled histories loosened the hold: round 2's interiors darker than R
    held still, brighter moving (tools/test_prism_pt.py, the hold's bias)) */

    /* the slow history (its length, at most most_d), the fast (at most
    most_fast), the moments of the diffuse luminance (the slow's m1 m2, the
    fast's mean and m2) and the specular's fast mean and m2 */
    float n_d = history ? min(length_d + 1.0, most_d) : 1.0;
    float n_s = history ? min(hs.a + 1.0, most_s) : 1.0;
    float n_f = history ? min(length_d + 1.0, most_fast) : 1.0;
    float n_fs = history ? min(hs.a + 1.0, most_fast) : 1.0;
    vec3 slow_d = history ? mix(hd.rgb, cd, 1.0 / n_d) : cd;
    vec3 slow_s = history ? mix(hs.rgb, cs, 1.0 / n_s) : cs;
    float m1 = history ? mix(hm.x, ld, 1.0 / n_d) : ld;
    float m2 = history ? mix(hm.y, ld * ld, 1.0 / n_d) : ld * ld;
    float fast_d = history ? mix(hm.z, ld, 1.0 / n_f) : ld;
    float fast_m2 = history ? mix(hm.w, ld * ld, 1.0 / n_f) : ld * ld;
    float fast_s = history ? mix(hms.x, ls, 1.0 / n_fs) : ls;
    float fast_m2s = history ? mix(hms.y, ls * ls, 1.0 / n_fs) : ls * ls;
    /* (the fast history holds the slow one (ReLAX's history clamping): the
    slow's luminance kept within the fast mean and gamma times its own
    uncertainty (the fast samples' spread over its count), so that a light
    that changed does not linger; the slow history's length cut where it
    was held) */
    float variance = max(m2 - m1 * m1, 0.0);
    float gamma_ = job.denoise.w;
    /* (the fast mean's uncertainty: the samples' spread the slow moments
    know, over the fast count; a light seen in a few frames of many keeps
    its spread, so its frames without it do not hold the slow history to 0) */
    float sigma_f = sqrt(max(variance, fast_m2 - fast_d * fast_d) / n_f);
    float lo = fast_d - gamma_ * sigma_f - 0.02 * fast_d, hi = fast_d + gamma_ * sigma_f + 0.02 * fast_d;
    float sl = luma(slow_d);
    if (history && n_d > most_fast && (sl < lo || sl > hi))
    {
        float c = clamp(sl, lo, hi);
        slow_d *= sl > 1.0e-6 ? c / sl : 0.0;
        if (sl <= 1.0e-6)
            slow_d = vec3(c);
        n_d = min(n_d, most_fast);
    }
    float m1s = history ? mix(hms.z, ls, 1.0 / n_s) : ls;
    float m2s = history ? mix(hms.w, ls * ls, 1.0 / n_s) : ls * ls;
    float sigma_fs = sqrt(max(max(m2s - m1s * m1s, 0.0), fast_m2s - fast_s * fast_s) / n_fs);
    float los = fast_s - gamma_ * sigma_fs - 0.02 * fast_s, his = fast_s + gamma_ * sigma_fs + 0.02 * fast_s;
    float sls = luma(slow_s);
    if (history && n_s > most_fast && (sls < los || sls > his))
    {
        float c = clamp(sls, los, his);
        slow_s *= sls > 1.0e-6 ? c / sls : 0.0;
        if (sls <= 1.0e-6)
            slow_s = vec3(c);
        n_s = min(n_s, most_fast);
    }
    /* (a history the dynamic lights' change left none of: the frame's own
    pre-blurred light alone, its histories and moments begun again at it, as
    long as the fix pass's wide gather wants (a flash's light over a level
    of sparse bright samples gathered from 25 pixels about was blocks), its
    variance the neighbours' (the fix pass's, unknown here)) */
    bool dynamic_reset = history && dynamic_keep < DYNAMIC_RESET;
    if (dynamic_reset)
    {
        slow_d = cd;
        slow_s = cs;
        m1 = ld;
        m2 = ld * ld;
        fast_d = ld;
        fast_m2 = ld * ld;
        fast_s = ls;
        fast_m2s = ls * ls;
        m1s = ls;
        m2s = ls * ls;
        n_d = 4.0;
        n_s = 4.0;
    }
    imageStore(diff_now, pixel, vec4(slow_d, n_d));
    imageStore(spec_now, pixel, vec4(slow_s, n_s));
    imageStore(moments_now, pixel, vec4(m1, m2, fast_d, fast_m2));
    imageStore(spec_moments_now, pixel, vec4(fast_s, fast_m2s, m1s, m2s));
    /* (the variance for the spatial filter: the moments' once the history
    has a few frames; else unknown (the fix pass and the filter take it from
    the neighbours)) */
    float var_d = n_d >= 4.0 && !dynamic_reset ? variance : -1.0;
    imageStore(diff_a, pixel, vec4(slow_d, var_d));
    imageStore(spec_a, pixel, vec4(slow_s, n_s));

    /* the sun's visibility: its own history (at most most_d frames), its
    fast mean, its second moment */
    float n_v = history ? min(hsun.w + 1.0, most_d) : 1.0;
    float v = sr.x;
    float slow_v = history ? mix(hsun.x, v, 1.0 / n_v) : v;
    float fast_v = history ? mix(hsun.y, v, 1.0 / min(n_v, most_fast)) : v;
    float m2v = history ? mix(hsun.z, v * v, 1.0 / n_v) : v * v;
    /* (the visibility's histories held to the range the frame's own
    visibility spans over the pixel's 5 x 5 on the same surface: a lit pixel
    among lit neighbours is lit, whatever the history brought from a shadow
    beside it through the reprojection's bilinear taps. Round 1's G2 lost up
    to 8% of the foreground's light over its first leg's forward move (the
    denoised frames against the light as traced, block by block): each step's
    resampling blurred the rocks' shadows into the sand faster than the slow
    history's 1/32 a frame undid it, and the visibility's moments, blurred
    alike, let the fast history's hold pass it. Where the visibility is noisy
    (a penumbra, the trees' cutouts) its 25 samples span 0 to 1 (all alike
    one frame in thousands where the light is 0.7) and the history
    accumulates as before) */
    /* (where both histories already equal this frame's visibility (an
    interior's 0, an open lit ground's 1) the range cannot move them: the
    5 x 5 not read) */
    if (history && (slow_v != v || fast_v != v))
    {
        uint here = imageLoad(guide_now, pixel).x;
        float v_lo = v, v_hi = v;
        for (int y = -2; y <= 2; y++)
            for (int x = -2; x <= 2; x++)
            {
                ivec2 q = pixel + ivec2(x, y);
                if ((x == 0 && y == 0) || !inside(q) || imageLoad(guide_now, q).x != here)
                    continue;
                float vq = imageLoad(sun_raw, q).x;
                v_lo = min(v_lo, vq);
                v_hi = max(v_hi, vq);
            }
        slow_v = clamp(slow_v, v_lo, v_hi);
        fast_v = clamp(fast_v, v_lo, v_hi);
    }
    float var_v = max(m2v - slow_v * slow_v, 0.0);
    float sv = sqrt(var_v);
    if (history && (slow_v < fast_v - 2.0 * sv - 0.1 || slow_v > fast_v + 2.0 * sv + 0.1))
    {
        slow_v = clamp(slow_v, fast_v - 2.0 * sv - 0.1, fast_v + 2.0 * sv + 0.1);
        n_v = min(n_v, most_fast * 2.0);
    }
    imageStore(sun_now, pixel, vec4(slow_v, fast_v, m2v, n_v));
    /* (for the filter: the visibility, its spread, the blocker's distance,
    whether it was traced) */
    imageStore(sun_a, pixel, vec4(slow_v, var_v, sr.y, sr.z));
}

//@pass fix
layout(local_size_x = 8, local_size_y = 8) in;
/* ReLAX's history fix: where the history is short (a disocclusion, the
first frames), the signal gathered wide from the neighbours on the same
surface (the instance, the normal, the plane; their history's length the
weight), so that a pixel's first frames are not one sample's noise; the
same written into this frame's history. Elsewhere passed on. The variance
where it is unknown, from the 5 x 5 neighbourhood's luminance.
parameters.x 1: the pre-blur instead (below), 2: the light as traced copied
to diff_b and spec_b (unused since the temporal pass reads it itself) */
/* the pre-blur (ReLAX's pre-pass, from its published descriptions): before
the temporal pass, the light as traced spread over a disc of PREBLUR_TAPS
points about the pixel (a radius of PREBLUR_RADIUS pixels; the specular's
by its roughness, none on a mirror), each weighed by its plane's distance,
its normal and the same instance only (no luminance stop: the light's
energy is kept, a lucky sample shared with its neighbours), the disc turned
each frame and pixel. One sample of a lamp few candidates find is then a
twelfth as bright at a dozen pixels, not a single one at many times its
light: round 3's moving interiors glittered with such samples and dragged
them into streaks with the history. Written to diff_b and spec_b (free
until the fix), which the temporal pass reads as this frame's samples */
const int PREBLUR_TAPS = 12;
const float PREBLUR_RADIUS = 6.0;
void preblur(ivec2 pixel)
{
    uvec4 gc = imageLoad(guide_now, pixel);
    vec4 nd = imageLoad(noisy_diffuse, pixel), ns = imageLoad(noisy_specular, pixel);
    if (gc.x == 0u)
    {
        imageStore(diff_b, pixel, nd);
        imageStore(spec_b, pixel, ns);
        return;
    }
    vec3 nc = unfold(unpackSnorm2x16(gc.y));
    float dist = uintBitsToFloat(gc.z);
    float roughness = unpackUnorm2x16(gc.w).x;
    vec3 pc = job.camera[0].xyz + dist * ray_of(job.camera, job.corners, vec2(pixel));
    float radius_s = PREBLUR_RADIUS * smoothstep(0.1, 0.6, roughness);
    float turn = float(hash_u(uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + job.frame.x * 26699u) & 0xffffu) *
        (6.2831853 / 65536.0);
    vec3 sum_d = nd.rgb, sum_s = ns.rgb;
    float w_d = 1.0, w_s = 1.0;
    for (int k = 0; k < PREBLUR_TAPS; k++)
    {
        /* (a Vogel disc: even over the disc for any count) */
        float r = sqrt((float(k) + 0.5) / float(PREBLUR_TAPS));
        float a = float(k) * 2.3999632 + turn;
        vec2 o = r * vec2(cos(a), sin(a));
        ivec2 qd = pixel + ivec2(round(o * PREBLUR_RADIUS));
        if (qd != pixel && inside(qd))
        {
            uvec4 gq = imageLoad(guide_now, qd);
            if (gq.x == gc.x)
            {
                vec3 nq = unfold(unpackSnorm2x16(gq.y));
                vec3 pq = job.camera[0].xyz + uintBitsToFloat(gq.z) * ray_of(job.camera, job.corners, vec2(qd));
                float w = exp(-abs(dot(pq - pc, nc)) / (0.02 + 0.004 * dist)) * power16(max(dot(nq, nc), 0.0));
                if (w > 1.0e-3)
                {
                    sum_d += w * imageLoad(noisy_diffuse, qd).rgb;
                    w_d += w;
                }
            }
        }
        ivec2 qs = pixel + ivec2(round(o * radius_s));
        if (radius_s >= 1.0 && qs != pixel && inside(qs))
        {
            uvec4 gq = imageLoad(guide_now, qs);
            if (gq.x == gc.x)
            {
                vec3 nq = unfold(unpackSnorm2x16(gq.y));
                vec3 pq = job.camera[0].xyz + uintBitsToFloat(gq.z) * ray_of(job.camera, job.corners, vec2(qs));
                float rq = unpackUnorm2x16(gq.w).x;
                float wn = power16(max(dot(nq, nc), 0.0));
                float w = exp(-abs(dot(pq - pc, nc)) / (0.02 + 0.004 * dist)) * wn * wn * exp(-abs(rq - roughness) * 8.0);
                if (w > 1.0e-3)
                {
                    sum_s += w * imageLoad(noisy_specular, qs).rgb;
                    w_s += w;
                }
            }
        }
    }
    sum_d /= w_d;
    sum_s /= w_s;
    imageStore(diff_b, pixel, vec4(sum_d, luma(sum_d)));
    imageStore(spec_b, pixel, vec4(sum_s, luma(sum_s)));
}
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    if (push.parameters.x == 1u)
    {
        preblur(pixel);
        return;
    }
    if (push.parameters.x == 2u)
    {
        imageStore(diff_b, pixel, imageLoad(noisy_diffuse, pixel));
        imageStore(spec_b, pixel, imageLoad(noisy_specular, pixel));
        return;
    }
    vec4 dc = imageLoad(diff_a, pixel);
    vec4 sc = imageLoad(spec_a, pixel);
    vec4 vc = imageLoad(sun_a, pixel);
    vec4 hist = imageLoad(diff_now, pixel);
    vec4 hist_v = imageLoad(sun_now, pixel);
    uvec4 gc = imageLoad(guide_now, pixel);
    if (gc.x == 0u)
    {
        imageStore(diff_b, pixel, dc);
        imageStore(spec_b, pixel, sc);
        imageStore(sun_b, pixel, vc);
        return;
    }
    vec3 nc = unfold(unpackSnorm2x16(gc.y));
    float dist = uintBitsToFloat(gc.z);
    vec3 pc = job.camera[0].xyz + dist * ray_of(job.camera, job.corners, vec2(pixel));
    float n_d = hist.a;
    float n_s = sc.a;
    bool fix_d = n_d < 4.0, fix_s = n_s < 4.0, fix_v = hist_v.w < 4.0 && vc.w > 0.5;
    float sum_v = 0.0, w_v = 0.0, blocker_most = vc.z;
    /* (the stride: wider the shorter the history) */
    int stride = n_d < 1.5 ? 4 : n_d < 2.5 ? 3 : 2;
    vec3 sum_d = vec3(0.0), sum_s = vec3(0.0);
    float w_d = 0.0, w_s = 0.0, m1 = 0.0, m2 = 0.0, wm = 0.0;
    /* (nothing to fix and the variance known: the gather not made) */
    int reach = fix_d || fix_s || fix_v || dc.a < 0.0 ? 3 : -1;
    for (int y = -reach; y <= reach; y++)
        for (int x = -reach; x <= reach; x++)
        {
            ivec2 q = pixel + ivec2(x, y) * ((fix_d || fix_s || fix_v) ? stride : 1);
            if (!inside(q))
                continue;
            uvec4 gq = imageLoad(guide_now, q);
            if (gq.x != gc.x)
                continue;
            vec3 nq = unfold(unpackSnorm2x16(gq.y));
            float wn = power16(max(dot(nq, nc), 0.0));
            vec3 pq = job.camera[0].xyz + uintBitsToFloat(gq.z) * ray_of(job.camera, job.corners, vec2(q));
            float plane = abs(dot(pq - pc, nc)) / (0.02 + 0.01 * dist);
            float wz = exp(-plane);
            float w = wn * wz;
            if (w <= 1.0e-4)
                continue;
            vec4 dq = imageLoad(diff_a, q);
            vec4 sq = imageLoad(spec_a, q);
            float hq = imageLoad(diff_now, q).a;
            float wd = w * (1.0 + hq);
            sum_d += wd * dq.rgb;
            w_d += wd;
            float ws = w * (1.0 + sq.a);
            sum_s += ws * sq.rgb;
            w_s += ws;
            if (fix_v && abs(x) <= 1 && abs(y) <= 1)
            {
                vec4 vq = imageLoad(sun_a, q);
                sum_v += w * vq.x;
                w_v += w;
                blocker_most = max(blocker_most, vq.z);
            }
            if (abs(x) <= 2 && abs(y) <= 2)
            {
                float l = luma(dq.rgb);
                m1 += w * l;
                m2 += w * l * l;
                wm += w;
            }
        }
    vec4 od = dc, os = sc;
    if (fix_d && w_d > 0.0)
    {
        od.rgb = sum_d / w_d;
        imageStore(diff_now, pixel, vec4(od.rgb, hist.a));
    }
    if (fix_s && w_s > 0.0)
    {
        os.rgb = sum_s / w_s;
        vec4 sh = imageLoad(spec_now, pixel);
        imageStore(spec_now, pixel, vec4(os.rgb, sh.a));
    }
    if (od.a < 0.0)
    {
        float mean = wm > 0.0 ? m1 / wm : luma(od.rgb);
        float spatial = wm > 0.0 ? max(m2 / wm - mean * mean, 0.0) : 0.0;
        /* (the neighbourhood's own spread, as SVGF takes it for a history
        under 4 frames: round 1 made it 4 / n times larger, which let the
        a-trous steps blur a moving camera's noise into smears) */
        od.a = spatial;
    }
    /* (the specular's variance: its spread over the 3 x 3 as it is. The
    history is already a mean of its frames, so its spread over the
    neighbours is what noise is left: round 3 divided it again by the
    history's length, so a long history's stops were a fifth as wide as its
    noise and a still eye's lamp pools on the semi-glossy walls stayed
    grainy (G4's second and third poses)) */
    os.a = 0.0;
    {
        float s1 = 0.0, s2 = 0.0, sw = 0.0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
            {
                ivec2 q = pixel + ivec2(x, y);
                if (!inside(q))
                    continue;
                float l = luma(imageLoad(spec_a, q).rgb);
                s1 += l;
                s2 += l * l;
                sw += 1.0;
            }
        float mean = s1 / max(sw, 1.0);
        os.a = max(s2 / max(sw, 1.0) - mean * mean, 0.0);
    }
    vec4 ov = vc;
    if (fix_v && w_v > 0.0)
    {
        /* (a short history's visibility: its neighbours' mean, its spread
        theirs, so that the filter takes it up) */
        float mean = sum_v / w_v;
        ov = vec4(mean, max(mean * (1.0 - mean), vc.y), blocker_most, vc.w);
    }
    imageStore(diff_b, pixel, od);
    imageStore(spec_b, pixel, os);
    imageStore(sun_b, pixel, ov);
}

//@pass atrous
layout(local_size_x = 8, local_size_y = 8) in;
/* one step of the a-trous filter (Dammertz et al.; SVGF's and ReLAX's edge
stops): a 3 x 3 kernel (1/4, 1/2, 1/4 each way) at a stride of 2^step, five
steps (strides 1 to 16: the reach of SVGF's five 5 x 5 steps with a third of
their taps), each tap weighed by
its plane's distance, its normal, the same instance (never across an
object's or the level's edge) and its luminance against the pixel's by the
variance (the diffuse's and the specular's), the specular's kernel narrowed
by the roughness; the variance filtered with the squared weights. The sun's
visibility likewise where it is neither all lit nor all dark about the
pixel, its kernel as wide as the penumbra (the blocker's distance by the
sun's size). parameters.x the step, y the direction (0: a to b, 1: b to
a), z which signals (1 diffuse, 2 specular, 4 the sun) */
vec4 load_d(ivec2 q, bool from_b) { return from_b ? imageLoad(diff_b, q) : imageLoad(diff_a, q); }
vec4 load_s(ivec2 q, bool from_b) { return from_b ? imageLoad(spec_b, q) : imageLoad(spec_a, q); }
vec4 load_v(ivec2 q, bool from_b) { return from_b ? imageLoad(sun_b, q) : imageLoad(sun_a, q); }
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    uint step_ = push.parameters.x;
    bool from_b = push.parameters.y != 0u;
    uint signals = push.parameters.z;
    int stride = 1 << step_;
    vec4 dc = load_d(pixel, from_b), sc = load_s(pixel, from_b), vc = load_v(pixel, from_b);
    uvec4 gc = imageLoad(guide_now, pixel);
    if (gc.x == 0u)
    {
        if (from_b)
        {
            imageStore(diff_a, pixel, dc);
            imageStore(spec_a, pixel, sc);
            imageStore(sun_a, pixel, vc);
        }
        else
        {
            imageStore(diff_b, pixel, dc);
            imageStore(spec_b, pixel, sc);
            imageStore(sun_b, pixel, vc);
        }
        return;
    }
    vec3 nc = unfold(unpackSnorm2x16(gc.y));
    float dist = uintBitsToFloat(gc.z);
    float roughness = unpackUnorm2x16(gc.w).x;
    vec3 pc = job.camera[0].xyz + dist * ray_of(job.camera, job.corners, vec2(pixel));
    /* (the first step's anti-firefly, for the frame shown alone: a pixel
    brighter than twice the brightest of its eight neighbours on the same
    surface is held to that, as ReLAX's anti-firefly holds a pixel to its
    neighbourhood. The histories keep the light whole (the temporal pass's
    hold is loose since its moments are the samples as traced), so a sample
    of a light few rays find adds its share to them; here its single bright
    pixel is not shown nor spread by the steps after (round 3's G7 walls, a
    glitter of single pixels in the still frame)) */
    if (step_ == 0u)
    {
        float most_d = 0.0, most_s = 0.0;
        bool any_ = false;
        for (int k = 0; k < 9; k++)
        {
            ivec2 q = pixel + ivec2(k % 3 - 1, k / 3 - 1);
            if (k == 4 || !inside(q) || imageLoad(guide_now, q).x != gc.x)
                continue;
            most_d = max(most_d, luma(load_d(q, from_b).rgb));
            most_s = max(most_s, luma(load_s(q, from_b).rgb));
            any_ = true;
        }
        float l0 = luma(dc.rgb), l1 = luma(sc.rgb);
        if (any_ && l0 > 2.0 * most_d + 1.0e-4)
            dc.rgb *= (2.0 * most_d + 1.0e-4) / l0;
        if (any_ && l1 > 2.0 * most_s + 1.0e-4)
            sc.rgb *= (2.0 * most_s + 1.0e-4) / l1;
    }
    float lc = luma(dc.rgb), lsc = luma(sc.rgb);
    /* (the luminance's edge stop by the variance prefiltered over the pixel's
    cross on the same surface (half the pixel's, an eighth each neighbour's),
    as SVGF prefilters it: one pixel's estimate alone is noisy, and a step
    whose stops vary from pixel to pixel leaves blotches) */
    float var_cross = 0.5 * max(dc.a, 0.0), var_w = 0.5;
    for (int k = 0; k < 4; k++)
    {
        ivec2 q = pixel + ivec2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0);
        if (!inside(q) || imageLoad(guide_now, q).x != gc.x)
            continue;
        var_cross += 0.125 * max(load_d(q, from_b).a, 0.0);
        var_w += 0.125;
    }
    float sigma_d = job.settings.w * sqrt(var_cross / var_w) + 1.0e-4;
    float sigma_s = job.settings.w * sqrt(max(sc.a, 0.0)) + 1.0e-4;
    /* (the specular's reach: none on a mirror, the diffuse's when rough;
    a signal with no variance left is passed on) */
    bool spec_on = (signals & 2u) != 0u && float(stride) <= 1.0 + 16.0 * roughness && sc.a > 1.0e-8;
    if (dc.a <= 1.0e-8)
        signals &= ~1u;
    /* (the sun: in a penumbra only; its kernel's reach in pixels, the
    penumbra's width: the blocker's distance by the disc's angle, over the
    pixel's footprint at this distance) */
    float footprint = 2.0 * length(job.camera[2].xyz) * dist / max(float(job.corners.z - job.corners.x), 1.0);
    float penumbra = vc.z * s.rig[0].w / max(footprint, 1.0e-4);
    /* (only a pixel in a penumbra, its visibility neither all lit nor all
    dark: a lit pixel beside a shadow is never darkened by it, which in G2's
    moves (round 1) took up to a third of the foreground's sunlight) */
    bool sun_on = (signals & 4u) != 0u && vc.w > 0.5 && float(stride) <= max(penumbra, 2.0) &&
        vc.y > 1.0e-4 && vc.x > 0.02 && vc.x < 0.98;
    const float kernel[2] = float[2](1.0 / 2.0, 1.0 / 4.0);
    vec3 sum_d = vec3(0.0), sum_s = vec3(0.0);
    float wsum_d = 0.0, wsum_s = 0.0, var_d = 0.0, var_s = 0.0, sum_v = 0.0, wsum_v = 0.0, var_v = 0.0;
    float blocker_most = vc.z;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y) * stride;
            if (!inside(q))
                continue;
            uvec4 gq = imageLoad(guide_now, q);
            if (gq.x != gc.x)
                continue;
            float k = kernel[abs(x)] * kernel[abs(y)];
            vec3 nq = unfold(unpackSnorm2x16(gq.y));
            vec3 pq = job.camera[0].xyz + uintBitsToFloat(gq.z) * ray_of(job.camera, job.corners, vec2(q));
            float plane = abs(dot(pq - pc, nc)) / (0.01 + 0.004 * dist * float(stride));
            float wz = exp(-plane);
            float wn = power16(max(dot(nq, nc), 0.0));
            wn *= wn;
            wn *= wn;
            float wgeo = k * wz * wn;
            if (wgeo <= 1.0e-6)
                continue;
            if ((signals & 1u) != 0u)
            {
                vec4 dq = load_d(q, from_b);
                float wl = exp(-abs(luma(dq.rgb) - lc) / sigma_d);
                float w = wgeo * wl;
                sum_d += w * dq.rgb;
                var_d += w * w * max(dq.a, 0.0);
                wsum_d += w;
            }
            if (spec_on)
            {
                vec4 sq = load_s(q, from_b);
                float rq = unpackUnorm2x16(gq.w).x;
                float wr = exp(-abs(rq - roughness) * 8.0);
                float wl = exp(-abs(luma(sq.rgb) - lsc) / sigma_s);
                float w = wgeo * wl * wr * wn;
                sum_s += w * sq.rgb;
                var_s += w * w * max(sq.a, 0.0);
                wsum_s += w;
            }
            if (sun_on)
            {
                vec4 vq = load_v(q, from_b);
                float w = wgeo;
                sum_v += w * vq.x;
                var_v += w * w * vq.y;
                wsum_v += w;
                blocker_most = max(blocker_most, vq.z);
            }
        }
    vec4 od = dc, os = sc, ov = vc;
    if ((signals & 1u) != 0u && wsum_d > 0.0)
        od = vec4(sum_d / wsum_d, var_d / (wsum_d * wsum_d));
    if (spec_on && wsum_s > 0.0)
        os = vec4(sum_s / wsum_s, var_s / (wsum_s * wsum_s));
    if (sun_on && wsum_v > 0.0)
        ov = vec4(sum_v / wsum_v, var_v / (wsum_v * wsum_v), blocker_most, vc.w);
    if (from_b)
    {
        imageStore(diff_a, pixel, od);
        imageStore(spec_a, pixel, os);
        imageStore(sun_a, pixel, ov);
    }
    else
    {
        imageStore(diff_b, pixel, od);
        imageStore(spec_b, pixel, os);
        imageStore(sun_b, pixel, ov);
    }
}

//@pass composite
layout(local_size_x = 8, local_size_y = 8) in;
/* the frame (Raster's form): the diffuse light (the floor, the bake's
indirect light, the denoised light, the sun's by its filtered visibility)
by the base colour less the specular's share, the specular light (the
denoised by its share, the probes', the sun's), the emissive light;
parameters.x the source of the denoised light (0 a, 1 b: where the last
filter step wrote; 2 the noisy light as traced, the denoiser off; 3 the
reference's sums) */
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    vec4 additions = texelFetch(additions_in, pixel, 0);
    Surface g = surface_at(pixel);
    uint view = job.frame.z;
    uint source = push.parameters.x;
    if (!g.present)
    {
        /* (no surface of the G-buffer: the frame as CE drew it there, the
        sky; nothing of it is CE's lighting) */
        vec3 classic = texelFetch(light_in, pixel, 0).rgb * texelFetch(base_in, pixel, 0).rgb + additions.rgb;
        imageStore(frame_out, pixel, vec4(view >= 20u ? vec3(0.0) : classic, additions.a));
        return;
    }
    vec3 share = specular_share(g);
    vec3 diffuse_light, specular_light;
    vec3 sd, ss;
    sun_parts(g, sd, ss);
    if (source == 3u)
    {
        /* the reference's means */
        Floats sums = Floats(job.sums);
        uint at = (uint(pixel.y) * job.counts.y + uint(pixel.x)) * SUMS;
        diffuse_light = vec3(sums.v[at], sums.v[at + 1u], sums.v[at + 2u]);
        specular_light = vec3(sums.v[at + 3u], sums.v[at + 4u], sums.v[at + 5u]);
    }
    else
    {
        vec4 dd = source == 2u ? imageLoad(noisy_diffuse, pixel) : source == 1u ? imageLoad(diff_b, pixel) :
            imageLoad(diff_a, pixel);
        vec4 ds = source == 2u ? imageLoad(noisy_specular, pixel) : source == 1u ? imageLoad(spec_b, pixel) :
            imageLoad(spec_a, pixel);
        vec4 dv = source == 2u ? imageLoad(sun_raw, pixel) : source == 1u ? imageLoad(sun_b, pixel) :
            imageLoad(sun_a, pixel);
        float v = clamp(dv.x, 0.0, 1.0);
        vec3 cd = imageLoad(clean_diffuse, pixel).rgb;
        vec3 cs = imageLoad(clean_specular, pixel).rgb;
        diffuse_light = cd + max(dd.rgb, vec3(0.0)) + sd * v;
        specular_light = cs + max(ds.rgb, vec3(0.0)) * share + ss * v;
        if (view == 22u)
        {
            imageStore(frame_out, pixel, vec4(vec3(v), additions.a));
            return;
        }
        if (view == 25u)
        {
            float n = imageLoad(diff_now, pixel).a;
            imageStore(frame_out, pixel, vec4(vec3(n / max(job.denoise.x, 1.0)), additions.a));
            return;
        }
        if (view == 26u)
        {
            vec4 nd = imageLoad(noisy_diffuse, pixel);
            imageStore(frame_out, pixel, vec4(cd + nd.rgb + sd * imageLoad(sun_raw, pixel).x, additions.a));
            return;
        }
    }
    if (view == 20u || view == 23u || view == 24u)
    {
        imageStore(frame_out, pixel, vec4(diffuse_light, additions.a));
        return;
    }
    if (view == 21u)
    {
        imageStore(frame_out, pixel, vec4(specular_light, additions.a));
        return;
    }
    vec3 kd = g.albedo * (1.0 - share);
    /* (the frame, as Raster's: the exposure (display.prism_exposure; "ce"
    metered against Classic's frame at the pixel, a lit pixel in 16, this
    frame before the exposure, the step after the pass making the next
    frame's) on the diffuse and specular light; the diffuse light then held at
    white as CE's page held it (a colour past white divided by its brightest
    channel, its hue kept), before the base colour; the glow as CE's; then
    the tone curve. The live frames and the reference's alike, so that the
    gates compare them on one side of the hold) */
    bool metered = job.exposure.z > 0.5 && job.meter != uvec2(0u);
    float exposure = metered && job.exposure.x > 0.5 ? exposure_of(job.meter) : 1.0;
    if (metered && (pixel.x & 3) == 0 && (pixel.y & 3) == 0)
    {
        vec3 classic = texelFetch(light_in, pixel, 0).rgb * texelFetch(base_in, pixel, 0).rgb + additions.rgb;
        meter_add(job.meter, luma(kd * diffuse_light + specular_light + g.emissive), luma(classic));
    }
    /* (the hold at white only under the "ce" curve, the display's clip:
    Adam's direction of 21:34, the light physically lit, a rolled-off curve
    taking what is past white) */
    int curve = int(job.exposure.y + 0.5);
    vec3 held = diffuse_light * exposure;
    float brightest = max(held.r, max(held.g, held.b));
    if (curve == 0 && brightest > 1.0)
        held /= brightest;
    vec3 result = tone_curved(kd * held + specular_light * exposure + g.emissive, curve);
    if (any(isnan(result)) || any(isinf(result)))
        result = vec3(0.0);
    imageStore(frame_out, pixel, vec4(result, additions.a));
}

//@pass reference
layout(local_size_x = 8, local_size_y = 8) in;
/* debug.pt_reference in this mode: at each pixel the same scene,
materials and lights traced unbiased with no bake, no reuse and no
denoiser: the sun (a ray to its disc), the domes, the lamps, the fixtures
and the dynamic lights (resampled importance, unbiased for its one ray), the
floor, the indirect light by the bake's own estimator (a cosine path of up
to 32 bounces, Russian roulette past the second, every hit's sun and lamps
by next-event estimation), the reflection by a GGX ray and the same path
(one of the two lobes a sample, by their shares); extra.y of these a pixel
each frame, summed (Welford's running mean, so
that a long sum keeps its precision) into job.sums: per pixel the diffuse
light's mean (3), the specular light's (3), and the mean and the sum of
squared deviations of the frame's luminance there (its noise:
reference_stats). parameters.x this frame's number in the sum (1 on).
parameters.y 1 (HALO_PT_REFERENCE_LIVE, a diagnosis): the light pass's own
estimate summed instead (its light as traced, ReSTIR's reuse kept, no
denoiser), so that its mean against R's says what the live estimator holds
apart from what the denoiser does */
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
    uint n = push.parameters.x;
    Floats sums = Floats(job.sums);
    uint at = (uint(pixel.y) * job.counts.y + uint(pixel.x)) * SUMS;
    Surface g = surface_at(pixel);
    if (n == 0u)
        return;
    if (!g.present)
    {
        if (n == 1u)
            for (uint k = 0u; k < SUMS; k++)
                sums.v[at + k] = 0.0;
        return;
    }
    float offset = 0.01 + 0.0005 * g.distance;
    vec3 o = g.point + g.normal * offset;
    vec3 share = specular_share(g);
    vec3 kd = g.albedo * (1.0 - share);
    uint paths = push.parameters.y == 1u ? 0u : max(uint(job.extra.y), 1u);
    vec3 ld_sum = vec3(0.0), ls_sum = vec3(0.0);
    /* (the frame's luminance, and its parts': the light the pixel takes
    directly (the floor, the sun, the domes, the lamps, fixtures and dynamic
    lights), its diffuse bounce's, its reflection's) */
    float y_sum = 0.0, y2_sum = 0.0;
    vec3 parts_sum = vec3(0.0);
    seed_for(pixel, n * 64u, 7u);
    vec3 floor_ = floor_light(g);
    for (uint k = 0u; k < paths; k++)
    {
        uint frame = n * 64u + k;
        vec3 ld = floor_, ls = vec3(0.0);
        vec3 sd, ss;
        sun_parts(g, sd, ss);
        if (dot(sd, sd) > 0.0)
        {
            seed_for(pixel, frame, 1u);
            float blocker;
            float v = sun_ray(o, blocker);
            ld += sd * v;
            ls += ss * v;
        }
        vec3 m = vec3(0.0);
        seed_for(pixel, frame, 2u);
        ld += domes_at(o, g.normal, m);
        vec3 dl, sl;
        seed_for(pixel, frame, 3u);
        /* (a glossy lobe (roughness under REFERENCE_GLOSSY) takes the lamps'
        specular light by its own direction, the reflection's path meeting the
        lamp's face; a rougher one by next-event estimation, the lobe in the
        resampling's target: each lamp's specular light by one of the two, so
        nothing is counted twice nor left out. Round 1's references were
        noisiest at a10's glossy trim, where few candidate points of a lamp
        fall in a narrow lobe's highlight: a pixel's relative standard error
        near 1 at 1536 frames, its specular light most of its light) */
        bool glossy = g.roughness < REFERENCE_GLOSSY;
        lights_ris_seen(g, o, 8, 16, 16, !glossy, dl, sl);
        ld += dl;
        ls += sl;
        float y_direct = luma(kd * ld + ls);
        /* (one path a sample: the diffuse lobe's or the specular's, chosen by
        their shares (at least a tenth each), its light over its chance) */
        float chance = clamp(luma(share) / max(luma(share) + luma(kd), 1.0e-4), 0.1, 0.9);
        /* (a glossy pixel's lamp highlights come by its reflection's path alone:
        that lobe taken at least half the time) */
        if (glossy)
            chance = max(chance, 0.5);
        vec3 bounce = vec3(0.0), reflection = vec3(0.0);
        seed_for(pixel, frame, 6u);
        if (rnd() >= chance)
        {
            seed_for(pixel, frame, 4u);
            bounce = guided_bounce(g, o) / (1.0 - chance);
        }
        else
        {
            vec3 weight;
            seed_for(pixel, frame, 5u);
            vec3 r = ggx_sample(g, weight);
            if (dot(weight, weight) > 0.0)
                reflection = weight * reference_path(o, r, REFERENCE_BOUNCES, true, glossy) / chance;
        }
        if (any(isnan(bounce)) || any(isinf(bounce)))
            bounce = vec3(0.0);
        if (any(isnan(reflection)) || any(isinf(reflection)))
            reflection = vec3(0.0);
        ld += bounce;
        ls += reflection;
        if (any(isnan(ld)) || any(isinf(ld)))
            ld = vec3(0.0);
        if (any(isnan(ls)) || any(isinf(ls)))
            ls = vec3(0.0);
        ld_sum += ld;
        ls_sum += ls;
        float y = luma(kd * ld + ls);
        y_sum += y;
        y2_sum += y * y;
        parts_sum += vec3(y_direct, luma(kd * bounce), luma(reflection));
    }
    if (paths == 0u)
    {
        /* (the live estimate: the light pass's parts as composite takes them
        with the denoiser off) */
        vec3 sd, ss;
        sun_parts(g, sd, ss);
        float v = imageLoad(sun_raw, pixel).x;
        ld_sum = imageLoad(clean_diffuse, pixel).rgb + imageLoad(noisy_diffuse, pixel).rgb + sd * v;
        ls_sum = imageLoad(clean_specular, pixel).rgb + imageLoad(noisy_specular, pixel).rgb * share + ss * v;
        y_sum = luma(kd * ld_sum + ls_sum);
        parts_sum = vec3(y_sum, 0.0, 0.0);
        paths = 1u;
    }
    /* (this frame's paths as one sample of their mean: Welford's update of
    the running means and of the luminance's squared deviations) */
    vec3 ld = ld_sum / float(paths), ls = ls_sum / float(paths);
    float y = y_sum / float(paths);
    float fn = float(n);
    vec3 mean_d = n == 1u ? ld : vec3(sums.v[at], sums.v[at + 1u], sums.v[at + 2u]);
    vec3 mean_s = n == 1u ? ls : vec3(sums.v[at + 3u], sums.v[at + 4u], sums.v[at + 5u]);
    float mean_y = n == 1u ? 0.0 : sums.v[at + 6u], m2_y = n == 1u ? 0.0 : sums.v[at + 7u];
    if (n > 1u)
    {
        mean_d += (ld - mean_d) / fn;
        mean_s += (ls - mean_s) / fn;
    }
    float delta = y - mean_y;
    mean_y += delta / fn;
    m2_y += delta * (y - mean_y);
    sums.v[at] = mean_d.x;
    sums.v[at + 1u] = mean_d.y;
    sums.v[at + 2u] = mean_d.z;
    sums.v[at + 3u] = mean_s.x;
    sums.v[at + 4u] = mean_s.y;
    sums.v[at + 5u] = mean_s.z;
    sums.v[at + 6u] = mean_y;
    sums.v[at + 7u] = m2_y;
    sums.v[at + 8u] = g.distance;
    /* (the parts' means and squared deviations, likewise: where the noise
    is, written beside the whole's) */
    vec3 part = parts_sum / float(paths);
    for (uint k = 0u; k < 3u; k++)
    {
        float pm = n == 1u ? 0.0 : sums.v[at + 9u + 2u * k], p2 = n == 1u ? 0.0 : sums.v[at + 10u + 2u * k];
        float pd = part[k] - pm;
        pm += pd / fn;
        p2 += pd * (part[k] - pm);
        sums.v[at + 9u + 2u * k] = pm;
        sums.v[at + 10u + 2u * k] = p2;
    }
}

//@pass reference_stats
layout(local_size_x = 256) in;
/* the reference's noise: each workgroup's sums over its pixels into job.stats
(four floats a workgroup), the CPU adding them up: the variance of the
luminance's mean (the squared deviations over n (n - 1)) and the mean
squared, linear (the whole light, unclamped: the HDR estimate, logged); and
the same as the frame shows them, which the gates measure: the exposure on
the light (exposure_of, as composite), a pixel held at white or past it in
any channel of the drawn frame (the display's clip) taking no variance (its
drawn value does not move with its noise), the drawn luminance squared. The
CPU scales the second ratio by the gates' frame's share of a pixel's
variance (their frames brought to 960 x 540 by bilinear filtering). The
pixels' noises are independent (no reuse, a seed a pixel), so the
filtering's squared weights carry it. parameters.x the frames summed, y the
pixels */
shared vec4 partial[256];
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    uint index = gl_GlobalInvocationID.x;
    uint n = push.parameters.x;
    vec4 value = vec4(0.0);
    if (index < push.parameters.y && n > 1u)
    {
        Floats sums = Floats(job.sums);
        uint at = index * SUMS;
        float mean = sums.v[at + 6u], m2 = sums.v[at + 7u];
        float variance = max(m2, 0.0) / (float(n) * float(n - 1u));
        value.xy = vec2(variance, mean * mean);
        ivec2 pixel = ivec2(int(index % job.counts.y), int(index / job.counts.y));
        Surface g = surface_at(pixel);
        if (g.present)
        {
            bool metered = job.exposure.z > 0.5 && job.meter != uvec2(0u);
            float exposure = metered && job.exposure.x > 0.5 ? exposure_of(job.meter) : 1.0;
            vec3 share = specular_share(g);
            vec3 kd = g.albedo * (1.0 - share);
            vec3 light_ = vec3(sums.v[at], sums.v[at + 1u], sums.v[at + 2u]) * exposure;
            vec3 spec = vec3(sums.v[at + 3u], sums.v[at + 4u], sums.v[at + 5u]) * exposure;
            float brightest = max(light_.r, max(light_.g, light_.b));
            int curve = int(job.exposure.y + 0.5);
            bool held = curve == 0 && brightest > 1.0;
            vec3 drawn = kd * (held ? light_ / brightest : light_) + spec + g.emissive;
            /* (the drawn value through the tone curve; its slope where the
            curve rolls off scales the noise the frame shows, estimated by the
            luminance's curved value against a step of the variance's spread) */
            vec3 shown = tone_curved(drawn, curve);
            float y = luma(clamp(shown, vec3(0.0), vec3(1.0)));
            float sd = sqrt(variance) * exposure, ly = luma(drawn);
            float slope = ly > 1.0e-6 ? (luma(clamp(tone_curved(drawn * ((ly + sd) / ly), curve), vec3(0.0), vec3(1.0))) - y) /
                max(sd, 1.0e-9) : 1.0;
            bool clipped = held || max(shown.r, max(shown.g, shown.b)) >= 1.0;
            value.zw = vec2(clipped ? 0.0 : variance * exposure * exposure * slope * slope, y * y);
        }
    }
    partial[gl_LocalInvocationID.x] = value;
    barrier();
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (gl_LocalInvocationID.x < stride)
            partial[gl_LocalInvocationID.x] += partial[gl_LocalInvocationID.x + stride];
        barrier();
    }
    if (gl_LocalInvocationID.x == 0u)
    {
        Floats stats = Floats(job.stats);
        stats.v[gl_WorkGroupID.x * 4u] = partial[0].x;
        stats.v[gl_WorkGroupID.x * 4u + 1u] = partial[0].y;
        stats.v[gl_WorkGroupID.x * 4u + 2u] = partial[0].z;
        stats.v[gl_WorkGroupID.x * 4u + 3u] = partial[0].w;
    }
}

//@pass lamps
layout(local_size_x = 256) in;
/* the bake's lamps copied for the host (its light tree, xgpu_vulkan_pt.c):
job.lamps_out's first word their count (all of them: the host makes no tree
where they are more than the copy holds), then from its 16th each lamp's 16
floats (the bake's: its corners, radiance, area, cumulative power);
parameters.x the most the buffer holds */
layout(buffer_reference, std430, buffer_reference_align = 16) buffer LampsOut
{
    float v[];
};
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    uint index = gl_GlobalInvocationID.x;
    uint count = min(s.counts.z, push.parameters.x);
    LampsOut o = LampsOut(job.lamps_out);
    if (index == 0u)
        o.v[0] = uintBitsToFloat(s.counts.z);
    if (index >= count)
        return;
    Floats f = Floats(s.lamps);
    for (uint k = 0u; k < 16u; k++)
        o.v[16u + index * 16u + k] = f.v[index * 16u + k];
}

//@pass cache
layout(local_size_x = 256) in;
/* the radiance cache's cells take this frame's samples (the light pass's
sums: their mean folded into the cell's running mean, over at most
job.cache_settings.w samples); a cell no frame updated for CACHE_IDLE frames
let go */
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    uint cell = gl_GlobalInvocationID.x;
    if (cell >= CACHE_CELLS || job.cache == uvec2(0u))
        return;
    RadianceCache c = RadianceCache(job.cache);
    uint at = cell * CACHE_WORDS;
    if (c.v[at] == 0u)
        return;
    uint count = c.v[at + 4u], frame = job.frame.x;
    if (count == 0u)
    {
        if (frame - c.v[at + 5u] > CACHE_IDLE)
        {
            for (uint k = 0u; k < CACHE_WORDS; k++)
                c.v[at + k] = 0u;
        }
        return;
    }
    vec3 mean = vec3(c.v[at + 1u], c.v[at + 2u], c.v[at + 3u]) / (CACHE_FIXED * float(count));
    vec3 kept = vec3(uintBitsToFloat(c.v[at + 6u]), uintBitsToFloat(c.v[at + 7u]), uintBitsToFloat(c.v[at + 8u]));
    float samples = uintBitsToFloat(c.v[at + 9u]);
    float held = min(samples, max(job.cache_settings.w - float(count), 0.0));
    vec3 now = (kept * held + mean * float(count)) / (held + float(count));
    c.v[at + 1u] = 0u;
    c.v[at + 2u] = 0u;
    c.v[at + 3u] = 0u;
    c.v[at + 4u] = 0u;
    c.v[at + 5u] = frame;
    c.v[at + 6u] = floatBitsToUint(now.r);
    c.v[at + 7u] = floatBitsToUint(now.g);
    c.v[at + 8u] = floatBitsToUint(now.b);
    c.v[at + 9u] = floatBitsToUint(held + float(count));
}
