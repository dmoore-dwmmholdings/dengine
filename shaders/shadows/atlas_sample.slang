module atlas_sample;
import bindless;
__exported import shadow_filter;
public struct AtlasShadow
{
    public float4 matrix[4];
    public float4 position;
    public float4 projection; // near, far, tan(half FOV), source radius (metres)
    public float4 tile; // atlas UV offset.xy, scale.zw
    public uint4 identity; // entity low/high, face count, coherent valid
    public ShadowMapRef map;
};
// Run once per scene light in light preparation, never per shaded pixel.
public uint atlas_find(uint metadata, uint2 identity)
{
    if (metadata == 0u) return 0u;
    uint count = min(de_robuffer<AtlasShadow>(metadata)[0].identity.x, 32u);
    for (uint i = 0u; i < count; ++i)
    {
        uint index = 1u + i * 6u;
        AtlasShadow s = de_robuffer<AtlasShadow>(metadata)[index];
        if (all(s.identity.xy == identity) && s.identity.w != 0u) return index;
    }
    return 0u;
}
public uint atlas_face(float3 direction)
{
    float3 a = abs(direction);
    return a.x >= a.y && a.x >= a.z ? (direction.x >= 0.0 ? 0u : 1u) :
           a.y >= a.z ? (direction.y >= 0.0 ? 2u : 3u) : (direction.z >= 0.0 ? 4u : 5u);
}
public float atlas_shadow(uint metadata, uint index, float3 p, float3 normal)
{
    if (metadata == 0u) return 1.0;
    // Select a bounded map before PCF, including excluded lanes. As in CSM, resource loads
    // must stay outside lane-dependent fallback control on portable SIMD implementations.
    bool eligible = index > 0u && index <= 192u;
    uint selected = clamp(index,1u,192u);
    AtlasShadow first = de_robuffer<AtlasShadow>(metadata)[selected];
    uint face = first.identity.z == 6u ? atlas_face(p - first.position.xyz) : 0u;
    AtlasShadow s = de_robuffer<AtlasShadow>(metadata)[min(selected + face,192u)];
    float distance = length(p - s.position.xyz);
    float face_depth = dot(s.matrix[3], float4(p,1));
    float tile_pixels = s.tile.z * float(s.map.resolution);
    float texel = 2.0 * max(face_depth,0.0) * s.projection.z / max(tile_pixels,1.0);
    float3 light = (s.position.xyz - p) / max(distance,1e-8);
    float ndl = saturate(dot(normal, light));
    float slope = sqrt(max(1.0 - ndl * ndl, 0.0)) / max(ndl, 0.1);
    float4 h = float4(p + normal * texel * 0.5, 1.0);
    float4 clip = float4(dot(s.matrix[0],h),dot(s.matrix[1],h),
                         dot(s.matrix[2],h),dot(s.matrix[3],h));
    float3 uvz = clip.xyz / max(clip.w, 1e-8);
    bool covered = clip.w > 0.0 && all(abs(uvz.xy) <= 1.0) && uvz.z >= 0.0 && uvz.z <= 1.0;
    uvz.xy = (uvz.xy * float2(0.5,-0.5) + 0.5) * s.tile.zw + s.tile.xy;
    // Perspective reverse-Z derivative, in depth per metre; near/far stay explicit metadata.
    float derivative = s.projection.x * s.projection.y /
                       max((s.projection.y - s.projection.x) * clip.w * clip.w, 1e-8);
    uvz.z += derivative * texel * (0.5 + min(slope, 4.0));
    float value = shadow_filter(s.map, uvz, 1.5 / float(s.map.resolution));
    return eligible && covered && distance <= s.position.w && first.identity.w != 0u ? value : 1.0;
}

// A scissored draw clears only one allocation, including its guards. Attachment clear is
// deliberately disabled: it would invalidate every other cached tile in the same layer.
[shader("vertex")]
float4 vs_atlas_clear(uint vertex : SV_VertexID) : SV_Position
{
    float2 p = vertex == 0u ? float2(-1,-1) : vertex == 1u ? float2(3,-1) : float2(-1,3);
    return float4(p,0,1);
}
[shader("fragment")]
void ps_atlas_clear() {}

import push_constants;
struct AtlasQueryParams { uint metadata; uint queries; uint output; uint count; };
public struct AtlasQuery { public float4 position; public float4 normal; public uint4 index; };
[shader("compute")]
[numthreads(64,1,1)]
void cs_atlas_query(uint3 id:SV_DispatchThreadID, uniform AtlasQueryParams pc)
{
    if(id.x>=pc.count)return;
    AtlasQuery q=de_robuffer<AtlasQuery>(pc.queries)[id.x];
    float v=atlas_shadow(pc.metadata,q.index.x,q.position.xyz,q.normal.xyz);
    de_buffer<float4>(pc.output)[id.x]=float4(v,0,0,1);
}
