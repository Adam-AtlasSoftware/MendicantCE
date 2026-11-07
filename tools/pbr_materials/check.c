/*
CHECK.C

display.prism_materials' shaders (port/linux/src/xgpu.h's "PBR materials";
tools/test_pbr_materials.py): the SPIR-V emitter's pixel shaders of a model
lit for each pixel with a material, as the backend makes them, written for
the test to validate and read. A model shader's shape (texture 0 the base
map, 2 the multipurpose map, 3 the reflection cube: v0 by texture 0, plus
texture 3 by r1's alpha, fogged), lit by its point lights or the enhanced
dynamic lights, with and without the material overlay (a surface map), for
every material_pbr the key may have:

  check spirv <directory>      <name>.spv for each case: pbr-<bits>-<lights>-<overlay>-<cube>,
                               and base-<lights>-<overlay> (material_pbr 0); and
                               display.prism_models' wide variant (xgpu_spirv_options'
                               mesh_wide): wide-<bits>-<lights>-<overlay> (the cube's), and
                               vertex-stock, vertex-wide-full (a model's vertex program, lit:
                               its 32 scalars handed on leave no room for the tangent),
                               vertex-wide-room (unlit: the tangent handed on) and
                               vertex-reads-wide (unlit, reading the wide input itself: none)

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

/* the model shader's shape: t0 * v0 + t3 * r1.a, alpha t0's */
static void program_make(struct xgpu_pixel_program *program, int dynamic_lights, int overlay, int cube,
	unsigned int pbr)
{
	memset(program, 0, sizeof(*program));
	program->stages[0].declared_mode = program->stages[0].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[0].sampler = _xgpu_sampler_2d;
	program->stages[2].declared_mode = program->stages[2].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[2].sampler = _xgpu_sampler_2d;
	if (cube)
	{
		program->stages[3].declared_mode = program->stages[3].mode = XGPU_TEXTURE_CUBEMAP;
		program->stages[3].sampler = _xgpu_sampler_cube;
	}
	program->combiner_count = 1;
	/* (r0 = t0 * v0 + t3 * r1.a) */
	program->combiners[0].portions[0].inputs[0].reg = XGPU_REGISTER_T0;
	program->combiners[0].portions[0].inputs[1].reg = XGPU_REGISTER_V0;
	program->combiners[0].portions[0].inputs[2].reg = XGPU_REGISTER_T3;
	program->combiners[0].portions[0].inputs[3].reg = XGPU_REGISTER_R1;
	program->combiners[0].portions[0].inputs[3].alpha = 1;
	program->combiners[0].portions[0].sum = XGPU_REGISTER_R0;
	program->final.present = 1;
	program->final.inputs[0].reg = XGPU_REGISTER_FOG;
	program->final.inputs[0].alpha = 1;
	program->final.inputs[1].reg = XGPU_REGISTER_R0;
	program->final.inputs[2].reg = XGPU_REGISTER_FOG;
	program->final.inputs[6].reg = XGPU_REGISTER_T0;
	program->final.inputs[6].alpha = 1;
	program->per_pixel_lighting = 2;
	program->dynamic_lights = dynamic_lights ? XGPU_DYNAMIC_LIGHTS_MODEL : 0;
	program->material_overlay = overlay ? (unsigned char)(1 + XGPU_OVERLAY_SURFACE + XGPU_OVERLAY_SURFACE_ONLY) : 0;
	program->material_pbr = (unsigned char)pbr;
}

/* a model's vertex program's shape: the position from v0, lit (its
normal from r0, its position from r1); reading v<reads> too where not 0 */
static void vertex_make(struct xgpu_vertex_program *program, struct nv2a_vertex_lighting *lighting, int reads)
{
	int index;

	memset(program, 0, sizeof(*program));
	program->instruction_count = reads ? 2 : 1;
	for (index = 0; index < program->instruction_count; index++)
	{
		struct xgpu_vertex_instruction *instruction = &program->instructions[index];

		instruction->mac = XGPU_MAC_MOV;
		instruction->a.file = XGPU_VERTEX_INPUT;
		instruction->a.index = index ? reads : 0;
		instruction->a.swizzle[0] = 0;
		instruction->a.swizzle[1] = 1;
		instruction->a.swizzle[2] = 2;
		instruction->a.swizzle[3] = 3;
		instruction->writes_output = 1;
		instruction->output_is_register = 1;
		instruction->output = index ? XGPU_OUTPUT_TEXTURE0 : XGPU_OUTPUT_POSITION;
		instruction->output_mask = 15;
	}
	program->instructions[program->instruction_count - 1].last = 1;
	memset(lighting, 0, sizeof(*lighting));
	lighting->lights = 2;
	lighting->normal_instruction = 0;
	lighting->normal_register = 0;
	lighting->position_instruction = 0;
	lighting->position_register = 1;
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
	struct xgpu_pixel_program program;
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	char name[128];
	unsigned int bits;
	int lights, overlay, cube;

	memset(&options, 0, sizeof(options));
	for (lights = 0; lights < 2; lights++)
	{
		for (overlay = 0; overlay < 2; overlay++)
		{
			program_make(&program, lights, overlay, 1, 0);
			snprintf(name, sizeof(name), "base-%d-%d", lights, overlay);
			if (!xgpu_spirv_pixel_shader(&program, &options, &module) || write_module(directory, name, &module))
				return 1;
			free(module.words);
			for (cube = 0; cube < 2; cube++)
			{
				/* (every image's bit and the clear coat's lobe, with XGPU_PBR_ON) */
				for (bits = 0; bits < 128; bits++)
				{
					unsigned int pbr = XGPU_PBR_ON | (bits << 1);

					program_make(&program, lights, overlay, cube, pbr);
					snprintf(name, sizeof(name), "pbr-%03u-%d-%d-%d", pbr, lights, overlay, cube);
					if (!xgpu_spirv_pixel_shader(&program, &options, &module) || write_module(directory, name, &module))
						return 1;
					free(module.words);
				}
			}
			/* (the wide variant: the mesh's frame and uv0) */
			options.mesh_wide = TRUE;
			for (bits = 0; bits < 128; bits++)
			{
				unsigned int pbr = XGPU_PBR_ON | (bits << 1);

				program_make(&program, lights, overlay, 1, pbr);
				snprintf(name, sizeof(name), "wide-%03u-%d-%d", pbr, lights, overlay);
				if (!xgpu_spirv_pixel_shader(&program, &options, &module) || write_module(directory, name, &module))
					return 1;
				free(module.words);
			}
			options.mesh_wide = FALSE;
		}
	}
	{
		static const char *const names[4] = { "vertex-stock", "vertex-wide-full", "vertex-wide-room", "vertex-reads-wide" };
		struct xgpu_vertex_program *vertex = malloc(sizeof(*vertex));
		struct nv2a_vertex_lighting lighting;
		int kind;

		if (!vertex)
			return 1;
		for (kind = 0; kind < 4; kind++)
		{
			vertex_make(vertex, &lighting, kind == 3 ? XGPU_SPIRV_MESH_WIDE_INPUT : 0);
			options.mesh_wide = kind != 0;
			if (!xgpu_spirv_vertex_shader(vertex, 0, 0, kind >= 2 ? NULL : &lighting, &options, &module) ||
				write_module(directory, names[kind], &module))
			{
				free(vertex);
				return 1;
			}
			free(module.words);
		}
		options.mesh_wide = FALSE;
		free(vertex);
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
