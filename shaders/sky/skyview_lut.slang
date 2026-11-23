// shaders/sky/skyview_lut.slang: the sky-view LUT of Hillaire 2020 §5.3 (SPEC §12.13; DENG-206).
// Engine shader `sky_skyview`. Each view's frame makes one 192 x 108 RGBA16F LUT of the sky's
// luminance around the camera (sky_view_uv: world azimuth, horizon-weighted zenith), lit by the sun
// and the moon. The weights are illuminance x pre-exposure, so the texels are the view's
// pre-exposed luminance and fit half floats by day and by night.
import bindless;
import push_constants;
import "sky_common";

struct SkyViewParams
{
    uint4 idx;   // target (storage), transmittance, multiscatter, sampler (linear clamp)
    float4 sun;  // direction (world, +Z up), weight
    float4 moon; // direction, weight
    float4 misc; // radius of the camera (km), mie scale, ground albedo, steps
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_skyview(uint3 id : SV_DispatchThreadID, uniform SkyViewParams pc)
{
    if (id.x >= SKY_VIEW_W || id.y >= SKY_VIEW_H)
        return;
    float r = pc.misc.x;
    float2 uv = (float2(id.xy) + 0.5) / float2(SKY_VIEW_W, SKY_VIEW_H);
    float3 dir = sky_view_dir(r, uv);
    SkyLight sun, moon;
    sun.dir = pc.sun.xyz;
    sun.weight = pc.sun.w;
    moon.dir = pc.moon.xyz;
    moon.weight = pc.moon.w;
    float3 L = sky_integrate(pc.idx.y, pc.idx.z, pc.idx.w, pc.misc.y, pc.misc.z, r, dir, sun,
                             moon, uint(pc.misc.w));
    de_rwtex2d(pc.idx.x)[id.xy] = float4(min(L, float3(60000.0, 60000.0, 60000.0)), 1.0);
}
