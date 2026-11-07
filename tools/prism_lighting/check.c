/*
CHECK.C

display.prism_lightmaps' lightmap pass (port/linux/src/xgpu.h's "The
lightmaps smoothed"; tools/test_prism_lighting.py): the pixel shaders of a
structure's lightmap pass (its bump map in stage 0, its lightmap in stage 2
at projected coordinates), alone, with the enhanced dynamic lights and with
display.ambient_occlusion "world", each with the key's lightmap_filter 0
and XGPU_LIGHTMAP_FILTER_SMOOTH, as the emitters write them:

  check msl                 the Metal emitter's, each after a line
                            "==== <case> <filter: 0 or 1>"
  check spirv <directory>   the SPIR-V emitter's, <case>-<filter>.spv

Built by the test with the shader decoders and both emitters, as
tools/ambient_occlusion_light is.
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

static const struct
{
	const char *name;
	unsigned char dynamic_lights, ambient_occlusion, border;
} cases[] =
{
	{ "lightmap", 0, 0, 0 },
	{ "lightmap_dynamic", XGPU_DYNAMIC_LIGHTS_ENVIRONMENT, 0, 0 },
	{ "lightmap_world", 0, XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP | XGPU_AMBIENT_OCCLUSION_WORLD, 0 },
	/* (a border on stage 2: never smoothed, sampled as it is) */
	{ "lightmap_border", 0, 0, 3 },
};

/* the lightmap's light by the bump map's (t2 by t0: the combiners' shape
alone matters here) */
static void program_make(struct xgpu_pixel_program *program, int index, int filter)
{
	memset(program, 0, sizeof(*program));
	program->stages[0].declared_mode = program->stages[0].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[0].sampler = _xgpu_sampler_2d;
	program->stages[2].declared_mode = program->stages[2].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[2].sampler = _xgpu_sampler_2d;
	program->final.present = 1;
	program->final.inputs[0].reg = XGPU_REGISTER_T2;
	program->final.inputs[1].reg = XGPU_REGISTER_T0;
	program->final.inputs[6].reg = XGPU_REGISTER_T0;
	program->final.inputs[6].alpha = 1;
	program->dynamic_lights = cases[index].dynamic_lights;
	program->ambient_occlusion = cases[index].ambient_occlusion;
	program->lightmap_filter = (unsigned char)(filter ? XGPU_LIGHTMAP_FILTER_SMOOTH : 0);
}

static int msl(void)
{
	struct xgpu_pixel_program program;
	struct xgpu_msl_options options;
	unsigned int index;
	int filter;
	char *source;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		for (filter = 0; filter < 2; filter++)
		{
			memset(&options, 0, sizeof(options));
			options.border[2] = cases[index].border;
			program_make(&program, (int)index, filter);
			source = xgpu_msl_pixel_shader(&program, &options);
			if (!source)
				return 1;
			printf("==== %s %d\n%s\n", cases[index].name, filter, source);
			free(source);
		}
	}
	return 0;
}

static int spirv(const char *directory)
{
	struct xgpu_pixel_program program;
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	unsigned int index;
	int filter;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		for (filter = 0; filter < 2; filter++)
		{
			char path[1024];
			FILE *file;

			memset(&options, 0, sizeof(options));
			options.border[2] = cases[index].border;
			program_make(&program, (int)index, filter);
			if (!xgpu_spirv_pixel_shader(&program, &options, &module))
				return 1;
			snprintf(path, sizeof(path), "%s/%s-%d.spv", directory, cases[index].name, filter);
			if (!(file = fopen(path, "wb")))
				return 1;
			fwrite(module.words, 4, module.count, file);
			fclose(file);
			free(module.words);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "msl"))
		return msl();
	if (argc >= 3 && !strcmp(argv[1], "spirv"))
		return spirv(argv[2]);
	fprintf(stderr, "check msl | check spirv <directory>\n");
	return 2;
}
