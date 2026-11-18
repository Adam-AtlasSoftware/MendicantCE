/* PRISM v4's in-game baker's compute passes (xgpu_vulkan_bake.c;
docs/prism_bake.md), and PRISM Raster's lighting pass, in Vulkan's GLSL:
tools/vulkan_bake_shaders.py makes xgpu_vulkan_bake_shaders.c of them. Not
compiled into the game.

Every buffer is reached by its device address (GL_EXT_buffer_reference),
the scene's acceleration structure too; the passes' push constants name the
scene's block (struct bake_scene_block in xgpu_vulkan_bake.c, field for
field) and the pass's own. The estimator is the path tracer's reference's
(xgpu_vulkan_rt_glsl.inc's path pass with debug.pt_reference: no reuse, no
cache, no probes, up to 8 bounces, Russian roulette past the second), on
the same scene (SCENE_STATIC's level as drawn, its near level), materials
(display.prism_rt's: the mean albedo, the alpha maps, the glows) and light
rig (RT_FRAME's: the sun, the sky, the domes, the lamps), so that the bake
is the reference's light, converged:

  the light a point takes, less the sun's (the rig's linear units, a white
  surface's radiance: the irradiance over pi), at its normal:
    the direct light: the domes (a direction of each one's cone, its shadow
    traced) and the lamps (a lamp's point picked from the light-importance
    grid's cell about the point, the rest by power, its shadow traced);
    the indirect light: a cosine-distributed path, the sky where it leaves
    the level, at each hit the hit's glow and its albedo by the sun's and a
    lamp's light there and the path's next bounce, the sky and the domes
    where it leaves;
  the sun's visibility from the point apart (a direction of its disc).

The direction the light comes from is kept beside it (the first moment of
the incoming light's luminance, an SH L1 band), so that the light can be
read at another normal than the one it was baked at (a remodel's curved
face, a bump map's normal): B(n) = B(n0) (a0 + k . n), k the moment's share
over 2 pi, a0 = 1 - k . n0. */

#define BAKE_COMMON \
	"#version 460\n" \
	"#extension GL_EXT_ray_query : require\n" \
	"#extension GL_EXT_buffer_reference : require\n" \
	"#extension GL_EXT_buffer_reference_uvec2 : require\n" \
	"#extension GL_KHR_shader_subgroup_shuffle : require\n" \
	"layout(buffer_reference, std430, buffer_reference_align = 4) buffer Floats { float v[]; };\n" \
	"layout(buffer_reference, std430, buffer_reference_align = 4) buffer Uints { uint v[]; };\n" \
	"layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Scene\n" \
	"{\n" \
	"    uvec2 tlas, vertices, info, materials, alpha, lamps, grid, unused;\n" \
	"    uvec4 counts;\n" \
	"    vec4 sun;\n" \
	"    vec4 rig[16];\n" \
	"    vec4 lamp;\n" \
	"    uvec4 settings;\n" \
	"};\n" \
	"layout(push_constant) uniform Push\n" \
	"{\n" \
	"    uvec2 scene;\n" \
	"    uvec2 job;\n" \
	"    uvec4 parameters;\n" \
	"    vec4 values;\n" \
	"} push;\n" \
	"Scene s;\n" \
	"\n" \
	"float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n" \
	"\n" \
	"uint seed;\n" \
	"uint hash_u(uint v)\n" \
	"{\n" \
	"    uint state = v * 747796405u + 2891336453u;\n" \
	"    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;\n" \
	"    return (word >> 22u) ^ word;\n" \
	"}\n" \
	"float rnd()\n" \
	"{\n" \
	"    seed = seed * 747796405u + 2891336453u;\n" \
	"    uint w = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;\n" \
	"    w = (w >> 22u) ^ w;\n" \
	"    return float(w & 0xffffffu) / 16777216.0;\n" \
	"}\n" \
	"\n" \
	"/* the scene's triangles: the opaque (instance 0) then the alpha-tested\n" \
	"(instance 1); each's info: its material (display.prism_rt's, the\n" \
	"materials' count where none) and flags (bit 16 two-sided: a remodel's),\n" \
	"its corners' base map coordinates */\n" \
	"uint scene_triangle(uint custom, uint primitive)\n" \
	"{\n" \
	"    return custom == 0u ? primitive : custom == 1u ? primitive + s.counts.x : custom == 2u ?\n" \
	"        primitive + s.counts.x + s.counts.y : primitive + s.counts.x + s.counts.y + s.settings.w;\n" \
	"}\n" \
	"vec3 corner(uint t, uint k)\n" \
	"{\n" \
	"    Floats f = Floats(s.vertices);\n" \
	"    uint b = (t * 3u + k) * 3u;\n" \
	"    return vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);\n" \
	"}\n" \
	"uint material_of(uint t) { return min(Uints(s.info).v[t * 8u] & 0xffffu, s.counts.w); }\n" \
	"bool two_sided(uint t) { return (Uints(s.info).v[t * 8u] & 0x10000u) != 0u; }\n" \
	"/* (a structure triangle in one of its indoor clusters, one with no sky:\n" \
	"xgpu_stream.h's XGPU_SCENE_INDOOR) */\n" \
	"bool indoor(uint t) { return (Uints(s.info).v[t * 8u] & 0x20000u) != 0u; }\n" \
	"float material_value(uint m, uint k) { return Floats(s.materials).v[m * 16u + k]; }\n" \
	"\n" \
	"bool alpha_kept(uint custom, uint primitive, vec2 b)\n" \
	"{\n" \
	"    /* (the static scenery's cutouts: a hit kept by its share of texels\n" \
	"    drawn, as the reference's objects' are) */\n" \
	"    if (custom == 3u)\n" \
	"        return rnd() < 0.75 * material_value(material_of(scene_triangle(3u, primitive)), 7u);\n" \
	"    uint t = primitive + s.counts.x;\n" \
	"    uint m = material_of(t);\n" \
	"    if (m >= s.counts.w || (uint(material_value(m, 3u)) & 2u) == 0u)\n" \
	"        return true;\n" \
	"    Uints a = Uints(s.info);\n" \
	"    vec2 uv0 = uintBitsToFloat(uvec2(a.v[t * 8u + 1u], a.v[t * 8u + 2u]));\n" \
	"    vec2 uv1 = uintBitsToFloat(uvec2(a.v[t * 8u + 3u], a.v[t * 8u + 4u]));\n" \
	"    vec2 uv2 = uintBitsToFloat(uvec2(a.v[t * 8u + 5u], a.v[t * 8u + 6u]));\n" \
	"    vec2 uv = uv0 * (1.0 - b.x - b.y) + uv1 * b.x + uv2 * b.y;\n" \
	"    float w = material_value(m, 9u), h = material_value(m, 10u);\n" \
	"    uvec2 texel = uvec2(clamp(fract(uv) * vec2(w, h), vec2(0.0), vec2(w, h) - 1.0));\n" \
	"    uint at = uint(material_value(m, 8u)) + texel.y * uint(w) + texel.x;\n" \
	"    uint alpha = (Uints(s.alpha).v[at >> 2u] >> ((at & 3u) * 8u)) & 0xffu;\n" \
	"    return float(alpha) * (1.0 / 255.0) > material_value(m, 7u);\n" \
	"}\n" \
	"\n" \
	"#define ALPHA_PROCEED(q) while (rayQueryProceedEXT(q)) { \\\n" \
	"        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && \\\n" \
	"            alpha_kept(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false), \\\n" \
	"            uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), \\\n" \
	"            rayQueryGetIntersectionBarycentricsEXT(q, false))) \\\n" \
	"            rayQueryConfirmIntersectionEXT(q); }\n" \
	"\n" \
	"bool blocked(vec3 o, vec3 d, float t_max)\n" \
	"{\n" \
	"    rayQueryEXT q;\n" \
	"    rayQueryInitializeEXT(q, accelerationStructureEXT(s.tlas), gl_RayFlagsTerminateOnFirstHitEXT, 15u, o, 0.0, d,\n" \
	"        t_max);\n" \
	"    ALPHA_PROCEED(q)\n" \
	"    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;\n" \
	"}\n" \
	"\n" \
	"/* the nearest hit: t (negative none), its face's normal toward the ray (0\n" \
	"where it is a one-sided face's back: inside the level), its triangle */\n" \
	"float hit(vec3 o, vec3 d, float t_max, out vec3 nh, out uint t, out vec2 bary)\n" \
	"{\n" \
	"    rayQueryEXT q;\n" \
	"    rayQueryInitializeEXT(q, accelerationStructureEXT(s.tlas), 0u, 15u, o, 0.0, d, t_max);\n" \
	"    ALPHA_PROCEED(q)\n" \
	"    nh = vec3(0.0);\n" \
	"    t = 0u;\n" \
	"    bary = vec2(0.0);\n" \
	"    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)\n" \
	"        return -1.0;\n" \
	"    t = scene_triangle(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true),\n" \
	"        uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)));\n" \
	"    bary = rayQueryGetIntersectionBarycentricsEXT(q, true);\n" \
	"    vec3 a0 = corner(t, 0u), a1 = corner(t, 1u), a2 = corner(t, 2u);\n" \
	"    vec3 n = cross(a1 - a0, a2 - a0);\n" \
	"    n = dot(n, n) > 1.0e-20 ? normalize(n) : -d;\n" \
	"    if (dot(n, d) > 0.0)\n" \
	"        n = two_sided(t) ? -n : vec3(0.0);\n" \
	"    nh = n;\n" \
	"    return rayQueryGetIntersectionTEXT(q, true);\n" \
	"}\n" \
	"\n" \
	"vec3 albedo_of(uint t)\n" \
	"{\n" \
	"    if (s.rig[11].y < 0.5)\n" \
	"        return vec3(0.45);\n" \
	"    uint m = material_of(t);\n" \
	"    if (m >= s.counts.w)\n" \
	"        return vec3(0.35);\n" \
	"    return vec3(material_value(m, 0u), material_value(m, 1u), material_value(m, 2u));\n" \
	"}\n" \
	"/* (the terms a diagnostic leaves out: s.settings.y's bits, 1 the domes, 2\n" \
	"the lamps, 4 the glows, 8 the paths past the first hit, 16 the fixtures;\n" \
	"32 the grid (every lamp picked by power), 64 the lamps' spherical\n" \
	"sampling (by area alone); a texel's: 128 its indirect light (its paths),\n" \
	"256 its own lamps' direct light; 512 every lamp of its cell's lists even\n" \
	"without the gather, 1024 the resampled pick even with it; 0 none) */\n" \
	"bool term_off(uint bit) { return (s.settings.y & bit) != 0u; }\n" \
	"vec3 glow_of(uint t)\n" \
	"{\n" \
	"    if (s.rig[10].w < 1.5 || term_off(4u))\n" \
	"        return vec3(0.0);\n" \
	"    uint m = material_of(t);\n" \
	"    if (m >= s.counts.w || (uint(material_value(m, 3u)) & 4u) == 0u)\n" \
	"        return vec3(0.0);\n" \
	"    return vec3(material_value(m, 11u), material_value(m, 12u), material_value(m, 13u)) * s.rig[11].x;\n" \
	"}\n" \
	"\n" \
	"void basis(vec3 n, out vec3 t, out vec3 b)\n" \
	"{\n" \
	"    t = normalize(abs(n.z) < 0.95 ? cross(n, vec3(0.0, 0.0, 1.0)) : cross(n, vec3(1.0, 0.0, 0.0)));\n" \
	"    b = cross(n, t);\n" \
	"}\n" \
	"vec3 cosine_dir(vec3 n)\n" \
	"{\n" \
	"    vec3 t, b;\n" \
	"    basis(n, t, b);\n" \
	"    float u = rnd(), a = rnd() * 6.2831853;\n" \
	"    return normalize(sqrt(u) * (cos(a) * t + sin(a) * b) + sqrt(max(1.0 - u, 0.0)) * n);\n" \
	"}\n" \
	"vec3 cone_dir(vec3 axis, float cos_most)\n" \
	"{\n" \
	"    vec3 t, b;\n" \
	"    basis(axis, t, b);\n" \
	"    float c = mix(1.0, cos_most, rnd()), sn = sqrt(max(1.0 - c * c, 0.0)), a = rnd() * 6.2831853;\n" \
	"    return normalize(c * axis + sn * (cos(a) * t + sin(a) * b));\n" \
	"}\n" \
	"vec3 sphere_dir()\n" \
	"{\n" \
	"    float z = rnd() * 2.0 - 1.0, a = rnd() * 6.2831853, r = sqrt(max(1.0 - z * z, 0.0));\n" \
	"    return vec3(r * cos(a), r * sin(a), z);\n" \
	"}\n" \
	"\n" \
	"/* a sky light's rays (the sun's, a dome's) tested as Tool's radiosity\n" \
	"tests them: as far as the light's test distance (the rig's: rig[15].w the\n" \
	"sun's, a dome's its colour's w; 0 the whole level), and blocked by a face\n" \
	"met from its front or a two-sided one, not by a one-sided face's back */\n" \
	"float sun_reach() { return s.rig[15].w > 0.0 ? s.rig[15].w : 2000.0; }\n" \
	"float dome_reach(vec4 r) { return r.w > 0.0 ? r.w : 2000.0; }\n" \
	"bool sky_blocked(vec3 o, vec3 d, float reach)\n" \
	"{\n" \
	"    for (int k = 0; k < 4; k++)\n" \
	"    {\n" \
	"        vec3 nh;\n" \
	"        uint t;\n" \
	"        vec2 b;\n" \
	"        float h = hit(o, d, reach, nh, t, b);\n" \
	"        if (h < 0.0)\n" \
	"            return false;\n" \
	"        if (dot(nh, nh) > 0.0)\n" \
	"            return true;\n" \
	"        o += d * (h + 0.01);\n" \
	"        reach -= h + 0.01;\n" \
	"        if (reach <= 0.0)\n" \
	"            return false;\n" \
	"    }\n" \
	"    return true;\n" \
	"}\n" \
	"/* the sun's visibility from o (a direction of its disc) */\n" \
	"float sun_visible(vec3 o)\n" \
	"{\n" \
	"    if (s.sun.w <= 0.0)\n" \
	"        return 0.0;\n" \
	"    vec3 d = cone_dir(s.sun.xyz, cos(0.5 * s.rig[0].w));\n" \
	"    return sky_blocked(o, d, sun_reach()) ? 0.0 : 1.0;\n" \
	"}\n" \
	"vec3 sun_at(vec3 o, vec3 n)\n" \
	"{\n" \
	"    float c = dot(n, s.sun.xyz);\n" \
	"    if (s.sun.w <= 0.0 || c <= 0.0)\n" \
	"        return vec3(0.0);\n" \
	"    return s.rig[0].rgb * c * sun_visible(o);\n" \
	"}\n" \
	"\n" \
	"/* the domes' light at a point (each a direction of its cone, its shadow\n" \
	"traced); the direction kept, for the moment (each's light over its cosine) */\n" \
	"vec3 domes_at(vec3 o, vec3 n, inout vec3 moment)\n" \
	"{\n" \
	"    vec3 sum = vec3(0.0);\n" \
	"    if (term_off(1u))\n" \
	"        return sum;\n" \
	"    for (int i = 0; i < 4; i++)\n" \
	"    {\n" \
	"        vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];\n" \
	"        if (dot(r.rgb, vec3(1.0)) <= 0.0)\n" \
	"            continue;\n" \
	"        vec3 d = cone_dir(normalize(a.xyz), a.w);\n" \
	"        float c = dot(n, d);\n" \
	"        if (c > 0.0 && !sky_blocked(o, d, dome_reach(r)))\n" \
	"        {\n" \
	"            sum += r.rgb * c;\n" \
	"            moment += luma(r.rgb) * d;\n" \
	"        }\n" \
	"    }\n" \
	"    return sum;\n" \
	"}\n" \
	"/* the rig in the light's units (s.rig[15].y): the sky's ambient is CE's\n" \
	"flat floor of light, unoccluded, at every texel and probe: its outdoor\n" \
	"values in the structure's clusters with a sky (ambient()), sky 0's indoor\n" \
	"ones (s.rig[14].yzw: ambient_indoor()) in those with none (indoor(),\n" \
	"floor_at()); not a radiance from the open sky, and not passed on by the\n" \
	"bounces */\n" \
	"bool units_rig() { return s.rig[15].y > 0.5; }\n" \
	"vec3 ambient() { return units_rig() ? s.rig[1].rgb : vec3(0.0); }\n" \
	"vec3 ambient_indoor() { return units_rig() ? s.rig[14].yzw : vec3(0.0); }\n" \
	"/* 1 where a point in the air is in an outdoor cluster, 0 in an indoor\n" \
	"one: the nearest of the structure's surfaces the six axes meet says which,\n" \
	"by its triangle's flag (nothing met: outdoor) */\n" \
	"float floor_at(vec3 p)\n" \
	"{\n" \
	"    float nearest = 1.0e30, weight = 1.0;\n" \
	"    for (int a = 0; a < 6; a++)\n" \
	"    {\n" \
	"        vec3 d = vec3(0.0);\n" \
	"        d[a >> 1] = (a & 1) == 0 ? 1.0 : -1.0;\n" \
	"        vec3 nh;\n" \
	"        uint t;\n" \
	"        vec2 b;\n" \
	"        float h = hit(p, d, 64.0, nh, t, b);\n" \
	"        if (h >= 0.0 && h < nearest && t < s.counts.x + s.counts.y)\n" \
	"        {\n" \
	"            nearest = h;\n" \
	"            weight = indoor(t) ? 0.0 : 1.0;\n" \
	"        }\n" \
	"    }\n" \
	"    return weight;\n" \
	"}\n" \
	"vec3 sky_along(vec3 d, bool domes)\n" \
	"{\n" \
	"    vec3 l = units_rig() ? vec3(0.0) : s.rig[1].rgb;\n" \
	"    if (!domes)\n" \
	"        return l;\n" \
	"    for (int i = 0; i < 4; i++)\n" \
	"    {\n" \
	"        vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];\n" \
	"        if (dot(r.rgb, vec3(1.0)) > 0.0 && dot(d, normalize(a.xyz)) >= a.w)\n" \
	"            l += r.rgb * 3.14159265 / max(6.2831853 * (1.0 - a.w), 1.0e-4);\n" \
	"    }\n" \
	"    return l;\n" \
	"}\n" \
	"\n" \
	"/* the light-importance grid (xgpu_vulkan_bake.c's importance step): a\n" \
	"hashed table of cells (s.lamp.y world units a side) about the level's\n" \
	"surfaces, each its key (x y z + 1 << 20 each, packed; 0 empty) and where\n" \
	"its lists are in the same buffer (3 words a cell), after the table: two\n" \
	"lists of up to 64 lamps (a count, then each lamp and the cumulative weight\n" \
	"to it): the seen, by the light each gives the cell's points with its\n" \
	"shadow traced, and the near, by its unshadowed light at the cell's middle\n" \
	"(the lamps the cell's few points did not see that its other points may).\n" \
	"A lamp is picked from the seen by GRID_SEEN of the picks, the near by\n" \
	"GRID_NEAR (all but GRID_POWER where the cell saw none), else by power over\n" \
	"the level: every lamp can be picked, so the bake's lamps are R's, unbiased */\n" \
	"const uint BAKE_GRID_WORDS = 3u;\n" \
	"const float GRID_SEEN = 0.70, GRID_NEAR = 0.25;\n" \
	"int grid_cell(vec3 p)\n" \
	"{\n" \
	"    uint entries = s.settings.z;\n" \
	"    if (entries == 0u || s.lamp.y <= 0.0)\n" \
	"        return -1;\n" \
	"    ivec3 c = ivec3(floor(p / s.lamp.y)) + ivec3(1 << 20);\n" \
	"    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(1 << 21))))\n" \
	"        return -1;\n" \
	"    uvec2 key = uvec2(uint(c.x) | (uint(c.y) << 21), (uint(c.y) >> 11) | (uint(c.z) << 10) | 0x80000000u);\n" \
	"    uint h = hash_u(key.x ^ hash_u(key.y)) & (entries - 1u);\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    for (uint probe = 0u; probe < 32u; probe++)\n" \
	"    {\n" \
	"        uint at = ((h + probe) & (entries - 1u)) * BAKE_GRID_WORDS;\n" \
	"        uint k1 = g.v[at + 1u];\n" \
	"        if (k1 == 0u)\n" \
	"            return -1;\n" \
	"        if (g.v[at] == key.x && k1 == key.y)\n" \
	"            return int(g.v[at + 2u]);\n" \
	"    }\n" \
	"    return -1;\n" \
	"}\n" \
	"\n" \
	"/* a lamp picked by power (the cumulative power's search) */\n" \
	"uint lamp_by_power()\n" \
	"{\n" \
	"    Floats f = Floats(s.lamps);\n" \
	"    float u = rnd() * s.lamp.x;\n" \
	"    uint lo = 0u, hi = s.counts.z - 1u;\n" \
	"    while (lo < hi)\n" \
	"    {\n" \
	"        uint mid = (lo + hi) / 2u;\n" \
	"        if (f.v[mid * 16u + 13u] < u)\n" \
	"            lo = mid + 1u;\n" \
	"        else\n" \
	"            hi = mid;\n" \
	"    }\n" \
	"    return lo;\n" \
	"}\n" \
	"float lamp_power(uint l)\n" \
	"{\n" \
	"    Floats f = Floats(s.lamps);\n" \
	"    return luma(vec3(f.v[l * 16u + 9u], f.v[l * 16u + 10u], f.v[l * 16u + 11u])) * f.v[l * 16u + 12u];\n" \
	"}\n" \
	"/* a lamp from a cell's list (its first word the count, then lamp and\n" \
	"cumulative weight pairs, by the lamps' ids) by its weights (a binary\n" \
	"search of the cumulative weights); its probability there (a binary search\n" \
	"of the ids) */\n" \
	"uint list_pick(uint at, uint count)\n" \
	"{\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    float u = rnd() * uintBitsToFloat(g.v[at + 2u * (count - 1u) + 2u]);\n" \
	"    uint lo = 0u, hi = count - 1u;\n" \
	"    while (lo < hi)\n" \
	"    {\n" \
	"        uint mid = (lo + hi) / 2u;\n" \
	"        if (uintBitsToFloat(g.v[at + 2u * mid + 2u]) < u)\n" \
	"            lo = mid + 1u;\n" \
	"        else\n" \
	"            hi = mid;\n" \
	"    }\n" \
	"    return g.v[at + 2u * lo + 1u];\n" \
	"}\n" \
	"float list_probability(uint at, uint count, uint l)\n" \
	"{\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    uint lo = 0u, hi = count;\n" \
	"    while (lo < hi)\n" \
	"    {\n" \
	"        uint mid = (lo + hi) / 2u;\n" \
	"        if (g.v[at + 2u * mid + 1u] < l)\n" \
	"            lo = mid + 1u;\n" \
	"        else\n" \
	"            hi = mid;\n" \
	"    }\n" \
	"    if (lo >= count || g.v[at + 2u * lo + 1u] != l)\n" \
	"        return 0.0;\n" \
	"    float below = lo > 0u ? uintBitsToFloat(g.v[at + 2u * lo]) : 0.0;\n" \
	"    return (uintBitsToFloat(g.v[at + 2u * lo + 2u]) - below) /\n" \
	"        max(uintBitsToFloat(g.v[at + 2u * (count - 1u) + 2u]), 1.0e-20);\n" \
	"}\n" \
	"/* a lamp picked at p (the cell's seen, its near, else by power) and the\n" \
	"probability of its pick, the three densities' mixture */\n" \
	"uint lamp_pick(vec3 p, out float probability)\n" \
	"{\n" \
	"    int cell = grid_cell(p);\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    uint seen_at = uint(max(cell, 0)), seen = cell >= 0 ? min(g.v[seen_at], 64u) : 0u;\n" \
	"    uint near_at = seen_at + 1u + 2u * seen, near = cell >= 0 ? min(g.v[near_at], 64u) : 0u;\n" \
	"    /* (HALO_BAKE_TERMS' 32: every pick by power, the grid unused) */\n" \
	"    float share_seen = seen > 0u && !term_off(32u) ? GRID_SEEN : 0.0;\n" \
	"    float share_near = near > 0u && !term_off(32u) ? (seen > 0u ? GRID_NEAR : GRID_SEEN + GRID_NEAR) : 0.0;\n" \
	"    float u = rnd();\n" \
	"    uint l;\n" \
	"    if (u < share_seen)\n" \
	"        l = list_pick(seen_at, seen);\n" \
	"    else if (u < share_seen + share_near)\n" \
	"        l = list_pick(near_at, near);\n" \
	"    else\n" \
	"        l = lamp_by_power();\n" \
	"    probability = (1.0 - share_seen - share_near) * lamp_power(l) / max(s.lamp.x, 1.0e-20);\n" \
	"    if (seen > 0u)\n" \
	"        probability += share_seen * list_probability(seen_at, seen, l);\n" \
	"    if (near > 0u)\n" \
	"        probability += share_near * list_probability(near_at, near, l);\n" \
	"    return l;\n" \
	"}\n" \
	"\n" \
	"/* whether a lamp's point at distance d along l is seen from o: nothing\n" \
	"before it, another lamp's surface included (whose own light its pick\n" \
	"counts; let through, it was counted twice: the bake's lamp-lit rooms then\n" \
	"brighter than R's, whose bounces never let it through) */\n" \
	"bool lamp_visible(vec3 o, vec3 l, float d)\n" \
	"{\n" \
	"    return !blocked(o, l, max(d * 0.995 - 0.03, 0.0));\n" \
	"}\n" \
	"\n" \
	"/* a direction from p toward a point of a lamp's triangle (its corners a0\n" \
	"a1 a2, its normal ne), its distance, and its density over directions'\n" \
	"inverse: by the solid angle where the triangle subtends a fair one\n" \
	"(Arvo's spherical triangle, as PBRT's SampleSphericalTriangle: no 1/d^2 near\n" \
	"a lamp), else by its area (a small or far lamp); false where p is behind it */\n" \
	"bool lamp_direction(vec3 p, vec3 a0, vec3 a1, vec3 a2, vec3 ne, out vec3 w, out float d, out float inverse_pdf)\n" \
	"{\n" \
	"    w = vec3(0.0, 0.0, 1.0);\n" \
	"    d = 0.0;\n" \
	"    inverse_pdf = 0.0;\n" \
	"    if (dot(ne, p - a0) <= 1.0e-6)\n" \
	"        return false;\n" \
	"    vec3 a = a0 - p, b = a1 - p, c = a2 - p;\n" \
	"    float la = length(a), lb = length(b), lc = length(c);\n" \
	"    /* (HALO_BAKE_TERMS' 64: by area alone, a diagnostic) */\n" \
	"    bool spherical = la > 1.0e-5 && lb > 1.0e-5 && lc > 1.0e-5 && !term_off(64u);\n" \
	"    float solid = 0.0, alpha = 0.0;\n" \
	"    vec3 n_ab, n_bc, n_ca;\n" \
	"    if (spherical)\n" \
	"    {\n" \
	"        a /= la; b /= lb; c /= lc;\n" \
	"        n_ab = cross(a, b); n_bc = cross(b, c); n_ca = cross(c, a);\n" \
	"        spherical = dot(n_ab, n_ab) > 1.0e-12 && dot(n_bc, n_bc) > 1.0e-12 && dot(n_ca, n_ca) > 1.0e-12;\n" \
	"    }\n" \
	"    if (spherical)\n" \
	"    {\n" \
	"        n_ab = normalize(n_ab); n_bc = normalize(n_bc); n_ca = normalize(n_ca);\n" \
	"        alpha = acos(clamp(dot(n_ab, -n_ca), -1.0, 1.0));\n" \
	"        float beta = acos(clamp(dot(n_bc, -n_ab), -1.0, 1.0));\n" \
	"        float gamma = acos(clamp(dot(n_ca, -n_bc), -1.0, 1.0));\n" \
	"        solid = alpha + beta + gamma - 3.14159265;\n" \
	"        spherical = solid > 3.0e-4 && solid < 6.22;\n" \
	"    }\n" \
	"    if (spherical)\n" \
	"    {\n" \
	"        float u0 = rnd(), u1 = rnd();\n" \
	"        float ap = mix(3.14159265, alpha + 3.14159265 + solid - alpha, u0);\n" \
	"        ap = 3.14159265 + u0 * solid;\n" \
	"        float ca = cos(alpha), sa = sin(alpha);\n" \
	"        float sin_phi = sin(ap) * ca - cos(ap) * sa;\n" \
	"        float cos_phi = cos(ap) * ca + sin(ap) * sa;\n" \
	"        float k1 = cos_phi + ca;\n" \
	"        float k2 = sin_phi - sa * dot(a, b);\n" \
	"        float cos_bp = (k2 + (k2 * cos_phi - k1 * sin_phi) * ca) / max((k2 * sin_phi + k1 * cos_phi) * sa, 1.0e-12);\n" \
	"        cos_bp = clamp(cos_bp, -1.0, 1.0);\n" \
	"        float sin_bp = sqrt(max(1.0 - cos_bp * cos_bp, 0.0));\n" \
	"        vec3 ca_ = c - dot(c, a) * a;\n" \
	"        if (dot(ca_, ca_) < 1.0e-20)\n" \
	"            spherical = false;\n" \
	"        else\n" \
	"        {\n" \
	"            vec3 cp = cos_bp * a + sin_bp * normalize(ca_);\n" \
	"            float cos_t = 1.0 - u1 * (1.0 - dot(cp, b));\n" \
	"            float sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));\n" \
	"            vec3 cpb = cp - dot(cp, b) * b;\n" \
	"            if (dot(cpb, cpb) < 1.0e-20)\n" \
	"                spherical = false;\n" \
	"            else\n" \
	"            {\n" \
	"                w = normalize(cos_t * b + sin_t * normalize(cpb));\n" \
	"                float denominator = dot(ne, w);\n" \
	"                if (denominator >= -1.0e-8)\n" \
	"                    return false;\n" \
	"                d = dot(ne, a0 - p) / denominator;\n" \
	"                inverse_pdf = solid;\n" \
	"                return d > 0.0;\n" \
	"            }\n" \
	"        }\n" \
	"    }\n" \
	"    /* (by area: a uniform point, its density over directions d^2 / (area cos)) */\n" \
	"    float area = 0.5 * length(cross(a1 - a0, a2 - a0));\n" \
	"    float r1 = sqrt(rnd()), r2 = rnd();\n" \
	"    vec3 q = (1.0 - r1) * a0 + r1 * (1.0 - r2) * a1 + r1 * r2 * a2;\n" \
	"    vec3 to = q - p;\n" \
	"    float d2 = max(dot(to, to), 1.0e-6);\n" \
	"    d = sqrt(d2);\n" \
	"    w = to / d;\n" \
	"    float ce = dot(ne, -w);\n" \
	"    if (ce <= 0.0 || area <= 0.0)\n" \
	"        return false;\n" \
	"    inverse_pdf = area * ce / d2;\n" \
	"    return true;\n" \
	"}\n" \
	"\n" \
	"/* the lamps' light at p (normal n, 0 for a probe's: every direction):\n" \
	"resampled importance (Talbot et al.'s RIS, as the path tracer's ReSTIR DI\n" \
	"takes its candidates): candidates lamp points, each its lamp picked from\n" \
	"the grid's cell and its point by solid angle or area, one kept by its\n" \
	"unshadowed light there over its candidates' density, its shadow traced\n" \
	"from o; its light by the candidates' mean weight. Unbiased whatever the\n" \
	"candidates' count, the more of them the less noise for the one ray. The\n" \
	"direction kept for the moment */\n" \
	"vec3 lamps_ris(vec3 p, vec3 n, vec3 o, int candidates, out vec3 kept_l)\n" \
	"{\n" \
	"    kept_l = vec3(0.0, 0.0, 1.0);\n" \
	"    if (s.counts.z == 0u || s.lamp.x <= 0.0 || s.rig[10].w < 0.5 || term_off(2u))\n" \
	"        return vec3(0.0);\n" \
	"    Floats f = Floats(s.lamps);\n" \
	"    vec3 kept = vec3(0.0);\n" \
	"    float kept_target = 0.0, kept_d = 0.0, sum = 0.0;\n" \
	"    bool probe = dot(n, n) < 0.5;\n" \
	"    for (int c = 0; c < candidates; c++)\n" \
	"    {\n" \
	"        float probability;\n" \
	"        uint lamp = lamp_pick(p, probability);\n" \
	"        if (probability <= 0.0)\n" \
	"            continue;\n" \
	"        uint at = lamp * 16u;\n" \
	"        vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);\n" \
	"        vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);\n" \
	"        vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);\n" \
	"        vec3 radiance = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x;\n" \
	"        vec3 ne = normalize(cross(a1 - a0, a2 - a0));\n" \
	"        vec3 l;\n" \
	"        float d, inverse_pdf;\n" \
	"        if (!lamp_direction(p, a0, a1, a2, ne, l, d, inverse_pdf))\n" \
	"            continue;\n" \
	"        float cr = probe ? 1.0 : dot(n, l);\n" \
	"        if (cr <= 0.0)\n" \
	"            continue;\n" \
	"        /* (the integrand unshadowed over directions, its luminance the\n" \
	"        target; the candidate's weight it over the candidate's density) */\n" \
	"        vec3 integrand = probe ? radiance : radiance * cr / 3.14159265;\n" \
	"        float target = luma(integrand);\n" \
	"        float w = target * inverse_pdf / probability;\n" \
	"        if (!(w > 0.0) || isinf(w))\n" \
	"            continue;\n" \
	"        sum += w;\n" \
	"        if (rnd() * sum <= w)\n" \
	"        {\n" \
	"            kept = integrand;\n" \
	"            kept_target = target;\n" \
	"            kept_l = l;\n" \
	"            kept_d = d;\n" \
	"        }\n" \
	"    }\n" \
	"    if (kept_target <= 0.0 || sum <= 0.0)\n" \
	"        return vec3(0.0);\n" \
	"    if (!lamp_visible(o, kept_l, kept_d))\n" \
	"        return vec3(0.0);\n" \
	"    return kept / kept_target * (sum / float(candidates));\n" \
	"}\n" \
	"/* the fixtures (the level's light fixtures, CE's radiosity's point and\n" \
	"spot lights: s.unused's 4 vec4 each, s.lamp.z of them: the place, the\n" \
	"intensity's colour in the bake's unit at a world unit (the irradiance a\n" \
	"surface facing it takes, over pi folded in where n is a texel's), the\n" \
	"spot's axis and outer cone's cosine (2: a point light), the inner cone's\n" \
	"cosine): one picked by its unshadowed light at p among all (16 or fewer)\n" \
	"or 16 candidates, its shadow traced; n 0: a probe's (the irradiance a\n" \
	"surface facing it would take, no cosine) */\n" \
	"vec3 fixture_light(uint i, vec3 p, vec3 n, out vec3 l, out float d)\n" \
	"{\n" \
	"    Floats f = Floats(s.unused);\n" \
	"    uint at = i * 16u;\n" \
	"    vec3 to = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]) - p;\n" \
	"    float d2 = dot(to, to);\n" \
	"    d = sqrt(d2);\n" \
	"    l = to / max(d, 1.0e-6);\n" \
	"    float c = dot(n, n) < 0.5 ? 1.0 : dot(n, l) / 3.14159265;\n" \
	"    if (c <= 0.0 || d < 1.0e-4)\n" \
	"        return vec3(0.0);\n" \
	"    vec4 axis = vec4(f.v[at + 8u], f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);\n" \
	"    float cone = 1.0;\n" \
	"    if (axis.w < 1.5)\n" \
	"        cone = smoothstep(axis.w, max(f.v[at + 12u], axis.w + 1.0e-4), dot(-l, axis.xyz));\n" \
	"    /* (a world unit's tenth softens the inverse square at the light) */\n" \
	"    return vec3(f.v[at + 4u], f.v[at + 5u], f.v[at + 6u]) * (c * cone / (d2 + 0.01));\n" \
	"}\n" \
	"vec3 fixtures_ris(vec3 p, vec3 n, vec3 o, out vec3 kept_l)\n" \
	"{\n" \
	"    kept_l = vec3(0.0, 0.0, 1.0);\n" \
	"    uint count = uint(s.lamp.z);\n" \
	"    if (count == 0u || term_off(16u))\n" \
	"        return vec3(0.0);\n" \
	"    uint candidates = min(count, 16u);\n" \
	"    float sum = 0.0, kept_w = 0.0, kept_d = 0.0;\n" \
	"    vec3 kept = vec3(0.0);\n" \
	"    for (uint c = 0u; c < candidates; c++)\n" \
	"    {\n" \
	"        uint i = count <= 16u ? c : min(uint(rnd() * float(count)), count - 1u);\n" \
	"        vec3 l;\n" \
	"        float d;\n" \
	"        vec3 e = fixture_light(i, p, n, l, d);\n" \
	"        float w = luma(e);\n" \
	"        if (!(w > 0.0) || isinf(w))\n" \
	"            continue;\n" \
	"        sum += w;\n" \
	"        if (rnd() * sum <= w)\n" \
	"        {\n" \
	"            kept = e;\n" \
	"            kept_w = w;\n" \
	"            kept_l = l;\n" \
	"            kept_d = d;\n" \
	"        }\n" \
	"    }\n" \
	"    if (kept_w <= 0.0 || blocked(o, kept_l, max(kept_d - 0.05, 0.0)))\n" \
	"        return vec3(0.0);\n" \
	"    return kept / kept_w * sum * (float(count) / float(candidates));\n" \
	"}\n" \
	"vec3 lamps_at(vec3 p, vec3 n, vec3 o, inout vec3 moment)\n" \
	"{\n" \
	"    vec3 l;\n" \
	"    vec3 e = lamps_ris(p, n, o, 8, l);\n" \
	"    moment += luma(e) * 3.14159265 / max(dot(n, l), 0.1) * l;\n" \
	"    vec3 fl;\n" \
	"    vec3 fe = fixtures_ris(p, n, o, fl);\n" \
	"    moment += luma(fe) * 3.14159265 / max(dot(n, fl), 0.1) * fl;\n" \
	"    return e + fe;\n" \
	"}\n" \
	"/* whether lamp l is in a cell's list (a binary search of its ids) */\n" \
	"bool list_has(uint at, uint count, uint l)\n" \
	"{\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    uint lo = 0u, hi = count;\n" \
	"    while (lo < hi)\n" \
	"    {\n" \
	"        uint mid = (lo + hi) / 2u;\n" \
	"        if (g.v[at + 2u * mid + 1u] < l)\n" \
	"            lo = mid + 1u;\n" \
	"        else\n" \
	"            hi = mid;\n" \
	"    }\n" \
	"    return lo < count && g.v[at + 2u * lo + 1u] == l;\n" \
	"}\n" \
	"/* one lamp's light at p: a point of it (by solid angle or area), its\n" \
	"shadow traced from o, over its density; the moment kept */\n" \
	"vec3 lamp_sample(uint lamp, vec3 p, vec3 n, vec3 o, inout vec3 moment)\n" \
	"{\n" \
	"    Floats f = Floats(s.lamps);\n" \
	"    uint at = lamp * 16u;\n" \
	"    vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);\n" \
	"    vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);\n" \
	"    vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);\n" \
	"    vec3 ne = normalize(cross(a1 - a0, a2 - a0));\n" \
	"    vec3 l;\n" \
	"    float d, inverse_pdf;\n" \
	"    if (!lamp_direction(p, a0, a1, a2, ne, l, d, inverse_pdf))\n" \
	"        return vec3(0.0);\n" \
	"    float cr = dot(n, l);\n" \
	"    if (cr <= 0.0 || !lamp_visible(o, l, d))\n" \
	"        return vec3(0.0);\n" \
	"    vec3 e = vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]) * s.rig[11].x * (cr / 3.14159265) * inverse_pdf;\n" \
	"    if (any(isnan(e)) || any(isinf(e)))\n" \
	"        return vec3(0.0);\n" \
	"    moment += luma(e) * 3.14159265 / max(cr, 0.1) * l;\n" \
	"    return e;\n" \
	"}\n" \
	"/* the lamps' and the fixtures' light at a texel, every lamp of its cell's\n" \
	"two lists sampled each time (a point of each, its shadow traced) in place\n" \
	"of one pick among them, and the level's other lamps by one pick by power,\n" \
	"counted only where it is in neither list: unbiased, and without the pick's\n" \
	"noise among the lamps that light the cell (a cell with no lists: the\n" \
	"resampled pick, lamps_at) */\n" \
	"vec3 lamps_all(vec3 p, vec3 n, vec3 o, inout vec3 moment)\n" \
	"{\n" \
	"    int cell = grid_cell(p);\n" \
	"    if (cell < 0 || term_off(32u) || s.counts.z == 0u || s.lamp.x <= 0.0 || s.rig[10].w < 0.5 || term_off(2u))\n" \
	"        return lamps_at(p, n, o, moment);\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    uint seen_at = uint(cell), seen = min(g.v[seen_at], 64u);\n" \
	"    uint near_at = seen_at + 1u + 2u * seen, near = min(g.v[near_at], 64u);\n" \
	"    vec3 sum = vec3(0.0);\n" \
	"    for (uint i = 0u; i < seen; i++)\n" \
	"        sum += lamp_sample(g.v[seen_at + 1u + 2u * i], p, n, o, moment);\n" \
	"    for (uint i = 0u; i < near; i++)\n" \
	"    {\n" \
	"        uint l = g.v[near_at + 1u + 2u * i];\n" \
	"        if (!list_has(seen_at, seen, l))\n" \
	"            sum += lamp_sample(l, p, n, o, moment);\n" \
	"    }\n" \
	"    uint far_ = lamp_by_power();\n" \
	"    float pp = lamp_power(far_) / max(s.lamp.x, 1.0e-20);\n" \
	"    if (pp > 0.0 && !list_has(seen_at, seen, far_) && !list_has(near_at, near, far_))\n" \
	"        sum += lamp_sample(far_, p, n, o, moment) / pp;\n" \
	"    vec3 fl;\n" \
	"    vec3 fe = fixtures_ris(p, n, o, fl);\n" \
	"    moment += luma(fe) * 3.14159265 / max(dot(n, fl), 0.1) * fl;\n" \
	"    return sum + fe;\n" \
	"}\n" \
	"\n" \
	"/* a path from o along d (its first ray), the light it brings: the sky\n" \
	"where it leaves the level (the domes with it past the first ray, which\n" \
	"the point took as lights), at each hit its glow and its albedo by the sun's\n" \
	"and a lamp's light there and the next bounce; backface: the first ray met a\n" \
	"one-sided face's back */\n" \
	"vec3 path(vec3 o, vec3 d, int bounces, out bool backface)\n" \
	"{\n" \
	"    vec3 nh;\n" \
	"    uint t;\n" \
	"    vec2 b;\n" \
	"    backface = false;\n" \
	"    float th = hit(o, d, 300.0, nh, t, b);\n" \
	"    if (th < 0.0)\n" \
	"        return sky_along(d, false);\n" \
	"    if (dot(nh, nh) <= 0.0)\n" \
	"    {\n" \
	"        backface = true;\n" \
	"        return vec3(0.0);\n" \
	"    }\n" \
	"    vec3 h = o + d * th;\n" \
	"    vec3 throughput = vec3(1.0), light = vec3(0.0), unused = vec3(0.0);\n" \
	"    for (int bounce = 0; bounce < bounces && dot(nh, nh) > 0.0; bounce++)\n" \
	"    {\n" \
	"        light += throughput * glow_of(t);\n" \
	"        vec3 oh = h + nh * 0.03;\n" \
	"        vec3 lh;\n" \
	"        vec3 ls = sun_at(oh, nh) + lamps_ris(h, nh, oh, 4, lh) + fixtures_ris(h, nh, oh, lh);\n" \
	"        throughput *= albedo_of(t);\n" \
	"        light += throughput * ls;\n" \
	"        if (bounce == bounces - 1 || term_off(8u))\n" \
	"            break;\n" \
	"        if (bounce >= 1)\n" \
	"        {\n" \
	"            float keep = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);\n" \
	"            if (rnd() > keep)\n" \
	"                break;\n" \
	"            throughput /= keep;\n" \
	"        }\n" \
	"        vec3 wi = cosine_dir(nh);\n" \
	"        float t2 = hit(oh, wi, 300.0, nh, t, b);\n" \
	"        if (t2 < 0.0)\n" \
	"        {\n" \
	"            light += throughput * sky_along(wi, true);\n" \
	"            break;\n" \
	"        }\n" \
	"        h = oh + wi * t2;\n" \
	"    }\n" \
	"    return light;\n" \
	"}\n" \
	"\n" \
	"/* the final gather (docs/prism_bake.md, \"The estimator\"): the bake's own\n" \
	"light where a bounce's path meets the level, in place of the paths past\n" \
	"that hit. The cache's block (xgpu_vulkan_bake.c's gather_cache): the\n" \
	"virtual pages, the tile table, the pool (each texel's light and direction\n" \
	"words, Raster's), the atlas triangles (corners in their virtual page's\n" \
	"texels; flags from bit 8 the virtual page + 1) and each kept triangle's\n" \
	"atlas triangle (+1) */\n" \
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Cache\n" \
	"{\n" \
	"    uvec2 vpages, table, tiles, triangles, plan_atlas, unused[3];\n" \
	"};\n" \
	"vec3 cache_rgb9e5(uint v)\n" \
	"{\n" \
	"    float e = exp2(float(int(v >> 27u) - 15 - 9));\n" \
	"    return vec3(float(v & 511u), float((v >> 9u) & 511u), float((v >> 18u) & 511u)) * e;\n" \
	"}\n" \
	"bool cache_texel(Cache c, uint vp, ivec2 x, out uvec2 w)\n" \
	"{\n" \
	"    Uints v = Uints(c.vpages);\n" \
	"    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];\n" \
	"    w = uvec2(0u);\n" \
	"    if (x.x < 0 || x.y < 0)\n" \
	"        return false;\n" \
	"    uvec2 t = uvec2(x) / 16u;\n" \
	"    if (t.x >= tiles_x || t.y >= tiles_y)\n" \
	"        return false;\n" \
	"    uint physical = Uints(c.table).v[v.v[vp * 8u + 4u] + t.y * tiles_x + t.x];\n" \
	"    if (physical == 0u)\n" \
	"        return false;\n" \
	"    uvec2 l = uvec2(x) % 16u;\n" \
	"    uint at = (physical - 1u) * 512u + (l.y * 16u + l.x) * 2u;\n" \
	"    Uints tl = Uints(c.tiles);\n" \
	"    w = uvec2(tl.v[at], tl.v[at + 1u]);\n" \
	"    return (w.y & 0xff000000u) != 0u;\n" \
	"}\n" \
	"/* the bake's light at a structure hit (t, its barycentrics b) at the\n" \
	"normal n, less the floor of light (which the bounces do not pass on): its\n" \
	"texels' (bilinear, those the bake has); false where it has none there */\n" \
	"bool cached_at(uvec2 cache, uint t, vec2 b, vec3 n, out vec3 l)\n" \
	"{\n" \
	"    l = vec3(0.0);\n" \
	"    if (cache == uvec2(0u) || t >= s.counts.x + s.counts.y)\n" \
	"        return false;\n" \
	"    Cache c = Cache(cache);\n" \
	"    uint kept = Uints(s.info).v[t * 8u + 7u];\n" \
	"    if (kept == 0xffffffffu)\n" \
	"        return false;\n" \
	"    uint ai = Uints(c.plan_atlas).v[kept];\n" \
	"    if (ai == 0u)\n" \
	"        return false;\n" \
	"    Floats tr = Floats(c.triangles);\n" \
	"    uint base_ = (ai - 1u) * 16u;\n" \
	"    uint vp1 = floatBitsToUint(tr.v[base_ + 15u]) >> 8u;\n" \
	"    if (vp1 == 0u)\n" \
	"        return false;\n" \
	"    vec2 x = (1.0 - b.x - b.y) * vec2(tr.v[base_], tr.v[base_ + 1u]) + b.x * vec2(tr.v[base_ + 2u], tr.v[base_ + 3u]) +\n" \
	"        b.y * vec2(tr.v[base_ + 4u], tr.v[base_ + 5u]);\n" \
	"    vec2 tx = x - 0.5;\n" \
	"    ivec2 a = ivec2(floor(tx));\n" \
	"    vec2 f = tx - vec2(a);\n" \
	"    vec3 sum = vec3(0.0);\n" \
	"    float weights = 0.0;\n" \
	"    for (int k = 0; k < 4; k++)\n" \
	"    {\n" \
	"        ivec2 o = ivec2(k & 1, k >> 1);\n" \
	"        uvec2 tw;\n" \
	"        if (!cache_texel(c, vp1 - 1u, a + o, tw))\n" \
	"            continue;\n" \
	"        vec4 dir = unpackUnorm4x8(tw.y);\n" \
	"        if (dir.w <= 0.0)\n" \
	"            continue;\n" \
	"        vec3 kk = (dir.xyz - 0.5) * 4.0;\n" \
	"        float a0 = dir.w * 4.0 - 1.0;\n" \
	"        float wk = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n" \
	"        sum += wk * cache_rgb9e5(tw.x) * clamp(a0 + dot(kk, n), 0.0, 4.0);\n" \
	"        weights += wk;\n" \
	"    }\n" \
	"    if (weights < 1.0e-4)\n" \
	"        return false;\n" \
	"    l = max(sum / weights - (indoor(t) ? ambient_indoor() : ambient()), vec3(0.0));\n" \
	"    return true;\n" \
	"}\n" \
	"/* the light along d from o, gathered: at the first hit the hit's glow, and\n" \
	"its albedo by the sun there (traced) and the bake's own light there\n" \
	"(cached_at: every bounce past it, the lamps', the domes' and the\n" \
	"fixtures' light there), in place of the hit's lamp pick and the paths past\n" \
	"it; a hit the cache has nothing for (the static scenery, a tile not baked\n" \
	"yet) takes the path as path() does. No cache: path() */\n" \
	"vec3 path_gathered(vec3 o, vec3 d, int bounces, uvec2 cache, out bool backface)\n" \
	"{\n" \
	"    if (cache == uvec2(0u))\n" \
	"        return path(o, d, bounces, backface);\n" \
	"    vec3 nh;\n" \
	"    uint t;\n" \
	"    vec2 b;\n" \
	"    backface = false;\n" \
	"    float th = hit(o, d, 300.0, nh, t, b);\n" \
	"    if (th < 0.0)\n" \
	"        return sky_along(d, false);\n" \
	"    if (dot(nh, nh) <= 0.0)\n" \
	"    {\n" \
	"        backface = true;\n" \
	"        return vec3(0.0);\n" \
	"    }\n" \
	"    vec3 l;\n" \
	"    if (!cached_at(cache, t, b, nh, l))\n" \
	"        return path(o, d, bounces, backface);\n" \
	"    vec3 oh = o + d * th + nh * 0.03;\n" \
	"    return glow_of(t) + albedo_of(t) * (sun_at(oh, nh) + l);\n" \
	"}\n" \
	"\n" \
	"/* the samples' batches (each 64 samples, its first directions stratified\n" \
	"8 x 8: a batch's mean is the noise of a stratified estimate, which the\n" \
	"samples' own variance overstates): a sample added to the batch running\n" \
	"(sum, count), a whole batch's mean to the batches' statistics */\n" \
	"void batch_add(inout float sum, uint index, inout uint batches, inout float bmean, inout float bm2, float x)\n" \
	"{\n" \
	"    sum += x;\n" \
	"    if ((index & 63u) == 63u)\n" \
	"    {\n" \
	"        float m = sum / 64.0;\n" \
	"        batches++;\n" \
	"        float delta = m - bmean;\n" \
	"        bmean += delta / float(batches);\n" \
	"        bm2 += delta * (m - bmean);\n" \
	"        sum = 0.0;\n" \
	"    }\n" \
	"}\n" \
	"/* a stratum's jittered point of the unit square (64 strata, 8 x 8) */\n" \
	"vec2 stratum(uint index)\n" \
	"{\n" \
	"    uint k = index & 63u;\n" \
	"    return (vec2(float(k & 7u), float(k >> 3u)) + vec2(rnd(), rnd())) * 0.125;\n" \
	"}\n" \
	"vec3 cosine_dir_at(vec3 n, vec2 u)\n" \
	"{\n" \
	"    vec3 t, b;\n" \
	"    basis(n, t, b);\n" \
	"    float a = u.y * 6.2831853;\n" \
	"    return normalize(sqrt(u.x) * (cos(a) * t + sin(a) * b) + sqrt(max(1.0 - u.x, 0.0)) * n);\n" \
	"}\n" \
	"vec3 sphere_dir_at(vec2 u)\n" \
	"{\n" \
	"    float z = u.x * 2.0 - 1.0, a = u.y * 6.2831853, r = sqrt(max(1.0 - z * z, 0.0));\n" \
	"    return vec3(r * cos(a), r * sin(a), z);\n" \
	"}\n" \
	"\n" \
	"/* the convergence's rule (prism_bake_converge.c's, on the GPU): the\n" \
	"running statistics (count, hits, mean, m2, maximum) merged with a batch's\n" \
	"(Chan's form), then whether the texel stops: 1 converged, 2 black, 0 not */\n" \
	"uint converge(inout uint count, inout uint hits, inout float mean, inout float m2, inout float most,\n" \
	"    uint bc, uint bh, float bmean, float bm2, float bmost, uint batches, float batch_m2)\n" \
	"{\n" \
	"    if (bc > 0u)\n" \
	"    {\n" \
	"        if (count == 0u)\n" \
	"        {\n" \
	"            count = bc; hits = bh; mean = bmean; m2 = bm2; most = bmost;\n" \
	"        }\n" \
	"        else\n" \
	"        {\n" \
	"            float n = float(count) + float(bc), delta = bmean - mean;\n" \
	"            m2 = m2 + bm2 + delta * delta * (float(count) * float(bc) / n);\n" \
	"            mean = mean + delta * (float(bc) / n);\n" \
	"            count += bc;\n" \
	"            hits += bh;\n" \
	"            most = max(most, bmost);\n" \
	"        }\n" \
	"    }\n" \
	"    /* (the error measured against the light, or the noise floor where the\n" \
	"    light is less: s.rig[15].z, D4 amended's 0.1 of middle grey at `max`;\n" \
	"    0 the literal rule; black below floor_ after 4096 samples) */\n" \
	"    float threshold = push.values.x, floor_ = push.values.y, error_floor = max(floor_, s.rig[15].z);\n" \
	"    uint minimum = push.parameters.z & 0xffffu;\n" \
	"    if (count < minimum)\n" \
	"        return 0u;\n" \
	"    if (push.values.z > 0.0 && float(count) >= push.values.z)\n" \
	"        return hits == 0u ? 2u : 1u;\n" \
	"    if (hits == 0u)\n" \
	"        return count >= 4096u ? 2u : 0u;\n" \
	"    if (hits < 16u)\n" \
	"        return 0u;\n" \
	"    float scale = max(abs(mean), error_floor);\n" \
	"    if (abs(mean) < floor_ && count >= 4096u)\n" \
	"        return 2u;\n" \
	"    if (most > threshold * scale * float(count))\n" \
	"        return 0u;\n" \
	"    /* (the relative standard error itself, as the gates define D4's\n" \
	"    threshold (docs/prism_gates.md), of the stratified batches' means: 8\n" \
	"    batches at least) */\n" \
	"    if (batches < 8u)\n" \
	"        return 0u;\n" \
	"    float variance = max(batch_m2, 0.0) / float(batches - 1u);\n" \
	"    float error = sqrt(variance / float(batches)) / scale;\n" \
	"    return error <= threshold ? 1u : 0u;\n" \
	"}\n"

/* the texels' places (a batch of tiles at a time): each texel of each new
tile (its entry: its origin in its virtual page's texels, its slot, its
triangles' first and count in the tiles' lists) found on its tile's
triangles (each: its corners in the virtual page's texels, x y each, its
world corners, its flags: 16 floats), at its middle: the triangle that
holds it, or the nearest within a texel and a half (the ring the bilinear
read and the gutter take), its point and face normal. Written: the texel's
record (8 floats: its point, its normal, its flags as a uint: 1 a point, 2
on a triangle, 4 a remodel's; 0), its state (0 running, 4 none), its
accumulators zeroed and its slot's count of running texels counted. The job
block: [0] the entries (8 words), [1] the tiles' lists, [2] the triangles,
[3] the records, [4] the states, [5] the accumulators, [6] the running
counts (0 for a mask's: no accumulators, no counts); parameters.x the
entries, w the texels a tile (256 the lightmap's, 4096 a mask's) */
static const char setup_source[] =
	BAKE_COMMON
	"layout(local_size_x = 256) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 entries, list, triangles, texels, states, accumulators, running, unused; };\n"
	"void main()\n"
	"{\n"
	"    Job j = Job(push.job);\n"
	"    uint per = push.parameters.w;\n"
	"    uint entry = gl_GlobalInvocationID.x / per, texel = gl_GlobalInvocationID.x % per;\n"
	"    if (entry >= push.parameters.x)\n"
	"        return;\n"
	"    uint side = uint(sqrt(float(per)) + 0.5);\n"
	"    float step_ = 16.0 / float(side);\n"
	"    Floats e = Floats(j.entries);\n"
	"    vec2 origin = vec2(e.v[entry * 8u], e.v[entry * 8u + 1u]);\n"
	"    uint slot = floatBitsToUint(e.v[entry * 8u + 2u]);\n"
	"    uint first = floatBitsToUint(e.v[entry * 8u + 3u]), count = floatBitsToUint(e.v[entry * 8u + 4u]);\n"
	"    vec2 at = origin + (vec2(texel % side, texel / side) + 0.5) * step_;\n"
	"    Uints list = Uints(j.list);\n"
	"    Floats t = Floats(j.triangles);\n"
	"    float best = 1.0e30;\n"
	"    vec3 point = vec3(0.0), normal = vec3(0.0, 0.0, 1.0);\n"
	"    uint flags = 0u;\n"
	"    float reach = 1.5 * step_;\n"
	"    for (uint k = 0u; k < count; k++)\n"
	"    {\n"
	"        uint b = list.v[first + k] * 16u;\n"
	"        vec2 p0 = vec2(t.v[b], t.v[b + 1u]), p1 = vec2(t.v[b + 2u], t.v[b + 3u]), p2 = vec2(t.v[b + 4u], t.v[b + 5u]);\n"
	"        vec2 e1 = p1 - p0, e2 = p2 - p0, q = at - p0;\n"
	"        float det = e1.x * e2.y - e1.y * e2.x;\n"
	"        if (abs(det) < 1.0e-12)\n"
	"            continue;\n"
	"        float u = (q.x * e2.y - q.y * e2.x) / det, v = (e1.x * q.y - e1.y * q.x) / det;\n"
	"        float w = 1.0 - u - v;\n"
	"        float distance;\n"
	"        vec3 bc;\n"
	"        if (u >= 0.0 && v >= 0.0 && w >= 0.0)\n"
	"        {\n"
	"            distance = 0.0;\n"
	"            bc = vec3(w, u, v);\n"
	"        }\n"
	"        else\n"
	"        {\n"
	"            distance = 1.0e30;\n"
	"            bc = vec3(1.0 / 3.0);\n"
	"            vec2 c[3] = vec2[3](p0, p1, p2);\n"
	"            for (int edge = 0; edge < 3; edge++)\n"
	"            {\n"
	"                vec2 a = c[edge], d = c[(edge + 1) % 3] - c[edge];\n"
	"                float h = clamp(dot(at - a, d) / max(dot(d, d), 1.0e-12), 0.0, 1.0);\n"
	"                float dd = length(at - (a + h * d));\n"
	"                if (dd < distance)\n"
	"                {\n"
	"                    distance = dd;\n"
	"                    vec3 x = vec3(0.0);\n"
	"                    x[edge] = 1.0 - h;\n"
	"                    x[(edge + 1) % 3] = h;\n"
	"                    bc = x;\n"
	"                }\n"
	"            }\n"
	"            bc = mix(bc, vec3(1.0 / 3.0), 0.002);\n"
	"        }\n"
	"        if (distance > reach || distance >= best)\n"
	"            continue;\n"
	"        best = distance;\n"
	"        vec3 w0 = vec3(t.v[b + 6u], t.v[b + 7u], t.v[b + 8u]);\n"
	"        vec3 w1 = vec3(t.v[b + 9u], t.v[b + 10u], t.v[b + 11u]);\n"
	"        vec3 w2 = vec3(t.v[b + 12u], t.v[b + 13u], t.v[b + 14u]);\n"
	"        point = bc.x * w0 + bc.y * w1 + bc.z * w2;\n"
	"        vec3 n = cross(w1 - w0, w2 - w0);\n"
	"        normal = dot(n, n) > 1.0e-24 ? normalize(n) : vec3(0.0, 0.0, 1.0);\n"
	"        flags = 1u | (distance == 0.0 ? 2u : 0u) | ((floatBitsToUint(t.v[b + 15u]) & 1u) != 0u ? 4u : 0u) |\n"
	"            ((floatBitsToUint(t.v[b + 15u]) & 2u) != 0u ? 8u : 0u);\n"
	"        if (distance == 0.0)\n"
	"            break;\n"
	"    }\n"
	"    uint index = slot * per + texel;\n"
	"    Floats o = Floats(j.texels);\n"
	"    o.v[index * 8u] = point.x;\n"
	"    o.v[index * 8u + 1u] = point.y;\n"
	"    o.v[index * 8u + 2u] = point.z;\n"
	"    o.v[index * 8u + 3u] = normal.x;\n"
	"    o.v[index * 8u + 4u] = normal.y;\n"
	"    o.v[index * 8u + 5u] = normal.z;\n"
	"    o.v[index * 8u + 6u] = uintBitsToFloat(flags);\n"
	"    o.v[index * 8u + 7u] = 0.0;\n"
	"    Uints(j.states).v[index] = flags != 0u ? 0u : 4u;\n"
	"    if (j.accumulators != uvec2(0u))\n"
	"    {\n"
	"        Floats ac = Floats(j.accumulators);\n"
	"        for (uint k = 0u; k < 20u; k++)\n"
	"            ac.v[index * 20u + k] = 0.0;\n"
	"        if (flags != 0u)\n"
	"            atomicAdd(Uints(j.running).v[slot], 1u);\n"
	"    }\n"
	"}\n";

/* the lightmap's texels baked: each running texel of the batch takes
parameters.y samples (the light less the sun's, direct and indirect, the
light's moment, the sun's visibility; the first path ray's backfaces), its
statistics merged and the stopping rule asked; a texel that stops takes
one from its tile's count of running texels. A texel whose first rays meet
mostly one-sided faces' backs is inside the level (state 3), its light
found from its neighbours afterwards. The job block: [0] the texels'
records, [1] the accumulators (16 floats a texel: the direct light's mean
RGB, the indirect's, the moment's mean, the sun's visibility's mean, the
backfaces' share, then the statistics: count and hits as uints, mean, m2,
maximum), [2] the states, [3] the tiles' running counts; parameters.x the
texels, y the samples, z the least samples (64), w the frame (the seeds');
values: x the threshold, y the black floor, z the samples at which a texel
stops whatever its error (0: none; the draft's), w the bounces */
static const char lightmap_source[] =
	BAKE_COMMON
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 texels, accumulators, states, running, list, args, cache, all_lamps; };\n"
	"/* two sets' count, mean and sum of squared deviations made one (Chan's) */\n"
	"void merge(inout float n, inout float mean, inout float m2, float n2, float mean2, float m22)\n"
	"{\n"
	"    float t = n + n2;\n"
	"    if (t <= 0.0)\n"
	"        return;\n"
	"    float d = mean2 - mean;\n"
	"    m2 = m2 + m22 + d * d * n * n2 / t;\n"
	"    mean += d * n2 / t;\n"
	"    n = t;\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    /* (the running texels alone: the compaction's list; each texel's samples\n"
	"    spread over `lanes` invocations side by side in a subgroup, each its own\n"
	"    whole batches of them, their sums and statistics put together before\n"
	"    the texel's are written: the same samples, the same rule, sooner) */\n"
	"    uint lanes = max(push.parameters.z >> 16u, 1u);\n"
	"    uint lane = gl_GlobalInvocationID.x % lanes;\n"
	"    uint slot = gl_GlobalInvocationID.x / lanes;\n"
	"    if (slot >= Uints(j.args).v[3])\n"
	"        return;\n"
	"    uint index = Uints(j.list).v[slot];\n"
	"    if (index >= push.parameters.x)\n"
	"        return;\n"
	"    Uints st = Uints(j.states);\n"
	"    if (st.v[index] != 0u)\n"
	"        return;\n"
	"    Floats tx = Floats(j.texels);\n"
	"    vec3 p = vec3(tx.v[index * 8u], tx.v[index * 8u + 1u], tx.v[index * 8u + 2u]);\n"
	"    vec3 n = vec3(tx.v[index * 8u + 3u], tx.v[index * 8u + 4u], tx.v[index * 8u + 5u]);\n"
	"    uint flags = floatBitsToUint(tx.v[index * 8u + 6u]);\n"
	"    Floats ac = Floats(j.accumulators);\n"
	"    uint base = index * 20u;\n"
	"    uint count = floatBitsToUint(ac.v[base + 11u]);\n"
	"    float batch_sum = ac.v[base + 16u], batch_mean = ac.v[base + 18u], batch_m2 = ac.v[base + 19u];\n"
	"    uint batches = floatBitsToUint(ac.v[base + 17u]);\n"
	"    seed = hash_u(index ^ hash_u(push.parameters.w * 2654435761u ^ hash_u(count + 0x9e3779b9u + lane * 0x632be5abu +\n"
	"        s.unused.x * 0x85ebca6bu)));\n"
	"    int bounces = int(push.values.w);\n"
	"    /* (a remodel's face, two-sided, its normal turned toward its open side\n"
	"    once: the side whose rays travel further before they meet the level) */\n"
	"    if (count == 0u && (flags & 4u) != 0u && lane == 0u)\n"
	"    {\n"
	"        float reach[2] = float[2](0.0, 0.0);\n"
	"        for (int side = 0; side < 2; side++)\n"
	"        {\n"
	"            vec3 ns = side == 0 ? n : -n;\n"
	"            for (int r = 0; r < 8; r++)\n"
	"            {\n"
	"                vec3 nh;\n"
	"                uint th;\n"
	"                vec2 bh_;\n"
	"                float d = hit(p + ns * 0.01, cosine_dir(ns), 50.0, nh, th, bh_);\n"
	"                reach[side] += d < 0.0 ? 50.0 : d;\n"
	"            }\n"
	"        }\n"
	"        if (reach[1] > reach[0] * 1.25)\n"
	"        {\n"
	"            n = -n;\n"
	"            tx.v[index * 8u + 3u] = n.x;\n"
	"            tx.v[index * 8u + 4u] = n.y;\n"
	"            tx.v[index * 8u + 5u] = n.z;\n"
	"        }\n"
	"    }\n"
	"    vec3 o = p + n * 0.01;\n"
	"    vec3 direct = vec3(0.0), indirect = vec3(0.0), moment = vec3(0.0);\n"
	"    float sun = 0.0, back = 0.0;\n"
	"    uint bc = 0u, bh = 0u;\n"
	"    float bmean = 0.0, bm2 = 0.0, bmost = 0.0;\n"
	"    /* (the lane's samples: alone, the dispatch's; spread, lane 0 the rest of\n"
	"    the texel's batch begun and its share, each other lane its share of\n"
	"    whole batches after; none for a texel not begun, whose first lane turns\n"
	"    its face) */\n"
	"    uint samples = push.parameters.y, first = count;\n"
	"    if (lanes > 1u)\n"
	"    {\n"
	"        uint share = max((samples + 63u) & ~63u, 64u);\n"
	"        uint aligned = (count + 63u) & ~63u;\n"
	"        samples = count == 0u ? (lane == 0u ? push.parameters.y : 0u) : lane == 0u ? aligned - count + share : share;\n"
	"        first = lane == 0u ? count : aligned + lane * share;\n"
	"        if (lane != 0u)\n"
	"        {\n"
	"            batch_sum = 0.0;\n"
	"            batches = 0u;\n"
	"            batch_mean = 0.0;\n"
	"            batch_m2 = 0.0;\n"
	"        }\n"
	"    }\n"
	"    /* (the units rig's floor of light: the indoor one in an indoor\n"
	"    cluster, flag 8) */\n"
	"    vec3 floor_ = (flags & 8u) != 0u ? ambient_indoor() : ambient();\n"
	"    for (uint k = 0u; k < samples; k++)\n"
	"    {\n"
	"        vec3 m = vec3(0.0);\n"
	"        /* (past the draft: every lamp of the cell's lists; the draft: the\n"
	"        resampled pick) */\n"
	"        bool all_lamps = (j.all_lamps.x != 0u && !term_off(1024u)) || term_off(512u);\n"
	"        vec3 dl = domes_at(o, n, m) + (term_off(256u) ? vec3(0.0) : all_lamps ? lamps_all(p, n, o, m) :\n"
	"            lamps_at(p, n, o, m)) + floor_;\n"
	"        vec3 d = cosine_dir_at(n, stratum(first + k));\n"
	"        bool backface;\n"
	"        vec3 il = path_gathered(o, d, bounces, j.cache, backface);\n"
	"        if (term_off(128u))\n"
	"            il = vec3(0.0);\n"
	"        float c = max(dot(d, n), 0.1);\n"
	"        m += luma(il) * 3.14159265 / c * d;\n"
	"        float v = s.sun.w > 0.0 && dot(n, s.sun.xyz) > 0.0 ? sun_visible(o) : 0.0;\n"
	"        if (any(isnan(dl)) || any(isinf(dl)))\n"
	"            dl = vec3(0.0);\n"
	"        if (any(isnan(il)) || any(isinf(il)))\n"
	"            il = vec3(0.0);\n"
	"        if (any(isnan(m)) || any(isinf(m)))\n"
	"            m = vec3(0.0);\n"
	"        direct += dl;\n"
	"        indirect += il;\n"
	"        moment += m;\n"
	"        sun += v;\n"
	"        back += backface ? 1.0 : 0.0;\n"
	"        float x = luma(dl + il);\n"
	"        batch_add(batch_sum, first + k, batches, batch_mean, batch_m2, x);\n"
	"        bc++;\n"
	"        bh += x != 0.0 ? 1u : 0u;\n"
	"        float delta = x - bmean;\n"
	"        bmean += delta / float(bc);\n"
	"        bm2 += delta * (x - bmean);\n"
	"        bmost = max(bmost, x);\n"
	"    }\n"
	"    if (lanes > 1u)\n"
	"    {\n"
	"        float fbc = float(bc), fb = float(batches);\n"
	"        for (uint off = 1u; off < lanes; off <<= 1u)\n"
	"        {\n"
	"            direct += subgroupShuffleXor(direct, off);\n"
	"            indirect += subgroupShuffleXor(indirect, off);\n"
	"            moment += subgroupShuffleXor(moment, off);\n"
	"            sun += subgroupShuffleXor(sun, off);\n"
	"            back += subgroupShuffleXor(back, off);\n"
	"            bh += subgroupShuffleXor(bh, off);\n"
	"            bmost = max(bmost, subgroupShuffleXor(bmost, off));\n"
	"            float n2 = subgroupShuffleXor(fbc, off), mean2 = subgroupShuffleXor(bmean, off);\n"
	"            float m22 = subgroupShuffleXor(bm2, off);\n"
	"            merge(fbc, bmean, bm2, n2, mean2, m22);\n"
	"            n2 = subgroupShuffleXor(fb, off);\n"
	"            mean2 = subgroupShuffleXor(batch_mean, off);\n"
	"            m22 = subgroupShuffleXor(batch_m2, off);\n"
	"            merge(fb, batch_mean, batch_m2, n2, mean2, m22);\n"
	"        }\n"
	"        bc = uint(fbc + 0.5);\n"
	"        batches = uint(fb + 0.5);\n"
	"        batch_sum = 0.0;\n"
	"        if (lane != 0u)\n"
	"            return;\n"
	"        if (bc == 0u)\n"
	"            return;\n"
	"    }\n"
	"    float total = float(count) + float(bc);\n"
	"    float wb = float(bc) / max(total, 1.0);\n"
	"    for (uint k = 0u; k < 3u; k++)\n"
	"    {\n"
	"        ac.v[base + k] = mix(ac.v[base + k], direct[k] / float(bc), wb);\n"
	"        ac.v[base + 3u + k] = mix(ac.v[base + 3u + k], indirect[k] / float(bc), wb);\n"
	"        ac.v[base + 6u + k] = mix(ac.v[base + 6u + k], moment[k] / float(bc), wb);\n"
	"    }\n"
	"    ac.v[base + 9u] = mix(ac.v[base + 9u], sun / float(bc), wb);\n"
	"    ac.v[base + 10u] = mix(ac.v[base + 10u], back / float(bc), wb);\n"
	"    uint hits = floatBitsToUint(ac.v[base + 12u]);\n"
	"    float mean = ac.v[base + 13u], m2 = ac.v[base + 14u], most = ac.v[base + 15u];\n"
	"    uint state = converge(count, hits, mean, m2, most, bc, bh, bmean, bm2, bmost, batches, batch_m2);\n"
	"    ac.v[base + 16u] = batch_sum;\n"
	"    ac.v[base + 17u] = uintBitsToFloat(batches);\n"
	"    ac.v[base + 18u] = batch_mean;\n"
	"    ac.v[base + 19u] = batch_m2;\n"
	"    /* (inside the level: most of its first rays met backs) */\n"
	"    if (count >= 32u && ac.v[base + 10u] > 0.4)\n"
	"        state = 3u;\n"
	"    ac.v[base + 11u] = uintBitsToFloat(count);\n"
	"    ac.v[base + 12u] = uintBitsToFloat(hits);\n"
	"    ac.v[base + 13u] = mean;\n"
	"    ac.v[base + 14u] = m2;\n"
	"    ac.v[base + 15u] = most;\n"
	"    if (state != 0u)\n"
	"    {\n"
	"        st.v[index] = state;\n"
	"        atomicAdd(Uints(j.running).v[index / 256u], 0xffffffffu);\n"
	"    }\n"
	"}\n";

/* the sun's visibility at a mask's texels (a tile's 64 x 64 where its
texels' visibility is neither all lit nor all dark): each texel's point
(the setup pass's, at the mask's density) and parameters.y directions of
the sun's disc, stratified, then more while they disagree, up to
parameters.z. Written: the visibility, 0 to 255, a byte a texel (the job
block's [1], a word for 4 texels). [0] the records; parameters.x the texels */
static const char mask_source[] =
	BAKE_COMMON
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 texels, mask, unused[6]; };\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    uint index = gl_GlobalInvocationID.x;\n"
	"    if (index >= push.parameters.x)\n"
	"        return;\n"
	"    Floats tx = Floats(j.texels);\n"
	"    vec3 p = vec3(tx.v[index * 8u], tx.v[index * 8u + 1u], tx.v[index * 8u + 2u]);\n"
	"    vec3 n = vec3(tx.v[index * 8u + 3u], tx.v[index * 8u + 4u], tx.v[index * 8u + 5u]);\n"
	"    uint flags = floatBitsToUint(tx.v[index * 8u + 6u]);\n"
	"    float v = 0.0;\n"
	"    /* (a remodel's face, two-sided: from its side toward the sun) */\n"
	"    if ((flags & 4u) != 0u && dot(n, s.sun.xyz) < 0.0)\n"
	"        n = -n;\n"
	"    if (flags != 0u && s.sun.w > 0.0 && dot(n, s.sun.xyz) > 0.0)\n"
	"    {\n"
	"        seed = hash_u(index ^ hash_u(push.parameters.w + 0x51ed270bu));\n"
	"        vec3 o = p + n * 0.01;\n"
	"        vec3 t, b;\n"
	"        basis(s.sun.xyz, t, b);\n"
	"        float cos_most = cos(0.5 * s.rig[0].w);\n"
	"        uint first = push.parameters.y, most = push.parameters.z, lit = 0u, rays = 0u;\n"
	"        uint side = uint(sqrt(float(first)) + 0.5);\n"
	"        for (uint k = 0u; k < most; k++)\n"
	"        {\n"
	"            if (k == first && (lit == 0u || lit == rays))\n"
	"                break;\n"
	"            vec2 u = k < side * side ? (vec2(k % side, k / side) + vec2(rnd(), rnd())) / float(side) : vec2(rnd(), rnd());\n"
	"            float c = mix(1.0, cos_most, u.x), sn = sqrt(max(1.0 - c * c, 0.0)), a = u.y * 6.2831853;\n"
	"            vec3 d = normalize(c * s.sun.xyz + sn * (cos(a) * t + sin(a) * b));\n"
	"            lit += sky_blocked(o, d, sun_reach()) ? 0u : 1u;\n"
	"            rays++;\n"
	"        }\n"
	"        v = float(lit) / float(max(rays, 1u));\n"
	"    }\n"
	"    uint value = uint(clamp(v, 0.0, 1.0) * 255.0 + 0.5);\n"
	"    atomicOr(Uints(j.mask).v[index >> 2u], value << ((index & 3u) * 8u));\n"
	"}\n";

/* the light-importance grid's candidates weighed: each (cell, candidate)
the light the candidate lamp gives the cell's points (up to 8, on the
level's surfaces in the cell, with their normals), parameters.y points of
the lamp each, its shadow traced; the mean written (the job block's [2]).
[0] the cells' points (8 a cell: x y z and the normal packed, 4 floats;
w 0: none), [1] the candidates (a uint lamp each, parameters.z a cell);
parameters.x the candidates' end, w the first */
static const char importance_source[] =
	BAKE_COMMON
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 points, candidates, weights, unused[5]; };\n"
	"vec3 unfold(vec2 f)\n"
	"{\n"
	"    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));\n"
	"    float t = max(-n.z, 0.0);\n"
	"    n.x += n.x >= 0.0 ? -t : t;\n"
	"    n.y += n.y >= 0.0 ? -t : t;\n"
	"    return normalize(n);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    uint index = gl_GlobalInvocationID.x + push.parameters.w;\n"
	"    if (index >= push.parameters.x)\n"
	"        return;\n"
	"    uint per = push.parameters.z;\n"
	"    uint cell = index / per;\n"
	"    uint lamp = Uints(j.candidates).v[index];\n"
	"    float sum = 0.0;\n"
	"    if (lamp != 0xffffffffu)\n"
	"    {\n"
	"        seed = hash_u(index ^ 0x2545f491u);\n"
	"        Floats f = Floats(s.lamps);\n"
	"        uint at = lamp * 16u;\n"
	"        vec3 a0 = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]);\n"
	"        vec3 a1 = vec3(f.v[at + 3u], f.v[at + 4u], f.v[at + 5u]);\n"
	"        vec3 a2 = vec3(f.v[at + 6u], f.v[at + 7u], f.v[at + 8u]);\n"
	"        float power = luma(vec3(f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]));\n"
	"        float area = f.v[at + 12u];\n"
	"        vec3 ne = normalize(cross(a1 - a0, a2 - a0));\n"
	"        Floats pts = Floats(j.points);\n"
	"        uint points = 0u;\n"
	"        for (uint k = 0u; k < 8u; k++)\n"
	"        {\n"
	"            uint b = (cell * 8u + k) * 4u;\n"
	"            if (pts.v[b + 3u] == 0.0)\n"
	"                continue;\n"
	"            vec3 p = vec3(pts.v[b], pts.v[b + 1u], pts.v[b + 2u]);\n"
	"            vec3 n = unfold(unpackSnorm2x16(floatBitsToUint(pts.v[b + 3u])));\n"
	"            vec3 o = p + n * 0.02;\n"
	"            points++;\n"
	"            for (uint r = 0u; r < push.parameters.y; r++)\n"
	"            {\n"
	"                float r1 = sqrt(rnd()), r2 = rnd();\n"
	"                vec3 q = (1.0 - r1) * a0 + r1 * (1.0 - r2) * a1 + r1 * r2 * a2;\n"
	"                vec3 to = q - p;\n"
	"                float d2 = max(dot(to, to), 1.0e-4), d = sqrt(d2);\n"
	"                vec3 l = to / d;\n"
	"                float cr = dot(n, l), ce = dot(ne, -l);\n"
	"                if (cr <= 0.0 || ce <= 0.0)\n"
	"                    continue;\n"
	"                if (!lamp_visible(o, l, d))\n"
	"                    continue;\n"
	"                sum += power * cr * ce * area / d2;\n"
	"            }\n"
	"        }\n"
	"        sum /= float(max(points * push.parameters.y, 1u));\n"
	"    }\n"
	"    Floats(j.weights).v[index] = sum;\n"
	"}\n";

/* the irradiance volume's probes baked: each running probe takes
parameters.y samples of the light reaching it from every direction, as SH
L1 (RGB, 4 coefficients each) of the radiance: a uniform direction's path
(the sky where it leaves, at a hit the light the path brings), and the
lights apart (a lamp's point picked as the lightmap's, its shadow traced; a
direction of each dome's cone), each over its density; the sun's
visibility; the share of its rays that meet backs (inside the level:
invalid); the rule asked on its L0's luminance. The job block: [0] the
probes' points (4 floats each), [1] the accumulators (16 floats: SH red 4,
green 4, blue 4, the sun's visibility, the backs' share, 0, 0), [2] the
states, [3] the statistics (count, hits, mean, m2, maximum: 5 words), [4]
the batch's count of running probes; parameters as the lightmap's */
static const char volume_source[] =
	BAKE_COMMON
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 probes, accumulators, states, statistics, running, list, args, cache; };\n"
	"vec4 sh(vec3 d) { return vec4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x); }\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    if (gl_GlobalInvocationID.x >= Uints(j.args).v[3])\n"
	"        return;\n"
	"    uint index = Uints(j.list).v[gl_GlobalInvocationID.x];\n"
	"    if (index >= push.parameters.x)\n"
	"        return;\n"
	"    Uints st = Uints(j.states);\n"
	"    if (st.v[index] != 0u)\n"
	"        return;\n"
	"    Floats pr = Floats(j.probes);\n"
	"    vec3 p = vec3(pr.v[index * 4u], pr.v[index * 4u + 1u], pr.v[index * 4u + 2u]);\n"
	"    Uints stats = Uints(j.statistics);\n"
	"    uint count = stats.v[index * 9u];\n"
	"    float batch_sum = uintBitsToFloat(stats.v[index * 9u + 5u]), batch_mean = uintBitsToFloat(stats.v[index * 9u + 7u]);\n"
	"    float batch_m2 = uintBitsToFloat(stats.v[index * 9u + 8u]);\n"
	"    uint batches = stats.v[index * 9u + 6u];\n"
	"    seed = hash_u(index ^ hash_u(push.parameters.w * 2654435761u ^ hash_u(count + 0x7f4a7c15u +\n"
	"        s.unused.x * 0x85ebca6bu)));\n"
	"    int bounces = int(push.values.w);\n"
	"    vec4 shr = vec4(0.0), shg = vec4(0.0), shb = vec4(0.0);\n"
	"    float sun = 0.0, back = 0.0;\n"
	"    uint bc = 0u, bh = 0u;\n"
	"    float bmean = 0.0, bm2 = 0.0, bmost = 0.0;\n"
	"    /* (the units rig's floor of light where the probe is: the indoor one\n"
	"    in an indoor cluster) */\n"
	"    vec3 floor_ = units_rig() ? mix(ambient_indoor(), ambient(), floor_at(p)) : vec3(0.0);\n"
	"    for (uint k = 0u; k < push.parameters.y; k++)\n"
	"    {\n"
	"        vec3 d = sphere_dir_at(stratum(count + k));\n"
	"        bool backface;\n"
	"        vec3 l = (path_gathered(p, d, bounces, j.cache, backface) + floor_) * 12.566371;\n"
	"        if (any(isnan(l)) || any(isinf(l))) l = vec3(0.0);\n"
	"        vec4 y = sh(d);\n"
	"        vec3 shc0 = l * y.x;\n"
	"        shr += l.r * y; shg += l.g * y; shb += l.b * y;\n"
	"        /* (the lamps: radiance from a direction, resampled as the texels') */\n"
	"        {\n"
	"            vec3 ld;\n"
	"            vec3 e = lamps_ris(p, vec3(0.0), p, 8, ld);\n"
	"            if (!(any(isnan(e)) || any(isinf(e))))\n"
	"            {\n"
	"                vec4 yl = sh(ld);\n"
	"                shr += e.r * yl; shg += e.g * yl; shb += e.b * yl;\n"
	"                shc0 += e * yl.x;\n"
	"            }\n"
	"            e = fixtures_ris(p, vec3(0.0), p, ld);\n"
	"            if (!(any(isnan(e)) || any(isinf(e))))\n"
	"            {\n"
	"                vec4 yl = sh(ld);\n"
	"                shr += e.r * yl; shg += e.g * yl; shb += e.b * yl;\n"
	"                shc0 += e * yl.x;\n"
	"            }\n"
	"        }\n"
	"        /* (each dome: a direction of its cone, its radiance pi r / its\n"
	"        solid angle over its density 1 / that) */\n"
	"        for (int i = 0; i < 4; i++)\n"
	"        {\n"
	"            vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];\n"
	"            if (dot(r.rgb, vec3(1.0)) <= 0.0)\n"
	"                continue;\n"
	"            vec3 dd = cone_dir(normalize(a.xyz), a.w);\n"
	"            if (!sky_blocked(p, dd, dome_reach(r)))\n"
	"            {\n"
	"                vec3 e = r.rgb * 3.14159265;\n"
	"                vec4 yd = sh(dd);\n"
	"                shr += e.r * yd; shg += e.g * yd; shb += e.b * yd;\n"
	"                shc0 += e * yd.x;\n"
	"            }\n"
	"        }\n"
	"        sun += s.sun.w > 0.0 ? sun_visible(p) : 0.0;\n"
	"        back += backface ? 1.0 : 0.0;\n"
	"        float x = luma(shc0);\n"
	"        batch_add(batch_sum, count + k, batches, batch_mean, batch_m2, x);\n"
	"        bc++;\n"
	"        bh += x != 0.0 ? 1u : 0u;\n"
	"        float delta = x - bmean;\n"
	"        bmean += delta / float(bc);\n"
	"        bm2 += delta * (x - bmean);\n"
	"        bmost = max(bmost, x);\n"
	"    }\n"
	"    Floats ac = Floats(j.accumulators);\n"
	"    uint base = index * 20u;\n"
	"    float wb = float(bc) / max(float(count) + float(bc), 1.0);\n"
	"    for (uint k = 0u; k < 4u; k++)\n"
	"    {\n"
	"        ac.v[base + k] = mix(ac.v[base + k], shr[k] / float(bc), wb);\n"
	"        ac.v[base + 4u + k] = mix(ac.v[base + 4u + k], shg[k] / float(bc), wb);\n"
	"        ac.v[base + 8u + k] = mix(ac.v[base + 8u + k], shb[k] / float(bc), wb);\n"
	"    }\n"
	"    ac.v[base + 12u] = mix(ac.v[base + 12u], sun / float(bc), wb);\n"
	"    ac.v[base + 13u] = mix(ac.v[base + 13u], back / float(bc), wb);\n"
	"    uint hits = stats.v[index * 9u + 1u];\n"
	"    float mean = uintBitsToFloat(stats.v[index * 9u + 2u]), m2 = uintBitsToFloat(stats.v[index * 9u + 3u]);\n"
	"    float most = uintBitsToFloat(stats.v[index * 9u + 4u]);\n"
	"    uint state = converge(count, hits, mean, m2, most, bc, bh, bmean, bm2, bmost, batches, batch_m2);\n"
	"    if (count >= 32u && ac.v[base + 13u] > 0.3)\n"
	"        state = 3u;\n"
	"    stats.v[index * 9u] = count;\n"
	"    stats.v[index * 9u + 1u] = hits;\n"
	"    stats.v[index * 9u + 2u] = floatBitsToUint(mean);\n"
	"    stats.v[index * 9u + 3u] = floatBitsToUint(m2);\n"
	"    stats.v[index * 9u + 4u] = floatBitsToUint(most);\n"
	"    stats.v[index * 9u + 5u] = floatBitsToUint(batch_sum);\n"
	"    stats.v[index * 9u + 6u] = batches;\n"
	"    stats.v[index * 9u + 7u] = floatBitsToUint(batch_mean);\n"
	"    stats.v[index * 9u + 8u] = floatBitsToUint(batch_m2);\n"
	"    if (state != 0u)\n"
	"    {\n"
	"        st.v[index] = state;\n"
	"        atomicAdd(Uints(j.running).v[0], 0xffffffffu);\n"
	"    }\n"
	"}\n";

/* the reflection probes (design section 3.2: GGX-prefiltered cube maps in
reachable space): values.x 0, each probe's box (its six axes' reach) and
place; 1, each texel of each level of each probe's cube (levels' sides
side >> level; a level's roughness (level / (levels - 1))^2) the light seen
along its direction (the bake's estimator's: path(), the sky and the domes
where a ray leaves), the GGX lobe's directions about it weighed by their
cosines, RGB9E5. The job block: [0] the probes' places (4 floats each), [1]
their info (12 floats: place, first texel, box's low corner, its high one),
[2] the texels; parameters x the batch's first probe, y its probes, z the
samples a texel, w the frame; values y the side, z the levels, w the
bounces */
static const char reflection_source[] =
	BAKE_COMMON
	"layout(local_size_x = 64) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 positions, info, data, unused[5]; };\n"
	"uint pack_rgb9e5(vec3 c)\n"
	"{\n"
	"    c = clamp(c, vec3(0.0), vec3(65408.0));\n"
	"    float m = max(c.r, max(c.g, c.b));\n"
	"    int e = max(-16, int(floor(log2(max(m, 1.0e-30))))) + 1;\n"
	"    float scale = exp2(float(9 - e));\n"
	"    if (floor(m * scale + 0.5) >= 512.0)\n"
	"    {\n"
	"        e++;\n"
	"        scale *= 0.5;\n"
	"    }\n"
	"    uvec3 q = uvec3(floor(c * scale + 0.5));\n"
	"    return min(q.r, 511u) | (min(q.g, 511u) << 9u) | (min(q.b, 511u) << 18u) | (uint(clamp(e + 15, 0, 31)) << 27u);\n"
	"}\n"
	"/* a cube's face (0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z) and its coordinates\n"
	"(-1 to 1): the direction (raster_source's reflection_at's mapping) */\n"
	"vec3 cube_dir(uint face, vec2 uv)\n"
	"{\n"
	"    vec3 d = face == 0u ? vec3(1.0, -uv.y, -uv.x) : face == 1u ? vec3(-1.0, -uv.y, uv.x) : face == 2u ?\n"
	"        vec3(uv.x, 1.0, uv.y) : face == 3u ? vec3(uv.x, -1.0, -uv.y) : face == 4u ? vec3(uv.x, -uv.y, 1.0) :\n"
	"        vec3(-uv.x, -uv.y, -1.0);\n"
	"    return normalize(d);\n"
	"}\n"
	"/* the light seen along d from o: the level's (the bake's estimator) where\n"
	"the ray meets it, the sky and the domes where it leaves */\n"
	"vec3 seen(vec3 o, vec3 d, int bounces)\n"
	"{\n"
	"    vec3 nh;\n"
	"    uint t;\n"
	"    vec2 b;\n"
	"    if (hit(o, d, 300.0, nh, t, b) < 0.0)\n"
	"        return sky_along(d, true) + ambient();\n"
	"    bool backface;\n"
	"    return path(o, d, bounces, backface);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    uint first = push.parameters.x, count = push.parameters.y, samples = push.parameters.z;\n"
	"    uint side = uint(push.values.y), levels = uint(push.values.z);\n"
	"    int bounces = int(push.values.w);\n"
	"    uint g = gl_GlobalInvocationID.x;\n"
	"    Floats pos = Floats(j.positions);\n"
	"    if (push.values.x < 0.5)\n"
	"    {\n"
	"        /* (each probe's box: how far its six axes reach, a world unit at\n"
	"        least, 64 at most; its place and first texel) */\n"
	"        if (g >= count)\n"
	"            return;\n"
	"        uint probe = first + g;\n"
	"        vec3 p = vec3(pos.v[probe * 4u], pos.v[probe * 4u + 1u], pos.v[probe * 4u + 2u]);\n"
	"        vec3 lo, hi;\n"
	"        for (int axis = 0; axis < 3; axis++)\n"
	"        {\n"
	"            for (int sign = 0; sign < 2; sign++)\n"
	"            {\n"
	"                vec3 d = vec3(0.0);\n"
	"                d[axis] = sign == 0 ? 1.0 : -1.0;\n"
	"                vec3 nh;\n"
	"                uint t;\n"
	"                vec2 b;\n"
	"                float th = hit(p, d, 64.0, nh, t, b);\n"
	"                float reach = clamp(th < 0.0 ? 64.0 : th, 1.0, 64.0);\n"
	"                if (sign == 0)\n"
	"                    hi[axis] = p[axis] + reach;\n"
	"                else\n"
	"                    lo[axis] = p[axis] - reach;\n"
	"            }\n"
	"        }\n"
	"        Floats inf = Floats(j.info);\n"
	"        uint texels = uint(push.values.y) * uint(push.values.y) * 6u;\n"
	"        uint total = 0u;\n"
	"        for (uint q = 0u; q < levels; q++)\n"
	"            total += 6u * max(side >> q, 1u) * max(side >> q, 1u);\n"
	"        inf.v[probe * 12u] = p.x;\n"
	"        inf.v[probe * 12u + 1u] = p.y;\n"
	"        inf.v[probe * 12u + 2u] = p.z;\n"
	"        inf.v[probe * 12u + 3u] = uintBitsToFloat(probe * total);\n"
	"        inf.v[probe * 12u + 4u] = lo.x;\n"
	"        inf.v[probe * 12u + 5u] = lo.y;\n"
	"        inf.v[probe * 12u + 6u] = lo.z;\n"
	"        inf.v[probe * 12u + 8u] = hi.x;\n"
	"        inf.v[probe * 12u + 9u] = hi.y;\n"
	"        inf.v[probe * 12u + 10u] = hi.z;\n"
	"        return;\n"
	"    }\n"
	"    uint total = 0u;\n"
	"    for (uint q = 0u; q < levels; q++)\n"
	"        total += 6u * max(side >> q, 1u) * max(side >> q, 1u);\n"
	"    if (g >= count * total)\n"
	"        return;\n"
	"    uint probe = first + g / total, texel = g % total, level = 0u, sz = side;\n"
	"    while (level + 1u < levels && texel >= 6u * sz * sz)\n"
	"    {\n"
	"        texel -= 6u * sz * sz;\n"
	"        level++;\n"
	"        sz = max(side >> level, 1u);\n"
	"    }\n"
	"    uint face = texel / (sz * sz), within = texel % (sz * sz);\n"
	"    uvec2 xy = uvec2(within % sz, within / sz);\n"
	"    seed = hash_u(g ^ hash_u(push.parameters.w * 2654435761u + probe));\n"
	"    vec3 p = vec3(pos.v[probe * 4u], pos.v[probe * 4u + 1u], pos.v[probe * 4u + 2u]);\n"
	"    float roughness = levels > 1u ? float(level) / float(levels - 1u) : 0.0;\n"
	"    roughness *= roughness;\n"
	"    float a = max(roughness * roughness, 1.0e-4);\n"
	"    vec3 sum = vec3(0.0);\n"
	"    float weights = 0.0;\n"
	"    for (uint k = 0u; k < samples; k++)\n"
	"    {\n"
	"        vec2 uv = (vec2(xy) + vec2(rnd(), rnd())) / float(sz) * 2.0 - 1.0;\n"
	"        vec3 n = cube_dir(face, uv);\n"
	"        vec3 l = n;\n"
	"        float w = 1.0;\n"
	"        if (level > 0u)\n"
	"        {\n"
	"            /* (GGX's half vectors about n, the view along n: the light's\n"
	"            direction by each, weighed by its cosine) */\n"
	"            float u1 = rnd(), u2 = rnd() * 6.2831853;\n"
	"            float ct = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1)), st = sqrt(max(1.0 - ct * ct, 0.0));\n"
	"            vec3 t, b;\n"
	"            basis(n, t, b);\n"
	"            vec3 h = normalize(st * cos(u2) * t + st * sin(u2) * b + ct * n);\n"
	"            l = 2.0 * dot(n, h) * h - n;\n"
	"            w = dot(n, l);\n"
	"            if (w <= 0.0)\n"
	"                continue;\n"
	"        }\n"
	"        vec3 c = seen(p, normalize(l), bounces);\n"
	"        if (any(isnan(c)) || any(isinf(c)))\n"
	"            continue;\n"
	"        sum += c * w;\n"
	"        weights += w;\n"
	"    }\n"
	"    Uints(j.data).v[probe * total + (g % total)] = pack_rgb9e5(weights > 0.0 ? sum / weights : vec3(0.0));\n"
	"}\n";

/* PRISM Raster's light (display.lighting "raster", design section 1.8):
each pixel of the window's colour target (the light target: CE's light
until here) lit from the bake where the bake has it. A level's pixel (its
G-buffer's lightmap page and coordinates): its CE texel's chart's virtual
page, its texels read bilinearly through the virtual page's tile table (a
tile's 256 texels each two words: the light less the sun's, RGB9E5, and
its direction's k and a0, RGBA8), the light at the pixel's normal, and the
sun's light by its facing and the sun mask (the tile's state: all dark, all
lit, or its mask's 64 x 64 bytes, read bilinearly), plus the emissive light
CE's lightmap pass folds in. An object's: the irradiance volume about its
point (its bricks' probes, trilinear, those inside the level left out), at
its normal, and the sun's light by the probes' visibility. Written into the
light target (r16g16b16a16 or the window's 8-bit target: the light in CE's
encoding, pow(light, 1/gamma) where the rig was fitted so). The views
(XGPU_GBUFFER_VIEW_BAKE_*) write theirs in the frame's place. Bindings: 0
the G-buffer's words, 1 its second target, 2 the light target (storage);
the job block by address: [0] the pages' chart maps' offsets (a uint each
page: its first word; then the page's width and height), [1] the chart maps
(a uint16 per CE texel: virtual page + 1), [2] the virtual pages (8 words:
page, scale, tiles across, down, the tile table's first, 0...), [3] the
tile table (a uint per tile: its physical tile + 1, 0 none), [4] the tiles
(512 words each), [5] the sun's table (a uint per tile: 0 unknown, 1 dark,
2 lit, 3 + its mask), [6] the masks (1024 words each), [7] the volume's
brick table (a hashed table: key x, y, z packed, its first probe; 4 words),
then values: the camera (eye, the axis, its x and y: 4 vec4 in the block's
extra words); parameters: x the view, y the pages, z the volume's brick
table's entries, w the flags (1 gamma 2.2) */
/* the running ones (state 0) of parameters.x states packed: each one's
index appended to the list, the dispatch's groups (64 a group) and the
count kept in the arguments (x, 1, 1, count; reset to 0, 1, 1, 0 before).
The job block: [0] the states, [1] the list, [2] the arguments */
static const char compact_source[] =
	BAKE_COMMON
	"layout(local_size_x = 256) in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 states, list, args, unused[5]; };\n"
	"void main()\n"
	"{\n"
	"    Job j = Job(push.job);\n"
	"    uint index = gl_GlobalInvocationID.x;\n"
	"    if (index >= push.parameters.x || Uints(j.states).v[index] != 0u)\n"
	"        return;\n"
	"    Uints a = Uints(j.args);\n"
	"    uint at = atomicAdd(a.v[3], 1u);\n"
	"    Uints(j.list).v[at] = index;\n"
	"    atomicMax(a.v[0], (at * max(push.parameters.y, 1u)) / 64u + 1u);\n"
	"}\n";

/* PRISM v4's exposure (Raster's frame; the composite to come takes the same
meter and step): the meter of the frame's lit pixels, the lit frame before
the exposure (the bake's light by the materials, Raster's) and Classic's at the
same pixels (CE's light target by its base, its additions added), their
luminances' histograms (log2 from -12 to 4, a quarter of a stop a bin,
64 bins each: as xgpu_vulkan_rt_glsl.inc's for the path tracer), filled by
the frame's pass (meter_add: a pixel in 16, 4 by 4) and read by the exposure
step after it (exposure_source), which keeps the exposure (log2) for the
next frame and empties the histograms. The display's tone curve over the
lit light after it (tone_curved: display.prism_tone_curve's). */
#define EXPOSURE_COMMON \
	"layout(buffer_reference, std430, buffer_reference_align = 16) buffer Meter\n" \
	"{\n" \
	"    uint histogram[128];\n" \
	"    float exposure;\n" \
	"    uint made;\n" \
	"    uint pad[2];\n" \
	"};\n" \
	"uint meter_bin(float l) { return uint(clamp((log2(max(l, 1.0e-6)) + 12.0) * 4.0, 0.0, 63.0)); }\n" \
	"/* a pixel's two luminances counted (the lit frame's, Classic's) */\n" \
	"void meter_add(uvec2 meter, float lit, float ce)\n" \
	"{\n" \
	"    Meter m = Meter(meter);\n" \
	"    atomicAdd(m.histogram[meter_bin(lit)], 1u);\n" \
	"    atomicAdd(m.histogram[64u + meter_bin(ce)], 1u);\n" \
	"}\n" \
	"/* the exposure the last step made (a factor; 1 before the first) */\n" \
	"float exposure_of(uvec2 meter)\n" \
	"{\n" \
	"    Meter m = Meter(meter);\n" \
	"    return m.made == 1u ? exp2(clamp(m.exposure, -8.0, 8.0)) : 1.0;\n" \
	"}\n" \
	"/* display.prism_tone_curve over a lit colour (1 white): 1 filmic (CE's light\n" \
	"as it is up to 0.6 of white, past it rolled off toward white, the colour's\n" \
	"brightest channel's, its hue kept; \"local\" here as filmic), 2 Narkowicz's\n" \
	"fit of ACES's, else as it is (\"ce\": the display clips it) */\n" \
	"vec3 tone_curved(vec3 c, int mode)\n" \
	"{\n" \
	"    c = max(c, vec3(0.0));\n" \
	"    if (mode == 2)\n" \
	"        return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);\n" \
	"    if (mode != 1 && mode != 3)\n" \
	"        return c;\n" \
	"    float brightest = max(c.r, max(c.g, c.b));\n" \
	"    const float k = 0.6;\n" \
	"    float mapped = brightest <= k ? brightest : k + (1.0 - k) * (1.0 - exp(-(brightest - k) / (1.0 - k)));\n" \
	"    return brightest > 1.0e-5 ? c * (mapped / brightest) : c;\n" \
	"}\n"

/* the exposure step, a single invocation after the frame's pass: the push
constants' scene the meter's address; parameters.x display.prism_exposure (0
fixed, 1 "ce": the lit frame's median pinned to Classic's over the same
pixels, within the range; 2 "auto": to a middle grey, 0.18, EV -2 to 5 about
it); values: the range (stops), the speed (stops a second),
the bias (stops, added) and the seconds since the last step. Moved toward
its aim at the speed, at once the first time; the histograms emptied */
static const char exposure_source[] =
	BAKE_COMMON
	EXPOSURE_COMMON
	"layout(local_size_x = 1) in;\n"
	"float median_of(Meter m, uint first)\n"
	"{\n"
	"    uint total = 0u;\n"
	"    for (uint i = 0u; i < 64u; i++)\n"
	"        total += m.histogram[first + i];\n"
	"    if (total < 16u)\n"
	"        return 99.0;\n"
	"    uint half_ = total / 2u, sum = 0u;\n"
	"    for (uint i = 0u; i < 64u; i++)\n"
	"    {\n"
	"        sum += m.histogram[first + i];\n"
	"        if (sum >= half_)\n"
	"            return (float(i) + 0.5) / 4.0 - 12.0;\n"
	"    }\n"
	"    return 99.0;\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    Meter m = Meter(push.scene);\n"
	"    uint mode = push.parameters.x;\n"
	"    float range = push.values.x, speed = push.values.y, bias = push.values.z, seconds = push.values.w;\n"
	"    float lit = median_of(m, 0u), ce = median_of(m, 64u);\n"
	"    float aim = 0.0;\n"
	"    bool measured = false;\n"
	"    if (mode == 2u && lit < 90.0)\n"
	"    {\n"
	"        aim = clamp(log2(0.18) - lit, -2.0, 5.0) + bias;\n"
	"        measured = true;\n"
	"    }\n"
	"    else if (mode == 1u && lit < 90.0 && ce < 90.0)\n"
	"    {\n"
	"        aim = clamp(ce - lit, -range, range) + bias;\n"
	"        measured = true;\n"
	"    }\n"
	"    /* (no measure this frame, none of the level's pixels seen: kept) */\n"
	"    float now = m.made == 1u ? m.exposure : aim;\n"
	"    if (mode == 0u)\n"
	"        now = aim = 0.0;\n"
	"    else if (isnan(now) || isinf(now))\n"
	"        now = aim;\n"
	"    if (!measured && mode != 0u)\n"
	"        aim = now;\n"
	"    float step_ = speed * seconds;\n"
	"    m.exposure = now + clamp(aim - now, -step_, step_);\n"
	"    m.made = 1u;\n"
	"    for (uint i = 0u; i < 128u; i++)\n"
	"        m.histogram[i] = 0u;\n"
	"}\n";

static const char raster_source[] =
	BAKE_COMMON
	EXPOSURE_COMMON
	"layout(local_size_x = 8, local_size_y = 8) in;\n"
	"layout(set = 0, binding = 0) uniform usampler2D words;\n"
	"layout(set = 0, binding = 1) uniform usampler2D extras;\n"
	"layout(set = 0, binding = 2) uniform sampler2D light_in;\n"
	"layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D light;\n"
	"layout(set = 0, binding = 4) uniform sampler2D base_in;\n"
	"layout(set = 0, binding = 5) uniform sampler2D additions_in;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Job\n"
	"{\n"
	"    uvec2 pages, charts, vpages, table, tiles, sun, masks, bricks;\n"
	"    uvec2 probes, coarse, objects, object_table;\n"
	"    vec4 camera[4];\n"
	"    ivec4 corners;\n"
	"    vec4 volume;\n"
	"    uvec4 counts;\n"
	"    uvec2 lights, reflections, reflection_table, reflection_data;\n"
	"    uvec4 more;\n"
	"    uvec2 meter, meter_pad;\n"
	"    vec4 exposure;\n"
	"};\n"
	"vec3 unfold(vec2 f)\n"
	"{\n"
	"    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));\n"
	"    float t = max(-n.z, 0.0);\n"
	"    n.x += n.x >= 0.0 ? -t : t;\n"
	"    n.y += n.y >= 0.0 ? -t : t;\n"
	"    return normalize(n);\n"
	"}\n"
	"vec3 rgb9e5(uint v)\n"
	"{\n"
	"    float e = exp2(float(int(v >> 27u) - 15 - 9));\n"
	"    return vec3(float(v & 511u), float((v >> 9u) & 511u), float((v >> 18u) & 511u)) * e;\n"
	"}\n"
	"Job job;\n"
	"/* a virtual page's texel (x, y): its light and direction words, or none */\n"
	"bool texel_of(uint vp, ivec2 x, out uvec2 w)\n"
	"{\n"
	"    Uints v = Uints(job.vpages);\n"
	"    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];\n"
	"    w = uvec2(0u);\n"
	"    if (x.x < 0 || x.y < 0)\n"
	"        return false;\n"
	"    uvec2 t = uvec2(x) / 16u;\n"
	"    if (t.x >= tiles_x || t.y >= tiles_y)\n"
	"        return false;\n"
	"    uint physical = Uints(job.table).v[v.v[vp * 8u + 4u] + t.y * tiles_x + t.x];\n"
	"    if (physical == 0u)\n"
	"        return false;\n"
	"    uvec2 l = uvec2(x) % 16u;\n"
	"    uint at = (physical - 1u) * 512u + (l.y * 16u + l.x) * 2u;\n"
	"    Uints tl = Uints(job.tiles);\n"
	"    w = uvec2(tl.v[at], tl.v[at + 1u]);\n"
	"    return (w.y & 0xff000000u) != 0u || w.x != 0u || w.y != 0u;\n"
	"}\n"
	"/* the sun's visibility at a virtual page's point (texels, continuous) */\n"
	"float sun_of(uint vp, vec2 x)\n"
	"{\n"
	"    Uints v = Uints(job.vpages);\n"
	"    uint tiles_x = v.v[vp * 8u + 2u], tiles_y = v.v[vp * 8u + 3u];\n"
	"    ivec2 t = ivec2(floor(x / 16.0));\n"
	"    if (any(lessThan(t, ivec2(0))) || t.x >= int(tiles_x) || t.y >= int(tiles_y))\n"
	"        return -1.0;\n"
	"    uint state = Uints(job.sun).v[v.v[vp * 8u + 4u] + uint(t.y) * tiles_x + uint(t.x)];\n"
	"    if (state == 0u)\n"
	"        return -1.0;\n"
	"    if (state < 3u)\n"
	"        return state == 2u ? 1.0 : 0.0;\n"
	"    /* (the mask's 64 x 64 bytes, bilinear within the tile) */\n"
	"    vec2 m = clamp((x - vec2(t) * 16.0) * 4.0 - 0.5, vec2(0.0), vec2(63.0));\n"
	"    ivec2 a = ivec2(floor(m));\n"
	"    ivec2 b = min(a + 1, ivec2(63));\n"
	"    vec2 f = m - vec2(a);\n"
	"    Uints mk = Uints(job.masks);\n"
	"    uint base = (state - 3u) * 4096u;\n"
	"    uint i00 = base + uint(a.y * 64 + a.x), i10 = base + uint(a.y * 64 + b.x);\n"
	"    uint i01 = base + uint(b.y * 64 + a.x), i11 = base + uint(b.y * 64 + b.x);\n"
	"    float v00 = float((mk.v[i00 >> 2u] >> ((i00 & 3u) * 8u)) & 255u);\n"
	"    float v10 = float((mk.v[i10 >> 2u] >> ((i10 & 3u) * 8u)) & 255u);\n"
	"    float v01 = float((mk.v[i01 >> 2u] >> ((i01 & 3u) * 8u)) & 255u);\n"
	"    float v11 = float((mk.v[i11 >> 2u] >> ((i11 & 3u) * 8u)) & 255u);\n"
	"    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y) / 255.0;\n"
	"}\n"
	"/* a cascade of the irradiance volume at p: its bricks' table (entries, a\n"
	"power of 2), its probes' spacing (a brick 4 of them a side); at the normal\n"
	"n and at the direction r: the light over pi at each (rgb), the sun's\n"
	"visibility (w); -1 none */\n"
	"bool cascade_at(uvec2 table, uint entries, float spacing, vec3 p, vec3 n, vec3 r, out vec4 at_n, out vec3 at_r)\n"
	"{\n"
	"    at_n = vec4(-1.0);\n"
	"    at_r = vec3(0.0);\n"
	"    if (entries == 0u || table == uvec2(0u))\n"
	"        return false;\n"
	"    vec3 f = p / spacing - 0.5;\n"
	"    ivec3 g0 = ivec3(floor(f));\n"
	"    vec3 t = f - vec3(g0);\n"
	"    vec3 sum = vec3(0.0), sum_r = vec3(0.0);\n"
	"    float sun = 0.0, weights = 0.0;\n"
	"    Uints bt = Uints(table);\n"
	"    Floats pr = Floats(job.probes);\n"
	"    vec4 yn = vec4(0.282095 * 3.14159265, 0.488603 * 2.0943951 * n.y, 0.488603 * 2.0943951 * n.z,\n"
	"        0.488603 * 2.0943951 * n.x);\n"
	"    vec4 yr = vec4(0.282095 * 3.14159265, 0.488603 * 2.0943951 * r.y, 0.488603 * 2.0943951 * r.z,\n"
	"        0.488603 * 2.0943951 * r.x);\n"
	"    for (int c = 0; c < 8; c++)\n"
	"    {\n"
	"        ivec3 g = g0 + ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);\n"
	"        ivec3 brick = ivec3(floor(vec3(g) / 4.0));\n"
	"        ivec3 local = g - brick * 4;\n"
	"        ivec3 key3 = brick + ivec3(32768);\n"
	"        uint key = (uint(key3.x) & 2047u) | ((uint(key3.y) & 2047u) << 11u) | ((uint(key3.z) & 1023u) << 22u);\n"
	"        uint h = hash_u(key ^ 0x68e31da4u) & (entries - 1u);\n"
	"        int first = -1;\n"
	"        for (uint probe = 0u; probe < 16u; probe++)\n"
	"        {\n"
	"            uint at = ((h + probe) & (entries - 1u)) * 2u;\n"
	"            uint k = bt.v[at];\n"
	"            if (k == 0u)\n"
	"                break;\n"
	"            if (k == key + 1u)\n"
	"            {\n"
	"                first = int(bt.v[at + 1u]);\n"
	"                break;\n"
	"            }\n"
	"        }\n"
	"        if (first < 0)\n"
	"            continue;\n"
	"        uint index = uint(first) + uint(local.z * 16 + local.y * 4 + local.x);\n"
	"        uint b = index * 8u;\n"
	"        float valid = pr.v[b + 7u];\n"
	"        if (valid <= 0.0)\n"
	"            continue;\n"
	"        vec3 w3 = mix(1.0 - t, t, vec3(c & 1, (c >> 1) & 1, (c >> 2) & 1));\n"
	"        float w = w3.x * w3.y * w3.z;\n"
	"        vec4 cr = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b])), unpackHalf2x16(floatBitsToUint(pr.v[b + 1u])));\n"
	"        vec4 cg = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b + 2u])), unpackHalf2x16(floatBitsToUint(pr.v[b + 3u])));\n"
	"        vec4 cb = vec4(unpackHalf2x16(floatBitsToUint(pr.v[b + 4u])), unpackHalf2x16(floatBitsToUint(pr.v[b + 5u])));\n"
	"        sum += w * max(vec3(dot(cr, yn), dot(cg, yn), dot(cb, yn)) / 3.14159265, vec3(0.0));\n"
	"        sum_r += w * max(vec3(dot(cr, yr), dot(cg, yr), dot(cb, yr)) / 3.14159265, vec3(0.0));\n"
	"        sun += w * pr.v[b + 6u];\n"
	"        weights += w;\n"
	"    }\n"
	"    if (weights <= 1.0e-3)\n"
	"        return false;\n"
	"    at_n = vec4(sum / weights, sun / weights);\n"
	"    at_r = sum_r / weights;\n"
	"    return true;\n"
	"}\n"
	"/* the volume at p: its fine cascade, else its coarse one */\n"
	"bool volume_at(vec3 p, vec3 n, vec3 r, out vec4 at_n, out vec3 at_r)\n"
	"{\n"
	"    if (cascade_at(job.bricks, job.counts.x, job.volume.x, p, n, r, at_n, at_r))\n"
	"        return true;\n"
	"    return cascade_at(job.coarse, job.counts.w, job.volume.y, p, n, r, at_n, at_r);\n"
	"}\n"
	"/* whether a moving object (this frame's, display.prism_rt's structure of\n"
	"the objects drawn: its cutouts kept by their share of texels drawn) blocks\n"
	"the ray */\n"
	"bool objects_blocked(vec3 o, vec3 d, float t_max)\n"
	"{\n"
	"    if (job.objects == uvec2(0u))\n"
	"        return false;\n"
	"    rayQueryEXT q;\n"
	"    rayQueryInitializeEXT(q, accelerationStructureEXT(job.objects), gl_RayFlagsTerminateOnFirstHitEXT, 12u, o, 0.02, d,\n"
	"        t_max);\n"
	"    while (rayQueryProceedEXT(q))\n"
	"    {\n"
	"        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT)\n"
	"            continue;\n"
	"        uint custom = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);\n"
	"        float coverage = custom >= 1u ? uintBitsToFloat(Uints(job.object_table).v[(custom - 1u) * 4u + 3u]) : 1.0;\n"
	"        if (rnd() < 0.75 * coverage)\n"
	"            rayQueryConfirmIntersectionEXT(q);\n"
	"    }\n"
	"    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;\n"
	"}\n"
	"/* the reflection probes (the bake's, in reachable space): the nearest few\n"
	"to p whose boxes hold it, parallax-corrected along r, at the roughness's\n"
	"level; -1 none */\n"
	"vec3 reflection_at(vec3 p, vec3 r, float roughness, out float found)\n"
	"{\n"
	"    found = 0.0;\n"
	"    uint count = job.more.x;\n"
	"    if (count == 0u || job.reflections == uvec2(0u))\n"
	"        return vec3(0.0);\n"
	"    Floats pr = Floats(job.reflections);\n"
	"    /* (a hashed grid of the probes, 8 world units a cell: each cell's 8\n"
	"    nearest probes) */\n"
	"    ivec3 cell = ivec3(floor(p / 8.0));\n"
	"    uint entries = job.more.y;\n"
	"    uint key = (uint(cell.x + 32768) & 2047u) | ((uint(cell.y + 32768) & 2047u) << 11u) |\n"
	"        ((uint(cell.z + 32768) & 1023u) << 22u);\n"
	"    uint h = hash_u(key ^ 0x2c1b3c6du) & (entries - 1u);\n"
	"    Uints rt = Uints(job.reflection_table);\n"
	"    int list = -1;\n"
	"    for (uint probe = 0u; probe < 32u; probe++)\n"
	"    {\n"
	"        uint at = ((h + probe) & (entries - 1u)) * 10u;\n"
	"        uint k = rt.v[at];\n"
	"        if (k == 0u)\n"
	"            break;\n"
	"        if (k == key + 1u)\n"
	"        {\n"
	"            list = int(at);\n"
	"            break;\n"
	"        }\n"
	"    }\n"
	"    if (list < 0)\n"
	"        return vec3(0.0);\n"
	"    vec3 sum = vec3(0.0);\n"
	"    float weights = 0.0;\n"
	"    uint levels = job.more.z, side = job.more.w;\n"
	"    float level = clamp(sqrt(roughness) * float(levels - 1u), 0.0, float(levels - 1u));\n"
	"    for (uint k = 0u; k < 8u; k++)\n"
	"    {\n"
	"        uint i = rt.v[uint(list) + 2u + k];\n"
	"        if (i == 0xffffffffu)\n"
	"            break;\n"
	"        /* (a probe: its place, its box's corners, its first texel) */\n"
	"        uint at = i * 12u;\n"
	"        vec3 c = vec3(pr.v[at], pr.v[at + 1u], pr.v[at + 2u]);\n"
	"        vec3 lo = vec3(pr.v[at + 4u], pr.v[at + 5u], pr.v[at + 6u]);\n"
	"        vec3 hi = vec3(pr.v[at + 8u], pr.v[at + 9u], pr.v[at + 10u]);\n"
	"        if (any(lessThan(p, lo - 0.5)) || any(greaterThan(p, hi + 0.5)))\n"
	"            continue;\n"
	"        /* (parallax: the box's wall along r from p, seen from the probe) */\n"
	"        vec3 rr = mix(r, vec3(1.0e-5), lessThan(abs(r), vec3(1.0e-5)));\n"
	"        vec3 t1 = (hi - p) / rr, t2 = (lo - p) / rr;\n"
	"        float tw = max(min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z)), 0.0);\n"
	"        vec3 d = normalize(p + r * tw - c);\n"
	"        /* (the cube's face and texel) */\n"
	"        vec3 a = abs(d);\n"
	"        uint face;\n"
	"        vec2 uv;\n"
	"        if (a.x >= a.y && a.x >= a.z)\n"
	"        {\n"
	"            face = d.x > 0.0 ? 0u : 1u;\n"
	"            uv = vec2(d.x > 0.0 ? -d.z : d.z, -d.y) / a.x;\n"
	"        }\n"
	"        else if (a.y >= a.z)\n"
	"        {\n"
	"            face = d.y > 0.0 ? 2u : 3u;\n"
	"            uv = vec2(d.x, d.y > 0.0 ? d.z : -d.z) / a.y;\n"
	"        }\n"
	"        else\n"
	"        {\n"
	"            face = d.z > 0.0 ? 4u : 5u;\n"
	"            uv = vec2(d.z > 0.0 ? d.x : -d.x, -d.y) / a.z;\n"
	"        }\n"
	"        vec3 l2 = vec3(0.0);\n"
	"        for (int lv = 0; lv < 2; lv++)\n"
	"        {\n"
	"            uint li = min(uint(level) + uint(lv), levels - 1u);\n"
	"            uint s_ = max(side >> li, 1u);\n"
	"            uint first = floatBitsToUint(pr.v[at + 3u]);\n"
	"            /* (the levels before li: 6 faces each) */\n"
	"            uint offset = 0u;\n"
	"            for (uint q = 0u; q < li; q++)\n"
	"                offset += 6u * max(side >> q, 1u) * max(side >> q, 1u);\n"
	"            /* (bilinear within the face, its edges clamped) */\n"
	"            vec2 fx = clamp((uv * 0.5 + 0.5) * float(s_) - 0.5, vec2(0.0), vec2(float(s_) - 1.0));\n"
	"            ivec2 t0 = ivec2(floor(fx)), t1 = min(t0 + 1, ivec2(int(s_) - 1));\n"
	"            vec2 ff = fx - vec2(t0);\n"
	"            uint fb = first + offset + face * s_ * s_;\n"
	"            Uints rd = Uints(job.reflection_data);\n"
	"            vec3 value = mix(mix(rgb9e5(rd.v[fb + uint(t0.y) * s_ + uint(t0.x)]), rgb9e5(rd.v[fb + uint(t0.y) * s_ + uint(t1.x)]), ff.x),\n"
	"                mix(rgb9e5(rd.v[fb + uint(t1.y) * s_ + uint(t0.x)]), rgb9e5(rd.v[fb + uint(t1.y) * s_ + uint(t1.x)]), ff.x), ff.y);\n"
	"            l2 += (lv == 0 ? 1.0 - fract(level) : fract(level)) * value;\n"
	"        }\n"
	"        float w = 1.0 / (0.25 + dot(p - c, p - c));\n"
	"        sum += w * l2;\n"
	"        weights += w;\n"
	"    }\n"
	"    if (weights <= 0.0)\n"
	"        return vec3(0.0);\n"
	"    found = 1.0;\n"
	"    return sum / weights;\n"
	"}\n"
	"/* the analytic split sum of GGX's (Karis's fit): F0's scale and bias */\n"
	"vec2 environment_brdf(float roughness, float nv)\n"
	"{\n"
	"    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022), c1 = vec4(1.0, 0.0425, 1.04, -0.04);\n"
	"    vec4 r = roughness * c0 + c1;\n"
	"    float a = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;\n"
	"    return vec2(-1.04, 1.04) * a + r.zw;\n"
	"}\n"
	"/* the dynamic lights (CE's: world_scene's, SCENE_LIGHTS' dynamic class): 4\n"
	"vec4 each (3 used: the place and reach, the colour by intensity, the spot's\n"
	"axis and cone's cosine, 2 a point), volume.z of them; CE's falloff,\n"
	"1 - d^2/r^2 */\n"
	"vec3 dynamic_at(vec3 p, vec3 n)\n"
	"{\n"
	"    uint count = uint(job.volume.z);\n"
	"    vec3 sum = vec3(0.0);\n"
	"    if (count == 0u || job.lights == uvec2(0u))\n"
	"        return sum;\n"
	"    Floats f = Floats(job.lights);\n"
	"    for (uint i = 0u; i < count; i++)\n"
	"    {\n"
	"        uint at = i * 12u;\n"
	"        vec3 to = vec3(f.v[at], f.v[at + 1u], f.v[at + 2u]) - p;\n"
	"        float r = f.v[at + 3u], d2 = dot(to, to);\n"
	"        if (r <= 0.0 || d2 >= r * r)\n"
	"            continue;\n"
	"        vec3 l = to * inversesqrt(max(d2, 1.0e-8));\n"
	"        float c = dot(n, n) > 0.5 ? max(dot(n, l), 0.0) : 1.0;\n"
	"        float cone = 1.0;\n"
	"        vec4 axis = vec4(f.v[at + 8u], f.v[at + 9u], f.v[at + 10u], f.v[at + 11u]);\n"
	"        if (axis.w < 1.5)\n"
	"            cone = smoothstep(axis.w, mix(axis.w, 1.0, 0.2), dot(-l, axis.xyz));\n"
	"        sum += vec3(f.v[at + 4u], f.v[at + 5u], f.v[at + 6u]) * (c * cone * (1.0 - d2 / (r * r)));\n"
	"    }\n"
	"    return sum;\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    job = Job(push.job);\n"
	"    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);\n"
	"    uint width = job.counts.y, height = job.counts.z;\n"
	"    if (pixel.x >= int(width) || pixel.y >= int(height))\n"
	"        return;\n"
	"    /* (the cutouts' stochastic shadows the same each frame: no flicker) */\n"
	"    seed = hash_u(uint(pixel.x) * 1973u + uint(pixel.y) * 9277u);\n"
	"    vec4 old = texelFetch(light_in, pixel, 0);\n"
	"    vec4 base = texelFetch(base_in, pixel, 0);\n"
	"    vec4 additions = texelFetch(additions_in, pixel, 0);\n"
	"    uvec4 w = texelFetch(words, pixel, 0);\n"
	"    uvec4 x = texelFetch(extras, pixel, 0);\n"
	"    uint view = push.parameters.x;\n"
	"    bool whole = (push.parameters.w & 2u) != 0u;\n"

	"    /* (CE's own frame at the pixel: what the resolve would have made) */\n"
	"    vec3 classic = old.rgb * base.rgb + additions.rgb;\n"
	"    bool present = (w.w & 0x8000u) != 0u;\n"
	"    uint instance = w.w >> 16;\n"
	"    uint page = (x.z >> 17u) & 0x7fu;\n"
	"    bool has_normal = ((w.z >> 28u) & 8u) != 0u;\n"
	"    vec3 n = has_normal ? unfold(unpackSnorm2x16(w.y)) : vec3(0.0);\n"
	"    float distance = uintBitsToFloat(x.w);\n"
	"    vec4 c = vec4(job.corners);\n"
	"    vec2 sxy = vec2(2.0 * (float(pixel.x) + 0.5 - c.x) / max(c.z - c.x, 1.0) - 1.0,\n"
	"        2.0 * (float(pixel.y) + 0.5 - c.y) / max(c.w - c.y, 1.0) - 1.0);\n"
	"    vec3 ray = job.camera[1].xyz + sxy.x * job.camera[2].xyz + sxy.y * job.camera[3].xyz;\n"
	"    vec3 point = job.camera[0].xyz + distance * ray;\n"
	"    vec3 eye_dir = -normalize(ray);\n"
	"    vec3 facing_normal = has_normal ? n : eye_dir;\n"
	"    if (dot(facing_normal, ray) > 0.0 && instance == 0u)\n"
	"        facing_normal = -facing_normal;\n"
	"    vec3 refl = reflect(-eye_dir, facing_normal);\n"
	"    float gamma = (push.parameters.w & 1u) != 0u ? 2.2 : 1.0;\n"
	"    vec3 lit = vec3(-1.0), lit_r = vec3(0.0);\n"
	"    float sunv = -1.0, checker = -1.0;\n"
	"    bool level = false;\n"
	"    /* (the texels' view's reasons where no texel is found: no page, no\n"
	"    chart at its CE texel, no baked texel about it) */\n"
	"    int reason = present && instance == 0u ? (page == 0u ? 1 : 0) : 0;\n"
	"    if (present && instance == 0u && page > 0u && page <= push.parameters.y)\n"
	"    {\n"
	"        Uints pg = Uints(job.pages);\n"
	"        uint offset = pg.v[(page - 1u) * 4u], pw = pg.v[(page - 1u) * 4u + 1u], ph = pg.v[(page - 1u) * 4u + 2u];\n"
	"        vec2 ce = unpackUnorm2x16(w.x) * vec2(pw, ph);\n"
	"        ivec2 cell = clamp(ivec2(floor(ce)), ivec2(0), ivec2(pw, ph) - 1);\n"
	"        Uints cm = Uints(job.charts);\n"
	"        uint ci = offset + uint(cell.y) * pw + uint(cell.x);\n"
	"        uint vp1 = (cm.v[ci >> 1u] >> ((ci & 1u) * 16u)) & 0xffffu;\n"
	"        reason = vp1 == 0u ? 2 : 3;\n"
	"        if (vp1 > 0u)\n"
	"        {\n"
	"            uint vp = vp1 - 1u;\n"
	"            float scale = float(Uints(job.vpages).v[vp * 8u + 1u]);\n"
	"            vec2 tx = ce * scale - 0.5;\n"
	"            ivec2 a = ivec2(floor(tx));\n"
	"            vec2 f = tx - vec2(a);\n"
	"            vec3 sum = vec3(0.0), sum_r = vec3(0.0);\n"
	"            float weights = 0.0;\n"
	"            for (int k = 0; k < 4; k++)\n"
	"            {\n"
	"                ivec2 o = ivec2(k & 1, k >> 1);\n"
	"                uvec2 tw;\n"
	"                if (!texel_of(vp, a + o, tw))\n"
	"                    continue;\n"
	"                vec4 dir = unpackUnorm4x8(tw.y);\n"
	"                if (dir.w <= 0.0)\n"
	"                    continue;\n"
	"                vec3 kk = (dir.xyz - 0.5) * 4.0;\n"
	"                float a0 = dir.w * 4.0 - 1.0;\n"
	"                float factor = has_normal ? clamp(a0 + dot(kk, facing_normal), 0.0, 4.0) : 1.0;\n"
	"                float factor_r = clamp(a0 + dot(kk, refl), 0.0, 4.0);\n"
	"                float wk = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n"
	"                vec3 e = rgb9e5(tw.x);\n"
	"                sum += wk * e * factor;\n"
	"                sum_r += wk * e * factor_r;\n"
	"                weights += wk;\n"
	"            }\n"
	"            if (weights > 1.0e-4)\n"
	"            {\n"
	"                lit = sum / weights;\n"
	"                lit_r = sum_r / weights;\n"
	"                sunv = sun_of(vp, ce * scale);\n"
	"                checker = float((a.x + a.y) & 1);\n"
	"                level = true;\n"
	"            }\n"
	"        }\n"
	"    }\n"
	"    if (present && distance > 0.0 && ((!level && instance != 0u) || view == 15u))\n"
	"    {\n"
	"        vec4 v;\n"
	"        vec3 vr;\n"
	"        if (volume_at(point + facing_normal * 0.05, facing_normal, refl, v, vr))\n"
	"        {\n"
	"            if (view == 15u || !level)\n"
	"            {\n"
	"                lit = v.rgb;\n"
	"                lit_r = vr;\n"
	"                sunv = v.w;\n"
	"                /* (an object's sun: its shadow traced against the level and\n"
	"                the static scenery, the volume's visibility being coarse) */\n"
	"                if (!level && s.sun.w > 0.0 && dot(facing_normal, s.sun.xyz) > 0.0)\n"
	"                    sunv = sky_blocked(point + facing_normal * 0.04, s.sun.xyz, sun_reach()) ? 0.0 : 1.0;\n"
	"            }\n"
	"        }\n"
	"        else if (view == 15u)\n"
	"            lit = vec3(-1.0);\n"
	"    }\n"
	"    vec3 result = whole ? classic : old.rgb;\n"
	"    vec3 raster_parts[3] = vec3[3](vec3(0.0), vec3(0.0), vec3(0.0));\n"
	"    if (lit.x >= 0.0)\n"
	"    {\n"
	"        float facing = max(dot(facing_normal, s.sun.xyz), 0.0);\n"
	"        /* (the moving objects' sun shadows: traced where the frame has their\n"
	"        structure) */\n"
	"        if (s.sun.w > 0.0 && sunv > 0.0 && facing > 0.0 && view == 0u &&\n"
	"            objects_blocked(point + facing_normal * 0.04, s.sun.xyz, sun_reach()))\n"
	"            sunv = 0.0;\n"
	"        /* (a level's pixel: the moving objects' shadows CE drew over its\n"
	"        lightmap pass (its projected shadow maps), their share of the\n"
	"        light the G-buffer says that pass gave, taken from the sun's) */\n"
	"        float ce_code = float(x.z >> 24u) / 255.0;\n"
	"        if (instance == 0u && level && ce_code > 0.0 && view == 0u)\n"
	"        {\n"
	"            vec3 em = vec3(unpackHalf2x16(x.y), unpackHalf2x16(x.z & 0xffffu).x);\n"
	"            float ce_light = ce_code * ce_code - luma(em);\n"
	"            float drawn = luma(max(old.rgb - em, vec3(0.0)));\n"
	"            if (ce_light > 0.02 && drawn < 0.97 * ce_light)\n"
	"                sunv *= clamp(drawn / ce_light, 0.0, 1.0);\n"
	"        }\n"
	"        float v = s.sun.w > 0.0 ? max(sunv, 0.0) : 0.0;\n"
	"        vec3 sunlight = s.rig[0].rgb * facing * v;\n"
	"        vec3 emissive = vec3(unpackHalf2x16(x.y), unpackHalf2x16(x.z & 0xffffu).x);\n"
	"        vec3 dynamic = dynamic_at(point + facing_normal * 0.02, facing_normal);\n"
	"        vec3 diffuse = max(lit + sunlight, vec3(0.0));\n"
	"        bool metered = view == 0u && job.exposure.z > 0.5 && (job.meter.x != 0u || job.meter.y != 0u);\n"
	"        float exposure = metered && job.exposure.x > 0.5 ? exposure_of(job.meter) : 1.0;\n"
	"        vec3 encoded = gamma > 1.001 ? pow(diffuse, vec3(1.0 / gamma)) : diffuse;\n"
	"        if (whole)\n"
	"        {\n"
	"            /* (the pixel's material: CE's base colour (the level's from its\n"
	"            texture pass, an object's the G-buffer's), the table's roughness,\n"
	"            metalness and specular; its light diffuse and specular, energy\n"
	"            conserving: Fresnel's share of the light reflected, the rest\n"
	"            diffuse) */\n"
	"            vec3 albedo = instance == 0u ? base.rgb : unpackUnorm4x8(w.x).rgb;\n"
	"            vec4 m = unpackUnorm4x8(w.z);\n"
	"            float roughness = clamp(m.x, 0.04, 1.0), metal = m.y;\n"
	"            /* (CE's base maps are diffuse colours, metal or not: metalness\n"
	"            tints the reflection half way to the base colour, and the\n"
	"            diffuse keeps what the reflection does not take) */\n"
	"            vec3 f0 = mix(vec3(0.08 * m.z), albedo, 0.5 * metal);\n"
	"            float nv = clamp(dot(facing_normal, eye_dir), 1.0e-3, 1.0);\n"
	"            vec2 ab = environment_brdf(roughness, nv);\n"
	"            vec3 spec_share = f0 * ab.x + ab.y;\n"
	"            /* (the light the reflection sees: the probes', else the bake's\n"
	"            light from the reflection's side) */\n"
	"            float found;\n"
	"            vec3 reflected = reflection_at(point + facing_normal * 0.05, refl, roughness, found);\n"
	"            /* (no probe: the volume's light from the reflection's side, its\n"
	"            SH converged and smooth; else the pixel's own light) */\n"
	"            if (found <= 0.0 && level)\n"
	"            {\n"
	"                vec4 vn;\n"
	"                vec3 vr;\n"
	"                lit_r = volume_at(point + facing_normal * 0.05, facing_normal, refl, vn, vr) ? vr : lit;\n"
	"            }\n"
	"            if (found <= 0.0)\n"
	"                reflected = gamma > 1.001 ? pow(max(lit_r, vec3(0.0)), vec3(1.0 / gamma)) : max(lit_r, vec3(0.0));\n"
	"            vec3 specular = reflected * spec_share;\n"
	"            /* (the sun's highlight: GGX, Smith's visibility, Schlick's\n"
	"            Fresnel; the sun's irradiance pi times the rig's) */\n"
	"            if (v > 0.0 && facing > 0.0)\n"
	"            {\n"
	"                vec3 hv = normalize(s.sun.xyz + eye_dir);\n"
	"                float nh = max(dot(facing_normal, hv), 0.0), vh = max(dot(eye_dir, hv), 0.0);\n"
	"                float a = roughness * roughness, a2 = a * a;\n"
	"                float dd = nh * nh * (a2 - 1.0) + 1.0;\n"
	"                float distribution = a2 / (3.14159265 * dd * dd);\n"
	"                float visibility = 0.5 / max(facing * (nv * (1.0 - a) + a) + nv * (facing * (1.0 - a) + a), 1.0e-4);\n"
	"                vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);\n"
	"                specular += min(distribution * visibility, 64.0) * fresnel * facing * v * 3.14159265 * s.rig[0].rgb;\n"
	"            }\n"
	"            vec3 kd = albedo * (1.0 - spec_share);\n"
	"            /* (the exposure on the light, then held at white as CE's page\n"
	"            held its light (a colour past white by its brightest channel, its\n"
	"            hue kept), before the base; the glow as CE's; then the tone\n"
	"            curve) */\n"
	"            vec3 glow = instance == 0u ? emissive * base.rgb : emissive;\n"
	"            /* (the exposure's meter: a lit pixel in 16 (every 4th each way),\n"
	"            this frame before the exposure against Classic's at the pixel, CE's\n"
	"            light target by its base and its additions: \"as bright as\n"
	"            Classic\" is the frame's, its materials' parts with the light) */\n"
	"            if (metered && (pixel.x & 3) == 0 && (pixel.y & 3) == 0)\n"
	"                meter_add(job.meter, luma(kd * (encoded + dynamic) + specular + glow), luma(classic));\n"
	"            vec3 light_ = (encoded + dynamic) * exposure;\n"
	"            float brightest = max(light_.r, max(light_.g, light_.b));\n"
	"            if (view == 0u && brightest > 1.0)\n"
	"                light_ /= brightest;\n"
	"            result = kd * light_ + specular * exposure + glow;\n"
	"            if (view == 0u)\n"
	"                result = tone_curved(result, int(job.exposure.y + 0.5));\n"
	"            raster_parts[0] = albedo;\n"
	"            raster_parts[1] = specular;\n"
	"            raster_parts[2] = kd * (encoded + dynamic);\n"
	"        }\n"
	"        else\n"
	"        {\n"
	"            /* (CE's composition (display.prism_raster_composition \"ce\"): the\n"
	"            light into CE's light target, which the resolve multiplies by CE's\n"
	"            base and adds CE's additions to; the exposure on it and held at\n"
	"            white as in the frame whole; the level's glow folded in as CE's\n"
	"            lightmap pass folds it, an object's base colour (the G-buffer's, its\n"
	"            combiners with white light) and glow as its combiners give them) */\n"
	"            vec3 light_ = (encoded + dynamic) * (view == 0u ? exposure : 1.0);\n"
	"            float brightest = max(light_.r, max(light_.g, light_.b));\n"
	"            if (view == 0u && brightest > 1.0)\n"
	"                light_ /= brightest;\n"
	"            result = light_ + (instance == 0u ? emissive : vec3(0.0));\n"
	"            if (instance != 0u)\n"
	"            {\n"
	"                result = light_ * unpackUnorm4x8(w.x).rgb + emissive;\n"
	"                /* (an object never lit under Classic's light for its pixel: the\n"
	"                volume's raised to CE's own (the light target's there, the\n"
	"                object's lit colour), its colour kept; over it, the volume's;\n"
	"                display.prism_raster_objects \"volume\": the volume's alone) */\n"
	"                float lr = luma(result), lc = luma(old.rgb);\n"
	"                if (view == 0u && (push.parameters.w & 4u) == 0u && lr < lc)\n"
	"                    result = lr > 1.0e-4 ? result * (lc / lr) : old.rgb;\n"
	"            }\n"
	"            /* (the meter, like with like: Raster's light and CE's by the same\n"
	"            base, the same additions added) */\n"
	"            if (metered && (pixel.x & 3) == 0 && (pixel.y & 3) == 0)\n"
	"            {\n"
	"                vec3 raw = (encoded + dynamic) * (instance == 0u ? vec3(1.0) : unpackUnorm4x8(w.x).rgb) +\n"
	"                    emissive;\n"
	"                meter_add(job.meter, luma(raw * base.rgb + additions.rgb), luma(classic));\n"
	"            }\n"
	"        }\n"
	"        /* (the views: the light less the sun's, in CE's encoding: the gates\n"
	"        put the sun's back by the pixel's normal and the visibility view) */\n"
	"        if (view == 12u || view == 15u)\n"
	"            result = gamma > 1.001 ? pow(max(lit, vec3(0.0)), vec3(1.0 / gamma)) : max(lit, vec3(0.0));\n"
	"        else if (view == 16u)\n"
	"            result = encoded;\n"
	"        else if (view == 13u)\n"
	"            result = vec3(max(sunv, 0.0) * (facing > 0.0 ? 1.0 : 0.25));\n"
	"    }\n"
	"    else if (view >= 12u && view <= 19u)\n"
	"        result = present ? vec3(0.5, 0.0, 0.0) : vec3(0.0);\n"
	"    /* (the frame's parts: its albedo, its specular light, its diffuse) */\n"
	"    if (view >= 17u && view <= 19u && lit.x >= 0.0)\n"
	"        result = raster_parts[view - 17u];\n"
	"    if (view == 14u)\n"
	"        result = checker >= 0.0 ? vec3(0.2 + 0.6 * checker) : !present ? vec3(0.0) : reason == 1 ?\n"
	"            vec3(0.8, 0.0, 0.0) : reason == 2 ? vec3(0.8, 0.0, 0.8) : reason == 3 ? vec3(0.8, 0.8, 0.0) :\n"
	"            vec3(0.0, 0.3, 0.8);\n"
	"    if (any(isnan(result)) || any(isinf(result)))\n"
	"        result = whole ? classic : old.rgb;\n"
	"    imageStore(light, pixel, vec4(result, whole ? additions.a : old.a));\n"
	"}\n";
