import bindless;
import push_constants;
import "../visbuffer/gbuffer";
import ssr_common;

struct HistoryParams {
    uint color, depth, normal, dst_color, dst_surface, width, height, reserved;
};
// Shared by production resolve and the native subpixel-hit oracle.
[ForceInline]
float4 ssr_hit_history(SsrParams pc, DeSceneConstants sc, float2 uv, float depth,
                       float2 motion, float3 normal) {
    // Hi-Z returns a leaf's point-sampled depth. Its continuous ray UV is not the
    // point where that depth was measured. Anchor the validation plane at its texel centre,
    // while keeping the continuous UV for bilinear radiance sampling.
    uint2 size = uint2(pc.width, pc.height);
    uint2 pixel = min(uint2(uv * float2(size)), size - 1);
    float2 source_uv = (float2(pixel) + .5) / float2(size);
    float3 position = ssr_position(sc, source_uv, depth);
    return ssr_history_sample(pc, sc, pc.history_color, ssr_previous_uv(uv, motion),
                              position, normal, true);
}
// Lighting blends SSR with fallback using confidence. Accumulate its contribution,
// not two independent averages: correlated colour/coverage must preserve energy.
float4 ssr_temporal_accumulate(float4 previous, float4 current, float3 clipped, float alpha,
                               bool stationary) {
    // Do not lag confidence behind camera/object motion or extend reflection-edge trails.
    if (!stationary) return float4(lerp(clipped, current.rgb, alpha), current.a);
    float confidence = lerp(previous.a, current.a, alpha);
    float3 contribution = lerp(clipped * previous.a, current.rgb * current.a, alpha);
    return float4(contribution / max(confidence, 1e-8), confidence);
}
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
                // History is pre-exposed; correct into this frame, then remove its exposure once.
                float exposure = max(sc.pre_exposure, 1e-8);
                float ratio = exposure / max(pc.previous_exposure, 1e-8);
                float4 history = ssr_hit_history(pc, sc, hit.xy, hit.z,
                                                de_tex2d(pc.motion).Load(int3(hp, 0)).xy, hs.normal);
                if (!(history.a > 0)) continue; // No valid taps: retain the probe/sky fallback.
                float3 color = history.rgb * ratio / exposure;
                if (!all(isfinite(color)))
                    continue;
                color = max(color, float3(0, 0, 0));
                sum += color * w;
                weight += w;
                coverage += w * hit.w * history.a;
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
            if ((pc.history_valid & 2u) != 0) {
                float4 old =
                    ssr_history_sample(pc, sc, pc.previous_specular, prev, position, surface.normal);
                float4 stats =
                    ssr_history_sample(pc, sc, pc.previous_moments, prev, position, surface.normal);
                if (old.a > 0 && all(isfinite(old)) && all(isfinite(stats))) {
                    float variance = max(lum2 / weight - (lum / weight) * (lum / weight), 0);
                    float sigma = max(sqrt(variance) * max(max(response.r, response.g), response.b),
                                      max(l * 0.1, 0.005));
                    float3 clipped = clamp(old.rgb, max(result.rgb - 3 * sigma, float3(0, 0, 0)),
                                           result.rgb + 3 * sigma);
                    float count = min(stats.z, 31), a = 1 / (count + 1);
                    bool stationary = length((prev - uv) * float2(pc.width, pc.height)) <= .01;
                    result = ssr_temporal_accumulate(old, result, clipped, a, stationary);
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

// Native regression oracle: uses the same history sampling as reflection resolve.
struct HistoryQueryParams { uint color, surface, output, valid; };
[shader("compute")][numthreads(8,8,1)]
void cs_ssr_history_queries(uint3 id : SV_DispatchThreadID, uniform HistoryQueryParams q) {
    if (any(id.xy >= 4)) return;
    if ((q.valid & 4u) != 0) {
        float4 previous = de_rwtex2d(q.surface)[id.xy];
        float4 current = de_rwtex2d(q.color)[id.xy];
        de_rwtex2d(q.output)[id.xy] =
            ssr_temporal_accumulate(previous, current, previous.rgb, .5, id.x != 1);
        return;
    }
    SsrParams pc = (SsrParams)0;
    pc.width = pc.height = 4;
    pc.history_surface = q.surface;
    pc.history_color = q.color;
    pc.history_valid = q.valid;
    DeSceneConstants sc = (DeSceneConstants)0;
    sc.prev_view_proj[0] = float4(1,0,0,0);
    sc.prev_view_proj[1] = float4(0,1,0,0);
    sc.prev_view_proj[2] = float4(0,0,1,0);
    sc.prev_view_proj[3] = float4(0,0,0,1);
    sc.view_proj[0] = float4(1,0,0,.2);
    sc.view_proj[1] = float4(0,1,0,0);
    sc.view_proj[2] = float4(0,0,1,0);
    sc.view_proj[3] = float4(0,0,0,1);
    float3 position = float3(0,0,.5);
    if ((q.valid & 2u) != 0) {
        position = float3(0,0,-100);
        sc.prev_view_proj[2] = float4(0,0,0,.1);
        sc.prev_view_proj[3] = float4(0,0,-1,0);
        sc.view_proj[0] = float4(1,0,0,20);
        sc.view_proj[2] = sc.prev_view_proj[2];
        sc.view_proj[3] = sc.prev_view_proj[3];
    }
    if ((q.valid & 16u) != 0)
        sc.prev_view_proj[2].x = (q.valid & 2u) != 0 ? .0002 : .2;
    if ((q.valid & 8u) != 0)
        for (uint row = 0; row < 4; ++row)
            sc.view_proj[row] = sc.prev_view_proj[row];
    if ((q.valid & 32u) != 0) {
        sc.proj[3].w = 1;
        sc.proj[2].z = 1;
    }
    pc.thickness = .1;
    if ((q.valid & 64u) != 0) {
        sc.inv_view_proj[0] = float4(1,0,0,0);
        sc.inv_view_proj[1] = float4(0,1,0,0);
        if ((q.valid & 2u) != 0) {
            sc.inv_view_proj[2] = float4(0,0,0,-1);
            sc.inv_view_proj[3] = float4(-.002,0,10,0);
        } else {
            sc.inv_view_proj[2] = float4(-.2,0,1,0);
            sc.inv_view_proj[3] = float4(0,0,0,1);
        }
        // Depth belongs to texel (2,2), but the ray enters elsewhere within that texel.
        float depth = (q.valid & 2u) != 0 ? .00105 : .55;
        de_rwtex2d(q.output)[id.xy] =
            ssr_hit_history(pc, sc, float2(.6,.5), depth, float2(0), float3(0,0,1));
        return;
    }
    float2 uv = float2(.5,.5);
    // A normal map changes shading, not the flat geometry represented by the depth samples.
    float3 normal = (q.valid & 128u) != 0 ? normalize(float3(.25,0,1)) : float3(0,0,1);
    de_rwtex2d(q.output)[id.xy] = ssr_history_sample(pc,sc,q.color,uv,position,normal,true);
}
