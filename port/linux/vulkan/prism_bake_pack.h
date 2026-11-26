/*
PRISM_BAKE_PACK.H

The bake file's packing (prism_bake_cache.h's packed records, docs/
prism_bake.md, "The bake file"): a finished bake's large records (the
lightmap's tiles and their layers, the volume's bricks, the reflection
probes, the sun masks) are kept packed, and unpacked as they are read.

A payload is packed in three steps, each undone in turn by the unpacking:

	the shuffle: a payload of words of `stride` bytes (1, 2, 4 or 8) is
	rearranged as its bytes' planes: every word's first byte, then every
	word's second byte, and so on (the RGB9E5 words' exponents and high
	bits, which change little from texel to texel, then come together);
	the delta: each byte of a plane less the one before it (mod 256), so a
	smooth plane is mostly small values and zeros;
	an LZ77 in LZ4's block format (a token of the literals' and the
	match's lengths, the literals, a 16-bit offset back, the lengths'
	extensions in bytes of 255), with no end-of-block restrictions: the
	block ends where its last literals do.

The packed bytes are: "PK", the stride, 0, the unpacked size (32 bits,
little endian), then the LZ block. The unpacking checks every length and
offset against both buffers: a damaged block is refused, never read or
written past.

Host code with no engine dependency, written as C89.
*/

#ifndef PRISM_BAKE_PACK_H
#define PRISM_BAKE_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PRISM_BAKE_PACK_HEADER = 8
};

/* the most a payload of size bytes can take packed (a block that does not
pack is no more than this) */
size_t prism_bake_pack_bound(size_t size);

/* size bytes of words of stride bytes (1, 2, 4 or 8; size need not be a
multiple: the tail is shuffled as bytes) packed into out (capacity bytes,
at least prism_bake_pack_bound(size)): the packed size, or 0 where it does
not fit or stride is not one of those */
size_t prism_bake_pack(void const *in, size_t size, uint32_t stride, void *out, size_t capacity);

/* the unpacked size a packed payload says it has, or 0 where it is not one */
size_t prism_bake_unpacked_size(void const *packed, size_t size);

/* a packed payload unpacked into out (capacity bytes): the unpacked size,
or 0 where the block is damaged or does not fit */
size_t prism_bake_unpack(void const *packed, size_t size, void *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* PRISM_BAKE_PACK_H */
