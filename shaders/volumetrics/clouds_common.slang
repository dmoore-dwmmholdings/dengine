// shaders/volumetrics/clouds_common.slang: the volumetric clouds' constants and density field
// (DENG-208, S34). src/world/sky/clouds_internal.h describes the layer, the noises and the passes;
// de__clouds_header is CloudsHeader.
//
// Space: render-origin metres (z up), as the lights. `rel` is a point relative to the camera.
// The layer is a shell around the planet's centre, which is straight below the camera.
import bindless;
import "../sky/sky_common";

public static const float CLOUDS_PI = 3.14159265358979323846;
public static const uint CLOUDS_BASE_N = 128;
public static const uint CLOUDS_BASE_TILE = 130;
public static const uint CLOUDS_BASE_COLUMNS = 16;
public static const uint CLOUDS_DETAIL_N = 32;
public static const uint CLOUDS_DETAIL_TILE = 34;
public static const uint CLOUDS_DETAIL_COLUMNS = 8;
/// The extinction (1/m) of density 1 at full cloud.
public static const float CLOUDS_EXTINCTION = 0.04;
/// The single-scattering albedo of cloud droplets.
public static const float CLOUDS_ALBEDO = 0.99;
/// The farthest the march goes (m): about the horizon of the layer top.
public static const float CLOUDS_MAX_DISTANCE = 60000.0;
/// The largest value written (pre-exposed): as the sky's.
public static const float CLOUDS_MAX_VALUE = 60000.0;

public struct CloudsHeader
{
    public uint4 tex;     // base atlas, detail atlas, weather map, linear wrap sampler
    public uint4 tex2;    // transmittance LUT, linear clamp sampler, frame, flags
    public uint4 steps;   // primary, light, octaves, shadow steps
    public float4 layer;  // planet radius (m), bottom radius, top radius, camera altitude (m)
    public float4 shape;  // coverage, density, erosion, history weight
    public float4 off_wb; // tile offsets (m): weather xy, base xy
    public float4 off_d;  // detail xy, 1 / weather tile, 1 / base tile
    public float4 misc;   // 1 / detail tile, pre-exposure, camera z (render-origin), 0
    public float4 sun;    // toward the sun; w: illuminance at the top (pre-exposed)
    public float4 ray00;  // camera ray (world rotation) at uv (0, 0)
    public float4 ray_du; // its change across the width
    public float4 ray_dv; // its change down the height
    public float4 vray;   // view-space ray at z = -1: top-left (x, y), change (dx, dy)
    public float4 proj;   // projection m[10], m[14], m[11], m[15]
    public float4 camera; // camera position (render-origin space)
    public float4 prev_vp[4]; // last frame's view-projection (rows)
};

public CloudsHeader clouds_header(uint header, uint entry)
{
    return de_robuffer<CloudsHeader>(header)[entry];
}

/// The full-resolution pixel that block `m` marches in frame `frame`.
public uint2 clouds_block_pixel(uint2 m, uint frame, uint width, uint height)
{
    static const uint2 k_order[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
    return min(m * 2u + k_order[frame & 3u], uint2(width - 1u, height - 1u));
}

public float clouds_remap(float v, float a, float b, float c, float d)
{
    return c + (v - a) / max(b - a, 1e-5) * (d - c);
}

/// The planet's centre relative to the camera.
public float3 clouds_centre(CloudsHeader h)
{
    return float3(0.0, 0.0, -(h.layer.x + h.layer.w));
}

/// The altitude (m above the planet's surface) of `rel`.
public float clouds_altitude(CloudsHeader h, float3 rel)
{
    return length(rel - clouds_centre(h)) - h.layer.x;
}

/// The distances along `dir` (unit) from `org` (relative to the planet's centre) to a sphere of
/// radius `r`: x near, y far; y < 0: no hit ahead.
public float2 clouds_sphere(float3 org, float3 dir, float r)
{
    float b = dot(org, dir);
    float c = dot(org, org) - r * r;
    float disc = b * b - c;
    if (disc < 0.0)
        return float2(-1.0, -1.0);
    float s = sqrt(disc);
    return float2(-b - s, -b + s);
}

/// The part [x, y] of the ray from the camera along `dir` inside the layer (the first part only),
/// up to `limit`; x >= y: none.
public float2 clouds_segment(CloudsHeader h, float3 dir, float limit)
{
    float3 o = -clouds_centre(h);
    float r = length(o);
    float rb = h.layer.y, rt = h.layer.z;
    float2 top = clouds_sphere(o, dir, rt);
    float2 bot = clouds_sphere(o, dir, rb);
    float2 ground = clouds_sphere(o, dir, h.layer.x);
    float t0, t1;
    if (r < rb)
    {
        // Below the layer: from the bottom up to the top, unless the ground comes first.
        if (ground.x > 0.0)
            return float2(1.0, 0.0);
        t0 = bot.y;
        t1 = top.y;
    }
    else if (r < rt)
    {
        // Inside: to the top, or down to the bottom.
        t0 = 0.0;
        t1 = bot.x > 0.0 ? bot.x : top.y;
    }
    else
    {
        // Above: from the top down to the bottom, or out of the top again.
        if (top.x < 0.0)
            return float2(1.0, 0.0);
        t0 = top.x;
        t1 = bot.x > 0.0 ? bot.x : top.y;
    }
    return float2(max(t0, 0.0), min(min(t1, limit), t0 + CLOUDS_MAX_DISTANCE));
}

/// Tiling 3D noise in an atlas of slices with a one-texel wrapped gutter. `q` is in tiles.
float4 clouds_atlas(uint tex, uint smp, float3 q, uint n, uint tile, uint columns)
{
    float3 f = frac(q) * float(n);
    float zf = f.z - 0.5;
    float zi = floor(zf);
    float t = zf - zi;
    uint z0 = uint(zi + float(n)) % n;
    uint z1 = (z0 + 1u) % n;
    float2 size = float2(float(tile * columns), float(tile * (n / columns)));
    float2 o0 = float2(float((z0 % columns) * tile), float((z0 / columns) * tile)) + 1.0 + f.xy;
    float2 o1 = float2(float((z1 % columns) * tile), float((z1 / columns) * tile)) + 1.0 + f.xy;
    Texture2D<float4> a = de_tex2d(tex);
    SamplerState s = de_sampler(smp);
    return lerp(a.SampleLevel(s, o0 / size, 0.0), a.SampleLevel(s, o1 / size, 0.0), t);
}

/// The weather map at `rel`: r coverage, g type, b precipitation.
public float4 clouds_weather(CloudsHeader h, float3 rel)
{
    float2 uv = (h.off_wb.xy + rel.xy) * h.off_d.z;
    return de_tex2d(h.tex.z).SampleLevel(de_sampler(h.tex.w), uv, 0.0);
}

/// The local coverage: 0 everywhere at coverage 0, 1 everywhere (overcast) at coverage 1.
public float clouds_coverage(CloudsHeader h, float4 w)
{
    return saturate(w.r * 0.6 + h.shape.x * 1.6 - 0.8);
}

/// The height profile of cloud type `type` (0 stratus, 1 cumulus) at height fraction `f`.
public float clouds_profile(float f, float type)
{
    float top = lerp(0.35, 1.0, type);
    float bottom = saturate(clouds_remap(f, 0.0, 0.07, 0.0, 1.0));
    float upper = saturate(clouds_remap(f, top * 0.55, top, 1.0, 0.0));
    return bottom * upper;
}

/// The density (0..1, before DeSky.cloud_density) at `rel`. `detail`: erode with the detail noise.
public float clouds_density(CloudsHeader h, float3 rel, bool detail)
{
    float alt = clouds_altitude(h, rel);
    float bottom = h.layer.y - h.layer.x, top = h.layer.z - h.layer.x;
    float f = (alt - bottom) / max(top - bottom, 1.0);
    if (f <= 0.0 || f >= 1.0)
        return 0.0;
    float4 w = clouds_weather(h, rel);
    float cov = clouds_coverage(h, w);
    if (cov <= 0.0)
        return 0.0;
    // Overcast turns the layer into a deck: the type goes to a thick stratus-cumulus mix.
    float type = lerp(w.g, 0.6, saturate(h.shape.x * 2.0 - 1.0));
    float profile = clouds_profile(f, type);
    if (profile <= 0.0)
        return 0.0;
    float3 qb = float3((h.off_wb.zw + rel.xy) * h.off_d.w, (alt - bottom) * h.off_d.w);
    float4 b = clouds_atlas(h.tex.x, h.tex2.y, qb, CLOUDS_BASE_N, CLOUDS_BASE_TILE, CLOUDS_BASE_COLUMNS);
    float wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;
    float shape = clouds_remap(b.r, wfbm - 1.0, 1.0, 0.0, 1.0);
    float c = saturate(clouds_remap(shape * profile, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    if (c <= 0.0)
        return 0.0;
    if (detail && h.shape.z > 0.0)
    {
        float3 qd = float3((h.off_d.xy + rel.xy) * h.misc.x, (alt - bottom) * h.misc.x);
        float4 d = clouds_atlas(h.tex.y, h.tex2.y, qd, CLOUDS_DETAIL_N, CLOUDS_DETAIL_TILE,
                                CLOUDS_DETAIL_COLUMNS);
        float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
        // Wispy at the base, billowy higher up.
        float m = lerp(dfbm, 1.0 - dfbm, saturate(f * 8.0));
        c = saturate(clouds_remap(c, m * 0.35 * h.shape.z, 1.0, 0.0, 1.0));
    }
    // Rain darkens and thickens the bases.
    return c * (1.0 + w.b * 0.5);
}

/// The extinction (1/m) at `rel`.
public float clouds_extinction(CloudsHeader h, float3 rel, bool detail)
{
    return clouds_density(h, rel, detail) * h.shape.y * CLOUDS_EXTINCTION;
}

public float clouds_hg(float g, float c)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * CLOUDS_PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

/// The dual-lobe phase (a forward and a back lobe); `k` scales both eccentricities (octaves).
public float clouds_phase(float c, float k)
{
    return lerp(clouds_hg(-0.3 * k, c), clouds_hg(0.8 * k, c), 0.7);
}

/// The sun's illuminance (pre-exposed) arriving at the top of the cloud at `rel`: the sky's
/// transmittance from there.
public float3 clouds_sun_light(CloudsHeader h, float3 rel)
{
    float3 p = rel - clouds_centre(h);
    float r = length(p);
    float mu = dot(p / r, h.sun.xyz);
    return h.sun.w * sky_transmittance(h.tex2.x, h.tex2.y, r * 0.001, mu);
}
