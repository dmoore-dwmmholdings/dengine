import bindless;
import push_constants;
import contact_common;
import pcss;
import "../scene/scene_types";
import "../visbuffer/gbuffer";

struct ContactParams
{
    uint scene, slot, depth, normal;
    uint records, packed, width, height;
    uint pitch, local_offset, steps, frame;
    float length, thickness, normal_bias, reserved;
};
float3 contact_position(DeSceneConstants sc, float2 uv, float depth)
{
    float4 p = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 h = float4(dot(sc.inv_view_proj[0], p), dot(sc.inv_view_proj[1], p),
                      dot(sc.inv_view_proj[2], p), dot(sc.inv_view_proj[3], p));
    return h.xyz / h.w;
}
float contact_visibility(ContactParams pc, DeSceneConstants sc, uint2 pixel, float3 p,
                         float3 normal, ContactLightRecord light)
{
    float3 delta = light.position_range.xyz - p;
    float distance = length(delta);
    float3 wi = light.kind == 0u ? -light.direction_radius.xyz : delta / max(distance, 1e-8);
    if (dot(normal, wi) <= 0.02 ||
        (light.kind != 0u && distance > light.position_range.w)) return 1.0;
    float limit = light.kind == 0u ? pc.length : min(pc.length, max(distance - pc.normal_bias, 0.0));
    float jitter = de_shadow_noise(float2(pixel), pc.frame);
    float3 origin = p + normal * pc.normal_bias;
    for (uint i = 0u; i < pc.steps; ++i)
    {
        float t = (float(i) + 0.5 + 0.5 * jitter) / float(pc.steps) * limit;
        float3 q = origin + wi * t;
        float4 h = float4(q, 1.0);
        float4 clip = float4(dot(sc.view_proj[0], h), dot(sc.view_proj[1], h),
                            dot(sc.view_proj[2], h), dot(sc.view_proj[3], h));
        if (!(clip.w > 0.0)) break;
        float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        if (any(uv < 0.0) || any(uv >= 1.0)) break;
        uint2 sample_pixel = min(uint2(uv * float2(pc.width, pc.height)),
                                uint2(pc.width - 1u, pc.height - 1u));
        if (all(sample_pixel == pixel)) continue;
        float z = de_tex2d(pc.depth).Load(int3(sample_pixel, 0)).r;
        if (!(z > 0.0)) continue;
        float2 center = (float2(sample_pixel) + 0.5) / float2(pc.width, pc.height);
        float3 hit = contact_position(sc, center, z);
        float separation = -dot(sc.view[2], h) + dot(sc.view[2], float4(hit, 1.0));
        if (separation > pc.normal_bias && separation < pc.thickness) return 0.0;
    }
    return 1.0;
}
void contact_pack(uint buffer, uint offset, float4 visibility)
{
    de_buffer<uint>(buffer)[offset / 4u] = f32tof16(visibility.x) | (f32tof16(visibility.y) << 16u);
    de_buffer<uint>(buffer)[offset / 4u + 1u] = f32tof16(visibility.z) | (f32tof16(visibility.w) << 16u);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void cs_contact_shadows(uint3 id : SV_DispatchThreadID, uniform ContactParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height) return;
    float directional = 1.0;
    float4 local = float4(1.0);
    float depth = de_tex2d(pc.depth).Load(int3(id.xy, 0)).r;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    if (depth > 0.0)
    {
        float3 p = contact_position(sc, (float2(id.xy) + 0.5) / float2(pc.width, pc.height), depth);
        float3 n = gb_decode(float4(0.0), de_tex2d(pc.normal).Load(int3(id.xy, 0)), float4(0.0)).normal;
        uint4 header = de_bytes(pc.records).load4(0u);
        if (header.w != 0u)
            directional = contact_visibility(pc, sc, id.xy, p, n, de_contact_record(pc.records, 0u));
        // Fixed explicit stores avoid Metal dynamic-index stores in large pixel structures.
        if (header.z > 0u) local.x = contact_visibility(pc, sc, id.xy, p, n, de_contact_record(pc.records, 1u));
        if (header.z > 1u) local.y = contact_visibility(pc, sc, id.xy, p, n, de_contact_record(pc.records, 2u));
        if (header.z > 2u) local.z = contact_visibility(pc, sc, id.xy, p, n, de_contact_record(pc.records, 3u));
        if (header.z > 3u) local.w = contact_visibility(pc, sc, id.xy, p, n, de_contact_record(pc.records, 4u));
    }
    uint offset = id.y * pc.pitch + id.x * 8u;
    contact_pack(pc.packed, offset, float4(directional, 1.0, 1.0, 1.0));
    contact_pack(pc.packed, pc.local_offset + offset, local);
}
