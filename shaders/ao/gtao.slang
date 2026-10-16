// View-axis horizon integration, including projected-normal cosine weighting (Jimenez et al.).
// Raw visibility is independent of albedo/material AO; lighting owns those once.
module gtao;
__exported import "../ssr/ssr_common";
import push_constants;
public
struct GtaoParams {
  public
    uint scene, slot, depth, normal;
  public
    uint motion, width, height, tw;
  public
    uint th, slices, steps, frame;
  public
    uint raw, spatial, previous, previous_surface;
  public
    uint history, history_surface, packed, pitch_ao;
  public
    uint pitch_bent, bent_offset, history_valid, bent;
  public
    float radius, falloff, thickness, history_weight;
};
public
uint2 gtao_pixel(GtaoParams pc, uint2 p) {
    uint scale = pc.tw == pc.width && pc.th == pc.height ? 1u : 2u;
    uint2 first = min(p * scale, uint2(pc.width - 1u, pc.height - 1u));
    uint2 selected = first;
    float closest = -1.0;
    for (uint y = 0; y < scale; ++y)
        for (uint x = 0; x < scale; ++x) {
            uint2 q = min(first + uint2(x, y), uint2(pc.width - 1u, pc.height - 1u));
            float d = de_tex2d(pc.depth).Load(int3(q, 0)).r;
            if (d > closest) {
                closest = d;
                selected = q;
            }
        }
    return selected;
}
public
float3 gtao_normal(GtaoParams pc, uint2 p) {
    return gb_decode(float4(0, 0, 0, 0), de_tex2d(pc.normal).Load(int3(p, 0)), float4(0, 0, 0, 0))
        .normal;
}
float gtao_integral(float h, float n) {
    return 0.25 * (cos(n) - cos(2.0 * h - n) + 2.0 * h * sin(n));
}
float2 gtao_moment(float h, float n) {
    float s = sin(h), c = cos(h), sn = sin(n), cn = cos(n);
    return float2(cn * (1.0 - c * c * c) + sn * s * s * s,
                  cn * s * s * s + sn * (2.0 - 3.0 * c + c * c * c)) /
           3.0;
}
float gtao_horizon(GtaoParams pc, DeSceneConstants sc, float3 p, float3 v, float3 normal, float2 uv,
                   float2 direction, float radius_px, float jitter) {
    float horizon = -1.0, previous_cosine = -1.0;
    for (uint step = 0; step < pc.steps; ++step) {
        float t = (float(step) + 0.5 + 0.5 * jitter) / float(pc.steps);
        float distance_px = max(1.0, radius_px * t * t);
        int2 q = int2(floor(uv * float2(pc.width, pc.height) + direction * distance_px));
        if (any(q < 0) || any(q >= int2(pc.width, pc.height)))
            continue;
        float depth = de_tex2d(pc.depth).Load(int3(q, 0)).r;
        if (!(depth > 0.0)) {
            horizon = lerp(horizon, -1.0, pc.thickness);
            previous_cosine = -1.0;
            continue;
        }
        float3 delta = ssr_position(sc, (float2(q) + 0.5) / float2(pc.width, pc.height), depth) - p;
        float distance = length(delta);
        if (!(distance > 1e-5) || distance > pc.radius)
            continue;
        // GB1's quantized normals and integer pixel centers must not turn coplanar
        // samples into occluders. A 0.002 cosine bias bounds this to ~0.115 degrees.
        if (dot(delta, normal) <= distance * 0.002)
            continue;
        float cosine = clamp(dot(delta, v) / distance, -1.0, 1.0);
        float weight = saturate((pc.radius - distance) / max(pc.radius * (1.0 - pc.falloff), 1e-5));
        float candidate = lerp(-1.0, cosine, weight);
        // Decreasing unattenuated horizon after a foreground silhouette suggests thin geometry.
        // Continuous corner walls increase it, so falloff alone cannot erase their near horizon.
        horizon = cosine < previous_cosine - 0.001 ? lerp(horizon, candidate, pc.thickness)
                                                   : max(horizon, candidate);
        previous_cosine = cosine;
    }
    return horizon;
}
[shader("compute")][numthreads(8, 8, 1)] void cs_gtao(uint3 tid : SV_DispatchThreadID,
                                                      uniform GtaoParams pc) {
    if (tid.x >= pc.tw || tid.y >= pc.th)
        return;
    uint2 pixel = gtao_pixel(pc, tid.xy);
    float depth = de_tex2d(pc.depth).Load(int3(pixel, 0)).r;
    if (!(depth > 0.0)) {
        de_rwtex2d(pc.raw)[tid.xy] = float4(1, 0.5, 0.5, 0);
        return;
    }
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float2 uv = (float2(pixel) + 0.5) / float2(pc.width, pc.height);
    float3 p = ssr_position(sc, uv, depth), v = ssr_view(sc, p), normal = gtao_normal(pc, pixel);
    uint hash = pixel.x * 1664525u + pixel.y * 1013904223u;
    float rotation = frac(float(hash & 65535u) / 65536.0 + float(pc.frame & 7u) * 0.61803398875);
    float jitter = frac(rotation * 7.0 + 0.37);
    float sum = 0.0, unoccluded = 0.0;
    float3 moment = float3(0, 0, 0);
    for (uint slice = 0; slice < pc.slices; ++slice) {
        float phi = (float(slice) + rotation) * DE_PI / float(pc.slices);
        float2 direction = float2(cos(phi), sin(phi));
        float3 step_world =
            ssr_position(sc, uv + direction / float2(pc.width, pc.height), depth) - p;
        float3 tangent = normalize(step_world - v * dot(step_world, v));
        float3 plane_normal = normalize(cross(v, tangent));
        float3 projected = normal - plane_normal * dot(normal, plane_normal);
        float projected_length = length(projected);
        float angle = atan2(dot(projected, tangent), dot(projected, v));
        float lower = angle - 0.5 * DE_PI, upper = angle + 0.5 * DE_PI;
        float radius_px =
            min(pc.radius / max(length(step_world), 1e-6), float(max(pc.width, pc.height)));
        float left = -acos(clamp(
            gtao_horizon(pc, sc, p, v, normal, uv, -direction, radius_px, jitter), -1.0, 1.0));
        float right = acos(
            clamp(gtao_horizon(pc, sc, p, v, normal, uv, direction, radius_px, jitter), -1.0, 1.0));
        left = clamp(left, lower, upper);
        right = clamp(right, lower, upper);
        sum += projected_length * (gtao_integral(left, angle) + gtao_integral(right, angle));
        unoccluded +=
            projected_length * (gtao_integral(lower, angle) + gtao_integral(upper, angle));
        if (pc.bent != 0u) {
            float2 m = projected_length * (gtao_moment(left, angle) + gtao_moment(right, angle));
            moment += v * m.x + tangent * m.y;
        }
    }
    float ao = saturate(sum / max(unoccluded, 1e-5));
    float3 bent = pc.bent != 0u && dot(moment, moment) > 1e-10 ? normalize(moment) : normal;
    de_rwtex2d(pc.raw)[tid.xy] = float4(ao, gb_oct_encode(bent) * 0.5 + 0.5, depth);
}
