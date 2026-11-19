/*
PRISM_UPSCALE.C

PRISM v4's frame upscaling (overhaul_research/prism_v4_renderer/design.md,
sections 6 and 7; M5; docs/prism_plugin.md): display.upscaler ("off",
"fsr3": FidelityFX Super Resolution 3.1, the renderer's own, or
"plugin:<name>": a plugin the player built), display.render_scale (the share
of the screen's pixels each way the 3D view is drawn at under an upscaler),
display.upscaler_sharpness and display.denoiser (the path tracer's light:
"prism", its own filter, "dlss_rr", DLSS Ray Reconstruction (the plugin
"dlss"), "plugin:<name>" or "off").

A player's window's frame (a mirror's view and its own, render_player_frame)
is drawn at the render scale and its view upscaled into the window as it is
whole and tone mapped, before its HUD (d3d8_device.c's halo_upscale_view_*:
the screen's scale is the view's meanwhile, so that every target of the
screen's the view draws is drawn smaller; xgpu_stream.h's UPSCALE). The
backend jitters the view's draws, finds its motion (the G-buffer's, or the
camera's from its depth), its reactive mask (from its colour before and
after its transparent surfaces, OPAQUE) and upscales it.

Only one player's window (not split screen's), not the VR build's, nor a
view the replay holds or a movie plays over; only the Vulkan backend of the
desktop's builds (the record is ignored elsewhere, and the view would not be
upscaled: none is drawn smaller there). With display.upscaler "off" (the
default) and display.denoiser "prism" nothing is sent and nothing changes:
Classic and Enhanced draw byte for byte as before.
*/

#include "cseries.h"
#include "prism_upscale.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "render_cameras_internal.h"
#include "main/main.h"
#include "bink_playback.h"
#include "network_replay.h"
#include "game/game.h"

#include <string.h>

/* the platform's (port/linux/src) */
char const *config_string(char const *name);
double config_real(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
/* d3d8_device.c's (the view's pixels are the device's) */
int halo_upscale_view_begin(int upscaler, int denoiser, float ratio);
void halo_upscale_view_opaque(void);
void halo_upscale_view_end(short x0, short y0, short x1, short y1, float const camera[4][4], float sharpness,
	float frame_milliseconds);

/* xgpu_stream.h's XGPU_UPSCALE_UPSCALER_* and _DENOISER_* */
enum
{
	_upscaler_none,
	_upscaler_fsr3,
	_upscaler_plugin,
};
enum
{
	_denoiser_prism,
	_denoiser_plugin,
	_denoiser_off,
};

/* display.render_scale's default under an upscaler (0: the upscaler's
"Quality", 1.5 times each way) and its range on the desktop */
#define RENDER_SCALE_DEFAULT 0.67f
#define RENDER_SCALE_LEAST 0.33f

/* only the desktop's Vulkan builds upscale (not Android's, not the VR
build's; the Metal backend ignores the record) */
#if defined(HALO_VULKAN) && !defined(HALO_ANDROID) && !defined(HALO_VR)
#define UPSCALE_BUILT 1
#else
#define UPSCALE_BUILT 0
#endif

static struct
{
	boolean read;
	unsigned long read_at;
	int upscaler, denoiser;
	real scale, sharpness;
	/* the window's view is drawn smaller now (begun) */
	boolean drawing;
	/* the game's time of the last frame upscaled, for the upscaler's time */
	real last_time;
	boolean last_time_known;
} prism_upscale;

/* "plugin:<name>" with a name */
static boolean plugin_named(char const *value)
{
	return !strncmp(value, "plugin:", 7) && value[7] != 0;
}

static void settings_read(
	void)
{
	unsigned long changes = config_changes();
	char const *upscaler, *denoiser;
	int upscaler_was, denoiser_was;
	real scale;

	if (prism_upscale.read && prism_upscale.read_at == changes)
	{
		return;
	}
	upscaler_was = prism_upscale.read ? prism_upscale.upscaler : _upscaler_none;
	denoiser_was = prism_upscale.read ? prism_upscale.denoiser : _denoiser_prism;
	prism_upscale.read = TRUE;
	prism_upscale.read_at = changes;
	upscaler = config_string("display.upscaler");
	denoiser = config_string("display.denoiser");
	prism_upscale.upscaler = !strcmp(upscaler, "fsr3") ? _upscaler_fsr3 : plugin_named(upscaler) ? _upscaler_plugin :
		_upscaler_none;
	prism_upscale.denoiser = plugin_named(denoiser) || !strcmp(denoiser, "dlss_rr") ? _denoiser_plugin :
		!strcmp(denoiser, "off") ? _denoiser_off : _denoiser_prism;
	if (!UPSCALE_BUILT)
	{
		prism_upscale.upscaler = _upscaler_none;
		prism_upscale.denoiser = _denoiser_prism;
	}
	scale = (real)config_real("display.render_scale");
	if (!(scale > 0.0f))
	{
		scale = RENDER_SCALE_DEFAULT;
	}
	prism_upscale.scale = scale < RENDER_SCALE_LEAST ? RENDER_SCALE_LEAST : scale > 1.0f ? 1.0f : scale;
	prism_upscale.sharpness = (real)config_real("display.upscaler_sharpness");
	if (!(prism_upscale.sharpness > 0.0f))
	{
		prism_upscale.sharpness = 0.0f;
	}
	else if (prism_upscale.sharpness > 1.0f)
	{
		prism_upscale.sharpness = 1.0f;
	}
	if (prism_upscale.upscaler != upscaler_was || prism_upscale.denoiser != denoiser_was)
	{
		if (prism_upscale.upscaler == _upscaler_none && strcmp(upscaler, "off"))
		{
			platform_log("upscaler: \"%s\" is %s, so off", upscaler,
				UPSCALE_BUILT ? "unknown" : "not in this build");
		}
		else
		{
			platform_log("upscaler: %s, the view at %.0f%% each way; denoiser: %s", upscaler,
				(double)(prism_upscale.upscaler == _upscaler_none ? 100.0f : prism_upscale.scale * 100.0f), denoiser);
		}
	}
}

int prism_upscale_setting_on(
	void)
{
	settings_read();

	return prism_upscale.upscaler != _upscaler_none;
}

int prism_upscale_denoiser_off(
	void)
{
	settings_read();

	return prism_upscale.denoiser == _denoiser_off;
}

void prism_upscale_player_begin(
	short local_player_index)
{
	prism_upscale.drawing = FALSE;
	settings_read();
	if (prism_upscale.upscaler == _upscaler_none && prism_upscale.denoiser != _denoiser_plugin)
	{
		return;
	}
	/* (one player's window; not a view the replay holds, nor a movie over
	it: drawn as without) */
	if (local_player_index == NONE || main_get_window_count() != 1 || network_replay_frozen() ||
		bink_playback_in_progress())
	{
		return;
	}
	prism_upscale.drawing = halo_upscale_view_begin(prism_upscale.upscaler, prism_upscale.denoiser,
		prism_upscale.upscaler == _upscaler_none ? 1.0f : prism_upscale.scale);
}

void prism_upscale_opaque(
	void)
{
	if (prism_upscale.drawing)
	{
		halo_upscale_view_opaque();
	}
}

void prism_upscale_window_end(
	void const *camera_pointer,
	void const *frustum_pointer)
{
	struct render_camera const *camera = camera_pointer;
	struct render_frustum const *frustum = frustum_pointer;
	float view[4][4];
	real_point2d screen_point;
	real_vector3d view_vector, middle, step;
	real now, milliseconds;
	short axis;

	if (!prism_upscale.drawing)
	{
		return;
	}
	prism_upscale.drawing = FALSE;
	/* (the game's time between the frames upscaled: a tick's at a fixed
	step, as the captures draw them, else the display's) */
	now = (real)game_time_get() / 30.0f + render.time_delta_since_tick_sec;
	milliseconds = prism_upscale.last_time_known && now > prism_upscale.last_time ?
		(now - prism_upscale.last_time) * 1000.0f : 1000.0f / 60.0f;
	prism_upscale.last_time = now;
	prism_upscale.last_time_known = TRUE;
	if (!frustum->projection_valid)
	{
		halo_upscale_view_end(camera->viewport_bounds.x0, camera->viewport_bounds.y0, camera->viewport_bounds.x1,
			camera->viewport_bounds.y1, NULL, prism_upscale.sharpness, milliseconds);
		return;
	}
	/* (as render_window finds world AO's pass's camera, and
	prism_deferred_gbuffer_view SCENE_VIEW's) */
	view[0][0] = camera->position.x;
	view[0][1] = camera->position.y;
	view[0][2] = camera->position.z;
	view[0][3] = frustum->z_near;
	screen_point.x = 0.0f;
	screen_point.y = 0.0f;
	render_camera_screen_to_view(camera, frustum, &screen_point, &view_vector);
	matrix4x3_transform_vector(&frustum->view_to_world, &view_vector, &middle);
	view[1][0] = middle.i;
	view[1][1] = middle.j;
	view[1][2] = middle.k;
	view[1][3] = frustum->z_far;
	for (axis = 0; axis < 2; axis++)
	{
		screen_point.x = axis == 0 ? 1.0f : 0.0f;
		screen_point.y = axis == 1 ? 1.0f : 0.0f;
		render_camera_screen_to_view(camera, frustum, &screen_point, &view_vector);
		matrix4x3_transform_vector(&frustum->view_to_world, &view_vector, &step);
		view[2 + axis][0] = step.i - middle.i;
		view[2 + axis][1] = step.j - middle.j;
		view[2 + axis][2] = step.k - middle.k;
		view[2 + axis][3] = 0.0f;
	}
	halo_upscale_view_end(camera->viewport_bounds.x0, camera->viewport_bounds.y0, camera->viewport_bounds.x1,
		camera->viewport_bounds.y1, (float const (*)[4])view, prism_upscale.sharpness, milliseconds);
}

void prism_upscale_player_end(
	void)
{
	/* (a view that did not reach its end: the screen's scale again, the view
	upscaled as it is) */
	if (prism_upscale.drawing)
	{
		prism_upscale.drawing = FALSE;
		halo_upscale_view_end(0, 0, 0, 0, NULL, prism_upscale.sharpness, 1000.0f / 60.0f);
	}
}
