/*
XGPU_RIG.C

PRISM's extended skeletons: a skeleton's helper bones worked out from the
CE nodes' matrices by the rig's rules, and the springs' state (xgpu_rig.h).
*/

#include "xgpu_rig.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "../../third_party/jsmn/jsmn.h"

#define PI 3.14159265358979323846

_Static_assert(sizeof(struct xgpu_rig_bone) == 108, "a SKEL entry is 108 bytes");
_Static_assert(sizeof(struct xgpu_rig_helper) == 84, "a rule is 84 bytes");
_Static_assert(sizeof(struct xgpu_rig_header) == 16, "the rig's header is 16 bytes");

struct xgpu_rig
{
	uint32_t bone_count, ce_node_count, helper_count, spring_count;
	struct xgpu_rig_bone *bones;
	/* each bone's bind pose relative to its parent's (the root's: its bind world) */
	float (*rest_local)[12];
	/* each helper bone's rule (a follow if it has none), in the order they are
	worked out in: a bone after every bone its rule reads */
	struct xgpu_rig_helper *order;
	uint32_t order_count;
	/* per rule of order: its spring's number, else -1 */
	int32_t *spring_index;
	/* rest_local's rotations; per rule of order, a twist's child's bind
	rotation relative to its source's */
	double (*rest_rotation)[9];
	double (*twist_rest)[9];
	/* a rig of more than 64 bones: the evaluation's frames' rotations and
	worlds (one evaluation of a rig at a time) */
	double (*scratch_rotations)[9];
	float (*scratch_world)[12];
};

struct spring_state
{
	double p[3], v[3], previous[3];
	double target[3]; /* the last frame's sampled tip */
	double step_target[3]; /* the target at the last step (its velocity's start) */
};

struct state_entry
{
	uint64_t key;
	int64_t step; /* the game's clock in springs' steps: the state's time */
	double seen; /* the game time last evaluated */
	double target_seconds; /* when the last targets were sampled */
	uint32_t spring_count;
	int fresh; /* no step taken yet */
	struct spring_state *springs;
};

struct xgpu_rig_states
{
	struct state_entry *entries;
	uint32_t capacity, count;
};

/* ---------- 3x4 matrices: rows of (x, y, z, translation) */

static void matrix_multiply(const float a[12], const float b[12], float out[12])
{
	float r[12];
	int row;

	for (row = 0; row < 3; row++)
	{
		const float *x = a + row * 4;

		r[row * 4 + 0] = x[0] * b[0] + x[1] * b[4] + x[2] * b[8];
		r[row * 4 + 1] = x[0] * b[1] + x[1] * b[5] + x[2] * b[9];
		r[row * 4 + 2] = x[0] * b[2] + x[1] * b[6] + x[2] * b[10];
		r[row * 4 + 3] = x[0] * b[3] + x[1] * b[7] + x[2] * b[11] + x[3];
	}
	memcpy(out, r, sizeof(r));
}

static void matrix_column(const float m[12], int column, double out[3])
{
	out[0] = m[column];
	out[1] = m[4 + column];
	out[2] = m[8 + column];
}

static void matrix_translation(const float m[12], double out[3])
{
	matrix_column(m, 3, out);
}

static void quaternion_from_rotation(const double r[9], double q[4]);
static void rotation_from_quaternion(const double q[4], double r[9]);

/* the matrix's 3x3 part as a rotation: divided by its scale (CE's is uniform),
and by -1 if it mirrors (a left-hander's VR hands: only the products of two
such are used, in which both cancel), then made exactly orthonormal (through
its quaternion): the rules conjugate by these frames, and a frame a little
off a rotation would grow its error down a chain of helpers */
static void rotation_of(const float m[12], double r[9])
{
	double determinant =
		m[0] * ((double)m[5] * m[10] - (double)m[6] * m[9]) -
		m[1] * ((double)m[4] * m[10] - (double)m[6] * m[8]) +
		m[2] * ((double)m[4] * m[9] - (double)m[5] * m[8]);
	/* the first column's length, negative if it mirrors (cheaper than the
	determinant's cube root, and the same for a uniform scale) */
	double scale = sqrt((double)m[0] * m[0] + (double)m[4] * m[4] + (double)m[8] * m[8]), scaled[9], q[4];
	int row;

	if (scale < 1e-12)
		scale = 1.0;
	if (determinant < 0.0)
		scale = -scale;
	for (row = 0; row < 3; row++)
	{
		scaled[row * 3 + 0] = m[row * 4 + 0] / scale;
		scaled[row * 3 + 1] = m[row * 4 + 1] / scale;
		scaled[row * 3 + 2] = m[row * 4 + 2] / scale;
	}
	quaternion_from_rotation(scaled, q);
	rotation_from_quaternion(q, r);
}

static void rotation_multiply(const double a[9], const double b[9], double out[9])
{
	double r[9];
	int row, column;

	for (row = 0; row < 3; row++)
		for (column = 0; column < 3; column++)
			r[row * 3 + column] = a[row * 3] * b[column] + a[row * 3 + 1] * b[3 + column] +
				a[row * 3 + 2] * b[6 + column];
	memcpy(out, r, sizeof(r));
}

static void rotation_transpose(const double a[9], double out[9])
{
	double r[9] = {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};

	memcpy(out, r, sizeof(r));
}

/* ---------- quaternions (x, y, z, w) */

static void quaternion_normalize(double q[4])
{
	double length = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);

	if (length < 1e-20)
	{
		q[0] = q[1] = q[2] = 0.0;
		q[3] = 1.0;
		return;
	}
	q[0] /= length;
	q[1] /= length;
	q[2] /= length;
	q[3] /= length;
}

/* a proper rotation's (Shepperd's method), its w not negative */
static void quaternion_from_rotation(const double r[9], double q[4])
{
	double trace = r[0] + r[4] + r[8];

	if (trace > 0.0)
	{
		double s = sqrt(trace + 1.0) * 2.0;

		q[3] = 0.25 * s;
		q[0] = (r[7] - r[5]) / s;
		q[1] = (r[2] - r[6]) / s;
		q[2] = (r[3] - r[1]) / s;
	}
	else if (r[0] > r[4] && r[0] > r[8])
	{
		double s = sqrt(1.0 + r[0] - r[4] - r[8]) * 2.0;

		q[3] = (r[7] - r[5]) / s;
		q[0] = 0.25 * s;
		q[1] = (r[1] + r[3]) / s;
		q[2] = (r[2] + r[6]) / s;
	}
	else if (r[4] > r[8])
	{
		double s = sqrt(1.0 + r[4] - r[0] - r[8]) * 2.0;

		q[3] = (r[2] - r[6]) / s;
		q[0] = (r[1] + r[3]) / s;
		q[1] = 0.25 * s;
		q[2] = (r[5] + r[7]) / s;
	}
	else
	{
		double s = sqrt(1.0 + r[8] - r[0] - r[4]) * 2.0;

		q[3] = (r[3] - r[1]) / s;
		q[0] = (r[2] + r[6]) / s;
		q[1] = (r[5] + r[7]) / s;
		q[2] = 0.25 * s;
	}
	quaternion_normalize(q);
	if (q[3] < 0.0)
	{
		q[0] = -q[0];
		q[1] = -q[1];
		q[2] = -q[2];
		q[3] = -q[3];
	}
}

static void rotation_from_quaternion(const double q[4], double r[9])
{
	double x = q[0], y = q[1], z = q[2], w = q[3];

	r[0] = 1.0 - 2.0 * (y * y + z * z);
	r[1] = 2.0 * (x * y - z * w);
	r[2] = 2.0 * (x * z + y * w);
	r[3] = 2.0 * (x * y + z * w);
	r[4] = 1.0 - 2.0 * (x * x + z * z);
	r[5] = 2.0 * (y * z - x * w);
	r[6] = 2.0 * (x * z - y * w);
	r[7] = 2.0 * (y * z + x * w);
	r[8] = 1.0 - 2.0 * (x * x + y * y);
}

static void rotation_from_axis_angle(const double axis[3], double angle, double r[9])
{
	double length = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	double s, q[4];

	if (length < 1e-20)
	{
		q[0] = q[1] = q[2] = 0.0;
		q[3] = 1.0;
	}
	else
	{
		s = sin(angle * 0.5) / length;
		q[0] = axis[0] * s;
		q[1] = axis[1] * s;
		q[2] = axis[2] * s;
		q[3] = cos(angle * 0.5);
	}
	rotation_from_quaternion(q, r);
}

/* the rotation's angle (0 to pi) and axis */
static double quaternion_angle(const double q[4], double axis[3])
{
	double s = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
	double w = q[3] < 0.0 ? -q[3] : q[3];
	double sign = q[3] < 0.0 ? -1.0 : 1.0;

	if (s < 1e-15)
	{
		axis[0] = 1.0;
		axis[1] = axis[2] = 0.0;
		return 0.0;
	}
	axis[0] = sign * q[0] / s;
	axis[1] = sign * q[1] / s;
	axis[2] = sign * q[2] / s;
	return 2.0 * atan2(s, w);
}

/* the rotation from identity a fraction of the way to q, by its shortest arc */
static void quaternion_fraction(const double q[4], double fraction, double out[9])
{
	double axis[3];
	double angle = quaternion_angle(q, axis);

	rotation_from_axis_angle(axis, angle * fraction, out);
}

/* the twist of q about a unit axis (the swing and twist split), as an
angle in (-pi, pi] */
static double twist_angle(const double q[4], const double axis[3])
{
	double projection = q[0] * axis[0] + q[1] * axis[1] + q[2] * axis[2];
	double angle = 2.0 * atan2(projection, q[3]);

	if (angle > PI)
		angle -= 2.0 * PI;
	else if (angle <= -PI)
		angle += 2.0 * PI;
	return angle;
}

/* Blender's XYZ Euler: R = Rz(z) Ry(y) Rx(x) */
static void euler_from_rotation(const double r[9], double angles[3])
{
	double cy = sqrt(r[0] * r[0] + r[3] * r[3]);

	if (cy > 1e-9)
	{
		angles[0] = atan2(r[7], r[8]);
		angles[1] = atan2(-r[6], cy);
		angles[2] = atan2(r[3], r[0]);
	}
	else
	{
		angles[0] = atan2(-r[5], r[4]);
		angles[1] = atan2(-r[6], cy);
		angles[2] = 0.0;
	}
}

static void rotation_from_euler(const double angles[3], double r[9])
{
	double cx = cos(angles[0]), sx = sin(angles[0]);
	double cy = cos(angles[1]), sy = sin(angles[1]);
	double cz = cos(angles[2]), sz = sin(angles[2]);

	r[0] = cz * cy;
	r[1] = cz * sy * sx - sz * cx;
	r[2] = cz * sy * cx + sz * sx;
	r[3] = sz * cy;
	r[4] = sz * sy * sx + cz * cx;
	r[5] = sz * sy * cx - cz * sx;
	r[6] = -sy;
	r[7] = cy * sx;
	r[8] = cy * cx;
}

/* the shortest arc's rotation from a to b (neither zero) */
static void rotation_between(const double a[3], const double b[3], double r[9])
{
	double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	double lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
	double ua[3], ub[3], axis[3], c;

	if (la < 1e-15 || lb < 1e-15)
	{
		double none[3] = {1.0, 0.0, 0.0};

		rotation_from_axis_angle(none, 0.0, r);
		return;
	}
	ua[0] = a[0] / la, ua[1] = a[1] / la, ua[2] = a[2] / la;
	ub[0] = b[0] / lb, ub[1] = b[1] / lb, ub[2] = b[2] / lb;
	axis[0] = ua[1] * ub[2] - ua[2] * ub[1];
	axis[1] = ua[2] * ub[0] - ua[0] * ub[2];
	axis[2] = ua[0] * ub[1] - ua[1] * ub[0];
	c = ua[0] * ub[0] + ua[1] * ub[1] + ua[2] * ub[2];
	if (c < -1.0)
		c = -1.0;
	if (c > 1.0)
		c = 1.0;
	if (sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) < 1e-12)
	{
		if (c > 0.0)
		{
			axis[0] = 1.0, axis[1] = axis[2] = 0.0;
			rotation_from_axis_angle(axis, 0.0, r);
			return;
		}
		/* opposite: any perpendicular axis */
		axis[0] = 0.0, axis[1] = -ua[2], axis[2] = ua[1];
		if (fabs(ua[0]) > 0.9)
			axis[0] = -ua[1], axis[1] = ua[0], axis[2] = 0.0;
	}
	rotation_from_axis_angle(axis, acos(c), r);
}

/* a world rotation's quaternion limited to an angle and taken a fraction of
the way */
static void rotation_limit_fraction(const double r[9], double limit, double fraction, double out[9])
{
	double q[4], axis[3], angle;

	quaternion_from_rotation(r, q);
	angle = quaternion_angle(q, axis);
	if (limit > 0.0 && angle > limit)
		angle = limit;
	rotation_from_axis_angle(axis, angle * fraction, out);
}

/* m turned by the world rotation r about its own origin */
static void matrix_turn(float m[12], const double r[9])
{
	double basis[9], turned[9];
	int row;

	for (row = 0; row < 3; row++)
	{
		basis[row * 3 + 0] = m[row * 4 + 0];
		basis[row * 3 + 1] = m[row * 4 + 1];
		basis[row * 3 + 2] = m[row * 4 + 2];
	}
	rotation_multiply(r, basis, turned);
	for (row = 0; row < 3; row++)
	{
		m[row * 4 + 0] = (float)turned[row * 3 + 0];
		m[row * 4 + 1] = (float)turned[row * 3 + 1];
		m[row * 4 + 2] = (float)turned[row * 3 + 2];
	}
}

/* ---------- the rig */

uint32_t xgpu_rig_name_hash(const char *name, size_t length)
{
	uint32_t hash = 2166136261u;
	size_t index;

	for (index = 0; index < length; index++)
	{
		unsigned char c = (unsigned char)name[index];

		if (c >= 'A' && c <= 'Z')
			c = (unsigned char)(c - 'A' + 'a');
		hash = (hash ^ c) * 16777619u;
	}
	return hash;
}

static enum xgpu_rig_status fail(enum xgpu_rig_status status, char *error, size_t error_size, const char *format, ...)
{
	va_list arguments;

	if (error && error_size)
	{
		va_start(arguments, format);
		vsnprintf(error, error_size, format, arguments);
		va_end(arguments);
	}
	return status;
}

static int bone_valid(const struct xgpu_rig *rig, int bone)
{
	return bone >= 0 && (uint32_t)bone < rig->bone_count;
}

static enum xgpu_rig_status check_skeleton(const struct xgpu_rig_bone *bones, uint32_t bone_count,
	uint32_t ce_node_count, char *error, size_t error_size)
{
	uint32_t index;

	if (!bones || bone_count == 0 || bone_count > XGPU_RIG_BONES_MAXIMUM)
		return fail(XGPU_RIG_SKELETON, error, error_size, "%u bones (1 to %u)", bone_count, XGPU_RIG_BONES_MAXIMUM);
	if (ce_node_count == 0 || ce_node_count > XGPU_RIG_CE_NODES_MAXIMUM || ce_node_count > bone_count)
		return fail(XGPU_RIG_SKELETON, error, error_size, "%u CE nodes of %u bones", ce_node_count, bone_count);
	for (index = 0; index < bone_count; index++)
	{
		const struct xgpu_rig_bone *bone = &bones[index];
		float product[12];
		int element;

		if (bone->parent < -1 || bone->parent >= (int)index)
			return fail(XGPU_RIG_SKELETON, error, error_size, "bone %u: parent %d not an earlier bone", index,
				bone->parent);
		if (index < ce_node_count)
		{
			if (!(bone->flags & XGPU_RIG_BONE_CE_NODE) || (bone->flags & XGPU_RIG_BONE_HELPER) ||
				bone->ce_node != (int)index)
				return fail(XGPU_RIG_SKELETON, error, error_size, "bone %u: not CE node %u", index, index);
		}
		else if (!(bone->flags & XGPU_RIG_BONE_HELPER) || (bone->flags & XGPU_RIG_BONE_CE_NODE) || bone->ce_node != -1)
			return fail(XGPU_RIG_SKELETON, error, error_size, "bone %u: not a helper", index);
		for (element = 0; element < 12; element++)
			if (!isfinite(bone->bind_world[element]) || !isfinite(bone->inverse_bind[element]))
				return fail(XGPU_RIG_SKELETON, error, error_size, "bone %u: a matrix not finite", index);
		matrix_multiply(bone->bind_world, bone->inverse_bind, product);
		for (element = 0; element < 12; element++)
		{
			float expected = (element == 0 || element == 5 || element == 10) ? 1.0f : 0.0f;

			if (fabsf(product[element] - expected) > 1e-3f)
				return fail(XGPU_RIG_SKELETON, error, error_size, "bone %u: inverse bind not the bind's inverse",
					index);
		}
	}
	return XGPU_RIG_OK;
}

static enum xgpu_rig_status check_helper(const struct xgpu_rig *rig, const struct xgpu_rig_helper *helper,
	char *error, size_t error_size)
{
	int bone = helper->bone;

	if (bone < (int)rig->ce_node_count || !bone_valid(rig, bone))
		return fail(XGPU_RIG_FORMAT, error, error_size, "a rule for bone %d, not a helper", bone);
	if (helper->type >= XGPU_RIG_HELPER_TYPES || helper->axis > 2 || !isfinite(helper->fraction))
		return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: type %u, axis %u", bone, helper->type, helper->axis);
	{
		int index;

		for (index = 0; index < 16; index++)
			if (!isfinite(helper->values[index]))
				return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: a value not finite", bone);
	}
	switch (helper->type)
	{
	case XGPU_RIG_COPY_ROTATION:
		if (!bone_valid(rig, helper->source) || helper->source == bone || helper->axes == 0 || helper->axes > 7 ||
			helper->space > XGPU_RIG_SPACE_PARENT)
			return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: copy_rotation's source, axes or space", bone);
		break;
	case XGPU_RIG_TWIST:
		if (!bone_valid(rig, helper->source) || !bone_valid(rig, helper->target) || helper->source == helper->target ||
			helper->source == bone || helper->target == bone || helper->axes > 2)
			return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: twist's source or child", bone);
		break;
	case XGPU_RIG_AIM:
	case XGPU_RIG_LOOK_AT:
		if ((helper->target != XGPU_RIG_TARGET_CAMERA && !bone_valid(rig, helper->target)) || helper->target == bone ||
			(helper->type == XGPU_RIG_LOOK_AT && (helper->axes > 2 || helper->axes == helper->axis)))
			return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: aim's target or up axis", bone);
		break;
	case XGPU_RIG_DRIVEN:
		if (helper->driver > XGPU_RIG_DRIVER_DISTANCE || helper->curve > XGPU_RIG_CURVE_SMOOTHSTEP ||
			(helper->driver == XGPU_RIG_DRIVER_FUNCTION && helper->function > 3) ||
			(helper->driver != XGPU_RIG_DRIVER_FUNCTION && (!bone_valid(rig, helper->source) || helper->source == bone)) ||
			(helper->driver == XGPU_RIG_DRIVER_DISTANCE && (!bone_valid(rig, helper->target) || helper->target == bone)) ||
			helper->values[0] == helper->values[1])
			return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d: driven's driver or range", bone);
		break;
	case XGPU_RIG_SPRING:
		if (helper->values[0] <= 0.0f || helper->values[1] < 0.0f || helper->values[3] < 0.0f ||
			helper->values[4] <= 0.0f || (helper->function != 0xff && helper->function > 3) ||
			helper->values[0] * XGPU_RIG_SPRING_STEP * XGPU_RIG_SPRING_STEP > 2.0 ||
			helper->values[1] * XGPU_RIG_SPRING_STEP > 1.0)
			return fail(XGPU_RIG_FORMAT, error, error_size,
				"bone %d: spring's stiffness, damping, limit or length (stiffness under %.0f, damping under %.0f)",
				bone, 2.0 / (XGPU_RIG_SPRING_STEP * XGPU_RIG_SPRING_STEP), 1.0 / XGPU_RIG_SPRING_STEP);
		break;
	default:
		break;
	}
	return XGPU_RIG_OK;
}

/* the bones a rule reads, besides its own parent */
static int helper_reads(const struct xgpu_rig_helper *helper, int reads[2])
{
	int count = 0;

	switch (helper->type)
	{
	case XGPU_RIG_COPY_ROTATION:
		reads[count++] = helper->source;
		break;
	case XGPU_RIG_TWIST:
		reads[count++] = helper->source;
		reads[count++] = helper->target;
		break;
	case XGPU_RIG_AIM:
	case XGPU_RIG_LOOK_AT:
		if (helper->target >= 0)
			reads[count++] = helper->target;
		break;
	case XGPU_RIG_DRIVEN:
		if (helper->driver != XGPU_RIG_DRIVER_FUNCTION)
			reads[count++] = helper->source;
		if (helper->driver == XGPU_RIG_DRIVER_DISTANCE)
			reads[count++] = helper->target;
		break;
	default:
		break;
	}
	return count;
}

struct xgpu_rig *xgpu_rig_create(const struct xgpu_rig_bone *bones, uint32_t bone_count, uint32_t ce_node_count,
	const void *bytes, size_t size, enum xgpu_rig_status *status, char *error, size_t error_size)
{
	struct xgpu_rig *rig = NULL;
	struct xgpu_rig_header header;
	const struct xgpu_rig_helper *helpers = NULL;
	int32_t *rule = NULL; /* per bone: its rule's index in helpers, -1 none */
	uint8_t *done = NULL;
	enum xgpu_rig_status result;
	uint32_t index, helper_count = 0;

	if (error && error_size)
		error[0] = 0;
	result = check_skeleton(bones, bone_count, ce_node_count, error, error_size);
	if (result != XGPU_RIG_OK)
		goto out;
	if (bytes)
	{
		if (size < sizeof(header))
		{
			result = fail(XGPU_RIG_FORMAT, error, error_size, "%zu bytes", size);
			goto out;
		}
		memcpy(&header, bytes, sizeof(header));
		helper_count = header.helper_count;
		if (header.magic != XGPU_RIG_MAGIC || header.version != XGPU_RIG_VERSION ||
			size != sizeof(header) + (size_t)helper_count * sizeof(struct xgpu_rig_helper) ||
			helper_count > XGPU_RIG_HELPERS_MAXIMUM)
		{
			result = fail(XGPU_RIG_FORMAT, error, error_size, "not a rig of version %u, or its size", XGPU_RIG_VERSION);
			goto out;
		}
		if (header.bone_count != bone_count || header.ce_node_count != ce_node_count)
		{
			result = fail(XGPU_RIG_SKELETON, error, error_size, "compiled for %u bones (%u CE nodes), not %u (%u)",
				header.bone_count, header.ce_node_count, bone_count, ce_node_count);
			goto out;
		}
		helpers = (const struct xgpu_rig_helper *)((const uint8_t *)bytes + sizeof(header));
	}
	rig = calloc(1, sizeof(*rig));
	rule = malloc(bone_count * sizeof(*rule));
	done = calloc(bone_count, 1);
	if (!rig || !rule || !done)
	{
		result = fail(XGPU_RIG_FORMAT, error, error_size, "out of memory");
		goto out;
	}
	rig->bone_count = bone_count;
	rig->ce_node_count = ce_node_count;
	rig->bones = malloc(bone_count * sizeof(*rig->bones));
	rig->rest_local = malloc(bone_count * sizeof(*rig->rest_local));
	rig->order = calloc(bone_count - ce_node_count + 1, sizeof(*rig->order));
	rig->spring_index = calloc(bone_count - ce_node_count + 1, sizeof(*rig->spring_index));
	rig->rest_rotation = malloc(bone_count * sizeof(*rig->rest_rotation));
	rig->twist_rest = calloc(bone_count - ce_node_count + 1, sizeof(*rig->twist_rest));
	if (bone_count > 64)
	{
		rig->scratch_rotations = malloc(bone_count * sizeof(*rig->scratch_rotations));
		rig->scratch_world = malloc(bone_count * sizeof(*rig->scratch_world));
	}
	if (!rig->bones || !rig->rest_local || !rig->order || !rig->spring_index || !rig->rest_rotation || !rig->twist_rest ||
		(bone_count > 64 && (!rig->scratch_rotations || !rig->scratch_world)))
	{
		result = fail(XGPU_RIG_FORMAT, error, error_size, "out of memory");
		goto out;
	}
	memcpy(rig->bones, bones, bone_count * sizeof(*bones));
	for (index = 0; index < bone_count; index++)
	{
		const struct xgpu_rig_bone *bone = &bones[index];

		if (bone->parent < 0)
			memcpy(rig->rest_local[index], bone->bind_world, sizeof(rig->rest_local[index]));
		else
			matrix_multiply(bones[bone->parent].inverse_bind, bone->bind_world, rig->rest_local[index]);
		rotation_of(rig->rest_local[index], rig->rest_rotation[index]);
		rule[index] = -1;
	}
	for (index = 0; index < helper_count; index++)
	{
		result = check_helper(rig, &helpers[index], error, error_size);
		if (result != XGPU_RIG_OK)
			goto out;
		if (rule[helpers[index].bone] >= 0)
		{
			result = fail(XGPU_RIG_FORMAT, error, error_size, "bone %u has two rules", helpers[index].bone);
			goto out;
		}
		rule[helpers[index].bone] = (int32_t)index;
	}
	rig->helper_count = helper_count;
	/* the order: CE nodes are known; then repeatedly every helper whose
	parent and reads are known (a cycle leaves some never ready) */
	for (index = 0; index < ce_node_count; index++)
		done[index] = 1;
	for (;;)
	{
		int progress = 0;

		for (index = ce_node_count; index < bone_count; index++)
		{
			struct xgpu_rig_helper follow;
			const struct xgpu_rig_helper *helper;
			int reads[2], count, read, ready;

			if (done[index])
				continue;
			if (rule[index] >= 0)
				helper = &helpers[rule[index]];
			else
			{
				memset(&follow, 0, sizeof(follow));
				follow.bone = (uint16_t)index;
				follow.type = XGPU_RIG_FOLLOW;
				follow.source = follow.target = -1;
				follow.function = 0xff;
				helper = &follow;
			}
			ready = bones[index].parent < 0 || done[bones[index].parent];
			count = helper_reads(helper, reads);
			for (read = 0; read < count; read++)
				ready = ready && done[reads[read]];
			if (!ready)
				continue;
			rig->spring_index[rig->order_count] = helper->type == XGPU_RIG_SPRING ? (int32_t)rig->spring_count++ : -1;
			if (helper->type == XGPU_RIG_TWIST)
			{
				float rest[12];

				matrix_multiply(bones[helper->source].inverse_bind, bones[helper->target].bind_world, rest);
				rotation_of(rest, rig->twist_rest[rig->order_count]);
			}
			rig->order[rig->order_count++] = *helper;
			done[index] = 1;
			progress = 1;
		}
		if (!progress)
			break;
	}
	if (rig->order_count != bone_count - ce_node_count)
	{
		result = fail(XGPU_RIG_FORMAT, error, error_size, "the rules read each other in a cycle");
		goto out;
	}
	result = XGPU_RIG_OK;
out:
	free(rule);
	free(done);
	if (result != XGPU_RIG_OK)
	{
		xgpu_rig_destroy(rig);
		rig = NULL;
	}
	if (status)
		*status = result;
	return rig;
}

void xgpu_rig_destroy(struct xgpu_rig *rig)
{
	if (!rig)
		return;
	free(rig->bones);
	free(rig->rest_local);
	free(rig->order);
	free(rig->spring_index);
	free(rig->rest_rotation);
	free(rig->twist_rest);
	free(rig->scratch_rotations);
	free(rig->scratch_world);
	free(rig);
}

uint32_t xgpu_rig_bone_count(const struct xgpu_rig *rig)
{
	return rig->bone_count;
}

uint32_t xgpu_rig_ce_node_count(const struct xgpu_rig *rig)
{
	return rig->ce_node_count;
}

uint32_t xgpu_rig_spring_count(const struct xgpu_rig *rig)
{
	return rig->spring_count;
}

/* ---------- the springs' state */

struct xgpu_rig_states *xgpu_rig_states_create(void)
{
	struct xgpu_rig_states *states = calloc(1, sizeof(*states));

	return states;
}

void xgpu_rig_states_clear(struct xgpu_rig_states *states)
{
	uint32_t index;

	for (index = 0; index < states->capacity; index++)
		free(states->entries[index].springs);
	free(states->entries);
	states->entries = NULL;
	states->capacity = states->count = 0;
}

void xgpu_rig_states_destroy(struct xgpu_rig_states *states)
{
	if (!states)
		return;
	xgpu_rig_states_clear(states);
	free(states);
}

uint32_t xgpu_rig_states_count(const struct xgpu_rig_states *states)
{
	return states->count;
}

static uint32_t key_slot(uint64_t key, uint32_t capacity)
{
	key ^= key >> 33;
	key *= 0xff51afd7ed558ccdull;
	key ^= key >> 33;
	return (uint32_t)key & (capacity - 1);
}

static int states_grow(struct xgpu_rig_states *states)
{
	uint32_t capacity = states->capacity ? states->capacity * 2 : 64;
	struct state_entry *entries = calloc(capacity, sizeof(*entries));
	uint32_t index;

	if (!entries)
		return 0;
	for (index = 0; index < states->capacity; index++)
	{
		struct state_entry *entry = &states->entries[index];
		uint32_t slot;

		if (!entry->key)
			continue;
		slot = key_slot(entry->key, capacity);
		while (entries[slot].key)
			slot = (slot + 1) & (capacity - 1);
		entries[slot] = *entry;
	}
	free(states->entries);
	states->entries = entries;
	states->capacity = capacity;
	return 1;
}

/* the entry of key, made (fresh) if there is none; NULL if memory is out */
static struct state_entry *states_find(struct xgpu_rig_states *states, uint64_t key, uint32_t spring_count)
{
	struct state_entry *entry;
	uint32_t slot;

	if ((states->count + 1) * 4 > states->capacity * 3 && !states_grow(states))
		return NULL;
	slot = key_slot(key, states->capacity);
	while (states->entries[slot].key && states->entries[slot].key != key)
		slot = (slot + 1) & (states->capacity - 1);
	entry = &states->entries[slot];
	if (!entry->key)
	{
		memset(entry, 0, sizeof(*entry));
		entry->key = key;
		entry->fresh = 1;
		states->count++;
	}
	if (entry->spring_count != spring_count)
	{
		free(entry->springs);
		entry->springs = spring_count ? calloc(spring_count, sizeof(*entry->springs)) : NULL;
		entry->spring_count = entry->springs ? spring_count : 0;
		entry->fresh = 1;
	}
	return entry;
}

static void states_remove_slot(struct xgpu_rig_states *states, uint32_t slot)
{
	uint32_t mask = states->capacity - 1, next;

	free(states->entries[slot].springs);
	memset(&states->entries[slot], 0, sizeof(states->entries[slot]));
	states->count--;
	/* the entries after it in its run moved back (linear probing's removal) */
	next = (slot + 1) & mask;
	while (states->entries[next].key)
	{
		uint32_t home = key_slot(states->entries[next].key, states->capacity);

		/* it may move back into the gap if its home is not after the gap */
		if (((next - home) & mask) >= ((next - slot) & mask))
		{
			states->entries[slot] = states->entries[next];
			memset(&states->entries[next], 0, sizeof(states->entries[next]));
			slot = next;
		}
		next = (next + 1) & mask;
	}
}

void xgpu_rig_states_age(struct xgpu_rig_states *states, double game_seconds)
{
	uint32_t index = 0;

	while (index < states->capacity)
	{
		struct state_entry *entry = &states->entries[index];

		if (entry->key && (game_seconds - entry->seen > XGPU_RIG_STATE_FORGET_SECONDS || game_seconds < entry->seen))
		{
			states_remove_slot(states, index);
			continue; /* an entry may have moved into this slot */
		}
		index++;
	}
}

/* ---------- evaluation */

static double clamp01(double value)
{
	return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

/* the source's rotation away from its bind pose relative to its parent, in
its own bind frame (delta), with its parent's world rotation and its bind
rotation relative to its parent: its world rotation is parent rest delta */
static void local_delta(const struct xgpu_rig *rig, const double (*rotations)[9], int source, double delta[9],
	double parent_rotation[9], double rest[9])
{
	int parent = rig->bones[source].parent;
	double relative[9], transposed[9];
	const double *source_rotation = rotations[source];

	memcpy(rest, rig->rest_rotation[source], 9 * sizeof(double));
	if (parent >= 0)
		memcpy(parent_rotation, rotations[parent], 9 * sizeof(double));
	else
		/* a root: relative to the world, its bind world its rest */
		memcpy(parent_rotation, (double[9]){1, 0, 0, 0, 1, 0, 0, 0, 1}, 9 * sizeof(double));
	rotation_transpose(parent_rotation, transposed);
	rotation_multiply(transposed, source_rotation, relative);
	rotation_transpose(rest, transposed);
	rotation_multiply(transposed, relative, delta);
}

static void unit_axis(int axis, double out[3])
{
	out[0] = axis == 0;
	out[1] = axis == 1;
	out[2] = axis == 2;
}

/* conjugated: frame r frame^-1 */
static void rotation_conjugate(const double frame[9], const double r[9], double out[9])
{
	double transposed[9], product[9];

	rotation_transpose(frame, transposed);
	rotation_multiply(frame, r, product);
	rotation_multiply(product, transposed, out);
}

static void evaluate_copy_rotation(const struct xgpu_rig *rig, const struct xgpu_rig_helper *helper,
	const double (*rotations)[9], float m[12])
{
	double delta[9], parent[9], rest[9], frame[9], q[4], part[9], world_delta[9];

	local_delta(rig, rotations, helper->source, delta, parent, rest);
	if (helper->space == XGPU_RIG_SPACE_PARENT)
	{
		/* the same rotation in the parent's axes: rest delta rest^-1 */
		double in_parent[9];

		rotation_conjugate(rest, delta, in_parent);
		memcpy(delta, in_parent, sizeof(delta));
		memcpy(frame, parent, sizeof(frame));
	}
	else
		/* the source's frame at its bind pose relative to its parent */
		rotation_multiply(parent, rest, frame);
	if (helper->axes == 7)
	{
		quaternion_from_rotation(delta, q);
		quaternion_fraction(q, helper->fraction, part);
	}
	else
	{
		double angles[3];

		euler_from_rotation(delta, angles);
		angles[0] = (helper->axes & 1) ? angles[0] * helper->fraction : 0.0;
		angles[1] = (helper->axes & 2) ? angles[1] * helper->fraction : 0.0;
		angles[2] = (helper->axes & 4) ? angles[2] * helper->fraction : 0.0;
		rotation_from_euler(angles, part);
	}
	rotation_conjugate(frame, part, world_delta);
	matrix_turn(m, world_delta);
}

/* the twist of child relative to source about the child's axis (rest: the
child's bind rotation relative to the source's) */
static double source_child_twist(const double (*rotations)[9], const double rest_rotation[9], int source, int child,
	int axis)
{
	double relative[9], delta[9], transposed[9], q[4], unit[3];

	rotation_transpose(rotations[source], transposed);
	rotation_multiply(transposed, rotations[child], relative);
	rotation_transpose(rest_rotation, transposed);
	rotation_multiply(transposed, relative, delta);
	quaternion_from_rotation(delta, q);
	unit_axis(axis, unit);
	return twist_angle(q, unit);
}

static void own_axis(const float m[12], int axis, double out[3])
{
	double length;

	matrix_column(m, axis, out);
	length = sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if (length > 1e-20)
		out[0] /= length, out[1] /= length, out[2] /= length;
}

static void evaluate_twist(const struct xgpu_rig_helper *helper, const double (*rotations)[9],
	const double rest_rotation[9], float m[12])
{
	double angle = source_child_twist(rotations, rest_rotation, helper->source, helper->target, helper->axis);
	double axis[3], r[9];

	own_axis(m, helper->axes, axis);
	rotation_from_axis_angle(axis, angle * helper->fraction, r);
	matrix_turn(m, r);
}

static void evaluate_aim(const struct xgpu_rig_helper *helper, float (*world)[12],
	const struct xgpu_rig_instance *instance, float m[12])
{
	double head[3], target[3], direction[3], pointing[3], r[9], limited[9];

	matrix_translation(m, head);
	if (helper->target == XGPU_RIG_TARGET_CAMERA)
		target[0] = instance->camera[0], target[1] = instance->camera[1], target[2] = instance->camera[2];
	else
		matrix_translation(world[helper->target], target);
	direction[0] = target[0] - head[0];
	direction[1] = target[1] - head[1];
	direction[2] = target[2] - head[2];
	if (direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2] < 1e-14)
		return;
	own_axis(m, helper->axis, pointing);
	if (helper->type == XGPU_RIG_LOOK_AT)
	{
		/* a frame whose axis points at the target and whose up axis is as
		near as can be to the follow frame's */
		double up[3], length, frame[3][3], current[9], wanted[9], transposed[9];
		int third = 3 - helper->axis - helper->axes, column;
		/* (axis, up, third) a right-handed order of x, y, z, or not */
		int right_handed = ((helper->axis + 1) % 3) == helper->axes;

		own_axis(m, helper->axes, up);
		length = sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
		frame[0][0] = direction[0] / length, frame[0][1] = direction[1] / length, frame[0][2] = direction[2] / length;
		{
			double along = up[0] * frame[0][0] + up[1] * frame[0][1] + up[2] * frame[0][2];

			frame[1][0] = up[0] - along * frame[0][0];
			frame[1][1] = up[1] - along * frame[0][1];
			frame[1][2] = up[2] - along * frame[0][2];
		}
		length = sqrt(frame[1][0] * frame[1][0] + frame[1][1] * frame[1][1] + frame[1][2] * frame[1][2]);
		if (length > 1e-6)
		{
			double s = right_handed ? 1.0 : -1.0;

			frame[1][0] /= length, frame[1][1] /= length, frame[1][2] /= length;
			frame[2][0] = s * (frame[0][1] * frame[1][2] - frame[0][2] * frame[1][1]);
			frame[2][1] = s * (frame[0][2] * frame[1][0] - frame[0][0] * frame[1][2]);
			frame[2][2] = s * (frame[0][0] * frame[1][1] - frame[0][1] * frame[1][0]);
			for (column = 0; column < 3; column++)
			{
				int which = column == helper->axis ? 0 : column == helper->axes ? 1 : 2;

				wanted[column] = frame[which][0];
				wanted[3 + column] = frame[which][1];
				wanted[6 + column] = frame[which][2];
			}
			(void)third;
			rotation_of(m, current);
			rotation_transpose(current, transposed);
			rotation_multiply(wanted, transposed, r);
			rotation_limit_fraction(r, helper->values[0], helper->fraction, limited);
			matrix_turn(m, limited);
			return;
		}
	}
	rotation_between(pointing, direction, r);
	rotation_limit_fraction(r, helper->values[0], helper->fraction, limited);
	matrix_turn(m, limited);
}

static void evaluate_driven(const struct xgpu_rig *rig, const struct xgpu_rig_helper *helper, float (*world)[12],
	const double (*rotations)[9], const struct xgpu_rig_instance *instance, float m[12])
{
	double value = 0.0, t, angles[3], r[9];
	float offset[12];
	int row;

	switch (helper->driver)
	{
	case XGPU_RIG_DRIVER_ANGLE:
	{
		double delta[9], parent[9], rest[9], q[4], unit[3];

		local_delta(rig, rotations, helper->source, delta, parent, rest);
		quaternion_from_rotation(delta, q);
		unit_axis(helper->axis, unit);
		value = twist_angle(q, unit);
		break;
	}
	case XGPU_RIG_DRIVER_FUNCTION:
		value = instance->functions[helper->function];
		break;
	default:
	{
		double a[3], b[3];

		matrix_translation(world[helper->source], a);
		matrix_translation(world[helper->target], b);
		value = sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
		break;
	}
	}
	t = clamp01((value - helper->values[0]) / ((double)helper->values[1] - helper->values[0]));
	if (helper->curve == XGPU_RIG_CURVE_SMOOTHSTEP)
		t = t * t * (3.0 - 2.0 * t);
	angles[0] = helper->values[5] * t;
	angles[1] = helper->values[6] * t;
	angles[2] = helper->values[7] * t;
	rotation_from_euler(angles, r);
	for (row = 0; row < 3; row++)
	{
		offset[row * 4 + 0] = (float)(r[row * 3 + 0] * (1.0 + (helper->values[8] - 1.0) * t));
		offset[row * 4 + 1] = (float)(r[row * 3 + 1] * (1.0 + (helper->values[9] - 1.0) * t));
		offset[row * 4 + 2] = (float)(r[row * 3 + 2] * (1.0 + (helper->values[10] - 1.0) * t));
		offset[row * 4 + 3] = (float)(helper->values[2 + row] * t);
	}
	matrix_multiply(m, offset, m);
}

static void spring_reset(struct spring_state *spring, const double tip[3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		spring->p[axis] = spring->previous[axis] = spring->target[axis] = spring->step_target[axis] = tip[axis];
		spring->v[axis] = 0.0;
	}
}

/* the step a time falls in: ticks are whole steps, a little under one
counts as it */
static int64_t step_of(double seconds)
{
	return (int64_t)floor(seconds / XGPU_RIG_SPRING_STEP + 1e-6);
}

static void evaluate_spring(const struct xgpu_rig_helper *helper, struct spring_state *spring, int reset, int skipped,
	int64_t steps_from, int64_t steps_to, double target_seconds, const struct xgpu_rig_instance *instance, float m[12])
{
	double head[3], along[3], tip[3], out[3], from[3], to[3], r[9], limited[9], length = helper->values[4];
	double gravity = helper->values[2] * XGPU_RIG_GRAVITY, alpha;
	int64_t step;
	int axis;

	if (helper->function != 0xff)
	{
		double drive = instance->functions[helper->function], angles[3], d[9], basis[9];

		angles[0] = helper->values[5] * drive;
		angles[1] = helper->values[6] * drive;
		angles[2] = helper->values[7] * drive;
		/* in the helper's own frame: m's rotation times the drive's */
		rotation_from_euler(angles, d);
		rotation_of(m, basis);
		{
			double world_drive[9];

			rotation_conjugate(basis, d, world_drive);
			matrix_turn(m, world_drive);
		}
	}
	matrix_translation(m, head);
	own_axis(m, helper->axis, along);
	for (axis = 0; axis < 3; axis++)
		tip[axis] = head[axis] + along[axis] * length;
	if (!reset)
	{
		double jump = 0.0;

		for (axis = 0; axis < 3; axis++)
			jump += (tip[axis] - spring->target[axis]) * (tip[axis] - spring->target[axis]);
		reset = jump > (double)XGPU_RIG_SPRING_JUMP * XGPU_RIG_SPRING_JUMP;
	}
	if (reset || (instance->flags & XGPU_RIG_INSTANCE_NO_SECONDARY))
	{
		spring_reset(spring, tip);
		return;
	}
	for (step = steps_from + 1; step <= steps_to; step++)
	{
		/* the target where it was at this step's time, between the last
		frame's sample and this one's */
		double s = instance->game_seconds > target_seconds ?
			clamp01((step * XGPU_RIG_SPRING_STEP - target_seconds) / (instance->game_seconds - target_seconds)) : 1.0;
		double h = XGPU_RIG_SPRING_STEP;

		for (axis = 0; axis < 3; axis++)
		{
			/* damped against the target's own velocity: a body moving steadily
			carries the tip along without lag, its accelerations swing it */
			double target = spring->target[axis] + (tip[axis] - spring->target[axis]) * s;
			double target_velocity;

			/* after a gap past the steps a frame may take, no velocity from
			across it */
			if (skipped && step == steps_from + 1)
				spring->step_target[axis] = target;
			target_velocity = (target - spring->step_target[axis]) / h;
			double acceleration = -helper->values[0] * (spring->p[axis] - target) -
				helper->values[1] * (spring->v[axis] - target_velocity);

			if (axis == 2)
				acceleration -= gravity;
			spring->previous[axis] = spring->p[axis];
			spring->step_target[axis] = target;
			spring->v[axis] += h * acceleration;
			spring->p[axis] += h * spring->v[axis];
		}
	}
	memcpy(spring->target, tip, sizeof(tip));
	/* drawn between the last two steps */
	alpha = clamp01((instance->game_seconds - steps_to * XGPU_RIG_SPRING_STEP) / XGPU_RIG_SPRING_STEP);
	for (axis = 0; axis < 3; axis++)
	{
		out[axis] = spring->previous[axis] + (spring->p[axis] - spring->previous[axis]) * alpha;
		from[axis] = tip[axis] - head[axis];
		to[axis] = out[axis] - head[axis];
	}
	rotation_between(from, to, r);
	rotation_limit_fraction(r, helper->values[3], 1.0, limited);
	matrix_turn(m, limited);
}

enum xgpu_rig_status xgpu_rig_evaluate(const struct xgpu_rig *rig, struct xgpu_rig_states *states,
	const struct xgpu_rig_instance *instance, float (*palette)[12], float (*world_out)[12])
{
	float world_local[64][12];
	double rotations_local[64][9];
	float (*world)[12] = world_out;
	double (*rotations)[9] = rotations_local;
	struct state_entry *entry = NULL;
	int64_t steps_from = 0, steps_to = 0;
	int reset = 1, skipped = 0;
	uint32_t index;

	if (!instance->ce_matrices || instance->ce_matrix_count < rig->ce_node_count)
		return XGPU_RIG_NODES;
	/* the frames' rotations (and the worlds, if not given): on the stack, or
	the rig's own past 64 bones */
	if (rig->bone_count > 64)
		rotations = rig->scratch_rotations;
	if (!world)
		world = rig->bone_count > 64 ? rig->scratch_world : world_local;
	for (index = 0; index < rig->ce_node_count; index++)
	{
		memcpy(palette[index], instance->ce_matrices[index], sizeof(palette[index]));
		matrix_multiply(instance->ce_matrices[index], rig->bones[index].bind_world, world[index]);
		rotation_of(world[index], rotations[index]);
	}
	if (rig->spring_count && states && instance->key)
	{
		entry = states_find(states, instance->key, rig->spring_count);
		if (entry && entry->springs)
		{
			int64_t now = step_of(instance->game_seconds);

			reset = entry->fresh || (instance->flags & XGPU_RIG_INSTANCE_TELEPORT) ||
				instance->game_seconds < entry->target_seconds ||
				instance->game_seconds - entry->seen > XGPU_RIG_STATE_FORGET_SECONDS;
			steps_from = reset ? now : entry->step;
			if (now - steps_from > (int64_t)XGPU_RIG_SPRING_STEPS_MAXIMUM)
			{
				steps_from = now - XGPU_RIG_SPRING_STEPS_MAXIMUM;
				skipped = 1;
			}
			steps_to = now;
		}
		else
			entry = NULL;
	}
	for (index = 0; index < rig->order_count; index++)
	{
		const struct xgpu_rig_helper *helper = &rig->order[index];
		int bone = helper->bone, parent = rig->bones[bone].parent;
		float *m = world[bone];

		/* carried by its parent (the root's: by CE node 0's skin) */
		if (parent >= 0)
			matrix_multiply(world[parent], rig->rest_local[bone], m);
		else
			matrix_multiply(palette[0], rig->bones[bone].bind_world, m);
		switch (helper->type)
		{
		case XGPU_RIG_COPY_ROTATION:
			evaluate_copy_rotation(rig, helper, (const double (*)[9])rotations, m);
			break;
		case XGPU_RIG_TWIST:
			evaluate_twist(helper, (const double (*)[9])rotations, rig->twist_rest[index], m);
			break;
		case XGPU_RIG_AIM:
		case XGPU_RIG_LOOK_AT:
			evaluate_aim(helper, world, instance, m);
			break;
		case XGPU_RIG_DRIVEN:
			if (!(instance->flags & XGPU_RIG_INSTANCE_NO_SECONDARY))
				evaluate_driven(rig, helper, world, (const double (*)[9])rotations, instance, m);
			break;
		case XGPU_RIG_SPRING:
			if (entry)
				evaluate_spring(helper, &entry->springs[rig->spring_index[index]], reset, skipped, steps_from, steps_to,
					entry->target_seconds, instance, m);
			break;
		default:
			break;
		}
		matrix_multiply(m, rig->bones[bone].inverse_bind, palette[bone]);
		rotation_of(m, rotations[bone]);
	}
	if (entry)
	{
		entry->step = steps_to;
		entry->seen = instance->game_seconds;
		entry->target_seconds = instance->game_seconds;
		entry->fresh = 0;
	}
	return XGPU_RIG_OK;
}

void xgpu_rig_skin_reference(const float (*palette)[12], uint32_t bone_count, const float (*positions)[3],
	const float (*normals)[3], const uint16_t (*joints)[4], const uint8_t (*weights)[4], uint32_t vertex_count,
	float (*out_positions)[3], float (*out_normals)[3])
{
	uint32_t vertex;

	for (vertex = 0; vertex < vertex_count; vertex++)
	{
		float blend[12] = {0};
		int influence, element;
		float length;

		for (influence = 0; influence < 4; influence++)
		{
			uint16_t joint = joints[vertex][influence];
			float weight = weights[vertex][influence] * (1.0f / 255.0f);

			if (!weights[vertex][influence] || joint >= bone_count)
				continue;
			for (element = 0; element < 12; element++)
				blend[element] += palette[joint][element] * weight;
		}
		for (element = 0; element < 3; element++)
		{
			const float *row = blend + element * 4;

			out_positions[vertex][element] = row[0] * positions[vertex][0] + row[1] * positions[vertex][1] +
				row[2] * positions[vertex][2] + row[3];
			if (normals && out_normals)
				out_normals[vertex][element] = row[0] * normals[vertex][0] + row[1] * normals[vertex][1] +
					row[2] * normals[vertex][2];
		}
		if (normals && out_normals)
		{
			length = sqrtf(out_normals[vertex][0] * out_normals[vertex][0] + out_normals[vertex][1] * out_normals[vertex][1] +
				out_normals[vertex][2] * out_normals[vertex][2]);
			if (length > 0.0f)
				for (element = 0; element < 3; element++)
					out_normals[vertex][element] /= length;
		}
	}
}

/* ---------- .rig.json */

struct json
{
	const char *text;
	jsmntok_t *tokens;
	int count;
};

/* the token after token and everything inside it */
static int json_skip(const struct json *json, int token)
{
	int end = json->tokens[token].end, next = token + 1;

	while (next < json->count && json->tokens[next].start < end)
		next++;
	return next;
}

static int json_equals(const struct json *json, int token, const char *string)
{
	const jsmntok_t *t = &json->tokens[token];
	size_t length = strlen(string);

	return t->type == JSMN_STRING && (size_t)(t->end - t->start) == length &&
		memcmp(json->text + t->start, string, length) == 0;
}

static int json_number(const struct json *json, int token, double *value)
{
	const jsmntok_t *t = &json->tokens[token];
	char text[64], *end;
	int length = t->end - t->start;

	if (t->type != JSMN_PRIMITIVE || length <= 0 || length >= (int)sizeof(text))
		return 0;
	memcpy(text, json->text + t->start, (size_t)length);
	text[length] = 0;
	if (!((text[0] >= '0' && text[0] <= '9') || text[0] == '-'))
		return 0;
	*value = strtod(text, &end);
	return *end == 0 && isfinite(*value);
}

static int json_numbers(const struct json *json, int token, double *values, int count)
{
	int index, element;

	if (json->tokens[token].type != JSMN_ARRAY || json->tokens[token].size != count)
		return 0;
	element = token + 1;
	for (index = 0; index < count; index++)
	{
		if (!json_number(json, element, &values[index]))
			return 0;
		element = json_skip(json, element);
	}
	return 1;
}

static int json_axis(const struct json *json, int token, uint8_t *axis)
{
	if (json_equals(json, token, "x"))
		*axis = 0;
	else if (json_equals(json, token, "y"))
		*axis = 1;
	else if (json_equals(json, token, "z"))
		*axis = 2;
	else
		return 0;
	return 1;
}

static int json_function(const struct json *json, int token, uint8_t *function)
{
	const jsmntok_t *t = &json->tokens[token];
	char c;

	if (t->type != JSMN_STRING || t->end - t->start != 1)
		return 0;
	c = json->text[t->start];
	if (c >= 'a' && c <= 'd')
		c = (char)(c - 'a' + 'A');
	if (c < 'A' || c > 'D')
		return 0;
	*function = (uint8_t)(c - 'A');
	return 1;
}

/* the bone a string names: -1 none, -2 two of that name's hash */
static int json_bone(const struct json *json, int token, const struct xgpu_rig_bone *bones, uint32_t bone_count)
{
	const jsmntok_t *t = &json->tokens[token];
	uint32_t hash, index;
	int found = -1;

	if (t->type != JSMN_STRING)
		return -1;
	hash = xgpu_rig_name_hash(json->text + t->start, (size_t)(t->end - t->start));
	for (index = 0; index < bone_count; index++)
		if (bones[index].name_hash == hash)
		{
			if (found >= 0)
				return -2;
			found = (int)index;
		}
	return found;
}

/* a bone a rule names: its number, else -1 with *unknown set when no bone
has that name (not when the name is not a string or two bones share it) */
static int json_reference(const struct json *json, int token, const struct xgpu_rig_bone *bones, uint32_t bone_count,
	int *unknown)
{
	int bone = json_bone(json, token, bones, bone_count);

	if (bone == -1 && json->tokens[token].type == JSMN_STRING)
		*unknown = 1;
	return bone < 0 ? -1 : bone;
}

static enum xgpu_rig_status compile_helper(const struct json *json, int object, const struct xgpu_rig_bone *bones,
	uint32_t bone_count, uint32_t ce_node_count, struct xgpu_rig_helper *helper, char *error, size_t error_size)
{
	int key, keys = json->tokens[object].size, pair;
	int type = -1, bone = -1, helper_axis = 0, unknown = 0;
	double number, values[3];
	char name[64];

	memset(helper, 0, sizeof(*helper));
	helper->source = helper->target = -1;
	helper->function = 0xff;
	helper->fraction = 1.0f;
	helper->axes = 7;
	helper->values[8] = helper->values[9] = helper->values[10] = 1.0f; /* driven's scale */
	/* the type first: the defaults of a spring */
	key = object + 1;
	for (pair = 0; pair < keys; pair++)
	{
		int value = key + 1;

		if (json_equals(json, key, "type"))
		{
			static const char *names[XGPU_RIG_HELPER_TYPES] = {
				"follow", "copy_rotation", "twist", "aim", "look_at", "driven", "spring"};
			int index;

			for (index = 0; index < XGPU_RIG_HELPER_TYPES; index++)
				if (json_equals(json, value, names[index]))
					type = index;
		}
		if (json_equals(json, key, "bone"))
			bone = json_bone(json, value, bones, bone_count);
		key = json_skip(json, value);
	}
	if (type < 0)
		return fail(XGPU_RIG_FORMAT, error, error_size, "a helper of no known type");
	if (bone < 0)
		return fail(bone == -2 ? XGPU_RIG_FORMAT : XGPU_RIG_UNKNOWN_BONE, error, error_size, "a helper's bone");
	if ((uint32_t)bone < ce_node_count)
		return fail(XGPU_RIG_FORMAT, error, error_size, "bone %d is a CE node", bone);
	helper->type = (uint8_t)type;
	helper->bone = (uint16_t)bone;
	if (type == XGPU_RIG_SPRING)
	{
		helper->values[0] = 100.0f;
		helper->values[1] = 10.0f;
		helper->values[4] = 0.05f;
	}
	key = object + 1;
	for (pair = 0; pair < keys; pair++)
	{
		int value = key + 1, ok = 1;
		const jsmntok_t *k = &json->tokens[key];

		snprintf(name, sizeof(name), "%.*s", k->end - k->start, json->text + k->start);
		if (json_equals(json, key, "type") || json_equals(json, key, "bone") || json_equals(json, key, "comment"))
			;
		else if (json_equals(json, key, "source") && (type == XGPU_RIG_COPY_ROTATION || type == XGPU_RIG_TWIST))
			ok = (helper->source = (int16_t)json_reference(json, value, bones, bone_count, &unknown)) >= 0;
		else if (json_equals(json, key, "child") && type == XGPU_RIG_TWIST)
			ok = (helper->target = (int16_t)json_reference(json, value, bones, bone_count, &unknown)) >= 0;
		else if (json_equals(json, key, "target") && (type == XGPU_RIG_AIM || type == XGPU_RIG_LOOK_AT))
		{
			if (json_equals(json, value, "camera"))
				helper->target = XGPU_RIG_TARGET_CAMERA;
			else
				ok = (helper->target = (int16_t)json_reference(json, value, bones, bone_count, &unknown)) >= 0;
		}
		else if (json_equals(json, key, "parent") && type == XGPU_RIG_SPRING)
			/* the skeleton's own parent, named again */
			ok = json_reference(json, value, bones, bone_count, &unknown) == bones[bone].parent;
		else if (json_equals(json, key, "axis"))
			ok = json_axis(json, value, &helper->axis);
		else if (json_equals(json, key, "up") && type == XGPU_RIG_LOOK_AT)
			ok = json_axis(json, value, &helper->axes);
		else if (json_equals(json, key, "helper_axis") && type == XGPU_RIG_TWIST)
		{
			ok = json_axis(json, value, &helper->axes);
			helper_axis = 1;
		}
		else if (json_equals(json, key, "axes") && type == XGPU_RIG_COPY_ROTATION)
		{
			const jsmntok_t *t = &json->tokens[value];
			int index;

			helper->axes = 0;
			ok = t->type == JSMN_STRING && t->end > t->start;
			for (index = t->start; ok && index < t->end; index++)
			{
				char c = json->text[index];
				uint8_t bit = c == 'x' ? 1 : c == 'y' ? 2 : c == 'z' ? 4 : 0;

				ok = bit && !(helper->axes & bit);
				helper->axes |= bit;
			}
		}
		else if (json_equals(json, key, "space") && type == XGPU_RIG_COPY_ROTATION)
		{
			if (json_equals(json, value, "local"))
				helper->space = XGPU_RIG_SPACE_LOCAL;
			else if (json_equals(json, value, "parent"))
				helper->space = XGPU_RIG_SPACE_PARENT;
			else
				ok = 0;
		}
		else if (json_equals(json, key, "fraction") &&
			(type == XGPU_RIG_COPY_ROTATION || type == XGPU_RIG_TWIST || type == XGPU_RIG_AIM || type == XGPU_RIG_LOOK_AT))
		{
			ok = json_number(json, value, &number);
			helper->fraction = (float)number;
		}
		else if (json_equals(json, key, "limit_deg") && (type == XGPU_RIG_SPRING || type == XGPU_RIG_AIM ||
			type == XGPU_RIG_LOOK_AT))
		{
			ok = json_number(json, value, &number) && number >= 0.0;
			helper->values[type == XGPU_RIG_SPRING ? 3 : 0] = (float)(number * (PI / 180.0));
		}
		else if (type == XGPU_RIG_SPRING && (json_equals(json, key, "stiffness") || json_equals(json, key, "damping") ||
			json_equals(json, key, "gravity") || json_equals(json, key, "length")))
		{
			int slot = json_equals(json, key, "stiffness") ? 0 : json_equals(json, key, "damping") ? 1 :
				json_equals(json, key, "gravity") ? 2 : 4;

			ok = json_number(json, value, &number);
			helper->values[slot] = (float)number;
		}
		else if (type == XGPU_RIG_SPRING && json_equals(json, key, "drive"))
		{
			int inner = value + 1, index;

			ok = json->tokens[value].type == JSMN_OBJECT;
			for (index = 0; ok && index < json->tokens[value].size; index++)
			{
				if (json_equals(json, inner, "function"))
					ok = json_function(json, inner + 1, &helper->function);
				else if (json_equals(json, inner, "rotate_deg"))
				{
					ok = json_numbers(json, inner + 1, values, 3);
					helper->values[5] = (float)(values[0] * (PI / 180.0));
					helper->values[6] = (float)(values[1] * (PI / 180.0));
					helper->values[7] = (float)(values[2] * (PI / 180.0));
				}
				else
					ok = 0;
				inner = json_skip(json, inner + 1);
			}
			ok = ok && helper->function != 0xff;
		}
		else if (type == XGPU_RIG_DRIVEN && json_equals(json, key, "driver"))
		{
			int inner = value + 1, index, angle = 0;

			ok = json->tokens[value].type == JSMN_OBJECT;
			for (index = 0; ok && index < json->tokens[value].size; index++)
			{
				if (json_equals(json, inner, "bone"))
				{
					ok = (helper->source = (int16_t)json_reference(json, inner + 1, bones, bone_count, &unknown)) >= 0;
					helper->driver = XGPU_RIG_DRIVER_ANGLE;
				}
				else if (json_equals(json, inner, "angle"))
					ok = json_axis(json, inner + 1, &helper->axis);
				else if (json_equals(json, inner, "function"))
				{
					ok = json_function(json, inner + 1, &helper->function);
					helper->driver = XGPU_RIG_DRIVER_FUNCTION;
				}
				else if (json_equals(json, inner, "distance"))
				{
					int first = inner + 2;

					ok = json->tokens[inner + 1].type == JSMN_ARRAY && json->tokens[inner + 1].size == 2;
					if (ok)
					{
						helper->source = (int16_t)json_reference(json, first, bones, bone_count, &unknown);
						helper->target = (int16_t)json_reference(json, json_skip(json, first), bones, bone_count, &unknown);
						ok = helper->source >= 0 && helper->target >= 0;
					}
					helper->driver = XGPU_RIG_DRIVER_DISTANCE;
				}
				else if (json_equals(json, inner, "range_deg"))
				{
					ok = json_numbers(json, inner + 1, values, 2);
					helper->values[0] = (float)(values[0] * (PI / 180.0));
					helper->values[1] = (float)(values[1] * (PI / 180.0));
					angle = 1;
				}
				else if (json_equals(json, inner, "range"))
				{
					ok = json_numbers(json, inner + 1, values, 2);
					helper->values[0] = (float)values[0];
					helper->values[1] = (float)values[1];
				}
				else
					ok = 0;
				inner = json_skip(json, inner + 1);
			}
			/* an angle's range in degrees, the others' in their own units */
			ok = ok && angle == (helper->driver == XGPU_RIG_DRIVER_ANGLE);
		}
		else if (type == XGPU_RIG_DRIVEN && json_equals(json, key, "translate"))
		{
			ok = json_numbers(json, value, values, 3);
			helper->values[2] = (float)values[0], helper->values[3] = (float)values[1], helper->values[4] = (float)values[2];
		}
		else if (type == XGPU_RIG_DRIVEN && json_equals(json, key, "rotate_deg"))
		{
			ok = json_numbers(json, value, values, 3);
			helper->values[5] = (float)(values[0] * (PI / 180.0));
			helper->values[6] = (float)(values[1] * (PI / 180.0));
			helper->values[7] = (float)(values[2] * (PI / 180.0));
		}
		else if (type == XGPU_RIG_DRIVEN && json_equals(json, key, "scale"))
		{
			ok = json_numbers(json, value, values, 3);
			helper->values[8] = (float)values[0], helper->values[9] = (float)values[1], helper->values[10] = (float)values[2];
		}
		else if (type == XGPU_RIG_DRIVEN && json_equals(json, key, "curve"))
		{
			if (json_equals(json, value, "linear"))
				helper->curve = XGPU_RIG_CURVE_LINEAR;
			else if (json_equals(json, value, "smoothstep"))
				helper->curve = XGPU_RIG_CURVE_SMOOTHSTEP;
			else
				ok = 0;
		}
		else
			return fail(XGPU_RIG_FORMAT, error, error_size, "helper of bone %d: \"%s\" not known for its type", bone, name);
		if (!ok)
			return fail(unknown ? XGPU_RIG_UNKNOWN_BONE : XGPU_RIG_FORMAT, error, error_size,
				"helper of bone %d: \"%s\"%s", bone, name, unknown ? " names no bone" : "");
		key = json_skip(json, value);
	}
	/* a twist turns the helper about the same axis as the child's unless
	told otherwise */
	if (type == XGPU_RIG_TWIST && !helper_axis)
		helper->axes = helper->axis;
	return XGPU_RIG_OK;
}

enum xgpu_rig_status xgpu_rig_compile_json(const struct xgpu_rig_bone *bones, uint32_t bone_count,
	uint32_t ce_node_count, const char *text, size_t length, void **output, size_t *output_size,
	char *error, size_t error_size)
{
	jsmn_parser parser;
	struct json json;
	struct xgpu_rig_header header;
	struct xgpu_rig_helper *helpers = NULL;
	enum xgpu_rig_status status;
	int token, pair, version = 0, helpers_token = -1, count = 0, index;
	unsigned char *bytes;

	*output = NULL;
	*output_size = 0;
	status = check_skeleton(bones, bone_count, ce_node_count, error, error_size);
	if (status != XGPU_RIG_OK)
		return status;
	jsmn_init(&parser);
	json.text = text;
	json.count = jsmn_parse(&parser, text, length, NULL, 0);
	if (json.count < 1)
		return fail(XGPU_RIG_FORMAT, error, error_size, "not JSON");
	json.tokens = malloc((size_t)json.count * sizeof(*json.tokens));
	if (!json.tokens)
		return fail(XGPU_RIG_FORMAT, error, error_size, "out of memory");
	jsmn_init(&parser);
	status = XGPU_RIG_FORMAT;
	if (jsmn_parse(&parser, text, length, json.tokens, (unsigned int)json.count) != json.count)
	{
		fail(status, error, error_size, "not JSON");
		goto out;
	}
	if (json.tokens[0].type != JSMN_OBJECT)
	{
		fail(status, error, error_size, "not an object");
		goto out;
	}
	token = 1;
	for (pair = 0; pair < json.tokens[0].size; pair++)
	{
		double number;

		if (json_equals(&json, token, "version") && json_number(&json, token + 1, &number))
			version = (int)number;
		else if (json_equals(&json, token, "helpers") && json.tokens[token + 1].type == JSMN_ARRAY)
			helpers_token = token + 1;
		else if (!json_equals(&json, token, "comment") && !json_equals(&json, token, "skeleton"))
		{
			fail(status, error, error_size, "\"%.*s\" not known", json.tokens[token].end - json.tokens[token].start,
				text + json.tokens[token].start);
			goto out;
		}
		token = json_skip(&json, token + 1);
	}
	if (version != 1 || helpers_token < 0)
	{
		fail(status, error, error_size, "not version 1, or no helpers");
		goto out;
	}
	count = json.tokens[helpers_token].size;
	if (count > (int)XGPU_RIG_HELPERS_MAXIMUM)
	{
		fail(status, error, error_size, "%d helpers", count);
		goto out;
	}
	helpers = calloc((size_t)count + 1, sizeof(*helpers));
	if (!helpers)
	{
		fail(status, error, error_size, "out of memory");
		goto out;
	}
	token = helpers_token + 1;
	for (index = 0; index < count; index++)
	{
		if (json.tokens[token].type != JSMN_OBJECT)
		{
			fail(status, error, error_size, "helper %d not an object", index);
			goto out;
		}
		status = compile_helper(&json, token, bones, bone_count, ce_node_count, &helpers[index], error, error_size);
		if (status != XGPU_RIG_OK)
			goto out;
		status = XGPU_RIG_FORMAT;
		token = json_skip(&json, token);
	}
	header.magic = XGPU_RIG_MAGIC;
	header.version = XGPU_RIG_VERSION;
	header.helper_count = (uint16_t)count;
	header.bone_count = bone_count;
	header.ce_node_count = ce_node_count;
	bytes = malloc(sizeof(header) + (size_t)count * sizeof(*helpers));
	if (!bytes)
	{
		fail(status, error, error_size, "out of memory");
		goto out;
	}
	memcpy(bytes, &header, sizeof(header));
	memcpy(bytes + sizeof(header), helpers, (size_t)count * sizeof(*helpers));
	*output = bytes;
	*output_size = sizeof(header) + (size_t)count * sizeof(*helpers);
	/* the rules checked as the rig will (types' fields, cycles) */
	{
		struct xgpu_rig *rig = xgpu_rig_create(bones, bone_count, ce_node_count, bytes, *output_size, &status,
			error, error_size);

		xgpu_rig_destroy(rig);
		if (status != XGPU_RIG_OK)
		{
			free(bytes);
			*output = NULL;
			*output_size = 0;
			goto out;
		}
	}
	status = XGPU_RIG_OK;
out:
	free(helpers);
	free(json.tokens);
	return status;
}
