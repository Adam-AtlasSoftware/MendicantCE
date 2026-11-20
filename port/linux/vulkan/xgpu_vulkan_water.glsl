#version 460
/* XGPU_VULKAN_WATER.GLSL

The terrain overhaul's water layer (T7, xgpu_vulkan_water.c; docs/prism_terrain.md "T7"): a CE water group's pixels
composited after CE's depth-only draw, from what the game drew for the host (rasterizer_xbox_water.c's
rasterizer_water_draw_prism): what is behind the water (the colour before it), CE's ripple map at CE's
own coordinates, CE's base map (its colour, or its alpha; the capture's alpha the water's mask), and the G-buffer's
distances.

Two dispatches of this shader (pc.flags.w bit 16 the first):
- the first at half the resolution: each texel a 2x2 block of the frame, at the block's water pixels' mean position
  and CE's ripples' mean there, the reflection (Rebaked: found along the screen, the frame behind where the
  reflected ray meets a surface it has; the sky where it leaves the frame's top through sky; CE's reflection cube,
  in CE's tint, where it finds neither. Path Traced: the traced rays filtered over the roughness's footprint, the
  sky as the frame shows it where they left the level) and the sun's visibility (Rebaked: the sun's ray marched
  over the frame's distances, only where the block's highlight can be lit), written premultiplied by the block's
  water share (the texel's alpha), so that a linear filter of them is a filter of the water's pixels alone;
- the second at every pixel: the surface (the group's plane met by the pixel's ray, its normal bent by CE's
  ripples), lit as water is: Fresnel at an IOR of 1.33, the first pass's reflection (CE's ripples' own pattern put
  back over it: CE's cube by the pixel's ripples against the block's), a refraction (the frame behind the water at
  the refracted ray's floor, absorbed over its path by Beer and Lambert in CE's tint's colours) over CE's own colour
  of its water as the water's body (what Classic's draw gives the pixel: what is behind by CE's base map, CE's
  cube in CE's tint), the sun's highlight off the ripples (GGX); far off and edge on CE's own colour of its water
  (Classic's flat pale sea); from under the surface, Snell's window and total internal reflection with CE's
  ripples' pattern. A water CE draws before the fog (c10's swamp) has CE's fog of the frame behind laid back over
  it (the share it took of the floor), and every layer fades into the ground by the depth of the water over it.
Everywhere else, the frame as it was behind the water (what the game's capture draws left there put back). Colour
is composed in light (the frame's values to a power, display.gamma's 2.2 where the frame is encoded) and put back
in the frame's encoding.

In Path Traced (T8) the reflection and the refraction are traced (xgpu_vulkan_pt.glsl's pass "water", at the first
pass's half resolution: the bake's scene, the hit lit from the bake, in light): traced_up the light along the upper
ray (above the surface the reflection, below it Snell's window's), traced_down the light along the lower ray (above,
the refraction to the floor; below, the reflection back into the water), each premultiplied by the share of the
block's water where it met the level; traced_aux the sun's visibility at the surface, the upper ray's share that
left the level (there the sky as the frame shows it: CE's sky, as Rebaked's reflection finds it; the scene's sky is
the light's, not the picture's), the lower ray's length in the water (-1: none) and the block's water share. The
traced floor takes CE's planar fog under the surface as CE's frame has it. The reference's (debug.pt_reference) is
summed there over the footprint's directions, so it is taken as it is. */

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D behind;
layout(set = 0, binding = 1) uniform sampler2D ripples;
layout(set = 0, binding = 2) uniform sampler2D base;
layout(set = 0, binding = 3) uniform sampler2D depth;
layout(set = 0, binding = 4) uniform usampler2D words;
layout(set = 0, binding = 5) uniform usampler2D extras;
layout(set = 0, binding = 6) uniform samplerCube cube;
/* (the first pass: Path Traced's rays, at half the resolution; the second: the first's reflection (7) and its
second image (9), with Path Traced's lower ray (8)) */
layout(set = 0, binding = 7) uniform sampler2D traced_up;
layout(set = 0, binding = 8) uniform sampler2D traced_down;
layout(set = 0, binding = 9) uniform sampler2D traced_aux;
/* (the first pass: the reflection and its second image (the sun's visibility, the share that left the level, the
lower ray's length, the water's share), at half the resolution; the second: the frame, and nothing) */
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2D result;
layout(set = 0, binding = 11, rgba16f) uniform writeonly image2D second;

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
    water, 4 its rays traced (traced_up and traced_down), 8 the reference's (summed over the footprint), 16 the
    first pass */
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
const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

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

/* the same from the distances alone (the marches' steps: half the reads; a pixel with no surface has none, and the
hit is checked by opaque_at) */
float opaque_quick(ivec2 p)
{
    uint x = texelFetch(extras, p, 0).w;
    float d = uintBitsToFloat(x);
    return d > 0.0 && (x & 0x7f800000u) != 0x7f800000u ? d : NONE;
}

/* an opaque water's layer (flags.w bit 1): over the resolved frame, the level's pixels of a material of the water
class (the classes' table in the base's place: a texel a material, its two classes) */
bool opaque_mode()
{
    return (pc.flags.w & 2u) != 0u;
}

bool traced() { return (pc.flags.w & 4u) != 0u; }
bool reference() { return (pc.flags.w & 8u) != 0u; }
bool first_pass() { return (pc.flags.w & 16u) != 0u; }

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

/* the half resolution's size */
ivec2 half_size()
{
    return (ivec2(pc.flags.yz) + 1) / 2;
}

/* a 2x2 block of the frame (the first pass's texel h): its water pixels' share, their mean position and CE's
ripples' mean at them */
float block_of(ivec2 h, out vec2 position, out vec3 rip)
{
    float count = 0.0;
    position = vec2(0.0);
    rip = vec3(0.0);
    for (int k = 0; k < 4; k++)
    {
        ivec2 q = h * 2 + ivec2(k & 1, k >> 1);
        if (q.x >= int(pc.flags.y) || q.y >= int(pc.flags.z) || !inside(vec2(q)) || !water_at(q))
            continue;
        count += 1.0;
        position += vec2(q);
        rip += texelFetch(ripples, q, 0).rgb * 2.0 - 1.0;
    }
    if (count > 0.0)
    {
        position /= count;
        rip /= count;
    }
    return count * 0.25;
}

/* the surface at a position of the frame: where the pixel's ray meets the plane, how far, how edge on, and the
roughness's footprint there (blur_radius pixels wide, blur_stretch times as tall: a rough water's reflections
stretch toward the eye, the ripples tilting the reflected rays most in the plane of the view) */
struct Surface
{
    vec3 p, v, ray;
    float t, view_length, far_fade, edge_on, grazing, strength, opaque0;
    bool above;
};
vec3 n_plane, tangent, bitangent;
float d_plane;
float blur_radius, blur_stretch = 1.0;

Surface surface_at(vec2 position)
{
    Surface s;
    vec3 eye = pc.camera[0].xyz;
    s.ray = ray_of(position);
    float eye_side = dot(n_plane, eye) - d_plane;
    s.above = eye_side >= 0.0;
    float facing = dot(n_plane, s.ray);
    s.t = abs(facing) > 1.0e-6 ? -eye_side / facing : -1.0;
    s.opaque0 = opaque_at(ivec2(position + 0.25));
    if (s.t <= 0.0)
        s.t = min(s.opaque0, 1.0e4);
    s.p = eye + s.t * s.ray;
    s.view_length = s.t * length(s.ray);
    s.v = -normalize(s.ray);
    s.far_fade = smoothstep(8.0, 160.0, s.view_length);
    /* (CE's ripples flattened a little edge on and far off, where its map's texels would alias into blocks; what
    they lose the reflection's blur keeps as roughness: far off and edge on the reflection is the rough sea's, not
    a mirror's) */
    s.edge_on = clamp(abs(dot(normalize(s.ray), n_plane)) * 3.0, 0.0, 1.0);
    s.strength = pc.tuning.y * mix(1.0, 0.6, s.far_fade) * mix(0.6, 1.0, s.edge_on);
    s.grazing = s.far_fade * (1.0 - s.edge_on);
    float scale = float(pc.flags.z) / 720.0;
    blur_radius = (1.0 + 4.0 * s.far_fade + 16.0 * s.grazing) * mix(1.8, 1.0, s.edge_on) * scale;
    blur_stretch = mix(2.4, 1.0, s.edge_on);
    return s;
}

vec3 bent(Surface s, vec3 rip)
{
    return normalize(n_plane + s.strength * (rip.x * tangent + rip.y * bitangent));
}

/* the reflected ray off a normal, kept a little over the plane */
vec3 reflected(vec3 v, vec3 normal)
{
    vec3 r = reflect(-v, normal);
    float under = dot(r, n_plane);
    if (under < 0.02)
        r = normalize(r + n_plane * (0.02 - under));
    return r;
}

/* the frame behind at a pixel, blurred over the reflection's footprint, in light: nine taps over an ellipse
blur_radius wide and blur_stretch times as tall (summed in the frame's encoding and put in light once: the taps'
powers were most of the first pass's time) */
const vec2 TAPS[8] = vec2[8](vec2(0.92, 0.18), vec2(-0.71, 0.55), vec2(0.21, -0.94), vec2(-0.38, -0.36),
    vec2(0.45, 0.62), vec2(-0.96, -0.12), vec2(0.62, -0.47), vec2(-0.12, 0.98));
vec3 behind_blurred(vec2 q)
{
    vec2 size = vec2(pc.flags.yz);
    vec3 sum = textureLod(behind, (q + 0.5) / size, 0.0).rgb * 2.0;
    if (blur_radius < 0.75)
        return light_of(sum * 0.5);
    for (int k = 0; k < 8; k++)
    {
        vec2 o = TAPS[k] * vec2(blur_radius, blur_radius * blur_stretch);
        sum += textureLod(behind, (q + o + 0.5) / size, 0.0).rgb;
    }
    if (blur_radius < 6.0)
        return light_of(sum / 10.0);
    /* (a wide footprint (a far, rough sea's) eight taps more between them, turned and nearer: so few taps over
    so wide a footprint would show its pattern, not its blur) */
    for (int k = 0; k < 8; k++)
    {
        vec2 o = vec2(TAPS[k].y, -TAPS[k].x) * 0.55 * vec2(blur_radius, blur_radius * blur_stretch);
        sum += textureLod(behind, (q + o + 0.5) / size, 0.0).rgb;
    }
    return light_of(sum / 18.0);
}

/* the reflection along the screen from p (its axis distance) toward r: the frame where it meets a surface on the
side of the plane above (above: the water seen from above; else from below), its weight (0 none). Its steps fewer
and longer far off (far_fade), where the footprint's blur hides what the march would miss */
vec4 screen_reflection(vec3 p, vec3 r, float axis0, bool above, float far_fade)
{
    vec3 eye = pc.camera[0].xyz;
    float s0 = 0.04 + 0.012 * axis0;
    float travelled = 0.0, last = 0.0;
    float step_length = s0;
    int steps = int(mix(18.0, 10.0, far_fade));
    float growth = mix(1.26, 1.4, far_fade);
    for (int i = 0; i < steps; i++)
    {
        travelled += step_length;
        step_length *= growth;
        vec3 x = p + r * travelled;
        float axis;
        vec2 q = pixel_of_direction(x - eye, axis);
        if (axis <= 0.0 || !inside(q))
            return vec4(0.0);
        float opaque = opaque_quick(ivec2(q));
        float thickness = max(0.25, 0.04 * axis) + step_length;
        if (opaque < NONE && axis > opaque && axis - opaque < thickness)
        {
            /* (a few halvings between the last step and this) */
            float a = last, b = travelled;
            for (int k = 0; k < 3; k++)
            {
                float m = 0.5 * (a + b);
                float am;
                vec2 qm = pixel_of_direction(p + r * m - eye, am);
                float om = opaque_quick(ivec2(clamp(qm, vec2(pc.corners.xy), vec2(pc.corners.zw) - 1.0)));
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
vec4 far_reflection(vec3 r)
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

/* the sky's reflection: the sky through the frame, else CE's cube in CE's tint; far off and edge on a rough sea's
reflection of the sky is its broad light, not its clouds' shapes (CE's cube at its widest blur) */
vec3 environment_of(Surface s, vec3 r)
{
    vec3 ce_tint = mix(pc.parallel.rgb, pc.perpendicular.rgb, clamp(dot(s.v, n_plane), 0.0, 1.0));
    vec3 environment = light_of(textureLod(cube, r, 1.0 + 3.0 * s.grazing).rgb * ce_tint) * pc.more.z;
    vec4 sky = far_reflection(r);
    environment = mix(environment, sky.rgb, sky.a);
    vec3 broad = light_of(textureLod(cube, r, 6.0).rgb * ce_tint) * pc.more.z;
    return mix(environment, mix(broad, environment, 0.5), s.grazing);
}

/* the sun's visibility at a point of the surface, along the screen (Rebaked): the sun's ray marched over the
frame's distances; a surface the ray passes behind, within a thickness, shades it (the cliffs' shadow on the
water, the logs'), fading out toward the view's edges where the frame cannot say. Fewer, longer steps far off */
float sun_seen_screen(vec3 p, vec3 l, float far_fade)
{
    vec3 eye = pc.camera[0].xyz;
    float travelled = 0.0, step_length = 0.15, shade = 0.0;
    int steps = int(mix(14.0, 8.0, far_fade));
    float growth = mix(1.32, 1.5, far_fade);
    for (int i = 0; i < steps; i++)
    {
        travelled += step_length;
        step_length *= growth;
        vec3 x = p + l * travelled;
        float axis;
        vec2 q = pixel_of_direction(x - eye, axis);
        if (axis <= 0.0 || !inside(q))
            break;
        float opaque = opaque_quick(ivec2(q));
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

/* the highlight's roughness (wide: CE's ripple facets' glints soft, not white plates) and its strength */
float glint_rough(float far_fade)
{
    return 0.22 + 0.16 * far_fade;
}
const float GLINT = 0.3;

/* the sun's highlight could be lit somewhere in a block (its normal, the lobe widened over the block's pixels'
normals): the sun's march only there */
bool sun_wanted(vec3 normal, vec3 v, float far_fade)
{
    if (pc.sun_direction.w < 0.5 || dot(n_plane, normalize(pc.sun_direction.xyz)) <= 0.0)
        return false;
    return dot(sun_highlight(normal, v, glint_rough(far_fade) + 0.2, 1.0), LUMA) > 0.002;
}

/* CE's own sheen of its water at a pixel, as Classic's draw adds it over what is behind (rasterizer_xbox_water.c's
reflection pass, blended by ONE): CE's cube along the reflected ray in CE's tint (CE's: the group's, between its
edge-on and straight-on tints by the camera's forward against the plane, not the pixel's view; by its alpha between
its two views', and the base map's, where that modulates the reflection), in the frame's encoding. Its ray is CE's:
off CE's ripples at CE's own strength, neither flattened far off nor edge on (Classic's far sea samples the sky well
above the horizon through them, not the cube's horizon). Its alpha between the views' by the square root of the
view's cosine, and the whole at 0.7: calibrated by Classic's frames (b30's sunlit sea W3 and W4, B1's far sea, W1
looking down), which a linear blend by the cosine left about twice as bright edge on */
const float CE_RIPPLES = 0.7;
const float CE_SHEEN = 0.7;
vec3 ce_sheen(vec3 rip, float lod, vec3 v, float mask)
{
    vec3 normal = normalize(n_plane + CE_RIPPLES * (rip.x * tangent + rip.y * bitangent));
    vec3 r = reflect(-v, normal);
    if (dot(v, n_plane) >= 0.0)
        r = reflected(v, normal);
    float c = clamp(dot(v, n_plane), 0.0, 1.0);
    float amount = clamp(-dot(normalize(pc.camera[1].xyz), n_plane), 0.0, 1.0);
    vec3 tint = mix(pc.parallel.rgb, pc.perpendicular.rgb, amount);
    float alpha = (pc.flags.x & ALPHA_MODULATES_REFLECTION) != 0u ?
        mix(pc.parallel.a, pc.perpendicular.a, sqrt(c)) * mask : 1.0;
    return textureLod(cube, r, lod).rgb * tint * alpha * CE_SHEEN * pc.more.z;
}

/* CE's own colour of its water: what is behind (by CE's base map's colour where that modulates the background)
plus CE's sheen, held at white as CE's frame was (frame encoding in, light out) */
vec3 ce_over(vec3 behind_color, vec3 sheen)
{
    return light_of(min(behind_color + sheen, vec3(1.0)));
}

/* a texture of the first pass (premultiplied by the water's share, at half the resolution) at a pixel, filtered
linearly over the water's texels alone */
vec4 half_sampled(sampler2D image, ivec2 pixel)
{
    vec2 hs = vec2(half_size());
    vec4 v = textureLod(image, ((vec2(pixel) + 0.5) * 0.5) / hs, 0.0);
    if (v.a <= 1.0e-4)
        v = texelFetch(image, pixel / 2, 0);
    return v.a > 1.0e-4 ? v / v.a : vec4(0.0);
}

/* Path Traced's upper ray's light over the roughness's footprint at a first pass's texel (its taps at half the
resolution; premultiplied, so the water's texels alone, and where the rays met the level), the reference's as it
was summed */
vec4 traced_filtered(ivec2 h)
{
    vec4 sum = texelFetch(traced_up, h, 0) * 2.0;
    float radius = blur_radius * 0.5;
    if (reference() || radius < 0.6)
        return sum * 0.5;
    vec2 last = vec2(half_size()) - 1.0;
    for (int k = 0; k < 8; k++)
    {
        vec2 o = TAPS[k] * vec2(radius, radius * blur_stretch);
        sum += texelFetch(traced_up, ivec2(clamp(vec2(h) + o + 0.5, vec2(0.0), last)), 0);
    }
    if (radius >= 4.0)
        for (int k = 0; k < 8; k++)
        {
            vec2 o = vec2(TAPS[k].y, -TAPS[k].x) * 0.55 * vec2(radius, radius * blur_stretch);
            sum += texelFetch(traced_up, ivec2(clamp(vec2(h) + o + 0.5, vec2(0.0), last)), 0);
        }
    return sum;
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
    /* (the footprint wider than tall edge on: a tall one drew the frame's vertical edges down into the
    water as a faint haze) */
    blur_radius = (3.0 + 16.0 * far_fade) * mix(2.0, 1.0, edge_on) * scale;
    blur_stretch = mix(0.7, 1.0, edge_on);
    float fresnel = min(schlick(0.02, clamp(dot(v, normal), 0.0, 1.0)) * 0.45, 0.25);
    vec3 r = reflect(-v, normal);
    if (dot(r, n) < 0.02)
        r = normalize(r + n * (0.02 - dot(r, n)));
    vec3 body = light_of(was);
    vec4 sky = far_reflection(r);
    vec3 reflection = mix(body, sky.rgb, sky.a);
    vec4 found = screen_reflection(p, r, t, true, far_fade);
    reflection = mix(reflection, found.rgb, found.a);
    /* (in CE's colour's hue: its luminance kept, half its colour the water's) */
    float body_luma = max(dot(body, LUMA), 1.0e-4);
    float reflection_luma = dot(reflection, LUMA);
    reflection = mix(reflection, body * (reflection_luma / body_luma), 0.55);
    /* (and held under 1.35 of the body's light: the bright clouds' reflection near the shore stood as pale columns
    of haze over CE's opaque water, which Classic gives no reflection at all) */
    reflection *= min(1.0, 1.35 * body_luma / max(dot(reflection, LUMA), 1.0e-4));
    float seen = sun_wanted(normal, v, far_fade) ?
        sun_seen_screen(p + n * 0.05, normalize(pc.sun_direction.xyz), far_fade) : 0.0;
    /* (faint and wide: a muddy water under d40's overcast; a stronger one stood as a pale column of haze under the
    sun) */
    vec3 highlight = sun_highlight(normal, v, 0.3 + 0.16 * far_fade, seen) * 0.12;
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

void plane_setup()
{
    to_camera = inverse(mat3(pc.camera[1].xyz, pc.camera[2].xyz, pc.camera[3].xyz));
    n_plane = normalize(pc.plane.xyz);
    d_plane = pc.plane.w / max(length(pc.plane.xyz), 1.0e-6);
    tangent = normalize(abs(n_plane.z) < 0.9 ? cross(vec3(0.0, 0.0, 1.0), n_plane) : cross(n_plane, vec3(0.0, 1.0, 0.0)));
    bitangent = cross(n_plane, tangent);
}

/* the first pass: a 2x2 block's reflection and sun's visibility (premultiplied by its water's share) */
void first(ivec2 h)
{
    if (h.x >= half_size().x || h.y >= half_size().y)
        return;
    vec2 position;
    vec3 rip;
    float share = block_of(h, position, rip);
    if (share <= 0.0)
    {
        imageStore(result, h, vec4(0.0));
        imageStore(second, h, vec4(0.0));
        return;
    }
    plane_setup();
    Surface s = surface_at(position);
    vec3 normal = bent(s, rip);
    vec3 reflection = vec3(0.0);
    float sun = 1.0, miss = 0.0, down = -1.0;
    if (traced())
    {
        /* (Path Traced: the traced reflection over its footprint, the sky as the frame shows it where it left the
        level; the sun's visibility and the lower ray's length traced) */
        vec4 aux = texelFetch(traced_aux, h, 0);
        float w = max(aux.w, 1.0e-4);
        sun = aux.x / w;
        miss = clamp(aux.y / w, 0.0, 1.0);
        down = aux.z / w;
        vec4 hit = traced_filtered(h);
        bool met = hit.a > 1.0e-4;
        if (s.above)
        {
            vec3 environment = miss > 0.002 || !met ? environment_of(s, reflected(s.v, normal)) : vec3(0.0);
            reflection = met ? mix(hit.rgb / hit.a, environment, miss) : environment;
        }
        else if (met)
            reflection = hit.rgb / hit.a;
        else
            miss = 1.0;
    }
    else if (s.above)
    {
        /* (Rebaked: along the screen, else the sky's; the sun's march where its highlight can be lit) */
        vec3 r = reflected(s.v, normal);
        vec4 found = screen_reflection(s.p, r, s.t, true, s.far_fade);
        reflection = found.rgb;
        if (found.a < 0.999)
            reflection = mix(environment_of(s, r), found.rgb, found.a);
        sun = sun_wanted(normal, s.v, s.far_fade) ?
            sun_seen_screen(s.p + n_plane * 0.05, normalize(pc.sun_direction.xyz), s.far_fade) : 1.0;
    }
    else
        miss = 1.0;
    imageStore(result, h, vec4(reflection * share, share));
    imageStore(second, h, vec4(sun * share, miss * share, down * share, share));
}

void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (first_pass())
    {
        first(pixel);
        return;
    }
    if (pixel.x >= int(pc.flags.y) || pixel.y >= int(pc.flags.z))
        return;
    vec4 was = texelFetch(behind, pixel, 0);
    if (!inside(vec2(pixel)) || !water_at(pixel))
    {
        imageStore(result, pixel, was);
        return;
    }
    plane_setup();
    vec3 eye = pc.camera[0].xyz;
    if (opaque_mode())
    {
        imageStore(result, pixel, vec4(frame_of(opaque_water(pixel, eye, was.rgb)), was.a));
        return;
    }
    vec3 n = n_plane;
    float d = d_plane;
    Surface s = surface_at(vec2(pixel));
    vec3 ray = s.ray;
    vec3 p = s.p;
    vec3 v = s.v;
    float t = s.t;
    float opaque0 = s.opaque0;
    vec2 size = vec2(pc.flags.yz);
    vec2 uv = (vec2(pixel) + 0.5) / size;
    float scale = size.y / 720.0;

    /* the normal: CE's ripples (its bump map's signed x and y) about the plane's; the block's (the first pass's
    texel's: its reflection's normal) */
    vec3 rip = texelFetch(ripples, pixel, 0).rgb * 2.0 - 1.0;
    vec3 normal = bent(s, rip);
    /* (the block's ripples: its four pixels', the shore's few blocks with land in them alike) */
    vec3 block_rip = vec3(0.0);
    for (int k = 0; k < 4; k++)
        block_rip += texelFetch(ripples, min((pixel / 2) * 2 + ivec2(k & 1, k >> 1), ivec2(size) - 1), 0).rgb;
    block_rip = block_rip * 0.5 - 1.0;
    vec3 normal_block = bent(s, block_rip);
    vec4 base_texel = texelFetch(base, pixel, 0);
    bool has_base = (pc.flags.x & HAS_BASE) != 0u;
    vec3 base_color = has_base && (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_texel.rgb : vec3(1.0);
    float mask = has_base && (pc.flags.x & COLOR_MODULATES_BACKGROUND) == 0u &&
        (pc.flags.x & ALPHA_MODULATES_REFLECTION) != 0u ? base_texel.r : 1.0;
    vec4 first_reflection = half_sampled(traced_up, pixel);
    vec4 first_second = half_sampled(traced_aux, pixel);

    /* CE's tints: its absorption's colours (the background's tint where its base map's colour is it, else the
    straight-on tint's hue) and its deep colour */
    vec3 tint = pc.perpendicular.rgb;
    vec3 hue = tint / max(max(tint.r, max(tint.g, tint.b)), 1.0e-3);
    vec3 absorb_color = (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_color : hue;
    vec3 sigma = pc.tuning.z * -log(clamp(absorb_color, vec3(0.03), vec3(1.0))) + 0.015;
    /* (the deep colour from below: CE's planar fog's under the water, its own colour of the water's depths, where
    the window has one; else the tint's) */
    vec3 deep = light_of(tint) * 0.6 * pc.more.x;
    if ((pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u)
        deep = light_of(base_color * tint) * 0.6 * pc.more.x;
    if (pc.fog.a > 0.0 && (pc.flags.x & BEFORE_FOG) == 0u)
        deep = light_of(pc.fog.rgb) * pc.more.x;

    /* (a water CE draws before the fog (c10's swamp): the share CE's planar fog took of what is behind it (its
    colour's part of the frame there; the floor under a swamp dark), which Classic's fog laid over the water after
    it; the floor under that fog is the frame without it) */
    vec3 fog_color = clamp(pc.fog.rgb, vec3(0.0), vec3(1.0));
    float fogged = 0.0;
    if ((pc.flags.x & BEFORE_FOG) != 0u && pc.fog.a > 0.0)
        fogged = clamp(dot(was.rgb, fog_color) / max(dot(fog_color, fog_color), 1.0e-4), 0.0, 1.0) *
            clamp(pc.fog.a, 0.0, 1.0);
    vec3 clear = fogged > 0.0 ? max(was.rgb - fog_color * fogged, vec3(0.0)) / max(1.0 - fogged, 0.05) : was.rgb;
    /* (CE's base map's colour over what is behind, as Classic's colour draw modulates it) */
    vec3 under = (pc.flags.x & COLOR_MODULATES_BACKGROUND) != 0u ? base_color : vec3(1.0);

    vec3 color;
    vec3 reflected_shown = vec3(0.0), refracted_shown = vec3(0.0);
    float depth0 = 1.0e4;
    if (s.above)
    {
        float cos_v = clamp(dot(v, normal), 0.0, 1.0);
        /* (a rough sea's Fresnel edge on a little less than a mirror's: its facets turned from the eye shadow and
        mask the grazing reflection) */
        float fresnel = schlick(0.02, cos_v) * (1.0 - 0.3 * s.grazing);
        vec3 r = reflected(v, normal);
        vec3 r_block = reflected(v, normal_block);
        /* the reflection: the first pass's, with CE's ripples' pattern of the pixel put back over it (CE's cube by
        the pixel's ripples against the block's: the half resolution's reflection keeps CE's every ripple) */
        vec3 reflection = first_reflection.rgb;
        float pattern_lod = 1.0 + 2.0 * s.grazing;
        float pattern = dot(textureLod(cube, r, pattern_lod).rgb, LUMA) /
            max(dot(textureLod(cube, r_block, pattern_lod).rgb, LUMA), 0.02);
        reflection *= mix(1.0, clamp(pattern, 0.7, 1.35), 0.8);
        float sun_seen = clamp(first_second.x, 0.0, 1.0);

        /* the water's body: the floor seen through it, fading with depth into the water's own colour as the frame
        has it (CE's planar fog under the surface); CE's sheen (CE's cube by the pixel's ripples in its tint) laid over
        it as Classic's draw lays it over what is behind: CE's blue and CE's ripple pattern under the reflection, in
        Path Traced and Rebaked alike */
        vec3 sheen = ce_sheen(rip, 0.5 + 1.5 * s.far_fade, v, mask);
        vec3 body = light_of(clear * under) * pc.more.x;

        /* the refraction: the floor the refracted ray reaches (the pixel's own where the screen has it not) */
        vec3 tr = refract(-v, normal, 1.0 / 1.33);
        vec3 floor0 = opaque0 < NONE ? eye + opaque0 * ray : p - n * 40.0;
        depth0 = max(d - dot(n, floor0), 0.0);
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
            /* (CE's fog taken off the floor where it is laid back over the surface; CE's base map's colour over
            it) */
            if (fogged > 0.0)
                seen_frame = max(seen_frame - fog_color * fogged, vec3(0.0)) / max(1.0 - fogged, 0.05);
            vec3 seen = light_of(seen_frame * under);
            if (opaque >= NONE)
            {
                /* (no floor on the frame (the far sea, past the level): the body alone) */
                transmitted = body;
                no_floor = true;
            }
            else
            {
                float path = length(floor_point - p);
                vec3 through = exp(-sigma * path);
                transmitted = seen * through + body * (1.0 - through);
            }
        }
        if (traced() && no_floor)
        {
            vec4 down = half_sampled(traced_down, pixel);
            float down_length = first_second.z;
            /* (the traced floor has no fog: under a water CE draws before the fog, none (laid over the surface
            below); under any other, CE's planar fog under the surface by the floor's depth under its plane, as CE's
            environment fog takes it (b30's: opaque 0.15 world units down)) */
            vec3 floor_seen = down.rgb;
            if (fogged <= 0.0 && pc.fog.a > 0.0 && down_length >= 0.0)
            {
                float below = down_length * max(-dot(tr, n), 0.0);
                float fog_share = clamp(below / max(pc.fog_shape.x, 1.0e-3), 0.0, 1.0) * clamp(pc.fog.a, 0.0, 1.0);
                floor_seen = mix(floor_seen, light_of(fog_color), fog_share);
            }
            if (down_length < 0.0)
                transmitted = body;
            else
            {
                vec3 through = exp(-sigma * down_length);
                transmitted = floor_seen * light_of(under) * through + body * (1.0 - through);
            }
        }
        /* the sun's highlight off the ripples, in the shadows of what stands between it and the surface: a wide,
        soft lobe (CE's ripple facets glint, not flash) */
        vec3 highlight = sun_highlight(normal, v, glint_rough(s.far_fade), sun_seen) * GLINT *
            clamp(dot(n, normalize(pc.sun_direction.xyz)) * 4.0, 0.0, 1.0);
        transmitted = ce_over(frame_of(transmitted), sheen);
        reflected_shown = reflection * mask;
        refracted_shown = transmitted;
        color = transmitted * (1.0 - fresnel) + reflection * fresnel * mask + highlight;
        /* far off and edge on, CE's own colour of its water (Classic's flat pale sea, CE's cube at a wide blur in
        its edge-on tint over what is behind): a far rough sea's reflection is the sky's broad light, which CE's art
        gives as its tint */
        float far_ce = 0.9 * smoothstep(0.04, 0.6, s.grazing);
        if (far_ce > 0.0)
            color = mix(color, ce_over(clear * under, ce_sheen(block_rip, 1.0 + 1.5 * s.far_fade, v, mask)), far_ce);
        /* (a water CE draws before the fog: CE's fog laid over the surface as Classic's, its share of the floor
        behind: its shape is the frame's own) */
        if (fogged > 0.0)
            color = light_of(fog_color * fogged + frame_of(color) * (1.0 - fogged));
    }
    else
    {
        /* from under the surface: Snell's window, total internal reflection outside it. The window's edge by the
        ripples smoothed over a few pixels (the critical angle is a hard line, which CE's ripples' every texel would
        otherwise make jagged), Fresnel's reflectance rising to 1 at the critical angle and a little past it blended
        (the window's edge a soft ring) */
        vec3 smooth_rip = rip * 0.2;
        for (int k = 0; k < 8; k++)
        {
            ivec2 q = ivec2(clamp(vec2(pixel) + TAPS[k] * 5.0 * scale, vec2(0.0), size - 1.0));
            smooth_rip += (texelFetch(ripples, q, 0).rgb * 2.0 - 1.0) * 0.1;
        }
        vec3 normal_smooth = bent(s, smooth_rip);
        vec3 facing_smooth = -normalize(mix(n, normal_smooth, 0.4));
        float c_smooth = clamp(dot(v, facing_smooth), 0.0, 1.0);
        float s2 = 1.33 * 1.33 * (1.0 - c_smooth * c_smooth);
        float reflectance = max(fresnel_exact(c_smooth, 1.33, 1.0), smoothstep(0.7, 1.0, s2));
        /* (the window's ray by the pixel's ripples (most of them: CE's pattern in the window), else by the
        smoothed ones, else along the surface) */
        vec3 tr = refract(-v, -normalize(mix(normal_smooth, normal, 0.75)), 1.33);
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
        vec4 frame_body = far_reflection(r);
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
            vec4 down = half_sampled(traced_down, pixel);
            float down_length = first_second.z;
            vec3 through = down_length < 0.0 ? vec3(0.0) : exp(-sigma * down_length);
            body = down.rgb * through + water_tone * (1.0 - through);
        }
        float pattern_full = dot(textureLod(cube, reflect(-v, -normal), 0.8).rgb, LUMA);
        float pattern_smooth = dot(textureLod(cube, reflect(-v, facing_smooth), 3.0).rgb, LUMA);
        float pattern = clamp(pattern_full / max(pattern_smooth, 0.02), 0.6, 1.8);
        /* (and CE's own colour of its surface from below, sharp: what is behind it (the sky) and CE's sheen, its
        blue and white ripple pattern) */
        vec3 ce_below = ce_over(was.rgb, ce_sheen(rip, 0.6, v, mask));
        vec3 reflection = mix(body, ce_below, 0.8) * mix(1.0, pattern, 0.7);
        /* (Snell's window: the sky above, traced in Path Traced; else CE's cube in its tint, sharp away from the
        zenith and blurred toward it (the cube's zenith, which Classic never shows from below, smeared by the
        ripples), with CE's ripples' pattern over it as Classic's surface from below has it) */
        float straight = clamp(dot(tr, n), 0.0, 1.0);
        vec3 window_tint = mix(pc.parallel.rgb, vec3(1.0), straight);
        float window_lod = mix(0.6, 4.5, smoothstep(0.75, 0.97, straight));
        vec3 seen = light_of(textureLod(cube, tr, window_lod).rgb * window_tint) * pc.more.z;
        if (traced())
            seen = mix(first_reflection.rgb, seen, clamp(first_second.y, 0.0, 1.0));
        seen = mix(seen, ce_below, 0.6);
        color = seen * (1.0 - reflectance) + reflection * reflectance;
        /* (the water between the eye and the surface: a little of the deep colour over long paths; CE's planar fog
        never fogged its water's surface, drawn after it, and its few tenths of a unit would hide Snell's window) */
        color = mix(color, water_tone, 0.35 * (1.0 - exp(-s.view_length / 8.0)));
        reflected_shown = reflection;
        refracted_shown = seen;
    }

    /* (the shore: the layer fades into the ground it lies on by the water's depth over it (the G-buffer's floor
    under the plane), wider where CE's fog lies over it) */
    float feather = (pc.flags.x & BEFORE_FOG) != 0u ? 0.8 : 0.3;
    color = mix(light_of(was.rgb), color, smoothstep(0.0, feather, depth0));
    int view = int(pc.more.w + 0.5);
    if (view == 1)
        color = light_of(rip * 0.5 + 0.5);
    else if (view == 2)
        color = light_of(base_texel.rgb);
    else if (view == 3)
        color = vec3(mask, clamp(s.view_length / 100.0, 0.0, 1.0), s.above ? 1.0 : 0.0);
    else if (view == 4)
        color = reflected_shown;
    else if (view == 5)
        color = refracted_shown;
    imageStore(result, pixel, vec4(frame_of(min(color, vec3(16.0))), was.a));
}
