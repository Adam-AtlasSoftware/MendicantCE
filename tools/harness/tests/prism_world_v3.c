/* PRISM's level remodel files, version 3 (port/linux/game/prism_world_geometry.c, test_prism_world_v3.py): the reader
of versions 2 and 3 on hand-made files (the tables' bounds, refusals), the shape's checks (a needle along a sliver that
the old centroid prism let pass, the declared deviations), the exact stock edge of version 3's seal groups, and a group
falling back whole only where its border is the stock edges; a material's parts drawn stock together. */

#include "harness.h"

#include <math.h>

struct structure_bsp;
struct structure_material;
static int logged;
#define platform_log(...) (logged++)

#include "under_test.inc"

/* ---------- files written here */

static byte file_bytes[4096];
static unsigned long file_size;

static void put_u32(unsigned long value)
{
	int i;

	for (i = 0; i < 4; i++)
		file_bytes[file_size++] = (byte)(value >> (8 * i));
}

static void put_u16(unsigned long value)
{
	file_bytes[file_size++] = (byte)value;
	file_bytes[file_size++] = (byte)(value >> 8);
}

static void put_f32(float value)
{
	memcpy(file_bytes + file_size, &value, 4);
	file_size += 4;
}

static void put_pad(void)
{
	while (file_size & 3)
		file_bytes[file_size++] = 0;
}

/* (the knobs of the version 3 file) */
static struct
{
	unsigned long flags, groups, pages, page_size, charts, group, chart, chart_page, u;
	float density, deviation;
	int short_charts;
} v3;

/* one material record of one surface: a triangle of three vertices, its far level the same */
static void write_file(unsigned long version)
{
	float corners[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
	int i, k;

	file_size = 0;
	memcpy(file_bytes, "PWG1", 4);
	file_size = 4;
	put_u32(version);
	put_u32(1);
	put_u32(version >= 3 ? v3.flags : version >= 2 ? 1 : 0);
	for (i = 0; i < 48; i++)
		file_bytes[file_size++] = 0;
	if (version >= 3)
	{
		put_u32(v3.groups);
		put_u32(v3.pages);
		put_u32(v3.page_size);
		put_u32(v3.page_size);
		put_f32(v3.density);
		put_u32(v3.charts);
		for (i = 0; i < 40; i++)
			file_bytes[file_size++] = 0;
	}
	/* the record */
	put_u16(0);
	put_u16(0);
	put_u32(1);  /* stock surfaces */
	put_u32(3);  /* stock vertices */
	put_u32(3);  /* vertices */
	put_u32(1);  /* triangles */
	put_u32(0);  /* topology */
	put_u32(0);
	put_u32(0); /* position check (a double) */
	put_u32(0);  /* first surface */
	put_u32(1);  /* surfaces */
	put_u32(1);  /* near triangles */
	put_f32(8.0f);
	for (i = 0; i < 16; i++)
		file_bytes[file_size++] = 0;
	put_u32(0);
	put_u32(1);
	for (i = 0; i < 3; i++)
	{
		for (k = 0; k < 3; k++)
			put_f32(corners[i][k]);
		for (k = 3; k < 14; k++)
			put_f32(0.0f);
	}
	for (i = 0; i < 3 * 5; i++)
		put_f32(0.0f);
	put_u16(0);
	put_u16(1);
	put_u16(2);
	put_pad();
	if (version >= 2)
	{
		put_u32(0);
		put_u32(1);
		for (i = 0; i < 4; i++)
			put_f32(1.0f);
	}
	if (version >= 3)
	{
		put_u32(v3.group);
		for (i = 0; i < 4; i++)
			put_f32(v3.deviation);
		put_u32(v3.chart);
		for (i = 0; i < 3; i++)
			put_u16(v3.u);
		for (i = 0; i < 3; i++)
			put_u16(8);
		for (i = 0; i < (v3.short_charts ? (int)v3.charts - 1 : (int)v3.charts); i++)
			put_u32(v3.chart_page);
	}
}

static void v3_defaults(void)
{
	v3.flags = PWG_FLAG_FAR | PWG_FLAG_GROUPS | PWG_FLAG_DEVIATION | PWG_FLAG_ATLAS;
	v3.groups = 1;
	v3.pages = 1;
	v3.page_size = 64;
	v3.charts = 1;
	v3.group = 0;
	v3.chart = 0;
	v3.chart_page = 0;
	v3.u = 8;
	v3.density = 4.0f;
	v3.deviation = 0.0f;
	v3.short_charts = 0;
}

static int parse(struct pwg_file *file, struct pwg_material_view **views)
{
	long count = 0;
	int ok = pwg_parse(file_bytes, file_size, NULL, views, &count, file, "test.pwg");

	return ok ? (int)count : -1;
}

static void case_parse(void)
{
	struct pwg_file file;
	struct pwg_material_view *views;

	write_file(2);
	CHECK(parse(&file, &views) == 1, "version 2 read");
	CHECK(file.version == 2 && !views[0].groups && views[0].far_triangles, "version 2's far level, no tables");
	free(views);
	v3_defaults();
	write_file(3);
	CHECK(parse(&file, &views) == 1, "version 3 read (%lu bytes)", file_size);
	CHECK(file.version == 3 && file.group_count == 1 && file.page_count == 1 && file.chart_count == 1 &&
		file.page_width == 64 && file.density == 4.0f, "version 3's figures");
	CHECK(views[0].groups && views[0].deviations && views[0].atlas && read_u32(views[0].atlas) == 0,
		"version 3's tables");
	free(views);
	v3.chart = PWG_NO_CHART;
	write_file(3);
	CHECK(parse(&file, &views) == 1, "a triangle of no chart");
	free(views);
}

static void case_refused(void)
{
	struct pwg_file file;
	struct pwg_material_view *views = NULL;

	v3_defaults();
	v3.group = 1;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "a surface of no group");
	v3_defaults();
	v3.u = 64 * 8 + 1;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "atlas coordinates off the page");
	v3_defaults();
	v3.chart = 1;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "a triangle of no chart in the table");
	v3_defaults();
	v3.chart_page = 1;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "a chart of no page");
	v3_defaults();
	v3.charts = 2;
	v3.short_charts = 1;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "the chart table cut short");
	v3_defaults();
	v3.flags = PWG_FLAG_FAR;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "version 3 without its tables");
	v3_defaults();
	v3.deviation = -1.0f;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "a deviation below zero");
	v3_defaults();
	v3.density = 0.0f;
	write_file(3);
	CHECK(parse(&file, &views) < 0, "an atlas of no density");
	v3_defaults();
	write_file(3);
	file_size -= 2;
	CHECK(parse(&file, &views) < 0, "cut short");
	write_file(4);
	CHECK(parse(&file, &views) < 0, "version 4");
}

/* ---------- views made of vertices and triangles directly */

static byte vertex_bytes[64 * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE];
static byte triangle_bytes[64 * 6];
static byte first_bytes[2 * 4 + 4];
static byte far_bytes[8 * 2];

static void view_of(struct pwg_material_view *view, float const (*points)[3], long point_count, int const (*tris)[3],
	long tri_count)
{
	long i;
	int k;

	memset(view, 0, sizeof(*view));
	memset(vertex_bytes, 0, sizeof(vertex_bytes));
	for (i = 0; i < point_count; i++)
		memcpy(vertex_bytes + i * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, points[i], 12);
	for (i = 0; i < tri_count; i++)
		for (k = 0; k < 3; k++)
		{
			triangle_bytes[6 * i + 2 * k] = (byte)tris[i][k];
			triangle_bytes[6 * i + 2 * k + 1] = 0;
		}
	memset(first_bytes, 0, sizeof(first_bytes));
	first_bytes[4] = (byte)tri_count;
	view->vertices = vertex_bytes;
	view->triangles = triangle_bytes;
	view->surface_first_triangle = first_bytes;
	view->vertex_count = (unsigned long)point_count;
	view->triangle_count = (unsigned long)tri_count;
	view->near_triangle_count = (unsigned long)tri_count;
	view->part_surface_count = 1;
}

static void case_facets(void)
{
	/* a sliver: its corner at (1, 0) a few degrees sharp */
	float corners[9] = { 0, 0, 0, 1, 0, 0, 0, 0.05f, 0 };
	float on[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.05f, 0 }, { 0.3f, 0.01f, 0 } };
	/* a needle out along the line of its edge from (0, 0.05) to (1, 0), 5 wu past the corner: 0.25 past the edge on
	the x axis and inside the other two, within SHAPE_MARGIN of every edge (a10f's, which the old centroid prism let
	pass) */
	float needle[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.05f, 0 }, { 5.9938f, -0.2497f, 0 } };
	/* (outward is minus the corners' cross product: -z) */
	float above[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0.05f, 0 }, { 0.3f, 0.01f, -0.2f } };
	int tris[2][3] = { { 0, 1, 3 }, { 1, 2, 3 } };
	struct pwg_material_view view;
	float declared[4] = { 0.25f, 0.25f, 0.05f, 0.0f };
	unsigned long facets = 0;

	view_of(&view, (float const (*)[3])on, 4, (int const (*)[3])tris, 2);
	CHECK(!shape_run_facets(&view, corners, NULL, 0, 2, &facets) && !facets, "on the triangle: no facet");
	view_of(&view, (float const (*)[3])needle, 4, (int const (*)[3])tris, 2);
	CHECK(shape_run_facets(&view, corners, NULL, 0, 2, &facets) && facets == 2, "the needle along the sliver (%lu)",
		facets);
	view_of(&view, (float const (*)[3])above, 4, (int const (*)[3])tris, 2);
	facets = 0;
	CHECK(!shape_run_facets(&view, corners, NULL, 0, 2, &facets), "0.2 above, within the prism");
	CHECK(!shape_run_facets(&view, corners, declared, 0, 2, &facets), "0.2 above, within the declared 0.25");
	declared[0] = 0.1f;
	CHECK(shape_run_facets(&view, corners, declared, 0, 2, &facets), "0.2 above, past the declared 0.1");
	declared[0] = 1.5f;
	facets = 0;
	CHECK(shape_run_facets(&view, corners, declared, 0, 2, &facets) && facets == 1, "a declared deviation past the "
		"prism: the surface whole");
	declared[0] = 0.0f;
	declared[1] = 0.25f;
	CHECK(shape_run_facets(&view, corners, declared, 0, 2, &facets), "0.2 above when only below is declared");
}

static void case_plain(void)
{
	float u[3] = { 0, 0, 0 }, v[3] = { 1, 0, 0 };
	/* the stock triangle as one triangle: every edge the stock edge */
	float whole[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
	int one[1][3] = { { 0, 1, 2 } };
	/* the edge u-v split at its middle (two triangles along it: a T-junction against a stock neighbour) */
	float split[4][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0.5f, 0, 0 } };
	int two[2][3] = { { 0, 3, 2 }, { 3, 1, 2 } };
	/* the triangle twice (a fold): the edge is not its boundary */
	int doubled[2][3] = { { 0, 1, 2 }, { 0, 2, 1 } };
	/* the near level whole, the far level split */
	int levels[3][3] = { { 0, 1, 2 }, { 0, 3, 2 }, { 3, 1, 2 } };
	struct pwg_material_view view;

	view_of(&view, (float const (*)[3])whole, 3, (int const (*)[3])one, 1);
	CHECK(shape_edge_plain(&view, 0, u, v), "the stock edge");
	CHECK(shape_edge_plain(&view, 0, v, u), "either way about");
	view_of(&view, (float const (*)[3])split, 4, (int const (*)[3])two, 2);
	CHECK(!shape_edge_plain(&view, 0, u, v), "a vertex between the corners");
	view_of(&view, (float const (*)[3])whole, 3, (int const (*)[3])doubled, 2);
	CHECK(!shape_edge_plain(&view, 0, u, v), "the edge twice");
	view_of(&view, (float const (*)[3])split, 4, (int const (*)[3])levels, 3);
	first_bytes[4] = 1;
	memset(far_bytes, 0, sizeof(far_bytes));
	far_bytes[0] = 1;
	far_bytes[4] = 2;
	view.far_triangles = far_bytes;
	view.near_triangle_count = 1;
	CHECK(!shape_edge_plain(&view, 0, u, v), "the near level whole, the far split");
	far_bytes[0] = 0;
	far_bytes[4] = 1;
	CHECK(shape_edge_plain(&view, 0, u, v), "the far level the near's");
}

/* three surfaces in a row: A (0) and B (1) share the edge x = 1, B and C (2) the edge x = 2; A and B are group 0, C
group 1. A has facets. C's side along x = 2 the stock edge or split: group 0 falls back or is kept */
static void case_groups(int mode)
{
	static byte vertices[3][16 * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE];
	static byte triangles[3][8 * 6];
	static byte firsts[3][8];
	static byte group_bytes[3][4];
	struct pwg_material_view views[3];
	struct shape_check check;
	long view_first[4] = { 0, 1, 2, 3 };
	float corners[3][9];
	struct shape_edge edges[9];
	/* (mode 2: A is of a material drawn stock, with no facet; mode 3: B has a facet too, in a group of its own) */
	byte state[3] = { mode == 2 ? _shape_stock : _shape_facets, mode == 3 ? _shape_facets : _shape_kept, _shape_kept };
	long queue[3], first_member[3];
	byte group_state[3] = { 0, 0, 0 };
	unsigned long groups = 0, kept = 0, fallbacks = 0, largest = 0;
	int s, k;

	for (s = 0; s < 3; s++)
	{
		/* A (0, 0) (1, 0) (0, 1); B (1, 0) (1, 1) (0, 1), A's edge from (1, 0) to (0, 1); C (1, 0) (2, 0) (1, 1), B's
		edge from (1, 0) to (1, 1) */
		float strip[3][3][3] = { { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } },
			{ { 1, 0, 0 }, { 2, 0, 0 }, { 1, 1, 0 } } };
		float c[3][3];
		int count = 1;

		memcpy(c, strip[s], sizeof(c));

		memset(&views[s], 0, sizeof(views[s]));
		memset(vertices[s], 0, sizeof(vertices[s]));
		for (k = 0; k < 3; k++)
			memcpy(vertices[s] + k * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, c[k], 12);
		memset(triangles[s], 0, sizeof(triangles[s]));
		triangles[s][0] = 0, triangles[s][2] = 1, triangles[s][4] = 2;
		if (s == 1 && mode == 3)
		{
			/* B's edge from (0, 1) to (1, 0) split at (0.5, 0.5): its side along A is not the stock edge */
			float m[3] = { 0.5f, 0.5f, 0 };

			memcpy(vertices[s] + 3 * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, m, 12);
			triangles[s][0] = 0, triangles[s][2] = 1, triangles[s][4] = 3;
			triangles[s][6] = 3, triangles[s][8] = 1, triangles[s][10] = 2;
			count = 2;
		}
		if (s == 2 && mode == 1)
		{
			/* C's edge from (1, 1) to (1, 0) split at (1, 0.5): its side along B is not the stock edge */
			float m[3] = { 1, 0.5f, 0 };

			memcpy(vertices[s] + 3 * ENVIRONMENT_VERTEX_UNCOMPRESSED_SIZE, m, 12);
			triangles[s][0] = 0, triangles[s][2] = 1, triangles[s][4] = 3;
			triangles[s][6] = 3, triangles[s][8] = 1, triangles[s][10] = 2;
			count = 2;
		}
		memset(firsts[s], 0, sizeof(firsts[s]));
		firsts[s][4] = (byte)count;
		views[s].vertices = vertices[s];
		views[s].triangles = triangles[s];
		views[s].surface_first_triangle = firsts[s];
		views[s].part_surface_count = 1;
		views[s].usable = TRUE;
		memset(group_bytes[s], 0, 4);
		/* (mode 3: each its own group) */
		group_bytes[s][0] = (byte)(mode == 3 ? s : s == 2);
		views[s].groups = group_bytes[s];
		memcpy(corners[s], c, sizeof(c));
	}
	memset(&check, 0, sizeof(check));
	check.views = views;
	check.view_count = 3;
	check.view_first = view_first;
	check.surface_count = 3;
	check.corners = corners[0];
	check.state = state;
	check.queue = queue;
	check.edges = edges;
	for (s = 0; s < 3; s++)
		for (k = 0; k < 3; k++)
		{
			shape_edge_key(corners[s] + 3 * k, corners[s] + 3 * ((k + 1) % 3), &edges[3 * s + k]);
			edges[3 * s + k].surface = s;
		}
	check.edge_count = 9;
	qsort(edges, 9, sizeof(edges[0]), shape_edge_compare);
	if (mode == 3)
	{
		first_member[0] = 0, first_member[1] = 1, first_member[2] = 2;
		queue[0] = queue[1] = queue[2] = NONE;
	}
	else
	{
		first_member[0] = 0, queue[0] = 1, queue[1] = NONE;
		first_member[1] = 2, queue[2] = NONE;
	}
	shape_groups_named(&check, mode == 3 ? 3 : 2, first_member, group_state, &groups, &kept, &fallbacks, &largest);
	if (mode == 0)
	{
		CHECK(state[0] == _shape_fallback && state[1] == _shape_fallback && state[2] == _shape_kept,
			"the group with the facet falls back whole (%d %d %d)", state[0], state[1], state[2]);
		CHECK(groups == 1 && kept == 0 && fallbacks == 2 && largest == 2, "counted (%lu %lu %lu %lu)", groups, kept,
			fallbacks, largest);
	}
	else if (mode == 3)
	{
		/* (B's split side is no crack once both fall back: neither keeps the other) */
		CHECK(state[0] == _shape_fallback && state[1] == _shape_fallback && state[2] == _shape_kept,
			"two groups with facets, the edge between them not the stock edge, both fall back (%d %d %d)", state[0],
			state[1], state[2]);
		CHECK(groups == 2 && kept == 0, "counted (%lu groups, %lu kept)", groups, kept);
	}
	else if (mode == 2)
	{
		CHECK(state[0] == _shape_stock && state[1] == _shape_fallback && state[2] == _shape_kept,
			"the group of a surface drawn stock falls back with it (%d %d %d)", state[0], state[1], state[2]);
		CHECK(fallbacks == 1, "one surface more drawn stock (%lu)", fallbacks);
	}
	else
	{
		CHECK(state[0] == _shape_kept_whole && state[1] == _shape_kept_whole && state[2] == _shape_kept,
			"a border that is not the stock edge keeps the group (%d %d %d)", state[0], state[1], state[2]);
		CHECK(kept == 1 && fallbacks == 0, "counted as kept");
	}
}

/* a material's parts drawn stock together: the draw leaves out the surfaces of a part not made, so one part gone stock
takes the material's other parts, and no other material's */
static void case_parts(void)
{
	struct pwg_material_view views[4];
	long taken;

	memset(views, 0, sizeof(views));
	views[0].lightmap_index = 0, views[0].material_index = 1;
	views[1].lightmap_index = 0, views[1].material_index = 1;
	views[2].lightmap_index = 0, views[2].material_index = 2;
	views[3].lightmap_index = 1, views[3].material_index = 1;
	views[0].usable = views[1].usable = views[2].usable = views[3].usable = TRUE;
	taken = material_parts_stock(views, 4, 1);
	CHECK(!views[0].usable && !views[1].usable, "the part and the other part of its material drawn stock (%d %d)",
		views[0].usable, views[1].usable);
	CHECK(views[2].usable && views[3].usable, "the other materials keep their remodel (%d %d)", views[2].usable,
		views[3].usable);
	CHECK(taken == 1, "one other part taken (%ld)", taken);
	views[2].usable = FALSE;
	taken = material_parts_stock(views, 4, 2);
	CHECK(taken == 0 && views[3].usable, "a material of one part takes nothing else (%ld)", taken);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "parse"))
		case_parse();
	else if (!strcmp(name, "refused"))
		case_refused();
	else if (!strcmp(name, "facets"))
		case_facets();
	else if (!strcmp(name, "plain"))
		case_plain();
	else if (!strcmp(name, "groups"))
		case_groups(0);
	else if (!strcmp(name, "groups_kept"))
		case_groups(1);
	else if (!strcmp(name, "groups_stock"))
		case_groups(2);
	else if (!strcmp(name, "groups_together"))
		case_groups(3);
	else if (!strcmp(name, "parts"))
		case_parts();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
