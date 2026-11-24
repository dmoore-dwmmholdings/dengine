// shaders/volumetrics/froxel_inject_media.slang: the media of the froxel fog (DENG-207, S34).
// Engine shader `fog_inject_media`, entry cs_fog_media: one thread per froxel sums every packed
// DeFogVolume at the froxel's jittered point (fog_internal.h; packed by fog_volumes.c):
// - a box (camera-relative inverse transform, half extents) with soft faces (edge fade, m);
// - global height fog (half extent x < 0): density exp(-falloff (z - base z));
// - optional 3D value noise: density x lerp(1, 2 n, noise), in the volume's own space.
// Writes media0 = (scattering rgb, extinction) (1/m) and media1.x = the scattering-weighted
// anisotropy.
import bindless;
import push_constants;
import "froxel_common";

struct MediaParams
{
    uint header; // FogHeader ring
    uint entry;
    uint media0; // storage
    uint media1; // storage
};

float fog_hash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

/// Value noise in [0, 1].
float fog_noise(float3 x)
{
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float a = lerp(lerp(fog_hash(i), fog_hash(i + float3(1, 0, 0)), f.x),
                   lerp(fog_hash(i + float3(0, 1, 0)), fog_hash(i + float3(1, 1, 0)), f.x), f.y);
    float b = lerp(lerp(fog_hash(i + float3(0, 0, 1)), fog_hash(i + float3(1, 0, 1)), f.x),
                   lerp(fog_hash(i + float3(0, 1, 1)), fog_hash(i + float3(1, 1, 1)), f.x), f.y);
    return lerp(a, b, f.z);
}

/// The density (1/m) of volume `v` at camera-relative point `p`.
float fog_volume_density(FogVolume v, float3 p)
{
    float4 hp = float4(p, 1.0);
    float3 q = float3(dot(v.inv[0], hp), dot(v.inv[1], hp), dot(v.inv[2], hp));
    float d = v.extent_density.w;
    if (v.extent_density.x < 0.0)
    {
        // Global height fog; the exponent is capped so a deep valley does not overflow.
        d *= exp(-clamp(v.params.x * (p.z - v.params.w), -8.0, 80.0));
    }
    else
    {
        float3 e = v.extent_density.xyz - abs(q);
        float inside = min(e.x, min(e.y, e.z));
        if (inside < 0.0)
            return 0.0;
        if (v.params.z > 0.0)
            d *= smoothstep(0.0, 1.0, inside / v.params.z);
    }
    if (v.params.y > 0.0)
        d *= lerp(1.0, 2.0 * fog_noise(q * 0.5), v.params.y);
    return d;
}

[shader("compute")]
[numthreads(4, 4, 4)]
void cs_fog_media(uint3 id : SV_DispatchThreadID, uniform MediaParams pc)
{
    FogHeader h = fog_header(pc.header, pc.entry);
    if (any(id >= h.grid.xyz))
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(h.grid.xy);
    float z = fog_slice_depth(h, float(id.z) + h.depth.w);
    float3 org, ray;
    fog_ray(h, uv, org, ray);
    float3 p = org + ray * z;
    float3 scat = float3(0.0, 0.0, 0.0);
    float ext = 0.0, gw = 0.0;
    uint first = uint(h.org00.w);
    for (uint i = 0; i < h.tex.w; ++i)
    {
        FogVolume v = de_robuffer<FogVolume>(h.tex.z)[first + i];
        float d = fog_volume_density(v, p);
        if (!(d > 0.0))
            continue;
        float3 s = v.albedo_g.rgb * d;
        scat += s;
        ext += d;
        gw += v.albedo_g.w * dot(s, float3(0.2126, 0.7152, 0.0722));
    }
    float lum = dot(scat, float3(0.2126, 0.7152, 0.0722));
    uint2 t = fog_texel(h, id);
    de_rwtex2d(pc.media0)[t] = float4(scat, ext);
    de_rwtex2d(pc.media1)[t] = float4(lum > 0.0 ? gw / lum : 0.0, 0.0, 0.0, 0.0);
}
