/*
RIG_EVAL.C

A rig (xgpu_rig.c) run over recorded frames, for checking a mod's rig
against its own animations and against Blender's evaluation of the same
rules (tools/prism_rig_blender.py writes the inputs):

  rig_eval <skeleton entries> <ce node count> <rig.json or compiled rig> <frames> <out> [<pskin> <skinned>]

frames: "PRFR", u32 version 1, u32 frame count, u32 CE node count, then per
frame f64 game seconds, u32 flags (XGPU_RIG_INSTANCE_*), u32 key (0: no
springs' state), f32 functions[4], f32 camera[3], f32 unused, f32 the CE
nodes' skin matrices [count][12].
out: per frame each bone's world matrix [bones][12] then its skin matrix
[bones][12] (f32).
With a .pskin (tools/prism_rig.py), skinned: per frame each vertex's skinned
position [vertices][3] (f32), by xgpu_rig_skin_reference.
Prints the rig's bones, helpers and springs and the evaluations' time.
*/

#include "xgpu_rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

struct frame_header
{
	double game_seconds;
	uint32_t flags, key;
	float functions[4], camera[3], unused;
};

int main(int argc, char **argv)
{
	size_t skeleton_size, rig_size, frames_size, pskin_size = 0;
	struct xgpu_rig_bone *bones;
	unsigned char *rig_bytes, *frames, *pskin = NULL;
	void *compiled = NULL;
	size_t compiled_size = 0;
	uint32_t bone_count, ce_node_count, frame_count, frame_ce, frame, vertex_count = 0;
	enum xgpu_rig_status status;
	struct xgpu_rig *rig;
	struct xgpu_rig_states *states;
	float (*palette)[12], (*world)[12], (*skinned)[3] = NULL;
	FILE *out, *skinned_out = NULL;
	char error[256];
	double seconds = 0.0;

	if (argc != 6 && argc != 8)
	{
		fprintf(stderr, "usage: rig_eval <skeleton entries> <ce nodes> <rig> <frames> <out> [<pskin> <skinned>]\n");
		return 2;
	}
	bones = read_file(argv[1], &skeleton_size);
	rig_bytes = read_file(argv[3], &rig_size);
	frames = read_file(argv[4], &frames_size);
	if (!bones || !rig_bytes || !frames || skeleton_size % sizeof(*bones) || frames_size < 16)
	{
		fprintf(stderr, "cannot read the inputs\n");
		return 2;
	}
	bone_count = (uint32_t)(skeleton_size / sizeof(*bones));
	ce_node_count = (uint32_t)atoi(argv[2]);
	if (rig_size >= 4 && memcmp(rig_bytes, "PRIG", 4) == 0)
	{
		compiled = rig_bytes;
		compiled_size = rig_size;
	}
	else
	{
		status = xgpu_rig_compile_json(bones, bone_count, ce_node_count, (const char *)rig_bytes, rig_size, &compiled,
			&compiled_size, error, sizeof(error));
		if (status != XGPU_RIG_OK)
		{
			fprintf(stderr, "rig refused (status %d): %s\n", status, error);
			return 3;
		}
	}
	rig = xgpu_rig_create(bones, bone_count, ce_node_count, compiled, compiled_size, &status, error, sizeof(error));
	if (!rig)
	{
		fprintf(stderr, "rig refused (status %d): %s\n", status, error);
		return 3;
	}
	memcpy(&frame_count, frames + 8, 4);
	memcpy(&frame_ce, frames + 12, 4);
	if (memcmp(frames, "PRFR", 4) || frame_ce < ce_node_count ||
		frames_size != 16 + (size_t)frame_count * (sizeof(struct frame_header) + frame_ce * 48u))
	{
		fprintf(stderr, "frames: not a frames file of %u CE nodes\n", ce_node_count);
		return 2;
	}
	if (argc == 8)
	{
		pskin = read_file(argv[6], &pskin_size);
		if (!pskin || pskin_size < 16 || memcmp(pskin, "PSKN", 4))
		{
			fprintf(stderr, "cannot read the .pskin\n");
			return 2;
		}
		memcpy(&vertex_count, pskin + 8, 4);
		skinned = malloc((size_t)vertex_count * sizeof(*skinned) + 1);
		skinned_out = fopen(argv[7], "wb");
	}
	palette = malloc(bone_count * sizeof(*palette));
	world = malloc(bone_count * sizeof(*world));
	states = xgpu_rig_states_create();
	out = fopen(argv[5], "wb");
	if (!palette || !world || !states || !out || (argc == 8 && (!skinned || !skinned_out)))
		return 2;
	for (frame = 0; frame < frame_count; frame++)
	{
		const unsigned char *at = frames + 16 + (size_t)frame * (sizeof(struct frame_header) + frame_ce * 48u);
		struct frame_header header;
		struct xgpu_rig_instance instance;
		struct timespec start, end;

		memcpy(&header, at, sizeof(header));
		memset(&instance, 0, sizeof(instance));
		instance.key = header.key;
		instance.game_seconds = header.game_seconds;
		instance.ce_matrices = (const float (*)[12])(at + sizeof(header));
		instance.ce_matrix_count = frame_ce;
		instance.flags = header.flags;
		memcpy(instance.functions, header.functions, sizeof(instance.functions));
		memcpy(instance.camera, header.camera, sizeof(instance.camera));
		clock_gettime(CLOCK_MONOTONIC, &start);
		status = xgpu_rig_evaluate(rig, states, &instance, palette, world);
		xgpu_rig_states_age(states, instance.game_seconds);
		clock_gettime(CLOCK_MONOTONIC, &end);
		seconds += (double)(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) * 1e-9;
		if (status != XGPU_RIG_OK)
		{
			fprintf(stderr, "frame %u: status %d\n", frame, status);
			return 4;
		}
		fwrite(world, sizeof(*world), bone_count, out);
		fwrite(palette, sizeof(*palette), bone_count, out);
		if (skinned)
		{
			const unsigned char *positions = pskin + 16;
			const unsigned char *normals = positions + (size_t)vertex_count * 12;
			const unsigned char *joints = normals + (size_t)vertex_count * 12;
			const unsigned char *weights = joints + (size_t)vertex_count * 8;

			xgpu_rig_skin_reference((const float (*)[12])palette, bone_count, (const float (*)[3])positions, NULL,
				(const uint16_t (*)[4])joints, (const uint8_t (*)[4])weights, vertex_count, skinned, NULL);
			fwrite(skinned, sizeof(*skinned), vertex_count, skinned_out);
		}
	}
	fclose(out);
	if (skinned_out)
		fclose(skinned_out);
	printf("bones %u (CE nodes %u), springs %u, frames %u, %.2f us an evaluation\n", bone_count, ce_node_count,
		xgpu_rig_spring_count(rig), frame_count, frame_count ? seconds * 1e6 / frame_count : 0.0);
	xgpu_rig_states_destroy(states);
	xgpu_rig_destroy(rig);
	return 0;
}
