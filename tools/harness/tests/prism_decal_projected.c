#include "harness.h"

/* PRISM v4's projected decals, the draw's side (rasterizer_xbox_decals.c's
rasterizer_decal_port_projected): which decals are projected rather than
drawn, with which texture; the bitmap's type the draw's stage asks for (a
2D texture: rasterizer_set_texture's type 0), else CE draws it */
#include "layers.inc"
typedef unsigned long pixel32;
struct tag_reference { long group_tag; long name; long name_length; long index; };
#include "structures.inc"
struct bitmap_data { short type; };
struct bitmap_group { struct { long count; } bitmaps; };

static boolean render_prism_deferred_window;
static short local_layer;
static struct bitmap_group group = { { 2 } };
static struct bitmap_data bitmaps[2];
static char hardware[2];
static int asked;
static struct
{
	long decal_index;
	unsigned long texture;
	short blend;
	unsigned long color, intensity;
	int count;
} sent;

static struct bitmap_group *bitmap_group_get(long index)
{
	return index == 7 ? &group : NULL;
}

static struct bitmap_data *bitmap_group_try_and_get_bitmap(long index, short bitmap_index)
{
	return index == 7 && bitmap_index >= 0 && bitmap_index < 2 ? &bitmaps[bitmap_index] : NULL;
}

static void *_texture_cache_bitmap_get_hardware_format(struct bitmap_data *bitmap, boolean block, boolean load)
{
	(void)block;
	(void)load;
	asked++;
	return &hardware[bitmap - bitmaps];
}

static boolean prism_deferred_decal(long decal_index, unsigned long texture, short blend, unsigned long color,
	unsigned long intensity, boolean alpha_tested)
{
	(void)alpha_tested;
	sent.decal_index = decal_index;
	sent.texture = texture;
	sent.blend = blend;
	sent.color = color;
	sent.intensity = intensity;
	sent.count++;
	return TRUE;
}
#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *which = argc > 1 ? argv[1] : "";
	struct decal_datum decal;
	struct decal_shader_definition shader;

	memset(&decal, 0, sizeof(decal));
	memset(&shader, 0, sizeof(shader));
	decal.bitmap_index = 3;
	decal.color = 0xff336699UL;
	shader.map.index = 7;
	shader.framebuffer_blend_function = 2;
	render_prism_deferred_window = TRUE;
	local_layer = _decal_layer_primary;
	if (!strcmp(which, "projected"))
	{
		/* (a 2D texture's decal of the primary layer: projected, with the
		texture of its bitmap, its sequence's index wrapped by the count) */
		CHECK(rasterizer_decal_port_projected(12, &decal, &shader, 128), "not projected");
		CHECK(sent.count == 1 && sent.decal_index == 12 && sent.texture == (unsigned long)&hardware[1] &&
			sent.blend == 2 && sent.color == 0xff336699UL && sent.intensity == 128, "the record's fields");
		return 0;
	}
	if (!strcmp(which, "type"))
	{
		/* (a volume's or a cube map's bitmap: CE draws it, with its default
		texture; its texture never asked for, nothing sent) */
		bitmaps[1].type = 1;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "a volume's decal projected");
		bitmaps[1].type = 2;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "a cube map's decal projected");
		CHECK(sent.count == 0 && asked == 0, "sent %d, textures asked for %d", sent.count, asked);
		return 0;
	}
	if (!strcmp(which, "drawn"))
	{
		/* (CE's draws: Classic and Enhanced, the water's and the alpha-tested
		layers, a decal with no bitmap) */
		render_prism_deferred_window = FALSE;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "projected in Classic");
		render_prism_deferred_window = TRUE;
		local_layer = _decal_layer_water;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "the water's layer projected");
		local_layer = _decal_layer_alpha_tested;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "the alpha-tested layer projected");
		local_layer = _decal_layer_secondary;
		shader.map.index = NONE;
		CHECK(!rasterizer_decal_port_projected(12, &decal, &shader, 128), "no bitmap, projected");
		CHECK(sent.count == 0, "sent %d", sent.count);
		return 0;
	}
	fprintf(stderr, "no case %s\n", which);
	return 2;
}
