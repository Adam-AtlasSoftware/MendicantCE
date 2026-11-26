/*
PRISM_REF_DECODE.C

prism_ref_decode.h has what and why. Integer and bit operations only.
*/

#include "prism_ref_decode.h"

#include <string.h>

static uint32_t ref_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ref_put_u32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static int ref_top_bit(uint64_t value)
{
	int bit = -1;

	while (value)
	{
		value >>= 1;
		bit++;
	}
	return bit;
}

uint32_t prsx_reciprocal_bits(uint32_t divisor)
{
	uint64_t numerator, quotient, remainder;
	int shift = 0;

	/* (1/d = q 2^-(23+s) with q of 24 bits) */
	for (;;)
	{
		numerator = (uint64_t)1 << (23 + shift);
		if (numerator / divisor >= ((uint64_t)1 << 23))
			break;
		shift++;
	}
	quotient = numerator / divisor;
	remainder = numerator % divisor;
	if (2 * remainder > divisor || (2 * remainder == divisor && (quotient & 1)))
		quotient++;
	if (quotient == ((uint64_t)1 << 24))
	{
		quotient >>= 1;
		shift--;
	}
	return (uint32_t)(127 - shift) << 23 | (uint32_t)(quotient & 0x7FFFFF);
}

uint32_t prsx_multiply_bits(int32_t integer, uint32_t factor_bits)
{
	uint32_t sign = integer < 0 ? 0x80000000u : 0;
	uint64_t magnitude = (uint64_t)(integer < 0 ? -(int64_t)integer : (int64_t)integer);
	uint64_t mantissa = (uint64_t)((factor_bits & 0x7FFFFF) | 0x800000);
	int exponent = (int)((factor_bits >> 23) & 0xFF) - 127;
	uint64_t product = magnitude * mantissa;
	int top = ref_top_bit(product);
	int shift = top - 23;
	uint64_t kept = product, rest, half;

	if (shift > 0)
	{
		kept = product >> shift;
		rest = product & (((uint64_t)1 << shift) - 1);
		half = (uint64_t)1 << (shift - 1);
		if (rest > half || (rest == half && (kept & 1)))
			kept++;
		if (kept == ((uint64_t)1 << 24))
		{
			kept >>= 1;
			shift++;
		}
	}
	else if (shift < 0)
	{
		kept = product << -shift;
	}
	/* (product = kept 2^(shift + exponent - 23), kept of 24 bits) */
	return sign | (uint32_t)(exponent + shift + 127) << 23 | (uint32_t)(kept & 0x7FFFFF);
}

static int32_t ref_signed(uint32_t value, int bits)
{
	uint32_t sign = (uint32_t)1 << (bits - 1);

	value &= (sign << 1) - 1;
	return (int32_t)(value ^ sign) - (int32_t)sign;
}

void prsx_decode_vector_bits(uint32_t packed, uint32_t bits[3])
{
	uint32_t eleven = prsx_reciprocal_bits(2047);
	uint32_t ten = prsx_reciprocal_bits(1023);

	bits[0] = prsx_multiply_bits(2 * ref_signed(packed, 11) + 1, eleven);
	bits[1] = prsx_multiply_bits(2 * ref_signed(packed >> 11, 11) + 1, eleven);
	bits[2] = prsx_multiply_bits(2 * ref_signed(packed >> 22, 10) + 1, ten);
}

uint32_t prsx_decode_coordinate_bits(int16_t value)
{
	return prsx_multiply_bits(2 * (int32_t)value + 1, prsx_reciprocal_bits(65535));
}

int prsx_grid_bits(int32_t q, uint32_t *bits)
{
	uint32_t sign = q < 0 ? 0x80000000u : 0;
	uint32_t magnitude = (uint32_t)(q < 0 ? -(int64_t)q : (int64_t)q);
	int top;

	if (magnitude >= PRSX_GRID_LIMIT)
		return 0;
	if (!magnitude)
	{
		*bits = 0;
		return 1;
	}
	top = ref_top_bit(magnitude);
	/* (q / 1024 = magnitude 2^-10: exact, magnitude has at most 24 bits) */
	*bits = sign | (uint32_t)(top - 10 + 127) << 23 | ((magnitude << (23 - top)) & 0x7FFFFF);
	return 1;
}

void prsx_decode_environment_vertex(const unsigned char compressed[32], unsigned char uncompressed[56])
{
	uint32_t bits[3];
	int vector, axis;

	memcpy(uncompressed, compressed, 12);
	for (vector = 0; vector < 3; vector++)
	{
		prsx_decode_vector_bits(ref_u32(compressed + 12 + 4 * vector), bits);
		for (axis = 0; axis < 3; axis++)
			ref_put_u32(uncompressed + 12 + 12 * vector + 4 * axis, bits[axis]);
	}
	memcpy(uncompressed + 48, compressed + 24, 8);
}

void prsx_decode_lightmap_vertex(const unsigned char compressed[8], unsigned char uncompressed[20])
{
	uint32_t bits[3];
	int axis;

	prsx_decode_vector_bits(ref_u32(compressed), bits);
	for (axis = 0; axis < 3; axis++)
		ref_put_u32(uncompressed + 4 * axis, bits[axis]);
	ref_put_u32(uncompressed + 12,
		prsx_decode_coordinate_bits((int16_t)(compressed[4] | compressed[5] << 8)));
	ref_put_u32(uncompressed + 16,
		prsx_decode_coordinate_bits((int16_t)(compressed[6] | compressed[7] << 8)));
}
