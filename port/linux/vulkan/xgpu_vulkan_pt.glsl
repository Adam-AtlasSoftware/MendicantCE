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

const float PI = 3.14159265;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Job
{
    uvec2 pages, charts, vpages, table, tiles, indirect, sun, masks;
    uvec2 bricks, coarse, probes, lightmaps, reflections, reflection_table, reflection_data, lights;
    uvec2 sums, stats, unused2[6];
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
};
Job job;

/* job.frame.y's flags */
const uint FLAG_HISTORY = 1u;
const uint FLAG_PERFORMANCE = 2u;
const uint FLAG_DENOISER_OFF = 4u;
const uint FLAG_REFERENCE = 8u;
const uint FLAG_TILES = 16u;
/* the reference's sums: floats a pixel (the diffuse light's mean, the
specular's, the luminance's mean and squared deviations, the distance) */
const uint SUMS = 12u;

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

Surface surface_at(ivec2 pixel)
{
    Surface g;
    uvec4 w = texelFetch(words, pixel, 0);
    uvec4 x = texelFetch(extras, pixel, 0);
    g.present = (w.w & 0x8000u) != 0u && uintBitsToFloat(x.w) > 0.0;
    g.instance = w.w >> 16;
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
float sun_of(uint vp, vec2 x)
{
    Uints v = Uints(job.vpages);
    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];
    ivec2 t = ivec2(floor(x / 16.0));
    if (any(lessThan(t, ivec2(0))) || t.x >= int(tiles_x) || t.y >= int(tiles_y))
        return -1.0;
    uint state = Uints(job.sun).v[v.v[vp * 8u + 4u] + uint(t.y) * tiles_x + uint(t.x)];
    if (state == 0u)
        return -1.0;
    if (state < 3u)
        return state == 2u ? 1.0 : 0.0;
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
muzzle flashes): 12 floats each (the place and reach, the colour by its
intensity, the spot's axis and its cone's cosine, 2 a point light), CE's
falloff 1 - d^2/r^2, as Raster has them; their shadows traced. */
vec3 dynamic_light(uint i, vec3 p, out vec3 l, out float d)
{
    Floats f = Floats(job.lights);
    uint at = i * 12u;
    vec3 to = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]) - p;
    float r = f.v[at + 3u], d2 = dot(to, to);
    d = sqrt(d2);
    l = to / max(d, 1.0e-6);
    if (r <= 0.0 || d2 >= r * r)
        return vec3(0.0);
    float cone = 1.0;
    vec4 axis = vec4(f.v[at + 8u], f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);
    if (axis.w < 1.5)
        cone = smoothstep(axis.w, mix(axis.w, 1.0, 0.2), dot(-l, axis.xyz));
    return vec3(f.v[at + 4u], f.v[at + 5u], f.v[at + 6u]) * (cone * (1.0 - d2 / (r * r)));
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

/* a lamp picked at p as the bake's lamp_pick picks it (the cell's seen list,
its near list, else by power: the same mixture, the same probability), but
each list searched by halves for the pick and the pick's own list's share
read at its place: a list is scanned only for the lamp's share in the other
(each lamp is in a list once) */
uint list_search(uint at, uint count, float u, out float share)
{
    Uints gr = Uints(s.grid);
    float total = uintBitsToFloat(gr.v[at + 2u * (count - 1u) + 2u]);
    float x = u * total;
    uint lo = 0u, hi = count - 1u;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2u;
        if (uintBitsToFloat(gr.v[at + 2u * mid + 2u]) < x)
            lo = mid + 1u;
        else
            hi = mid;
    }
    float below = lo > 0u ? uintBitsToFloat(gr.v[at + 2u * (lo - 1u) + 2u]) : 0.0;
    share = (uintBitsToFloat(gr.v[at + 2u * lo + 2u]) - below) / max(total, 1.0e-20);
    return gr.v[at + 2u * lo + 1u];
}
float list_share(uint at, uint count, uint l)
{
    Uints gr = Uints(s.grid);
    for (uint k = 0u; k < count; k++)
        if (gr.v[at + 2u * k + 1u] == l)
        {
            float total = uintBitsToFloat(gr.v[at + 2u * (count - 1u) + 2u]);
            float below = k > 0u ? uintBitsToFloat(gr.v[at + 2u * (k - 1u) + 2u]) : 0.0;
            return (uintBitsToFloat(gr.v[at + 2u * k + 2u]) - below) / max(total, 1.0e-20);
        }
    return 0.0;
}
uint lamp_pick_fast(vec3 p, out float probability)
{
    int cell = grid_cell(p);
    Uints gr = Uints(s.grid);
    uint seen_at = uint(max(cell, 0)), seen = cell >= 0 ? min(gr.v[seen_at], 64u) : 0u;
    uint near_at = seen_at + 1u + 2u * seen, near = cell >= 0 ? min(gr.v[near_at], 64u) : 0u;
    float share_seen = seen > 0u && !term_off(32u) ? GRID_SEEN : 0.0;
    float share_near = near > 0u && !term_off(32u) ? (seen > 0u ? GRID_NEAR : GRID_SEEN + GRID_NEAR) : 0.0;
    float u = rnd();
    uint l;
    float p_seen = -1.0, p_near = -1.0;
    if (u < share_seen)
        l = list_search(seen_at, seen, rnd(), p_seen);
    else if (u < share_seen + share_near)
        l = list_search(near_at, near, rnd(), p_near);
    else
        l = lamp_by_power();
    if (seen > 0u && p_seen < 0.0)
        p_seen = list_share(seen_at, seen, l);
    if (near > 0u && p_near < 0.0)
        p_near = list_share(near_at, near, l);
    probability = (1.0 - share_seen - share_near) * lamp_power(l) / max(s.lamp.x, 1.0e-20);
    if (seen > 0u)
        probability += share_seen * max(p_seen, 0.0);
    if (near > 0u)
        probability += share_near * max(p_near, 0.0);
    return l;
}

/* the lamps, the fixtures and the dynamic lights at the surface: resampled
importance over candidates (lamp_count from the grid's lamp_pick, the
fixtures' and the dynamic lights' by count), each its unshadowed diffuse and
specular light's luminance the target, one kept, its shadow traced from o:
the diffuse and the specular light, unbiased for any count of candidates
(each kind its own domain, its candidates' weight over its count) */
void lights_ris(Surface g, vec3 o, int lamp_count, int fixture_count, int dynamic_count, bool diffuse_only,
    out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    vec3 kept_d = vec3(0.0), kept_s = vec3(0.0), kept_l = vec3(0.0);
    float kept_t = 0.0, kept_dist = 0.0, sum = 0.0;
    int kept_kind = -1;
    vec3 p = g.point;
    bool lamps_on = s.counts.z > 0u && s.lamp.x > 0.0 && s.rig[10].w >= 0.5 && !term_off(2u);
    if (lamps_on)
    {
        Floats f = Floats(s.lamps);
        for (int c = 0; c < lamp_count; c++)
        {
            float probability;
            uint lamp = lamp_pick_fast(p, probability);
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
            if (dot(g.normal, l) <= 0.0)
                continue;
            vec3 dif, spe;
            parts(g, l, radiance * dot(g.normal, l), diffuse_only, dif, spe);
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
                kept_l = l;
                kept_dist = d;
                kept_kind = 0;
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
            /* (fixture_light: the irradiance's colour by the cosine over pi;
            n 0 gives the irradiance a surface facing it takes) */
            vec3 e = fixture_light(i, p, vec3(0.0), l, d);
            if (dot(g.normal, l) <= 0.0 || dot(e, e) <= 0.0)
                continue;
            vec3 dif, spe;
            parts(g, l, e * dot(g.normal, l), diffuse_only, dif, spe);
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
                kept_l = l;
                kept_dist = d;
                kept_kind = 1;
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
            if (nl <= 0.0 || dot(e, e) <= 0.0)
                continue;
            /* (CE's dynamic light: its colour by the cosine is the diffuse
            light in the bake's unit, as Raster adds it; pi of it the
            irradiance its specular takes) */
            vec3 dif = e * nl;
            vec3 spe = diffuse_only ? vec3(0.0) : ggx(g, l) * e * PI;
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
                kept_l = l;
                kept_dist = d;
                kept_kind = 2;
            }
        }
    }
    if (kept_kind < 0 || kept_t <= 0.0 || sum <= 0.0)
        return;
    bool seen = kept_kind == 0 ? lamp_visible(o, kept_l, kept_dist) :
        !blocked(o, kept_l, max(kept_dist - 0.05, 0.0));
    if (!seen)
        return;
    diffuse = kept_d / kept_t * sum;
    specular = kept_s / kept_t * sum;
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
        spe = ggx(g, l) * e * PI;
        return dot(e, e) > 0.0;
    }
    return false;
}

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
            uint lamp = lamp_pick_fast(p, probability);
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
            float target = luma(e * nl) + luma(ggx(g, l) * e * PI);
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
kept by its weight; its shadow traced; the reservoir kept for the next frame,
its weight 0 where its sample was hidden but its confidence counted (a
reused reservoir whose lamp was hidden still counts its candidates, so the
light does not brighten as it is reused) */
void lights_restir(ivec2 pixel, Surface g, vec3 o, bool reuse, out vec3 diffuse, out vec3 specular)
{
    diffuse = vec3(0.0);
    specular = vec3(0.0);
    int kind;
    uint index;
    vec3 y;
    float target_own, sum;
    lights_initial(g, int(job.extra.x), 8, 4, kind, index, y, target_own, sum);
    float w_cur = kind != KIND_NONE && target_own > 0.0 ? sum : 0.0;
    float m_total = 1.0;
    /* (the kept sample's weight sum, its target here) */
    float w_total = w_cur;
    float target_kept = target_own;
    if (reuse && g.has_history && (job.frame.y & FLAG_HISTORY) != 0u)
    {
        ivec2 q = ivec2(floor(vec2(pixel) + 0.5 + g.motion));
        if (inside(q))
        {
            uvec4 gl = imageLoad(guide_last, q);
            vec3 nl = unfold(unpackSnorm2x16(gl.y));
            float dl = uintBitsToFloat(gl.z);
            bool same = gl.x == (g.instance | 0x10000u) && dot(nl, g.normal) >= 0.9;
            if (same && g.instance == 0u)
                same = abs(dot(last_point(q, dl) - g.point, g.normal)) <= 0.02 + 0.004 * g.distance;
            else if (same)
                same = abs(dl - g.distance) <= 0.05 * g.distance + 0.05;
            if (same)
            {
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
                    if (w_last > 0.0 && !isinf(w_last))
                    {
                        w_total += w_last;
                        if (rnd() * w_total <= w_last)
                        {
                            kind = k_last;
                            index = i_last;
                            y = rp.xyz;
                            target_kept = target;
                        }
                    }
                }
                m_total += max(m_last, 0.0);
            }
        }
    }
    /* (W: the weights' sum over the kept target, by the confidences' sum:
    the initial reservoir's weights are already its candidates' mean) */
    float big_w = kind != KIND_NONE && target_kept > 0.0 ? w_total / (target_kept * m_total) : 0.0;
    vec3 dif = vec3(0.0), spe = vec3(0.0), l = vec3(0.0, 0.0, 1.0);
    float d = 0.0;
    bool seen = false;
    if (big_w > 0.0 && sample_light(g, kind, index, y, dif, spe, l, d))
    {
        seen = kind == KIND_LAMP ? lamp_visible(o, l, d) : !blocked(o, l, max(d - 0.05, 0.0));
        if (seen)
        {
            diffuse = dif * big_w;
            specular = spe * big_w;
        }
    }
    imageStore(reservoir_point_now, pixel, vec4(y, seen ? big_w : 0.0));
    imageStore(reservoir_info_now, pixel, uvec4(uint(kind) | (index << 2), floatBitsToUint(m_total), 0u, 0u));
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
vec3 hit_light(vec3 o, vec3 d, float t_max, bool domes, out float hit_distance)
{
    vec3 nh;
    uint t;
    vec2 b;
    hit_distance = -1.0;
    float th = hit(o, d, t_max, nh, t, b);
    if (th < 0.0)
        return sky_along(d, domes);
    hit_distance = th;
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
                total = max(tot - ambient(), vec3(0.0));
                sun_v = sv;
            }
        }
    }
    if (total.x < 0.0)
    {
        vec4 v;
        if (volume_at(h + nh * 0.05, nh, v))
        {
            total = max(v.rgb - ambient(), vec3(0.0));
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
    if (l.z <= 0.0)
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


/* the sun's ray from o toward a direction of its disc: 1 seen, 0 blocked,
and where blocked the distance to a blocker (the first the query meets, not
always the nearest: the penumbra's width for the filter) */
float sun_ray(vec3 o, out float blocker)
{
    vec3 d = cone_dir(s.sun.xyz, cos(0.5 * s.rig[0].w));
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(s.tlas), gl_RayFlagsTerminateOnFirstHitEXT, 15u, o, 0.0, d,
        2000.0);
    ALPHA_PROCEED(q)
    blocker = 0.0;
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return 1.0;
    blocker = rayQueryGetIntersectionTEXT(q, true);
    return 0.0;
}

/* the lamps' light at a path's hit (the bake's lamps_ris for a texel's
normal, its lamps picked by lamp_pick_fast: the same distribution, the
same estimate) */
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
        uint lamp = lamp_pick_fast(p, probability);
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

/* the reference's path (the bake's path(), but the domes taken at the first
ray's miss where the point did not take them as lights: a reflection's) */
vec3 reference_path(vec3 o, vec3 d, int bounces, bool domes_first)
{
    vec3 nh;
    uint t;
    vec2 b;
    float th = hit(o, d, 300.0, nh, t, b);
    if (th < 0.0)
        return sky_along(d, domes_first);
    if (dot(nh, nh) <= 0.0)
        return vec3(0.0);
    vec3 h = o + d * th;
    vec3 throughput = vec3(1.0), light = vec3(0.0);
    for (int bounce = 0; bounce < bounces && dot(nh, nh) > 0.0; bounce++)
    {
        light += throughput * glow_of(t);
        vec3 oh = h + nh * 0.03;
        vec3 lh;
        vec3 ls = sun_at(oh, nh) + hit_lamps(h, nh, oh, 4) + fixtures_ris(h, nh, oh, lh);
        throughput *= albedo_of(t);
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
        float t2 = hit(oh, wi, 300.0, nh, t, b);
        if (t2 < 0.0)
        {
            light += throughput * sky_along(wi, true);
            break;
        }
        h = oh + wi * t2;
    }
    return light;
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
    if (s.sun.w > 0.0 && dot(g.normal, s.sun.xyz) > 0.0)
    {
        if (!texel || bake_sun < 0.0 || g.distance < job.settings.x)
        {
            seed_for(pixel, frame, 1u);
            v_sun = sun_ray(o, blocker);
            traced = 1.0;
        }
        else
            v_sun = bake_sun;
    }
    imageStore(sun_raw, pixel, vec4(direct_on ? v_sun : 0.0, blocker, traced, 0.0));

    vec3 noisy_d = vec3(0.0), noisy_s = vec3(0.0), clean_d = vec3(0.0), clean_s = vec3(0.0);
    vec3 share = specular_share(g);
    if (direct_on)
    {
        /* the domes, the lamps, the fixtures, the dynamic lights */
        vec3 m = vec3(0.0);
        seed_for(pixel, frame, 2u);
        noisy_d += domes_at(o, g.normal, m);
        vec3 ld, ls;
        seed_for(pixel, frame, 3u);
        lights_restir(pixel, g, o, job.extra.z > 0.0, ld, ls);
        noisy_d += ld;
        noisy_s += ls;
        /* the sky's floor (the units' rig: unoccluded, as the bake adds it) */
        clean_d += ambient();
    }
    if (indirect_on)
    {
        /* the indirect light: the bake's at the pixel, else a bounce */
        if (performance && texel)
            clean_d += bake_indirect;
        else
        {
            float hd;
            seed_for(pixel, frame, 4u);
            noisy_d += hit_light(o, cosine_dir(g.normal), 300.0, false, hd);
        }
        /* the reflections: a GGX ray below the roughness, else the probes */
        if (g.roughness <= job.settings.y)
        {
            vec3 weight;
            seed_for(pixel, frame, 5u);
            vec3 r = ggx_sample(g, weight);
            float hd;
            if (dot(weight, weight) > 0.0)
                noisy_s += weight * hit_light(o, r, 300.0, true, hd);
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
}

//@pass temporal
layout(local_size_x = 8, local_size_y = 8) in;
/* the last frame's histories at a pixel of this frame: the motion's point
there, its 2 x 2 taps each kept where its guide is the surface's (the same
instance, the normal within 25 degrees, the plane through it near the
point: no history across an edge, nor through an opening onto a surface
behind), bilinear over the kept ones */
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
    vec2 at = vec2(pixel) + 0.5 + g.motion - 0.5;
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
    vec4 nd = imageLoad(noisy_diffuse, pixel);
    vec4 ns = imageLoad(noisy_specular, pixel);
    vec4 sr = imageLoad(sun_raw, pixel);
    vec4 hd, hs, hm, hms, hsun;
    float length_d;
    bool history = reproject(pixel, g, hd, hs, hm, hms, hsun, length_d);
    float most_d = job.denoise.x, most_fast = job.denoise.y;
    /* (the specular's history shorter where the surface is glossy: a
    reflection moves over it as the eye does) */
    float most_s = mix(4.0, most_d, clamp(g.roughness * 2.0, 0.0, 1.0));

    /* fireflies: the new sample's luminance held to its 3 x 3 neighbours'
    brightest (the centre left out) times firefly, and, once the history is
    long, to the history's mean and spread */
    float brightest_d = 0.0, brightest_s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            if ((x == 0 && y == 0) || !inside(q))
                continue;
            brightest_d = max(brightest_d, imageLoad(noisy_diffuse, q).a);
            brightest_s = max(brightest_s, imageLoad(noisy_specular, q).a);
        }
    float firefly = job.settings.z;
    float ld = nd.a, ls = ns.a;
    float cap_d = brightest_d * firefly + 1.0e-4, cap_s = brightest_s * firefly + 1.0e-4;
    if (history && length_d >= 8.0)
    {
        float sigma = sqrt(max(hm.y - hm.x * hm.x, 0.0));
        cap_d = min(cap_d, max(hm.x + 4.0 * sigma, 1.0e-4) * 2.0);
    }
    vec3 cd = ld > cap_d ? nd.rgb * (cap_d / ld) : nd.rgb;
    vec3 cs = ls > cap_s ? ns.rgb * (cap_s / ls) : ns.rgb;
    ld = luma(cd);
    ls = luma(cs);

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
    float sigma_f = sqrt(max(fast_m2 - fast_d * fast_d, 0.0) / n_f);
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
    float sigma_fs = sqrt(max(fast_m2s - fast_s * fast_s, 0.0) / n_fs);
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
    imageStore(diff_now, pixel, vec4(slow_d, n_d));
    imageStore(spec_now, pixel, vec4(slow_s, n_s));
    imageStore(moments_now, pixel, vec4(m1, m2, fast_d, fast_m2));
    imageStore(spec_moments_now, pixel, vec4(fast_s, fast_m2s, 0.0, 0.0));
    /* (the variance for the spatial filter: the moments' once the history
    has a few frames; else unknown (the fix pass and the filter take it from
    the neighbours)) */
    float var_d = n_d >= 4.0 ? variance : -1.0;
    imageStore(diff_a, pixel, vec4(slow_d, var_d));
    imageStore(spec_a, pixel, vec4(slow_s, n_s));

    /* the sun's visibility: its own history (at most most_d frames), its
    fast mean, its second moment */
    float n_v = history ? min(hsun.w + 1.0, most_d) : 1.0;
    float v = sr.x;
    float slow_v = history ? mix(hsun.x, v, 1.0 / n_v) : v;
    float fast_v = history ? mix(hsun.y, v, 1.0 / min(n_v, most_fast)) : v;
    float m2v = history ? mix(hsun.z, v * v, 1.0 / n_v) : v * v;
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
where it is unknown, from the 5 x 5 neighbourhood's luminance */
void main()
{
    s = Scene(push.scene);
    job = Job(push.job);
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (!inside(pixel))
        return;
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
        /* (a short history's variance the larger: its noise is a few
        samples') */
        od.a = spatial * 4.0 / max(n_d, 1.0);
    }
    /* (the specular's variance: its spread over the 3 x 3, scaled by its
    history) */
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
        os.a = max(s2 / max(sw, 1.0) - mean * mean, 0.0) * 4.0 / max(n_s, 1.0);
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
    float lc = luma(dc.rgb), lsc = luma(sc.rgb);
    float sigma_d = job.settings.w * sqrt(max(dc.a, 0.0)) + 1.0e-4;
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
    bool sun_on = (signals & 4u) != 0u && vc.w > 0.5 && float(stride) <= max(penumbra, 2.0) &&
        vc.y > 1.0e-4;
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
    vec3 result = kd * diffuse_light + specular_light + g.emissive;
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
to 8 bounces, Russian roulette past the second, every hit's sun and lamps
by next-event estimation), the reflection by a GGX ray and the same path
(one of the two lobes a sample, by their shares); extra.y of these a pixel
each frame, summed (Welford's running mean, so
that a long sum keeps its precision) into job.sums: per pixel the diffuse
light's mean (3), the specular light's (3), and the mean and the sum of
squared deviations of the frame's luminance there (its noise:
reference_stats). parameters.x this frame's number in the sum (1 on) */
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
    uint paths = max(uint(job.extra.y), 1u);
    vec3 ld_sum = vec3(0.0), ls_sum = vec3(0.0);
    float y_sum = 0.0, y2_sum = 0.0;
    for (uint k = 0u; k < paths; k++)
    {
        uint frame = n * 64u + k;
        vec3 ld = ambient(), ls = vec3(0.0);
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
        lights_ris(g, o, 8, 16, 16, false, dl, sl);
        ld += dl;
        ls += sl;
        /* (one path a sample: the diffuse lobe's or the specular's, chosen by
        their shares (at least a tenth each), its light over its chance) */
        float chance = clamp(luma(share) / max(luma(share) + luma(kd), 1.0e-4), 0.1, 0.9);
        seed_for(pixel, frame, 6u);
        if (rnd() >= chance)
        {
            seed_for(pixel, frame, 4u);
            ld += reference_path(o, cosine_dir(g.normal), 8, false) / (1.0 - chance);
        }
        else
        {
            vec3 weight;
            seed_for(pixel, frame, 5u);
            vec3 r = ggx_sample(g, weight);
            if (dot(weight, weight) > 0.0)
                ls += weight * reference_path(o, r, 8, true) / chance;
        }
        if (any(isnan(ld)) || any(isinf(ld)))
            ld = vec3(0.0);
        if (any(isnan(ls)) || any(isinf(ls)))
            ls = vec3(0.0);
        ld_sum += ld;
        ls_sum += ls;
        float y = luma(kd * ld + ls);
        y_sum += y;
        y2_sum += y * y;
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
}

//@pass reference_stats
layout(local_size_x = 256) in;
/* the reference's noise: each workgroup's sum over its pixels of the
variance of their luminance's mean (the squared deviations over n (n - 1))
and of the mean squared, into job.stats (two floats a workgroup); the CPU
adds them: their ratio is the frame's estimated relative MSE.
parameters.x the frames summed, y the pixels */
shared vec2 partial[256];
void main()
{
    job = Job(push.job);
    uint index = gl_GlobalInvocationID.x;
    uint n = push.parameters.x;
    vec2 value = vec2(0.0);
    if (index < push.parameters.y && n > 1u)
    {
        Floats sums = Floats(job.sums);
        float mean = sums.v[index * SUMS + 6u], m2 = sums.v[index * SUMS + 7u];
        value = vec2(max(m2, 0.0) / (float(n) * float(n - 1u)), mean * mean);
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
        stats.v[gl_WorkGroupID.x * 2u] = partial[0].x;
        stats.v[gl_WorkGroupID.x * 2u + 1u] = partial[0].y;
    }
}
