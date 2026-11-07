/* PRISM's mesh store's skinning (display.prism_models; xgpu_vulkan_meshes.c),
in Vulkan's GLSL: tools/vulkan_mesh_shaders.py makes xgpu_vulkan_mesh_shaders.c
of it. Not compiled into the game.

A mesh's vertices (.pmesh VERT, 48 bytes each: position f32x3, normal and
tangent octahedral snorm16x2, uv1 f16x2, uv0 f32x2, joints u16x4, weights
unorm8x4, the tangent's sign i8 and 3 bytes of padding) from first to
first + count, blended by up to four bones (three rows of four each, from
bone_base in vec4s: the game's node matrices as its vertex shaders'
constants hold them, or a helper bone's), written from out_base (in words) in the stock compressed model vertex's layout
(rasterizer_model_types.h's struct model_vertex_compressed, 32 bytes:
position f32x3; normal, binormal and tangent packed 11:11:10, signed;
texture coordinates int16x2 over the model's base map scale; nodes 0 and 0,
weight 1), which the model's own pipelines draw with an identity for node 0;
and where wide_base is not ~0, from wide_base (in words) the wide stream a
material's pipelines read beside it (16 bytes a vertex: the tangent in the
world, x and y snorm16, then z and the bitangent's sign snorm16; uv0 as two
floats, unscaled and unclamped). */

static const char skin_source[] =
	"#version 450\n"
	"layout(local_size_x = 64) in;\n"
	"layout(std430, set = 0, binding = 0) readonly buffer Rest { uint rest[]; };\n"
	"layout(std430, set = 0, binding = 1) readonly buffer Bones { vec4 bones[]; };\n"
	"layout(std430, set = 0, binding = 2) writeonly buffer Skinned { uint skinned[]; };\n"
	"layout(push_constant) uniform Skin { uint first; uint count; vec2 inverse_scale; uint bone_base; uint out_base; uint wide_base; } skin;\n"
	"\n"
	"uint pack_normal(vec3 v)\n"
	"{\n"
	"    v = clamp(v, -1.0, 1.0);\n"
	"    int x = int(round(v.x * 1023.0));\n"
	"    int y = int(round(v.y * 1023.0));\n"
	"    int z = int(round(v.z * 511.0));\n"
	"    return (uint(x) & 0x7ffu) | ((uint(y) & 0x7ffu) << 11) | ((uint(z) & 0x3ffu) << 22);\n"
	"}\n"
	"\n"
	"vec3 octahedral(uint packed)\n"
	"{\n"
	"    vec2 e = unpackSnorm2x16(packed);\n"
	"    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));\n"
	"    if (v.z < 0.0)\n"
	"        v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);\n"
	"    return normalize(v);\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"    uint i = gl_GlobalInvocationID.x;\n"
	"    if (i >= skin.count)\n"
	"        return;\n"
	"    uint b = (skin.first + i) * 12u;\n"
	"    vec4 p = vec4(uintBitsToFloat(rest[b]), uintBitsToFloat(rest[b + 1u]), uintBitsToFloat(rest[b + 2u]), 1.0);\n"
	"    vec3 n = octahedral(rest[b + 3u]);\n"
	"    vec3 t = octahedral(rest[b + 4u]);\n"
	"    vec2 uv = vec2(uintBitsToFloat(rest[b + 6u]), uintBitsToFloat(rest[b + 7u]));\n"
	"    uint j01 = rest[b + 8u];\n"
	"    uint j23 = rest[b + 9u];\n"
	"    vec4 w = unpackUnorm4x8(rest[b + 10u]);\n"
	"    float handedness = (rest[b + 11u] & 0x80u) != 0u ? -1.0 : 1.0;\n"
	"    uint j[4] = uint[4](j01 & 0xffffu, j01 >> 16, j23 & 0xffffu, j23 >> 16);\n"
	"    vec4 r0 = vec4(0.0);\n"
	"    vec4 r1 = vec4(0.0);\n"
	"    vec4 r2 = vec4(0.0);\n"
	"    for (int k = 0; k < 4; k++)\n"
	"    {\n"
	"        if (w[k] > 0.0)\n"
	"        {\n"
	"            uint m = skin.bone_base + j[k] * 3u;\n"
	"            r0 += w[k] * bones[m];\n"
	"            r1 += w[k] * bones[m + 1u];\n"
	"            r2 += w[k] * bones[m + 2u];\n"
	"        }\n"
	"    }\n"
	"    vec3 position = vec3(dot(r0, p), dot(r1, p), dot(r2, p));\n"
	"    vec3 normal = normalize(vec3(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n)));\n"
	"    vec3 tangent = normalize(vec3(dot(r0.xyz, t), dot(r1.xyz, t), dot(r2.xyz, t)));\n"
	"    vec3 binormal = cross(normal, tangent) * handedness;\n"
	"    ivec2 q = ivec2(clamp(round(uv * skin.inverse_scale * 32767.0), vec2(-32767.0), vec2(32767.0)));\n"
	"    uint o = skin.out_base + i * 8u;\n"
	"    skinned[o] = floatBitsToUint(position.x);\n"
	"    skinned[o + 1u] = floatBitsToUint(position.y);\n"
	"    skinned[o + 2u] = floatBitsToUint(position.z);\n"
	"    skinned[o + 3u] = pack_normal(normal);\n"
	"    skinned[o + 4u] = pack_normal(binormal);\n"
	"    skinned[o + 5u] = pack_normal(tangent);\n"
	"    skinned[o + 6u] = (uint(q.x) & 0xffffu) | (uint(q.y) << 16);\n"
	"    skinned[o + 7u] = 0x7fff0000u;\n"
	"    if (skin.wide_base != 0xffffffffu)\n"
	"    {\n"
	"        uint s = skin.wide_base + i * 4u;\n"
	"        skinned[s] = packSnorm2x16(tangent.xy);\n"
	"        skinned[s + 1u] = packSnorm2x16(vec2(tangent.z, handedness));\n"
	"        skinned[s + 2u] = floatBitsToUint(uv.x);\n"
	"        skinned[s + 3u] = floatBitsToUint(uv.y);\n"
	"    }\n"
	"}\n";
