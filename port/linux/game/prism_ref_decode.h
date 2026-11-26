/*
PRISM_REF_DECODE.H

Exact decode of retail values that a sidecar refers to (a REF op) instead of
carrying them: the farm's tools and the game run this one C file
(tools/geopatch/native.py loads it), so a REF resolves to the same bits on
every machine and compiler, with no floating point at all (D-B3).

Xbox BSPs hold only the compressed vertex forms (v4_geopatch_runs/m1/
VERIFY.md, V1): every lightmap material's vertex buffer is type 1, the
32-byte compressed environment vertex, and its lightmap vertices type 3,
the 8-byte compressed lightmap vertex, both in compressed_vertex_data;
uncompressed_vertex_data is empty. The decode gives the 56- and 20-byte
uncompressed layouts with the bits the game's own decompressor
(source/rasterizer/rasterizer_geometry.c, rasterizer_geometry_uncompress_
vertices and uncompress_int32_to_real_vector3d) computes in IEEE single
precision with round to nearest even:

  packed vector (11:11:10 bits, x low): x = (2 sx + 1) * fl(1/2047),
    y likewise from bits 11-21, z = (2 sz + 1) * fl(1/1023) from bits 22-31,
    each s the field read as a two's complement integer;
  16-bit coordinate: (2 v + 1) * fl(1/65535);
  positions and the environment vertex's texture coordinates: the bits as
    stored.

fl(1/d) is the float nearest 1/d, and each product is rounded once, as the
game's float multiply does. tools/test_geopatch.py checks every packed field
value and every 16-bit value against the compiler's float arithmetic.

The sidecar's own positions are on a grid of 1/1024 world unit, int32, and
become floats exactly (prsx_grid_bits) while |x| < 16384 world units.
*/

#ifndef PRISM_REF_DECODE_H
#define PRISM_REF_DECODE_H

#include <stdint.h>

enum
{
	PRSX_COMPRESSED_VERTEX_SIZE = 32,
	PRSX_UNCOMPRESSED_VERTEX_SIZE = 56,
	PRSX_COMPRESSED_LIGHTMAP_VERTEX_SIZE = 8,
	PRSX_UNCOMPRESSED_LIGHTMAP_VERTEX_SIZE = 20,
	/* the grid: 1024 steps per world unit, |q| < 2^24 */
	PRSX_GRID_STEPS = 1024,
	PRSX_GRID_LIMIT = 1 << 24,
};

/* the float nearest 1/divisor (divisor at least 2), as bits */
uint32_t prsx_reciprocal_bits(uint32_t divisor);
/* integer * the float of factor_bits (a positive normal float), rounded to
nearest even, as bits; integer nonzero, |integer| < 2^24 */
uint32_t prsx_multiply_bits(int32_t integer, uint32_t factor_bits);

/* a packed 11:11:10 vector's three floats, as bits */
void prsx_decode_vector_bits(uint32_t packed, uint32_t bits[3]);
/* a 16-bit compressed coordinate's float, as bits */
uint32_t prsx_decode_coordinate_bits(int16_t value);
/* a grid position's float, as bits: 1, or 0 if |q| >= PRSX_GRID_LIMIT */
int prsx_grid_bits(int32_t q, uint32_t *bits);

/* one compressed environment vertex (32 bytes) as the uncompressed one (56
bytes: position, normal, binormal, tangent, texture coordinates) */
void prsx_decode_environment_vertex(const unsigned char compressed[32], unsigned char uncompressed[56]);
/* one compressed lightmap vertex (8 bytes) as the uncompressed one (20
bytes: incident radiosity, lightmap coordinates) */
void prsx_decode_lightmap_vertex(const unsigned char compressed[8], unsigned char uncompressed[20]);

#endif
