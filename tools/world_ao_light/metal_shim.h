/*
METAL_SHIM.H

What of the Metal Shading Language the Metal emitter's display.ambient_
occlusion "world" functions use (port/linux/src/xgpu_msl.c's world_ao_*),
in C++, so that tools/world_ao_light/msl_check.cpp runs those functions'
very text on the CPU: float3 and float4 (their components, .xyz, a
component by index), their arithmetic, the few library functions, the
constant address space, and a 3D texture sampled linear and clamped to its
edge as a GPU samples it.
*/

#ifndef METAL_SHIM_H
#define METAL_SHIM_H

#include <algorithm>
#include <cmath>

#define constant const

struct float3
{
	float x, y, z;
	float3() = default;
	float3(float s) : x(s), y(s), z(s) {}
	float3(float a, float b, float c) : x(a), y(b), z(c) {}
	float &operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
	float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
	float3 &operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
};

/* (its parts kept apart, each set by the constructor: the functions only
read a float4's, and never assign one part) */
struct float4
{
	float x, y, z, w;
	float r;
	float3 xyz;
	float4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f), r(0.0f), xyz(0.0f) {}
	float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d), r(a), xyz(a, b, c) {}
};

static inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float3 operator*(float3 a, float s) { return float3(a.x * s, a.y * s, a.z * s); }
static inline float3 operator*(float s, float3 a) { return float3(a.x * s, a.y * s, a.z * s); }
static inline float3 operator*(float3 a, float3 b) { return float3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float3 operator/(float3 a, float s) { return float3(a.x / s, a.y / s, a.z / s); }
static inline float3 operator/(float3 a, float3 b) { return float3(a.x / b.x, a.y / b.y, a.z / b.z); }
static inline float3 operator+(float3 a, float s) { return float3(a.x + s, a.y + s, a.z + s); }
static inline float3 operator-(float3 a) { return float3(-a.x, -a.y, -a.z); }
static inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
static inline float length(float3 a) { return std::sqrt(dot(a, a)); }
static inline float3 cross(float3 a, float3 b)
{
	return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float saturate(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static inline float clamp(float x, float low, float high) { return x < low ? low : x > high ? high : x; }
static inline float mix(float a, float b, float t) { return a + (b - a) * t; }
static inline float rsqrt(float x) { return 1.0f / std::sqrt(x); }
using std::abs;
using std::acos;
using std::atan;
using std::atan2;
using std::max;
using std::min;
using std::sqrt;
static inline float max(float a, double b) { return a > (float)b ? a : (float)b; }

namespace filter { enum value { linear }; }
namespace address { enum value { clamp_to_edge }; }
namespace coord { enum value { normalized }; }

struct sampler
{
	constexpr sampler(filter::value, address::value, coord::value) {}
};

struct level
{
	float lod;
	level(float l) : lod(l) {}
};

/* the field: signed bytes, x fastest (world_sdf.h), read as R8 snorm */
template <typename T> struct texture3d
{
	const signed char *voxels;
	int width, height, depth;
	float4 sample(sampler, float3 uvw, level) const
	{
		int size[3] = { width, height, depth }, base[3];
		float fraction[3], result = 0.0f;

		for (int axis = 0; axis < 3; axis++)
		{
			float coordinate = uvw[axis] * (float)size[axis] - 0.5f;
			float floor_value = std::floor(coordinate);

			base[axis] = (int)floor_value;
			fraction[axis] = coordinate - floor_value;
		}
		for (int corner = 0; corner < 8; corner++)
		{
			int at[3];
			float weight = 1.0f;

			for (int axis = 0; axis < 3; axis++)
			{
				int bit = (corner >> axis) & 1;

				at[axis] = base[axis] + bit;
				at[axis] = at[axis] < 0 ? 0 : at[axis] >= size[axis] ? size[axis] - 1 : at[axis];
				weight *= bit ? fraction[axis] : 1.0f - fraction[axis];
			}
			float texel = (float)voxels[(at[2] * height + at[1]) * width + at[0]] / 127.0f;

			result += weight * (texel < -1.0f ? -1.0f : texel);
		}
		return float4(result, 0.0f, 0.0f, 1.0f);
	}
};

#endif
