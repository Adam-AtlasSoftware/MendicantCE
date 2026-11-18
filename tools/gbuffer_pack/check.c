/*
CHECK.C

PRISM v4 M4's G-buffer uber-shaders (port/linux/src/xgpu_spirv.c's
gbuffer_pack; tools/test_gbuffer_pack.py): the SPIR-V emitter's G-buffer
twins as the backend makes them, of a level's lightmap pass (texture 0 the
bump map, 2 the lightmap, 3 the normalization cube: v0 by the lightmap) and
of a model's draw (texture 0 the base map, 2 the multipurpose map, 3 the
reflection cube: t0 by v0 plus t3 by r1's alpha), each with and without a
material's maps (gbuffer_pack), with and without the vertices' normal, and
the level's with and without the base map's coordinates handed on; and the
level's vertex shader with and without them:

  check spirv <directory>    <name>.spv for each case:
                             <level|object>-<pack>-<normal>-<base_uv> (pixel),
                             vertex-<base_uv>

Built by the test with the shader decoders and both emitters, as
tools/pbr_materials is.
*/

#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "xgpu_msl.h"
#include "xgpu_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...) { (void)format; }

static void stage_make(struct xgpu_pixel_program *program, int stage, int mode, int sampler)
{
	program->stages[stage].declared_mode = program->stages[stage].mode = (unsigned char)mode;
	program->stages[stage].sampler = (unsigned char)sampler;
}

/* the level's lightmap pass (r0 = t2 * v0) or a model's draw (r0 = t0 * v0
+ t3 * r1.a), alpha t0's */
static void program_make(struct xgpu_pixel_program *program, int level)
{
	memset(program, 0, sizeof(*program));
	stage_make(program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
	stage_make(program, 2, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
	stage_make(program, 3, XGPU_TEXTURE_CUBEMAP, _xgpu_sampler_cube);
	program->combiner_count = 1;
	program->combiners[0].portions[0].inputs[0].reg = level ? XGPU_REGISTER_T2 : XGPU_REGISTER_T0;
	program->combiners[0].portions[0].inputs[1].reg = XGPU_REGISTER_V0;
	if (!level)
	{
		program->combiners[0].portions[0].inputs[2].reg = XGPU_REGISTER_T3;
		program->combiners[0].portions[0].inputs[3].reg = XGPU_REGISTER_R1;
		program->combiners[0].portions[0].inputs[3].alpha = 1;
	}
	program->combiners[0].portions[0].sum = XGPU_REGISTER_R0;
	program->final.present = 1;
	program->final.inputs[3].reg = XGPU_REGISTER_R0;
	program->final.inputs[6].reg = XGPU_REGISTER_T0;
	program->final.inputs[6].alpha = 1;
}

/* the level's vertex program: oPos = v0, oT0 = v4 (its coordinates, read) */
static void vertex_make(struct xgpu_vertex_program *program)
{
	int index;

	memset(program, 0, sizeof(*program));
	program->instruction_count = 2;
	for (index = 0; index < 2; index++)
	{
		struct xgpu_vertex_instruction *instruction = &program->instructions[index];

		instruction->mac = XGPU_MAC_MOV;
		instruction->a.file = XGPU_VERTEX_INPUT;
		instruction->a.index = index ? 4 : 0;
		instruction->a.swizzle[0] = 0;
		instruction->a.swizzle[1] = 1;
		instruction->a.swizzle[2] = 2;
		instruction->a.swizzle[3] = 3;
		instruction->writes_output = 1;
		instruction->output_is_register = 1;
		instruction->output = index ? XGPU_OUTPUT_TEXTURE0 : XGPU_OUTPUT_POSITION;
		instruction->output_mask = 15;
	}
	program->instructions[1].last = 1;
}

static int write_module(const char *directory, const char *name, const struct xgpu_spirv_module *module)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s.spv", directory, name);
	if (!(file = fopen(path, "wb")))
		return 1;
	fwrite(module->words, 4, module->count, file);
	fclose(file);
	return 0;
}

static int spirv(const char *directory)
{
	static const struct nv2a_vertex_lighting environment = { XGPU_ENVIRONMENT_LIGHTING };
	struct xgpu_pixel_program program;
	struct xgpu_vertex_program vertex;
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	char name[128];
	int level, pack, normal, base_uv;

	for (level = 0; level < 2; level++)
		for (pack = 0; pack < 2; pack++)
			for (normal = 0; normal < 2; normal++)
				for (base_uv = 0; base_uv < 1 + level; base_uv++)
				{
					program_make(&program, level);
					memset(&options, 0, sizeof(options));
					options.gbuffer = level ? XGPU_SPIRV_GBUFFER_LEVEL : XGPU_SPIRV_GBUFFER_OBJECT;
					options.gbuffer_normal = normal;
					options.gbuffer_pack = pack;
					options.gbuffer_base_uv = base_uv;
					snprintf(name, sizeof(name), "%s-%d-%d-%d", level ? "level" : "object", pack, normal, base_uv);
					if (!xgpu_spirv_pixel_shader(&program, &options, &module) || write_module(directory, name, &module))
						return 1;
					free(module.words);
				}
	for (base_uv = 0; base_uv < 2; base_uv++)
	{
		vertex_make(&vertex);
		memset(&options, 0, sizeof(options));
		options.gbuffer = XGPU_SPIRV_GBUFFER_LEVEL;
		options.gbuffer_normal = 1;
		options.gbuffer_pack = 1;
		options.gbuffer_base_uv = base_uv;
		snprintf(name, sizeof(name), "vertex-%d", base_uv);
		if (!xgpu_spirv_vertex_shader(&vertex, 0, 0, &environment, &options, &module) ||
			write_module(directory, name, &module))
			return 1;
		free(module.words);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "spirv"))
		return spirv(argv[2]);
	fprintf(stderr, "check spirv <directory>\n");
	return 2;
}
