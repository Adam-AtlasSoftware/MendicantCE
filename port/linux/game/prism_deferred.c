/*
PRISM_DEFERRED.C

PRISM v4's deferred core, its first step (overhaul_research/prism_v4_renderer
design.md, M1): display.lighting "ce_deferred", the design's diagnostic "CE
light, deferred". The window's opaque surfaces are drawn as the game draws
them, but CE's environment passes go to targets of their own rather than
into one another, and a resolve puts the frame together from them before
the transparent geometry, the fog and the effects are drawn over it as
before:

  the light: the sky, the objects' opaque parts, the lightmap pass (with the
    self-illumination it folds in), the objects' shadows, the diffuse
    lights and the light decals: the window's own colour target, as now;
  the base: the diffuse texture pass (base map, detail maps, micro detail:
    CE's material colour) and the decals over it (GBUFFER_PHASE_BASE), into
    an image of its own that starts white, with the light's alpha;
  the additions: the specular lights, specular lightmaps, reflection masks,
    mirrors and reflections (GBUFFER_PHASE_ADDS), into an image that starts
    black, with the base's alpha (their blend reads the base pass's
    destination alpha, CE's specular mask);
  the resolve (GBUFFER_PHASE_RESOLVE): light x base + additions, the
    additions' alpha.

That is CE's own arithmetic (the lightmap pass's light, multiplied by the
texture pass's colour, the specular and reflection passes added), so the
frame is Classic's up to rounding and the decals (drawn over the base, not
the lit colour): the equivalence gate (docs/prism_gates.md) checks that the
targets carry every surface. debug.gbuffer_view shows one of them in the
resolve's place. The objects' opaque parts are drawn lit in one pass by CE
(their light is in their combiners), so they stay in the light target: their
base colour and light apart are the next step (the uber-shaders per family).

Only the primary window's view, not a mirror's nor the VR mode's eyes, and
only where the host draws it (the Vulkan backend; the others ignore the
records and draw as Classic). With "ce" (the default) no record is sent and
nothing changes.
*/

#include "cseries.h"
#include "prism_deferred.h"

#include <string.h>

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
unsigned long config_changes(void);
void xgpu_backend_gbuffer_phase(unsigned long phase);

/* xgpu_stream.h's GBUFFER_PHASE values */
#define PHASE_BASE 1ul
#define PHASE_ADDS 2ul
#define PHASE_RESOLVE 3ul
#define PHASE_VIEW_SHIFT 8

static struct
{
	unsigned long settings_read;
	boolean wanted;
	unsigned long view;
	/* the window's phase sent last (0 none: the light) */
	unsigned long phase;
} prism_deferred;

static void settings_read(
	void)
{
	char const *value;

	if (prism_deferred.settings_read == config_changes() + 1)
	{
		return;
	}
	prism_deferred.settings_read = config_changes() + 1;
	value = config_string("display.lighting");
	prism_deferred.wanted = value && !strcmp(value, "ce_deferred");
	value = config_string("debug.gbuffer_view");
	prism_deferred.view = !value ? 0 : !strcmp(value, "base") ? 1 : !strcmp(value, "light") ? 2 :
		!strcmp(value, "additions") ? 3 : 0;

	return;
}

boolean prism_deferred_wanted(
	void)
{
	settings_read();

	return prism_deferred.wanted;
}

void prism_deferred_phase(
	short phase)
{
	unsigned long value;

	switch (phase)
	{
	case _prism_deferred_base:
		value = PHASE_BASE;
		break;
	case _prism_deferred_additions:
		value = PHASE_ADDS;
		break;
	default:
		value = PHASE_RESOLVE | (prism_deferred.view << PHASE_VIEW_SHIFT);
		break;
	}
	prism_deferred.phase = value;
	xgpu_backend_gbuffer_phase(value);

	return;
}
