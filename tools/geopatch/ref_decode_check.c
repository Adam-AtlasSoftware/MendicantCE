/*
tools/test_geopatch.py's check of prism_ref_decode.c against the game's own
float decode (source/rasterizer/rasterizer_geometry.c: uncompress_int32_to_
real_vector3d and the lightmap coordinates' expression, copied below as the
game has them), compiled with the compiler's IEEE single arithmetic: every
11-bit and 10-bit field value, every 16-bit coordinate, and a run of random
whole vertices. Prints "ok" and exits 0, or the first difference and 1.
*/

#include <stdio.h>
#include <string.h>

#include "prism_ref_decode.h"

typedef float real;

static void game_vector(unsigned long compressed, real out[3])
{
	/* (the game's long is 32 bits: its guests are 32-bit) */
	real value = (real)(int32_t)(uint32_t)(compressed << 21);

	out[0] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (real)(int32_t)(uint32_t)(compressed << 21);
	out[1] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	compressed >>= 11;
	value = (real)(int32_t)(uint32_t)(compressed << 22);
	out[2] = (value * (1.0f / 2097152.0f) + 1.0f) * (1.0f / 1023.0f);
}

static real game_coordinate(short value)
{
	return ((real)value * 2.0f + 1.0f) * (1.0f / 65535.0f);
}

static uint32_t bits_of(real value)
{
	uint32_t bits;

	memcpy(&bits, &value, 4);
	return bits;
}

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static int check_vector(uint32_t packed)
{
	volatile real expected[3];
	real tmp[3];
	uint32_t got[3];
	int axis;

	game_vector(packed, tmp);
	for (axis = 0; axis < 3; axis++)
		expected[axis] = tmp[axis];
	prsx_decode_vector_bits(packed, got);
	for (axis = 0; axis < 3; axis++)
	{
		if (bits_of(expected[axis]) != got[axis])
		{
			printf("vector %08x axis %d: game %08x ours %08x\n", (unsigned)packed, axis,
				(unsigned)bits_of(expected[axis]), (unsigned)got[axis]);
			return 0;
		}
	}
	return 1;
}

int main(void)
{
	uint32_t value;
	long index;

	if (prsx_reciprocal_bits(2047) != bits_of(1.0f / 2047.0f) || prsx_reciprocal_bits(1023) != bits_of(1.0f / 1023.0f) ||
		prsx_reciprocal_bits(65535) != bits_of(1.0f / 65535.0f))
	{
		printf("reciprocals differ\n");
		return 1;
	}
	/* every field value of each axis (the others zero, then random) */
	for (value = 0; value < 2048; value++)
	{
		if (!check_vector(value) || !check_vector(value << 11) || !check_vector(rng() & ~0x7FFu ? value | (rng() & ~0x7FFu) : value))
			return 1;
	}
	for (value = 0; value < 1024; value++)
	{
		if (!check_vector(value << 22) || !check_vector((value << 22) | (rng() & 0x3FFFFFu)))
			return 1;
	}
	for (index = -32768; index < 32768; index++)
	{
		volatile real expected = game_coordinate((short)index);

		if (bits_of(expected) != prsx_decode_coordinate_bits((int16_t)index))
		{
			printf("coordinate %ld: game %08x ours %08x\n", index, (unsigned)bits_of(expected),
				(unsigned)prsx_decode_coordinate_bits((int16_t)index));
			return 1;
		}
	}
	/* whole vertices: the game's per-field decode laid out as its uncompressed vertices */
	for (index = 0; index < 200000; index++)
	{
		unsigned char compressed[32], uncompressed[56], expected[56];
		unsigned char lightmap[8], lightmap_out[20], lightmap_expected[20];
		real vec[3];
		int k, axis;

		for (k = 0; k < 32; k++)
			compressed[k] = (unsigned char)rng();
		for (k = 0; k < 8; k++)
			lightmap[k] = (unsigned char)rng();
		memcpy(expected, compressed, 12);
		for (k = 0; k < 3; k++)
		{
			uint32_t packed;

			memcpy(&packed, compressed + 12 + 4 * k, 4);
			game_vector(packed, vec);
			for (axis = 0; axis < 3; axis++)
				memcpy(expected + 12 + 12 * k + 4 * axis, &vec[axis], 4);
		}
		memcpy(expected + 48, compressed + 24, 8);
		prsx_decode_environment_vertex(compressed, uncompressed);
		if (memcmp(expected, uncompressed, 56) != 0)
		{
			printf("environment vertex %ld differs\n", index);
			return 1;
		}
		{
			uint32_t packed;
			short u, v;
			real fu, fv;

			memcpy(&packed, lightmap, 4);
			memcpy(&u, lightmap + 4, 2);
			memcpy(&v, lightmap + 6, 2);
			game_vector(packed, vec);
			memcpy(lightmap_expected, vec, 12);
			fu = game_coordinate(u);
			fv = game_coordinate(v);
			memcpy(lightmap_expected + 12, &fu, 4);
			memcpy(lightmap_expected + 16, &fv, 4);
		}
		prsx_decode_lightmap_vertex(lightmap, lightmap_out);
		if (memcmp(lightmap_expected, lightmap_out, 20) != 0)
		{
			printf("lightmap vertex %ld differs\n", index);
			return 1;
		}
	}
	printf("ok\n");
	return 0;
}
