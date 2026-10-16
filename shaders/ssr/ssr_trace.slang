import bindless;
import push_constants;
import ssr_common;
import hiz_trace;
[shader("compute")][numthreads(8, 8, 1)] void cs_ssr_trace(uint3 id : SV_DispatchThreadID,
                                                           uniform SsrParams pc) {
    if (id.x >= pc.tw || id.y >= pc.th)
        return;
    de_rwtex2d(pc.hits)[id.xy] = float4(0, 0, 0, 0);
    de_rwtex2d(pc.rays)[id.xy] = float4(0, 0, 0, 0);
    if ((pc.history_valid & 1u) == 0)
        return;
    uint2 pixel =
        min(uint2((float2(id.xy) + 0.5) * float2(pc.width, pc.height) / float2(pc.tw, pc.th)),
            uint2(pc.width - 1, pc.height - 1));
    float2 uv = (float2(pixel) + 0.5) / float2(pc.width, pc.height);
    float depth = de_tex2d(pc.depth).Load(int3(pixel, 0)).r;
    SurfaceData surface = ssr_surface(pc, pixel);
    if (!(depth > 0) || surface.roughness >= pc.max_roughness ||
        (surface.features & DE_MATERIAL_FEATURE_UNLIT) != 0)
        return;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float3 position = ssr_position(sc, uv, depth), v = ssr_view(sc, position);
    ShadingFrame f = de_surface_frame(surface);
    float3 wo = f.to_local(v);
    if (!(wo.z > 0))
        return;
    float alpha = max(surface.roughness * surface.roughness, 0.001);
    float2 random = frac(de_r2(pc.frame) +
                         float2(de_u32_to_unit(de_pcg_hash(pixel.x + pixel.y * pc.width)),
                                de_u32_to_unit(de_pcg_hash(pixel.y + pixel.x * pc.height + 17))));
    float3 half_vector =
        surface.roughness < 0.05 ? float3(0, 0, 1) : de_ggx_vndf_sample(alpha, alpha, wo, random);
    float3 wi = de_reflect(wo, half_vector);
    float pdf = surface.roughness < 0.05 ? 1 : max(de_ggx_reflect_pdf(alpha, alpha, wo, wi), 1e-6);
    if (!(wi.z > 0) || !all(isfinite(wi)) || !isfinite(pdf))
        return;
    float3 direction = f.to_world(wi);
    de_rwtex2d(pc.rays)[id.xy] = float4(direction, pdf);
    float3 start = position + surface.normal * max(0.002, pc.thickness * 0.05);
    float distance = 100;
    float vz = dot(sc.view[2], float4(start, 1));
    float dz = dot(sc.view[2].xyz, direction);
    if (dz > 0)
        distance = min(distance, max((-pc.near_z * 1.01 - vz) / dz, 0));
    float4 a =
        float4(dot(sc.view_proj[0], float4(start, 1)), dot(sc.view_proj[1], float4(start, 1)),
               dot(sc.view_proj[2], float4(start, 1)), dot(sc.view_proj[3], float4(start, 1)));
    float4 b = float4(dot(sc.view_proj[0], float4(start + direction * distance, 1)),
                      dot(sc.view_proj[1], float4(start + direction * distance, 1)),
                      dot(sc.view_proj[2], float4(start + direction * distance, 1)),
                      dot(sc.view_proj[3], float4(start + direction * distance, 1)));
    if (!(a.w > 0) || !(b.w > 0))
        return;
    float3 origin = float3(a.x / a.w * 0.5 + 0.5, 0.5 - a.y / a.w * 0.5, a.z / a.w);
    float3 end = float3(b.x / b.w * 0.5 + 0.5, 0.5 - b.y / b.w * 0.5, b.z / b.w);
    DeHiZHit hit =
        de_hiz_trace(pc.hzb, pc.depth, uint2(pc.hzb_width, pc.hzb_height), pc.mips,
                     uint2(pc.width, pc.height), origin, end - origin, pc.near_z, pc.thickness,
                     sc.proj[3].w > 0.5 ? abs(sc.proj[2].z) : 0, pc.max_steps);
    if (hit.confidence > 0) {
        uint2 hp =
            min(uint2(hit.uv * float2(pc.width, pc.height)), uint2(pc.width - 1, pc.height - 1));
        SurfaceData hs = ssr_surface(pc, hp);
        float edge = min(min(hit.uv.x, 1 - hit.uv.x), min(hit.uv.y, 1 - hit.uv.y));
        float rough = saturate((pc.max_roughness - surface.roughness) / 0.1);
        hit.confidence *= saturate(edge * 20) * rough * saturate(dot(hs.normal, -direction) * 4);
    }
    de_rwtex2d(pc.hits)[id.xy] = float4(hit.uv, hit.depth, hit.confidence);
} struct HiZTestRay {
    float4 origin;
    float4 delta;
};
struct HiZTestParams {
    uint hzb, depth, rays, result, width, height, hzb_width, hzb_height, mips, count, max_steps,
        reserved;
    float near_z, thickness, depth_scale, padding;
};
[shader("compute")][numthreads(64, 1, 1)] void cs_hiz_test(uint3 id : SV_DispatchThreadID,
                                                           uniform HiZTestParams pc) {
    if (id.x >= pc.count)
        return;
    HiZTestRay ray = de_robuffer<HiZTestRay>(pc.rays)[id.x];
    DeHiZHit h = de_hiz_trace(pc.hzb, pc.depth, uint2(pc.hzb_width, pc.hzb_height), pc.mips,
                              uint2(pc.width, pc.height), ray.origin.xyz, ray.delta.xyz, pc.near_z,
                              pc.thickness, pc.depth_scale, pc.max_steps);
    de_buffer<float4>(pc.result)[id.x] = float4(h.uv, h.depth, h.confidence);
}
