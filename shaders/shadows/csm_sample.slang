module csm_sample;
import bindless;
__exported import shadow_filter;
public struct CsmCascade
{
    public float4 matrix[4];
    public float4 sphere;
    public float4 light; // xyz direction towards scene, w world texel size
    public float4 params; // x inverse depth range, y source angular radius, z fade fraction
    public uint4 identity; // entity low/high, cascade count, reserved
    public ShadowMapRef map;
};
float csm_one(CsmCascade c, float3 p, float3 n)
{
    float ndl = saturate(dot(n, -c.light.xyz));
    float slope = sqrt(max(1.0 - ndl * ndl, 0.0)) / max(ndl, 0.1);
    float3 q = p + n * (c.light.w * 0.5);
    float4 h = float4(q, 1.0);
    float3 uvz = float3(dot(c.matrix[0], h), dot(c.matrix[1], h), dot(c.matrix[2], h));
    uvz.xy = uvz.xy * float2(0.5, -0.5) + 0.5;
    uvz.z += c.light.w * c.params.x * (0.5 + min(slope, 4.0));
    if (any(uvz.xy < 0.0) || any(uvz.xy > 1.0) || uvz.z < 0.0) return 1.0;
    return shadow_filter(c.map, uvz, 1.5 / float(c.map.resolution));
}
/// Identity comes from the directional cluster record, independent of atomic list order.
public float csm_shadow(uint metadata, float3 p, float3 n, uint2 identity)
{
    if (metadata == 0u) return 1.0;
    CsmCascade head = de_robuffer<CsmCascade>(metadata)[0];
    float distance = dot(p - head.sphere.xyz, head.light.xyz);
    if (distance < 0.0 || distance > head.sphere.w) return 1.0;
    uint lights = min(head.identity.x, 2u), cascades = min(head.identity.y, 4u);
    for (uint l = 0; l < lights; ++l)
    {
        uint base = 1u + l * cascades;
        CsmCascade first = de_robuffer<CsmCascade>(metadata)[base];
        if (any(first.identity.xy != identity)) continue;
        for (uint i = 0; i < cascades; ++i)
        {
            CsmCascade c = de_robuffer<CsmCascade>(metadata)[base + i];
            float radius = length(p - c.sphere.xyz) / c.sphere.w;
            if (radius > 1.0) continue;
            float value = csm_one(c, p, n);
            if (i + 1u < cascades && radius > 0.9)
            {
                CsmCascade next = de_robuffer<CsmCascade>(metadata)[base + i + 1u];
                if (length(p - next.sphere.xyz) <= next.sphere.w)
                    value = lerp(value, csm_one(next, p, n), smoothstep(0.9, 1.0, radius));
            }
            return value;
        }
    }
    return 1.0;
}

// Internal acceptance/readback hook; every backend runs the same production sampler.
import push_constants;
public struct ShadowQuery
{
    public float4 position;
    public float4 normal;
    public uint4 identity;
};
struct ShadowQueryParams
{
    uint metadata;
    uint queries;
    uint output;
    uint count;
    uint mode;
    float radius;
    uint reserved0;
    uint reserved1;
};
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_shadow_query(uint3 tid : SV_DispatchThreadID, uniform ShadowQueryParams pc)
{
    if (tid.x >= pc.count) return;
    ShadowQuery q = de_robuffer<ShadowQuery>(pc.queries)[tid.x];
    float visibility;
    if (pc.mode == 0u)
        visibility = shadow_filter(de_robuffer<CsmCascade>(pc.metadata)[0].map,
                                   q.position.xyz, pc.radius);
    else
        visibility = csm_shadow(pc.metadata, q.position.xyz, q.normal.xyz, q.identity.xy);
    de_buffer<float4>(pc.output)[tid.x] = float4(visibility, 0.0, 0.0, 1.0);
}
