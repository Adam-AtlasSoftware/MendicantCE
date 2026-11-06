/*
FIRST_PERSON_LEGS_DROP.C

How far the first-person legs are lowered out of the near plane's cut
(port/linux/game/first_person_legs.c's first_person_legs_drop, #692): the
least drop at which none of the legs' points is in the part of the view the
near plane cuts away (between the eye and the near plane, within the view's
sides), each kept the margin outside it. Checked against a view of its own
(a pinhole: the eye, its forward and up, the tangents of its sides, a
viewport off its window's middle as split screen's are, the near distance
as near_clip.c brings it in at a wide view) and against a search of its own:
each drop tried in small steps from none, the first at which no point is in
the cut the answer.
*/

#include "harness.h"
#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define _pi 3.14159265358979323846

typedef struct { real_vector3d n; real d; } real_plane3d;

struct render_frustum
{
	real_plane3d world_planes[6];
	real z_near;
};

#include "under_test.inc"

/* the code's margin and most drop (test_first_person_legs_drop.py checks
them against the code's) */
#define MARGIN 0.01f
#define MOST 0.5f

/* the game's near distance and the reach at a wide view (near_clip.c) */
#define GAME_NEAR 0.0625f
#define REACH 0.2f

struct view
{
	real_point3d eye;
	real_vector3d forward, right, up;
	struct render_frustum frustum;
};

static real_vector3d combine(real_vector3d a, real sa, real_vector3d b, real sb)
{
	real_vector3d v = { a.i * sa + b.i * sb, a.j * sa + b.j * sb, a.k * sa + b.k * sb };
	real length = sqrtf(v.i * v.i + v.j * v.j + v.k * v.k);

	v.i /= length;
	v.j /= length;
	v.k /= length;
	return v;
}

static real dot(real_vector3d a, real_vector3d b)
{
	return a.i * b.i + a.j * b.j + a.k * b.k;
}

/* a view: degrees across at 16:9 (0: the stock 70 across 4:3, widened as the
game does), the window's shape (width over height), the viewport's bounds in
the window (-1 to 1 each way), pitched (degrees, up positive) and turned */
static struct view view_of(double degrees, double shape, double x0, double x1, double y0, double y1,
	double pitch, double yaw)
{
	struct view view;
	double vertical = degrees ? tan(degrees * _pi / 360.0) * (9.0 / 16.0) : 0.75 * 0.85 * tan(35.0 * _pi / 180.0);
	double across = vertical * shape;
	double p = pitch * _pi / 180.0, y = yaw * _pi / 180.0;
	real_vector3d zero = { 0, 0, 0 };
	real_vector3d forward = { (real)(cos(p) * cos(y)), (real)(cos(p) * sin(y)), (real)sin(p) };
	real_vector3d up = { (real)(-sin(p) * cos(y)), (real)(-sin(p) * sin(y)), (real)cos(p) };
	real_vector3d right = { forward.j * up.k - forward.k * up.j, forward.k * up.i - forward.i * up.k,
		forward.i * up.j - forward.j * up.i };
	double corner = sqrt(1.0 + pow(MAX(fabs(x0), fabs(x1)) * across, 2) + pow(MAX(fabs(y0), fabs(y1)) * vertical, 2));
	real z_near = (real)MIN(GAME_NEAR, REACH / corner);
	short plane;

	memset(&view, 0, sizeof(view));
	view.eye.x = 12.5f;
	view.eye.y = -40.25f;
	view.eye.z = 3.0f;
	view.forward = forward;
	view.right = right;
	view.up = up;
	/* (each side's normal outward, through the eye: left, right, bottom, top) */
	view.frustum.world_planes[0].n = combine(forward, (real)(x0 * across), right, -1.f);
	view.frustum.world_planes[1].n = combine(right, 1.f, forward, (real)(-x1 * across));
	view.frustum.world_planes[2].n = combine(forward, (real)(y0 * vertical), up, -1.f);
	view.frustum.world_planes[3].n = combine(up, 1.f, forward, (real)(-y1 * vertical));
	for (plane = 0; plane < 4; plane++)
		view.frustum.world_planes[plane].d = dot(view.frustum.world_planes[plane].n, *(real_vector3d *)&view.eye);
	/* (the near plane's outward toward the eye: its distance the near
	distance less a point's depth) */
	view.frustum.world_planes[4].n = combine(forward, -1.f, zero, 0.f);
	view.frustum.world_planes[4].d = -dot(forward, *(real_vector3d *)&view.eye) - z_near;
	view.frustum.z_near = z_near;
	return view;
}

static struct view level(double degrees)
{
	return view_of(degrees, 16.0 / 9.0, -1, 1, -1, 1, 0, 30);
}

/* a point at so far forward, left and up of the eye, level (the body's
frame: forward the way the eye looks, flattened) */
static real_point3d at(struct view const *view, real forward, real left, real up)
{
	real_vector3d ahead = { view->forward.i, view->forward.j, 0 };
	real length = sqrtf(ahead.i * ahead.i + ahead.j * ahead.j);
	real_point3d point;

	if (length < 1.0e-4f)
	{
		/* (straight up or down: the body faces the way the view's up does
		on the far side) */
		ahead.i = -view->up.i * (view->forward.k < 0 ? -1.f : 1.f);
		ahead.j = -view->up.j * (view->forward.k < 0 ? -1.f : 1.f);
		length = sqrtf(ahead.i * ahead.i + ahead.j * ahead.j);
	}
	ahead.i /= length;
	ahead.j /= length;
	point.x = view->eye.x + ahead.i * forward - ahead.j * left;
	point.y = view->eye.y + ahead.j * forward + ahead.i * left;
	point.z = view->eye.z + up;
	return point;
}

/* whether a point lowered so is in the near plane's cut, by the view's own
measures: its depth less than the near distance and the margin (less a
little: the code's spans end on it), within each side (by more than
a hundredth of a millimetre, as the spans end on the sides too) */
static boolean in_cut(struct view const *view, real_point3d point, real drop, real margin)
{
	real_vector3d from = { point.x - view->eye.x, point.y - view->eye.y, point.z - drop - view->eye.z };
	short plane;

	if (dot(from, view->forward) >= view->frustum.z_near + margin)
		return FALSE;
	for (plane = 0; plane < 4; plane++)
	{
		if (dot(from, view->frustum.world_planes[plane].n) >= -1.0e-5f)
			return FALSE;
	}
	return TRUE;
}

static boolean any_cut(struct view const *view, real_point3d const *points, long count, real drop, real margin)
{
	long index;

	for (index = 0; index < count; index++)
	{
		if (in_cut(view, points[index], drop, margin))
			return TRUE;
	}
	return FALSE;
}

/* the search's answer: the first drop in steps of a fifth of a millimetre
at which none is in the cut (its margin a hair less than the code's), up
to beyond the most */
#define STEP 0.0002f
static real searched(struct view const *view, real_point3d const *points, long count)
{
	real drop;

	for (drop = 0.f; drop < 1.5f; drop += STEP)
	{
		if (!any_cut(view, points, count, drop, MARGIN * 0.999f))
			return drop;
	}
	return 1.5f;
}

static real_point3d points[8192];
static real spans[8192][2];

/* a box of points about the eye (the body's frame, world units), a grid */
static long box(struct view const *view, real back, real front, real right, real left, real low, real high)
{
	long count = 0;
	real f, l, u;

	for (f = back; f <= front + 1.0e-4f; f += 0.02f)
		for (l = right; l <= left + 1.0e-4f; l += 0.03f)
			for (u = low; u <= high + 1.0e-4f; u += 0.02f)
				points[count++] = at(view, f, l, u);
	return count;
}

static real dropped(struct view const *view, long count)
{
	return first_person_legs_drop(&view->frustum, points, count, spans);
}

/* the drop is the search's, and leaves none in the cut */
static void check_least(struct view const *view, long count, char const *what)
{
	real drop = dropped(view, count);
	real expected = searched(view, points, count);

	CHECK(fabsf(drop - expected) <= 2.f * STEP, "%s: dropped %.4f, the least that keeps all out of the cut %.4f",
		what, drop, expected);
	CHECK(!any_cut(view, points, count, drop, MARGIN * 0.99f), "%s: a point is in the cut at %.4f", what, drop);
}

/* the views every case is tried in: the stock view, 90, 120, 150 and 160
across 16:9, 160 at 4:3, 21:9 and 32:9, a portrait screen, split screen's
halves (the top's and the left's), each level, looking down and up */
static struct view views(int index, char *name, size_t size)
{
	static double const degrees[] = { 0, 90, 120, 150, 160, 160, 160, 160, 160, 160, 120 };
	static double const shapes[] = { 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 16.0 / 9.0, 4.0 / 3.0,
		21.0 / 9.0, 32.0 / 9.0, 9.0 / 16.0, 32.0 / 9.0, 8.0 / 9.0 };
	/* (split screen's top half: a window as wide and half as tall, its
	viewport's bounds off the middle; its left half likewise) */
	static double const bounds[][4] = { { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 },
		{ -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 }, { -1, 1, -1, 1 },
		{ -1, 1, -0.5, 1.5 }, { -0.25, 1.75, -1, 1 } };
	static double const pitches[] = { 0, -40, -80, 25 };
	int shape = index / 4;
	int pitch = index % 4;

	snprintf(name, size, "%g degrees, shape %.3f, bounds %g %g %g %g, pitch %g", degrees[shape], shapes[shape],
		bounds[shape][0], bounds[shape][1], bounds[shape][2], bounds[shape][3], pitches[pitch]);
	return view_of(degrees[shape], shapes[shape], bounds[shape][0], bounds[shape][1], bounds[shape][2],
		bounds[shape][3], pitches[pitch], 30.0 + index * 17.0);
}
#define VIEWS 44

int main(int argc, char **argv)
{
	char const *which = argc > 1 ? argv[1] : "";
	char name[160];
	int index;

	if (!strcmp(which, "standing"))
	{
		/* standing on the ground: the waist 0.2 below the eye (the cyborg's),
		the legs below and before it: nothing in the cut at any view, and
		nothing lowered. Crouching (the waist 0.126 below) likewise, but on a
		portrait screen at 160, whose near plane reaches lower */
		for (index = 0; index < VIEWS; index++)
		{
			struct view view = views(index, name, sizeof(name));
			long count = box(&view, -0.1f, 0.24f, -0.12f, 0.12f, -0.8f, -0.2f);

			CHECK(dropped(&view, count) == 0.f, "%s: standing, lowered %.4f", name, dropped(&view, count));
			if (index / 4 == 8)
				continue;
			count = box(&view, -0.1f, 0.24f, -0.12f, 0.12f, -0.6f, -0.126f);
			CHECK(dropped(&view, count) == 0.f, "%s: crouching, lowered %.4f", name, dropped(&view, count));
		}
	}
	else if (!strcmp(which, "crouch_jump"))
	{
		/* crouching in the air: the hips at the eye, the thighs beside and
		before it. Lowered by the least that keeps them out of the cut,
		wherever the view's near plane and sides are */
		boolean any = FALSE;

		for (index = 0; index < VIEWS; index++)
		{
			struct view view = views(index, name, sizeof(name));
			long count = box(&view, -0.08f, 0.2f, -0.12f, 0.12f, -0.5f, 0.06f);

			check_least(&view, count, name);
			any |= dropped(&view, count) > 0.f;
		}
		CHECK(any, "nothing lowered at any view");
		/* (and at a wide one, level, it is lowered: the hips are in the cut) */
		{
			struct view wide = level(160);
			long count = box(&wide, -0.08f, 0.2f, -0.12f, 0.12f, -0.5f, 0.06f);

			CHECK(dropped(&wide, count) > 0.05f, "160 level: lowered only %.4f", dropped(&wide, count));
		}
	}
	else if (!strcmp(which, "beside"))
	{
		/* points nearer than the near plane but out past the view's side:
		lowered, they stay out of it (the sides' normals level), and nothing
		is lowered for them */
		struct view view = level(120);

		points[0] = at(&view, 0.01f, 0.06f, 0.0f);
		points[1] = at(&view, 0.01f, -0.06f, -0.02f);
		points[2] = at(&view, 0.02f, 0.08f, 0.03f);
		CHECK(!any_cut(&view, points, 3, 0.f, MARGIN), "the case's points are in the cut");
		CHECK(dropped(&view, 3) == 0.f, "lowered %.4f for points beside the view", dropped(&view, 3));
	}
	else if (!strcmp(which, "above"))
	{
		/* a point above the view's top, near the eye: out of the cut as it is,
		in it only if it were lowered: nothing is lowered for it */
		struct view view = level(120);

		points[0] = at(&view, 0.03f, 0.0f, 0.12f);
		points[1] = at(&view, 0.04f, 0.02f, 0.2f);
		CHECK(dropped(&view, 2) == 0.f, "lowered %.4f for points above the view", dropped(&view, 2));
		/* (with one in the cut below them: lowered past it, and past them) */
		points[2] = at(&view, 0.03f, 0.0f, 0.0f);
		check_least(&view, 3, "above, and one in the cut");
		CHECK(dropped(&view, 3) > 0.f, "nothing lowered");
	}
	else if (!strcmp(which, "chain"))
	{
		/* points one above another before the eye, the highest first: each
		lowered out of the cut brings the next above into it */
		struct view view = level(120);
		long count = 0;
		int step;

		for (step = 6; step >= 0; step--)
			points[count++] = at(&view, 0.03f, 0.0f, step * 0.04f);
		check_least(&view, count, "chain");
		CHECK(dropped(&view, count) > 0.24f, "lowered only %.4f", dropped(&view, count));
	}
	else if (!strcmp(which, "most"))
	{
		/* a column of points before the eye a metre and a half high: lowered
		no farther than the most */
		struct view view = level(120);
		long count = 0;
		int step;

		for (step = 0; step <= 60; step++)
			points[count++] = at(&view, 0.03f, 0.0f, step * 0.02f);
		CHECK(searched(&view, points, count) > MOST, "the case needs no more than the most");
		CHECK(dropped(&view, count) == MOST, "lowered %.4f, not the most %.2f", dropped(&view, count), MOST);
	}
	else if (!strcmp(which, "none"))
	{
		struct view view = level(160);

		CHECK(dropped(&view, 0) == 0.f, "lowered %.4f for no points", dropped(&view, 0));
	}
	else
	{
		fprintf(stderr, "unknown case %s\n", which);
		return 2;
	}
	return 0;
}
