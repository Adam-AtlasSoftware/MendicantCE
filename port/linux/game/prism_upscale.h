/*
PRISM_UPSCALE.H

PRISM v4's frame upscaling and its plugins' settings (prism_upscale.c):
display.upscaler, display.upscaler_scale, display.upscaler_sharpness and
display.denoiser.
*/

#ifndef __PRISM_UPSCALE_H
#define __PRISM_UPSCALE_H

/* a player's window's frame begins (render_frame, before its views are
drawn: a mirror's, then the window's own): its 3D view drawn smaller from
here on where display.upscaler asks for it */
void prism_upscale_player_begin(short local_player_index);
/* the window's opaque surfaces drawn, its transparent ones to come
(render_window) */
void prism_upscale_opaque(void);
/* the window's 3D view whole and tone mapped, before its HUD: upscaled into
the window (render_window; camera and frustum the window's rasterizer's) */
void prism_upscale_window_end(void const *camera, void const *frustum);
/* the window's frame done: the screen's scale again, in case the view did
not reach its end (render_frame) */
void prism_upscale_player_end(void);

/* display.upscaler is not "off" (d3d8_device.c's anti-aliasing: none by
multisampling or supersampling under an upscaler) */
int prism_upscale_setting_on(void);
/* display.denoiser is "off", in a build that upscales (prism_rt.c: the path
tracer's own filter off; elsewhere the setting is "prism"'s) */
int prism_upscale_denoiser_off(void);
/* display.pt_clarity's share of the G-buffer's pixels each way the path
tracer's light is traced at (prism_deferred.c, where display.pt_light_scale
is not set); 0 none */
float prism_upscale_pt_light_share(void);
/* the window's 3D view's share of its pixels each way while it is drawn
for an upscaler (1: DLAA's, at every pixel); 0 while it is not (no
upscaler, or not the window's view) */
float prism_upscale_view_scale(void);

#endif
