/*
FIRST_PERSON_LEGS.C

Your own legs in first person (Video Setup > FOV and Viewmodels > Legs): the
player's own body, which the game animates as everyone else sees it but does
not draw for its own player, drawn from the waist down. Look down while
standing, running, strafing or crouching and the legs move as the body's
animation moves them.

Only the drawing is changed: a copy of the body's pose, all but its hips and
legs folded into a lid over the waist, the whole set back a little behind
the eye so the camera never looks out through the hips, and lowered where
the near plane would cut it (first_person_legs_drop). The body itself, its
hit boxes and markers, are the game's as ever. The legs are drawn in the
world with the world's camera, so they keep their place and size at any
field of view.

The body is drawn between the game's ticks, a tick behind, with the view
between them; the view drawn on foot is the newest step's
(display.direct_camera), a tick's motion on and aimed in the frame. Drawn
where the body is, the legs trailed the view by that, more or less as each
step and each frame's aim came, and stepped against it walking and
strafing (#697). So the pose is first moved as the view between the ticks
is moved to the view drawn, level (render_interpolation_view_body): the
legs are where the body has them from its own eye, and move as smoothly as
the rest of the view at any frame rate.

The eye is not always as far above the hips as standing or crouching puts
it. Crouching in the air (a crouch jump) brings the eye down to crouching
height while the body keeps its airborne pose, its hips at the eye's height
and its thighs beside and before it (#692). The near plane cuts them there,
and a wide view, whose bottom and sides reach to beside the eye, shows the
cut pieces, black slivers from the bottom of the screen. So the copy is
lowered as far as keeps all of it, every triangle of its surface, out of
the part of the view the near plane cuts away (FIRST_PERSON_LEGS_UNCUT
beyond it), and down along the view's bottom where that faces up (looking
down so far that the bottom edge of the view points back under the eye),
so that the waist's rim stays as far below the view as it was set back to:
none of it is cut, at
any field of view, window shape, split screen's view or the Panini pass's
(the view as drawn: its frustum, its near plane as near_clip.c brings it
in). Standing, walking and crouching on the ground nothing is that near the
eye, and the legs are drawn where they were.

In the VR mode the whole body is drawn but its head and arms (the arms are
the VR mode's own, from its shoulders to the controllers: vr_hands.c), and
where the real body is rather than the game's: the game's eye is where the
head was at the last recentring and the unit faces where the gun aims, but
the head moves about and turns on its own. So the copy of the pose stands
upright and square over its hips (the game's leans into its aim, the chest
turned to the gun while the feet stay planted), its neck under the real
head, turned to the body's heading (vr.c's: the head's, which it follows
only past a turn and slowly; the holsters' and the arms' shoulders' too),
and it is not set back: the chest closes the waist, and looking down shows
the chest, the legs and the feet below the head, as the arms reach from its
shoulders. Its upper body leans back about the hips as far as keeps what
the eyes see of it out of their near plane, and the collar's opening out of
their view (first_person_legs_vr_lean): looking down, the eyes nod forward
over the chest. Its neck is a little behind the real one (the armour's
chest is deeper than a person's).

In the VR mode's seats (vr.vehicle_view = "first_person": vr_seat.c) the
body is drawn too, once the eye is in the seat and the body is seated
(first_person_legs_vr_seated): as the game poses it in the seat (its legs
the seat's), but moved so that the top of its neck is under where the
real one is with the head as it was recentred (below and behind the seat's
eye, or a cabin's: vr_cabin.c), its upper body turned about its hips toward
the real neck (as far as FIRST_PERSON_LEGS_SEAT_TURN_MOST) and leant back
clear of the eyes as on foot, its head folded away and, where the VR mode
draws its own arms (vr_hands.c: the hands alone, or with the weapon), its
arms too. In the
Scorpion's hatch (vr.scorpion_view = "hatch") it stands, its legs hanging
straight down into the turret, turned to the body's heading. The game's
body, what others see, is as ever: nothing of the unit changes.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "models/model_definitions.h"
#include "units/units.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"

#include "object_mesh.h"


#include <stdlib.h>
#include <string.h>

#ifdef HALO_VR
#include "vr_body.h"

/* port/linux/game/vr_render.c */
boolean vr_render_body_anchor(real_point3d *neck, real *yaw);
boolean vr_render_body_eyes(real_point3d eyes[2], struct render_frustum const *views[2], real *near, real *seconds);
boolean vr_render_body_seat(long object_index, struct vr_body_seat *seat);
#endif

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
unsigned long config_changes(void);
/* port/linux/game/render_interpolation.c */
boolean render_interpolation_view_body(long unit_index, real_matrix4x3 *move);

enum
{
	FIRST_PERSON_LEGS_MAXIMUM_NODES = 64,
	FIRST_PERSON_LEGS_MAXIMUM_RIM = 64,
};

/* how far the body is set back behind the eye at least, world units (about
12 cm), and at most (about 1 m) */
#define FIRST_PERSON_LEGS_SET_BACK 0.04f
#define FIRST_PERSON_LEGS_SET_BACK_MAXIMUM 0.33f
/* how far out of the view the waist's rim is kept (world units: 3 cm) */
#define FIRST_PERSON_LEGS_OUT_OF_VIEW 0.01f
/* how far beyond the near plane the legs are kept where they are in view
(world units: 3 cm; the points of their surface are weighed, so a little
more, for the triangles between them), and the farthest they are lowered
for it (world units: about 1.5 m) */
#define FIRST_PERSON_LEGS_UNCUT 0.01f
#define FIRST_PERSON_LEGS_DROP_MAXIMUM 0.5f
/* the point the upper body folds into: the middle of the waist's rim, a
little above it (world units: about 3 cm), so the skin that joined hips to
chest closes the waist as a low lid, flush with the belt (the body's model is
a hollow shell: folded higher, it stood up as a cone before the eye; lower,
it sank into a funnel) */
#define FIRST_PERSON_LEGS_LID_UP 0.01f

#ifdef HALO_VR
/* the VR mode's body: its neck this far behind the top of the real neck
(world units, along its heading: about 5 cm), the armour's chest being
deeper than a person's: standing, the front of its chest is then about 10
cm before the eyes (a person's is about under them), not 15, and the eyes
nodding over it see past it to the belt sooner. The holster at the right
hip and the pouch at the left (vr.c's, from the head) are still at the
hips' sides, about 5 cm short of their front */
#define FIRST_PERSON_LEGS_VR_BACK 0.016f

/* the VR mode's body leant back from the eyes (first_person_legs_vr_lean):
its upper body, turned back about its hips in steps of this (radians), no
farther than this, as far as keeps all of it that either eye sees (within
FIRST_PERSON_LEGS_VIEW_MARGIN of its view's edges, world units: about 2 cm)
at least the eyes' near plane's distance times FIRST_PERSON_LEGS_CLEAR_SCALE
and FIRST_PERSON_LEGS_CLEAR_MORE (world units: about 2 cm) before it, and
the top of its neck, if seen, FIRST_PERSON_LEGS_NECK_BEHIND behind the eyes
(world units, along its heading: about 27 cm); coming back upright no faster
than FIRST_PERSON_LEGS_UPRIGHT_SPEED (radians a second). Of the body's
surface, so many points at most are weighed (those that can come that near) */
#define FIRST_PERSON_LEGS_LEAN_STEP 0.025f
#define FIRST_PERSON_LEGS_LEAN_MAXIMUM 0.9f
#define FIRST_PERSON_LEGS_VIEW_MARGIN 0.007f
#define FIRST_PERSON_LEGS_CLEAR_SCALE 1.1f
#define FIRST_PERSON_LEGS_CLEAR_MORE 0.007f
#define FIRST_PERSON_LEGS_NECK_BEHIND 0.09f
#define FIRST_PERSON_LEGS_UPRIGHT_SPEED 1.0f
#define FIRST_PERSON_LEGS_LEAN_POINTS 8192
/* the VR mode's body in a seat (first_person_legs_vr_seated): its upper
body turned about its hips toward the real neck no farther than this
(radians: as far as a person seated leans and twists) */
#define FIRST_PERSON_LEGS_SEAT_TURN_MOST 0.6f
/* the VR mode's body's chest as it was drawn last (first_person_legs_vr_back,
for the long gun slung across its back: vr_holster.c): its frame
(at the foot of the chest, up along it to the neck, left across the
shoulders) from the top of the real neck in the body's heading's axes
(world units: forward, left, up), and how far before the frame's middle its
back is (the farthest back of its upper body: negative); and its hips (for
the hip's holster): their middle from the top of the real neck, and how far
left of it their right side is (negative). Before any is drawn, the Master
Chief's standing upright (the chest about 5 cm behind and 32 cm below the
top of the real neck, the back 29 cm behind it; the hips 66 cm below it,
their right side 22 cm right of their middle) */
static struct
{
	real_point3d origin;
	real_vector3d forward, left, up;
	real back;
	real_point3d hips;
	real hips_side;
} first_person_legs_vr_chest =
{
	{ -0.047f / 3.048f, 0.0f, -0.321f / 3.048f },
	{ 1.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f },
	-0.294f / 3.048f,
	{ -0.049f / 3.048f, 0.0f, -0.660f / 3.048f },
	-0.22f / 3.048f,
};
#endif

static real_matrix4x3 first_person_legs_matrices[FIRST_PERSON_LEGS_MAXIMUM_NODES];
/* the body's pose seen from the view drawn (first_person_legs_node_matrices) */
static real_matrix4x3 first_person_legs_seen[FIRST_PERSON_LEGS_MAXIMUM_NODES];
static boolean first_person_legs_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;

	if (changes != config_changes())
	{
		changes = config_changes();
		enabled = config_boolean("display.first_person_legs") != 0;
	}
	return enabled;
}

/* the player's own unit, seen from inside it: its legs drawn (on foot,
alive, not hidden; in the VR mode, in a seat too: first_person_legs_vr_
seated) */
boolean first_person_legs_wanted(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (!first_person_legs_enabled() || !object || object->object.type != _object_type_biped ||
		TEST_FLAG(object->object.damage_flags, _object_dead_bit) ||
		TEST_FLAG(object->object.flags, _object_invisible_bit))
	{
		return FALSE;
	}
#ifdef HALO_VR
	if (object->object.parent_object_index != NONE)
	{
		struct vr_body_seat seat;

		return vr_render_body_seat(object_index, &seat);
	}
#endif
	return object->object.parent_object_index == NONE;
}

#ifdef HALO_VR
/* vr_render.c: the player's body is drawn in its seat (the VR mode's
hands drawn there with it, where the game draws none) */
boolean first_person_legs_seated(
	long object_index)
{
	return first_person_legs_wanted(object_index) && object_get(object_index)->object.parent_object_index != NONE;
}
#endif

/* whether a node's name has part in it, whatever its case (the game's
characters name theirs "bip01 l thigh", "Bip01 Pelvis") */
static boolean first_person_legs_name_has(
	char const *name,
	char const *part)
{
	char lower[TAG_STRING_LENGTH + 1];
	short index;

	for (index = 0; index < TAG_STRING_LENGTH && name[index]; index++)
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]);
	lower[index] = 0;
	return strstr(lower, part) != NULL;
}

/* a node of the legs: the hips, thighs, calves, feet and toes */
static boolean first_person_legs_node(
	char const *name)
{
	static char const *const parts[] = { "pelvis", "thigh", "calf", "foot", "toe", "leg" };
	short index;

	for (index = 0; index < (short)NUMBEROF(parts); index++)
	{
		if (first_person_legs_name_has(name, parts[index]))
			return TRUE;
	}
	return FALSE;
}

/* the middle of the waist's rim in the model's space: the hips' vertices
on triangles that run up into the body above them (FALSE: none), kept for
the mesh it was found in (a new map forgets it: first_person_legs_reset) */
static real_point3d first_person_legs_rim[FIRST_PERSON_LEGS_MAXIMUM_RIM];
static short first_person_legs_rim_count = 0;
static long first_person_legs_waist_key = NONE;
static long first_person_legs_waist_model = NONE;
static boolean first_person_legs_waist_found = FALSE;
static real_point3d first_person_legs_waist_middle;

/* a new map: the waist found, and the mesh it was found in, forgotten (the
new map's models have indices of their own) */
void first_person_legs_reset(
	void)
{
	first_person_legs_waist_key = NONE;
	first_person_legs_waist_model = NONE;
	first_person_legs_rim_count = 0;
	object_mesh_reset();
}

static boolean first_person_legs_waist(
	struct object_mesh const *mesh,
	struct model const *model,
	short pelvis,
	real_point3d *middle)
{
	real_point3d sum = { 0.f, 0.f, 0.f };
	long found = 0;
	long index;

	if (mesh->key == first_person_legs_waist_key && mesh->model_index == first_person_legs_waist_model)
	{
		*middle = first_person_legs_waist_middle;
		return first_person_legs_waist_found;
	}
	first_person_legs_rim_count = 0;
	for (index = 0; index < mesh->triangle_count; index++)
	{
		boolean hips = FALSE;
		boolean above = FALSE;
		short corner;

		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];
			short n;

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
				hips = TRUE;
			for (n = 0; n < 2; n++)
			{
				if (vertex->nodes[n] < model->nodes.count && (n == 0 || vertex->weight < 32767) &&
					!first_person_legs_node(TAG_BLOCK_GET_ELEMENT(&model->nodes, vertex->nodes[n], struct model_node)->name))
				{
					above = TRUE;
				}
			}
		}
		if (!hips || !above)
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
			{
				sum.x += vertex->position.x;
				sum.y += vertex->position.y;
				sum.z += vertex->position.z;
				found++;
				if (first_person_legs_rim_count < FIRST_PERSON_LEGS_MAXIMUM_RIM)
					first_person_legs_rim[first_person_legs_rim_count++] = vertex->position;
			}
		}
	}
	first_person_legs_waist_key = mesh->key;
	first_person_legs_waist_model = mesh->model_index;
	first_person_legs_waist_found = found > 0;
	if (found > 0)
	{
		first_person_legs_waist_middle.x = sum.x / found;
		first_person_legs_waist_middle.y = sum.y / found;
		first_person_legs_waist_middle.z = sum.z / found + FIRST_PERSON_LEGS_LID_UP;
	}
	*middle = first_person_legs_waist_middle;
	return first_person_legs_waist_found;
}

#ifdef HALO_VR
/* a vector turned by the rotation from the frame from (forward, left, up)
to the frame to */
static void first_person_legs_turn(
	real_vector3d *v,
	real_vector3d const from[3],
	real_vector3d const to[3])
{
	real along[3];
	short axis;

	for (axis = 0; axis < 3; axis++)
		along[axis] = dot_product3d(v, &from[axis]);
	v->i = to[0].i * along[0] + to[1].i * along[1] + to[2].i * along[2];
	v->j = to[0].j * along[0] + to[1].j * along[1] + to[2].j * along[2];
	v->k = to[0].k * along[0] + to[1].k * along[1] + to[2].k * along[2];
}

/* a node's matrix turned so (first_person_legs_turn) about pivot, then
moved by move */
static void first_person_legs_rotate(
	real_matrix4x3 *matrix,
	real_vector3d const from[3],
	real_vector3d const to[3],
	real_point3d const *pivot,
	real_vector3d const *move)
{
	real_vector3d offset;

	first_person_legs_turn(&matrix->forward, from, to);
	first_person_legs_turn(&matrix->left, from, to);
	first_person_legs_turn(&matrix->up, from, to);
	vector_from_points3d(pivot, &matrix->position, &offset);
	first_person_legs_turn(&offset, from, to);
	matrix->position.x = pivot->x + offset.i + move->i;
	matrix->position.y = pivot->y + offset.j + move->j;
	matrix->position.z = pivot->z + offset.k + move->k;
}

/* whether an eye's view (its frustum, each plane's distance outward) has
a point in it, within FIRST_PERSON_LEGS_VIEW_MARGIN of its sides, and less
than within before the eye */
static boolean first_person_legs_seen_within(
	struct render_frustum const *view,
	real_point3d const *point,
	real within)
{
	/* (the near plane's distance outward is its own distance less the
	point's depth; then left, right, bottom and top) */
	real near_side = plane3d_distance_to_point(&view->world_planes[4], point);
	short side;

	if (near_side < view->z_near - within || near_side >= view->z_near)
		return FALSE;
	for (side = 0; side < 4; side++)
	{
		if (plane3d_distance_to_point(&view->world_planes[side], point) > FIRST_PERSON_LEGS_VIEW_MARGIN)
			return FALSE;
	}
	return TRUE;
}

/* a vertex of the body's mesh as the game would draw it (its nodes' blend,
skin: each node's from the model's space), split in two: the part on nodes
that do not lean and the pivot (base), and the part turned about the pivot
(arm); FALSE: not seen (the head and arms folded away) */
static boolean first_person_legs_vr_lean_split(
	struct object_mesh const *mesh,
	long vertex_index,
	long count,
	real_matrix4x3 const *skin,
	boolean const *leans,
	real_point3d const *pivot,
	real_point3d *base,
	real_vector3d *arm)
{
	struct object_mesh_vertex const *vertex = &mesh->vertices[vertex_index];
	real weights[2];
	boolean shown = FALSE;
	short n;

	*base = *global_origin3d;
	*arm = *global_zero_vector3d;
	weights[0] = vertex->weight >= 32767 ? 1.f : vertex->weight / 32767.f;
	weights[1] = 1.f - weights[0];
	for (n = 0; n < 2; n++)
	{
		real_point3d p;

		if (weights[n] <= 0.f || vertex->nodes[n] >= count)
			continue;
		matrix4x3_transform_point(&skin[vertex->nodes[n]], &vertex->position, &p);
		if (first_person_legs_matrices[vertex->nodes[n]].scale > 0.01f)
			shown = TRUE;
		if (leans[vertex->nodes[n]])
		{
			arm->i += (p.x - pivot->x) * weights[n];
			arm->j += (p.y - pivot->y) * weights[n];
			arm->k += (p.z - pivot->z) * weights[n];
			base->x += pivot->x * weights[n];
			base->y += pivot->y * weights[n];
			base->z += pivot->z * weights[n];
		}
		else
		{
			base->x += p.x * weights[n];
			base->y += p.y * weights[n];
			base->z += p.z * weights[n];
		}
	}
	return shown;
}

/* each vertex's split (first_person_legs_vr_lean_split) for one lean: made
the first time a triangle has the vertex as a corner, and kept for the
others that share it (most vertices are corners of about six), which gives
the same points as working each corner out again. NULL where the memory
cannot be had: each corner is worked out as it comes */
struct first_person_legs_split
{
	real_point3d base;
	real_vector3d arm;
	/* 0: not yet; 1: not seen; 2: seen */
	byte state;
};

static struct first_person_legs_split *first_person_legs_splits(
	long vertex_count)
{
	static struct first_person_legs_split *splits = NULL;
	long index;

	if (!splits)
		splits = malloc(sizeof(*splits) * OBJECT_MESH_MAXIMUM_VERTICES);
	if (!splits)
		return NULL;
	for (index = 0; index < vertex_count && index < OBJECT_MESH_MAXIMUM_VERTICES; index++)
		splits[index].state = 0;
	return splits;
}

/* a triangle's corner's split, kept in splits (if any) */
static boolean first_person_legs_vr_lean_corner(
	struct object_mesh const *mesh,
	struct first_person_legs_split *splits,
	long vertex_index,
	long count,
	real_matrix4x3 const *skin,
	boolean const *leans,
	real_point3d const *pivot,
	real_point3d *base,
	real_vector3d *arm)
{
	struct first_person_legs_split *split;

	if (!splits || vertex_index < 0 || vertex_index >= mesh->vertex_count || vertex_index >= OBJECT_MESH_MAXIMUM_VERTICES)
		return first_person_legs_vr_lean_split(mesh, vertex_index, count, skin, leans, pivot, base, arm);
	split = &splits[vertex_index];
	if (!split->state)
	{
		split->state = first_person_legs_vr_lean_split(mesh, vertex_index, count, skin, leans, pivot, &split->base,
			&split->arm) ? 2 : 1;
	}
	*base = split->base;
	*arm = split->arm;
	return split->state == 2;
}

/* the VR mode's body leant back about its hips (pivot; the top of its neck;
its heading's forward, and left), as little as keeps it clear of the eyes:
the eyes nod forward over the chest as the head looks down (the neck stays
under them), and the near plane, about 19 cm before them, would cut into
the armour of its chest and collar, showing it hollow. Its hips and legs
stand where they are, as do the holsters and the pouch about them; its
upper body leans back, out of the eyes' way, as far as it must, and no
farther. Only what the eyes see counts, as they look now: looking ahead,
or down at the belt, it stands upright (the chest's armour below and behind
the eyes, out of their view, the arms' shoulders over its own); looking
down over the chest, it leans back, as little as it can about the hips (it
leant farther about the waist, nearer the chest, the chest turned up into
the view and the belly bulging before the feet). Nor
do the eyes see the opening of its collar (the head folded away, the dark
top of the neck, a pit before the eyes), which a chin would hide: seen,
it is behind them. Each vertex of the upper body is where the game would
draw it: its nodes' blend, the part on nodes that do not lean and the pivot
(base), and the part turned about the pivot (arm) */
static void first_person_legs_vr_lean(
	long object_index,
	struct model const *model,
	long count,
	real_point3d const *pivot,
	real_point3d const *neck_top,
	real_vector3d const *forward,
	real_vector3d const *left)
{
	static real_point3d base[FIRST_PERSON_LEGS_LEAN_POINTS];
	static real_vector3d arm[FIRST_PERSON_LEGS_LEAN_POINTS];
	static real lean = 0.f;
	struct object_mesh const *mesh = object_mesh_get(object_index);
	real_matrix4x3 skin[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	boolean leans[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	real_point3d eyes[2];
	struct render_frustum const *views[2];
	real_vector3d from[3], to[3], move, neck;
	real near, seconds, clear, seen_reach = 0.f, wanted, angle, eyes_ahead, neck_forward, neck_up;
	long index, weighed = 0;
	short eye, corner;
	struct first_person_legs_split *splits;

	if (!mesh || mesh->node_count < count || !vr_render_body_eyes(eyes, views, &near, &seconds))
		return;
	clear = near * FIRST_PERSON_LEGS_CLEAR_SCALE + FIRST_PERSON_LEGS_CLEAR_MORE;
	/* (how far from an eye a point it sees less than clear before it may
	be: out to its view's corners at that depth, and the margin) */
	for (eye = 0; eye < 2; eye++)
	{
		for (corner = 0; corner < 4; corner++)
		{
			real reach = distance3d(&views[eye]->world_vertices[corner], &views[eye]->world_vertices[4]) /
				views[eye]->z_far * clear + 2.f * FIRST_PERSON_LEGS_VIEW_MARGIN;

			seen_reach = MAX(seen_reach, reach);
		}
	}
	/* (the eyes ahead of the pivot, and the top of the neck from it, along
	the heading and up) */
	eyes_ahead = ((eyes[0].x + eyes[1].x) * 0.5f - pivot->x) * forward->i +
		((eyes[0].y + eyes[1].y) * 0.5f - pivot->y) * forward->j;
	vector_from_points3d(pivot, neck_top, &neck);
	neck_forward = dot_product3d(&neck, forward);
	neck_up = neck.k;
	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		leans[index] = node->parent_node_index != NONE && !first_person_legs_node(node->name);
		matrix4x3_multiply(&first_person_legs_matrices[index], &mesh->inverse[index], &skin[index]);
	}
	splits = first_person_legs_splits(mesh->vertex_count);

	/* (the points of the upper body's surface that a lean could bring
	within seen_reach of an eye, its arm's reach about the pivot crossing
	that sphere: each triangle's corners, the middles of its sides and its
	middle, as a large triangle may cross the view with its corners out of
	it; a lean turns them as their corners) */
	for (index = 0; index < mesh->triangle_count; index++)
	{
		real_point3d corner_base[3];
		real_vector3d corner_arm[3];
		boolean leaning = FALSE, shown = FALSE;
		short point;

		for (corner = 0; corner < 3; corner++)
		{
			if (first_person_legs_vr_lean_corner(mesh, splits, mesh->triangles[index][corner], count, skin, leans, pivot,
				&corner_base[corner], &corner_arm[corner]))
			{
				shown = TRUE;
			}
			if (magnitude_squared3d(&corner_arm[corner]) > 0.f)
				leaning = TRUE;
		}
		/* (not the head and arms folded away: a point, not seen) */
		if (!leaning || !shown)
			continue;
		for (point = 0; point < 7 && weighed < FIRST_PERSON_LEGS_LEAN_POINTS; point++)
		{
			/* (the corners, the sides' middles, the middle) */
			static real const thirds[7][3] =
			{
				{ 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f },
				{ 0.5f, 0.5f, 0.f }, { 0.f, 0.5f, 0.5f }, { 0.5f, 0.f, 0.5f },
				{ 1.f / 3.f, 1.f / 3.f, 1.f / 3.f },
			};
			real_point3d b = { 0.f, 0.f, 0.f };
			real_vector3d a = { 0.f, 0.f, 0.f };
			boolean near_eye = FALSE;
			real reach, distance;

			for (corner = 0; corner < 3; corner++)
			{
				real w = thirds[point][corner];

				b.x += corner_base[corner].x * w;
				b.y += corner_base[corner].y * w;
				b.z += corner_base[corner].z * w;
				a.i += corner_arm[corner].i * w;
				a.j += corner_arm[corner].j * w;
				a.k += corner_arm[corner].k * w;
			}
			reach = magnitude3d(&a);
			for (eye = 0; eye < 2; eye++)
			{
				distance = distance3d(&b, &eyes[eye]);
				if (distance - reach < seen_reach && reach - distance < seen_reach)
					near_eye = TRUE;
			}
			if (!near_eye)
				continue;
			base[weighed] = b;
			arm[weighed] = a;
			weighed++;
		}
	}

	/* (the least lean that keeps them all clear of what the eyes see;
	leaning back at once, as far as needed, and coming upright again
	smoothly: none less than it may come to this frame is tried) */
	for (wanted = MAX(lean - FIRST_PERSON_LEGS_UPRIGHT_SPEED * seconds, 0.f); wanted < FIRST_PERSON_LEGS_LEAN_MAXIMUM;
		wanted += FIRST_PERSON_LEGS_LEAN_STEP)
	{
		real c = (real)cos(wanted), s = (real)sin(wanted);
		boolean too_near = FALSE;

		if (neck_forward * c - neck_up * s > eyes_ahead - FIRST_PERSON_LEGS_NECK_BEHIND)
		{
			real_point3d p;

			/* (turned back as the points below are) */
			p.x = pivot->x + neck.i + forward->i * (neck_forward * (c - 1.f) - neck_up * s);
			p.y = pivot->y + neck.j + forward->j * (neck_forward * (c - 1.f) - neck_up * s);
			p.z = pivot->z + neck.k + neck_forward * s + neck_up * (c - 1.f);
			for (eye = 0; eye < 2; eye++)
			{
				if (first_person_legs_seen_within(views[eye], &p, 1000.f))
					too_near = TRUE;
			}
		}
		for (index = 0; index < weighed && !too_near; index++)
		{
			real_vector3d const *a = &arm[index];
			real along_forward = dot_product3d(a, forward), along_up = a->k;
			real_point3d p;

			/* (turned back: forward toward up, up toward backward) */
			p.x = base[index].x + a->i + forward->i * (along_forward * (c - 1.f) - along_up * s);
			p.y = base[index].y + a->j + forward->j * (along_forward * (c - 1.f) - along_up * s);
			p.z = base[index].z + a->k + along_forward * s + along_up * (c - 1.f);
			for (eye = 0; eye < 2; eye++)
			{
				if (first_person_legs_seen_within(views[eye], &p, clear))
					too_near = TRUE;
			}
		}
		if (!too_near)
			break;
	}
	if (wanted > FIRST_PERSON_LEGS_LEAN_MAXIMUM)
		wanted = FIRST_PERSON_LEGS_LEAN_MAXIMUM;
	lean = wanted;
	if (lean <= 0.f)
		return;

	angle = lean;
	from[0] = *forward;
	from[1] = *left;
	from[2] = *global_up3d;
	to[0].i = forward->i * (real)cos(angle);
	to[0].j = forward->j * (real)cos(angle);
	to[0].k = (real)sin(angle);
	to[1] = *left;
	to[2].i = -forward->i * (real)sin(angle);
	to[2].j = -forward->j * (real)sin(angle);
	to[2].k = (real)cos(angle);
	move = *global_zero_vector3d;
	for (index = 0; index < count; index++)
	{
		if (leans[index])
			first_person_legs_rotate(&first_person_legs_matrices[index], from, to, pivot, &move);
	}
}

/* the VR mode's body's chest as it is drawn (chest: its node, the neck's
parent; neck; hips: the root, the pelvis; the top of the real neck, and the
heading's forward and left there), and how far its back is behind it, and
its hips' right side: first_person_legs_vr_chest */
static void first_person_legs_vr_chest_note(
	long object_index,
	struct model const *model,
	long count,
	short chest,
	short neck,
	short hips,
	real_point3d const *anchor,
	real_vector3d const *forward,
	real_vector3d const *left)
{
	struct object_mesh const *mesh = object_mesh_get(object_index);
	real_matrix4x3 skin[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	boolean upper[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	real_point3d const *origin = &first_person_legs_matrices[chest].position;
	real_point3d const *middle = &first_person_legs_matrices[hips].position;
	real_vector3d up, across, ahead, offset;
	real along, back = 0.0f, side = 0.0f;
	long index;

	vector_from_points3d(origin, &first_person_legs_matrices[neck].position, &up);
	if (!mesh || mesh->node_count < count || normalize3d(&up) <= 0.0f)
		return;
	across = *left;
	along = dot_product3d(&across, &up);
	across.i -= up.i * along;
	across.j -= up.j * along;
	across.k -= up.k * along;
	if (normalize3d(&across) <= 0.0f)
		return;
	cross_product3d(&across, &up, &ahead);
	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		/* (the upper body as drawn: not the legs, nor the head and arms
		folded away) */
		upper[index] = node->parent_node_index != NONE && !first_person_legs_node(node->name) &&
			first_person_legs_matrices[index].scale > 0.01f;
		matrix4x3_multiply(&first_person_legs_matrices[index], &mesh->inverse[index], &skin[index]);
	}
	for (index = 0; index < mesh->vertex_count; index++)
	{
		struct object_mesh_vertex const *vertex = &mesh->vertices[index];
		real_point3d p;

		if (vertex->nodes[0] >= count || (!upper[vertex->nodes[0]] && vertex->nodes[0] != hips))
			continue;
		matrix4x3_transform_point(&skin[vertex->nodes[0]], &vertex->position, &p);
		if (vertex->nodes[0] == hips)
		{
			/* (the hips' right side, level) */
			vector_from_points3d(middle, &p, &offset);
			along = dot_product3d(&offset, left);
			if (along < side)
				side = along;
			continue;
		}
		vector_from_points3d(origin, &p, &offset);
		along = dot_product3d(&offset, &ahead);
		if (along < back)
			back = along;
	}
	/* (from the top of the real neck, in the heading's axes) */
	vector_from_points3d(anchor, origin, &offset);
	first_person_legs_vr_chest.origin.x = dot_product3d(&offset, forward);
	first_person_legs_vr_chest.origin.y = dot_product3d(&offset, left);
	first_person_legs_vr_chest.origin.z = offset.k;
	first_person_legs_vr_chest.forward.i = dot_product3d(&ahead, forward);
	first_person_legs_vr_chest.forward.j = dot_product3d(&ahead, left);
	first_person_legs_vr_chest.forward.k = ahead.k;
	first_person_legs_vr_chest.left.i = dot_product3d(&across, forward);
	first_person_legs_vr_chest.left.j = dot_product3d(&across, left);
	first_person_legs_vr_chest.left.k = across.k;
	first_person_legs_vr_chest.up.i = dot_product3d(&up, forward);
	first_person_legs_vr_chest.up.j = dot_product3d(&up, left);
	first_person_legs_vr_chest.up.k = up.k;
	first_person_legs_vr_chest.back = back;
	vector_from_points3d(anchor, middle, &offset);
	first_person_legs_vr_chest.hips.x = dot_product3d(&offset, forward);
	first_person_legs_vr_chest.hips.y = dot_product3d(&offset, left);
	first_person_legs_vr_chest.hips.z = offset.k;
	first_person_legs_vr_chest.hips_side = side;
}

/* vr_holster.c: the chest of the player's body as it was drawn last, from where the top of the real neck is now and the body's heading
(world yaw): its frame (at the foot of the chest, forward, left and up
along it) in the world, and how far before the frame's middle its back is
(world units, negative); the Master Chief's standing upright before it is
drawn (display.first_person_legs off) */
void first_person_legs_vr_back(real_point3d const *anchor, real yaw, real_matrix4x3 *chest, real *back)
{
	real c = (real)cos(yaw), s = (real)sin(yaw);
	real_point3d const *o = &first_person_legs_vr_chest.origin;
	real_vector3d const *f = &first_person_legs_vr_chest.forward, *u = &first_person_legs_vr_chest.up;

	chest->scale = 1.0f;
	chest->position.x = anchor->x + o->x * c - o->y * s;
	chest->position.y = anchor->y + o->x * s + o->y * c;
	chest->position.z = anchor->z + o->z;
	chest->forward.i = f->i * c - f->j * s;
	chest->forward.j = f->i * s + f->j * c;
	chest->forward.k = f->k;
	chest->up.i = u->i * c - u->j * s;
	chest->up.j = u->i * s + u->j * c;
	chest->up.k = u->k;
	cross_product3d(&chest->up, &chest->forward, &chest->left);
	*back = first_person_legs_vr_chest.back;
}

/* vr_holster.c: the hips of the player's body as they were drawn last,
from where the top of the real neck is now and the body's heading (world
yaw): their middle in the world, and how far left of it their right side is
(world units, negative) */
void first_person_legs_vr_hips(real_point3d const *anchor, real yaw, real_point3d *middle, real *side)
{
	real c = (real)cos(yaw), s = (real)sin(yaw);
	real_point3d const *h = &first_person_legs_vr_chest.hips;

	middle->x = anchor->x + h->x * c - h->y * s;
	middle->y = anchor->y + h->x * s + h->y * c;
	middle->z = anchor->z + h->z;
	*side = first_person_legs_vr_chest.hips_side;
}

/* the pose to draw the body with in the VR mode, the head's place and the
body's heading the real ones (vr_render_body_anchor); NULL: the game's
(no eye pass, a body without a spine, a neck and upper arms named) */
static real_matrix4x3 const *first_person_legs_vr_body(
	long object_index,
	struct object_datum const *object,
	struct model const *model,
	long count,
	real_matrix4x3 const *nodes)
{
	short pelvis = NONE, spine = NONE, neck = NONE, head = NONE, left_arm = NONE, right_arm = NONE;
	short index;
	real_point3d anchor, collar;
	real_vector3d from[3], to[3], move;
	real heading, along, c, s;

	if (!vr_render_body_anchor(&anchor, &heading))
		return NULL;
	for (index = 0; index < count; index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

		if (pelvis == NONE && first_person_legs_name_has(name, "pelvis"))
			pelvis = index;
		if (spine == NONE && first_person_legs_name_has(name, "spine"))
			spine = index;
		if (neck == NONE && first_person_legs_name_has(name, "neck"))
			neck = index;
		if (head == NONE && first_person_legs_name_has(name, "head"))
			head = index;
		if (left_arm == NONE && first_person_legs_name_has(name, "l upperarm"))
			left_arm = index;
		if (right_arm == NONE && first_person_legs_name_has(name, "r upperarm"))
			right_arm = index;
	}
	if (pelvis == NONE || spine == NONE || neck == NONE || head == NONE || left_arm == NONE || right_arm == NONE)
		return NULL;

	/* (the upper body upright and square over the hips: the frame of its
	spine, from its foot up to the neck and across the shoulders, turned
	about its foot to the body's own, upright and to the unit's left) */
	vector_from_points3d(&nodes[spine].position, &nodes[neck].position, &from[2]);
	vector_from_points3d(&nodes[right_arm].position, &nodes[left_arm].position, &from[1]);
	if (normalize3d(&from[2]) <= 0.f)
		return NULL;
	along = dot_product3d(&from[1], &from[2]);
	from[1].i -= from[2].i * along;
	from[1].j -= from[2].j * along;
	from[1].k -= from[2].k * along;
	if (normalize3d(&from[1]) <= 0.f)
		return NULL;
	cross_product3d(&from[1], &from[2], &from[0]);
	to[0].i = object->object.forward.i;
	to[0].j = object->object.forward.j;
	to[0].k = 0.f;
	if (normalize3d(&to[0]) <= 0.f)
		return NULL;
	to[1].i = -to[0].j;
	to[1].j = to[0].i;
	to[1].k = 0.f;
	to[2] = *global_up3d;
	move = *global_zero_vector3d;
	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		first_person_legs_matrices[index] = nodes[index];
		if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
			first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &nodes[spine].position, &move);
	}
	/* (the head folded into the top of the neck, and each arm into its
	shoulder, closing them: folded into the base of the neck, the neck
	opened a dark hole before the eyes, and the arms drew the armpits up
	across the chest; the collarbones are the shoulders' tops) */
	collar = first_person_legs_matrices[neck].position;
	for (index = 0; index < count; index++)
	{
		short up;

		for (up = index; up != NONE && up != head && up != left_arm && up != right_arm;
			up = TAG_BLOCK_GET_ELEMENT(&model->nodes, up, struct model_node)->parent_node_index);
		if (up != NONE)
		{
			first_person_legs_matrices[index].position = first_person_legs_matrices[up].position;
			first_person_legs_matrices[index].scale = 0.001f;
		}
	}

	/* (all of it turned from the unit's facing to the body's heading about
	the base of the neck, and that moved under the top of the real neck, a
	little behind it (FIRST_PERSON_LEGS_VR_BACK), at its own height: the
	body stands where it stands, the head bowing or not) */
	from[0] = to[0];
	from[1] = to[1];
	from[2] = to[2];
	c = (real)cos(heading);
	s = (real)sin(heading);
	to[0].i = c;
	to[0].j = s;
	to[0].k = 0.f;
	to[1].i = -s;
	to[1].j = c;
	to[1].k = 0.f;
	move.i = anchor.x - collar.x - c * FIRST_PERSON_LEGS_VR_BACK;
	move.j = anchor.y - collar.y - s * FIRST_PERSON_LEGS_VR_BACK;
	move.k = 0.f;
	for (index = 0; index < count; index++)
		first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &collar, &move);
	/* (and its upper body leant back about its hips, as far as keeps it
	clear of the eyes) */
	first_person_legs_vr_lean(object_index, model, count, &first_person_legs_matrices[pelvis].position,
		&first_person_legs_matrices[head].position, &to[0], &to[1]);
	/* (and its chest's frame and back noted, as drawn, for the long gun
	slung across it, and its hips' side, for the pistol at the hip) */
	if (TAG_BLOCK_GET_ELEMENT(&model->nodes, neck, struct model_node)->parent_node_index != NONE)
	{
		first_person_legs_vr_chest_note(object_index, model, count,
			TAG_BLOCK_GET_ELEMENT(&model->nodes, neck, struct model_node)->parent_node_index, neck, pelvis, &anchor,
			&to[0], &to[1]);
	}
	return first_person_legs_matrices;
}

/* the frames (from[], to[]: forward, left, up's places) of the turn that
takes the direction a toward b, about the axis across both, no farther than
most (radians); FALSE: none (either none, or the two along one line) */
static boolean first_person_legs_vr_turn_toward(
	real_vector3d const *a,
	real_vector3d const *b,
	real most,
	real_vector3d from[3],
	real_vector3d to[3])
{
	real_vector3d u = *a, v, w;
	real angle, c, s;

	cross_product3d(a, b, &w);
	if (normalize3d(&u) <= 0.f || magnitude3d(&w) < 1e-6f || normalize3d(&w) <= 0.f)
		return FALSE;
	cross_product3d(&w, &u, &v);
	angle = (real)atan2(dot_product3d(b, &v), dot_product3d(b, &u));
	if (angle > most)
		angle = most;
	c = (real)cos(angle);
	s = (real)sin(angle);
	from[0] = u;
	from[1] = v;
	from[2] = w;
	to[0].i = u.i * c + v.i * s;
	to[0].j = u.j * c + v.j * s;
	to[0].k = u.k * c + v.k * s;
	to[1].i = v.i * c - u.i * s;
	to[1].j = v.j * c - u.j * s;
	to[1].k = v.k * c - u.k * s;
	to[2] = w;
	return TRUE;
}

/* whether a node is the node below, or one of its children's children */
static boolean first_person_legs_below(
	struct model const *model,
	short index,
	short below)
{
	for (; index != NONE; index = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->parent_node_index)
	{
		if (index == below)
			return TRUE;
	}
	return FALSE;
}

/* the pose to draw the body with in a seat in the VR mode (vr_render_body_
seat), the real head's place the neck's (vr_render_body_anchor); NULL: none
(not seated so, a body without a neck, a head and upper arms named). The
game's pose in the seat, its legs the seat's; standing, the legs hang
straight down from the hips and the body turns to the body's heading. All of
it is moved so that the top of its neck is a little behind (FIRST_PERSON_
LEGS_VR_BACK) where the real one is with the head as it was recentred: under
the seat's eye, or its cabin's, whose seat and floor are where its hips and
feet then are (tools/vr_cabin_model.py). Its upper body turns about its hips
toward the real neck as the head moves (no farther than FIRST_PERSON_LEGS_
SEAT_TURN_MOST), and leans back clear of the eyes, as on foot
(first_person_legs_vr_lean); its head is folded into the top of its neck,
and its arms into its shoulders where the VR mode draws its own */
static real_matrix4x3 const *first_person_legs_vr_seated(
	long object_index,
	struct model const *model,
	long count,
	real_matrix4x3 const *nodes)
{
	struct vr_body_seat seat;
	short pelvis = NONE, neck = NONE, head = NONE, left_arm = NONE, right_arm = NONE;
	short index, child, pass;
	real_point3d anchor, target;
	real_vector3d from[3], to[3], move, a, b;
	real yaw;

	if (!vr_render_body_seat(object_index, &seat) || !vr_render_body_anchor(&anchor, &yaw))
		return NULL;
	for (index = 0; index < count; index++)
	{
		char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

		if (pelvis == NONE && first_person_legs_name_has(name, "pelvis"))
			pelvis = index;
		if (neck == NONE && first_person_legs_name_has(name, "neck"))
			neck = index;
		if (head == NONE && first_person_legs_name_has(name, "head"))
			head = index;
		if (left_arm == NONE && first_person_legs_name_has(name, "l upperarm"))
			left_arm = index;
		if (right_arm == NONE && first_person_legs_name_has(name, "r upperarm"))
			right_arm = index;
	}
	if (pelvis == NONE || neck == NONE || head == NONE || left_arm == NONE || right_arm == NONE)
		return NULL;
	for (index = 0; index < count; index++)
		first_person_legs_matrices[index] = nodes[index];
	move = *global_zero_vector3d;

	if (seat.standing)
	{
		/* (each thigh, then each calf, turned about its joint so that the
		leg below it hangs straight down, all below it with it) */
		for (pass = 0; pass < 2; pass++)
		{
			for (index = 0; index < count; index++)
			{
				char const *name = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node)->name;

				if (!first_person_legs_name_has(name, pass == 0 ? "thigh" : "calf"))
					continue;
				for (child = 0; child < count; child++)
				{
					if (TAG_BLOCK_GET_ELEMENT(&model->nodes, child, struct model_node)->parent_node_index == index)
						break;
				}
				if (child >= count)
					continue;
				vector_from_points3d(&first_person_legs_matrices[index].position, &first_person_legs_matrices[child].position,
					&a);
				b.i = 0.f;
				b.j = 0.f;
				b.k = -1.f;
				if (first_person_legs_vr_turn_toward(&a, &b, 3.2f, from, to))
				{
					real_point3d pivot = first_person_legs_matrices[index].position;
					short below;

					for (below = 0; below < count; below++)
					{
						if (first_person_legs_below(model, below, index))
							first_person_legs_rotate(&first_person_legs_matrices[below], from, to, &pivot, &move);
					}
				}
			}
		}
		/* (and the whole turned about the vertical through its neck, from
		the way the game poses it facing to the body's heading) */
		{
			real_point3d pivot = first_person_legs_matrices[neck].position;

			from[0] = seat.pose_forward;
			from[1].i = -seat.pose_forward.j;
			from[1].j = seat.pose_forward.i;
			from[1].k = 0.f;
			from[2] = *global_up3d;
			to[0] = seat.forward;
			to[1] = seat.left;
			to[2] = *global_up3d;
			for (index = 0; index < count; index++)
				first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &pivot, &move);
		}
	}

	/* (moved so that the top of its neck is a little behind the real one's
	place with the head as it was recentred) */
	target.x = seat.rest_neck.x - seat.forward.i * FIRST_PERSON_LEGS_VR_BACK;
	target.y = seat.rest_neck.y - seat.forward.j * FIRST_PERSON_LEGS_VR_BACK;
	target.z = seat.rest_neck.z;
	vector_from_points3d(&first_person_legs_matrices[neck].position, &target, &move);
	for (index = 0; index < count; index++)
	{
		first_person_legs_matrices[index].position.x += move.i;
		first_person_legs_matrices[index].position.y += move.j;
		first_person_legs_matrices[index].position.z += move.k;
	}
	move = *global_zero_vector3d;

	/* (its upper body turned about its hips toward the real neck, a little
	behind it, as far as it turns) */
	target.x = anchor.x - seat.forward.i * FIRST_PERSON_LEGS_VR_BACK;
	target.y = anchor.y - seat.forward.j * FIRST_PERSON_LEGS_VR_BACK;
	target.z = anchor.z;
	vector_from_points3d(&first_person_legs_matrices[pelvis].position, &first_person_legs_matrices[neck].position, &a);
	vector_from_points3d(&first_person_legs_matrices[pelvis].position, &target, &b);
	if (first_person_legs_vr_turn_toward(&a, &b, FIRST_PERSON_LEGS_SEAT_TURN_MOST, from, to))
	{
		real_point3d pivot = first_person_legs_matrices[pelvis].position;

		for (index = 0; index < count; index++)
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

			if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
				first_person_legs_rotate(&first_person_legs_matrices[index], from, to, &pivot, &move);
		}
	}

	/* (the head folded into the top of the neck, and the arms into the
	shoulders where the VR mode draws its own, as on foot) */
	for (index = 0; index < count; index++)
	{
		short up;

		for (up = index; up != NONE && up != head && (!seat.arms || (up != left_arm && up != right_arm));
			up = TAG_BLOCK_GET_ELEMENT(&model->nodes, up, struct model_node)->parent_node_index);
		if (up != NONE)
		{
			first_person_legs_matrices[index].position = first_person_legs_matrices[up].position;
			first_person_legs_matrices[index].scale = 0.001f;
		}
	}

	/* (and its upper body leant back about its hips, as far as keeps it
	clear of the eyes) */
	first_person_legs_vr_lean(object_index, model, count, &first_person_legs_matrices[pelvis].position,
		&first_person_legs_matrices[head].position, &seat.forward, &seat.left);
	return first_person_legs_matrices;
}
#endif

/* the span of drops (world units) over which a point lowered is in the part
of the view the near plane cuts away: within the view's sides and less than
FIRST_PERSON_LEGS_UNCUT beyond its near plane (view: the frustum as drawn,
its planes' distances outward; along: the way it is lowered, a drop of 1
moving it along this, 1 down and maybe aside). That part is convex (between
the eye and the near plane, within the sides), so the line the point moves
along crosses it over one span, found from each plane's distance; FALSE:
none */
static boolean first_person_legs_point_span(
	struct render_frustum const *view,
	real_vector3d const *along,
	real_point3d const *point,
	real span[2])
{
	real low = -FIRST_PERSON_LEGS_DROP_MAXIMUM - 1.f;
	real high = FIRST_PERSON_LEGS_DROP_MAXIMUM + 1.f;
	short plane;

	/* (lowered by d, a plane's distance outward is its distance less d
	times how far a drop of 1 takes it inward, up. In the cut: within each
	side, its distance less than none; nearer than the near plane and the
	margin, the near plane's (outward toward the eye) more than less the
	margin. Each a bound on d, below or above, by the way its normal leans) */
	for (plane = 0; plane < 5 && low < high; plane++)
	{
		real_plane3d const *p = &view->world_planes[plane];
		real distance = plane3d_distance_to_point(p, point);
		real up = -dot_product3d(&p->n, along);
		real margin = 0.f;

		if (plane == 4)
		{
			/* (-(distance - d * up) < margin) */
			distance = -distance;
			up = -up;
			margin = FIRST_PERSON_LEGS_UNCUT;
		}
		/* (distance - d * up < margin) */
		if (up > 1.0e-6f)
			low = MAX(low, (distance - margin) / up);
		else if (up < -1.0e-6f)
			high = MIN(high, (distance - margin) / up);
		else if (distance >= margin)
			high = low;
	}
	span[0] = low;
	span[1] = high;
	return low < high;
}

/* the corners of the part of the view the near plane cuts away: the eye,
where the sides meet, and where each two neighbouring sides (left or right,
bottom or top) meet the near plane moved FIRST_PERSON_LEGS_UNCUT toward the
eye's far side, in turn about it (left bottom, right bottom, right top,
left top); FALSE: the planes meet in no point */
static boolean first_person_legs_cut_corners(
	struct render_frustum const *view,
	real_point3d corners[5])
{
	static short const planes[5][3] = { { 0, 1, 2 }, { 0, 2, 4 }, { 1, 2, 4 }, { 1, 3, 4 }, { 0, 3, 4 } };
	short corner;

	for (corner = 0; corner < 5; corner++)
	{
		real_plane3d const *a = &view->world_planes[planes[corner][0]];
		real_plane3d const *b = &view->world_planes[planes[corner][1]];
		real_plane3d const *c = &view->world_planes[planes[corner][2]];
		/* (the near plane where its distance is less the margin) */
		real cd = planes[corner][2] == 4 ? c->d - FIRST_PERSON_LEGS_UNCUT : c->d;
		real_vector3d bc, ca, ab;
		real determinant;

		cross_product3d(&b->n, &c->n, &bc);
		cross_product3d(&c->n, &a->n, &ca);
		cross_product3d(&a->n, &b->n, &ab);
		determinant = dot_product3d(&a->n, &bc);
		if (determinant > -1.0e-6f && determinant < 1.0e-6f)
			return FALSE;
		corners[corner].x = (a->d * bc.i + b->d * ca.i + cd * ab.i) / determinant;
		corners[corner].y = (a->d * bc.j + b->d * ca.j + cd * ab.j) / determinant;
		corners[corner].z = (a->d * bc.k + b->d * ca.k + cd * ab.k) / determinant;
	}
	return TRUE;
}

/* the span of drops over which a triangle lowered along this meets the cut
(corners: first_person_legs_cut_corners'; spans: each of the triangle's
corners' first_person_legs_point_span, held: whether it has one). Both
convex, they meet over one span, from where they first touch to where they
last do: a corner of the one on a face of the other, or an edge of each on
an edge of the other. FALSE: they never meet */
static boolean first_person_legs_triangle_span(
	real_vector3d const *along,
	real_point3d const *corners,
	real_point3d const *const triangle[3],
	real const *const spans[3],
	boolean const held[3],
	real span[2])
{
	static short const edges[8][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 1 } };
	real low = FIRST_PERSON_LEGS_DROP_MAXIMUM + 1.f;
	real high = -FIRST_PERSON_LEGS_DROP_MAXIMUM - 1.f;
	real_vector3d sides[3];
	real_vector3d normal;
	real toward;
	short index;
	short other;

	/* (a corner of the triangle on a face of the cut: its own span's ends) */
	for (index = 0; index < 3; index++)
	{
		if (held[index])
		{
			low = MIN(low, spans[index][0]);
			high = MAX(high, spans[index][1]);
		}
		sides[index].i = triangle[(index + 1) % 3]->x - triangle[index]->x;
		sides[index].j = triangle[(index + 1) % 3]->y - triangle[index]->y;
		sides[index].k = triangle[(index + 1) % 3]->z - triangle[index]->z;
	}
	/* (a corner of the cut on the triangle: the triangle lowered to it, d
	where the corner is on its plane, the corner within each of its edges) */
	cross_product3d(&sides[0], &sides[1], &normal);
	toward = dot_product3d(&normal, along);
	if (toward > 1.0e-9f || toward < -1.0e-9f)
	{
		for (index = 0; index < 5; index++)
		{
			real_vector3d to = { corners[index].x - triangle[0]->x, corners[index].y - triangle[0]->y,
				corners[index].z - triangle[0]->z };
			real d = dot_product3d(&normal, &to) / toward;
			boolean within = TRUE;

			for (other = 0; other < 3 && within; other++)
			{
				real_vector3d from = { corners[index].x - d * along->i - triangle[other]->x,
					corners[index].y - d * along->j - triangle[other]->y,
					corners[index].z - d * along->k - triangle[other]->z };
				real_vector3d turn;

				within = dot_product3d(cross_product3d(&sides[other], &from, &turn), &normal) >= 0.f;
			}
			if (within)
			{
				low = MIN(low, d);
				high = MAX(high, d);
			}
		}
	}
	/* (an edge of the triangle on an edge of the cut: a + u (b - a) + d along
	= s + w (t - s), u and w within 0 and 1) */
	for (index = 0; index < 3; index++)
	{
		for (other = 0; other < 8; other++)
		{
			real_point3d const *s = &corners[edges[other][0]];
			real_point3d const *t = &corners[edges[other][1]];
			real_vector3d e = sides[index];
			real_vector3d f = { s->x - t->x, s->y - t->y, s->z - t->z };
			real_vector3d to = { s->x - triangle[index]->x, s->y - triangle[index]->y, s->z - triangle[index]->z };
			real_vector3d fa, ae, ef;
			real determinant;
			real u, w;

			/* (u e + w f + d along = to, by Cramer's rule) */
			cross_product3d(&f, along, &fa);
			determinant = dot_product3d(&e, &fa);
			if (determinant > -1.0e-9f && determinant < 1.0e-9f)
				continue;
			cross_product3d(along, &e, &ae);
			cross_product3d(&e, &f, &ef);
			u = dot_product3d(&to, &fa) / determinant;
			w = dot_product3d(&to, &ae) / determinant;
			if (u >= 0.f && u <= 1.f && w >= 0.f && w <= 1.f)
			{
				real d = dot_product3d(&to, &ef) / determinant;

				low = MIN(low, d);
				high = MAX(high, d);
			}
		}
	}
	span[0] = low;
	span[1] = high;
	return low < high;
}

/* how far to lower the legs (along this: a drop of 1 moves them 1 down, and
maybe aside) so that none of their surface is in the part of the view the
near plane cuts away: the least drop (world units, 0 or more) at which each
of its triangles (each three of points' indices, triangles of them; NULL:
each point on its own) is outside the view's sides or
FIRST_PERSON_LEGS_UNCUT or more beyond its near plane. Lowered, each meets
that part over one span of drops (first_person_legs_triangle_span); the
least drop in none of the spans is the answer. Weighing the points alone
missed what is between them: the legs' plates are broad triangles, and
crouching in the air a thigh's reached from below the view's bottom, beside
the eye, to beyond the near plane, its corners out of the cut and its
middle in it (#692). cut holds a span for each point and triangle,
point_spans one for each point */
static real first_person_legs_drop(
	struct render_frustum const *view,
	real_vector3d const *along,
	real_point3d const *points,
	long count,
	unsigned short const (*triangles)[3],
	long triangle_count,
	real (*point_spans)[2],
	real (*cut)[2])
{
	long spans = 0;
	long index;
	real drop = 0.f;
	boolean moved;
	real_point3d corners[5];
	real_point3d box[2];

	for (index = 0; index < count; index++)
	{
		if (first_person_legs_point_span(view, along, &points[index], point_spans[index]) &&
			point_spans[index][1] > 0.f && !triangles)
		{
			cut[spans][0] = point_spans[index][0];
			cut[spans][1] = point_spans[index][1];
			spans++;
		}
	}
	/* (the triangles that can meet the cut lowered from none to the most:
	those within its bounds and theirs moved back up by the most) */
	if (triangles && first_person_legs_cut_corners(view, corners))
	{
		box[0] = box[1] = corners[0];
		for (index = 0; index < 10; index++)
		{
			real_point3d corner = corners[index % 5];

			if (index >= 5)
			{
				corner.x -= along->i * FIRST_PERSON_LEGS_DROP_MAXIMUM;
				corner.y -= along->j * FIRST_PERSON_LEGS_DROP_MAXIMUM;
				corner.z -= along->k * FIRST_PERSON_LEGS_DROP_MAXIMUM;
			}
			box[0].x = MIN(box[0].x, corner.x);
			box[0].y = MIN(box[0].y, corner.y);
			box[0].z = MIN(box[0].z, corner.z);
			box[1].x = MAX(box[1].x, corner.x);
			box[1].y = MAX(box[1].y, corner.y);
			box[1].z = MAX(box[1].z, corner.z);
		}
		for (index = 0; index < triangle_count; index++)
		{
			real_point3d const *triangle[3];
			real const *corner_spans[3];
			boolean held[3];
			short corner;

			if (triangles[index][0] >= count || triangles[index][1] >= count || triangles[index][2] >= count)
				continue;
			for (corner = 0; corner < 3; corner++)
			{
				triangle[corner] = &points[triangles[index][corner]];
				corner_spans[corner] = point_spans[triangles[index][corner]];
				held[corner] = corner_spans[corner][0] < corner_spans[corner][1];
			}
			if (MAX(MAX(triangle[0]->x, triangle[1]->x), triangle[2]->x) < box[0].x ||
				MIN(MIN(triangle[0]->x, triangle[1]->x), triangle[2]->x) > box[1].x ||
				MAX(MAX(triangle[0]->y, triangle[1]->y), triangle[2]->y) < box[0].y ||
				MIN(MIN(triangle[0]->y, triangle[1]->y), triangle[2]->y) > box[1].y ||
				MAX(MAX(triangle[0]->z, triangle[1]->z), triangle[2]->z) < box[0].z ||
				MIN(MIN(triangle[0]->z, triangle[1]->z), triangle[2]->z) > box[1].z)
			{
				continue;
			}
			if (first_person_legs_triangle_span(along, corners, triangle, corner_spans, held, cut[spans]) &&
				cut[spans][1] > 0.f)
			{
				spans++;
			}
		}
	}
	/* (the least drop past every span it falls in, each span left behind
	for good: the drop only grows) */
	do
	{
		moved = FALSE;
		for (index = 0; index < spans; index++)
		{
			if (cut[index][0] <= drop && drop < cut[index][1])
			{
				drop = cut[index][1];
				moved = TRUE;
			}
		}
	}
	while (moved && drop < FIRST_PERSON_LEGS_DROP_MAXIMUM);
	return MIN(drop, FIRST_PERSON_LEGS_DROP_MAXIMUM);
}

/* the points of the body's surface as drawn with a pose (each node's matrix,
count of them): its mesh's vertices in the model's space, each node's from
it (the mesh's inverse) then posed, blended by its two nodes' weights as the
game's skinning blends them, each at its vertex's index (the mesh's
triangles' indices); how many (none: no mesh, or a vertex bound to a node
the pose has not) */
static real_point3d first_person_legs_points[OBJECT_MESH_MAXIMUM_VERTICES];
static real first_person_legs_point_spans[OBJECT_MESH_MAXIMUM_VERTICES][2];
static real first_person_legs_spans[MAX(OBJECT_MESH_MAXIMUM_VERTICES, OBJECT_MESH_MAXIMUM_TRIANGLES)][2];

static long first_person_legs_surface(
	struct object_mesh const *mesh,
	real_matrix4x3 const *pose,
	long count)
{
	real_matrix4x3 skin[FIRST_PERSON_LEGS_MAXIMUM_NODES];
	long nodes = MIN(count, (long)mesh->node_count);
	long index;

	if (mesh->vertex_count > OBJECT_MESH_MAXIMUM_VERTICES)
		return 0;
	for (index = 0; index < nodes; index++)
		matrix4x3_multiply(&pose[index], &mesh->inverse[index], &skin[index]);
	for (index = 0; index < mesh->vertex_count; index++)
	{
		struct object_mesh_vertex const *vertex = &mesh->vertices[index];
		real weight = (real)vertex->weight / 32767.f;
		real_point3d first;
		real_point3d second;
		real_point3d *point = &first_person_legs_points[index];

		if (vertex->nodes[0] >= nodes || (weight < 1.f && vertex->nodes[1] >= nodes))
			return 0;
		matrix4x3_transform_point(&skin[vertex->nodes[0]], &vertex->position, &first);
		if (weight >= 1.f)
		{
			*point = first;
		}
		else
		{
			matrix4x3_transform_point(&skin[vertex->nodes[1]], &vertex->position, &second);
			point->x = first.x * weight + second.x * (1.f - weight);
			point->y = first.y * weight + second.y * (1.f - weight);
			point->z = first.z * weight + second.z * (1.f - weight);
		}
	}
	return mesh->vertex_count;
}

/* the pose to draw the legs with: the body's own (as it is drawn now, about
the view drawn as it is about its own: render_interpolation_view_body), all
but the legs folded into the hips, the whole set back behind the eye and
lowered out of the near plane's cut (first_person_legs_drop); NULL: none (a
body without named legs) */
real_matrix4x3 const *first_person_legs_node_matrices(
	long object_index,
	real_matrix4x3 const *nodes)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_definition *definition;
	struct model const *model;
	long count;
	short pelvis = NONE;
	short thighs = 0;
	short index;
	real_point3d fold;
	real_vector3d back;
	real_matrix4x3 skin;
	real set_back;

	if (!object || !nodes)
		return NULL;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.model.index == NONE)
		return NULL;
	model = model_definition_get(definition->object.model.index);
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (!model || count <= 0 || count > FIRST_PERSON_LEGS_MAXIMUM_NODES || model->nodes.count < count)
		return NULL;

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		if (pelvis == NONE && first_person_legs_name_has(node->name, "pelvis"))
			pelvis = index;
		if (first_person_legs_name_has(node->name, "thigh"))
			thighs++;
	}
	if (pelvis == NONE || thighs == 0)
		return NULL;
#ifdef HALO_VR
	/* (in a seat: the VR mode's seated body, or none) */
	if (object->object.parent_object_index != NONE)
		return first_person_legs_vr_seated(object_index, model, count, nodes);
	{
		real_matrix4x3 const *body = first_person_legs_vr_body(object_index, object, model, count, nodes);

		if (body)
			return body;
	}
#endif
	/* (the body as it is about its own view, between the ticks, about the
	view drawn: that is the newest step's, a tick's motion on, aimed in the
	frame; the legs trailed it, sliding and stepping against it, #697) */
	{
		real_matrix4x3 move;

		if (render_interpolation_view_body(object_index, &move))
		{
			for (index = 0; index < count; index++)
				matrix4x3_multiply(&move, &nodes[index], &first_person_legs_seen[index]);
			nodes = first_person_legs_seen;
		}
	}
	/* (the waist's middle as the hips are posed now) */
	{
		struct object_mesh const *mesh = object_mesh_get(object_index);
		real_point3d middle;

		if (!mesh || pelvis >= mesh->node_count || !first_person_legs_waist(mesh, model, pelvis, &middle))
			return NULL;
		matrix4x3_multiply(&nodes[pelvis], &mesh->inverse[pelvis], &skin);
		matrix4x3_transform_point(&skin, &middle, &fold);
	}

	/* (set back away from the way the eye looks, level) */
	back.i = -global_window_parameters.camera.forward.i;
	back.j = -global_window_parameters.camera.forward.j;
	back.k = 0.f;
	if (normalize3d(&back) <= 0.f)
		back = *global_zero_vector3d;

	/* (as far back as keeps the waist's rim out of the bottom of the view,
	as Halo 2 keeps its first person body's open top out of it: looking
	ahead, the least; looking down, the legs stand behind the eye, the
	thighs, knees and feet seen as they swing forward) */
	set_back = FIRST_PERSON_LEGS_SET_BACK;
	{
		real_plane3d const *bottom = &global_window_parameters.frustum.world_planes[2];
		real toward = dot_product3d(&bottom->n, &back);
		short corner;

		if (toward > 0.05f)
		{
			for (corner = 0; corner < first_person_legs_rim_count; corner++)
			{
				real_point3d p;
				real distance;

				matrix4x3_transform_point(&skin, &first_person_legs_rim[corner], &p);
				distance = plane3d_distance_to_point(bottom, &p) + toward * FIRST_PERSON_LEGS_SET_BACK;
				if (distance < FIRST_PERSON_LEGS_OUT_OF_VIEW)
				{
					real needed = FIRST_PERSON_LEGS_SET_BACK + (FIRST_PERSON_LEGS_OUT_OF_VIEW - distance) / toward;

					if (needed > set_back)
						set_back = needed;
				}
			}
		}
		if (set_back > FIRST_PERSON_LEGS_SET_BACK_MAXIMUM)
			set_back = FIRST_PERSON_LEGS_SET_BACK_MAXIMUM;
	}

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 *matrix = &first_person_legs_matrices[index];

		*matrix = nodes[index];
		/* (the root kept: it is the body's frame, not a part of it) */
		if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
		{
			matrix->position = fold;
			matrix->scale = 0.001f;
		}
		matrix->position.x += back.i * set_back;
		matrix->position.y += back.j * set_back;
		matrix->position.z += back.k * set_back;
	}

	/* (lowered as far as keeps all of it out of the near plane's cut: the
	hips and thighs come up to the eye as it crouches in the air. Straight
	down, while the view's bottom faces down: the waist's rim only goes
	farther below it. Looking down by more than the view's lower half's
	angle from level, the bottom faces up a little, and straight down
	brought the rim up into the view, the open waist seen from above (#692):
	then down along the bottom, the rim as far below it as it was set) */
	{
		struct object_mesh const *mesh = object_mesh_get(object_index);
		long points = mesh ? first_person_legs_surface(mesh, first_person_legs_matrices, count) : 0;
		real_vector3d const *bottom = &global_window_parameters.frustum.world_planes[2].n;
		real_vector3d along = { 0.f, 0.f, -1.f };
		real drop;

		if (bottom->k > 0.f && bottom->k < 1.f)
		{
			/* (down less its part across the bottom, scaled to 1 down) */
			real scale = 1.f / (1.f - bottom->k * bottom->k);

			along.i = bottom->k * bottom->i * scale;
			along.j = bottom->k * bottom->j * scale;
			along.k = -1.f;
		}
		drop = first_person_legs_drop(&global_window_parameters.frustum, &along, first_person_legs_points, points,
			mesh ? mesh->triangles : NULL, mesh ? mesh->triangle_count : 0, first_person_legs_point_spans,
			first_person_legs_spans);
		if (drop > 0.f)
		{
			for (index = 0; index < count; index++)
			{
				first_person_legs_matrices[index].position.x += along.i * drop;
				first_person_legs_matrices[index].position.y += along.j * drop;
				first_person_legs_matrices[index].position.z -= drop;
			}
		}
	}
	return first_person_legs_matrices;
}
