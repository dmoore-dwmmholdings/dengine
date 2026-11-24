// shaders/volumetrics/clouds_shadow.slang: the clouds' shadow map (DENG-208, S34). Engine shader
// `clouds_shadow`, entry cs_clouds_shadow, pass clouds.shadow. Top-down: texel (x, y) is a point
// of the layer-bottom plane (CloudShadow area, lighting_inputs.slang de_input_cloud_shadow); it
// stores the layer's transmittance from there toward the sun, marched through the base shape
// (no detail erosion: the map is coarse).
import bindless;
import push_constants;
import "clouds_common";

struct ShadowParams
{
    uint header, entry, target, n;
    float4 area; // centre x, y (render-origin), extent (m), layer bottom z (render-origin)
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_shadow(uint3 id : SV_DispatchThreadID, uniform ShadowParams pc)
{
    if (id.x >= pc.n || id.y >= pc.n)
        return;
    CloudsHeader h = clouds_header(pc.header, pc.entry);
    float2 uv = (float2(id.xy) + 0.5) / float(pc.n);
    float3 p = float3(pc.area.xy + (uv - 0.5) * pc.area.z, pc.area.w);
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
            od += clouds_extinction(h, rel + sun * ((float(i) + 0.5) * dt), false) * dt;
        t = exp(-od);
    }
    de_rwtex2d(pc.target)[id.xy] = float4(t, t, t, 1.0);
}
