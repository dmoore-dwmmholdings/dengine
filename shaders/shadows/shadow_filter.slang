// Stable S15 contract: texture selects a sampled 2D mirror; layer identifies its canonical
// depth-array layer. Atlas clients provide a clamp rectangle of safe texel centres.
module shadow_filter;
import bindless;
__exported import pcss;
public float shadow_pcf(ShadowMapRef map, float3 uvz, float filter_radius_uv)
{
    if (map.texture == 0u || map.resolution == 0u) return 1.0;
    uint taps = clamp(map.taps, 1u, 32u);
    float lit = 0.0;
    for (uint i = 0; i < taps; ++i)
    {
        float radius = sqrt((float(i) + 0.5) / float(taps));
        float angle = float(i) * 2.399963229728653;
        float2 offset = radius * float2(cos(angle), sin(angle)) * max(filter_radius_uv, 0.0);
        float2 uv = clamp(uvz.xy + offset, map.uv_clamp.xy, map.uv_clamp.zw);
        int2 texel = clamp(int2(floor(uv * float(map.resolution))), int2(0),
                          int2(map.resolution - 1u));
        float depth = de_tex2d(map.texture).Load(int3(texel, 0)).r;
        lit += uvz.z >= depth ? 1.0 : 0.0; // reverse-Z: receiver nearer than stored caster
    }
    return lit / float(taps);
}

public float shadow_filter(ShadowMapRef map, float3 uvz, float filter_radius_uv)
{
    // Do not perturb the original PCF arithmetic, taps, order or rotation on either fallback.
    if (!(map.depth_projection.w > 0.0) || map.filter_state.y <= 1u ||
        map.filter_state.x == 0u)
        return shadow_pcf(map, uvz, filter_radius_uv);
    return de_shadow_pcss(map, uvz);
}
