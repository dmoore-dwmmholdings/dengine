import bindless;
import push_constants;
import "../visbuffer/gbuffer";
import ssr_common;

struct HistoryParams {
    uint color, depth, normal, dst_color, dst_surface, width, height, reserved;
};
[shader("compute")][numthreads(8, 8, 1)] void cs_scene_history(uint3 id : SV_DispatchThreadID,
                                                               uniform HistoryParams pc) {
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    int3 p = int3(id.xy, 0);
    float4 color = de_tex2d(pc.color).Load(p);
    float z = de_tex2d(pc.depth).Load(p).r;
    float3 normal =
        gb_decode(float4(0, 0, 0, 0), de_tex2d(pc.normal).Load(p), float4(0, 0, 0, 0)).normal;
    de_rwtex2d(pc.dst_color)[id.xy] = all(isfinite(color)) ? color : float4(0, 0, 0, 0);
    de_rwtex2d(pc.dst_surface)[id.xy] = float4(z, normal);
}

    // Stachowiak's ratio estimator: reuse directions with target BRDF*cos / source PDF weights;
    // normalize incident radiance, then apply the same integrated material response as probe/sky.
    // Missing rays contribute only to confidence, so fallback never acquires a black estimate.
    [shader("compute")][numthreads(8, 8, 1)] void cs_ssr_resolve(uint3 id : SV_DispatchThreadID,
                                                                 uniform SsrParams pc) {
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    uint2 pixel = id.xy;
    float2 uv = (float2(pixel) + 0.5) / float2(pc.width, pc.height);
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float z = de_tex2d(pc.depth).Load(int3(pixel, 0)).r;
    SurfaceData surface = ssr_surface(pc, pixel);
    float4 result = float4(0, 0, 0, 0), moments = float4(0, 0, 0, 0);
    if (z > 0 && surface.roughness < pc.max_roughness &&
        (surface.features & DE_MATERIAL_FEATURE_UNLIT) == 0) {
        float3 position = ssr_position(sc, uv, z), v = ssr_view(sc, position);
        ShadingFrame frame = de_surface_frame(surface);
        float3 wo = frame.to_local(v);
        float alpha = max(surface.roughness * surface.roughness, 0.001);
        int2 base = int2(uv * float2(pc.tw, pc.th));
        float3 sum = float3(0, 0, 0);
        float weight = 0, total = 0, coverage = 0, lum = 0, lum2 = 0;
        int radius = surface.roughness < 0.05 ? 0 : 1;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x) {
                int2 ray_pixel = base + int2(x, y);
                if (any(ray_pixel < 0) || any(ray_pixel >= int2(pc.tw, pc.th)))
                    continue;
                uint2 source = min(uint2((float2(ray_pixel) + 0.5) * float2(pc.width, pc.height) /
                                         float2(pc.tw, pc.th)),
                                   uint2(pc.width - 1, pc.height - 1));
                SurfaceData source_surface = ssr_surface(pc, source);
                float source_z = de_tex2d(pc.depth).Load(int3(source, 0)).r;
                if (dot(surface.normal, source_surface.normal) < 0.85 ||
                    abs(source_z - z) > max(z * 0.05, 1e-4))
                    continue;
                float4 hit = de_tex2d(pc.hits).Load(int3(ray_pixel, 0));
                float4 ray = de_tex2d(pc.rays).Load(int3(ray_pixel, 0));
                if (!(ray.w > 0) || !all(isfinite(ray)))
                    continue;
                float3 hit_position =
                    hit.z > 0 ? ssr_position(sc, hit.xy, hit.z) : position + ray.xyz;
                float3 direction = hit.z > 0 ? normalize(hit_position - position) : ray.xyz;
                float3 wi = frame.to_local(direction);
                if (!(wi.z > 0) || !(wo.z > 0))
                    continue;
                float w =
                    surface.roughness < 0.05
                        ? 1
                        : min(de_ggx_specular(alpha, alpha, wo, wi) * wi.z / max(ray.w, 1e-6), 1e5);
                if (!(w > 0) || !isfinite(w))
                    continue;
                total += w;
                if (!(hit.w > 0))
                    continue;
                uint2 hp = min(uint2(hit.xy * float2(pc.width, pc.height)),
                               uint2(pc.width - 1, pc.height - 1));
                SurfaceData hs = ssr_surface(pc, hp);
                float2 prev = ssr_previous_uv(hit.xy, de_tex2d(pc.motion).Load(int3(hp, 0)).xy);
                if (!ssr_history_valid(pc, sc, prev, hit_position, hs.normal))
                    continue;
                // History is pre-exposed; correct into this frame, then remove its exposure once.
                float exposure = max(sc.pre_exposure, 1e-8);
                float ratio = exposure / max(pc.previous_exposure, 1e-8);
                float3 color =
                    ssr_history_load(pc.history_color, prev, uint2(pc.width, pc.height)).rgb *
                    ratio / exposure;
                if (!all(isfinite(color)))
                    continue;
                color = max(color, float3(0, 0, 0));
                sum += color * w;
                weight += w;
                coverage += w * hit.w;
                float l = dot(color, float3(0.2126, 0.7152, 0.0722));
                lum += l * w;
                lum2 += l * l * w;
            }
        if (weight > 1e-8 && total > 1e-8) {
            float3 response = ssr_response(surface, v, pc.lut);
            result = float4(sum / weight * response, saturate(coverage / total));
            float l = dot(result.rgb, float3(0.2126, 0.7152, 0.0722));
            moments = float4(l, l * l, 1, 0);
            float2 prev = ssr_previous_uv(uv, de_tex2d(pc.motion).Load(int3(pixel, 0)).xy);
            if ((pc.history_valid & 2u) != 0 &&
                ssr_history_valid(pc, sc, prev, position, surface.normal)) {
                float4 old =
                    ssr_history_load(pc.previous_specular, prev, uint2(pc.width, pc.height));
                float4 stats =
                    ssr_history_load(pc.previous_moments, prev, uint2(pc.width, pc.height));
                if (old.a > 0 && all(isfinite(old)) && all(isfinite(stats))) {
                    float variance = max(lum2 / weight - (lum / weight) * (lum / weight), 0);
                    float sigma = max(sqrt(variance) * max(max(response.r, response.g), response.b),
                                      max(l * 0.1, 0.005));
                    float3 clipped = clamp(old.rgb, max(result.rgb - 3 * sigma, float3(0, 0, 0)),
                                           result.rgb + 3 * sigma);
                    float count = min(stats.z, 31), a = 1 / (count + 1);
                    result.rgb = lerp(clipped, result.rgb, a);
                    moments = float4(lerp(stats.xy, float2(l, l * l), a), count + 1, 0);
                }
            }
        }
    }
    if (!all(isfinite(result)) || !all(isfinite(moments))) {
        result = float4(0, 0, 0, 0);
        moments = float4(0, 0, 0, 0);
    }
    result.rgb = clamp(result.rgb, float3(0, 0, 0), float3(65000, 65000, 65000));
    de_rwtex2d(pc.output)[pixel] = result;
    de_rwtex2d(pc.write_specular)[pixel] = result;
    de_rwtex2d(pc.write_moments)[pixel] = moments;
}
