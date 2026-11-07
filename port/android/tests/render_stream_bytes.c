/*
RENDER_STREAM_BYTES.C

The renderer's command stream's bytes as the encoder writes them, for
comparing two encoders (tools/test_render_stream.py: this branch's and the
base's, for display.prism_textures stock, whose records must be the same).
The encoder (xgpu_stream_encode.c, built with XGPU_STREAM_TEST) writes a
repeatable run of texture stages, targets, clears and constants, frames
apart, into a ring whose host is this file: each submit's records are
written as they are to the file argv[1], and released at once. INITIALIZE's
answer has the texture store's capability set (a byte the base's
capabilities left unused), so that only the hook decides; with BYTES_MESH,
argv[2] "meshcap" sets the mesh store's capability too (the next byte) and
calls nothing of it (display.prism_models stock: the records must be the
same), and "mesh" sends a model's instance and its parts' draws each frame
(they differ).
*/

#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "xgpu_stream.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void xgpu_stream_encode_configure(unsigned long ring_size, unsigned long frames_in_flight);
void xgpu_stream_encode_close(void);

/* the guest's memory: a window at a base, as host_render_stream_test.c's */
uintptr_t host_guest_base;
static uintptr_t window_next, window_end;
#define WINDOW_SIZE (256ul << 20)

static void *guest(uint32_t address)
{
	return (void *)(host_guest_base + address);
}

void *xgpu_stream_test_allocate(size_t alignment, size_t size)
{
	uintptr_t at = (window_next + alignment - 1) & ~(uintptr_t)(alignment - 1);

	if (at + size > window_end)
		return NULL;
	window_next = at + size;
	memset((void *)at, 0, size);
	return (void *)at;
}

void platform_log(const char *format, ...)
{
	(void)format;
}

/* (the decoding the encoder asks of the interface: none here) */
void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description)
{
	(void)format_word;
	(void)size_word;
	memset(description, 0, sizeof(*description));
}

void nv2a_pixel_program_decode(const struct nv2a_pixel_shader_key *key, struct xgpu_pixel_program *program)
{
	(void)key;
	memset(program, 0, sizeof(*program));
}

#ifdef BYTES_MESH
static int mesh_capability, mesh_calls;
#endif

#ifdef BYTES_HOOK
/* argv[2] "zero": a store hook that has no ids (the guest's with a tier but
no replacement drawn); "ids": one that has some */
static int hook_ids;

static unsigned long hook(unsigned long resource)
{
	return hook_ids ? (resource >> 4) & 3 : 0;
}
#endif

static FILE *output;
static struct xgpu_stream_ring *ring;
static uint32_t written;

int host_render_stream_open(unsigned int address)
{
	ring = guest(address);
	written = 0;
	return 0;
}

void host_render_stream_submit(unsigned int flags)
{
	unsigned char *buffer = guest(ring->buffer);

	(void)flags;
	while (written != ring->committed)
	{
		uint32_t header = *(uint32_t *)(buffer + (written & (ring->size - 1)));
		uint32_t type = XGPU_STREAM_HEADER_TYPE(header), size = XGPU_STREAM_HEADER_SIZE(header);

		if (type == XGPU_STREAM_SKIP && !size)
		{
			written += ring->size - (written & (ring->size - 1));
			continue;
		}
		fwrite(buffer + (written & (ring->size - 1)), 1, size, output);
		if (type == XGPU_STREAM_INITIALIZE)
		{
			struct xgpu_stream_capabilities *answer =
				guest(((const struct xgpu_stream_reply_record *)(buffer + (written & (ring->size - 1))))->reply);

			memset(answer, 0, sizeof(*answer));
			answer->largest_target = 16384;
			/* (texture_store's place: the byte after material_overlay) */
			((uint8_t *)&answer->material_overlay)[1] = 1;
#ifdef BYTES_MESH
			/* (mesh_store's: the byte after it) */
			((uint8_t *)&answer->material_overlay)[2] = (uint8_t)mesh_capability;
#endif
		}
		written += size;
	}
	ring->released = ring->committed;
	ring->frames_retired = ring->frames_submitted;
}

void host_render_stream_wait(unsigned int position)
{
	(void)position;
}

void host_render_stream_close(void)
{
}

static uint32_t seed = 12345;

static uint32_t next(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

int main(int argc, char **argv)
{
	void *window;
	D3DBaseTexture *textures[16];
	struct xgpu_texture_source stages[4];
	int index, frame;

	if (argc < 2 || !(output = fopen(argv[1], "wb")))
		return 2;
	/* (a window at a 4 GB boundary: a guest address is a pointer's low 32
	bits) */
	window = mmap(NULL, 8ul << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (window == MAP_FAILED)
		return 2;
	host_guest_base = ((uintptr_t)window + (1ul << 32) - 1) & ~((1ul << 32) - 1);
	if (mprotect((void *)host_guest_base, WINDOW_SIZE, PROT_READ | PROT_WRITE))
		return 2;
	window_next = host_guest_base + 0x10000;
	window_end = host_guest_base + WINDOW_SIZE;
	for (index = 0; index < 16; index++)
	{
		textures[index] = xgpu_stream_test_allocate(16, sizeof(*textures[index]));
		textures[index]->Data = 0x1000u * (uint32_t)(index + 1);
		textures[index]->Format = 0x00010029u | ((uint32_t)index << 20);
		textures[index]->Size = (uint32_t)index * 7;
	}
	xgpu_stream_encode_configure(1u << 20, 2);
#ifdef BYTES_MESH
	mesh_capability = argc > 2 && (!strcmp(argv[2], "meshcap") || !strcmp(argv[2], "mesh"));
	mesh_calls = argc > 2 && !strcmp(argv[2], "mesh");
#endif
	xgpu_backend_initialize();
#ifdef BYTES_HOOK
	if (argc > 2 && strcmp(argv[2], "none"))
	{
		hook_ids = !strcmp(argv[2], "ids");
		xgpu_backend_texture_store_hook(hook);
	}
#endif
	memset(stages, 0, sizeof(stages));
	for (frame = 0; frame < 200; frame++)
	{
		int draw;

		for (draw = 0; draw < 20; draw++)
		{
			float color[4] = { (float)(next() % 256) / 255.0f, 0.5f, 0.25f, 1.0f };
			float constants[2][4];
			int stage;

			for (stage = 0; stage < 4; stage++)
			{
				struct xgpu_texture_source *source = &stages[stage];

				if (next() % 3)
					continue;
				memset(source, 0, sizeof(*source));
				source->source = (unsigned char)(next() % 3);
				source->sampler_type = (unsigned char)(next() % 4);
				source->sampled = 1;
				if (source->source == XGPU_SOURCE_TEXTURE)
				{
					D3DBaseTexture *texture = textures[next() % 16];

					source->resource = (DWORD)(uintptr_t)texture;
					source->data = texture->Data;
					source->width = 1u << (next() % 10);
					source->height = 1u << (next() % 10);
					source->levels = 1 + next() % 10;
				}
				else if (source->source == XGPU_SOURCE_TARGET)
				{
					source->target = 1 + next() % 8;
				}
				source->sampler.minify = (unsigned char)(next() % 3);
				source->sampler.mip = (unsigned char)(next() % 3);
				source->sampler.magnify = (unsigned char)(next() % 3);
				source->sampler.lod_bias = (float)(next() % 100) / 37.0f;
				source->sampler.anisotropy = next() % 17;
			}
			xgpu_backend_set_textures(stages);
			constants[0][0] = constants[1][1] = (float)(next() % 1000);
			constants[0][1] = constants[0][2] = constants[0][3] = constants[1][0] = constants[1][2] = constants[1][3] = 0.0f;
			xgpu_backend_set_constants(next() % 96, constants, 2);
#ifdef BYTES_MESH
			/* (display.prism_models: a model's nodes and a part's draws named) */
			if (mesh_calls && !(draw % 5))
			{
				float matrices[19][3][4];

				memset(matrices, 0, sizeof(matrices));
				matrices[0][0][0] = matrices[0][1][1] = matrices[0][2][2] = (float)(next() % 7);
				xgpu_backend_mesh_instance((unsigned long)(frame * 20 + draw + 1), 0x00100001ul, 0xe0010004ul, 400.0f,
					0, (const float (*)[3][4])matrices, 19);
				xgpu_backend_mesh_draw((unsigned long)(frame * 20 + draw + 1), 0, 0, 2, XGPU_STREAM_MESH_DRAW_CARRIER, 0, 2);
				xgpu_backend_mesh_draw(0, 0, 0, 0, 0, 0, 0);
			}
#endif
			if (!(draw % 7))
				xgpu_backend_clear(16, color, 1.0f, 0, NULL, 0);
		}
		xgpu_backend_present(1);
		xgpu_backend_frame_end();
	}
	xgpu_stream_encode_close();
	fclose(output);
	return 0;
}
