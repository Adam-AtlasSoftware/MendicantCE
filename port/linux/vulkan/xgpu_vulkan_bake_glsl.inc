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
	"uint scene_triangle(uint custom, uint primitive) { return custom == 0u ? primitive : primitive + s.counts.x; }\n" \
	"vec3 corner(uint t, uint k)\n" \
	"{\n" \
	"    Floats f = Floats(s.vertices);\n" \
	"    uint b = (t * 3u + k) * 3u;\n" \
	"    return vec3(f.v[b], f.v[b + 1u], f.v[b + 2u]);\n" \
	"}\n" \
	"uint material_of(uint t) { return min(Uints(s.info).v[t * 8u] & 0xffffu, s.counts.w); }\n" \
	"bool two_sided(uint t) { return (Uints(s.info).v[t * 8u] & 0x10000u) != 0u; }\n" \
	"float material_value(uint m, uint k) { return Floats(s.materials).v[m * 16u + k]; }\n" \
	"\n" \
	"bool alpha_kept(uint primitive, vec2 b)\n" \
	"{\n" \
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
	"            alpha_kept(uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)), \\\n" \
	"            rayQueryGetIntersectionBarycentricsEXT(q, false))) \\\n" \
	"            rayQueryConfirmIntersectionEXT(q); }\n" \
	"\n" \
	"bool blocked(vec3 o, vec3 d, float t_max)\n" \
	"{\n" \
	"    rayQueryEXT q;\n" \
	"    rayQueryInitializeEXT(q, accelerationStructureEXT(s.tlas), gl_RayFlagsTerminateOnFirstHitEXT, 3u, o, 0.0, d,\n" \
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
	"    rayQueryInitializeEXT(q, accelerationStructureEXT(s.tlas), 0u, 3u, o, 0.0, d, t_max);\n" \
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
	"the lamps, 4 the glows, 8 the paths past the first hit; 0 none) */\n" \
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
	"/* the sun's visibility from o (a direction of its disc) */\n" \
	"float sun_visible(vec3 o)\n" \
	"{\n" \
	"    if (s.sun.w <= 0.0)\n" \
	"        return 0.0;\n" \
	"    vec3 d = cone_dir(s.sun.xyz, cos(0.5 * s.rig[0].w));\n" \
	"    return blocked(o, d, 2000.0) ? 0.0 : 1.0;\n" \
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
	"        if (c > 0.0 && !blocked(o, d, 2000.0))\n" \
	"        {\n" \
	"            sum += r.rgb * c;\n" \
	"            moment += luma(r.rgb) * d;\n" \
	"        }\n" \
	"    }\n" \
	"    return sum;\n" \
	"}\n" \
	"vec3 sky_along(vec3 d, bool domes)\n" \
	"{\n" \
	"    vec3 l = s.rig[1].rgb;\n" \
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
	"surfaces, each its key (x y z + 1 << 20 each, packed; 0 empty) and up to 64\n" \
	"lamps with their cumulative weights, by the light each gives the cell's\n" \
	"points with its shadow traced (BAKE_GRID_WORDS words a cell); a lamp is\n" \
	"picked from the cell about the point by s.lamp.w of the picks, else by\n" \
	"power over the level, so that every lamp can be picked */\n" \
	"const uint BAKE_GRID_WORDS = 131u;\n" \
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
	"            return int(at);\n" \
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
	"/* a lamp picked at p (the cell's by its weights, else by power) and the\n" \
	"probability of its pick */\n" \
	"uint lamp_pick(vec3 p, out float probability)\n" \
	"{\n" \
	"    int cell = grid_cell(p);\n" \
	"    Uints g = Uints(s.grid);\n" \
	"    uint count = cell >= 0 ? min(g.v[cell + 2], 64u) : 0u;\n" \
	"    /* (a cell whose points no lamp's light reaches: none) */\n" \
	"    if (cell >= 0 && count == 0u)\n" \
	"    {\n" \
	"        probability = 0.0;\n" \
	"        return 0u;\n" \
	"    }\n" \
	"    float share = count > 0u ? s.lamp.w : 0.0;\n" \
	"    uint l;\n" \
	"    if (count > 0u && rnd() < share)\n" \
	"    {\n" \
	"        float total = uintBitsToFloat(g.v[cell + 3 + 2 * int(count - 1u) + 1]);\n" \
	"        float u = rnd() * total;\n" \
	"        uint k = 0u;\n" \
	"        while (k + 1u < count && uintBitsToFloat(g.v[cell + 3 + 2 * int(k) + 1]) < u)\n" \
	"            k++;\n" \
	"        l = g.v[cell + 3 + 2 * int(k)];\n" \
	"    }\n" \
	"    else\n" \
	"        l = lamp_by_power();\n" \
	"    probability = (1.0 - share) * lamp_power(l) / max(s.lamp.x, 1.0e-20);\n" \
	"    if (count > 0u)\n" \
	"    {\n" \
	"        float total = uintBitsToFloat(g.v[cell + 3 + 2 * int(count - 1u) + 1]), below = 0.0;\n" \
	"        for (uint k = 0u; k < count; k++)\n" \
	"        {\n" \
	"            float c = uintBitsToFloat(g.v[cell + 3 + 2 * int(k) + 1]);\n" \
	"            if (g.v[cell + 3 + 2 * int(k)] == l)\n" \
	"                probability += share * (c - below) / max(total, 1.0e-20);\n" \
	"            below = c;\n" \
	"        }\n" \
	"    }\n" \
	"    return l;\n" \
	"}\n" \
	"\n" \
	"/* whether a lamp's point at distance d along l is seen from o: nothing\n" \
	"before it, or first another lamp's surface */\n" \
	"bool lamp_visible(vec3 o, vec3 l, float d)\n" \
	"{\n" \
	"    float most = max(d * 0.995 - 0.03, 0.0);\n" \
	"    if (!blocked(o, l, most))\n" \
	"        return true;\n" \
	"    vec3 nh;\n" \
	"    uint t;\n" \
	"    vec2 b;\n" \
	"    if (hit(o, l, most, nh, t, b) < 0.0)\n" \
	"        return true;\n" \
	"    uint m = material_of(t);\n" \
	"    return m < s.counts.w && (uint(material_value(m, 3u)) & 1u) != 0u;\n" \
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
	"    bool spherical = la > 1.0e-5 && lb > 1.0e-5 && lc > 1.0e-5;\n" \
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
	"vec3 lamps_at(vec3 p, vec3 n, vec3 o, inout vec3 moment)\n" \
	"{\n" \
	"    vec3 l;\n" \
	"    vec3 e = lamps_ris(p, n, o, 8, l);\n" \
	"    moment += luma(e) * 3.14159265 / max(dot(n, l), 0.1) * l;\n" \
	"    return e;\n" \
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
	"        vec3 ls = sun_at(oh, nh) + lamps_ris(h, nh, oh, 4, lh);\n" \
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
	"    float threshold = push.values.x, floor_ = push.values.y;\n" \
	"    uint minimum = push.parameters.z;\n" \
	"    if (count < minimum)\n" \
	"        return 0u;\n" \
	"    if (push.values.z > 0.0 && float(count) >= push.values.z)\n" \
	"        return hits == 0u ? 2u : 1u;\n" \
	"    if (hits == 0u)\n" \
	"        return count >= 4096u ? 2u : 0u;\n" \
	"    if (hits < 16u)\n" \
	"        return 0u;\n" \
	"    float scale = max(abs(mean), floor_);\n" \
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
	"        flags = 1u | (distance == 0.0 ? 2u : 0u) | ((floatBitsToUint(t.v[b + 15u]) & 1u) != 0u ? 4u : 0u);\n"
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
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 texels, accumulators, states, running, list, args, unused[2]; };\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    Job j = Job(push.job);\n"
	"    /* (the running texels alone: the compaction's list) */\n"
	"    if (gl_GlobalInvocationID.x >= Uints(j.args).v[3])\n"
	"        return;\n"
	"    uint index = Uints(j.list).v[gl_GlobalInvocationID.x];\n"
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
	"    seed = hash_u(index ^ hash_u(push.parameters.w * 2654435761u ^ hash_u(count + 0x9e3779b9u)));\n"
	"    int bounces = int(push.values.w);\n"
	"    /* (a remodel's face, two-sided, its normal turned toward its open side\n"
	"    once: the side whose rays travel further before they meet the level) */\n"
	"    if (count == 0u && (flags & 4u) != 0u)\n"
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
	"    uint samples = push.parameters.y;\n"
	"    /* (a remodel's face, two-sided: its normal toward the open side, the\n"
	"    side more of whose first rays leave or meet a face's front) */\n"
	"    for (uint k = 0u; k < samples; k++)\n"
	"    {\n"
	"        vec3 m = vec3(0.0);\n"
	"        vec3 dl = domes_at(o, n, m) + lamps_at(p, n, o, m);\n"
	"        vec3 d = cosine_dir_at(n, stratum(count + k));\n"
	"        bool backface;\n"
	"        vec3 il = path(o, d, bounces, backface);\n"
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
	"        batch_add(batch_sum, count + k, batches, batch_mean, batch_m2, x);\n"
	"        bc++;\n"
	"        bh += x != 0.0 ? 1u : 0u;\n"
	"        float delta = x - bmean;\n"
	"        bmean += delta / float(bc);\n"
	"        bm2 += delta * (x - bmean);\n"
	"        bmost = max(bmost, x);\n"
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
	"            lit += blocked(o, d, 2000.0) ? 0u : 1u;\n"
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
	"layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Job { uvec2 probes, accumulators, states, statistics, running, list, args, unused; };\n"
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
	"    seed = hash_u(index ^ hash_u(push.parameters.w * 2654435761u ^ hash_u(count + 0x7f4a7c15u)));\n"
	"    int bounces = int(push.values.w);\n"
	"    vec4 shr = vec4(0.0), shg = vec4(0.0), shb = vec4(0.0);\n"
	"    float sun = 0.0, back = 0.0;\n"
	"    uint bc = 0u, bh = 0u;\n"
	"    float bmean = 0.0, bm2 = 0.0, bmost = 0.0;\n"
	"    for (uint k = 0u; k < push.parameters.y; k++)\n"
	"    {\n"
	"        vec3 d = sphere_dir_at(stratum(count + k));\n"
	"        bool backface;\n"
	"        vec3 l = path(p, d, bounces, backface) * 12.566371;\n"
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
	"        }\n"
	"        /* (each dome: a direction of its cone, its radiance pi r / its\n"
	"        solid angle over its density 1 / that) */\n"
	"        for (int i = 0; i < 4; i++)\n"
	"        {\n"
	"            vec4 a = s.rig[2 + 2 * i], r = s.rig[3 + 2 * i];\n"
	"            if (dot(r.rgb, vec3(1.0)) <= 0.0)\n"
	"                continue;\n"
	"            vec3 dd = cone_dir(normalize(a.xyz), a.w);\n"
	"            if (!blocked(p, dd, 2000.0))\n"
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
	"    atomicMax(a.v[0], at / 64u + 1u);\n"
	"}\n";

static const char raster_source[] =
	BAKE_COMMON
	"layout(local_size_x = 8, local_size_y = 8) in;\n"
	"layout(set = 0, binding = 0) uniform usampler2D words;\n"
	"layout(set = 0, binding = 1) uniform usampler2D extras;\n"
	"layout(set = 0, binding = 2) uniform sampler2D light_in;\n"
	"layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D light;\n"
	"layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Job\n"
	"{\n"
	"    uvec2 pages, charts, vpages, table, tiles, sun, masks, bricks;\n"
	"    uvec2 probes, unused[3];\n"
	"    vec4 camera[4];\n"
	"    ivec4 corners;\n"
	"    vec4 volume;\n"
	"    uvec4 counts;\n"
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
	"/* the irradiance volume at p (normal n): the light over pi; and the sun's\n"
	"visibility (w); -1 none */\n"
	"vec4 volume_at(vec3 p, vec3 n)\n"
	"{\n"
	"    uint entries = job.counts.x;\n"
	"    if (entries == 0u)\n"
	"        return vec4(-1.0);\n"
	"    float spacing = job.volume.x;\n"
	"    vec3 f = p / spacing - 0.5;\n"
	"    ivec3 g0 = ivec3(floor(f));\n"
	"    vec3 t = f - vec3(g0);\n"
	"    vec3 sum = vec3(0.0);\n"
	"    float sun = 0.0, weights = 0.0;\n"
	"    Uints bt = Uints(job.bricks);\n"
	"    Floats pr = Floats(job.probes);\n"
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
	"        vec2 r0 = unpackHalf2x16(floatBitsToUint(pr.v[b])), r1 = unpackHalf2x16(floatBitsToUint(pr.v[b + 1u]));\n"
	"        vec2 g0_ = unpackHalf2x16(floatBitsToUint(pr.v[b + 2u])), g1 = unpackHalf2x16(floatBitsToUint(pr.v[b + 3u]));\n"
	"        vec2 b0 = unpackHalf2x16(floatBitsToUint(pr.v[b + 4u])), b1 = unpackHalf2x16(floatBitsToUint(pr.v[b + 5u]));\n"
	"        vec4 y = vec4(0.282095 * 3.14159265, 0.488603 * 2.0943951 * n.y, 0.488603 * 2.0943951 * n.z,\n"
	"            0.488603 * 2.0943951 * n.x);\n"
	"        vec3 e = vec3(dot(vec4(r0, r1), y), dot(vec4(g0_, g1), y), dot(vec4(b0, b1), y)) / 3.14159265;\n"
	"        sum += w * max(e, vec3(0.0));\n"
	"        sun += w * pr.v[b + 6u];\n"
	"        weights += w;\n"
	"    }\n"
	"    return weights > 1.0e-3 ? vec4(sum / weights, sun / weights) : vec4(-1.0);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"    s = Scene(push.scene);\n"
	"    job = Job(push.job);\n"
	"    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);\n"
	"    if (pixel.x >= int(job.counts.y) || pixel.y >= int(job.counts.z))\n"
	"        return;\n"
	"    vec4 old = texelFetch(light_in, pixel, 0);\n"
	"    uvec4 w = texelFetch(words, pixel, 0);\n"
	"    uvec4 x = texelFetch(extras, pixel, 0);\n"
	"    uint view = push.parameters.x;\n"
	"    bool present = (w.w & 0x8000u) != 0u;\n"
	"    uint instance = w.w >> 16;\n"
	"    uint page = (x.z >> 17u) & 0x7fu;\n"
	"    float ce_code = float(x.z >> 24u) / 255.0;\n"
	"    bool has_normal = ((w.z >> 28u) & 8u) != 0u;\n"
	"    vec3 n = has_normal ? unfold(unpackSnorm2x16(w.y)) : vec3(0.0);\n"
	"    float distance = uintBitsToFloat(x.w);\n"
	"    vec4 c = vec4(job.corners);\n"
	"    vec2 sxy = vec2(2.0 * (float(pixel.x) + 0.5 - c.x) / max(c.z - c.x, 1.0) - 1.0,\n"
	"        2.0 * (float(pixel.y) + 0.5 - c.y) / max(c.w - c.y, 1.0) - 1.0);\n"
	"    vec3 ray = job.camera[1].xyz + sxy.x * job.camera[2].xyz + sxy.y * job.camera[3].xyz;\n"
	"    vec3 point = job.camera[0].xyz + distance * ray;\n"
	"    vec3 facing_normal = has_normal ? n : -normalize(ray);\n"
	"    if (dot(facing_normal, ray) > 0.0 && instance == 0u)\n"
	"        facing_normal = -facing_normal;\n"
	"    float gamma = (push.parameters.w & 1u) != 0u ? 2.2 : 1.0;\n"
	"    vec3 lit = vec3(-1.0);\n"
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
	"            vec3 sum = vec3(0.0);\n"
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
	"                float wk = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n"
	"                sum += wk * rgb9e5(tw.x) * factor;\n"
	"                weights += wk;\n"
	"            }\n"
	"            if (weights > 1.0e-4)\n"
	"            {\n"
	"                lit = sum / weights;\n"
	"                sunv = sun_of(vp, ce * scale);\n"
	"                checker = float((a.x + a.y) & 1);\n"
	"                level = true;\n"
	"            }\n"
	"        }\n"
	"    }\n"
	"    if (present && distance > 0.0 && ((!level && instance != 0u) || view == 15u))\n"
	"    {\n"
	"        vec4 v = volume_at(point + facing_normal * 0.05, facing_normal);\n"
	"        if (v.x >= 0.0)\n"
	"        {\n"
	"            if (view == 15u || !level)\n"
	"            {\n"
	"                lit = v.rgb;\n"
	"                sunv = v.w;\n"
	"            }\n"
	"        }\n"
	"        else if (view == 15u)\n"
	"            lit = vec3(-1.0);\n"
	"    }\n"
	"    vec3 result = old.rgb;\n"
	"    if (lit.x >= 0.0)\n"
	"    {\n"
	"        float facing = max(dot(facing_normal, s.sun.xyz), 0.0);\n"
	"        vec3 sunlight = s.sun.w > 0.0 && sunv > 0.0 ? s.rig[0].rgb * facing * sunv : vec3(0.0);\n"
	"        vec3 emissive = vec3(unpackHalf2x16(x.y), unpackHalf2x16(x.z & 0xffffu).x);\n"
	"        vec3 total = lit + sunlight;\n"
	"        vec3 encoded = gamma > 1.001 ? pow(max(total, vec3(0.0)), vec3(1.0 / gamma)) : max(total, vec3(0.0));\n"
	"        result = encoded + (instance == 0u ? emissive : vec3(0.0));\n"
	"        /* (an object's pixel: CE draws it whole into the light target, its\n"
	"        base target white, so its light goes in by its own base colour,\n"
	"        the G-buffer's) */\n"
	"        if (instance != 0u)\n"
	"            result = encoded * unpackUnorm4x8(w.x).rgb;\n"
	"        /* (a level's pixel: what CE drew over its lightmap pass's light, the\n"
	"        light target's pixel against that light whole (the G-buffer's), kept:\n"
	"        the objects' shadows darken the baked light by their share, the\n"
	"        dynamic lights add theirs) */\n"
	"        if (instance == 0u && ce_code > 0.0)\n"
	"        {\n"
	"            float ce_light = ce_code * ce_code - luma(emissive);\n"
	"            vec3 drawn = max(old.rgb - emissive, vec3(0.0));\n"
	"            float drawn_l = luma(drawn);\n"
	"            if (ce_light > 0.01)\n"
	"            {\n"
	"                float share = clamp(drawn_l / ce_light, 0.0, 1.0);\n"
	"                vec3 added = drawn_l > ce_light ? drawn * (1.0 - ce_light / drawn_l) : vec3(0.0);\n"
	"                result = encoded * share + added + emissive;\n"
	"            }\n"
	"        }\n"
	"        /* (the views: the light less the sun's, in CE's encoding: the gates\n"
	"        put the sun's back by the pixel's normal and the visibility view) */\n"
	"        if (view == 12u || view == 15u)\n"
	"            result = gamma > 1.001 ? pow(max(lit, vec3(0.0)), vec3(1.0 / gamma)) : max(lit, vec3(0.0));\n"
	"        else if (view == 13u)\n"
	"            result = vec3(max(sunv, 0.0) * (facing > 0.0 ? 1.0 : 0.25));\n"
	"    }\n"
	"    else if (view >= 12u && view <= 15u)\n"
	"        result = present ? vec3(0.5, 0.0, 0.0) : vec3(0.0);\n"
	"    if (view == 14u && reason == 2)\n"
	"    {\n"
	"        /* (debug: the first page whose chart map has the coordinates) */\n"
	"        Uints pg = Uints(job.pages);\n"
	"        Uints cm = Uints(job.charts);\n"
	"        vec2 uv = unpackUnorm2x16(w.x);\n"
	"        int found = -1;\n"
	"        for (uint q = 0u; q < push.parameters.y && found < 0; q++)\n"
	"        {\n"
	"            uint offset = pg.v[q * 4u], pw = pg.v[q * 4u + 1u], ph = pg.v[q * 4u + 2u];\n"
	"            ivec2 cell = clamp(ivec2(floor(uv * vec2(pw, ph))), ivec2(0), ivec2(pw, ph) - 1);\n"
	"            uint ci = offset + uint(cell.y) * pw + uint(cell.x);\n"
	"            if (((cm.v[ci >> 1u] >> ((ci & 1u) * 16u)) & 0xffffu) != 0u)\n"
	"                found = int(q);\n"
	"        }\n"
	"        imageStore(light, pixel, vec4(float(page) / 32.0, found >= 0 ? float(found + 1) / 32.0 : 0.0, uv.x, old.a));\n"
	"        return;\n"
	"    }\n"
	"    if (view == 14u)\n"
	"        result = checker >= 0.0 ? vec3(0.2 + 0.6 * checker) : !present ? vec3(0.0) : reason == 1 ?\n"
	"            vec3(0.8, 0.0, 0.0) : reason == 2 ? vec3(0.8, 0.0, 0.8) : reason == 3 ? vec3(0.8, 0.8, 0.0) :\n"
	"            vec3(0.0, 0.3, 0.8);\n"
	"    if (any(isnan(result)) || any(isinf(result)))\n"
	"        result = old.rgb;\n"
	"    imageStore(light, pixel, vec4(result, old.a));\n"
	"}\n";
