// shaders/volumetrics/clouds_shadow.slang: the clouds' shadow map (DENG-208, S34). Engine shader
// `clouds_shadow`, entry cs_clouds_shadow, pass clouds.shadow. Top-down: texel (x, y) is a point
// of the layer-bottom plane (CloudShadow area, lighting_inputs.slang de_input_cloud_shadow); it
// stores the layer's transmittance from there toward the sun. The texture is n wide and 2 n high
// (DENG-854): rows 0..n-1 are the coarse cascade, marched through the base shape only (64 m
// texels: no detail erosion); rows n..2n-1 are the fine cascade (8 m texels), marched with the
// detail noise too where the quality has it (CloudsHeader.misc.w) and written only when
// ShadowParams.fine.w is set.
import bindless;
import push_constants;
import "clouds_common";

struct ShadowParams
{
    uint header, entry, target, n;
    float4 area; // centre x, y (render-origin), extent (m), layer bottom z (render-origin)
    float4 fine; // fine cascade: centre x, y, extent (m), 1: on
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_shadow(uint3 id : SV_DispatchThreadID, uniform ShadowParams pc)
{
    bool fine = id.y >= pc.n;
    if (id.x >= pc.n || id.y >= 2u * pc.n || (fine && pc.fine.w == 0.0))
        return;
    CloudsHeader h = clouds_header(pc.header, pc.entry);
    float2 uv = (float2(id.x, id.y - (fine ? pc.n : 0u)) + 0.5) / float(pc.n);
    float2 centre = fine ? pc.fine.xy : pc.area.xy;
    float extent = fine ? pc.fine.z : pc.area.z;
    float3 p = float3(centre + (uv - 0.5) * extent, pc.area.w);
    bool detail = fine && h.misc.w > 0.5;
    float3 rel = p - h.camera.xyz;
    float3 sun = h.sun.xyz;
    float t = 1.0;
    uint steps = max(h.steps.w, 1u);
    if (sun.z > 0.02)
    {
        float thick = h.layer.z - h.layer.y;
        float len = min(thick / sun.z, thick * 8.0);
        float dt = len / float(steps);
        float od = 0.0;
        for (uint i = 0; i < steps; ++i)
            od += clouds_extinction(h, rel + sun * ((float(i) + 0.5) * dt), detail) * dt;
        t = exp(-od);
    }
    de_rwtex2d(pc.target)[id.xy] = float4(t, t, t, 1.0);
}
