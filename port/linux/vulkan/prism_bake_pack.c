/*
PRISM_BAKE_PACK.C

The bake file's packing: prism_bake_pack.h.
*/

#include "prism_bake_pack.h"

#include <stdlib.h>
#include <string.h>

enum
{
	HASH_BITS = 16,
	MATCH_LEAST = 4,
	OFFSET_MOST = 65535
};

static uint32_t read32(unsigned char const *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t prism_bake_pack_bound(size_t size)
{
	return PRISM_BAKE_PACK_HEADER + size + size / 255 + 16;
}

/* the shuffle and the delta (forward): in's words' byte planes, each byte
less the one before it in its plane */
static void transform(unsigned char const *in, size_t size, uint32_t stride, unsigned char *out)
{
	size_t words = size / stride, at = 0, k, w;
	uint32_t plane;

	for (plane = 0; plane < stride; plane++)
	{
		unsigned char last = 0;

		for (w = 0; w < words; w++)
		{
			unsigned char v = in[w * stride + plane];

			out[at++] = (unsigned char)(v - last);
			last = v;
		}
	}
	for (k = words * stride; k < size; k++)
		out[at++] = in[k];
}

static void untransform(unsigned char const *in, size_t size, uint32_t stride, unsigned char *out)
{
	size_t words = size / stride, at = 0, k, w;
	uint32_t plane;

	for (plane = 0; plane < stride; plane++)
	{
		unsigned char last = 0;

		for (w = 0; w < words; w++)
		{
			last = (unsigned char)(last + in[at++]);
			out[w * stride + plane] = last;
		}
	}
	for (k = words * stride; k < size; k++)
		out[k] = in[at++];
}

/* a length's extension bytes (255s, then the rest): 0, or -1 past the end */
static int length_put(unsigned char *out, size_t *at, size_t capacity, size_t length)
{
	while (length >= 255)
	{
		if (*at >= capacity)
			return -1;
		out[(*at)++] = 255;
		length -= 255;
	}
	if (*at >= capacity)
		return -1;
	out[(*at)++] = (unsigned char)length;
	return 0;
}

/* one sequence: the literals from anchor to ip, then (match > 0) the match
offset back and its length */
static int sequence_put(unsigned char *out, size_t *at, size_t capacity, unsigned char const *literals,
	size_t literal_count, size_t offset, size_t match)
{
	size_t token_at = *at;
	unsigned char token;

	if (*at >= capacity)
		return -1;
	(*at)++;
	token = (unsigned char)((literal_count >= 15 ? 15 : literal_count) << 4);
	if (literal_count >= 15 && length_put(out, at, capacity, literal_count - 15))
		return -1;
	if (literal_count > capacity - *at)
		return -1;
	memcpy(out + *at, literals, literal_count);
	*at += literal_count;
	if (match)
	{
		size_t rest = match - MATCH_LEAST;

		if (capacity - *at < 2)
			return -1;
		out[(*at)++] = (unsigned char)(offset & 255);
		out[(*at)++] = (unsigned char)(offset >> 8);
		token |= (unsigned char)(rest >= 15 ? 15 : rest);
		if (rest >= 15 && length_put(out, at, capacity, rest - 15))
			return -1;
	}
	out[token_at] = token;
	return 0;
}

size_t prism_bake_pack(void const *in, size_t size, uint32_t stride, void *out_, size_t capacity)
{
	unsigned char *out = (unsigned char *)out_, *src;
	uint32_t *table;
	size_t ip = 0, anchor = 0, at = PRISM_BAKE_PACK_HEADER, misses = 0;

	if ((stride != 1 && stride != 2 && stride != 4 && stride != 8) || capacity < PRISM_BAKE_PACK_HEADER ||
		size > 0xFFFFFFFFu || (size && !in))
		return 0;
	src = (unsigned char *)malloc(size + 1);
	table = (uint32_t *)calloc((size_t)1 << HASH_BITS, sizeof(uint32_t));
	if (!src || !table)
	{
		free(src);
		free(table);
		return 0;
	}
	transform((unsigned char const *)in, size, stride, src);
	out[0] = 'P';
	out[1] = 'K';
	out[2] = (unsigned char)stride;
	out[3] = 0;
	out[4] = (unsigned char)(size & 255);
	out[5] = (unsigned char)((size >> 8) & 255);
	out[6] = (unsigned char)((size >> 16) & 255);
	out[7] = (unsigned char)((size >> 24) & 255);
	while (ip + MATCH_LEAST <= size)
	{
		uint32_t sequence = read32(src + ip), h = (sequence * 2654435761u) >> (32 - HASH_BITS);
		size_t reference = table[h];

		table[h] = (uint32_t)ip + 1u;
		if (reference && ip - (reference - 1) <= OFFSET_MOST && read32(src + reference - 1) == sequence)
		{
			size_t from = reference - 1, length = MATCH_LEAST;

			while (ip + length < size && src[from + length] == src[ip + length])
				length++;
			if (sequence_put(out, &at, capacity, src + anchor, ip - anchor, ip - from, length))
			{
				at = 0;
				break;
			}
			ip += length;
			anchor = ip;
			misses = 0;
			/* (the match's last places in the table too) */
			if (ip >= 2 && ip + 2 <= size && ip - 2 + MATCH_LEAST <= size)
				table[(read32(src + ip - 2) * 2654435761u) >> (32 - HASH_BITS)] = (uint32_t)(ip - 2) + 1u;
			continue;
		}
		/* (past many misses, the bytes that do not pack are stepped over
		faster) */
		ip += 1 + (misses++ >> 6);
	}
	if (at && sequence_put(out, &at, capacity, src + anchor, size - anchor, 0, 0))
		at = 0;
	free(src);
	free(table);
	return at;
}

size_t prism_bake_unpacked_size(void const *packed_, size_t size)
{
	unsigned char const *packed = (unsigned char const *)packed_;

	if (size < PRISM_BAKE_PACK_HEADER || packed[0] != 'P' || packed[1] != 'K' || packed[3] != 0 ||
		(packed[2] != 1 && packed[2] != 2 && packed[2] != 4 && packed[2] != 8))
		return 0;
	return (size_t)read32(packed + 4);
}

size_t prism_bake_unpack(void const *packed_, size_t size, void *out, size_t capacity)
{
	unsigned char const *packed = (unsigned char const *)packed_, *ip, *end;
	unsigned char *scratch;
	size_t unpacked = prism_bake_unpacked_size(packed_, size), op = 0;
	uint32_t stride;

	if (!unpacked || unpacked > capacity || !out)
		return 0;
	stride = packed[2];
	scratch = (unsigned char *)malloc(unpacked);
	if (!scratch)
		return 0;
	ip = packed + PRISM_BAKE_PACK_HEADER;
	end = packed + size;
	while (ip < end)
	{
		unsigned token = *ip++;
		size_t literals = token >> 4, match, offset;

		if (literals == 15)
		{
			unsigned char b;

			do
			{
				if (ip >= end)
					goto damaged;
				b = *ip++;
				literals += b;
			} while (b == 255);
		}
		if (literals > (size_t)(end - ip) || literals > unpacked - op)
			goto damaged;
		memcpy(scratch + op, ip, literals);
		ip += literals;
		op += literals;
		if (ip == end)
			break;
		if (end - ip < 2)
			goto damaged;
		offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		match = (token & 15u) + MATCH_LEAST;
		if ((token & 15u) == 15u)
		{
			unsigned char b;

			do
			{
				if (ip >= end)
					goto damaged;
				b = *ip++;
				match += b;
			} while (b == 255);
		}
		if (!offset || offset > op || match > unpacked - op)
			goto damaged;
		{
			unsigned char *to = scratch + op, *from = scratch + op - offset;
			size_t k;

			for (k = 0; k < match; k++)
				to[k] = from[k];
		}
		op += match;
	}
	if (op != unpacked)
		goto damaged;
	untransform(scratch, unpacked, stride, (unsigned char *)out);
	free(scratch);
	return unpacked;
damaged:
	free(scratch);
	return 0;
}
