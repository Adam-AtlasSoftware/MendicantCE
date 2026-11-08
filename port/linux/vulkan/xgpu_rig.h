/*
XGPU_RIG.H

PRISM's extended skeletons (display.prism_models, docs/prism_rigs.md): a
replacement mesh of the host's mesh store may be skinned to more bones than
the model it replaces has nodes. Its skeleton is the CE model's nodes (the
same names, hierarchy and bind pose, in the same order) followed by helper
bones: a forearm's twist, a deltoid, a knee's volume, a pouch, a mandible.
The game never sees a helper. Each frame the guest sends the CE nodes'
matrices as it skins with them (node matrix times the node's default
inverse, rasterizer_set_model_skinning's), and the host works out every
helper's transform from them by the rig's rules:

- copy_rotation: a fraction of a bone's rotation away from its bind pose, on
  some of its axes, in its own frame or its parent's;
- twist: a fraction of a child's twist about one of its axes relative to
  its parent (the swing and twist split of the child's local rotation),
  about one of the helper's own axes;
- aim: the helper's axis turned towards a bone or the camera by the shortest
  arc (Blender's damped track); look_at: the same with an up axis kept
  (Blender's track to);
- driven: a translation, rotation and scale along a curve of a bone's
  angle, an object function's value or the distance of two bones;
- spring: a damped spring of the helper's tip towards where its parent
  carries it (damped against the target's own velocity: steady motion
  carries it along, accelerations swing it), with gravity, a limit and an
  optional drive by a function.

Helpers only feed the skin's matrices: gameplay, markers and attachments go
on reading the guest's nodes, and no animation data is added. A rig has up
to XGPU_RIG_BONES_MAXIMUM bones; CE's 64 nodes stay the gameplay's ceiling.

Springs move on the game's clock, not the frame's: in fixed steps of
XGPU_RIG_SPRING_STEP seconds of game time (a tick is four), their targets
interpolated between the frames' samples, the result drawn between the last
two steps. A paused game holds them still, the same input gives the same
output bit for bit (a replay), and frames drawn between ticks (the
interpolation) change only where between two steps a frame is drawn. Their
state is kept per instance (the object's handle and the mesh), reset by a
teleport, a step back in time or an instance unseen for
XGPU_RIG_STATE_FORGET_SECONDS, and dropped when it has not been seen for
that long.

Matrices are 3x4, rows of (x, y, z, translation): the vertex shader's
constants (node_matrix_constants, rasterizer_xbox.c), mapping a point of the
model's bind space to the world. A bone's world matrix maps its own frame
to the world: its skin matrix is world times inverse bind.

Nothing here touches a device, a thread or the game's maths: the mesh store
calls it on the render thread, the tests directly.
*/

#ifndef __HALO_LINUX_XGPU_RIG_H
#define __HALO_LINUX_XGPU_RIG_H

#include <stddef.h>
#include <stdint.h>

#define XGPU_RIG_BONES_MAXIMUM 1024u
#define XGPU_RIG_HELPERS_MAXIMUM 1024u
/* the CE model's nodes (MAXIMUM_NODES_PER_MODEL, model_definitions.h) */
#define XGPU_RIG_CE_NODES_MAXIMUM 64u
#define XGPU_RIG_SPRING_STEP (1.0 / 120.0)
/* steps a frame at most; a longer gap moves the clock on without them */
#define XGPU_RIG_SPRING_STEPS_MAXIMUM 8u
#define XGPU_RIG_STATE_FORGET_SECONDS 0.5
/* a spring whose target jumps further than this between two frames
(world units) starts again where its target is */
#define XGPU_RIG_SPRING_JUMP 2.0f
/* a spring's gravity is a fraction of the game's (world units per second
squared: 9.81 m/s^2, 3.048 m a world unit) */
#define XGPU_RIG_GRAVITY 3.2185f

/* the compiled rig's bytes (a .pmesh's RIG section, or .rig.json compiled
by xgpu_rig_compile_json or tools/prism_rig.py) */
#define XGPU_RIG_MAGIC 0x47495250u /* 'PRIG' */
#define XGPU_RIG_VERSION 1u

enum
{
	XGPU_RIG_BONE_CE_NODE = 1,
	XGPU_RIG_BONE_HELPER = 2
};

/* a .pmesh's SKEL entry (108 bytes): bones 0 to ce_node_count - 1 are the
CE model's nodes in its order; a helper's ce_node is -1 */
struct xgpu_rig_bone
{
	uint32_t name_hash; /* xgpu_rig_name_hash */
	int16_t parent; /* -1 for the root, else an earlier bone */
	uint16_t flags;
	int16_t ce_node;
	uint16_t unused;
	float bind_world[12];
	float inverse_bind[12];
};

enum xgpu_rig_helper_type
{
	XGPU_RIG_FOLLOW, /* carried by its parent (a helper with no rule) */
	XGPU_RIG_COPY_ROTATION,
	XGPU_RIG_TWIST,
	XGPU_RIG_AIM,
	XGPU_RIG_LOOK_AT,
	XGPU_RIG_DRIVEN,
	XGPU_RIG_SPRING,
	XGPU_RIG_HELPER_TYPES
};

enum
{
	/* copy_rotation's space: the source's own bind frame, or its parent's */
	XGPU_RIG_SPACE_LOCAL = 0,
	XGPU_RIG_SPACE_PARENT = 1
};

enum
{
	XGPU_RIG_DRIVER_ANGLE = 0, /* the source's local rotation about axis, radians */
	XGPU_RIG_DRIVER_FUNCTION = 1, /* an object function's value, 0 to 1: function 0-3 (A-D) */
	XGPU_RIG_DRIVER_DISTANCE = 2 /* from source to target, world units */
};

enum
{
	XGPU_RIG_CURVE_LINEAR = 0,
	XGPU_RIG_CURVE_SMOOTHSTEP = 1
};

/* target of an aim or look_at: the camera rather than a bone */
#define XGPU_RIG_TARGET_CAMERA (-2)

/* one rule (84 bytes), helpers in the rig's order (not necessarily the
order they are worked out in) */
struct xgpu_rig_helper
{
	uint16_t bone;
	uint8_t type; /* xgpu_rig_helper_type */
	uint8_t flags; /* unused, 0 */
	int16_t source; /* copy_rotation, twist: whose rotation; aim, look_at: none (-1); driven: the driver's bone */
	int16_t target; /* twist: the child; aim, look_at: a bone or XGPU_RIG_TARGET_CAMERA; driven: distance's second bone */
	uint8_t axis; /* 0 x, 1 y, 2 z: twist's axis (the child's), aim's pointing axis, driven angle's axis, spring's along the bone */
	uint8_t axes; /* copy_rotation: bit 0 x, bit 1 y, bit 2 z; look_at: the up axis; twist: the helper's own axis it turns about */
	uint8_t space; /* copy_rotation: XGPU_RIG_SPACE_* */
	uint8_t curve; /* driven: XGPU_RIG_CURVE_* */
	uint8_t driver; /* driven: XGPU_RIG_DRIVER_* */
	uint8_t function; /* driven by a function, a spring's drive: 0-3 (A-D), 0xff none */
	uint16_t unused;
	float fraction; /* copy_rotation, twist, aim, look_at: 0 to 1 */
	/* driven: range[2], translate[3], rotate[3] (radians, x then y then z),
	   scale[3]; at the range's end (the curve's 1)
	   spring: stiffness (1/s^2), damping (1/s), gravity (a fraction of
	   XGPU_RIG_GRAVITY), limit (radians, 0 none), length (world units, the
	   tip's distance along axis), drive[3] (radians, times the function)
	   aim, look_at: limit (radians, 0 none) */
	float values[16];
};

struct xgpu_rig_header
{
	uint32_t magic;
	uint16_t version, helper_count;
	uint32_t bone_count; /* the skeleton's it was compiled for */
	uint32_t ce_node_count;
};

enum xgpu_rig_status
{
	XGPU_RIG_OK,
	XGPU_RIG_FORMAT, /* the bytes, a rule's fields, a cycle */
	XGPU_RIG_SKELETON, /* the bones: count, parents, flags, CE nodes */
	XGPU_RIG_UNKNOWN_BONE, /* a name the skeleton has not */
	XGPU_RIG_NODES /* the instance's CE nodes fewer than the rig's */
};

struct xgpu_rig;
struct xgpu_rig_states;

/* FNV-1a over the name's bytes, ASCII letters lower case */
uint32_t xgpu_rig_name_hash(const char *name, size_t length);

/* the rig of a skeleton (copied) and its compiled rules (rig, or NULL and
0 for helpers that only follow); NULL with *status and a message if they do
not agree */
struct xgpu_rig *xgpu_rig_create(const struct xgpu_rig_bone *bones, uint32_t bone_count, uint32_t ce_node_count,
	const void *rig, size_t rig_size, enum xgpu_rig_status *status, char *error, size_t error_size);
void xgpu_rig_destroy(struct xgpu_rig *rig);
uint32_t xgpu_rig_bone_count(const struct xgpu_rig *rig);
uint32_t xgpu_rig_ce_node_count(const struct xgpu_rig *rig);
uint32_t xgpu_rig_spring_count(const struct xgpu_rig *rig);

/* .rig.json (version 1, docs/prism_rigs.md) compiled against a skeleton,
bones named through their hashes: the bytes xgpu_rig_create takes, in
*output (free()) and *output_size; XGPU_RIG_OK or why not */
enum xgpu_rig_status xgpu_rig_compile_json(const struct xgpu_rig_bone *bones, uint32_t bone_count,
	uint32_t ce_node_count, const char *json, size_t length, void **output, size_t *output_size,
	char *error, size_t error_size);

enum
{
	XGPU_RIG_INSTANCE_TELEPORT = 1, /* springs start again (the object was made, or jumped) */
	XGPU_RIG_INSTANCE_NO_SECONDARY = 2 /* display.prism_secondary_motion off: springs and driven helpers follow */
};

struct xgpu_rig_instance
{
	uint64_t key; /* the springs' state: the object's handle and the mesh; 0 none kept */
	double game_seconds; /* the game's clock, ticks / 30 plus the interpolation's fraction */
	const float (*ce_matrices)[12]; /* the CE nodes' skin matrices */
	uint32_t ce_matrix_count;
	uint32_t flags;
	float functions[4]; /* the object's function values A-D */
	float camera[3]; /* the camera's position, for aims at it */
};

/* the skin matrix of each bone (bone_count entries, model bind space to
world: the compute skin's palette) and, if world is not NULL, each bone's
world matrix; states may be NULL (springs follow, as at a teleport). Nothing
is allocated (but a new instance's springs' state); a rig of more than 64
bones uses its own scratch memory, so it is evaluated by one thread at a
time. XGPU_RIG_NODES: fewer CE matrices than the rig's nodes */
enum xgpu_rig_status xgpu_rig_evaluate(const struct xgpu_rig *rig, struct xgpu_rig_states *states,
	const struct xgpu_rig_instance *instance, float (*palette)[12], float (*world)[12]);

/* the springs' state of every instance */
struct xgpu_rig_states *xgpu_rig_states_create(void);
void xgpu_rig_states_destroy(struct xgpu_rig_states *states);
/* drops the state of instances unseen for XGPU_RIG_STATE_FORGET_SECONDS of
game time (or seen in the future: the clock went back); once a frame */
void xgpu_rig_states_age(struct xgpu_rig_states *states, double game_seconds);
uint32_t xgpu_rig_states_count(const struct xgpu_rig_states *states);
void xgpu_rig_states_clear(struct xgpu_rig_states *states);

/* the compute skin's arithmetic on the CPU, the tests' reference: up to
four joints a vertex, weights out of 255 */
void xgpu_rig_skin_reference(const float (*palette)[12], uint32_t bone_count, const float (*positions)[3],
	const float (*normals)[3], const uint16_t (*joints)[4], const uint8_t (*weights)[4], uint32_t vertex_count,
	float (*out_positions)[3], float (*out_normals)[3]);

#endif
