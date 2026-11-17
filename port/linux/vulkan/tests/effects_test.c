/*
EFFECTS_TEST.C

The VR mode's effects' arithmetic on the CPU (xgpu_vulkan_effects.c): the
thickness bake's distances to the edge of known shapes (a disc, a square,
two discs of different sizes on one sheet), its sphere profile, its mask
(the alpha, or an added effect's brightness), and each eye's transforms: a
world point through a view's clip transform and back through its
unprojection, and the eye found where the eye's camera is.
*/

#include "../xgpu_vulkan_effects.c"

#include <assert.h>
#include <stdio.h>

#define CLOSE(a, b, tolerance) (fabs((double)(a) - (double)(b)) <= (tolerance))

static uint32_t sheet[128 * 128];
static uint8_t baked[128 * 128 * 4];
static uint8_t inside[128 * 128];
static float squared[128 * 128];

static void disc(uint32_t *texels, uint32_t width, float cx, float cy, float radius, uint32_t texel)
{
	uint32_t x, y;

	for (y = 0; y < width; y++)
	{
		for (x = 0; x < width; x++)
		{
			float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;

			if (dx * dx + dy * dy < radius * radius)
				texels[y * width + x] = texel;
		}
	}
}

static float height_at(uint32_t width, uint32_t x, uint32_t y)
{
	return baked[(y * width + x) * 4] / 255.0f * XGPU_VULKAN_THICKNESS_UNIT;
}

static float radius_at(uint32_t width, uint32_t x, uint32_t y)
{
	return baked[(y * width + x) * 4 + 1] / 255.0f * XGPU_VULKAN_THICKNESS_UNIT;
}

static void distances(void)
{
	uint32_t x, y;

	/* a 9x5 rectangle inside a 16x16 sheet: each texel's distance to the
	nearest outside, by its rows and columns */
	memset(inside, 0, sizeof(inside));
	for (y = 4; y < 9; y++)
		for (x = 3; x < 12; x++)
			inside[y * 16 + x] = 1;
	assert(xgpu_vulkan_thickness_distances(inside, 16, 16, squared));
	assert(squared[0] == 0.0f);
	assert(squared[6 * 16 + 7] == 9.0f);      /* the middle row: 3 to the top or bottom */
	assert(squared[4 * 16 + 7] == 1.0f);      /* the top row */
	assert(squared[6 * 16 + 3] == 1.0f);      /* the left column */
	assert(squared[5 * 16 + 4] == 4.0f);      /* 2 in from the top and from the left */
	/* the bitmap's edges are outside: a full bitmap's middle is half its size in */
	memset(inside, 1, 16 * 16);
	assert(xgpu_vulkan_thickness_distances(inside, 16, 16, squared));
	assert(squared[0] == 1.0f && squared[7 * 16 + 7] == 64.0f && squared[8 * 16 + 8] == 64.0f);
	/* Euclidean, not by rows and columns: a single texel outside, a diagonal
	away */
	memset(inside, 1, 16 * 16);
	inside[5 * 16 + 5] = 0;
	assert(xgpu_vulkan_thickness_distances(inside, 16, 16, squared));
	assert(squared[8 * 16 + 9] == 9.0f + 16.0f);
}

static void hemisphere(void)
{
	const float radius = 24.0f;
	uint32_t x;

	/* a disc of radius 24 in the alpha: its middle a radius high, its
	sides a sphere's, its radius known everywhere on it */
	memset(sheet, 0, sizeof(sheet));
	disc(sheet, 64, 32.0f, 32.0f, radius, 0xffffffffu);
	assert(xgpu_vulkan_thickness_bake(sheet, 64, 64, baked));
	assert(CLOSE(height_at(64, 32, 32), radius - 0.5f, 1.5));
	assert(CLOSE(radius_at(64, 32, 32), radius, 1.0));
	assert(CLOSE(radius_at(64, 32, 32 - 20), radius, 1.0));
	for (x = 32; x < 32 + 20; x += 4)
	{
		float r = (float)x + 0.5f - 32.0f, expected = sqrtf(radius * radius - r * r);

		/* (the blur lowers the steep side a little) */
		assert(CLOSE(height_at(64, x, 32), expected, 2.0));
	}
	/* nothing outside, and its radius beside it */
	assert(height_at(64, 1, 1) == 0.0f);
	assert(height_at(64, 32 + 26, 32) == 0.0f);
	assert(CLOSE(radius_at(64, 32 + 25, 32), radius, 1.0));
	/* rising toward the middle, never above it */
	for (x = 33 + 20; x > 33; x--)
		assert(height_at(64, x - 1, 32) + 0.6f >= height_at(64, x, 32));
}

static void two_sprites(void)
{
	/* a sheet of two sprites, of radius 10 and 30: each its own radius,
	neither flattened by the other */
	memset(sheet, 0, sizeof(sheet));
	disc(sheet, 128, 20.0f, 20.0f, 10.0f, 0xff808080u);
	disc(sheet, 128, 80.0f, 80.0f, 30.0f, 0xff808080u);
	assert(xgpu_vulkan_thickness_bake(sheet, 128, 128, baked));
	assert(CLOSE(radius_at(128, 20, 20), 10.0f, 1.0));
	assert(CLOSE(radius_at(128, 80, 80), 30.0f, 1.0));
	assert(CLOSE(height_at(128, 20, 20), 9.5f, 1.5));
	assert(CLOSE(height_at(128, 80, 80), 29.5f, 1.5));
	/* far from both, the sheet's typical radius */
	assert(radius_at(128, 127, 0) > 10.0f && radius_at(128, 127, 0) < 30.0f);
}

static void masks(void)
{
	uint32_t index;

	/* an added effect's alpha is the same everywhere: its brightness is
	its shape */
	for (index = 0; index < 64 * 64; index++)
		sheet[index] = 0xff000000u;
	disc(sheet, 64, 32.0f, 32.0f, 16.0f, 0xffffc040u);
	assert(xgpu_vulkan_thickness_bake(sheet, 64, 64, baked));
	assert(CLOSE(height_at(64, 32, 32), 15.5f, 1.5));
	assert(height_at(64, 2, 2) == 0.0f);
	/* a faint sprite (a quarter of the alpha) is half as thick */
	memset(sheet, 0, sizeof(sheet));
	disc(sheet, 64, 32.0f, 32.0f, 16.0f, 0x40ffffffu);
	assert(xgpu_vulkan_thickness_bake(sheet, 64, 64, baked));
	assert(CLOSE(height_at(64, 32, 32), 15.5f * sqrtf(64.0f / 255.0f), 1.5));
	/* too large a bitmap is not baked */
	assert(!xgpu_vulkan_thickness_bake(sheet, XGPU_VULKAN_THICKNESS_LARGEST + 1, 1, baked));
}

/* ---------- each eye's transforms */

/* a camera at position looking along forward (unit), up roughly up, with
a symmetric perspective, as the game's clip rows (OpenGL's z, w the
distance ahead) */
static void camera_rows(float rows[4][4], const float position[3], const float forward[3], const float up_hint[3],
	float tangent, float near, float far, float shift)
{
	float right[3], up[3], a, b;
	int axis;

	right[0] = forward[1] * up_hint[2] - forward[2] * up_hint[1];
	right[1] = forward[2] * up_hint[0] - forward[0] * up_hint[2];
	right[2] = forward[0] * up_hint[1] - forward[1] * up_hint[0];
	{
		float length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);

		for (axis = 0; axis < 3; axis++)
			right[axis] /= length;
	}
	up[0] = right[1] * forward[2] - right[2] * forward[1];
	up[1] = right[2] * forward[0] - right[0] * forward[2];
	up[2] = right[0] * forward[1] - right[1] * forward[0];
	a = (far + near) / (far - near);
	b = -2.0f * far * near / (far - near);
	for (axis = 0; axis < 3; axis++)
	{
		rows[0][axis] = right[axis] / tangent + shift * forward[axis];
		rows[1][axis] = up[axis] / tangent;
		rows[2][axis] = a * forward[axis];
		rows[3][axis] = forward[axis];
	}
	rows[0][3] = -(rows[0][0] * position[0] + rows[0][1] * position[1] + rows[0][2] * position[2]);
	rows[1][3] = -(rows[1][0] * position[0] + rows[1][1] * position[1] + rows[1][2] * position[2]);
	rows[2][3] = -(rows[2][0] * position[0] + rows[2][1] * position[1] + rows[2][2] * position[2]) + b;
	rows[3][3] = -(rows[3][0] * position[0] + rows[3][1] * position[1] + rows[3][2] * position[2]);
}

static void transform(const float m[4][4], const float in[4], float out[4])
{
	int row;

	for (row = 0; row < 4; row++)
		out[row] = m[row][0] * in[0] + m[row][1] * in[1] + m[row][2] * in[2] + m[row][3] * in[3];
}

static void views(void)
{
	const float head[3] = { 10.0f, -4.0f, 1.5f }, forward[3] = { 0.6f, 0.8f, 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f };
	/* the eye a little to the head's right (forward x up: (0.8, -0.6, 0)) */
	const float eye[3] = { 10.0f + 0.8f * 0.0105f, -4.0f - 0.6f * 0.0105f, 1.5f };
	const float scale[4] = { 1.0f, 1.0f, 1.0f, 0.0f }, offset[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float fovea[4] = { 2.0f, 2.0f, -0.25f, 0.5f };
	const int32_t viewport[4] = { 0, 0, 1728, 1728 };
	const float depth_range[2] = { 0.0f, 1.0f };
	float centre[4][4], eye_rows[4][4], centre_inverse[4][4], correction[4][4], projection[6][4];
	float correction_columns[16];
	struct xgpu_vulkan_effects_view view;
	int row, column, foveated;

	camera_rows(centre, head, forward, up, 1.2f, 0.01f, 100.0f, 0.0f);
	camera_rows(eye_rows, eye, forward, up, 1.1f, 0.01f, 100.0f, 0.08f);
	/* the correction: the eye's clip position from the centre's */
	assert(invert(centre_inverse, centre));
	multiply(correction, eye_rows, centre_inverse);
	for (row = 0; row < 4; row++)
		for (column = 0; column < 4; column++)
			correction_columns[column * 4 + row] = correction[row][column];
	memcpy(projection, centre, sizeof(centre));
	/* the screen conversion as the identity: c[-38] 1, c[-37] less the
	half-pixel offset the shader adds */
	for (column = 0; column < 4; column++)
	{
		projection[4][column] = 1.0f;
		projection[5][column] = column == 0 || column == 1 ? -0.5f : 0.0f;
	}
	for (foveated = 0; foveated < 2; foveated++)
	{
		const float points[3][3] = { { 14.0f, 2.0f, 1.0f }, { 11.0f, -3.0f, 1.6f }, { 40.0f, 30.0f, -2.0f } };
		int point;

		assert(xgpu_vulkan_effects_view(projection, scale, offset, 0.0f, correction_columns, foveated ? fovea : NULL,
			viewport, depth_range, &view));
		/* the eye is where the eye's camera is */
		for (row = 0; row < 3; row++)
			assert(CLOSE(view.eye[row], eye[row], 1.0e-3));
		for (point = 0; point < 3; point++)
		{
			float world[4] = { points[point][0], points[point][1], points[point][2], 1.0f }, clip[4], expected[4];
			float window[4], back[4];

			/* the clip position is the eye's camera's, y down (and the inset's) */
			transform(view.clip, world, clip);
			transform(eye_rows, world, expected);
			expected[1] = -expected[1];
			if (foveated)
			{
				expected[0] = expected[0] * fovea[0] + fovea[2] * expected[3];
				expected[1] = expected[1] * fovea[1] + fovea[3] * expected[3];
			}
			for (row = 0; row < 4; row++)
				assert(CLOSE(clip[row], expected[row], 1.0e-3 * (fabs(expected[row]) + 1.0)));
			/* the pixel and depth it lands on, and back to the world */
			window[0] = viewport[0] + (clip[0] / clip[3] + 1.0f) * 0.5f * viewport[2];
			window[1] = viewport[1] + (clip[1] / clip[3] + 1.0f) * 0.5f * viewport[3];
			window[2] = depth_range[0] + clip[2] / clip[3] * (depth_range[1] - depth_range[0]);
			window[3] = 1.0f;
			for (row = 0; row < 4; row++)
				back[row] = view.unproject[0][row] * window[0] + view.unproject[1][row] * window[1] +
					view.unproject[2][row] * window[2] + view.unproject[3][row];
			for (row = 0; row < 3; row++)
				assert(CLOSE(back[row] / back[3], world[row], 2.0e-3 * (fabs(world[row]) + 1.0)));
		}
	}
	/* no viewport: nothing */
	{
		const int32_t empty[4] = { 0, 0, 0, 0 };

		assert(!xgpu_vulkan_effects_view(projection, scale, offset, 0.0f, NULL, NULL, empty, depth_range, &view));
	}
}

/* a flat screen's view (display.soft_particles: no eye's correction) is
the VR mode's with an identity correction, so its fade is the eyes' math:
the game's camera, in a render-scaled viewport off the target's corner
(supersampled or scaled, the depth's pixels are the frame's); and an eye's
correction moves it (the negative control) */
static void flat_view(void)
{
	const float camera[3] = { 10.0f, -4.0f, 1.5f }, forward[3] = { 0.6f, 0.8f, 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f };
	const float scale[4] = { 1.0f, 1.0f, 1.0f, 0.0f }, offset[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const int32_t viewport[4] = { 64, 32, 3840, 2160 };
	const float depth_range[2] = { 0.0f, 1.0f };
	const float points[3][3] = { { 14.0f, 2.0f, 1.0f }, { 11.0f, -3.0f, 1.6f }, { 40.0f, 30.0f, -2.0f } };
	float rows[4][4], projection[6][4], identity[16], moved[16];
	struct xgpu_vulkan_effects_view flat, eye, other;
	int row, column, point;

	camera_rows(rows, camera, forward, up, 1.2f, 0.01f, 100.0f, 0.0f);
	memcpy(projection, rows, sizeof(rows));
	for (column = 0; column < 4; column++)
	{
		projection[4][column] = 1.0f;
		projection[5][column] = column == 0 || column == 1 ? -0.5f : 0.0f;
	}
	memset(identity, 0, sizeof(identity));
	identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
	memcpy(moved, identity, sizeof(moved));
	moved[12] = 0.05f;
	assert(xgpu_vulkan_effects_view(projection, scale, offset, 0.0f, NULL, NULL, viewport, depth_range, &flat));
	assert(xgpu_vulkan_effects_view(projection, scale, offset, 0.0f, identity, NULL, viewport, depth_range, &eye));
	assert(xgpu_vulkan_effects_view(projection, scale, offset, 0.0f, moved, NULL, viewport, depth_range, &other));
	for (row = 0; row < 4; row++)
		for (column = 0; column < 4; column++)
			assert(flat.unproject[row][column] == eye.unproject[row][column]);
	for (row = 0; row < 3; row++)
		assert(CLOSE(flat.eye[row], camera[row], 1.0e-3));
	assert(memcmp(other.unproject, flat.unproject, sizeof(flat.unproject)) != 0);
	for (point = 0; point < 3; point++)
	{
		float world[4] = { points[point][0], points[point][1], points[point][2], 1.0f }, clip[4], window[4], back[4];

		transform(flat.clip, world, clip);
		window[0] = viewport[0] + (clip[0] / clip[3] + 1.0f) * 0.5f * viewport[2];
		window[1] = viewport[1] + (clip[1] / clip[3] + 1.0f) * 0.5f * viewport[3];
		window[2] = depth_range[0] + clip[2] / clip[3] * (depth_range[1] - depth_range[0]);
		window[3] = 1.0f;
		for (row = 0; row < 4; row++)
			back[row] = flat.unproject[0][row] * window[0] + flat.unproject[1][row] * window[1] +
				flat.unproject[2][row] * window[2] + flat.unproject[3][row];
		for (row = 0; row < 3; row++)
			assert(CLOSE(back[row] / back[3], world[row], 2.0e-3 * (fabs(world[row]) + 1.0)));
	}
}

int main(void)
{
	distances();
	hemisphere();
	two_sprites();
	masks();
	views();
	flat_view();
	printf("ok\n");
	return 0;
}
