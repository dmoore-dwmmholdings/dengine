// shaders/sky/sky_render.slang: draws the sky into the HDR lighting target where the visibility
// depth is the far plane (SPEC §12.13; DENG-206). Engine shader `sky_render`. The sky-view LUT
// gives the scattered light; the sun disk (limb darkening) and the moon disk (lit by the sun, so
// it shows its phase) are added with the transmittance toward them. Values are pre-exposed.
import bindless;
import push_constants;
import "sky_common";

struct SkyRenderParams
{
    uint4 idx0;   // sky-view LUT, transmittance LUT, sampler (linear, wrap u), sampler (linear clamp)
    uint4 idx1;   // depth, target (storage), width, height
    float4 ray00; // the camera ray at pixel (0, 0) corner (world, unnormalized); w: camera radius km
    float4 ray_du; // its change across the width; w: sun disk luminance scale (pre-exposed)
    float4 ray_dv; // its change down the height; w: moon disk luminance scale (pre-exposed)
    float4 sun;    // direction toward the sun; w: angular radius (rad)
    float4 moon;   // direction toward the moon; w: angular radius (rad)
    float4 misc;   // x: the largest value written, y: moon albedo, zw: 0
};

/// The sun's limb darkening (Neckel and Labs 1994 fit, normalized to mean 1 over the disk).
float limb(float q)
{
    float m = sqrt(max(0.0, 1.0 - q * q));
    // u = 0.6: I(m) = 1 - u (1 - m); the disk mean of that is 1 - u / 3.
    return (1.0 - 0.6 * (1.0 - m)) / (1.0 - 0.6 / 3.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_sky(uint3 id : SV_DispatchThreadID, uniform SkyRenderParams pc)
{
    if (id.x >= pc.idx1.z || id.y >= pc.idx1.w)
        return;
    int2 p = int2(id.xy);
    float depth = de_tex2d(pc.idx1.x).Load(int3(p, 0)).r;
    if (depth > 0.0)
        return; // geometry (reverse Z: 0 is the far plane)
    float2 uv = (float2(id.xy) + 0.5) / float2(pc.idx1.zw);
    float3 dir = normalize(pc.ray00.xyz + pc.ray_du.xyz * uv.x + pc.ray_dv.xyz * uv.y);
    float r = pc.ray00.w;
    float3 L = sky_radiance(pc.idx0.x, pc.idx0.z, r, dir);
    if (!sky_hits_ground(r, dir.z))
    {
        // The sun disk.
        float cs = dot(dir, pc.sun.xyz);
        float rs = pc.sun.w;
        if (cs > cos(rs))
        {
            float q = sqrt(max(0.0, 1.0 - cs * cs)) / sin(rs);
            float3 t = sky_transmittance(pc.idx0.y, pc.idx0.w, r, dir.z);
            float solid = 2.0 * SKY_PI * (1.0 - cos(rs));
            L += pc.ray_du.w / solid * limb(q) * t;
        }
        // The moon disk: a Lambert sphere lit by the sun.
        float cm = dot(dir, pc.moon.xyz);
        float rm = pc.moon.w;
        if (cm > cos(rm))
        {
            float3 o = dir - pc.moon.xyz * cm;
            float3 q = o / sin(rm);
            float qq = min(dot(q, q), 1.0);
            float3 n = q - pc.moon.xyz * sqrt(1.0 - qq); // the surface normal, toward the viewer
            float lit = saturate(dot(n, pc.sun.xyz));
            float3 t = sky_transmittance(pc.idx0.y, pc.idx0.w, r, dir.z);
            L += pc.ray_dv.w * pc.misc.y / SKY_PI * lit * t;
        }
    }
    L = min(L, float3(pc.misc.x, pc.misc.x, pc.misc.x));
    de_rwtex2d(pc.idx1.y)[p] = float4(L, 1.0);
}
