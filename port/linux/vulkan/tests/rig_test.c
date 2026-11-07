/*
RIG_TEST.C

PRISM's extended skeletons (xgpu_rig.c) on made-up skeletons: the
skeleton's and the rules' checks, the CE nodes passed through untouched,
each rule's result against its definition (copy_rotation's fraction and
axes, twist's split, aim's and look_at's directions and limits, driven's
curve, the spring's lag, settling and sag), a rigid move of the whole
model moving every helper with it, the springs on the game's clock (a pause
holds them, the same input gives the same bits, 30, 60 and 144 frames a
second agree, a teleport, a jump, a gap and a step back start them again),
the state's table (ageing, removal in a crowded table), rigs of up to 1024
bones, .rig.json's compiler, the CPU skin, and the time a frame's
evaluations take. Run by tools/test_prism_rig.py; with "compile <skeleton
entries> <ce nodes> <rig.json> <out>" it writes the compiled bytes (for the
comparison with tools/prism_rig.py).
*/

#include "xgpu_rig.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;

#define CHECK(condition) do { if (!(condition)) { failures++; \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define NEAR(a, b, tolerance) CHECK(fabs((double)(a) - (double)(b)) <= (tolerance))

#define PI 3.14159265358979323846

/* ---------- matrices */

static void identity(float m[12])
{
	memset(m, 0, 12 * sizeof(float));
	m[0] = m[5] = m[10] = 1.0f;
}

static void multiply(const float a[12], const float b[12], float out[12])
{
	float r[12];
	int row, column;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 4; column++)
			r[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] + a[row * 4 + 2] * b[8 + column];
		r[row * 4 + 3] += a[row * 4 + 3];
	}
	memcpy(out, r, sizeof(r));
}

/* rigid inverse (orthonormal rotation, any translation) */
static void invert_rigid(const float m[12], float out[12])
{
	float r[12];
	int row;

	for (row = 0; row < 3; row++)
	{
		r[row * 4 + 0] = m[0 * 4 + row];
		r[row * 4 + 1] = m[1 * 4 + row];
		r[row * 4 + 2] = m[2 * 4 + row];
		r[row * 4 + 3] = -(m[0 * 4 + row] * m[3] + m[1 * 4 + row] * m[7] + m[2 * 4 + row] * m[11]);
	}
	memcpy(out, r, sizeof(r));
}

static void rotation(double ax, double ay, double az, double angle, float m[12])
{
	double length = sqrt(ax * ax + ay * ay + az * az), c, s, t;

	ax /= length, ay /= length, az /= length;
	c = cos(angle), s = sin(angle), t = 1.0 - c;
	m[0] = (float)(t * ax * ax + c), m[1] = (float)(t * ax * ay - s * az), m[2] = (float)(t * ax * az + s * ay);
	m[4] = (float)(t * ax * ay + s * az), m[5] = (float)(t * ay * ay + c), m[6] = (float)(t * ay * az - s * ax);
	m[8] = (float)(t * ax * az - s * ay), m[9] = (float)(t * ay * az + s * ax), m[10] = (float)(t * az * az + c);
	m[3] = m[7] = m[11] = 0.0f;
}

static void translation(float x, float y, float z, float m[12])
{
	identity(m);
	m[3] = x, m[7] = y, m[11] = z;
}

static void column(const float m[12], int c, double out[3])
{
	out[0] = m[c], out[1] = m[4 + c], out[2] = m[8 + c];
}

static double angle_between(const double a[3], const double b[3])
{
	double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
	double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);

	return acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c);
}

/* the rotation angle of a's 3x3 relative to b's (a b^T): from the
distance of a b^T to the identity, |R - I| = 2 sqrt(2) sin(angle / 2), which
keeps small angles' precision (acos of the trace does not) */
static double relative_angle(const float a[12], const float b[12])
{
	double sum = 0.0, s;
	int i, j, k;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
		{
			double element = 0.0;

			for (k = 0; k < 3; k++)
				element += (double)a[i * 4 + k] * b[j * 4 + k];
			element -= i == j;
			sum += element * element;
		}
	s = sqrt(sum) / (2.0 * sqrt(2.0));
	return 2.0 * asin(s > 1.0 ? 1.0 : s);
}

static double max_difference(const float a[12], const float b[12])
{
	double worst = 0.0;
	int i;

	for (i = 0; i < 12; i++)
		if (fabs((double)a[i] - b[i]) > worst)
			worst = fabs((double)a[i] - b[i]);
	return worst;
}

/* ---------- a made-up skeleton: an arm on a body, then helpers */

enum
{
	ROOT, UPPERARM, FOREARM, HAND, PELVIS, CE_NODES,
	DELTOID = CE_NODES, TWIST, AIMER, KNEE, POUCH, LOOKER, BONES
};

static const char *bone_names[BONES] = {
	"root", "upperarm", "forearm", "hand", "pelvis", "deltoid", "forearm twist", "aimer", "knee volume", "pouch", "looker"};

static struct xgpu_rig_bone bones[BONES];
/* each CE node's bind pose relative to its parent's */
static float rest_local[BONES][12];

static void set_bone(int index, int parent, const float local[12])
{
	struct xgpu_rig_bone *bone = &bones[index];
	float world[12];

	memset(bone, 0, sizeof(*bone));
	bone->name_hash = xgpu_rig_name_hash(bone_names[index], strlen(bone_names[index]));
	bone->parent = (int16_t)parent;
	bone->flags = index < CE_NODES ? XGPU_RIG_BONE_CE_NODE : XGPU_RIG_BONE_HELPER;
	bone->ce_node = (int16_t)(index < CE_NODES ? index : -1);
	if (parent >= 0)
		multiply(bones[parent].bind_world, local, world);
	else
		memcpy(world, local, sizeof(world));
	memcpy(bone->bind_world, world, sizeof(world));
	invert_rigid(world, bone->inverse_bind);
	memcpy(rest_local[index], local, sizeof(rest_local[index]));
}

/* frames turned (rotated_frames) so that no bone's axes are the model's */
static void make_skeleton(int rotated_frames)
{
	float m[12], r[12];

	translation(0.0f, 0.0f, 1.0f, m);
	set_bone(ROOT, -1, m);
	translation(0.0f, 0.2f, 0.4f, m);
	if (rotated_frames)
	{
		rotation(0.3, 1.0, 0.2, 0.7, r);
		multiply(m, r, m);
	}
	set_bone(UPPERARM, ROOT, m);
	translation(0.3f, 0.0f, 0.0f, m);
	if (rotated_frames)
	{
		rotation(1.0, 0.0, 0.4, -0.5, r);
		multiply(m, r, m);
	}
	set_bone(FOREARM, UPPERARM, m);
	translation(0.28f, 0.0f, 0.0f, m);
	if (rotated_frames)
	{
		rotation(0.0, 0.2, 1.0, 0.4, r);
		multiply(m, r, m);
	}
	set_bone(HAND, FOREARM, m);
	translation(0.0f, 0.0f, -0.1f, m);
	set_bone(PELVIS, ROOT, m);
	/* helpers: the deltoid at the shoulder (carried by the root), the twist
	half-way down the forearm, an aimer on the root, a knee's volume on the
	forearm, a pouch on the pelvis (pointing down), a looker on the root */
	translation(0.0f, 0.2f, 0.4f, m);
	if (rotated_frames)
	{
		rotation(0.3, 1.0, 0.2, 0.7, r);
		multiply(m, r, m);
	}
	set_bone(DELTOID, ROOT, m);
	translation(0.14f, 0.0f, 0.0f, m);
	set_bone(TWIST, FOREARM, m);
	translation(0.0f, 0.0f, 0.5f, m);
	set_bone(AIMER, ROOT, m);
	translation(0.0f, 0.0f, 0.0f, m);
	set_bone(KNEE, FOREARM, m);
	rotation(0.0, 1.0, 0.0, PI / 2.0, m); /* x down */
	m[3] = 0.1f, m[7] = 0.15f, m[11] = 0.0f;
	set_bone(POUCH, PELVIS, m);
	translation(0.0f, -0.1f, 0.5f, m);
	set_bone(LOOKER, ROOT, m);
}

/* a pose: each CE node's local rotation (on top of its rest), the root's
world placement */
struct pose
{
	float root[12];
	float local[CE_NODES][12];
};

static void pose_rest(struct pose *pose)
{
	int index;

	identity(pose->root);
	for (index = 0; index < CE_NODES; index++)
		identity(pose->local[index]);
}

/* the CE skin matrices of a pose (node world times default inverse) and
the nodes' worlds */
static void pose_skin(const struct pose *pose, float skin[CE_NODES][12], float world[CE_NODES][12])
{
	int index;

	for (index = 0; index < CE_NODES; index++)
	{
		float local[12];

		multiply(rest_local[index], pose->local[index], local);
		if (bones[index].parent >= 0)
			multiply(world[bones[index].parent], local, world[index]);
		else
			multiply(pose->root, local, world[index]);
		multiply(world[index], bones[index].inverse_bind, skin[index]);
	}
}

struct rule_list
{
	struct xgpu_rig_header header;
	struct xgpu_rig_helper helpers[16];
};

static struct xgpu_rig_helper rule(int bone, int type)
{
	struct xgpu_rig_helper helper;

	memset(&helper, 0, sizeof(helper));
	helper.bone = (uint16_t)bone;
	helper.type = (uint8_t)type;
	helper.source = helper.target = -1;
	helper.function = 0xff;
	helper.fraction = 1.0f;
	helper.axes = 7;
	helper.values[8] = helper.values[9] = helper.values[10] = 1.0f;
	return helper;
}

static size_t rules_bytes(struct rule_list *list, int count)
{
	list->header.magic = XGPU_RIG_MAGIC;
	list->header.version = XGPU_RIG_VERSION;
	list->header.helper_count = (uint16_t)count;
	list->header.bone_count = BONES;
	list->header.ce_node_count = CE_NODES;
	return sizeof(list->header) + (size_t)count * sizeof(struct xgpu_rig_helper);
}

static struct xgpu_rig *make_rig(struct rule_list *list, int count)
{
	enum xgpu_rig_status status;
	char error[256];
	struct xgpu_rig *rig = xgpu_rig_create(bones, BONES, CE_NODES, list, rules_bytes(list, count), &status, error,
		sizeof(error));

	if (!rig)
		fprintf(stderr, "rig refused: %s\n", error);
	CHECK(rig != NULL);
	if (!rig)
		exit(1);
	return rig;
}

static void evaluate(struct xgpu_rig *rig, struct xgpu_rig_states *states, const struct pose *pose, double seconds,
	uint32_t flags, float palette[BONES][12], float world[BONES][12])
{
	float skin[CE_NODES][12], ce_world[CE_NODES][12];
	struct xgpu_rig_instance instance;

	pose_skin(pose, skin, ce_world);
	memset(&instance, 0, sizeof(instance));
	instance.key = 0x100000001ull;
	instance.game_seconds = seconds;
	instance.ce_matrices = (const float (*)[12])skin;
	instance.ce_matrix_count = CE_NODES;
	instance.flags = flags;
	instance.functions[0] = 0.25f;
	instance.camera[0] = 3.0f, instance.camera[1] = 1.0f, instance.camera[2] = 1.5f;
	CHECK(xgpu_rig_evaluate(rig, states, &instance, palette, world) == XGPU_RIG_OK);
}

/* ---------- checks */

static void test_names(void)
{
	/* FNV-1a 32 of "a" is 0xe40c292c; of "bip01 l forearm" the same as of
	"BIP01 L Forearm" */
	CHECK(xgpu_rig_name_hash("a", 1) == 0xe40c292cu);
	CHECK(xgpu_rig_name_hash("bip01 l forearm", 15) == xgpu_rig_name_hash("BIP01 L Forearm", 15));
	CHECK(xgpu_rig_name_hash("bip01 l forearm", 15) != xgpu_rig_name_hash("bip01 r forearm", 15));
}

static void test_skeleton_checks(void)
{
	struct xgpu_rig_bone saved[BONES];
	enum xgpu_rig_status status;
	char error[256];
	struct xgpu_rig *rig;

	make_skeleton(0);
	memcpy(saved, bones, sizeof(bones));
	rig = xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error));
	CHECK(rig && status == XGPU_RIG_OK && xgpu_rig_bone_count(rig) == BONES && xgpu_rig_spring_count(rig) == 0);
	xgpu_rig_destroy(rig);
	bones[TWIST].parent = LOOKER; /* a later bone */
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	memcpy(bones, saved, sizeof(bones));
	bones[HAND].flags = XGPU_RIG_BONE_HELPER;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	memcpy(bones, saved, sizeof(bones));
	bones[POUCH].ce_node = 3;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	memcpy(bones, saved, sizeof(bones));
	bones[DELTOID].inverse_bind[3] += 0.5f;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	memcpy(bones, saved, sizeof(bones));
	bones[KNEE].bind_world[5] = NAN;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	memcpy(bones, saved, sizeof(bones));
	CHECK(!xgpu_rig_create(bones, BONES, 0, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	CHECK(!xgpu_rig_create(bones, 0, 0, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
}

static void test_rule_checks(void)
{
	struct rule_list list;
	enum xgpu_rig_status status;
	char error[256];
	size_t size;

	make_skeleton(0);
	/* a rule on a CE node */
	list.helpers[0] = rule(HAND, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = FOREARM;
	size = rules_bytes(&list, 1);
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	/* two rules for one bone */
	list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = UPPERARM;
	list.helpers[1] = list.helpers[0];
	size = rules_bytes(&list, 2);
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	/* a cycle: the deltoid copies the aimer, the aimer aims at the deltoid */
	list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = AIMER;
	list.helpers[1] = rule(AIMER, XGPU_RIG_AIM);
	list.helpers[1].target = DELTOID;
	size = rules_bytes(&list, 2);
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	CHECK(strstr(error, "cycle") != NULL);
	/* a stiff spring the steps cannot follow */
	list.helpers[0] = rule(POUCH, XGPU_RIG_SPRING);
	list.helpers[0].values[0] = 40000.0f, list.helpers[0].values[4] = 0.05f;
	size = rules_bytes(&list, 1);
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	/* the size wrong, the version wrong, compiled for another skeleton */
	list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = UPPERARM;
	size = rules_bytes(&list, 1);
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size - 1, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	list.header.version = 2;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
	size = rules_bytes(&list, 1);
	list.header.bone_count = BONES + 1;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	/* a value not finite */
	size = rules_bytes(&list, 1);
	list.helpers[0].values[3] = INFINITY;
	CHECK(!xgpu_rig_create(bones, BONES, CE_NODES, &list, size, &status, error, sizeof(error)) && status == XGPU_RIG_FORMAT);
}

static void test_passthrough_and_follow(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], skin[CE_NODES][12], ce_world[CE_NODES][12], expected[12];
	int index;

	make_skeleton(1);
	rig = make_rig(&list, 0);
	pose_rest(&pose);
	rotation(0.2, 0.3, 1.0, 1.1, pose.root);
	pose.root[3] = 5.0f, pose.root[7] = -2.0f, pose.root[11] = 0.3f;
	rotation(0.0, 1.0, 0.3, 0.8, pose.local[UPPERARM]);
	rotation(0.0, 0.0, 1.0, -0.6, pose.local[FOREARM]);
	evaluate(rig, NULL, &pose, 1.0, 0, palette, world);
	pose_skin(&pose, skin, ce_world);
	/* the CE nodes' matrices are the guest's, bit for bit */
	for (index = 0; index < CE_NODES; index++)
		CHECK(memcmp(palette[index], skin[index], sizeof(skin[index])) == 0);
	/* a follow: carried by its parent, its skin the parent's */
	CHECK(max_difference(palette[TWIST], palette[FOREARM]) < 1e-5);
	CHECK(max_difference(palette[DELTOID], palette[ROOT]) < 1e-5);
	CHECK(max_difference(palette[POUCH], palette[PELVIS]) < 1e-5);
	multiply(world[FOREARM], rest_local[TWIST], expected);
	CHECK(max_difference(world[TWIST], expected) < 1e-5);
	/* too few nodes: refused */
	{
		struct xgpu_rig_instance instance;

		memset(&instance, 0, sizeof(instance));
		instance.ce_matrices = (const float (*)[12])skin;
		instance.ce_matrix_count = CE_NODES - 1;
		CHECK(xgpu_rig_evaluate(rig, NULL, &instance, palette, NULL) == XGPU_RIG_NODES);
	}
	xgpu_rig_destroy(rig);
}

static void test_copy_rotation(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], rest[BONES][12], unused[BONES][12];
	double axis_world[3], before[3], after[3];
	int frames;

	for (frames = 0; frames < 2; frames++)
	{
		make_skeleton(frames);
		list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
		list.helpers[0].source = UPPERARM;
		list.helpers[0].fraction = 0.4f;
		rig = make_rig(&list, 1);
		pose_rest(&pose);
		evaluate(rig, NULL, &pose, 0.0, 0, unused, rest);
		/* the upperarm turned 1 radian about its local y: the deltoid turns
		0.4 radians about the same axis in the world, in place */
		rotation(0.0, 1.0, 0.0, 1.0, pose.local[UPPERARM]);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[DELTOID], rest[DELTOID]), 0.4, 1e-4);
		NEAR(relative_angle(world[UPPERARM], rest[UPPERARM]), 1.0, 1e-4);
		CHECK(fabs(world[DELTOID][3] - rest[DELTOID][3]) < 1e-6 && fabs(world[DELTOID][11] - rest[DELTOID][11]) < 1e-6);
		/* the axis: the upperarm's y at rest, in the world */
		column(rest[UPPERARM], 1, axis_world);
		column(rest[DELTOID], 0, before);
		column(world[DELTOID], 0, after);
		{
			/* the deltoid's x turned about axis_world: its component along
			the axis unchanged */
			double along_before = before[0] * axis_world[0] + before[1] * axis_world[1] + before[2] * axis_world[2];
			double along_after = after[0] * axis_world[0] + after[1] * axis_world[1] + after[2] * axis_world[2];

			NEAR(along_before, along_after, 1e-5);
		}
		xgpu_rig_destroy(rig);
		/* only the z axis copied: a turn about y is not */
		list.helpers[0].axes = 4;
		list.helpers[0].fraction = 1.0f;
		rig = make_rig(&list, 1);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[DELTOID], rest[DELTOID]), 0.0, 1e-4);
		rotation(0.0, 0.0, 1.0, 0.5, pose.local[UPPERARM]);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[DELTOID], rest[DELTOID]), 0.5, 1e-4);
		xgpu_rig_destroy(rig);
		/* in the parent's space: the same turn when the upperarm's bind
		frame is the parent's; with the frames turned, the axes are the
		parent's (z of the root): a turn about the root's z copied whole */
		list.helpers[0].space = XGPU_RIG_SPACE_PARENT;
		list.helpers[0].axes = 4;
		rig = make_rig(&list, 1);
		{
			/* a local rotation that is a turn about the root's z */
			float rz[12], inverse_rest[12], tmp[12];

			rotation(0.0, 0.0, 1.0, 0.3, rz);
			invert_rigid(rest_local[UPPERARM], inverse_rest);
			inverse_rest[3] = inverse_rest[7] = inverse_rest[11] = 0.0f;
			{
				float rest_rotation[12];

				memcpy(rest_rotation, rest_local[UPPERARM], sizeof(rest_rotation));
				rest_rotation[3] = rest_rotation[7] = rest_rotation[11] = 0.0f;
				multiply(inverse_rest, rz, tmp);
				multiply(tmp, rest_rotation, pose.local[UPPERARM]);
			}
		}
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[DELTOID], rest[DELTOID]), 0.3, 1e-4);
		xgpu_rig_destroy(rig);
	}
}

static void test_twist(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], rest[BONES][12], swing[12], twist[12];
	double before[3], after[3];
	int frames;

	for (frames = 0; frames < 2; frames++)
	{
		make_skeleton(frames);
		list.helpers[0] = rule(TWIST, XGPU_RIG_TWIST);
		list.helpers[0].source = FOREARM;
		list.helpers[0].target = HAND;
		list.helpers[0].axis = 0;
		list.helpers[0].axes = 0;
		list.helpers[0].fraction = 0.5f;
		rig = make_rig(&list, 1);
		pose_rest(&pose);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, rest);
		/* the hand twisted 1.2 radians about its x after a swing of 0.6
		about its y: the helper turns 0.6 about its own x, the swing ignored */
		rotation(1.0, 0.0, 0.0, 1.2, twist);
		rotation(0.0, 1.0, 0.0, 0.6, swing);
		multiply(swing, twist, pose.local[HAND]);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[TWIST], rest[TWIST]), 0.6, 1e-4);
		column(rest[TWIST], 0, before);
		column(world[TWIST], 0, after);
		NEAR(angle_between(before, after), 0.0, 1e-4);
		/* past half a turn the other way: -2.8 radians gives -1.4 */
		rotation(1.0, 0.0, 0.0, -2.8, pose.local[HAND]);
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[TWIST], rest[TWIST]), 1.4, 1e-4);
		/* a pure swing: nothing */
		memcpy(pose.local[HAND], swing, sizeof(swing));
		evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
		NEAR(relative_angle(world[TWIST], rest[TWIST]), 0.0, 1e-4);
		xgpu_rig_destroy(rig);
	}
}

static void test_aim_and_look_at(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], rest[BONES][12];
	double head[3], target[3], direction[3], pointing[3], up_before[3], up_after[3];

	make_skeleton(1);
	list.helpers[0] = rule(AIMER, XGPU_RIG_AIM);
	list.helpers[0].target = HAND;
	list.helpers[0].axis = 1;
	list.helpers[1] = rule(LOOKER, XGPU_RIG_LOOK_AT);
	list.helpers[1].target = XGPU_RIG_TARGET_CAMERA;
	list.helpers[1].axis = 0;
	list.helpers[1].axes = 2;
	rig = make_rig(&list, 2);
	pose_rest(&pose);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, rest);
	rotation(0.0, 1.0, 0.0, -0.9, pose.local[UPPERARM]);
	rotation(0.0, 0.0, 1.0, 0.4, pose.root);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	column(world[AIMER], 3, head);
	column(world[HAND], 3, target);
	direction[0] = target[0] - head[0], direction[1] = target[1] - head[1], direction[2] = target[2] - head[2];
	column(world[AIMER], 1, pointing);
	NEAR(angle_between(pointing, direction), 0.0, 1e-4);
	/* look_at: x at the camera, z as near the follow frame's z as can be */
	column(world[LOOKER], 3, head);
	direction[0] = 3.0 - head[0], direction[1] = 1.0 - head[1], direction[2] = 1.5 - head[2];
	column(world[LOOKER], 0, pointing);
	NEAR(angle_between(pointing, direction), 0.0, 1e-4);
	column(world[LOOKER], 2, up_after);
	up_before[0] = 0.0, up_before[1] = 0.0, up_before[2] = 1.0; /* the root's z: turned about z only */
	{
		/* the up axis lies in the plane of the target direction and the
		reference up: perpendicular to their cross product */
		double n[3] = {direction[1] * up_before[2] - direction[2] * up_before[1],
			direction[2] * up_before[0] - direction[0] * up_before[2],
			direction[0] * up_before[1] - direction[1] * up_before[0]};

		NEAR(n[0] * up_after[0] + n[1] * up_after[1] + n[2] * up_after[2], 0.0, 1e-4);
		CHECK(up_after[2] > 0.0);
	}
	/* the matrix stays a rotation (orthonormal, not mirrored) */
	{
		double x[3], y[3], z[3];

		column(world[LOOKER], 0, x), column(world[LOOKER], 1, y), column(world[LOOKER], 2, z);
		NEAR(x[0] * y[0] + x[1] * y[1] + x[2] * y[2], 0.0, 1e-5);
		NEAR(z[0] * (x[1] * y[2] - x[2] * y[1]) + z[1] * (x[2] * y[0] - x[0] * y[2]) + z[2] * (x[0] * y[1] - x[1] * y[0]),
			1.0, 1e-4);
	}
	xgpu_rig_destroy(rig);
	/* a limit of 10 degrees and a fraction of a half */
	list.helpers[0].values[0] = (float)(10.0 * PI / 180.0);
	list.helpers[0].fraction = 0.5f;
	rig = make_rig(&list, 2);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	{
		/* its follow frame is the root's: the aim turns it 5 degrees from it */
		NEAR(relative_angle(world[AIMER], world[ROOT]), 5.0 * PI / 180.0, 1e-4);
	}
	xgpu_rig_destroy(rig);
}

static void test_driven(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], rest[BONES][12];

	make_skeleton(0);
	list.helpers[0] = rule(KNEE, XGPU_RIG_DRIVEN);
	list.helpers[0].driver = XGPU_RIG_DRIVER_ANGLE;
	list.helpers[0].source = HAND;
	list.helpers[0].axis = 2;
	list.helpers[0].curve = XGPU_RIG_CURVE_SMOOTHSTEP;
	list.helpers[0].values[0] = 0.0f, list.helpers[0].values[1] = 1.0f;
	list.helpers[0].values[4] = 0.02f; /* translate z */
	list.helpers[0].values[9] = 1.5f; /* scale y */
	rig = make_rig(&list, 1);
	pose_rest(&pose);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, rest);
	/* the hand bent 0.5 about z: half-way, smoothstep(0.5) = 0.5 */
	rotation(0.0, 0.0, 1.0, 0.5, pose.local[HAND]);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	NEAR(world[KNEE][11] - rest[KNEE][11], 0.01, 1e-5);
	NEAR(world[KNEE][5], 1.25, 1e-5);
	/* bent 0.25: smoothstep(0.25) = 0.15625 */
	rotation(0.0, 0.0, 1.0, 0.25, pose.local[HAND]);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	NEAR(world[KNEE][11] - rest[KNEE][11], 0.02 * 0.15625, 1e-5);
	/* past the range's end: held at 1 */
	rotation(0.0, 0.0, 1.0, 2.0, pose.local[HAND]);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	NEAR(world[KNEE][11] - rest[KNEE][11], 0.02, 1e-5);
	/* secondary motion off: follows */
	evaluate(rig, NULL, &pose, 0.0, XGPU_RIG_INSTANCE_NO_SECONDARY, palette, world);
	NEAR(world[KNEE][11], rest[KNEE][11], 1e-6);
	xgpu_rig_destroy(rig);
	/* by function A (0.25 in evaluate), linear over 0 to 0.5: half */
	list.helpers[0] = rule(KNEE, XGPU_RIG_DRIVEN);
	list.helpers[0].driver = XGPU_RIG_DRIVER_FUNCTION;
	list.helpers[0].function = 0;
	list.helpers[0].values[1] = 0.5f;
	list.helpers[0].values[2] = 0.1f;
	rig = make_rig(&list, 1);
	pose_rest(&pose);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	NEAR(world[KNEE][3] - rest[KNEE][3], 0.05, 1e-6);
	xgpu_rig_destroy(rig);
	/* by distance: hand to pelvis */
	list.helpers[0] = rule(KNEE, XGPU_RIG_DRIVEN);
	list.helpers[0].driver = XGPU_RIG_DRIVER_DISTANCE;
	list.helpers[0].source = HAND;
	list.helpers[0].target = PELVIS;
	list.helpers[0].values[0] = 0.0f, list.helpers[0].values[1] = 2.0f;
	list.helpers[0].values[2] = 1.0f;
	rig = make_rig(&list, 1);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	{
		double d = sqrt(0.58 * 0.58 + 0.2 * 0.2 + 0.5 * 0.5);

		NEAR(world[KNEE][3] - rest[KNEE][3], d / 2.0, 1e-5);
	}
	xgpu_rig_destroy(rig);
}

static struct xgpu_rig *spring_rig(float stiffness, float damping, float gravity, float limit)
{
	struct rule_list list;

	make_skeleton(0);
	list.helpers[0] = rule(POUCH, XGPU_RIG_SPRING);
	list.helpers[0].axis = 0;
	list.helpers[0].values[0] = stiffness;
	list.helpers[0].values[1] = damping;
	list.helpers[0].values[2] = gravity;
	list.helpers[0].values[3] = limit;
	list.helpers[0].values[4] = 0.1f;
	return make_rig(&list, 1);
}

/* the pouch's tip in the world */
static void pouch_tip(float world[BONES][12], double tip[3])
{
	double along[3];

	column(world[POUCH], 0, along);
	tip[0] = world[POUCH][3] + along[0] * 0.1;
	tip[1] = world[POUCH][7] + along[1] * 0.1;
	tip[2] = world[POUCH][11] + along[2] * 0.1;
}

/* the body's sway at time t (world units): smooth, so frames between ticks
see it interpolated */
static void sway(double t, struct pose *pose)
{
	pose_rest(pose);
	pose->root[3] = (float)(0.3 * sin(2.0 * PI * 1.3 * t));
	pose->root[7] = (float)(0.1 * sin(2.0 * PI * 0.7 * t + 0.5));
	rotation(0.0, 0.0, 1.0, 0.4 * sin(2.0 * PI * 0.9 * t), pose->root);
	pose->root[3] = (float)(0.3 * sin(2.0 * PI * 1.3 * t));
	pose->root[7] = (float)(0.1 * sin(2.0 * PI * 0.7 * t + 0.5));
}

static void test_spring_response(void)
{
	struct xgpu_rig *rig = spring_rig(150.0f, 6.0f, 0.0f, 0.0f);
	struct xgpu_rig_states *states = xgpu_rig_states_create();
	struct pose pose;
	float palette[BONES][12], world[BONES][12];
	double tip[3], rest_tip[3], overshoot = 0.0;
	int frame;

	pose_rest(&pose);
	evaluate(rig, states, &pose, 10.0, 0, palette, world);
	pouch_tip(world, rest_tip);
	/* a step of the body 0.2 along y at 30 frames a second: the tip lags
	(still near the old place a tick later), overshoots, then settles */
	pose.root[7] = 0.2f;
	for (frame = 1; frame <= 120; frame++)
	{
		double offset;

		evaluate(rig, states, &pose, 10.0 + frame / 30.0, 0, palette, world);
		pouch_tip(world, tip);
		offset = tip[1] - rest_tip[1];
		if (frame == 1)
			CHECK(offset < 0.2 - 0.003); /* lags a tick after */
		if (offset - 0.2 > overshoot)
			overshoot = offset - 0.2;
	}
	/* the tip moves only on a sphere about the head: compare directions */
	CHECK(overshoot > 0.0005); /* under-damped: past the target */
	NEAR(tip[1] - rest_tip[1], 0.2, 1e-4); /* settled */
	xgpu_rig_destroy(rig);
	xgpu_rig_states_destroy(states);
	/* a body moving steadily (2 world units a second, the run) carries the
	tip along with no lag once it is moving */
	rig = spring_rig(300.0f, 14.0f, 0.0f, 0.0f);
	states = xgpu_rig_states_create();
	for (frame = 0; frame <= 90; frame++)
	{
		float follow[12];

		pose_rest(&pose);
		pose.root[3] = (float)(2.0 * frame / 30.0);
		evaluate(rig, states, &pose, 40.0 + frame / 30.0, 0, palette, world);
		multiply(world[PELVIS], rest_local[POUCH], follow);
		if (frame == 2)
			CHECK(relative_angle(world[POUCH], follow) > 1e-3); /* the start swings it */
		if (frame == 90)
			NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-4);
	}
	xgpu_rig_destroy(rig);
	xgpu_rig_states_destroy(states);
	/* gravity: a pouch pointing along x sags; its angle about the head is
	atan of g / (k length) for small sags (the tip's offset g/k) */
	rig = spring_rig(200.0f, 20.0f, 1.0f, 0.0f);
	states = xgpu_rig_states_create();
	pose_rest(&pose);
	rotation(0.0, 1.0, 0.0, -PI / 2.0, pose.local[PELVIS]); /* the pouch's x level, along the model's x */
	for (frame = 0; frame <= 300; frame++)
		evaluate(rig, states, &pose, 20.0 + frame / 30.0, 0, palette, world);
	{
		double along[3], sag, expected = atan(XGPU_RIG_GRAVITY / 200.0 / 0.1);

		column(world[POUCH], 0, along);
		sag = -asin(along[2]);
		NEAR(sag, expected, 0.02);
	}
	xgpu_rig_destroy(rig);
	xgpu_rig_states_destroy(states);
	/* a limit of 5 degrees holds it however hard it is shaken */
	rig = spring_rig(60.0f, 1.0f, 0.0f, (float)(5.0 * PI / 180.0));
	states = xgpu_rig_states_create();
	{
		double worst = 0.0;

		for (frame = 0; frame <= 200; frame++)
		{
			float follow[12];

			pose_rest(&pose);
			pose.root[3] = (frame & 4) ? 0.5f : -0.5f;
			evaluate(rig, states, &pose, 30.0 + frame / 30.0, 0, palette, world);
			multiply(world[PELVIS], rest_local[POUCH], follow);
			if (relative_angle(world[POUCH], follow) > worst)
				worst = relative_angle(world[POUCH], follow);
		}
		CHECK(worst <= 5.0 * PI / 180.0 + 1e-4);
		CHECK(worst > 4.0 * PI / 180.0);
	}
	xgpu_rig_destroy(rig);
	xgpu_rig_states_destroy(states);
}

static void test_spring_clock(void)
{
	struct xgpu_rig *rig = spring_rig(120.0f, 4.0f, 0.3f, 0.0f);
	struct xgpu_rig_states *a = xgpu_rig_states_create(), *b = xgpu_rig_states_create();
	struct xgpu_rig_states *rates[3];
	static const int per_second[3] = {30, 60, 144};
	struct pose pose;
	float palette[BONES][12], world[BONES][12], world_b[BONES][12], held[BONES][12];
	double tips[3][91][3], worst = 0.0, amplitude = 0.0;
	int seen[3][91];
	int frame, rate;

	/* the same input twice: the same bits */
	for (frame = 0; frame < 200; frame++)
	{
		double t = 1.0 + frame / 30.0;

		sway(t, &pose);
		evaluate(rig, a, &pose, t, 0, palette, world);
		evaluate(rig, b, &pose, t, 0, palette, world_b);
		CHECK(memcmp(world, world_b, sizeof(world)) == 0);
	}
	/* paused: the clock still, the result still, however many frames */
	memcpy(held, world, sizeof(held));
	for (frame = 0; frame < 50; frame++)
	{
		evaluate(rig, a, &pose, 1.0 + 199 / 30.0, 0, palette, world);
		CHECK(memcmp(world, held, sizeof(world)) == 0);
	}
	/* 30, 60 and 144 frames a second (the poses between ticks interpolated,
	as the game draws them) agree at every tick */
	memset(seen, 0, sizeof(seen));
	for (rate = 0; rate < 3; rate++)
	{
		rates[rate] = xgpu_rig_states_create();
		for (frame = 0; frame <= 3 * per_second[rate]; frame++)
		{
			double t = 2.0 + (double)frame / per_second[rate];
			double tick = (t - 2.0) * 30.0;
			int whole = (int)floor(tick + 1e-9);
			double fraction = tick - whole;
			struct pose p0, p1;
			int index;

			/* the game's interpolation: between the two ticks' poses */
			sway(2.0 + whole / 30.0, &p0);
			sway(2.0 + (whole + 1) / 30.0, &p1);
			pose_rest(&pose);
			for (index = 0; index < 12; index++)
				pose.root[index] = (float)(p0.root[index] + (p1.root[index] - p0.root[index]) * fraction);
			evaluate(rig, rates[rate], &pose, t, 0, palette, world);
			/* a frame on a tick (at 144 a second, every fifth tick) */
			if (fabs(fraction) < 1e-6 && whole <= 90)
			{
				pouch_tip(world, tips[rate][whole]);
				seen[rate][whole] = 1;
			}
		}
	}
	for (frame = 10; frame <= 90; frame++)
		for (rate = 1; rate < 3; rate++)
		{
			if (!seen[rate][frame] || !seen[0][frame] || !seen[0][frame - 1])
				continue;
			double d = sqrt((tips[rate][frame][0] - tips[0][frame][0]) * (tips[rate][frame][0] - tips[0][frame][0]) +
				(tips[rate][frame][1] - tips[0][frame][1]) * (tips[rate][frame][1] - tips[0][frame][1]) +
				(tips[rate][frame][2] - tips[0][frame][2]) * (tips[rate][frame][2] - tips[0][frame][2]));
			double moved = sqrt((tips[0][frame][0] - tips[0][frame - 1][0]) * (tips[0][frame][0] - tips[0][frame - 1][0]) +
				(tips[0][frame][1] - tips[0][frame - 1][1]) * (tips[0][frame][1] - tips[0][frame - 1][1]));

			if (d > worst)
				worst = d;
			if (moved > amplitude)
				amplitude = moved;
		}
	printf("spring at 30/60/144 frames a second: ticks differ at most %.6f world units (a tick moves it up to %.4f)\n",
		worst, amplitude);
	CHECK(worst < 0.002); /* 6 mm, against up to ~2.5 cm a tick */
	for (rate = 0; rate < 3; rate++)
		xgpu_rig_states_destroy(rates[rate]);
	xgpu_rig_states_destroy(a);
	xgpu_rig_states_destroy(b);
	xgpu_rig_destroy(rig);
}

static void test_spring_resets(void)
{
	struct xgpu_rig *rig = spring_rig(120.0f, 4.0f, 0.0f, 0.0f);
	struct xgpu_rig_states *states = xgpu_rig_states_create();
	struct pose pose;
	float palette[BONES][12], world[BONES][12], follow[12];
	int frame;

	/* get it swinging */
	for (frame = 0; frame < 30; frame++)
	{
		sway(5.0 + frame / 30.0, &pose);
		evaluate(rig, states, &pose, 5.0 + frame / 30.0, 0, palette, world);
	}
	multiply(world[PELVIS], rest_local[POUCH], follow);
	CHECK(relative_angle(world[POUCH], follow) > 1e-3);
	/* a teleport: where its parent carries it */
	evaluate(rig, states, &pose, 5.0 + 30 / 30.0, XGPU_RIG_INSTANCE_TELEPORT, palette, world);
	multiply(world[PELVIS], rest_local[POUCH], follow);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	/* a jump of the target past XGPU_RIG_SPRING_JUMP: the same */
	for (frame = 31; frame < 40; frame++)
	{
		sway(5.0 + frame / 30.0, &pose);
		evaluate(rig, states, &pose, 5.0 + frame / 30.0, 0, palette, world);
	}
	pose.root[3] += 5.0f;
	evaluate(rig, states, &pose, 5.0 + 40 / 30.0, 0, palette, world);
	multiply(world[PELVIS], rest_local[POUCH], follow);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	/* unseen for over half a second, or the clock back: the same */
	sway(5.0 + 41 / 30.0, &pose);
	evaluate(rig, states, &pose, 5.0 + 41 / 30.0, 0, palette, world);
	sway(7.0, &pose);
	evaluate(rig, states, &pose, 7.0, 0, palette, world);
	multiply(world[PELVIS], rest_local[POUCH], follow);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	sway(7.1, &pose);
	evaluate(rig, states, &pose, 7.1, 0, palette, world);
	sway(6.0, &pose);
	evaluate(rig, states, &pose, 6.0, 0, palette, world);
	multiply(world[PELVIS], rest_local[POUCH], follow);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	/* a gap of 0.4 seconds (under the forgetting): at most 8 steps taken, no
	blow-up, the result finite */
	sway(6.4, &pose);
	evaluate(rig, states, &pose, 6.4, 0, palette, world);
	CHECK(isfinite(world[POUCH][0]) && relative_angle(world[POUCH], world[PELVIS]) < PI);
	/* no state table: follows */
	evaluate(rig, NULL, &pose, 6.5, 0, palette, world);
	multiply(world[PELVIS], rest_local[POUCH], follow);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	/* secondary motion off: follows */
	evaluate(rig, states, &pose, 6.5, XGPU_RIG_INSTANCE_NO_SECONDARY, palette, world);
	NEAR(relative_angle(world[POUCH], follow), 0.0, 1e-5);
	xgpu_rig_states_destroy(states);
	xgpu_rig_destroy(rig);
}

static void test_states_table(void)
{
	struct xgpu_rig *rig = spring_rig(120.0f, 4.0f, 0.0f, 0.0f);
	struct xgpu_rig_states *states = xgpu_rig_states_create();
	float skin[CE_NODES][12], ce_world[CE_NODES][12], palette[BONES][12], world[BONES][12], follow[12];
	struct xgpu_rig_instance instance;
	struct pose pose;
	uint32_t key;
	int frame;

	memset(&instance, 0, sizeof(instance));
	instance.ce_matrices = (const float (*)[12])skin;
	instance.ce_matrix_count = CE_NODES;
	/* 3000 instances, all swinging; then only the even ones go on being
	drawn: after the forgetting the odd ones' state is gone and the even
	ones' kept (they still lag, not reset) */
	for (frame = 0; frame < 20; frame++)
	{
		sway(frame / 30.0, &pose);
		pose_skin(&pose, skin, ce_world);
		instance.game_seconds = frame / 30.0;
		for (key = 1; key <= 3000; key++)
		{
			instance.key = key * 0x9e3779b9ull;
			CHECK(xgpu_rig_evaluate(rig, states, &instance, palette, NULL) == XGPU_RIG_OK);
		}
		xgpu_rig_states_age(states, instance.game_seconds);
	}
	CHECK(xgpu_rig_states_count(states) == 3000);
	for (frame = 20; frame < 40; frame++)
	{
		sway(frame / 30.0, &pose);
		pose_skin(&pose, skin, ce_world);
		instance.game_seconds = frame / 30.0;
		for (key = 2; key <= 3000; key += 2)
		{
			instance.key = key * 0x9e3779b9ull;
			CHECK(xgpu_rig_evaluate(rig, states, &instance, palette, world) == XGPU_RIG_OK);
		}
		xgpu_rig_states_age(states, instance.game_seconds);
	}
	CHECK(xgpu_rig_states_count(states) == 1500);
	/* every even one still swinging (its state found after the removals) */
	sway(40 / 30.0, &pose);
	pose_skin(&pose, skin, ce_world);
	instance.game_seconds = 40 / 30.0;
	{
		int swinging = 0;

		for (key = 2; key <= 3000; key += 2)
		{
			instance.key = key * 0x9e3779b9ull;
			xgpu_rig_evaluate(rig, states, &instance, palette, world);
			multiply(world[PELVIS], rest_local[POUCH], follow);
			swinging += relative_angle(world[POUCH], follow) > 1e-4;
		}
		CHECK(swinging == 1500);
	}
	CHECK(xgpu_rig_states_count(states) == 1500);
	xgpu_rig_states_age(states, 100.0);
	CHECK(xgpu_rig_states_count(states) == 0);
	xgpu_rig_states_destroy(states);
	xgpu_rig_destroy(rig);
}

static void test_rigid_motion(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], moved[BONES][12], unused[BONES][12], g[12], expected[12];
	int index;

	make_skeleton(1);
	list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = UPPERARM;
	list.helpers[0].fraction = 0.35f;
	list.helpers[1] = rule(TWIST, XGPU_RIG_TWIST);
	list.helpers[1].source = FOREARM;
	list.helpers[1].target = HAND;
	list.helpers[1].axes = 0;
	list.helpers[1].fraction = 0.5f;
	list.helpers[2] = rule(AIMER, XGPU_RIG_AIM);
	list.helpers[2].target = HAND;
	list.helpers[3] = rule(KNEE, XGPU_RIG_DRIVEN);
	list.helpers[3].source = HAND;
	list.helpers[3].axis = 2;
	list.helpers[3].values[1] = 1.0f;
	list.helpers[3].values[3] = 0.05f;
	rig = make_rig(&list, 4);
	pose_rest(&pose);
	rotation(0.3, 1.0, 0.0, 0.7, pose.local[UPPERARM]);
	rotation(0.0, 0.0, 1.0, -0.9, pose.local[FOREARM]);
	rotation(1.0, 0.4, 0.1, 1.3, pose.local[HAND]);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	/* the whole model turned and moved: every helper with it */
	rotation(0.2, -0.5, 1.0, 2.1, g);
	g[3] = 40.0f, g[7] = -13.0f, g[11] = 7.5f;
	memcpy(pose.root, g, sizeof(g));
	evaluate(rig, NULL, &pose, 0.0, 0, unused, moved);
	for (index = CE_NODES; index < BONES; index++)
	{
		if (index == LOOKER || index == POUCH)
			continue;
		multiply(g, world[index], expected);
		CHECK(max_difference(moved[index], expected) < 2e-4);
	}
	xgpu_rig_destroy(rig);
}

static void test_mirrored(void)
{
	struct rule_list list;
	struct xgpu_rig *rig;
	struct pose pose;
	float palette[BONES][12], world[BONES][12], rest[BONES][12];
	int index;

	/* a left-hander's VR hands: every node mirrored; the rules still give
	rotations of the same size */
	make_skeleton(0);
	list.helpers[0] = rule(DELTOID, XGPU_RIG_COPY_ROTATION);
	list.helpers[0].source = UPPERARM;
	list.helpers[0].fraction = 0.5f;
	rig = make_rig(&list, 1);
	pose_rest(&pose);
	pose.root[0] = -1.0f;
	evaluate(rig, NULL, &pose, 0.0, 0, palette, rest);
	rotation(0.0, 0.0, 1.0, 0.8, pose.local[UPPERARM]);
	evaluate(rig, NULL, &pose, 0.0, 0, palette, world);
	for (index = 0; index < 12; index++)
		CHECK(isfinite(world[DELTOID][index]));
	NEAR(relative_angle(world[DELTOID], rest[DELTOID]), 0.4, 1e-4);
	xgpu_rig_destroy(rig);
}

static void test_many_bones(void)
{
	static struct xgpu_rig_bone many[XGPU_RIG_BONES_MAXIMUM + 1];
	static struct
	{
		struct xgpu_rig_header header;
		struct xgpu_rig_helper helpers[XGPU_RIG_BONES_MAXIMUM];
	} list;
	static float palette[XGPU_RIG_BONES_MAXIMUM][12], world[XGPU_RIG_BONES_MAXIMUM][12];
	float skin[64][12];
	struct xgpu_rig_instance instance;
	enum xgpu_rig_status status;
	struct xgpu_rig *rig;
	char error[256];
	uint32_t index, count = XGPU_RIG_BONES_MAXIMUM, ce = 64;

	/* 64 CE nodes in a chain, then 960 helpers: a chain of copies, each
	copying a tenth of the one before it */
	for (index = 0; index <= count; index++)
	{
		struct xgpu_rig_bone *bone = &many[index];
		char name[32];

		memset(bone, 0, sizeof(*bone));
		snprintf(name, sizeof(name), "bone %u", index);
		bone->name_hash = xgpu_rig_name_hash(name, strlen(name));
		bone->parent = (int16_t)(index ? (int)index - 1 : -1);
		bone->flags = index < ce ? XGPU_RIG_BONE_CE_NODE : XGPU_RIG_BONE_HELPER;
		bone->ce_node = (int16_t)(index < ce ? (int)index : -1);
		identity(bone->bind_world);
		bone->bind_world[3] = 0.01f * index;
		invert_rigid(bone->bind_world, bone->inverse_bind);
	}
	for (index = ce; index < count; index++)
	{
		list.helpers[index - ce] = rule((int)index, XGPU_RIG_COPY_ROTATION);
		list.helpers[index - ce].source = (int16_t)(index - 1);
		list.helpers[index - ce].fraction = 0.1f;
	}
	list.header.magic = XGPU_RIG_MAGIC;
	list.header.version = XGPU_RIG_VERSION;
	list.header.helper_count = (uint16_t)(count - ce);
	list.header.bone_count = count;
	list.header.ce_node_count = ce;
	rig = xgpu_rig_create(many, count, ce, &list, sizeof(list.header) + (count - ce) * sizeof(list.helpers[0]), &status,
		error, sizeof(error));
	CHECK(rig != NULL);
	if (rig)
	{
		memset(&instance, 0, sizeof(instance));
		for (index = 0; index < ce; index++)
		{
			float world_node[12], r[12];

			rotation(0.0, 0.0, 1.0, index == ce - 1 ? 0.5 : 0.0, r);
			memcpy(world_node, many[index].bind_world, sizeof(world_node));
			if (index == ce - 1)
			{
				multiply(world_node, r, world_node);
			}
			multiply(world_node, many[index].inverse_bind, skin[index]);
		}
		instance.ce_matrices = (const float (*)[12])skin;
		instance.ce_matrix_count = ce;
		CHECK(xgpu_rig_evaluate(rig, NULL, &instance, palette, world) == XGPU_RIG_OK);
		/* the first helper copies a tenth of the last node's 0.5 radian turn
		(and carries its parent's turn) */
		NEAR(relative_angle(world[ce], world[ce - 1]), 0.05, 1e-4);
		for (index = ce; index < count && isfinite(palette[index][0]) && isfinite(palette[index][11]); index++)
			;
		/* down the chain the turns shrink, the scale stays 1 */
		NEAR(relative_angle(world[ce + 1], world[ce]), 0.005, 1e-4);
		NEAR(relative_angle(world[count - 1], world[count - 2]), 0.0, 1e-4);
		NEAR(sqrt(world[count - 1][0] * world[count - 1][0] + world[count - 1][4] * world[count - 1][4] +
			world[count - 1][8] * world[count - 1][8]), 1.0, 1e-4);
		if (index < count)
			printf("bone %u: %f %f %f %f\n", index, palette[index][0], palette[index][3], world[index][0], world[index][3]);
		CHECK(index == count);
		/* without a world out: the evaluation allocates its own */
		CHECK(xgpu_rig_evaluate(rig, NULL, &instance, palette, NULL) == XGPU_RIG_OK);
		xgpu_rig_destroy(rig);
	}
	/* 1025 bones, or 65 CE nodes: refused */
	CHECK(!xgpu_rig_create(many, count + 1, ce, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
	CHECK(!xgpu_rig_create(many, count, 65, NULL, 0, &status, error, sizeof(error)) && status == XGPU_RIG_SKELETON);
}

static const char *example_json =
	"{ \"version\": 1, \"comment\": \"the design's example, on the made-up skeleton\",\n"
	"  \"helpers\": [\n"
	"    {\"bone\": \"forearm twist\", \"type\": \"twist\", \"source\": \"forearm\", \"child\": \"hand\", \"axis\": \"x\", \"fraction\": 0.5},\n"
	"    {\"bone\": \"deltoid\", \"type\": \"copy_rotation\", \"source\": \"upperarm\", \"fraction\": 0.35, \"axes\": \"yz\", \"space\": \"parent\"},\n"
	"    {\"bone\": \"knee volume\", \"type\": \"driven\", \"driver\": {\"bone\": \"hand\", \"angle\": \"y\", \"range_deg\": [0, 140]},\n"
	"     \"translate\": [0, 0, 0.012], \"scale\": [1, 1.08, 1.08], \"curve\": \"smoothstep\"},\n"
	"    {\"bone\": \"pouch\", \"type\": \"spring\", \"parent\": \"pelvis\", \"stiffness\": 120, \"damping\": 9, \"gravity\": 0.3,\n"
	"     \"limit_deg\": 25, \"length\": 0.08, \"drive\": {\"function\": \"A\", \"rotate_deg\": [0, 0, 12]}},\n"
	"    {\"bone\": \"looker\", \"type\": \"look_at\", \"target\": \"camera\", \"axis\": \"x\", \"up\": \"z\", \"fraction\": 0.25},\n"
	"    {\"bone\": \"aimer\", \"type\": \"aim\", \"target\": \"hand\", \"axis\": \"y\", \"limit_deg\": 40}\n"
	"  ] }\n";

static enum xgpu_rig_status compile(const char *json, char *error)
{
	void *bytes;
	size_t size;
	enum xgpu_rig_status status;

	status = xgpu_rig_compile_json(bones, BONES, CE_NODES, json, strlen(json), &bytes, &size, error, 256);
	free(bytes);
	return status;
}

static void test_json(void)
{
	void *bytes;
	size_t size;
	char error[256];
	enum xgpu_rig_status status;
	struct xgpu_rig *rig;

	make_skeleton(0);
	status = xgpu_rig_compile_json(bones, BONES, CE_NODES, example_json, strlen(example_json), &bytes, &size, error,
		sizeof(error));
	if (status != XGPU_RIG_OK)
		fprintf(stderr, "example: %s\n", error);
	CHECK(status == XGPU_RIG_OK && size == sizeof(struct xgpu_rig_header) + 6 * sizeof(struct xgpu_rig_helper));
	if (status == XGPU_RIG_OK)
	{
		const struct xgpu_rig_helper *helpers = (const struct xgpu_rig_helper *)((const char *)bytes + 16);

		CHECK(helpers[0].type == XGPU_RIG_TWIST && helpers[0].bone == TWIST && helpers[0].source == FOREARM &&
			helpers[0].target == HAND && helpers[0].fraction == 0.5f && helpers[0].axes == 0);
		CHECK(helpers[1].axes == 6 && helpers[1].space == XGPU_RIG_SPACE_PARENT && helpers[1].fraction == 0.35f);
		CHECK(helpers[2].driver == XGPU_RIG_DRIVER_ANGLE && helpers[2].axis == 1 &&
			helpers[2].values[1] == (float)(140.0 * (PI / 180.0)) && helpers[2].values[4] == 0.012f &&
			helpers[2].values[9] == 1.08f && helpers[2].values[8] == 1.0f && helpers[2].curve == 1);
		CHECK(helpers[3].values[0] == 120.0f && helpers[3].values[1] == 9.0f && helpers[3].values[2] == 0.3f &&
			helpers[3].values[4] == 0.08f && helpers[3].function == 0 && helpers[3].values[7] == (float)(12.0 * (PI / 180.0)));
		CHECK(helpers[4].type == XGPU_RIG_LOOK_AT && helpers[4].target == XGPU_RIG_TARGET_CAMERA && helpers[4].axes == 2);
		CHECK(helpers[5].type == XGPU_RIG_AIM && helpers[5].target == HAND && helpers[5].axis == 1);
		rig = xgpu_rig_create(bones, BONES, CE_NODES, bytes, size, &status, error, sizeof(error));
		CHECK(rig && xgpu_rig_spring_count(rig) == 1);
		xgpu_rig_destroy(rig);
	}
	free(bytes);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"nobody\", \"type\": \"twist\"}]}", error) == XGPU_RIG_UNKNOWN_BONE);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"deltoid\", \"type\": \"copy_rotation\", \"source\": \"upperarm\", \"fractoin\": 1}]}", error) == XGPU_RIG_FORMAT);
	CHECK(strstr(error, "fractoin") != NULL);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"hand\", \"type\": \"copy_rotation\", \"source\": \"upperarm\"}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 2, \"helpers\": []}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"deltoid\", \"type\": \"wiggle\"}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"deltoid\", \"type\": \"copy_rotation\", \"source\": \"aimer\"},"
		"{\"bone\": \"aimer\", \"type\": \"aim\", \"target\": \"deltoid\"}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"pouch\", \"type\": \"spring\", \"parent\": \"root\"}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"knee volume\", \"type\": \"driven\", \"driver\": {\"function\": \"A\", \"range_deg\": [0, 1]}}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"knee volume\", \"type\": \"driven\", \"driver\": {\"function\": \"E\", \"range\": [0, 1]}}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"deltoid\", \"type\": \"copy_rotation\", \"source\": \"upperarm\", \"axes\": \"xx\"}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [{\"bone\": \"deltoid\", \"type\": \"copy_rotation\", \"source\": \"upperarm\", \"fraction\": true}]}", error) == XGPU_RIG_FORMAT);
	CHECK(compile("[1, 2]", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": [", error) == XGPU_RIG_FORMAT);
	CHECK(compile("{\"version\": 1, \"helpers\": []}", error) == XGPU_RIG_OK);
}

static void test_skin_reference(void)
{
	float palette[2][12], positions[2][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
	float normals[2][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}, out[2][3], out_normals[2][3];
	uint16_t joints[2][4] = {{0, 1, 0, 0}, {1, 0, 0, 0}};
	uint8_t weights[2][4] = {{128, 127, 0, 0}, {255, 0, 0, 0}};

	identity(palette[0]);
	translation(0.0f, 0.0f, 2.0f, palette[1]);
	xgpu_rig_skin_reference((const float (*)[12])palette, 2, (const float (*)[3])positions, (const float (*)[3])normals,
		(const uint16_t (*)[4])joints, (const uint8_t (*)[4])weights, 2, out, out_normals);
	NEAR(out[0][0], 1.0, 1e-6);
	NEAR(out[0][2], 2.0 * 127.0 / 255.0, 1e-6);
	NEAR(out[1][2], 2.0, 1e-6);
	NEAR(out_normals[0][2], 1.0, 1e-6);
}

static void test_time(void)
{
	/* a frame of 40 instances of a rig of 30 helpers (10 copies, 10
	twists, 5 aims, 5 springs) on a 19-node body */
	enum { CE = 19, HELPERS = 30, ALL = CE + HELPERS, INSTANCES = 40, FRAMES = 200 };
	static struct xgpu_rig_bone body[ALL];
	static struct
	{
		struct xgpu_rig_header header;
		struct xgpu_rig_helper helpers[HELPERS];
	} list;
	float skin[CE][12], palette[ALL][12];
	struct xgpu_rig_states *states = xgpu_rig_states_create();
	struct xgpu_rig_instance instance;
	enum xgpu_rig_status status;
	struct xgpu_rig *rig;
	struct timespec start, end;
	char error[256];
	int index, frame, which;
	double seconds;

	for (index = 0; index < ALL; index++)
	{
		memset(&body[index], 0, sizeof(body[index]));
		body[index].parent = (int16_t)(index == 0 ? -1 : index < CE ? (index - 1) / 2 : (index - CE) % CE);
		body[index].flags = index < CE ? XGPU_RIG_BONE_CE_NODE : XGPU_RIG_BONE_HELPER;
		body[index].ce_node = (int16_t)(index < CE ? index : -1);
		rotation(0.1 * index, 1.0, 0.3, 0.05 * index, body[index].bind_world);
		body[index].bind_world[3] = 0.05f * index;
		body[index].bind_world[11] = 0.02f * (index % 5);
		invert_rigid(body[index].bind_world, body[index].inverse_bind);
	}
	for (index = 0; index < HELPERS; index++)
	{
		int bone = CE + index, parent = body[bone].parent;

		list.helpers[index] = rule(bone, index < 10 ? XGPU_RIG_COPY_ROTATION : index < 20 ? XGPU_RIG_TWIST :
			index < 25 ? XGPU_RIG_AIM : XGPU_RIG_SPRING);
		list.helpers[index].source = (int16_t)(parent == 0 ? 1 : parent);
		list.helpers[index].target = (int16_t)(index < 20 ? (2 * list.helpers[index].source + 1 < CE ?
			2 * list.helpers[index].source + 1 : 0) : 0);
		if (list.helpers[index].source == list.helpers[index].target)
			list.helpers[index].target = 2;
		if (index >= 20 && index < 25)
			list.helpers[index].target = XGPU_RIG_TARGET_CAMERA;
		list.helpers[index].fraction = 0.5f;
		list.helpers[index].axes = (uint8_t)(index < 10 ? 7 : 0);
		list.helpers[index].values[0] = 120.0f, list.helpers[index].values[1] = 6.0f, list.helpers[index].values[4] = 0.05f;
	}
	list.header.magic = XGPU_RIG_MAGIC;
	list.header.version = XGPU_RIG_VERSION;
	list.header.helper_count = HELPERS;
	list.header.bone_count = ALL;
	list.header.ce_node_count = CE;
	rig = xgpu_rig_create(body, ALL, CE, &list, sizeof(list), &status, error, sizeof(error));
	if (!rig)
		fprintf(stderr, "timing rig: %s\n", error);
	CHECK(rig != NULL);
	if (!rig)
		return;
	memset(&instance, 0, sizeof(instance));
	instance.ce_matrices = (const float (*)[12])skin;
	instance.ce_matrix_count = CE;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (frame = 0; frame < FRAMES; frame++)
	{
		for (index = 0; index < CE; index++)
		{
			rotation(0.2, 1.0, 0.1 * index, 0.3 * sin(frame * 0.1 + index), skin[index]);
			skin[index][3] = 0.01f * frame;
		}
		instance.game_seconds = frame / 60.0;
		for (which = 1; which <= INSTANCES; which++)
		{
			instance.key = (uint64_t)which;
			xgpu_rig_evaluate(rig, states, &instance, palette, NULL);
		}
		xgpu_rig_states_age(states, instance.game_seconds);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	seconds = (double)(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9;
	printf("a frame of %d instances x %d helpers (%d bones): %.4f ms\n", INSTANCES, HELPERS, ALL,
		seconds * 1000.0 / FRAMES);
	xgpu_rig_destroy(rig);
	xgpu_rig_states_destroy(states);
}

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	char *data;
	long length;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)length + 1);
	if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		free(data);
		data = NULL;
	}
	fclose(file);
	if (data)
		data[length] = 0;
	*size = (size_t)length;
	return data;
}

/* compile <skeleton entries> <ce nodes> <rig.json> <out>: 0 and the bytes,
else the status and the message */
static int compile_command(char **argv)
{
	size_t skeleton_size, json_size, size;
	struct xgpu_rig_bone *skeleton = read_file(argv[2], &skeleton_size);
	char *json = read_file(argv[4], &json_size), error[256];
	enum xgpu_rig_status status;
	void *bytes;
	FILE *out;

	if (!skeleton || !json || skeleton_size % sizeof(struct xgpu_rig_bone))
		return 2;
	status = xgpu_rig_compile_json(skeleton, (uint32_t)(skeleton_size / sizeof(struct xgpu_rig_bone)),
		(uint32_t)atoi(argv[3]), json, json_size, &bytes, &size, error, sizeof(error));
	if (status != XGPU_RIG_OK)
	{
		printf("status %d: %s\n", status, error);
		return 10 + status;
	}
	out = fopen(argv[5], "wb");
	fwrite(bytes, 1, size, out);
	fclose(out);
	free(bytes);
	free(skeleton);
	free(json);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 6 && strcmp(argv[1], "compile") == 0)
		return compile_command(argv);
	test_names();
	test_skeleton_checks();
	test_rule_checks();
	test_passthrough_and_follow();
	test_copy_rotation();
	test_twist();
	test_aim_and_look_at();
	test_driven();
	test_spring_response();
	test_spring_clock();
	test_spring_resets();
	test_states_table();
	test_rigid_motion();
	test_mirrored();
	test_many_bones();
	test_json();
	test_skin_reference();
	test_time();
	if (failures)
	{
		printf("%d failed\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
