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
  native deflate <level>                       stdin as one zlib stream on stdout (the vendored zlib 1.3.2's
      deflate, window 15, memory level 8, the default strategy): the farm's sidecar sections, the same bytes on
      every machine whatever zlib its Python has
  native ladder                                every input of prism_world_ladder and its rung, a line each
  native geom <stock> <sidecar> <bsp>          the BSP's GEOM and LMUV decoded by prism_geom.c against the stock
      file (native.py's write_stock: the BSP's materials' compressed vertices and its collision vertices) into
      the PWG1 image on stdout; "report ..." on stderr; exit 2 and the error on stderr if refused
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "prism_geom.h"
#include "prism_identity.h"
#include "prism_prsx.h"
#include "prism_ref_decode.h"
#include "zlib_prefixed.h"

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

/* ---------- geom: the stock file
u32 material count; each: u16 lightmap, u16 material, u32 count, count * 32 bytes, u32 lightmapped, count * 8
bytes if lightmapped; then u32 collision vertex count and count * 12 bytes (float bits) */

struct stock_file
{
	unsigned char *bytes;
	size_t size;
	uint32_t material_count;
	size_t *offsets;
	uint32_t collision_count;
	size_t collision;
};

static int stock_material(void *context, uint32_t lightmap_index, uint32_t material_index, uint32_t *count,
	const unsigned char **vertices, const unsigned char **lightmap_vertices)
{
	struct stock_file *stock = (struct stock_file *)context;
	uint32_t index;

	for (index = 0; index < stock->material_count; index++)
	{
		const unsigned char *at = stock->bytes + stock->offsets[index];

		if ((uint32_t)(at[0] | at[1] << 8) == lightmap_index && (uint32_t)(at[2] | at[3] << 8) == material_index)
		{
			*count = get_u32(at + 4);
			*vertices = at + 8;
			*lightmap_vertices = get_u32(at + 8 + 32 * (size_t)*count) ? at + 12 + 32 * (size_t)*count : NULL;
			return 1;
		}
	}
	return 0;
}

static int stock_collision(void *context, uint32_t index, uint32_t bits[3])
{
	struct stock_file *stock = (struct stock_file *)context;

	if (index >= stock->collision_count)
		return 0;
	bits[0] = get_u32(stock->bytes + stock->collision + 12 * (size_t)index);
	bits[1] = get_u32(stock->bytes + stock->collision + 12 * (size_t)index + 4);
	bits[2] = get_u32(stock->bytes + stock->collision + 12 * (size_t)index + 8);
	return 1;
}

static unsigned char *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *bytes;
	long end;

	if (!file || fseek(file, 0, SEEK_END) || (end = ftell(file)) < 0 || fseek(file, 0, SEEK_SET))
	{
		if (file)
			fclose(file);
		return NULL;
	}
	bytes = (unsigned char *)malloc((size_t)end + 1);
	if (bytes && fread(bytes, 1, (size_t)end, file) != (size_t)end)
	{
		free(bytes);
		bytes = NULL;
	}
	fclose(file);
	*size = (size_t)end;
	return bytes;
}

static int geom_command(const char *stock_path, const char *sidecar_path, unsigned bsp)
{
	struct stock_file stock;
	struct prism_geom_stock view;
	struct prism_geom_report report;
	struct prsx_source source;
	struct prsx prsx;
	unsigned char *sections[2] = { NULL, NULL }, *image = NULL;
	size_t sizes[2] = { 0, 0 }, image_size = 0, at = 4;
	enum prism_geom_result result;
	FILE *file;
	uint32_t index;
	int k;

	memset(&stock, 0, sizeof(stock));
	stock.bytes = read_file(stock_path, &stock.size);
	if (!stock.bytes || stock.size < 8)
		return 1;
	stock.material_count = get_u32(stock.bytes);
	stock.offsets = (size_t *)calloc(stock.material_count + 1, sizeof(size_t));
	for (index = 0; stock.offsets && index < stock.material_count; index++)
	{
		uint32_t count = get_u32(stock.bytes + at + 4);

		stock.offsets[index] = at;
		at += 8 + 32 * (size_t)count;
		at += 4 + (get_u32(stock.bytes + at) ? 8 * (size_t)count : 0);
	}
	stock.collision_count = get_u32(stock.bytes + at);
	stock.collision = at + 4;
	view.context = &stock;
	view.material = stock_material;
	view.collision_vertex = stock_collision;
	file = fopen(sidecar_path, "rb");
	if (!file || !prsx_source_file(&source, file) || prsx_open(&prsx, &source) != PRSX_OK)
	{
		fprintf(stderr, "sidecar refused\n");
		return 2;
	}
	for (k = 0; k < 2; k++)
	{
		long found = prsx_find(&prsx, k ? PRSX_LMUV : PRSX_GEOM, (uint16_t)bsp, 0);

		if (found < 0)
			continue;
		sizes[k] = (size_t)prsx.entries[found].usize;
		sections[k] = (unsigned char *)malloc(sizes[k] + 1);
		if (!sections[k] || prsx_section_read(&prsx, (uint32_t)found, sections[k], sizes[k]) != PRSX_OK)
		{
			fprintf(stderr, "section unreadable\n");
			return 2;
		}
	}
	result = prism_geom_decode(sections[0], sizes[0], sections[1], sizes[1], &view, "native", &image, &image_size,
		&report);
	fprintf(stderr, "report %s parts %u skipped %u vertices %u triangles %u lit %u ref %u refp %u refm %u refc %u far %d "
		"at %u %u\n", prism_geom_result_string(result), (unsigned)report.parts, (unsigned)report.parts_skipped,
		(unsigned)report.vertices, (unsigned)report.triangles, (unsigned)report.by_op[0], (unsigned)report.by_op[1],
		(unsigned)report.by_op[2], (unsigned)report.by_op[3], (unsigned)report.by_op[4], report.far,
		(unsigned)report.error_part, (unsigned)report.error_vertex);
	if (result == PRISM_GEOM_OK)
		fwrite(image, 1, image_size, stdout);
	free(image);
	free(sections[0]);
	free(sections[1]);
	prsx_close(&prsx);
	fclose(file);
	free(stock.offsets);
	free(stock.bytes);
	return result == PRISM_GEOM_OK ? 0 : 2;
}

static int deflate_command(int level)
{
	size_t size = 0;
	unsigned char *input = read_stdin(&size);
	unsigned char *output;
	uLong bound;
	z_stream z;
	int result;

	if (!input || level < 0 || level > 9 || size > 0x7fffffffu)
	{
		free(input);
		return 2;
	}
	memset(&z, 0, sizeof(z));
	if (deflateInit2(&z, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
	{
		free(input);
		return 2;
	}
	bound = deflateBound(&z, (uLong)size);
	output = (unsigned char *)malloc(bound ? bound : 1);
	if (!output)
	{
		deflateEnd(&z);
		free(input);
		return 2;
	}
	z.next_in = input;
	z.avail_in = (uInt)size;
	z.next_out = output;
	z.avail_out = (uInt)bound;
	result = deflate(&z, Z_FINISH);
	if (result == Z_STREAM_END)
		result = fwrite(output, 1, (size_t)z.total_out, stdout) == (size_t)z.total_out && !fflush(stdout) ? 0 : 2;
	else
		result = 2;
	deflateEnd(&z);
	free(output);
	free(input);
	return result;
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
	if (!strcmp(argv[1], "ladder") && argc == 2)
	{
		int bits;

		/* (every input: "ladder <classic> <geometry> <sidecar> <packs> <unbaked> <path traced> <rung>") */
		for (bits = 0; bits < 64; bits++)
			printf("ladder %d %d %d %d %d %d %d\n", bits & 1, bits >> 1 & 1, bits >> 2 & 1, bits >> 3 & 1, bits >> 4 & 1,
				bits >> 5 & 1, prism_world_ladder(bits & 1, bits >> 1 & 1, bits >> 2 & 1, bits >> 3 & 1, bits >> 4 & 1,
				bits >> 5 & 1));
		return 0;
	}
	if (!strcmp(argv[1], "deflate") && argc == 3)
		return deflate_command(atoi(argv[2]));
	if (!strcmp(argv[1], "geom") && argc == 5)
		return geom_command(argv[2], argv[3], (unsigned)strtoul(argv[4], NULL, 10));
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
