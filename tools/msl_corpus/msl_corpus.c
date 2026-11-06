/*
MSL_CORPUS.C

The Metal emitter's corpus (port/linux/src/xgpu_msl.c): every captured
vertex program in every variant the Mac app draws, and every captured pixel
shader key, written as Metal Shading Language, one .metal file each, for
Apple's compiler to check (compile_mac.sh). Identical shaders are written
once (index.txt names each variant's file).

  msl_corpus <capture dir> <out dir>

The capture directory holds what a run writes with the renderer's test
capture (HALO_TEST_CAPTURE): vsNNN.bin, a vertex program (its NORMPACKED3
inputs' mask, 4 bytes, then its microcode, 16 bytes an instruction), and
ps_XXXXXXXX.key, a pixel shader key as the 32-bit game lays it out (264
bytes).

Built by build.sh against a source tree with the renderer's shader
intermediate form (xgpu_shader.h).
*/

#include "xgpu.h"
#include "xgpu_shader.h"
#include "xgpu_msl.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

void platform_log(const char *format, ...) { (void)format; }

#include "synthetic.c"

static const char *out_dir;
static FILE *index_file;
static unsigned long written, same;

/* the shaders written so far, by their text's hash, to write each once */
struct written_shader
{
	unsigned long long hash;
	char name[96];
};
static struct written_shader *shaders;
static unsigned long shader_count, shader_capacity;

static int names_compare(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static int list(const char *dir, const char *prefix, char ***names)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int count = 0, capacity = 0;

	*names = NULL;
	while (d && (e = readdir(d)))
	{
		if (strncmp(e->d_name, prefix, strlen(prefix)))
			continue;
		if (count == capacity)
			*names = realloc(*names, (capacity = capacity ? capacity * 2 : 256) * sizeof(char *));
		(*names)[count++] = strdup(e->d_name);
	}
	if (d)
		closedir(d);
	qsort(*names, count, sizeof(char *), names_compare);
	return count;
}

static void *read_file(const char *dir, const char *name, long *size)
{
	char path[1024];
	FILE *f;
	void *data;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "rb")))
	{
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(*size);
	if (fread(data, 1, *size, f) != (size_t)*size)
	{
		perror(path);
		exit(1);
	}
	fclose(f);
	return data;
}

static unsigned long long text_hash(const char *text)
{
	unsigned long long hash = 14695981039346656037ULL;

	while (*text)
		hash = (hash ^ (unsigned char)*text++) * 1099511628211ULL;
	return hash;
}

/* writes a variant's shader as <name>.metal, unless the same text has been */
static void emit(const char *name, const char *what, char *source)
{
	unsigned long long hash;
	unsigned long index;
	char path[1024];
	FILE *file;

	if (!source)
	{
		fprintf(stderr, "%s: no shader\n", name);
		exit(1);
	}
	hash = text_hash(source);
	for (index = 0; index < shader_count; index++)
	{
		if (shaders[index].hash == hash)
		{
			fprintf(index_file, "%s\t%s\t%s\n", name, shaders[index].name, what);
			same++;
			free(source);
			return;
		}
	}
	if (shader_count == shader_capacity)
		shaders = realloc(shaders, (shader_capacity = shader_capacity ? shader_capacity * 2 : 1024) * sizeof(*shaders));
	shaders[shader_count].hash = hash;
	snprintf(shaders[shader_count].name, sizeof(shaders[shader_count].name), "%s", name);
	shader_count++;
	snprintf(path, sizeof(path), "%s/%s.metal", out_dir, name);
	if (!(file = fopen(path, "w")) || fputs(source, file) < 0 || fclose(file))
	{
		perror(path);
		exit(1);
	}
	fprintf(index_file, "%s\t%s\t%s\n", name, name, what);
	written++;
	free(source);
}

/* ---------- vertex programs: immediate mode (all floats) and streams
(their NORMPACKED3 inputs), unlit, the structures' lighting for the
enhanced dynamic lights, the model lighting for each pixel; and streams with
the other inputs unnormalized shorts, which no capture records (the vertex
layout does) */

static void vertex(const char *file, const DWORD *words, unsigned long count, unsigned long packed)
{
	static const struct xgpu_msl_options options = { 0 };
	struct xgpu_vertex_program program;
	struct nv2a_vertex_lighting lighting, environment;
	char base[64], name[96], what[160];
	BOOL lit;
	int immediate, light;

	snprintf(base, sizeof(base), "%.*s", (int)(strchr(file, '.') ? strchr(file, '.') - file : (long)strlen(file)), file);
	nv2a_vertex_program_decode(words, count, &program);
	memset(&lighting, 0, sizeof(lighting));
	lit = nv2a_vertex_shader_lighting(&program, &lighting);
	memset(&environment, 0, sizeof(environment));
	environment.lights = XGPU_ENVIRONMENT_LIGHTING;
	for (immediate = 0; immediate < 2; immediate++)
	{
		for (light = 0; light < 3; light++)
		{
			const struct nv2a_vertex_lighting *l = light == 0 ? NULL : light == 1 ? &environment : &lighting;

			if (light == 2 && !lit)
				continue;
			snprintf(name, sizeof(name), "%s-%s-light%d", base, immediate ? "immediate" : "streams", light);
			snprintf(what, sizeof(what), "%s packed %#lx lighting %d", file, immediate ? 0 : packed, l ? l->lights : 0);
			emit(name, what, xgpu_msl_vertex_shader(&program, immediate ? 0 : packed, 0, l, &options));
		}
	}
	snprintf(name, sizeof(name), "%s-shorts", base);
	snprintf(what, sizeof(what), "%s packed %#lx shorts %#lx", file, packed, 0xffffUL & ~packed);
	emit(name, what, xgpu_msl_vertex_shader(&program, packed, 0xffffUL & ~packed, NULL, &options));
}

/* ---------- pixel shader keys */

/* a key as the guest (ILP32) lays it out */
static void key_from_guest(const unsigned char *bytes, struct nv2a_pixel_shader_key *key)
{
	unsigned long offset = 0;
	unsigned int value;

	memset(key, 0, sizeof(*key));
	memcpy(key->combiner_state, bytes, sizeof(key->combiner_state));
	offset += sizeof(key->combiner_state);
	memcpy(&key->texture_modes, bytes + offset, 4);
	offset += 4;
	memcpy(key->sampler_type, bytes + offset, 4);
	offset += 4;
	memcpy(key->alpha_kill, bytes + offset, 4);
	offset += 4;
	memcpy(key->color_sign, bytes + offset, 4);
	offset += 4;
	memcpy(&value, bytes + offset, 4);
	key->alpha_test_function = value;
	offset += 4;
	key->fog_enable = bytes[offset++];
	key->fog_table_mode = bytes[offset++];
	key->count_samples = bytes[offset++];
	key->coverage_alpha = bytes[offset++];
	key->point_threshold = bytes[offset++];
	key->per_pixel_lighting = bytes[offset++];
	key->alpha_test_samples = bytes[offset++];
	key->multiview = bytes[offset++];
	key->source_blend = bytes[offset++];
	key->dynamic_lights = bytes[offset++];
}

/* a pixel program as the Mac app draws it: its visibility tests counted by
a visibility result buffer, and by the shader; the border drawn by the
shader on each stage that samples a 2D or 3D texture */
static void pixel_program(const char *base, const char *what, const struct xgpu_pixel_program *program)
{
	struct xgpu_msl_options options;
	char name[96], variant[160];
	int stage, border = 0;

	memset(&options, 0, sizeof(options));
	emit(base, what, xgpu_msl_pixel_shader(program, &options));
	if (program->count_samples)
	{
		options.count_samples = TRUE;
		snprintf(name, sizeof(name), "%s-count", base);
		snprintf(variant, sizeof(variant), "%s counted in the shader", what);
		emit(name, variant, xgpu_msl_pixel_shader(program, &options));
		options.count_samples = FALSE;
	}
	for (stage = 0; stage < 4; stage++)
	{
		if (program->stages[stage].mode != XGPU_TEXTURE_NONE && (program->stages[stage].sampler == _xgpu_sampler_2d ||
			program->stages[stage].sampler == _xgpu_sampler_3d))
		{
			options.border[stage] = 7;
			border = 1;
		}
	}
	if (border)
	{
		snprintf(name, sizeof(name), "%s-border", base);
		snprintf(variant, sizeof(variant), "%s border drawn by the shader", what);
		emit(name, variant, xgpu_msl_pixel_shader(program, &options));
	}
}

/* a captured key, flat (the VR mode's views drawn as none) */
static void pixel(const char *file, const unsigned char *bytes)
{
	struct nv2a_pixel_shader_key key;
	struct xgpu_pixel_program program;
	char base[64];

	snprintf(base, sizeof(base), "%.*s", (int)(strchr(file, '.') - file), file);
	key_from_guest(bytes, &key);
	key.multiview = XGPU_MULTIVIEW_NONE;
	nv2a_pixel_program_decode(&key, &program);
	pixel_program(base, file, &program);
}

static void synthetic_pixel(const char *name, const struct xgpu_pixel_program *program)
{
	pixel_program(name, name, program);
}

static void synthetic_vertex(const char *name, const struct xgpu_vertex_program *program)
{
	static const struct xgpu_msl_options options = { 0 };
	char variant[96];

	emit(name, name, xgpu_msl_vertex_shader(program, 0, 0, NULL, &options));
	snprintf(variant, sizeof(variant), "%s-streams", name);
	emit(variant, "packed 0x5, shorts 0x60", xgpu_msl_vertex_shader(program, 0x5, 0x60, NULL, &options));
}

/* display.prism.material_overlay (xgpu.h): a model's program lit for each
pixel with two tiles and with four and a class mask, in each debug view, by
the point lights and the enhanced dynamic lights, with a reflection cube
and without (spirv_corpus.c's overlay_programs) */
static void overlay_programs(void)
{
	static const unsigned char overlays[] = { 1, 3, 5, 7, 9, 1 | XGPU_OVERLAY_CLASS_MASK, 3 | XGPU_OVERLAY_CLASS_MASK,
		7 | XGPU_OVERLAY_CLASS_MASK, 9 | XGPU_OVERLAY_CLASS_MASK };
	struct xgpu_pixel_program program;
	char name[96];
	unsigned int overlay, lights, kind, cube;

	for (overlay = 0; overlay < sizeof(overlays); overlay++)
	{
		for (lights = 1; lights <= 2; lights++)
		{
			for (kind = 0; kind < 2; kind++)
			{
				for (cube = 0; cube < 2; cube++)
				{
					synthetic_base(&program);
					synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
					synthetic_stage(&program, 1, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
					synthetic_stage(&program, 2, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
					synthetic_stage(&program, 3, cube ? XGPU_TEXTURE_CUBEMAP : XGPU_TEXTURE_PROJECT2D,
						cube ? _xgpu_sampler_cube : _xgpu_sampler_2d);
					program.combiners[0].portions[0].inputs[0].reg = XGPU_REGISTER_V0;
					program.per_pixel_lighting = (unsigned char)lights;
					program.material_overlay = overlays[overlay];
					if (kind)
						program.dynamic_lights = XGPU_DYNAMIC_LIGHTS_MODEL;
					snprintf(name, sizeof(name), "overlay%u-lights%u-kind%u-cube%u", overlays[overlay], lights, kind, cube);
					pixel_program(name, "display.prism.material_overlay", &program);
				}
			}
		}
	}
}

int main(int argc, char **argv)
{
	const char *dir;
	char **names, path[1024];
	int count, index;

	if (argc != 3)
	{
		fprintf(stderr, "usage: msl_corpus <capture dir> <out dir>\n");
		return 2;
	}
	dir = argv[1];
	out_dir = argv[2];
	if (mkdir(out_dir, 0777) && errno != EEXIST)
	{
		perror(out_dir);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/index.txt", out_dir);
	if (!(index_file = fopen(path, "w")))
	{
		perror(path);
		return 1;
	}
	fprintf(index_file, "# variant\tfile (.metal)\twhat\n");
	count = list(dir, "vs", &names);
	for (index = 0; index < count; index++)
	{
		long size;
		unsigned char *data = read_file(dir, names[index], &size);
		unsigned int packed;

		memcpy(&packed, data, 4);
		vertex(names[index], (const DWORD *)(data + 4), (unsigned long)(size - 4) / 16, packed);
		free(data);
	}
	count = list(dir, "ps_", &names);
	for (index = 0; index < count; index++)
	{
		long size;
		unsigned char *data = read_file(dir, names[index], &size);

		if (size != 264)
		{
			fprintf(stderr, "%s: %ld bytes\n", names[index], size);
			return 1;
		}
		pixel(names[index], data);
		free(data);
	}
	synthetic_pixel_programs(synthetic_pixel);
	overlay_programs();
	synthetic_vertex_programs(synthetic_vertex);
	fclose(index_file);
	printf("%lu shaders written to %s, %lu variants the same as one of them\n", written, out_dir, same);
	return 0;
}
