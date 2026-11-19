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
gamma's 2.2 where the frame is encoded) and put back in the frame's encoding. */

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D behind;
layout(set = 0, binding = 1) uniform sampler2D ripples;
layout(set = 0, binding = 2) uniform sampler2D base;
layout(set = 0, binding = 3) uniform sampler2D depth;
layout(set = 0, binding = 4) uniform usampler2D words;
layout(set = 0, binding = 5) uniform usampler2D extras;
layout(set = 0, binding = 6) uniform samplerCube cube;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D result;

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
    /* x the shader's flags (XGPU_TERRAIN_WATER_*), y and z the target's size, w 1 Path Traced */
    uvec4 flags;
    /* the window's planar fog: CE's colour and maximum density (0 none); its maximum depth and distance, the eye's
    height over its plane */
    vec4 fog;
    vec4 fog_shape;
} pc;

const uint ALPHA_MODULATES_REFLECTION = 1u;
const uint COLOR_MODULATES_BACKGROUND = 2u;
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

/* the frame behind at a pixel, blurred over a radius (the reflection's roughness: the ripples finer than a pixel
far off), in light */
float blur_radius;
vec3 behind_blurred(vec2 q)
{
    vec2 size = vec2(pc.flags.yz);
    vec3 sum = light_of(textureLod(behind, (q + 0.5) / size, 0.0).rgb) * 2.0;
    if (blur_radius < 0.75)
        return sum * 0.5;
    sum += light_of(textureLod(behind, (q + vec2(blur_radius, 0.3 * blur_radius) + 0.5) / size, 0.0).rgb);
    sum += light_of(textureLod(behind, (q + vec2(-blur_radius, -0.3 * blur_radius) + 0.5) / size, 0.0).rgb);
    sum += light_of(textureLod(behind, (q + vec2(-0.3 * blur_radius, blur_radius) + 0.5) / size, 0.0).rgb);
    sum += light_of(textureLod(behind, (q + vec2(0.3 * blur_radius, -blur_radius) + 0.5) / size, 0.0).rgb);
    return sum / 6.0;
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

/* an opaque water (d40's): CE's colour of it kept as what the surface shows through (no refraction: the frame has
nothing under it), its own bump's normal flattened toward the level, reflection along the screen and of the sky,
Fresnel, the sun's highlight */
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
    float strength = pc.tuning.y * 1.6 * mix(1.0, 0.35, far_fade) * mix(0.45, 1.0, edge_on);
    vec3 normal = normalize(n + strength * (bump - n * dot(bump, n)));
    blur_radius = (1.0 + 10.0 * far_fade) * mix(1.6, 1.0, edge_on) * float(pc.flags.z) / 720.0;
    float fresnel = schlick(0.02, clamp(dot(v, normal), 0.0, 1.0));
    vec3 r = reflect(-v, normal);
    if (dot(r, n) < 0.02)
        r = normalize(r + n * (0.02 - dot(r, n)));
    vec3 body = light_of(was);
    vec4 sky = far_reflection(r, true);
    vec3 reflection = mix(body, sky.rgb, sky.a);
    vec4 found = screen_reflection(p, r, t, true);
    reflection = mix(reflection, found.rgb, found.a);
    vec3 highlight = vec3(0.0);
    if (pc.sun_direction.w > 0.5)
    {
        vec3 l = normalize(pc.sun_direction.xyz);
        vec3 h = normalize(l + v);
        float nl = clamp(dot(normal, l), 0.0, 1.0), nv = max(dot(normal, v), 1.0e-3), nh = clamp(dot(normal, h), 0.0,
            1.0);
        float rough = 0.09 + 0.16 * far_fade;
        float a2 = rough * rough * rough * rough;
        float den = nh * nh * (a2 - 1.0) + 1.0;
        float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1.0e-4);
        highlight = min(a2 / (PI * den * den) * vis * schlick(0.02, clamp(dot(v, h), 0.0, 1.0)) * nl, 64.0) *
            light_of(pc.sun_color.rgb) * pc.tuning.w;
    }
    int view = int(pc.more.w + 0.5);
    if (view == 4)
        return reflection;
    return body * (1.0 - fresnel) + reflection * fresnel + highlight;
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

    /* the normal: CE's ripples (its bump map's signed x and y) about the plane's, fading with distance (their detail
    then the roughness's) */
    vec3 rip = texelFetch(ripples, pixel, 0).rgb * 2.0 - 1.0;
    vec3 tangent = normalize(abs(n.z) < 0.9 ? cross(vec3(0.0, 0.0, 1.0), n) : cross(n, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(n, tangent);
    float far_fade = smoothstep(8.0, 160.0, view_length);
    /* (CE's ripples flattened edge on and far off, where its map's texels would alias into blocks; what they lose
    the reflection's blur keeps as roughness) */
    float edge_on = clamp(abs(dot(normalize(ray), n)) * 3.0, 0.0, 1.0);
    float strength = pc.tuning.y * mix(1.0, 0.35, far_fade) * mix(0.45, 1.0, edge_on);
    blur_radius = (1.0 + 10.0 * far_fade) * mix(1.6, 1.0, edge_on) * float(pc.flags.z) / 720.0;
    vec3 normal = normalize(n + strength * (rip.x * tangent + rip.y * bitangent));
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
    if (pc.fog.a > 0.0)
        deep = light_of(pc.fog.rgb) * pc.more.x;

    vec3 color;
    vec3 reflected_shown = vec3(0.0), refracted_shown = vec3(0.0);
    if (above)
    {
        float cos_v = clamp(dot(v, normal), 0.0, 1.0);
        float fresnel = schlick(0.02, cos_v);
        /* the reflection: along the screen, the sky through the frame, else CE's cube in CE's tint */
        vec3 r = reflect(-v, normal);
        float under = dot(r, n);
        if (under < 0.02)
            r = normalize(r + n * (0.02 - under));
        vec3 ce_tint = mix(pc.parallel.rgb, pc.perpendicular.rgb, clamp(dot(v, n), 0.0, 1.0));
        vec3 environment = light_of(textureLod(cube, r, 0.0).rgb * ce_tint) * pc.more.z;
        vec4 sky = far_reflection(r, true);
        environment = mix(environment, sky.rgb, sky.a);
        vec4 found = screen_reflection(p, r, t, true);
        vec3 reflection = mix(environment, found.rgb, found.a);

        /* the refraction: the floor the refracted ray reaches (the pixel's own where the screen has it not) */
        vec3 tr = refract(-v, normal, 1.0 / 1.33);
        vec3 floor0 = opaque0 < NONE ? eye + opaque0 * ray : p - n * 40.0;
        float depth0 = max(d - dot(n, floor0), 0.0);
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
        vec3 seen = light_of(textureLod(behind, refracted_uv, 0.0).rgb);
        vec3 transmitted;
        if (opaque >= NONE)
        {
            /* (no floor on the frame: what CE drew there, its fog beyond the world) */
            transmitted = seen;
        }
        else
        {
            float path = length(floor_point - p);
            vec3 through = exp(-sigma * path);
            transmitted = seen * through + deep * (1.0 - through);
        }
        /* the sun's highlight off the ripples */
        vec3 highlight = vec3(0.0);
        if (pc.sun_direction.w > 0.5)
        {
            vec3 l = normalize(pc.sun_direction.xyz);
            vec3 h = normalize(l + v);
            float nl = clamp(dot(normal, l), 0.0, 1.0), nv = max(dot(normal, v), 1.0e-3), nh = clamp(dot(normal, h),
                0.0, 1.0);
            float rough = 0.07 + 0.16 * far_fade;
            float a2 = rough * rough * rough * rough;
            float den = nh * nh * (a2 - 1.0) + 1.0;
            float dist = a2 / (PI * den * den);
            float vis = 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1.0e-4);
            highlight = min(dist * vis * schlick(0.02, clamp(dot(v, h), 0.0, 1.0)) * nl, 64.0) *
                light_of(pc.sun_color.rgb) * pc.tuning.w * clamp(dot(n, l) * 4.0, 0.0, 1.0);
        }
        reflected_shown = reflection * mask;
        refracted_shown = transmitted;
        color = transmitted * (1.0 - fresnel) + reflection * fresnel * mask + highlight;
        /* (the shore: the water's edge fades into the ground it lies on) */
        color = mix(light_of(was.rgb), color, smoothstep(0.0, 0.05, depth0));
    }
    else
    {
        /* from under the surface: Snell's window, total internal reflection outside it */
        vec3 facing_normal = -normal;
        vec3 tr = refract(-v, facing_normal, 1.33);
        /* (total internal reflection: the water's depths, its deep colour, and what the screen has under the surface
        along the reflected ray) */
        vec3 r = reflect(-v, facing_normal);
        vec4 found = screen_reflection(p, r, t, false);
        vec3 reflection = mix(deep, found.rgb, found.a);
        if (dot(tr, tr) < 1.0e-6)
        {
            color = reflection;
        }
        else
        {
            /* (Snell's window: the sky above, CE's cube in its tint, brighter where the refracted ray rises
            straight) */
            float fresnel = schlick(0.02, clamp(dot(tr, n), 0.0, 1.0));
            vec3 ce_tint = mix(pc.parallel.rgb, vec3(1.0), clamp(dot(tr, n), 0.0, 1.0));
            vec3 seen = light_of(textureLod(cube, tr, 0.0).rgb * ce_tint) * pc.more.z;
            color = seen * (1.0 - fresnel) + reflection * fresnel;
        }
        /* (the water between the eye and the surface: a little of the deep colour over long paths; CE's planar fog
        never fogged its water's surface, drawn after it, and its few tenths of a unit would hide Snell's window) */
        color = mix(color, deep, 0.6 * (1.0 - exp(-view_length / 8.0)));
        reflected_shown = reflection;
        refracted_shown = color;
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
