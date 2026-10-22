module ssr_common;
__exported import bindless;
__exported import "../scene/scene_types";
__exported import "../visbuffer/gbuffer";
__exported import "../materials/material_interface";
__exported import "../brdf/ggx";
__exported import "../brdf/sampling";
__exported import "../lighting/ibl";
public struct SsrParams {
  public uint scene, slot, depth, gb0, gb1, gb2, motion, hzb;
  public uint hits, rays, history_color, history_surface, width, height, tw, th;
  public uint hzb_width, hzb_height, mips, frame, history_valid, previous_specular, previous_moments,
        output;
  public uint write_specular, write_moments, lut, max_steps;
  public float near_z, thickness, max_roughness, previous_exposure;
};
public float3 ssr_position(DeSceneConstants sc, float2 uv, float depth) {
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, depth, 1);
    float4 w = float4(dot(sc.inv_view_proj[0], p), dot(sc.inv_view_proj[1], p),
                      dot(sc.inv_view_proj[2], p), dot(sc.inv_view_proj[3], p));
    return w.xyz / w.w;
}
public float3 ssr_view(DeSceneConstants sc, float3 p) {
    return sc.proj[3].w > 0.5 ? normalize(sc.view[2].xyz) : normalize(sc.camera_position.xyz - p);
}
public SurfaceData ssr_surface(SsrParams pc, uint2 pixel) {
    int3 p = int3(pixel, 0);
    return de_surface_from_gb(
        gb_decode(de_tex2d(pc.gb0).Load(p), de_tex2d(pc.gb1).Load(p), de_tex2d(pc.gb2).Load(p)));
}
public float2 ssr_previous_uv(float2 uv, float2 motion) {
    return uv + motion * float2(-0.5, 0.5); // motion is NDC current-minus-previous, not UV
}
public float4 ssr_history_load(uint index, float2 uv, uint2 size) {
    float2 p = uv * float2(size) - 0.5;
    int2 a = int2(floor(p));
    float2 f = frac(p);
    int2 hi = int2(size) - 1;
    float4 c00 = de_rwtex2d(index)[clamp(a, int2(0, 0), hi)];
    float4 c10 = de_rwtex2d(index)[clamp(a + int2(1, 0), int2(0, 0), hi)];
    float4 c01 = de_rwtex2d(index)[clamp(a + int2(0, 1), int2(0, 0), hi)];
    float4 c11 = de_rwtex2d(index)[clamp(a + int2(1, 1), int2(0, 0), hi)];
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}
public bool ssr_history_valid(SsrParams pc, DeSceneConstants sc, float2 uv, float3 position,
                       float3 normal) {
    if ((pc.history_valid & 1u) == 0 || any(uv < 0) || any(uv >= 1))
        return false;
    uint2 p = min(uint2(uv * float2(pc.width, pc.height)), uint2(pc.width - 1, pc.height - 1));
    float4 surface = de_rwtex2d(pc.history_surface)[p];
    float4 clip = float4(dot(sc.prev_view_proj[0], float4(position, 1)),
                         dot(sc.prev_view_proj[1], float4(position, 1)),
                         dot(sc.prev_view_proj[2], float4(position, 1)),
                         dot(sc.prev_view_proj[3], float4(position, 1)));
    if (!(clip.w > 0) || !(surface.x > 0) || dot(surface.yzw, normal) < 0.85)
        return false;
    float expected = clip.z / clip.w;
    return abs(surface.x - expected) <= max(0.002 * expected, 2e-4);
}
// Motion must validate each bilinear contributor, not accept four surfaces after one test.
// Keep the established stationary footprint (and convergence); always reject nonfinite colour.
public float4 ssr_history_sample(SsrParams pc, DeSceneConstants sc, uint index, float2 uv,
                                 float3 position, float3 normal, bool footprint_coverage = false) {
    if (!ssr_history_valid(pc, sc, uv, position, normal)) return float4(0);
    float4 point = float4(position, 1);
    float4 current = float4(dot(sc.view_proj[0], point), dot(sc.view_proj[1], point),
                            dot(sc.view_proj[2], point), dot(sc.view_proj[3], point));
    float2 current_uv = current.xy / current.w * float2(.5, -.5) + .5;
    bool moving = current.w <= 0 || length((current_uv - uv) * float2(pc.width, pc.height)) > .01;
    float4 previous = float4(dot(sc.prev_view_proj[0], point), dot(sc.prev_view_proj[1], point),
                             dot(sc.prev_view_proj[2], point), dot(sc.prev_view_proj[3], point));
    float expected = previous.z / previous.w;
    float2 p = uv * float2(pc.width, pc.height) - 0.5;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    float4 sum = float4(0);
    float weight = 0;
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x) {
            int2 q = base + int2(x, y);
            if (any(q < 0) || any(q >= int2(pc.width, pc.height)))
                continue;
            float2 center = (float2(q) + 0.5) / float2(pc.width, pc.height);
            float w = (x == 0 ? 1-f.x : f.x) * (y == 0 ? 1-f.y : f.y);
            if (w > 0 && (!moving || ssr_history_valid(pc, sc, center, position, normal))) {
                if (moving) {
                    float depth = de_rwtex2d(pc.history_surface)[q].x;
                    // A fixed reverse-Z epsilon accepts metre-scale separation at a distance.
                    // Use the ray trace's world-space thickness for moving history footprints.
                    float separation = sc.proj[3].w > .5
                        ? abs(depth - expected) / max(abs(sc.proj[2].z), 1e-8)
                        : abs(expected / max(depth, 1e-12) - 1) * abs(previous.w);
                    if (separation > max(pc.thickness, .01)) continue;
                }
                float4 value = de_rwtex2d(index)[q];
                if (all(isfinite(value))) { sum += value * w; weight += w; }
            }
        }
    if (weight <= 1e-6) return float4(0);
    float4 result = sum / weight;
    if (footprint_coverage) result.a = weight; // Coverage is independent of colour-buffer alpha.
    return result;
}
public float3 ssr_response(SurfaceData surface, float3 v, uint lut) {
    DeIbl ibl = (DeIbl)0;
    ibl.ambient = float4(1, 1, 1, 0);
    float3 d, s;
    de_ibl_evaluate_base(surface, v, ibl, lut, surface.ao, d, s);
    return s; // Base-only split-sum response; the producer reconstructs only the G-buffer.
}
