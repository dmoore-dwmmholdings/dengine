// shaders/volumetrics/clouds_shadow.slang: the clouds' shadow map (DENG-208, S34). Engine shader
// `clouds_shadow`, entry cs_clouds_shadow, pass clouds.shadow. Top-down: texel (x, y) is a point
// of the layer-bottom plane (CloudShadow area, lighting_inputs.slang de_input_cloud_shadow); it
// stores the layer's transmittance from there toward the sun. The texture is n wide and 2 n high
// (DENG-854): rows 0..n-1 are the coarse cascade, marched through the base shape only (64 m
// texels: no detail erosion); rows n..2n-1 are the fine cascade (8 m texels), marched with the
// detail noise too where the quality has it (CloudsHeader.misc.w) and written only when
// ShadowParams.fine.w is set.
// Entry cs_clouds_ground (pass clouds.ground, DENG-867) samples the depth buffer on an 8x8 screen
// grid and writes each visible receiver's camera-relative position. The CPU reads it back a few
// frames later to centre the fine cascade on the receivers' sun crossings.
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

// DENG-867: entry cs_clouds_ground, pass clouds.ground. One 8 x 8 group samples the depth buffer
// on an 8 x 8 grid of the screen; each thread writes the receiver's position relative to the
// camera (w 1) or 0 for the sky. The CPU reads them back a few frames later
// (de__clouds_ground_estimate) to place the fine cascade over the ground below a high camera.
struct GroundParams
{
    uint header, entry, depth, target;
    uint width, height, pad0, pad1;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_ground(uint3 id : SV_DispatchThreadID, uniform GroundParams pc)
{
    if (id.x >= 8u || id.y >= 8u)
        return;
    CloudsHeader h = clouds_header(pc.header, pc.entry);
    float2 uv = (float2(id.xy) + 0.5) / 8.0;
    uint2 px = min(uint2(uv * float2(pc.width, pc.height)), uint2(pc.width - 1u, pc.height - 1u));
    uv = (float2(px) + 0.5) / float2(pc.width, pc.height);
    float3 ray = h.ray00.xyz + h.ray_du.xyz * uv.x + h.ray_dv.xyz * uv.y;
    float3 dir = normalize(ray);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    float d = de_tex2d(pc.depth).Load(int3(int2(px), 0)).r;
    if (d > 0.0)
    {
        float zv = (d * h.proj.w - h.proj.y) / (h.proj.x - d * h.proj.z);
        float t = max(-zv, 0.0) * length(float3(h.vray.xy + h.vray.zw * uv, -1.0));
        o = float4(dir * t, 1.0);
    }
    de_buffer<float4>(pc.target)[id.y * 8u + id.x] = o;
}
