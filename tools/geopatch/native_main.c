/*
The game's own sidecar C (port/linux/game/prism_prsx.c, prism_identity.c,
prism_ref_decode.c) as a command for the farm's tools and the tests
(tools/geopatch/native.py builds and runs it), so that a REF resolves, a
sidecar is checked and a world_id is computed by the one code the game runs.

  native env|lightmap|vector|coordinate|grid   binary in on stdin, out on stdout:
      env: 32-byte compressed vertices -> 56-byte; lightmap: 8 -> 20;
      vector: u32 packed -> 3 u32 float bits; coordinate: i16 -> u32 bits;
      grid: i32 -> u32 bits (exit 3 if any is out of range)
  native reciprocal <divisor>                  the bits of fl(1/divisor)
  native open <file> [appended]                the sidecar checked, its sections verified, and its ids, as lines:
      "result <text>", "format <n>", "classic <hex>",
      "entry <i> <type> <bsp> <flags> <collv> <usize> <section result>",
      "world <bsp> classic <hex> enhanced <hex>" for each BSP its directory names
  native base-id <file>                        "base_id <hex>", or exit 2
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "prism_identity.h"
#include "prism_prsx.h"
#include "prism_ref_decode.h"

static unsigned char *read_stdin(size_t *size)
{
	size_t capacity = 1 << 16, used = 0, got;
	unsigned char *buffer = (unsigned char *)malloc(capacity);

	while (buffer && (got = fread(buffer + used, 1, capacity - used, stdin)) > 0)
	{
		used += got;
		if (used == capacity)
		{
			unsigned char *grown = (unsigned char *)realloc(buffer, capacity * 2);

			if (!grown)
			{
				free(buffer);
				return NULL;
			}
			buffer = grown;
			capacity *= 2;
		}
	}
	*size = used;
	return buffer;
}

static void put_u32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static uint32_t get_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int stream(const char *command)
{
	size_t size, index, in_size, out_size;
	unsigned char *in = read_stdin(&size), *out;
	int status = 0;

	if (!in)
		return 1;
	if (!strcmp(command, "env"))
		in_size = 32, out_size = 56;
	else if (!strcmp(command, "lightmap"))
		in_size = 8, out_size = 20;
	else if (!strcmp(command, "vector"))
		in_size = 4, out_size = 12;
	else if (!strcmp(command, "coordinate"))
		in_size = 2, out_size = 4;
	else
		in_size = 4, out_size = 4;
	if (size % in_size)
		return 1;
	out = (unsigned char *)malloc(size / in_size * out_size + 1);
	if (!out)
		return 1;
	for (index = 0; index < size / in_size; index++)
	{
		const unsigned char *src = in + index * in_size;
		unsigned char *dst = out + index * out_size;

		if (!strcmp(command, "env"))
			prsx_decode_environment_vertex(src, dst);
		else if (!strcmp(command, "lightmap"))
			prsx_decode_lightmap_vertex(src, dst);
		else if (!strcmp(command, "vector"))
		{
			uint32_t bits[3];

			prsx_decode_vector_bits(get_u32(src), bits);
			put_u32(dst, bits[0]);
			put_u32(dst + 4, bits[1]);
			put_u32(dst + 8, bits[2]);
		}
		else if (!strcmp(command, "coordinate"))
			put_u32(dst, prsx_decode_coordinate_bits((int16_t)(src[0] | src[1] << 8)));
		else
		{
			uint32_t bits = 0;

			if (!prsx_grid_bits((int32_t)get_u32(src), &bits))
				status = 3;
			put_u32(dst, bits);
		}
	}
	fwrite(out, 1, size / in_size * out_size, stdout);
	free(in);
	free(out);
	return status;
}

static int open_sidecar(const char *path, int appended)
{
	FILE *file = fopen(path, "rb");
	struct prsx_source whole, source;
	struct prsx prsx;
	enum prsx_result result;
	char hex[2 * PRISM_ID_SIZE + 1];
	uint32_t index;
	unsigned int bsp;

	if (!file || !prsx_source_file(&whole, file))
	{
		printf("result %s\n", prsx_result_string(PRSX_ERROR_READ));
		return 2;
	}
	source = whole;
	if (appended && !prsx_source_appended(&whole, &source))
	{
		printf("result no appended container\n");
		fclose(file);
		return 2;
	}
	result = prsx_open(&prsx, &source);
	printf("result %s\n", prsx_result_string(result));
	if (result != PRSX_OK)
	{
		fclose(file);
		return 2;
	}
	prism_id_hex(prsx.classic_world_id, hex);
	printf("format %u\nclassic %s\n", (unsigned)prsx.format, hex);
	for (index = 0; index < prsx.section_count; index++)
	{
		const struct prsx_entry *entry = &prsx.entries[index];
		void *buffer = malloc(entry->usize ? (size_t)entry->usize : 1);
		enum prsx_result read = buffer ? prsx_section_read(&prsx, index, buffer, (size_t)entry->usize) :
			PRSX_ERROR_MEMORY;

		printf("entry %u %c%c%c%c %u %u %u %lu %s\n", (unsigned)index, (char)(entry->type & 0xFF),
			(char)(entry->type >> 8 & 0xFF), (char)(entry->type >> 16 & 0xFF), (char)(entry->type >> 24),
			(unsigned)entry->bsp, (unsigned)entry->flags, (unsigned)entry->collv, (unsigned long)entry->usize,
			prsx_result_string(read));
		free(buffer);
	}
	for (bsp = 0; bsp < PRSX_BSP_ALL; bsp++)
	{
		int named = 0;

		for (index = 0; index < prsx.section_count; index++)
			named |= prsx.entries[index].bsp == bsp;
		if (named)
		{
			unsigned char id[PRISM_ID_SIZE];
			char classic[2 * PRISM_ID_SIZE + 1];

			prism_world_id_classic(prsx.classic_world_id, (uint16_t)bsp, id);
			prism_id_hex(id, classic);
			prism_world_id_enhanced(&prsx, (uint16_t)bsp, id);
			prism_id_hex(id, hex);
			printf("world %u classic %s enhanced %s\n", bsp, classic, hex);
		}
	}
	prsx_close(&prsx);
	fclose(file);
	return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	if (argc < 2)
		return 1;
	if (!strcmp(argv[1], "env") || !strcmp(argv[1], "lightmap") || !strcmp(argv[1], "vector") ||
		!strcmp(argv[1], "coordinate") || !strcmp(argv[1], "grid"))
	{
		return stream(argv[1]);
	}
	if (!strcmp(argv[1], "reciprocal") && argc == 3)
	{
		printf("%08x\n", (unsigned)prsx_reciprocal_bits((uint32_t)strtoul(argv[2], NULL, 10)));
		return 0;
	}
	if (!strcmp(argv[1], "open") && argc >= 3)
		return open_sidecar(argv[2], argc > 3 && !strcmp(argv[3], "appended"));
	if (!strcmp(argv[1], "base-id") && argc == 3)
	{
		FILE *file = fopen(argv[2], "rb");
		unsigned char id[PRISM_ID_SIZE];
		char hex[2 * PRISM_ID_SIZE + 1];
		int whole = file && prism_base_id_of_file(file, id);

		if (file)
			fclose(file);
		if (!whole)
			return 2;
		prism_id_hex(id, hex);
		printf("base_id %s\n", hex);
		return 0;
	}
	return 1;
}
