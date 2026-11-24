// shaders/volumetrics/froxel_integrate.slang: front-to-back integration of the froxel fog
// (DENG-207, S34). Engine shader `fog_integrate`, entry cs_fog_integrate: one thread per froxel
// column walks its slices from the camera (slice 0 starts at the camera) and writes, at texel k,
// the in-scattered light reaching the camera (rgb) and the transmittance (a) from the camera to
// the end of slice k. Each slice is a homogeneous segment (energy-conserving: S (1 - T) / ext).
import bindless;
import push_constants;
import "froxel_common";

struct IntegrateParams
{
    uint header, entry;
    uint src; // sampled: the lit media (in-scatter rgb, extinction)
    uint dst; // storage: the integrated volume
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_fog_integrate(uint3 id : SV_DispatchThreadID, uniform IntegrateParams pc)
{
    FogHeader h = fog_header(pc.header, pc.entry);
    if (any(id.xy >= h.grid.xy))
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(h.grid.xy);
    float3 org, ray;
    fog_ray(h, uv, org, ray);
    float len = length(ray);
    float3 L = float3(0.0, 0.0, 0.0);
    float T = 1.0;
    float z0 = 0.0;
    Texture2D<float4> src = de_tex2d(pc.src);
    RWTexture2D<float4> dst = de_rwtex2d(pc.dst);
    for (uint k = 0; k < h.grid.z; ++k)
    {
        float z1 = fog_slice_depth(h, float(k + 1u));
        float dt = (z1 - z0) * len;
        z0 = z1;
        uint2 t = fog_texel(h, uint3(id.xy, k));
        float4 m = src.Load(int3(int2(t), 0));
        float ext = max(m.a, 0.0);
        float st = exp(-ext * dt);
        float3 S = max(m.rgb, float3(0.0, 0.0, 0.0));
        L += T * (ext > 1e-7 ? S * (1.0 - st) / ext : S * dt);
        T *= st;
        dst[t] = float4(L, T);
    }
}
