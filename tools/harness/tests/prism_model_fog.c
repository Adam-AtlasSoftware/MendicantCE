#include "harness.h"

/* PRISM v4's objects' fog (rasterizer_xbox_models.c's
rasterizer_models_port_fog_by_host): in display.lighting's v4 modes an
opaque object's draw leaves its fog to the host's fog pass only while that
pass is still to come in the window (render.h's
render_prism_deferred_fog_pending); the sky's models, a blended part and
every model drawn after the pass keep their own */
boolean render_prism_deferred_window;
boolean render_prism_deferred_fog_pending;
boolean local_sky_flag;
#include "under_test.inc"

static void window(boolean v4, boolean pending, boolean sky)
{
	render_prism_deferred_window = v4;
	render_prism_deferred_fog_pending = pending;
	local_sky_flag = sky;
}

int main(int argc, char **argv)
{
	char const *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "before"))
	{
		/* (the window's opaque objects, before the host's fog pass: the
		host's fog, none of their own) */
		window(TRUE, TRUE, FALSE);
		CHECK(rasterizer_models_port_fog_by_host(FALSE), "an opaque part before the pass keeps its own fog");
		return 0;
	}
	if (!strcmp(which, "after"))
	{
		/* (a model drawn after the pass, in the same window: a partly
		cloaked unit's, the game engine's; the pass did not fog it) */
		window(TRUE, FALSE, FALSE);
		CHECK(!rasterizer_models_port_fog_by_host(FALSE), "a model after the pass drawn without fog");
		return 0;
	}
	if (!strcmp(which, "kept"))
	{
		/* (the sky's models and a blended part keep theirs, before the pass
		too) */
		window(TRUE, TRUE, TRUE);
		CHECK(!rasterizer_models_port_fog_by_host(FALSE), "the sky's model drawn without fog");
		window(TRUE, TRUE, FALSE);
		CHECK(!rasterizer_models_port_fog_by_host(TRUE), "a blended part drawn without fog");
		return 0;
	}
	if (!strcmp(which, "classic"))
	{
		/* (Classic and Enhanced: CE's fog, every draw) */
		window(FALSE, FALSE, FALSE);
		CHECK(!rasterizer_models_port_fog_by_host(FALSE), "Classic's draw without its fog");
		return 0;
	}
	fprintf(stderr, "no case %s\n", which);
	return 2;
}
