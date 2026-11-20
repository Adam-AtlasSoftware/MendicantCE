#version 460
/* XGPU_VULKAN_WATER.GLSL

The terrain overhaul's water layer (T7, xgpu_vulkan_water.c; docs/prism_terrain.md "T7"): a CE water group's pixels
composited after CE's depth-only draw, from what the game drew for the host (rasterizer_xbox_water.c's
rasterizer_water_draw_prism): what is behind the water (the colour before it), CE's ripple map at CE's
own coordinates, CE's base map (its colour, or its alpha; the capture's alpha the water's mask), and the G-buffer's
distances.

Each pixel of the view: where the capture draws drew (the water's pixels in front of what is behind), the water's
surface (the group's
plane met by the pixel's ray, its normal bent by CE's ripples), lit as water is: Fresnel at an IOR of 1.33, a
reflection found along the screen (the frame behind, where the reflected ray meets a surface it has; the sky where
it leaves the frame's top through sky; CE's reflection cube, in CE's tint, where it finds neither), a refraction
(the frame behind the water at the refracted ray's floor, absorbed over its path by Beer and Lambert in CE's tint's
colours, in-scattered toward its deep colour), the sun's highlight off the ripples (GGX); from under the surface,
Snell's window and total internal reflection. Everywhere else, the frame as it was behind the water (what the
game's capture draws left there put back). Colour is composed in light (the frame's values to a power, display.
gamma's 2.2 where the frame is encoded) and put back in the frame's encoding.

In Path Traced (T8) the reflection and the refraction are traced (xgpu_vulkan_pt.glsl's pass "water": the bake's
scene, the hit lit from the bake, in the frame's encoding): traced_up the light along the upper ray (above the
surface the reflection, below it Snell's window's), traced_down the light along the lower ray (above, the
refraction to the floor; below, the reflection back into the water); traced_aux the sun's visibility at the surface,
the upper ray's share that left the level (there the sky as the frame shows it: CE's sky, as Rebaked's reflection
finds it; the scene's sky is the light's, not the picture's) and the lower ray's length in the water (-1: none). The
traced floor takes CE's planar fog under the surface as CE's frame has it. The reflection is filtered here over the
roughness's
footprint (the water's own pixels alone: the denoiser's guide is the mask); the reference's (debug.pt_reference) is
summed there over the same footprint's directions, so it is taken as it is. */

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D behind;
layout(set = 0, binding = 1) uniform sampler2D ripples;
layout(set = 0, binding = 2) uniform sampler2D base;
layout(set = 0, binding = 3) uniform sampler2D depth;
layout(set = 0, binding = 4) uniform usampler2D words;
layout(set = 0, binding = 5) uniform usampler2D extras;
layout(set = 0, binding = 6) uniform samplerCube cube;
layout(set = 0, binding = 7) uniform sampler2D traced_up;
layout(set = 0, binding = 8) uniform sampler2D traced_down;
layout(set = 0, binding = 9) uniform sampler2D traced_aux;
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2D result;

layout(push_constant) uniform Push
{
    /* the camera (SCENE_VIEW's: the eye, the axis's unit, its change a unit of the screen's x and y) and the view's
    corners in the target's pixels */
    vec4 camera[4];
    ivec4 corners;
    /* the group's plane (its normal, the distance: dot(n, p) = d on it, the water's surface facing n) */
    vec4 plane;
    /* the sun: the direction toward it (w 1: a sun) and its colour (its largest channel 1) */
    vec4 sun_direction;
    vec4 sun_color;
    /* CE's tints seen straight on and edge on (rgb, alpha) */
    vec4 perpendicular;
    vec4 parallel;
    /* x the seconds; y the ripples' strength; z the absorption's (a world unit's, by CE's tint); w the sun's */
    vec4 tuning;
    /* x the deep colour's brightness; y the frame's power (2.2 encoded, 1 light); z the reflection's share of
    CE's cube; w the debug view (0 none; 1 ripples, 2 base, 3 the mask and depth, 4 reflection, 5 refraction) */
    vec4 more;
    /* x the shader's flags (XGPU_TERRAIN_WATER_*), y and z the target's size, w: 1 Path Traced, 2 an opaque
    water, 4 its rays traced (traced_up and traced_down), 8 the reference's (summed over the footprint) */
    uvec4 flags;
    /* the window's planar fog: CE's colour and maximum density (0 none); its maximum depth and distance, the eye's
    height over its plane */
    vec4 fog;
    vec4 fog_shape;
} pc;

const uint ALPHA_MODULATES_REFLECTION = 1u;
const uint COLOR_MODULATES_BACKGROUND = 2u;
const uint BEFORE_FOG = 8u;
const uint HAS_BASE = 256u;
const float NONE = 1.0e7;
const float PI = 3.14159265;

mat3 to_camera;

vec3 ray_of(vec2 pixel)
{
    vec4 c = vec4(pc.corners);
    vec2 sxy = vec2(2.0 * (pixel.x + 0.5 - c.x) / max(c.z - c.x, 1.0) - 1.0,
        2.0 * (pixel.y + 0.5 - c.y) / max(c.w - c.y, 1.0) - 1.0);
    return pc.camera[1].xyz + sxy.x * pc.camera[2].xyz + sxy.y * pc.camera[3].xyz;
}

/* a direction from the eye's pixel (its distance along the axis: axis; off the view, or behind: -1) */
vec2 pixel_of_direction(vec3 d, out float axis)
{
    vec3 u = to_camera * d;
    axis = u.x;
    if (u.x <= 1.0e-6)
        return vec2(-1.0e6);
    vec2 sxy = u.yz / u.x;
    vec4 c = vec4(pc.corners);
    return vec2((sxy.x + 1.0) * 0.5 * (c.z - c.x) + c.x - 0.5, (sxy.y + 1.0) * 0.5 * (c.w - c.y) + c.y - 0.5);
}

bool inside(vec2 p)
{
    vec4 c = vec4(pc.corners);
    return p.x >= c.x && p.y >= c.y && p.x <= c.z - 1.0 && p.y <= c.w - 1.0;
}

/* the opaque surface's distance along the axis at a pixel (NONE: none, the sky) */
float opaque_at(ivec2 p)
{
    uvec4 w = texelFetch(words, p, 0);
    uvec4 x = texelFetch(extras, p, 0);
    float d = uintBitsToFloat(x.w);
    bool present = (w.w & 0x8000u) != 0u && d > 0.0 && (x.w & 0x7f800000u) != 0x7f800000u;
    return present ? d : NONE;
}

/* an opaque water's layer (flags.w bit 1): over the resolved frame, the level's pixels of a material of the water
class (the classes' table in the base's place: a texel a material, its two classes) */
bool opaque_mode()
{
    return (pc.flags.w & 2u) != 0u;
}

/* the water's pixels: the capture's alpha (the capture draws' 1, the rest emptied at BEGIN); an opaque water's, its
material's class */
bool water_at(ivec2 p)
{
    if (opaque_mode())
    {
        uvec4 w = texelFetch(words, p, 0);
        uint material = w.w & 0xfffu;
        if ((w.w & 0x8000u) == 0u || (w.w >> 16) != 0u || material == 0u)
            return false;
        vec2 classes = texelFetch(base, ivec2(int(material & 63u), int(material >> 6)), 0).rg * 255.0;
        return abs(classes.x - 7.0) < 0.5 || abs(classes.y - 7.0) < 0.5;
    }
    return texelFetch(base, p, 0).a > 0.5;
}

/* an octahedral normal (the G-buffer's) */
vec3 unfold(vec2 e)
{
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

vec3 light_of(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(pc.more.y));
}

vec3 frame_of(vec3 c)
{
    return pow(max(c, vec3(0.0)), vec3(1.0 / pc.more.y));
}

float schlick(float f0, float c)
{
    float m = clamp(1.0 - c, 0.0, 1.0);
    return f0 + (1.0 - f0) * m * m * m * m * m;
}

/* the frame behind at a pixel, blurred over the reflection's footprint (its roughness: the ripples finer than a
pixel far off), in light: nine taps over an ellipse blur_radius wide and blur_stretch times as tall (a rough
water's reflections stretch toward the eye: the ripples tilt the reflected rays most in the plane of the view) */
float blur_radius, blur_stretch = 1.0;
const vec2 TAPS[8] = vec2[8](vec2(0.92, 0.18), vec2(-0.71, 0.55), vec2(0.21, -0.94), vec2(-0.38, -0.36),
    vec2(0.45, 0.62), vec2(-0.96, -0.12), vec2(0.62, -0.47), vec2(-0.12, 0.98));
vec3 behind_blurred(vec2 q)
{
    vec2 size = vec2(pc.flags.yz);
    vec3 sum = light_of(textureLod(behind, (q + 0.5) / size, 0.0).rgb) * 2.0;
    if (blur_radius < 0.75)
        return sum * 0.5;
    for (int k = 0; k < 8; k++)
    {
        vec2 o = TAPS[k] * vec2(blur_radius, blur_radius * blur_stretch);
        sum += light_of(textureLod(behind, (q + o + 0.5) / size, 0.0).rgb);
    }
    if (blur_radius < 6.0)
        return sum / 10.0;
    /* (a wide footprint (a far, rough sea's) eight taps more between them, turned and nearer: so few taps over
    so wide a footprint would show its pattern, not its blur) */
    for (int k = 0; k < 8; k++)
    {
        vec2 o = vec2(TAPS[k].y, -TAPS[k].x) * 0.55 * vec2(blur_radius, blur_radius * blur_stretch);
        sum += light_of(textureLod(behind, (q + o + 0.5) / size, 0.0).rgb);
    }
    return sum / 18.0;
}

/* the reflection along the screen from p (its axis distance) toward r: the frame where it meets a surface on the
side of the plane above (above: the water seen from above; else from below), its weight (0 none) */
vec4 screen_reflection(vec3 p, vec3 r, float axis0, bool above)
{
    vec3 eye = pc.camera[0].xyz;
    float s0 = 0.04 + 0.012 * axis0;
    float travelled = 0.0, last = 0.0;
    float step_length = s0;
    for (int i = 0; i < 40; i++)
    {
        travelled += step_length;
        step_length *= 1.14;
        vec3 x = p + r * travelled;
        float axis;
        vec2 q = pixel_of_direction(x - eye, axis);
        if (axis <= 0.0 || !inside(q))
            return vec4(0.0);
        float opaque = opaque_at(ivec2(q));
        float thickness = max(0.25, 0.04 * axis) + step_length;
        if (opaque < NONE && axis > opaque && axis - opaque < thickness)
        {
            /* (a few halvings between the last step and this) */
            float a = last, b = travelled;
            for (int k = 0; k < 5; k++)
            {
                float m = 0.5 * (a + b);
                float am;
                vec2 qm = pixel_of_direction(p + r * m - eye, am);
                float om = opaque_at(ivec2(clamp(qm, vec2(pc.corners.xy), vec2(pc.corners.zw) - 1.0)));
                if (om < NONE && am > om)
                    b = m;
                else
                    a = m;
            }
            float ab;
            q = pixel_of_direction(p + r * b - eye, ab);
            if (!inside(q))
                return vec4(0.0);
            ivec2 hit = ivec2(q);
            float o = opaque_at(hit);
            if (o >= NONE)
                return vec4(0.0);
            vec3 surface = eye + o * ray_of(vec2(hit));
            float side = dot(pc.plane.xyz, surface) - pc.plane.w;
            if (above ? side < -0.1 : side > 0.1)
                return vec4(0.0);
            /* (faded toward the view's edges) */
            vec4 c = vec4(pc.corners);
            vec2 edge = min(q - c.xy, c.zw - q) / max(c.zw - c.xy, vec2(1.0));
            float fade = clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0);
            return vec4(behind_blurred(q), fade);
        }
        last = travelled;
    }
    return vec4(0.0);
}

/* what lies far along the reflected ray, where the frame shows it (weight 0: off the view, or water there): the sky,
or the far surfaces in their fog, as a reflection of the world at a great distance would show them; the reflection
along the screen replaces it where it finds the surface itself */
vec4 far_reflection(vec3 r, bool above)
{
    float axis;
    vec2 q = pixel_of_direction(r * 1.0e4, axis);
    if (axis <= 0.0 || !inside(q))
        return vec4(0.0);
    ivec2 p = ivec2(q);
    if (water_at(p))
        return vec4(0.0);
    vec4 c = vec4(pc.corners);
    vec2 edge = min(q - c.xy, c.zw - q) / max(c.zw - c.xy, vec2(1.0));
    return vec4(behind_blurred(q), clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0));
}

float schlick(float f0, float c);

bool traced() { return (pc.flags.w & 4u) != 0u; }
bool reference() { return (pc.flags.w & 8u) != 0u; }

/* the traced reflection over the roughness's footprint (as behind_blurred's taps), the water's own pixels alone
(the mask the filter's guide: no light of the shore or of what stands in the water taken into it); the
reference's as it was summed */
vec3 traced_blurred(ivec2 pixel)
{
    vec2 size = vec2(pc.flags.yz);
    vec3 sum = light_of(texelFetch(traced_up, pixel, 0).rgb) * 2.0;
    if (reference() || blur_radius < 0.75)
        return sum * 0.5;
    float weight = 2.0;
    for (int k = 0; k < 8; k++)
    {
        vec2 o = TAPS[k] * vec2(blur_radius, blur_radius * blur_stretch);
        ivec2 q = ivec2(clamp(vec2(pixel) + o, vec2(0.0), size - 1.0));
        if (!water_at(q) || texelFetch(traced_aux, q, 0).y > 0.5)
            continue;
        sum += light_of(texelFetch(traced_up, q, 0).rgb);
        weight += 1.0;
    }
    return sum / weight;
}

/* the sun's visibility at a point of the surface, along the screen (Rebaked): the sun's ray marched over the
frame's distances; a surface the ray passes behind, within a thickness, shades it (the cliffs' shadow on the
water, the logs'), fading out toward the view's edges where the frame cannot say */
float sun_seen_screen(vec3 p, vec3 l)
{
    vec3 eye = pc.camera[0].xyz;
    float travelled = 0.0, step_length = 0.15, shade = 0.0;
    for (int i = 0; i < 24; i++)
    {
        travelled += step_length;
        step_length *= 1.22;
        vec3 x = p + l * travelled;
        float axis;
        vec2 q = pixel_of_direction(x - eye, axis);
        if (axis <= 0.0 || !inside(q))
            break;
        float opaque = opaque_at(ivec2(q));
        if (opaque < NONE && axis > opaque + 0.05 && axis - opaque < max(1.5, 0.08 * axis) + step_length)
        {
            vec4 c = vec4(pc.corners);
            vec2 edge = min(q - c.xy, c.zw - q) / max(c.zw - c.xy, vec2(1.0));
            shade = clamp(min(edge.x, edge.y) * 12.0, 0.0, 1.0);
            break;
        }
        if (travelled > 120.0)
            break;
    }
    return 1.0 - shade;
}

/* Fresnel's reflectance of an unpolarised ray from a medium of index eta_i into one of eta_t at the cosine c (1
past the critical angle) */
float fresnel_exact(float c, float eta_i, float eta_t)
{
    float e = eta_i / eta_t;
    float s2 = e * e * (1.0 - c * c);
    if (s2 >= 1.0)
        return 1.0;
    float ct = sqrt(1.0 - s2);
    float rs = (eta_i * c - eta_t * ct) / (eta_i * c + eta_t * ct);
    float rp = (eta_t * c - eta_i * ct) / (eta_t * c + eta_i * ct);
    return 0.5 * (rs * rs + rp * rp);
}

/* the sun's GGX highlight off a normal (a roughness), by its visibility */
vec3 sun_highlight(vec3 normal, vec3 v, float rough, float seen)
{
    if (pc.sun_direction.w < 0.5 || seen <= 0.0)
        return vec3(0.0);
    vec3 l = normalize(pc.sun_direction.xyz);
    vec3 h = normalize(l + v);
    float nl = clamp(dot(normal, l), 0.0, 1.0), nv = max(dot(normal, v), 1.0e-3), nh = clamp(dot(normal, h), 0.0, 1.0);
    float a2 = rough * rough * rough * rough;
    float den = nh * nh * (a2 - 1.0) + 1.0;
    float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1.0e-4);
    return min(a2 / (PI * den * den) * vis * schlick(0.02, clamp(dot(v, h), 0.0, 1.0)) * nl, 64.0) *
        light_of(pc.sun_color.rgb) * pc.tuning.w * seen;
}

/* an opaque water (d40's): CE's colour of it kept as what the surface shows through (no refraction: the frame has
nothing under it), its own bump's normal flattened toward the level (most edge on, where its texels streak into
the reflection), reflection along the screen and of the sky in CE's colour's hue (a muddy, opaque water reflects
little and tints what it does), a Fresnel under a clear water's, the sun's highlight in the shadows the screen
finds; and the shore: the layer fades out where the water's own pixels thin out about the pixel */
vec3 opaque_water(ivec2 pixel, vec3 eye, vec3 was)
{
    vec3 ray = ray_of(vec2(pixel));
    float t = opaque_at(pixel);
    vec3 p = eye + t * ray;
    vec3 v = -normalize(ray);
    vec3 n = vec3(0.0, 0.0, 1.0);
    uvec4 w = texelFetch(words, pixel, 0);
    vec3 bump = ((w.z >> 28u) & 8u) != 0u ? unfold(unpackSnorm2x16(w.y)) : n;
    float view_length = t * length(ray);
    float far_fade = smoothstep(8.0, 160.0, view_length);
    float edge_on = clamp(abs(dot(normalize(ray), n)) * 3.0, 0.0, 1.0);
    float strength = pc.tuning.y * 1.2 * mix(1.0, 0.35, far_fade) * mix(0.15, 1.0, edge_on * edge_on);
    vec3 normal = normalize(n + strength * (bump - n * dot(bump, n)));
    float scale = float(pc.flags.z) / 720.0;
    blur_radius = (2.0 + 14.0 * far_fade) * mix(2.0, 1.0, edge_on) * scale;
    blur_stretch = mix(2.6, 1.0, edge_on);
    float fresnel = min(schlick(0.02, clamp(dot(v, normal), 0.0, 1.0)) * 0.55, 0.3);
    vec3 r = reflect(-v, normal);
    if (dot(r, n) < 0.02)
        r = normalize(r + n * (0.02 - dot(r, n)));
    vec3 body = light_of(was);
    vec4 sky = far_reflection(r, true);
    vec3 reflection = mix(body, sky.rgb, sky.a);
    vec4 found = screen_reflection(p, r, t, true);
    reflection = mix(reflection, found.rgb, found.a);
    /* (in CE's colour's hue: its luminance kept, half its colour the water's) */
    float body_luma = max(dot(body, vec3(0.2126, 0.7152, 0.0722)), 1.0e-4);
    float reflection_luma = dot(reflection, vec3(0.2126, 0.7152, 0.0722));
    reflection = mix(reflection, body * (reflection_luma / body_luma), 0.55);
    float seen = pc.sun_direction.w > 0.5 ? sun_seen_screen(p + n * 0.05, normalize(pc.sun_direction.xyz)) : 0.0;
    vec3 highlight = sun_highlight(normal, v, 0.12 + 0.16 * far_fade, seen) * 0.6;
    /* (the shore: the share of water pixels on two rings about the pixel) */
    float share = 0.0;
    for (int k = 0; k < 8; k++)
    {
        vec2 o = TAPS[k] * 7.0 * scale;
        share += water_at(ivec2(clamp(vec2(pixel) + o, vec2(0.0), vec2(pc.flags.yz) - 1.0))) ? 1.0 : 0.0;
        share += water_at(ivec2(clamp(vec2(pixel) + o * 2.2, vec2(0.0), vec2(pc.flags.yz) - 1.0))) ? 1.0 : 0.0;
    }
    float shore = smoothstep(0.35, 0.95, share / 16.0);
    int view = int(pc.more.w + 0.5);
    if (view == 4)
        return reflection;
    return mix(body, body * (1.0 - fresnel) + reflection * fresnel + highlight, shore);
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= int(pc.flags.y) || pixel.y >= int(pc.flags.z))
        return;
    vec4 was = texelFetch(behind, pixel, 0);
    if (!inside(vec2(pixel)) || !water_at(pixel))
    {
        imageStore(result, pixel, was);
        return;
    }
    to_camera = inverse(mat3(pc.camera[1].xyz, pc.camera[2].xyz, pc.camera[3].xyz));
    vec3 eye = pc.camera[0].xyz;
    if (opaque_mode())
    {
        imageStore(result, pixel, vec4(frame_of(opaque_water(pixel, eye, was.rgb)), was.a));
        return;
    }
    vec3 n = normalize(pc.plane.xyz);
    float d = pc.plane.w / max(length(pc.plane.xyz), 1.0e-6);
    vec3 ray = ray_of(vec2(pixel));
    float eye_side = dot(n, eye) - d;
    bool above = eye_side >= 0.0;
    float facing = dot(n, ray);
    float t = abs(facing) > 1.0e-6 ? -eye_side / facing : -1.0;
    float opaque0 = opaque_at(pixel);
    if (t <= 0.0)
        t = min(opaque0, 1.0e4);
    vec3 p = eye + t * ray;
    float view_length = t * length(ray);
    vec3 v = -normalize(ray);
    vec2 size = vec2(pc.flags.yz);
    vec2 uv = (vec2(pixel) + 0.5) / size;
    float scale = size.y / 720.0;

    /* the normal: CE's ripples (its bump map's signed x and y) about the plane's, fading with distance (their detail
    then the roughness's); and the same ripples smoothed over a few pixels (Snell's window's edge decided by them:
    the critical angle is a hard line, which CE's ripples' every texel would otherwise make jagged) */
    vec3 rip = texelFetch(ripples, pixel, 0).rgb * 2.0 - 1.0;
    vec3 smooth_rip = rip * 0.2;
    for (int k = 0; k < 8; k++)
    {
        ivec2 q = ivec2(clamp(vec2(pixel) + TAPS[k] * 5.0 * scale, vec2(0.0), size - 1.0));
        smooth_rip += (texelFetch(ripples, q, 0).rgb * 2.0 - 1.0) * 0.1;
    }
    vec3 tangent = normalize(abs(n.z) < 0.9 ? cross(vec3(0.0, 0.0, 1.0), n) : cross(n, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(n, tangent);
    float far_fade = smoothstep(8.0, 160.0, view_length);
    /* (CE's ripples flattened a little edge on and far off, where its map's texels would alias into blocks; what
    they lose the reflection's blur keeps as roughness: far off and edge on the reflection is the rough sea's, not
    a mirror's) */
    float edge_on = clamp(abs(dot(normalize(ray), n)) * 3.0, 0.0, 1.0);
    float strength = pc.tuning.y * mix(1.0, 0.6, far_fade) * mix(0.6, 1.0, edge_on);
    float grazing_rough = far_fade * (1.0 - edge_on);
    blur_radius = (1.0 + 4.0 * far_fade + 16.0 * grazing_rough) * mix(1.8, 1.0, edge_on) * scale;
    blur_stretch = mix(2.4, 1.0, edge_on);
    vec3 normal = normalize(n + strength * (rip.x * tangent + rip.y * bitangent));
    vec3 normal_smooth = normalize(n + strength * (smooth_rip.x * tangent + smooth_rip.y * bitangent));
    vec4 base_texel = texelFetch(base, pixel, 0);
    bool has_base = (pc.flags.x & HAS_BASE) != 0u;
    vec3 base_color = has_base && (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_texel.rgb : vec3(1.0);
    float mask = has_base && (pc.flags.x & COLOR_MODULATES_BACKGROUND) == 0u &&
        (pc.flags.x & ALPHA_MODULATES_REFLECTION) != 0u ? base_texel.r : 1.0;

    /* CE's tints: its absorption's colours (the background's tint where its base map's colour is it, else the
    straight-on tint's hue) and its deep colour */
    vec3 tint = pc.perpendicular.rgb;
    vec3 hue = tint / max(max(tint.r, max(tint.g, tint.b)), 1.0e-3);
    vec3 absorb_color = (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_color : hue;
    vec3 sigma = pc.tuning.z * -log(clamp(absorb_color, vec3(0.03), vec3(1.0))) + 0.015;
    /* (the deep colour: CE's planar fog's under the water, its own colour of the water's depths, where the window
    has one; else the tint's) */
    vec3 deep = light_of(tint) * 0.6 * pc.more.x;
    if ((pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u)
        deep = light_of(base_color * tint) * 0.6 * pc.more.x;
    if (pc.fog.a > 0.0 && (pc.flags.x & BEFORE_FOG) == 0u)
        deep = light_of(pc.fog.rgb) * pc.more.x;

    /* (a water CE draws before the fog (c10's swamp): the share CE's planar fog took of what is behind it (its
    colour's part of the frame there; the floor under a swamp dark), which Classic's fog laid over the water, and
    its colour) */
    vec3 fog_color = clamp(pc.fog.rgb, vec3(0.0), vec3(1.0));
    float fogged = 0.0;
    if ((pc.flags.x & BEFORE_FOG) != 0u && pc.fog.a > 0.0)
        fogged = clamp(dot(was.rgb, fog_color) / max(dot(fog_color, fog_color), 1.0e-4), 0.0, 1.0) *
            clamp(pc.fog.a, 0.0, 1.0);
    /* (and CE's base map's colour over the floor under that fog, as Classic's colour draw modulates what is
    behind before the fog comes over it) */
    vec3 under_fog = (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_color : vec3(1.0);

    vec3 color;
    vec3 reflected_shown = vec3(0.0), refracted_shown = vec3(0.0);
    if (above)
    {
        float cos_v = clamp(dot(v, normal), 0.0, 1.0);
        /* (a rough sea's Fresnel edge on is less than a mirror's: its facets turned from the eye shadow and mask
        the grazing reflection) */
        float fresnel = schlick(0.02, cos_v) * (1.0 - 0.5 * grazing_rough);
        vec3 r = reflect(-v, normal);
        float under = dot(r, n);
        if (under < 0.02)
            r = normalize(r + n * (0.02 - under));
        vec3 reflection;
        float sun_seen;
        /* the sky's reflection: the sky through the frame, else CE's cube in CE's tint; far off and edge on a
        rough sea's reflection of the sky is its broad light, not its clouds' shapes (CE's cube at its widest blur) */
        vec3 ce_tint = mix(pc.parallel.rgb, pc.perpendicular.rgb, clamp(dot(v, n), 0.0, 1.0));
        vec3 environment = light_of(textureLod(cube, r, 1.0 + 3.0 * grazing_rough).rgb * ce_tint) * pc.more.z;
        vec4 sky = far_reflection(r, true);
        environment = mix(environment, sky.rgb, sky.a);
        vec3 broad = light_of(textureLod(cube, r, 6.0).rgb * ce_tint) * pc.more.z;
        vec4 aux = texelFetch(traced_aux, pixel, 0);
        if (traced())
        {
            /* (Path Traced: the traced reflection, filtered over its footprint, the sky where it left the level;
            the sun's visibility traced) */
            environment = mix(environment, mix(broad, environment, 0.5), grazing_rough);
            reflection = mix(traced_blurred(pixel), environment, clamp(aux.y, 0.0, 1.0));
            sun_seen = aux.x;
        }
        else
        {
            /* the reflection: along the screen, else the sky's */
            vec4 found = screen_reflection(p, r, t, true);
            reflection = mix(environment, found.rgb, found.a);
            reflection = mix(reflection, mix(broad, reflection, 0.5), grazing_rough * (1.0 - found.a));
            sun_seen = pc.sun_direction.w > 0.5 ? sun_seen_screen(p + n * 0.05, normalize(pc.sun_direction.xyz)) :
                0.0;
        }

        /* the refraction: the floor the refracted ray reaches (the pixel's own where the screen has it not) */
        vec3 tr = refract(-v, normal, 1.0 / 1.33);
        vec3 floor0 = opaque0 < NONE ? eye + opaque0 * ray : p - n * 40.0;
        float depth0 = max(d - dot(n, floor0), 0.0);
        /* (the frame's floor along the refracted ray, found on the screen: in Path Traced too, the floor the frame
        traced at its own pixels, with CE's fog and what CE draws under the surface (b30's surf); the traced ray where
        the frame has no floor) */
        vec3 transmitted;
        bool no_floor = false;
        {
            vec3 aim = p + tr * (depth0 / max(-dot(tr, n), 0.2));
            float axis;
            vec2 q = pixel_of_direction(aim - eye, axis);
            ivec2 qi = ivec2(q);
            float opaque = opaque0;
            vec2 refracted_uv = uv;
            vec3 floor_point = floor0;
            if (axis > 0.0 && inside(q) && water_at(qi))
            {
                float o = opaque_at(qi);
                vec3 f = o < NONE ? eye + o * ray_of(vec2(qi)) : p - n * 40.0;
                if (dot(n, f) - d < 0.0)
                {
                    refracted_uv = (q + 0.5) / size;
                    opaque = o;
                    floor_point = f;
                }
            }
            vec3 seen_frame = textureLod(behind, refracted_uv, 0.0).rgb;
            /* (the frame's floor has CE's fog on it: the base map's colour under it) */
            if (fogged > 0.0)
                seen_frame = fog_color * fogged + max(seen_frame - fog_color * fogged, vec3(0.0)) * under_fog;
            vec3 seen = light_of(seen_frame);
            if (opaque >= NONE)
            {
                /* (no floor on the frame: what CE drew there, its fog beyond the world) */
                transmitted = seen;
                no_floor = true;
            }
            else
            {
                float path = length(floor_point - p);
                vec3 through = exp(-sigma * path);
                transmitted = seen * through + deep * (1.0 - through);
            }
        }
        if (traced() && no_floor)
        {
            vec4 down = texelFetch(traced_down, pixel, 0);
            float down_length = aux.z;
            /* (the traced floor has no fog: CE's laid over it as CE's frame has it. Over a water drawn before the
            fog, Classic's fog over the water; under any other, CE's planar fog under the surface by the floor's
            depth under its plane, as CE's environment fog takes it (b30's: opaque 0.15 world units down)) */
            vec3 floor_seen = down.rgb;
            if (fogged > 0.0)
                floor_seen = fog_color * fogged + down.rgb * under_fog * (1.0 - fogged);
            else if (pc.fog.a > 0.0 && down_length >= 0.0)
            {
                float below = down_length * max(-dot(tr, n), 0.0);
                float fog_share = clamp(below / max(pc.fog_shape.x, 1.0e-3), 0.0, 1.0) * clamp(pc.fog.a, 0.0, 1.0);
                floor_seen = mix(floor_seen, fog_color, fog_share);
            }
            vec3 seen = light_of(floor_seen);
            if (down_length < 0.0)
                transmitted = deep;
            else
            {
                vec3 through = exp(-sigma * down_length);
                transmitted = seen * through + deep * (1.0 - through);
            }
        }
        /* the sun's highlight off the ripples, in the shadows of what stands between it and the surface */
        vec3 highlight = sun_highlight(normal, v, 0.1 + 0.16 * far_fade, sun_seen) * 0.7 *
            clamp(dot(n, normalize(pc.sun_direction.xyz)) * 4.0, 0.0, 1.0);
        reflected_shown = reflection * mask;
        refracted_shown = transmitted;
        vec3 surface = reflection * fresnel * mask + highlight;
        /* (a water CE draws before the fog (c10's swamp): CE's planar fog over its surface as Classic's, the share
        CE's fog took of the floor behind it (its colour's part of what is behind, the floor under a swamp dark): the
        refraction has it already, the reflection and the highlight take it here) */
        if (fogged > 0.0)
            surface = surface * (1.0 - fogged) + light_of(fog_color) * fogged * fresnel * mask;
        color = transmitted * (1.0 - fresnel) + surface;
        /* (the shore: the water's edge fades into the ground it lies on) */
        color = mix(light_of(was.rgb), color, smoothstep(0.0, 0.05, depth0));
    }
    else
    {
        /* from under the surface: Snell's window, total internal reflection outside it. The window's edge by the
        smoothed ripples' normal, Fresnel's reflectance rising to 1 at the critical angle and a little past it
        blended (the window's edge a soft ring, not CE's ripples' every texel) */
        vec3 facing_smooth = -normalize(mix(n, normal_smooth, 0.4));
        float c_smooth = clamp(dot(v, facing_smooth), 0.0, 1.0);
        float s2 = 1.33 * 1.33 * (1.0 - c_smooth * c_smooth);
        float reflectance = max(fresnel_exact(c_smooth, 1.33, 1.0), smoothstep(0.7, 1.0, s2));
        /* (the window's ray by the ripples a little smoothed, else by the smoothed ones, else along the
        surface) */
        vec3 tr = refract(-v, -normalize(mix(normal_smooth, normal, 0.4)), 1.33);
        if (dot(tr, tr) < 1.0e-6)
            tr = refract(-v, facing_smooth, 1.33);
        if (dot(tr, tr) < 1.0e-6)
        {
            vec3 along = -v - n * dot(-v, n);
            tr = normalize(along * 0.999 + n * 0.045);
        }
        vec3 r = reflect(-v, -normal);
        /* the reflection back into the water: the water's body as the frame shows it along the reflected ray
        (CE's fog planes under the surface), else its deep colour; CE's ripples' pattern over it (CE's cube by the
        full ripples against the smoothed, as Classic's surface from below shows them) */
        float saved = blur_radius;
        blur_radius = 12.0 * scale;
        vec4 frame_body = far_reflection(r, false);
        blur_radius = saved;
        /* (off the view: the water's body as the frame shows it at its bottom edge, where that is not the
        surface) */
        vec3 bottom = vec3(0.0);
        float bottom_count = 0.0;
        for (int k = 0; k < 7; k++)
        {
            ivec2 q = ivec2(mix(float(pc.corners.x), float(pc.corners.z - 1), (float(k) + 0.5) / 7.0),
                pc.corners.w - 3);
            if (!water_at(q))
            {
                bottom += light_of(texelFetch(behind, q, 0).rgb);
                bottom_count += 1.0;
            }
        }
        vec3 fallback = bottom_count > 0.0 ? bottom / bottom_count : deep;
        vec3 water_tone = mix(fallback, frame_body.rgb, frame_body.a);
        vec3 body = water_tone;
        if (traced())
        {
            vec4 down = texelFetch(traced_down, pixel, 0);
            float down_length = texelFetch(traced_aux, pixel, 0).z;
            vec3 through = down_length < 0.0 ? vec3(0.0) : exp(-sigma * down_length);
            body = light_of(down.rgb) * through + water_tone * (1.0 - through);
        }
        float pattern_full = dot(textureLod(cube, reflect(-v, -normal), 1.0).rgb, vec3(0.2126, 0.7152, 0.0722));
        float pattern_smooth = dot(textureLod(cube, reflect(-v, facing_smooth), 3.0).rgb, vec3(0.2126, 0.7152, 0.0722));
        float pattern = clamp(pattern_full / max(pattern_smooth, 0.02), 0.75, 1.6);
        /* (and a share of CE's cube itself, as Classic's surface from below shows it, in its edge-on tint) */
        vec3 ce_below = light_of(textureLod(cube, reflect(-v, -normal), 2.0).rgb * pc.parallel.rgb) * pc.more.z;
        vec3 reflection = mix(body, ce_below, 0.45) * mix(1.0, pattern, 0.6);
        /* (Snell's window: the sky above, traced in Path Traced; else CE's cube in its tint, blurred, as the
        ripples blur it) */
        float straight = clamp(dot(tr, n), 0.0, 1.0);
        vec3 window_tint = mix(pc.parallel.rgb, vec3(1.0), straight);
        vec3 seen = light_of(mix(textureLod(cube, tr, 4.0).rgb, textureLod(cube, tr, 1.5).rgb, 0.35) * window_tint) *
            pc.more.z;
        if (traced())
            seen = mix(traced_blurred(pixel), seen, clamp(texelFetch(traced_aux, pixel, 0).y, 0.0, 1.0));
        color = seen * (1.0 - reflectance) + reflection * reflectance;
        /* (the water between the eye and the surface: a little of the deep colour over long paths; CE's planar fog
        never fogged its water's surface, drawn after it, and its few tenths of a unit would hide Snell's window) */
        color = mix(color, water_tone, 0.6 * (1.0 - exp(-view_length / 8.0)));
        reflected_shown = reflection;
        refracted_shown = seen;
    }

    int view = int(pc.more.w + 0.5);
    if (view == 1)
        color = light_of(rip * 0.5 + 0.5);
    else if (view == 2)
        color = light_of(base_texel.rgb);
    else if (view == 3)
        color = vec3(mask, clamp(view_length / 100.0, 0.0, 1.0), above ? 1.0 : 0.0);
    else if (view == 4)
        color = reflected_shown;
    else if (view == 5)
        color = refracted_shown;
    imageStore(result, pixel, vec4(frame_of(min(color, vec3(16.0))), was.a));
}
