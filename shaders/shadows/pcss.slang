module pcss;
import bindless;

public struct ShadowMapRef
{
    // The original 32-byte PCF prefix is stable.
    public uint texture;
    public uint layer;
    public uint taps;
    public uint resolution;
    public float4 uv_clamp;
    public float4 depth_projection; // near, far, tan-half-FOV, angular/metre source radius
    public float4 uv_projection; // UV/world or UV*depth/world xy, max radius texels, reserved
    public uint4 filter_state; // projection 1=ortho/2=perspective, quality, frame, blocker taps
};

public float de_shadow_linear_depth(ShadowMapRef m, float z)
{
    float n = m.depth_projection.x, f = m.depth_projection.y;
    return m.filter_state.x == 1u ? f - z * (f - n) :
           n * f / max(n + z * (f - n), 1e-8);
}
float de_shadow_depth_load(ShadowMapRef m, float2 uv)
{
    uv = clamp(uv, m.uv_clamp.xy, m.uv_clamp.zw);
    int2 texel = clamp(int2(floor(uv * float(m.resolution))), int2(0),
                       int2(m.resolution - 1u));
    return de_tex2d(m.texture).Load(int3(texel, 0)).r;
}
public float de_shadow_noise(float2 pixel, uint frame)
{
    float ign = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return frac(ign + float(frame & 1023u) * 0.61803398875);
}
float2 de_shadow_disk(uint i, uint count, float rotation)
{
    float angle = float(i) * 2.399963229728653 + rotation;
    return sqrt((float(i) + 0.5) / float(count)) * float2(cos(angle), sin(angle));
}
public float2 de_shadow_search_radius(ShadowMapRef m, float receiver)
{
    float source = m.depth_projection.w;
    float2 physical;
    if (m.filter_state.x == 1u)
        physical = receiver * tan(source) * m.uv_projection.xy;
    else
    {
        // Project the emitter disk from its light plane through the actual near plane.
        float n = max(m.depth_projection.x, 1e-6);
        physical = source * m.uv_projection.xy * max(receiver - n, 0.0) /
                   max(n * receiver, 1e-8);
    }
    return min(max(physical, 0.0), max(m.uv_projection.z, 0.0) / float(m.resolution));
}
public float2 de_shadow_penumbra_radius(ShadowMapRef m, float receiver, float blocker)
{
    float gap = max(receiver - blocker, 0.0);
    float2 physical = m.filter_state.x == 1u ?
        gap * tan(m.depth_projection.w) * m.uv_projection.xy :
        gap * m.depth_projection.w * m.uv_projection.xy / max(blocker * receiver, 1e-8);
    return min(max(physical, 0.0), max(m.uv_projection.z, 0.0) / float(m.resolution));
}
public float de_shadow_pcss(ShadowMapRef m, float3 uvz)
{
    if (m.texture == 0u || m.resolution == 0u) return 1.0;
    float receiver = de_shadow_linear_depth(m, uvz.z);
    float rotation = 6.283185307179586 *
        de_shadow_noise(floor(uvz.xy * float(m.resolution)), m.filter_state.z);
    float2 search = de_shadow_search_radius(m, receiver);
    uint count = clamp(m.filter_state.w, 1u, 32u);
    float blocker_sum = 0.0;
    uint blockers = 0u;
    for (uint i = 0u; i < count; ++i)
    {
        float z = de_shadow_depth_load(m, uvz.xy + de_shadow_disk(i, count, rotation) * search);
        if (z > 0.0 && z > uvz.z)
        {
            // Perspective reverse-Z is nonlinear: convert each blocker before averaging.
            blocker_sum += de_shadow_linear_depth(m, z);
            ++blockers;
        }
    }
    if (blockers == 0u) return 1.0;
    float2 radius = de_shadow_penumbra_radius(m, receiver, blocker_sum / float(blockers));
    uint taps = clamp(m.taps, 1u, 32u);
    float lit = 0.0;
    for (uint i = 0u; i < taps; ++i)
    {
        float z = de_shadow_depth_load(m, uvz.xy + de_shadow_disk(i, taps, rotation) * radius);
        lit += uvz.z >= z ? 1.0 : 0.0;
    }
    return lit / float(taps);
}
