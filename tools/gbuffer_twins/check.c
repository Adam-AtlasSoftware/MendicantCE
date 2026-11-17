/*
CHECK.C

display.lighting's v4 modes' G-buffer twins (xgpu_spirv.h's
xgpu_spirv_options gbuffer; tools/test_gbuffer_twins.py): the SPIR-V
emitter's shaders of a twin as the backend makes them, written for the test
to validate and read. A model's vertex program (its microcode given, four
words an instruction, as the game's vertex shaders' data has it) as an
object's twin's vertex shader, with its second run on the last frame's
constants (gbuffer_previous) and without, and as the level's; and a model
shader's pixel program as an object's twin's pixel shader, reading the two
clip positions, and as the level's:

  check spirv <microcode file> <directory>
      vertex-object-previous, vertex-object, vertex-level,
      pixel-object-previous, pixel-level

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

/* a model shader's shape: t0 * v0, fogged (as tools/pbr_materials') */
static void pixel_program_make(struct xgpu_pixel_program *program)
{
	memset(program, 0, sizeof(*program));
	program->stages[0].declared_mode = program->stages[0].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[0].sampler = _xgpu_sampler_2d;
	program->combiner_count = 1;
	program->combiners[0].portions[0].inputs[0].reg = XGPU_REGISTER_T0;
	program->combiners[0].portions[0].inputs[1].reg = XGPU_REGISTER_V0;
	program->combiners[0].portions[0].sum = XGPU_REGISTER_R0;
	program->final.present = 1;
	program->final.inputs[0].reg = XGPU_REGISTER_FOG;
	program->final.inputs[0].alpha = 1;
	program->final.inputs[1].reg = XGPU_REGISTER_R0;
	program->final.inputs[2].reg = XGPU_REGISTER_FOG;
	program->final.inputs[6].reg = XGPU_REGISTER_T0;
	program->final.inputs[6].alpha = 1;
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
	free(module->words);
	return 0;
}

static int spirv(const char *microcode, const char *directory)
{
	static DWORD words[XGPU_VERTEX_PROGRAM_MAXIMUM * 4];
	struct xgpu_vertex_program program;
	struct xgpu_pixel_program pixel;
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	unsigned long count;
	FILE *file = fopen(microcode, "rb");

	if (!file)
		return 1;
	count = (unsigned long)fread(words, 4, XGPU_VERTEX_PROGRAM_MAXIMUM * 4, file) / 4;
	fclose(file);
	nv2a_vertex_program_decode(words, count, &program);
	memset(&options, 0, sizeof(options));
	options.reversed_depth = TRUE;
	options.gbuffer = XGPU_SPIRV_GBUFFER_OBJECT;
	options.gbuffer_previous = TRUE;
	if (!xgpu_spirv_vertex_shader(&program, 0, 0, NULL, &options, &module) ||
		write_module(directory, "vertex-object-previous", &module))
		return 1;
	options.gbuffer_previous = FALSE;
	if (!xgpu_spirv_vertex_shader(&program, 0, 0, NULL, &options, &module) ||
		write_module(directory, "vertex-object", &module))
		return 1;
	options.gbuffer = XGPU_SPIRV_GBUFFER_LEVEL;
	if (!xgpu_spirv_vertex_shader(&program, 0, 0, NULL, &options, &module) ||
		write_module(directory, "vertex-level", &module))
		return 1;
	pixel_program_make(&pixel);
	options.gbuffer = XGPU_SPIRV_GBUFFER_OBJECT;
	options.gbuffer_previous = TRUE;
	if (!xgpu_spirv_pixel_shader(&pixel, &options, &module) || write_module(directory, "pixel-object-previous", &module))
		return 1;
	options.gbuffer = XGPU_SPIRV_GBUFFER_LEVEL;
	options.gbuffer_previous = FALSE;
	if (!xgpu_spirv_pixel_shader(&pixel, &options, &module) || write_module(directory, "pixel-level", &module))
		return 1;
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "spirv"))
		return spirv(argv[2], argv[3]);
	fprintf(stderr, "check spirv <microcode file> <directory>\n");
	return 2;
}
