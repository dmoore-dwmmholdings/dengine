// shaders/volumetrics/froxel_common.slang: the froxel volumetric fog's shared code (DENG-207,
// S34; SPEC §12.13). The constants mirror src/render/passes/fog/fog_internal.h (de__fog_header,
// de__fog_volume_gpu). Volumes are 2D atlases: slice k is the tile at column k % cols, row
// k / cols, each tile grid.x x grid.y texels.
// - The media and light passes (froxel_inject_media, froxel_inject_light) write one texel per
//   froxel, evaluated at slice coordinate k + jitter.
// - The integrated volume (froxel_integrate) holds, at texel k, the in-scattered light (rgb,
//   pre-exposed) and the transmittance (a) from the camera to the end of slice k.
// - froxel_apply is the one function every consumer calls: fog_apply for the opaque surfaces and
//   the sky, the forward transparency pass (and particles) for blended surfaces.
module froxel_common;

import bindless;

public static const float FOG_PI = 3.14159265358979323846;

/// One view's fog constants (224 bytes; fog_internal.h de__fog_header).
public struct FogHeader
{
    public uint4 grid;    // w, h, d, atlas columns
    public uint4 tex;     // integrated volume (sampled), linear clamp sampler, volumes, count
    public float4 depth;  // near, far, ln(far / near), slice jitter (0..1)
    public float4 ray00;  // camera-relative offset per unit view depth at uv (0, 0); w: history weight
    public float4 ray_du; // its change across the width; w: pre-exposure
    public float4 ray_dv; // its change down the height; w: orthographic (0/1)
    public float4 org00;  // orthographic: camera-relative origin at uv (0, 0); w: first volume
    public float4 org_du;
    public float4 org_dv;
    public float4 camera; // camera position (render-origin space)
    public float4 prev_vp[4]; // last frame's view-projection (rows, render-origin space)
};

/// One packed fog volume (96 bytes; fog_internal.h de__fog_volume_gpu).
public struct FogVolume
{
    public float4 inv[3];         // rows: camera-relative position -> box-local position
    public float4 extent_density; // half extents (x < 0: global fog), density (1/m)
    public float4 albedo_g;       // scattering albedo rgb, anisotropy g
    public float4 params;         // height falloff (1/m), noise (0..1), edge fade (m), base z
};

public FogHeader fog_header(uint header, uint entry)
{
    return de_robuffer<FogHeader>(header)[entry];
}

/// The view depth (m) at slice coordinate `s` in [0, d].
public float fog_slice_depth(FogHeader h, float s)
{
    return h.depth.x * exp(h.depth.z * s / float(h.grid.z));
}

/// The slice coordinate of view depth `z` (unclamped; very negative for z <= 0).
public float fog_slice_of(FogHeader h, float z)
{
    if (!(z > 0.0))
        return -1e9;
    return float(h.grid.z) * log(z / h.depth.x) / h.depth.z;
}

/// The camera-relative ray (per unit view depth) and origin at screen uv (0..1, top-left).
public void fog_ray(FogHeader h, float2 uv, out float3 org, out float3 ray)
{
    ray = h.ray00.xyz + h.ray_du.xyz * uv.x + h.ray_dv.xyz * uv.y;
    org = h.org00.xyz + h.org_du.xyz * uv.x + h.org_dv.xyz * uv.y;
}

/// The atlas texel of froxel (x, y, k).
public uint2 fog_texel(FogHeader h, uint3 f)
{
    uint cols = h.grid.w;
    return uint2((f.z % cols) * h.grid.x + f.x, (f.z / cols) * h.grid.y + f.y);
}

/// Bilinear in tile `k` at uv (texel centres clamped: no bleed between tiles).
float4 fog_tile(FogHeader h, Texture2D<float4> t, SamplerState smp, float2 uv, uint k)
{
    uint cols = h.grid.w;
    uint rows = (h.grid.z + cols - 1u) / cols;
    float2 g = float2(h.grid.xy);
    float2 c = clamp(uv, 0.5 / g, 1.0 - 0.5 / g);
    float2 a = (float2(float(k % cols), float(k / cols)) + c) / float2(float(cols), float(rows));
    return t.SampleLevel(smp, a, 0.0);
}

/// Trilinear in volume `tex` at uv and slice coordinate `w` (texel k at w = k; clamped).
public float4 fog_sample(FogHeader h, uint tex, float2 uv, float w)
{
    Texture2D<float4> t = de_tex2d(tex);
    SamplerState smp = de_sampler(h.tex.y);
    float d = float(h.grid.z);
    w = clamp(w, 0.0, d - 1.0);
    float s0 = floor(w);
    float s1 = min(s0 + 1.0, d - 1.0);
    float4 a = fog_tile(h, t, smp, uv, uint(s0));
    float4 b = fog_tile(h, t, smp, uv, uint(s1));
    return lerp(a, b, w - s0);
}

/// The fog between the camera and view depth `view_depth` at screen uv: rgb in-scattered light
/// (pre-exposed), a transmittance. Fades to none at the camera inside the first slice; past the
/// far distance the last slice holds (the aerial perspective takes over behind it).
public float4 froxel_fog_at(FogHeader h, float2 uv, float view_depth)
{
    float s = fog_slice_of(h, view_depth); // texel k holds the end of slice k: s = k + 1
    float fade = saturate(s);
    float4 f = fog_sample(h, h.tex.x, uv, s - 1.0);
    return lerp(float4(0.0, 0.0, 0.0, 1.0), f, fade);
}

/// Applies the fog to a premultiplied colour (rgb already times alpha) at screen uv and view
/// depth: (rgb T + S alpha, alpha). `header` 0 (no fog this frame): unchanged. The opaque apply
/// passes alpha 1.
public float4 froxel_apply(uint header, uint entry, float2 uv, float view_depth, float3 rgb,
                           float alpha)
{
    if (header == DE_BINDLESS_INVALID)
        return float4(rgb, alpha);
    FogHeader h = fog_header(header, entry);
    if (h.tex.x == DE_BINDLESS_INVALID)
        return float4(rgb, alpha);
    float4 f = froxel_fog_at(h, uv, max(view_depth, 0.0));
    return float4(rgb * f.a + f.rgb * alpha, alpha);
}

/// The Henyey-Greenstein phase function (1/sr) for anisotropy g and cos(theta) between the
/// light's travel and the scattered direction.
public float fog_phase_hg(float g, float cos_theta)
{
    float g2 = g * g;
    float k = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-4);
    return (1.0 - g2) / (4.0 * FOG_PI * k * sqrt(k));
}
