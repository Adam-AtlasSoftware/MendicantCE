/*
FIRST_PERSON_LEGS_VIEW.C

The first-person legs moved from the view between the ticks, which goes with
the body, to the view drawn (render_interpolation.c's
render_interpolation_view_body, #697): turned about the vertical from the
one's heading to the other's and moved from the one's place to the other's,
so that the legs are where the body has them from its own eye. Drawn where
the body is, they trailed the view drawn from the newest step by a tick's
motion, more or less as each step and each frame's aim came, and stepped
against it.
*/

#include "harness.h"
#include <math.h>

#define MAXIMUM_LOCAL_PLAYERS 4

typedef struct
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
} real_matrix4x3;

typedef short director_perspective;
enum
{
	_director_perspective_first_person,
	_director_perspective_third_person,
	_director_perspective_scripted
};

struct observer_result
{
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
};

struct object_datum
{
	struct
	{
		long parent_object_index;
	} object;
};

static struct
{
	short local_player_index;
} render;
static long fake_unit = 7;
static director_perspective fake_perspective;
static boolean fake_inhibited, fake_cinematic;
static struct object_datum fake_object = { { NONE } };

static long player_control_get_unit_index(short local_player_index) { (void)local_player_index; return fake_unit; }
static director_perspective director_get_perspective(short local_player_index)
{
	(void)local_player_index;
	return fake_perspective;
}
static boolean director_inhibited_facing(short local_player_index) { (void)local_player_index; return fake_inhibited; }
static boolean cinematic_in_progress(void) { return fake_cinematic; }
static struct object_datum *object_get(long object_index) { (void)object_index; return &fake_object; }

static struct observer_result blended_views[MAXIMUM_LOCAL_PLAYERS];
static struct observer_result drawn_views[MAXIMUM_LOCAL_PLAYERS];
static boolean drawn_views_valid[MAXIMUM_LOCAL_PLAYERS];

#include "under_test.inc"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

/* a view at a place, turned by yaw about the vertical and pitched (radians,
up positive), its up square to its forward */
static struct observer_result view(real x, real y, real z, real yaw, real pitch)
{
	struct observer_result result;

	result.position.x = x;
	result.position.y = y;
	result.position.z = z;
	result.forward.i = cosf(yaw) * cosf(pitch);
	result.forward.j = sinf(yaw) * cosf(pitch);
	result.forward.k = sinf(pitch);
	result.up.i = -cosf(yaw) * sinf(pitch);
	result.up.j = -sinf(yaw) * sinf(pitch);
	result.up.k = cosf(pitch);
	return result;
}

/* the point as the move takes it (matrix4x3_transform_point's, scale 1) */
static real_point3d moved(real_matrix4x3 const *m, real_point3d p)
{
	real_point3d result;

	result.x = m->forward.i * p.x + m->left.i * p.y + m->up.i * p.z + m->position.x;
	result.y = m->forward.j * p.x + m->left.j * p.y + m->up.j * p.z + m->position.y;
	result.z = m->forward.k * p.x + m->left.k * p.y + m->up.k * p.z + m->position.z;
	return result;
}

/* a point seen from a view, level: how far along its heading, to its left
and above it */
static real_point3d seen(struct observer_result const *v, real_point3d p)
{
	real length = sqrtf(v->forward.i * v->forward.i + v->forward.j * v->forward.j);
	real hi = v->forward.i / length, hj = v->forward.j / length;
	real dx = p.x - v->position.x, dy = p.y - v->position.y;
	real_point3d result;

	result.x = dx * hi + dy * hj;
	result.y = -dx * hj + dy * hi;
	result.z = p.z - v->position.z;
	return result;
}

int main(int argc, char **argv)
{
	char const *case_name = argc == 2 ? argv[1] : "";
	real_matrix4x3 move;
	boolean carried;

	CASE("strafing")
	{
		/* frames at 144 a second over ticks at 30, the player strafing left
		and right, a step's speed changing every tick (#697's case). The body
		and its view are drawn between the last two ticks; the view drawn
		from the newest tick, carried on by its velocity, and its aim swaying
		in each frame. Seen from the view drawn, the legs stay where the body
		has them from its own eye: they do not step against the view */
		real positions[64], velocities[64];
		long tick, frame_index;
		real previous[3] = { 0.0f, 0.0f, 0.0f };

		positions[0] = 0.0f;
		for (tick = 1; tick < 64; tick++)
		{
			/* left and right, a few ticks each, the speed ramping */
			real target = ((tick / 6) & 1) ? -0.075f : 0.075f;

			velocities[tick] = tick > 1 ? velocities[tick - 1] + (target - velocities[tick - 1]) * 0.5f : 0.0f;
			positions[tick] = positions[tick - 1] + velocities[tick];
		}
		for (frame_index = 0; frame_index < 60 * 144 / 30 - 10; frame_index++)
		{
			real t = 2.0f + frame_index / (144.0f / 30.0f);
			long newest = (long)t;
			real fraction = t - (real)newest;
			/* (a tick behind: between the tick before the newest and it) */
			real body_y = positions[newest - 1] + (positions[newest] - positions[newest - 1]) * fraction;
			real view_y = positions[newest] + velocities[newest] * fraction;
			real aim = 0.02f * sinf((real)frame_index * 1.7f);
			real_point3d body_foot = { 10.3f, 4.0f + body_y - 0.1f, -0.05f };
			real_point3d at;

			render.local_player_index = 0;
			fake_perspective = _director_perspective_first_person;
			fake_object.object.parent_object_index = NONE;
			drawn_views_valid[0] = TRUE;
			blended_views[0] = view(10.0f, 4.0f + body_y, 0.6f, 0.0f, -0.8f);
			drawn_views[0] = view(10.0f, 4.0f + view_y, 0.6f, aim, -0.8f);
			carried = render_interpolation_view_body(7, &move);
			CHECK(carried, "not moved");
			at = seen(&drawn_views[0], moved(&move, body_foot));
			if (frame_index > 0)
			{
				CHECK(NEAR(at.x, previous[0]) && NEAR(at.y, previous[1]) && NEAR(at.z, previous[2]),
					"frame %ld: the foot stepped by %f %f %f against the view", frame_index, at.x - previous[0],
					at.y - previous[1], at.z - previous[2]);
			}
			previous[0] = at.x;
			previous[1] = at.y;
			previous[2] = at.z;
		}
	}
	else
	{
		fprintf(stderr, "no case %s\n", case_name);
		return 2;
	}
	return 0;
}
