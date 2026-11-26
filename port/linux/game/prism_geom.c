/*
PRISM_GEOM.C

prism_geom.h has what and why; tools/geopatch/geom.py writes and reads the
same layouts in Python.
*/

#include "prism_geom.h"
#include "prism_ref_decode.h"

#include <stdlib.h>
#include <string.h>

/* geom.py's vertex ops */
enum
{
	OP_LIT = 0,
	OP_REF = 1,
	OP_REFP = 2,
	OP_REFM = 3,
	OP_REFC = 4,
	OP_STOCK_TEX = 8,
	OP_REFPT = OP_REFP | OP_STOCK_TEX,
	OP_REFMT = OP_REFM | OP_STOCK_TEX,
};

enum
{
	GEOM_HEADER_SIZE = 20,
	GEOM_PART_SIZE = 56,
	GEOM_VERSION = 1,
	LMUV_HEADER_SIZE = 16,
	LMUV_VERSION = 1,
	NAMESPACE_STOCK = 0,
	NAMESPACE_PRISM = 1,
	MAXIMUM_COLUMNS = 64,
	ENVIRONMENT_UNCOMPRESSED = 56,
	LIGHTMAP_UNCOMPRESSED = 20,
};

const char *prism_geom_result_string(enum prism_geom_result result)
{
	switch (result)
	{
	case PRISM_GEOM_OK:
		return "ok";
	case PRISM_GEOM_ERROR_LAYOUT:
		return "a section's layout is broken";
	case PRISM_GEOM_ERROR_REFERENCE:
		return "a reference names no stock value of this map";
	case PRISM_GEOM_ERROR_LIGHTMAP:
		return "the lightmap vertices disagree with the geometry";
	case PRISM_GEOM_ERROR_MEMORY:
		return "out of memory";
	}
	return "unknown";
}

static uint32_t geom_u32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t geom_u16(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static void geom_put_u32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

static int32_t geom_unzigzag(uint32_t value)
{
	return (int32_t)((value >> 1) ^ (0u - (value & 1u)));
}

static uint64_t geom_pad4(uint64_t size)
{
	return (size + 3) & ~(uint64_t)3;
}

/* ---------- columns */

struct geom_column
{
	uint32_t fourcc;
	uint32_t width;
	uint32_t count;
	/* (unshuffled: width 1 bytes, width 4 words; NULL if absent) */
	unsigned char *bytes;
	uint32_t *words;
};

struct geom_columns
{
	struct geom_column columns[MAXIMUM_COLUMNS];
	uint32_t count;
};

#define GEOM_FOURCC(a, b, c, d) \
	((uint32_t)(unsigned char)(a) | (uint32_t)(unsigned char)(b) << 8 | \
	 (uint32_t)(unsigned char)(c) << 16 | (uint32_t)(unsigned char)(d) << 24)

static void geom_columns_free(struct geom_columns *columns)
{
	uint32_t index;

	for (index = 0; index < columns->count; index++)
	{
		free(columns->columns[index].bytes);
		free(columns->columns[index].words);
	}
	columns->count = 0;
}

/* the column table at `at` of count entries and the columns after it, which
must end at the data's end; FALSE if malformed (memory: *memory set) */
static int geom_columns_read(const unsigned char *data, size_t size, size_t at, uint32_t count,
	struct geom_columns *columns, int *memory)
{
	uint64_t offset;
	uint32_t index;

	memset(columns, 0, sizeof(*columns));
	if (count > MAXIMUM_COLUMNS || at > size || (uint64_t)count * 16 > size - at)
		return 0;
	offset = at + (uint64_t)count * 16;
	for (index = 0; index < count; index++)
	{
		const unsigned char *entry = data + at + 16 * (size_t)index;
		struct geom_column *column = &columns->columns[index];
		uint32_t width = geom_u32(entry + 4), elements = geom_u32(entry + 8), bytes = geom_u32(entry + 12);
		uint32_t other, plane, element;
		const unsigned char *source;

		column->fourcc = geom_u32(entry);
		column->width = width;
		column->count = elements;
		columns->count = index + 1;
		if ((width != 1 && width != 2 && width != 4 && width != 8) || (uint64_t)width * elements != bytes ||
			bytes > size - offset)
			return 0;
		for (other = 0; other < index; other++)
		{
			if (columns->columns[other].fourcc == column->fourcc)
				return 0;
		}
		source = data + offset;
		if (width == 1)
		{
			column->bytes = (unsigned char *)malloc(elements ? elements : 1);
			if (!column->bytes)
			{
				*memory = 1;
				return 0;
			}
			memcpy(column->bytes, source, elements);
		}
		else if (width == 4)
		{
			column->words = (uint32_t *)malloc(elements ? (size_t)elements * 4 : 4);
			if (!column->words)
			{
				*memory = 1;
				return 0;
			}
			for (element = 0; element < elements; element++)
			{
				uint32_t value = 0;

				for (plane = 0; plane < 4; plane++)
					value |= (uint32_t)source[(size_t)plane * elements + element] << (8 * plane);
				column->words[element] = value;
			}
		}
		/* (another width: a later column, which this reader ignores) */
		offset += bytes;
	}
	return offset == size;
}

/* a column by fourcc and width, or NULL (an absent column is empty) */
static const struct geom_column *geom_column(const struct geom_columns *columns, uint32_t fourcc, uint32_t width)
{
	uint32_t index;

	for (index = 0; index < columns->count; index++)
	{
		if (columns->columns[index].fourcc == fourcc)
			return columns->columns[index].width == width ? &columns->columns[index] : NULL;
	}
	return NULL;
}

static uint32_t geom_column_count(const struct geom_column *column)
{
	return column ? column->count : 0;
}

/* ---------- parts */

struct geom_part
{
	uint32_t lightmap_index, material_index;
	uint32_t stock_surfaces, stock_vertices, topology;
	uint32_t first_surface, surface_count;
	uint32_t vertex_count, triangle_count, near_count;
	uint32_t far_bits, flags, static_set, material_namespace, prism_material;
	/* where its data starts in the global columns */
	uint64_t first_vertex, first_corner, first_run, first_far, first_sphere;
	/* its stock material (stock namespace) */
	uint32_t stock_count;
	const unsigned char *stock_vertices_data;
	const unsigned char *stock_lightmap_data;
	int written;
};

struct geom_state
{
	const struct prism_geom_stock *stock;
	struct prism_geom_report *report;
	/* the vertices' running positions into VREF, VKEY, VPOS, VNRM, VTEX */
	uint64_t ref, key, pos, nrm, tex;
	int32_t last[3];
};

static void geom_put_bits3(unsigned char *to, const uint32_t bits[3])
{
	geom_put_u32(to, bits[0]);
	geom_put_u32(to + 4, bits[1]);
	geom_put_u32(to + 8, bits[2]);
}

/* the packed vectors (normal, binormal, tangent) decoded into the 56-byte
vertex's bytes 12 to 47 */
static void geom_vectors(unsigned char *vertex, const uint32_t packed[3])
{
	uint32_t bits[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		prsx_decode_vector_bits(packed[k], bits);
		geom_put_bits3(vertex + 12 + 12 * k, bits);
	}
}

/* ---------- the decode */

enum prism_geom_result prism_geom_decode(const unsigned char *geom, size_t geom_size, const unsigned char *lmuv,
	size_t lmuv_size, const struct prism_geom_stock *stock, const char *name, unsigned char **image,
	size_t *image_size, struct prism_geom_report *report)
{
	struct geom_columns columns, lightmap_columns;
	struct geom_part *parts = NULL;
	uint32_t part_count, column_count, part_size, lightmap_part_count, lightmap_column_count;
	const struct geom_column *vops, *vref, *vkey, *vpos, *vnrm, *vtex, *tidx, *srun, *frun, *fsph;
	const struct geom_column *lops, *lref, *lvtx;
	uint64_t vertex_total = 0, corner_total = 0, run_total = 0, far_total = 0, sphere_total = 0;
	uint64_t lit = 0, attributed = 0, textured = 0, refm = 0, refs = 0;
	uint64_t lightmap_refs = 0, lightmap_lit = 0, size = 0, at;
	uint32_t index, written = 0;
	enum prism_geom_result result = PRISM_GEOM_ERROR_LAYOUT;
	struct geom_state state;
	unsigned char *out = NULL;
	int memory = 0, far = 1;
	uint64_t lightmap_vertex = 0, lightmap_ref = 0, lightmap_word = 0;

	*image = NULL;
	*image_size = 0;
	memset(report, 0, sizeof(*report));
	memset(&columns, 0, sizeof(columns));
	memset(&lightmap_columns, 0, sizeof(lightmap_columns));
	memset(&state, 0, sizeof(state));
	state.stock = stock;
	state.report = report;

	/* the GEOM header and parts */
	if (!geom || geom_size < GEOM_HEADER_SIZE || memcmp(geom, "GEO1", 4) || geom_u32(geom + 4) != GEOM_VERSION)
		goto done;
	part_count = geom_u32(geom + 8);
	column_count = geom_u32(geom + 12);
	part_size = geom_u32(geom + 16);
	if (part_count > PRISM_GEOM_MAXIMUM_PARTS || part_size < GEOM_PART_SIZE || part_size > 4096 || part_size % 4 ||
		(uint64_t)part_size * part_count > geom_size - GEOM_HEADER_SIZE)
		goto done;
	if (!geom_columns_read(geom, geom_size, GEOM_HEADER_SIZE + (size_t)part_size * part_count, column_count,
		&columns, &memory))
		goto done;
	/* the LMUV header */
	if (!lmuv || lmuv_size < LMUV_HEADER_SIZE || memcmp(lmuv, "LMU1", 4) || geom_u32(lmuv + 4) != LMUV_VERSION)
	{
		result = PRISM_GEOM_ERROR_LIGHTMAP;
		goto done;
	}
	lightmap_part_count = geom_u32(lmuv + 8);
	lightmap_column_count = geom_u32(lmuv + 12);
	if (lightmap_part_count != part_count || (uint64_t)part_count * 4 > lmuv_size - LMUV_HEADER_SIZE ||
		!geom_columns_read(lmuv, lmuv_size, LMUV_HEADER_SIZE + (size_t)part_count * 4, lightmap_column_count,
			&lightmap_columns, &memory))
	{
		result = memory ? PRISM_GEOM_ERROR_MEMORY : PRISM_GEOM_ERROR_LIGHTMAP;
		goto done;
	}
	vops = geom_column(&columns, GEOM_FOURCC('V', 'O', 'P', 'S'), 1);
	vref = geom_column(&columns, GEOM_FOURCC('V', 'R', 'E', 'F'), 4);
	vkey = geom_column(&columns, GEOM_FOURCC('V', 'K', 'E', 'Y'), 4);
	vpos = geom_column(&columns, GEOM_FOURCC('V', 'P', 'O', 'S'), 4);
	vnrm = geom_column(&columns, GEOM_FOURCC('V', 'N', 'R', 'M'), 4);
	vtex = geom_column(&columns, GEOM_FOURCC('V', 'T', 'E', 'X'), 4);
	tidx = geom_column(&columns, GEOM_FOURCC('T', 'I', 'D', 'X'), 4);
	srun = geom_column(&columns, GEOM_FOURCC('S', 'R', 'U', 'N'), 4);
	frun = geom_column(&columns, GEOM_FOURCC('F', 'R', 'U', 'N'), 4);
	fsph = geom_column(&columns, GEOM_FOURCC('F', 'S', 'P', 'H'), 4);
	lops = geom_column(&lightmap_columns, GEOM_FOURCC('L', 'O', 'P', 'S'), 1);
	lref = geom_column(&lightmap_columns, GEOM_FOURCC('L', 'R', 'E', 'F'), 4);
	lvtx = geom_column(&lightmap_columns, GEOM_FOURCC('L', 'V', 'T', 'X'), 4);
	/* (every op known, and the columns' counts what the ops say) */
	for (index = 0; index < geom_column_count(vops); index++)
	{
		uint32_t op = vops->bytes[index];

		if (op != OP_LIT && op != OP_REF && op != OP_REFP && op != OP_REFM && op != OP_REFC && op != OP_REFPT &&
			op != OP_REFMT)
			goto done;
		lit += op == OP_LIT;
		refs += op != OP_LIT;
		attributed += op != OP_REF;
		textured += op != OP_REF && op != OP_REFPT && op != OP_REFMT;
		refm += (op & ~OP_STOCK_TEX) == OP_REFM;
	}
	if (geom_column_count(vpos) != 3 * lit || geom_column_count(vref) != refs || geom_column_count(vkey) != refm ||
		geom_column_count(vnrm) != 3 * attributed || geom_column_count(vtex) != 2 * textured)
		goto done;
	for (index = 0; index < geom_column_count(lops); index++)
	{
		uint32_t op = lops->bytes[index];

		if (op != OP_LIT && op != OP_REF)
		{
			result = PRISM_GEOM_ERROR_LIGHTMAP;
			goto done;
		}
		lightmap_refs += op == OP_REF;
		lightmap_lit += op == OP_LIT;
	}
	if (geom_column_count(lops) != geom_column_count(vops) || geom_column_count(lref) != lightmap_refs ||
		geom_column_count(lvtx) != 2 * lightmap_lit)
	{
		result = PRISM_GEOM_ERROR_LIGHTMAP;
		goto done;
	}

	parts = (struct geom_part *)calloc(part_count ? part_count : 1, sizeof(*parts));
	if (!parts)
	{
		result = PRISM_GEOM_ERROR_MEMORY;
		goto done;
	}
	for (index = 0; index < part_count; index++)
	{
		const unsigned char *record = geom + GEOM_HEADER_SIZE + (size_t)part_size * index;
		struct geom_part *part = &parts[index];

		part->lightmap_index = geom_u16(record);
		part->material_index = geom_u16(record + 2);
		part->stock_surfaces = geom_u32(record + 4);
		part->stock_vertices = geom_u32(record + 8);
		part->topology = geom_u32(record + 12);
		part->first_surface = geom_u32(record + 16);
		part->surface_count = geom_u32(record + 20);
		part->vertex_count = geom_u32(record + 24);
		part->triangle_count = geom_u32(record + 28);
		part->near_count = geom_u32(record + 32);
		part->far_bits = geom_u32(record + 36);
		part->flags = geom_u32(record + 40);
		part->static_set = geom_u32(record + 44);
		part->material_namespace = geom_u32(record + 48);
		part->prism_material = geom_u32(record + 52);
		report->error_part = index;
		if ((part->flags & ~1u) || part->near_count > part->triangle_count || part->surface_count > 0x100000 ||
			part->triangle_count > 0x400000 || part->vertex_count > 0x1000000)
			goto done;
		if (part->material_namespace == NAMESPACE_STOCK)
		{
			if (part->prism_material)
				goto done;
		}
		else if (part->material_namespace == NAMESPACE_PRISM)
		{
			if (part->lightmap_index != PRISM_GEOM_NO_INDEX || part->material_index != PRISM_GEOM_NO_INDEX ||
				part->stock_surfaces || part->stock_vertices || part->topology || part->first_surface ||
				part->surface_count)
				goto done;
		}
		else
			goto done;
		if (geom_u32(lmuv + LMUV_HEADER_SIZE + 4 * (size_t)index) != part->vertex_count)
		{
			result = PRISM_GEOM_ERROR_LIGHTMAP;
			goto done;
		}
		part->first_vertex = vertex_total;
		part->first_corner = corner_total;
		part->first_run = run_total;
		part->first_far = far_total;
		part->first_sphere = sphere_total;
		vertex_total += part->vertex_count;
		corner_total += 3 * (uint64_t)part->triangle_count;
		run_total += part->surface_count + 1;
		if (part->flags & 1)
		{
			far_total += 2 * (uint64_t)part->surface_count;
			sphere_total += 4 * (uint64_t)part->surface_count;
		}
		/* (what this image can draw: a stock pair's part, within 16-bit
		indices; a PRISM material's waits for a renderer that draws it) */
		part->written = part->material_namespace == NAMESPACE_STOCK &&
			part->vertex_count <= PRISM_GEOM_MAXIMUM_PART_VERTICES && part->vertex_count && part->triangle_count;
		if (part->written && stock->material(stock->context, part->lightmap_index, part->material_index,
			&part->stock_count, &part->stock_vertices_data, &part->stock_lightmap_data) &&
			part->stock_count == part->stock_vertices && part->stock_vertices_data)
		{
			written++;
			if (!(part->flags & 1) && part->surface_count)
				far = 0;
		}
		else if (part->written)
		{
			/* (a stock pair this map has not, or of another vertex count: this
			sidecar is of another map) */
			result = PRISM_GEOM_ERROR_REFERENCE;
			goto done;
		}
		else
			report->parts_skipped++;
	}
	if (vertex_total != geom_column_count(vops) || corner_total != geom_column_count(tidx) ||
		run_total != geom_column_count(srun) || far_total != geom_column_count(frun) ||
		sphere_total != geom_column_count(fsph))
		goto done;
	report->parts = written;
	report->far = far;

	/* the image's size */
	size = PRISM_GEOM_PWG_HEADER_SIZE;
	for (index = 0; index < part_count; index++)
	{
		const struct geom_part *part = &parts[index];
		uint32_t triangles = far ? part->triangle_count : part->near_count;

		if (!part->written)
			continue;
		size += PRISM_GEOM_PWG_RECORD_SIZE + geom_pad4(4 * ((uint64_t)part->surface_count + 1)) +
			geom_pad4((uint64_t)part->vertex_count * ENVIRONMENT_UNCOMPRESSED) +
			geom_pad4((uint64_t)part->vertex_count * LIGHTMAP_UNCOMPRESSED) + geom_pad4((uint64_t)triangles * 6) +
			(far ? (uint64_t)part->surface_count * 24 : 0);
	}
	if (size > (uint64_t)0x7FFFFFFF)
		goto done;
	out = (unsigned char *)calloc((size_t)size, 1);
	if (!out)
	{
		result = PRISM_GEOM_ERROR_MEMORY;
		goto done;
	}
	memcpy(out, "PWG1", 4);
	geom_put_u32(out + 4, PRISM_GEOM_PWG_VERSION);
	geom_put_u32(out + 8, written);
	geom_put_u32(out + 12, far ? 1u : 0u);
	if (name)
	{
		size_t length = strlen(name);

		memcpy(out + 16, name, length > 47 ? 47 : length);
	}
	at = PRISM_GEOM_PWG_HEADER_SIZE;

	/* each part: its vertices decoded in order (the running columns move
	through every part, written or not) */
	for (index = 0; index < part_count; index++)
	{
		struct geom_part *part = &parts[index];
		uint32_t triangles = far ? part->triangle_count : part->near_count;
		unsigned char *record = out + at, *runs, *vertices, *lightmaps, *corners;
		uint32_t vertex, k, corner;
		int32_t last_index = 0;
		double positions = 0.0;
		uint32_t previous = 0;

		report->error_part = index;
		if (part->written)
		{
			uint64_t offset = at + PRISM_GEOM_PWG_RECORD_SIZE;

			runs = out + offset;
			offset += geom_pad4(4 * ((uint64_t)part->surface_count + 1));
			vertices = out + offset;
			offset += geom_pad4((uint64_t)part->vertex_count * ENVIRONMENT_UNCOMPRESSED);
			lightmaps = out + offset;
			offset += geom_pad4((uint64_t)part->vertex_count * LIGHTMAP_UNCOMPRESSED);
			corners = out + offset;
			offset += geom_pad4((uint64_t)triangles * 6);
			if (far)
			{
				for (k = 0; k < part->surface_count; k++)
				{
					uint32_t first = frun->words[part->first_far + 2 * k];
					uint32_t count = frun->words[part->first_far + 2 * k + 1];

					if (first > part->triangle_count || count > part->triangle_count - first)
						goto done;
					geom_put_u32(out + offset + 8 * (uint64_t)k, first);
					geom_put_u32(out + offset + 8 * (uint64_t)k + 4, count);
				}
				offset += (uint64_t)part->surface_count * 8;
				for (k = 0; k < 4 * part->surface_count; k++)
					geom_put_u32(out + offset + 4 * (uint64_t)k, fsph->words[part->first_sphere + k]);
				offset += (uint64_t)part->surface_count * 16;
			}
			/* (the stock material's position check, the .pwg's: the sum of
			x + 2 y + 3 z over its vertices, in double) */
			for (vertex = 0; vertex < part->stock_count; vertex++)
			{
				float p[3];

				memcpy(p, part->stock_vertices_data + 32 * (size_t)vertex, sizeof(p));
				positions += (double)p[0] + 2.0 * (double)p[1] + 3.0 * (double)p[2];
			}
			record[0] = (unsigned char)part->lightmap_index;
			record[1] = (unsigned char)(part->lightmap_index >> 8);
			record[2] = (unsigned char)part->material_index;
			record[3] = (unsigned char)(part->material_index >> 8);
			geom_put_u32(record + 4, part->stock_surfaces);
			geom_put_u32(record + 8, part->stock_vertices);
			geom_put_u32(record + 12, part->vertex_count);
			geom_put_u32(record + 16, triangles);
			geom_put_u32(record + 20, part->topology);
			memcpy(record + 24, &positions, 8);
			geom_put_u32(record + 32, part->first_surface);
			geom_put_u32(record + 36, part->surface_count);
			geom_put_u32(record + 40, part->near_count);
			geom_put_u32(record + 44, far ? part->far_bits : 0u);
			at = offset;
		}
		else
		{
			runs = vertices = lightmaps = corners = NULL;
		}
		/* the surfaces' runs */
		for (k = 0; k <= part->surface_count; k++)
		{
			uint32_t first = srun->words[part->first_run + k];

			if (first < previous || first > part->triangle_count)
				goto done;
			previous = first;
			if (runs)
				geom_put_u32(runs + 4 * (size_t)k, first);
		}
		/* the vertices */
		for (vertex = 0; vertex < part->vertex_count; vertex++)
		{
			uint32_t op = vops->bytes[part->first_vertex + vertex];
			uint32_t base = op & ~OP_STOCK_TEX;
			unsigned char *v = vertices ? vertices + (size_t)vertex * ENVIRONMENT_UNCOMPRESSED : NULL;
			unsigned char *l = lightmaps ? lightmaps + (size_t)vertex * LIGHTMAP_UNCOMPRESSED : NULL;
			uint32_t lightmap_op = lops->bytes[part->first_vertex + vertex];
			const unsigned char *source = NULL;
			uint32_t reference = 0, bits[3];

			report->error_vertex = vertex;
			if (op != OP_LIT)
				reference = vref->words[state.ref++];
			if (op == OP_LIT)
			{
				for (k = 0; k < 3; k++)
				{
					state.last[k] = (int32_t)((uint32_t)state.last[k] + (uint32_t)geom_unzigzag(vpos->words[state.pos++]));
					if (!prsx_grid_bits(state.last[k], &bits[k]))
						goto done;
				}
				if (v)
					geom_put_bits3(v, bits);
			}
			else if (base == OP_REF || base == OP_REFP)
			{
				/* (its own material's stock vertex) */
				if (part->material_namespace != NAMESPACE_STOCK)
					goto done;
				if (v)
				{
					if (reference >= part->stock_count)
					{
						result = PRISM_GEOM_ERROR_REFERENCE;
						goto done;
					}
					source = part->stock_vertices_data + 32 * (size_t)reference;
				}
			}
			else if (base == OP_REFM)
			{
				uint32_t key = vkey->words[state.key++];
				uint32_t count = 0;
				const unsigned char *data = NULL, *lightmap_data = NULL;

				if (v)
				{
					if (!stock->material(stock->context, key >> 16, key & 0xFFFF, &count, &data, &lightmap_data) ||
						!data || reference >= count)
					{
						result = PRISM_GEOM_ERROR_REFERENCE;
						goto done;
					}
					source = data + 32 * (size_t)reference;
				}
			}
			else if (base == OP_REFC)
			{
				if (v)
				{
					if (!stock->collision_vertex(stock->context, reference, bits))
					{
						result = PRISM_GEOM_ERROR_REFERENCE;
						goto done;
					}
					geom_put_bits3(v, bits);
				}
			}
			if (op == OP_REF)
			{
				if (v)
					prsx_decode_environment_vertex(source, v);
			}
			else
			{
				uint32_t packed[3];

				packed[0] = vnrm->words[state.nrm++];
				packed[1] = vnrm->words[state.nrm++];
				packed[2] = vnrm->words[state.nrm++];
				if (v)
				{
					/* (a stock position: its float bits as stored) */
					if (source)
						memcpy(v, source, 12);
					geom_vectors(v, packed);
					if (op == OP_REFPT || op == OP_REFMT)
						memcpy(v + 48, source + 24, 8);
				}
				if (op != OP_REFPT && op != OP_REFMT)
				{
					uint32_t u = vtex->words[state.tex++], w = vtex->words[state.tex++];

					if (v)
					{
						geom_put_u32(v + 48, u);
						geom_put_u32(v + 52, w);
					}
				}
			}
			/* its lightmap vertex */
			if (lightmap_op == OP_REF)
			{
				uint32_t stock_index = lref->words[lightmap_ref++];

				if (l)
				{
					if (part->material_namespace != NAMESPACE_STOCK || !part->stock_lightmap_data ||
						stock_index >= part->stock_count)
					{
						result = PRISM_GEOM_ERROR_REFERENCE;
						goto done;
					}
					prsx_decode_lightmap_vertex(part->stock_lightmap_data + 8 * (size_t)stock_index, l);
				}
			}
			else
			{
				unsigned char compressed[8];

				geom_put_u32(compressed, lvtx->words[lightmap_word++]);
				geom_put_u32(compressed + 4, lvtx->words[lightmap_word++]);
				if (l)
					prsx_decode_lightmap_vertex(compressed, l);
			}
			lightmap_vertex++;
			if (part->written)
			{
				report->by_op[base]++;
				report->vertices++;
			}
		}
		/* the triangles (indices from 0 per part) */
		for (corner = 0; corner < 3 * part->triangle_count; corner++)
		{
			last_index = (int32_t)((uint32_t)last_index + (uint32_t)geom_unzigzag(tidx->words[part->first_corner + corner]));
			if (last_index < 0 || (uint32_t)last_index >= part->vertex_count)
				goto done;
			if (corners && corner < 3 * triangles)
			{
				corners[2 * (size_t)corner] = (unsigned char)last_index;
				corners[2 * (size_t)corner + 1] = (unsigned char)(last_index >> 8);
			}
		}
		if (part->written)
			report->triangles += triangles;
	}
	(void)lightmap_vertex;
	if (at != size)
		goto done;
	*image = out;
	*image_size = (size_t)size;
	out = NULL;
	result = PRISM_GEOM_OK;

done:
	if (memory && result == PRISM_GEOM_ERROR_LAYOUT)
		result = PRISM_GEOM_ERROR_MEMORY;
	free(out);
	free(parts);
	geom_columns_free(&columns);
	geom_columns_free(&lightmap_columns);
	return result;
}
