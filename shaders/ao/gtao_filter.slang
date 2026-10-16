module gtao_filter;
import gtao;
import bindless;
import push_constants;
import "../lighting/lighting_common";

struct AoLightingQuery {
    float4 color_ao, view_roughness;
    float4 material; // metallic, GTAO flag, reserved, reserved
};
// Acceptance entry uses the production indirect composition, returning its neutral material
// response separately so CPU tests can isolate AO/min/multi-bounce and SSR confidence algebra.
[shader("compute")][numthreads(8, 8, 1)] void
cs_gtao_lighting_queries(uint3 id : SV_DispatchThreadID, uniform GtaoParams pc) {
    if (id.x >= pc.width || id.y != 0)
        return;
    AoLightingQuery q = de_robuffer<AoLightingQuery>(pc.previous)[id.x];
    SurfaceData s = (SurfaceData)0;
    s.base_color = q.color_ao.rgb;
    s.ao = q.color_ao.a;
    s.normal = float3(0, 0, 1);
    s.roughness = q.view_roughness.w;
    s.metallic = q.material.x;
    s.specular = 0.5;
    float3 v = normalize(q.view_roughness.xyz);
    DeLightingView lv = (DeLightingView)0;
    lv.ibl.ambient = float4(1, 1, 1, 0);
    lv.brdf_lut = pc.normal;
    lv.inputs.ao = pc.depth;
    lv.inputs.indirect_diffuse = pc.motion;
    lv.inputs.indirect_specular = pc.raw;
    lv.inputs.reserved0 = q.material.y > 0 ? DE_INPUT_AO_GTAO : 0u;
    if (q.material.z == 1)
        lv.inputs.reserved1 = pc.scene;
    if (q.material.z == 2) {
        lv.tiles_x = 1;
        lv.tiles_y = 1;
        lv.slices = 1;
        lv.tile_px = pc.width;
        lv.near = 0.1;
        lv.inputs.probe_records = pc.slot;
        lv.inputs.probe_list = pc.previous_surface;
        lv.inputs.probe_sampler = pc.history;
    }
    float3 d, sp;
    evaluate_indirect_split(s, float3(0), v, uint2(id.x, 0), lv, d, sp);
    de_buffer<float4>(pc.packed)[id.x * 4] = float4(d, 1);
    de_buffer<float4>(pc.packed)[id.x * 4 + 1] = float4(sp, 1);
    s.ao = 1;
    lv.inputs = (LightingInputs)0;
    evaluate_indirect_split(s, float3(0), v, uint2(id.x, 0), lv, d, sp);
    de_buffer<float4>(pc.packed)[id.x * 4 + 2] = float4(d, 1);
    de_buffer<float4>(pc.packed)[id.x * 4 + 3] = float4(sp, 1);
}

// A bilateral footprint becomes asymmetric beside a depth/normal edge. Reconstruct its
// first-order AO field at the receiver instead of shifting it toward the surviving neighbours.
float reconstruct_ao(float weight, float sum, float2 position, float3 covariance, float2 cross) {
    if (weight <= 1e-6)
        return 1;
    float2 mean = position / weight;
    float3 c = covariance / weight - float3(mean.x * mean.x, mean.x * mean.y, mean.y * mean.y);
    float2 r = cross / weight - mean * (sum / weight);
    float det = c.x * c.z - c.y * c.y;
    float2 slope = det > 1e-5 ? float2(c.z * r.x - c.y * r.y, c.x * r.y - c.y * r.x) / det
                              : r / max(c.x + c.z, 1e-5);
    return saturate(sum / weight - dot(slope, mean));
}

float surface_weight(GtaoParams pc, DeSceneConstants sc, uint2 pixel, float3 p, float3 normal,
                     uint2 source, float depth) {
    if (!(depth > 0))
        return 0;
    float3 n = gtao_normal(pc, source);
    float nd = dot(normal, n);
    if (nd < 0.85)
        return 0;
    float3 q = ssr_position(sc, (float2(source) + 0.5) / float2(pc.width, pc.height), depth);
    // Plane distance tolerates a tilted continuous surface, unlike raw reverse-Z differences.
    float plane = abs(dot(q - p, normal));
    float tolerance = max(pc.radius * 0.02, length(q - p) * 0.02);
    return pow(saturate(nd), 16.0) * exp(-plane / max(tolerance, 1e-5));
}

[shader("compute")][numthreads(8, 8, 1)] void cs_gtao_spatial(uint3 id : SV_DispatchThreadID,
                                                              uniform GtaoParams pc) {
    if (id.x >= pc.tw || id.y >= pc.th)
        return;
    uint2 pixel = gtao_pixel(pc, id.xy);
    float depth = de_tex2d(pc.depth).Load(int3(pixel, 0)).r;
    float4 center = de_rwtex2d(pc.raw)[id.xy];
    if (!(depth > 0)) {
        de_rwtex2d(pc.spatial)[id.xy] = center;
        return;
    }
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float3 p = ssr_position(sc, (float2(pixel) + 0.5) / float2(pc.width, pc.height), depth);
    float3 normal = gtao_normal(pc, pixel), moment = float3(0, 0, 0);
    float sum = 0, weight = 0;
    float2 position = 0, cross = 0;
    float3 covariance = 0;
    int radius = pc.tw == pc.width && pc.th == pc.height ? 2 : 3;
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x) {
            int2 q = int2(id.xy) + int2(x, y);
            if (any(q < 0) || any(q >= int2(pc.tw, pc.th)))
                continue;
            float4 value = de_rwtex2d(pc.raw)[q];
            uint2 source = gtao_pixel(pc, uint2(q));
            float w = surface_weight(pc, sc, pixel, p, normal, source, value.w) *
                      float(radius + 1 - abs(x)) * float(radius + 1 - abs(y));
            sum += value.x * w;
            float2 offset = float2(x, y);
            position += offset * w;
            cross += offset * (value.x * w);
            covariance += float3(offset.x * offset.x, offset.x * offset.y, offset.y * offset.y) * w;
            moment += gb_oct_decode(value.yz * 2.0 - 1.0) * w;
            weight += w;
        }
    float3 bent = weight > 1e-6 && dot(moment, moment) > 1e-10 ? normalize(moment) : normal;
    de_rwtex2d(pc.spatial)[id.xy] =
        float4(weight > 1e-6 ? reconstruct_ao(weight, sum, position, covariance, cross) : center.x,
               gb_oct_encode(bent) * 0.5 + 0.5, depth);
}

    [shader("compute")][numthreads(8, 8, 1)] void cs_gtao_temporal(uint3 id : SV_DispatchThreadID,
                                                                   uniform GtaoParams pc) {
    if (id.x >= pc.tw || id.y >= pc.th)
        return;
    uint2 pixel = gtao_pixel(pc, id.xy);
    float4 current = de_rwtex2d(pc.spatial)[id.xy];
    float3 normal = gtao_normal(pc, pixel);
    float age = 1;
    if (pc.history_valid != 0 && current.w > 0) {
        DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
        float2 uv = (float2(pixel) + 0.5) / float2(pc.width, pc.height);
        float2 previous_uv = uv + de_tex2d(pc.motion).Load(int3(pixel, 0)).xy * float2(-0.5, 0.5);
        float3 p = ssr_position(sc, uv, current.w);
        float4 point = float4(p, 1);
        float4 clip = float4(dot(sc.prev_view_proj[0], point), dot(sc.prev_view_proj[1], point),
                             dot(sc.prev_view_proj[2], point), dot(sc.prev_view_proj[3], point));
        if (all(previous_uv >= 0) && all(previous_uv < 1) && clip.w > 0) {
            uint scale = pc.tw == pc.width && pc.th == pc.height ? 1u : 2u;
            uint2 q = min(uint2(previous_uv * float2(pc.width, pc.height)) / scale,
                          uint2(pc.tw - 1, pc.th - 1));
            float4 surface = de_rwtex2d(pc.previous_surface)[q];
            float expected = clip.z / clip.w;
            float4 old = de_rwtex2d(pc.previous)[q];
            // Bound separation in metres rather than reverse-Z units: a fixed depth epsilon
            // otherwise admits metre-scale disocclusions far from a perspective camera.
            float separation = sc.proj[3].w > 0.5
                                   ? abs(surface.x - expected) / max(abs(sc.proj[2].z), 1e-8)
                                   : abs(expected / max(surface.x, 1e-12) - 1.0) * abs(clip.w);
            if (surface.x > 0 && separation <= pc.radius * 0.05 &&
                dot(normal, surface.yzw) >= 0.85 && all(isfinite(old)) && old.w >= 1) {
                float lo = current.x, hi = current.x, mean = 0, second = 0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x) {
                        int2 s =
                            clamp(int2(id.xy) + int2(x, y), int2(0), int2(pc.tw - 1, pc.th - 1));
                        float a = de_rwtex2d(pc.spatial)[s].x;
                        lo = min(lo, a);
                        hi = max(hi, a);
                        mean += a;
                        second += a * a;
                    }
                age = min(old.w + 1.0, 32.0);
                float alpha = max(1.0 / age, 1.0 - pc.history_weight);
                float sigma = sqrt(max(second / 9.0 - (mean / 9.0) * (mean / 9.0), 0));
                float margin = max(0.02, 3.0 * sigma);
                current.x = lerp(clamp(old.x, lo - margin, hi + margin), current.x, alpha);
                float3 b =
                    lerp(gb_oct_decode(old.yz * 2 - 1), gb_oct_decode(current.yz * 2 - 1), alpha);
                current.yz = gb_oct_encode(dot(b, b) > 1e-10 ? normalize(b) : normal) * 0.5 + 0.5;
            }
        }
    }
    de_rwtex2d(pc.history)[id.xy] = float4(current.xyz, age);
    de_rwtex2d(pc.history_surface)[id.xy] = float4(current.w, normal);
}

float3 gtao_upsample(GtaoParams pc, uint2 pixel) {
    float depth = de_tex2d(pc.depth).Load(int3(pixel, 0)).r;
    if (!(depth > 0))
        return float3(1, 0.5, 0.5);
    if (pc.tw == pc.width && pc.th == pc.height)
        return de_rwtex2d(pc.history)[pixel].xyz;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float3 p = ssr_position(sc, (float2(pixel) + 0.5) / float2(pc.width, pc.height), depth);
    float3 normal = gtao_normal(pc, pixel), moment = float3(0, 0, 0);
    float sum = 0, weight = 0;
    int2 base = int2(pixel / 2);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            int2 q = base + int2(x, y);
            if (any(q < 0) || any(q >= int2(pc.tw, pc.th)))
                continue;
            uint2 source = gtao_pixel(pc, uint2(q));
            float4 surface = de_rwtex2d(pc.history_surface)[q];
            float w = surface_weight(pc, sc, pixel, p, normal, source, surface.x);
            float2 delta = (float2(source) - float2(pixel)) * 0.5;
            float2 kernel = saturate(1.0 - abs(delta));
            w *= kernel.x * kernel.y;
            float3 value = de_rwtex2d(pc.history)[q].xyz;
            sum += value.x * w;
            moment += gb_oct_decode(value.yz * 2 - 1) * w;
            weight += w;
        }
    float3 bent = weight > 1e-6 && dot(moment, moment) > 1e-10 ? normalize(moment) : normal;
    return float3(weight > 1e-6 ? sum / weight : 1, gb_oct_encode(bent) * 0.5 + 0.5);
}

// Copy-only narrow-format publication: storage writes stay in GENERAL buffers/textures.
[shader("compute")][numthreads(8, 8, 1)] void cs_gtao_pack(uint3 id : SV_DispatchThreadID,
                                                           uniform GtaoParams pc) {
    if (id.x >= pc.pitch_ao / 4 || id.y >= pc.height)
        return;
    uint ao = 0, b0 = 0, b1 = 0;
    for (uint lane = 0; lane < 4; ++lane) {
        uint x = id.x * 4 + lane;
        float3 value = x < pc.width ? gtao_upsample(pc, uint2(x, id.y)) : float3(1, 0.5, 0.5);
        uint a = uint(round(saturate(value.x) * 255));
        uint2 b = uint2(round(saturate(value.yz) * 255));
        ao |= a << (lane * 8);
        uint bits = b.x | (b.y << 8);
        if (lane < 2)
            b0 |= bits << (lane * 16);
        else
            b1 |= bits << ((lane - 2) * 16);
    }
    de_buffer<uint>(pc.packed)[(id.y * pc.pitch_ao) / 4 + id.x] = ao;
    if (pc.bent != 0 && id.x * 8 < pc.pitch_bent) {
        uint offset = (pc.bent_offset + id.y * pc.pitch_bent) / 4 + id.x * 2;
        de_buffer<uint>(pc.packed)[offset] = b0;
        de_buffer<uint>(pc.packed)[offset + 1] = b1;
    }
}
