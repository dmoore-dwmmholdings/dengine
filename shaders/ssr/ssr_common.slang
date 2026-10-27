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
public SurfaceData ssr_surface(SsrParams pc, uint2 pixel, out bool geometric_normal) {
    int3 p = int3(pixel, 0);
    DeGbSurface g = gb_decode(de_tex2d(pc.gb0).Load(p), de_tex2d(pc.gb1).Load(p),
                              de_tex2d(pc.gb2).Load(p));
    geometric_normal = (g.flags & DE_GB_FLAG_GEOMETRIC_NORMAL) != 0u;
    return de_surface_from_gb(g);
}
public SurfaceData ssr_surface(SsrParams pc, uint2 pixel) {
    bool unused;
    return ssr_surface(pc, pixel, unused);
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
// One depth-derived tangent. Prefer the nearer depth sample on either side so silhouettes
// do not normally borrow their opposite surface. A local isolated depth extremum with two
// distant neighbours does not establish a receiver plane and must fall back.
bool ssr_depth_axis(SsrParams pc, DeSceneConstants sc, uint2 pixel, float depth,
                    float3 position, int2 step, float3 shading_normal, out float3 axis) {
    axis = float3(0);
    int2 lo = int2(pixel) - step, hi = int2(pixel) + step;
    int2 size = int2(pc.width, pc.height);
    bool vl = all(lo >= 0) && all(lo < size), vh = all(hi >= 0) && all(hi < size);
    float zl = 0, zh = 0;
    if (vl) zl = de_tex2d(pc.depth).Load(int3(lo, 0)).r;
    if (vh) zh = de_tex2d(pc.depth).Load(int3(hi, 0)).r;
    vl = vl && isfinite(zl) && zl > 0;
    vh = vh && isfinite(zh) && zh > 0;
    if (!vl && !vh) return false;
    bool use_lo = vl && (!vh || abs(zl - depth) < abs(zh - depth));
    // At a corner the closest reverse-Z sample may lie on another face. Use shading
    // agreement only as a neighbour preference; mapped normals never define the plane.
    // If neither side agrees (e.g. a varying normal map), retain the depth choice.
    if (vl && vh) {
        int2 first = use_lo ? lo : hi;
        float3 first_normal = gb_decode(float4(0), de_tex2d(pc.gb1).Load(int3(first,0)), float4(0)).normal;
        bool agrees = all(isfinite(first_normal)) && dot(first_normal, shading_normal) >= .85;
        if (!agrees) {
            int2 other = use_lo ? hi : lo;
            float3 other_normal = gb_decode(float4(0), de_tex2d(pc.gb1).Load(int3(other,0)), float4(0)).normal;
            if (all(isfinite(other_normal)) && dot(other_normal, shading_normal) >= .85)
                use_lo = !use_lo;
        }
    }
    int2 q = use_lo ? lo : hi;
    float z = use_lo ? zl : zh;
    float3 neighbour = ssr_position(sc, (float2(q) + .5) / float2(size), z);
    if (!all(isfinite(neighbour))) return false;
    float3 delta = neighbour - position;
    if (vl && vh && (zl - depth) * (zh - depth) > 0 &&
        abs(dot(sc.view[2].xyz, delta)) > max(pc.thickness, .01)) return false;
    axis = use_lo ? -delta : delta;
    return all(isfinite(axis)) && dot(axis, axis) > 1e-20;
}

// Generic shading normals cannot predict the geometry's depth footprint. A producer-verified
// stored triangle normal is usable even when a face is only one pixel wide.
// Otherwise four depth loads / two position reconstructions establish a local geometric plane.
// Neighbour preference adds up to four GB1 loads; unverified shading normals never set its slope.
public bool ssr_geometric_normal(SsrParams pc, DeSceneConstants sc, uint2 pixel, float depth,
                                  float3 position, float3 shading_normal, out float3 normal,
                                  bool verified_geometry = false) {
    normal = float3(0);
    if (!(depth > 0) || !isfinite(depth) || !all(isfinite(position))) return false;
    if (verified_geometry) {
        float length2 = dot(shading_normal, shading_normal);
        if (!all(isfinite(shading_normal)) || !isfinite(length2) || !(length2 > 1e-8)) return false;
        normal = shading_normal * rsqrt(length2);
        return true;
    }
    float3 dx, dy;
    if (!ssr_depth_axis(pc, sc, pixel, depth, position, int2(1,0), shading_normal, dx) ||
        !ssr_depth_axis(pc, sc, pixel, depth, position, int2(0,1), shading_normal, dy)) return false;
    float3 n = cross(normalize(dx), normalize(dy));
    float length2 = dot(n, n);
    if (!all(isfinite(n)) || !(length2 > 1e-8)) return false;
    normal = n * rsqrt(length2);
    return true;
}

// Validate every contributor, including stationary receivers. A projected receiver plane
// predicts each tap's depth: a constant centre depth wrongly rejects continuous tilted surfaces.
public float4 ssr_history_sample(SsrParams pc, DeSceneConstants sc, uint index, float2 uv,
                                 float3 position, float3 normal, float3 geometric_normal,
                                 bool footprint_coverage = false) {
    if ((pc.history_valid & 1u) == 0 || !all(isfinite(uv)) || any(uv < 0) || any(uv >= 1) ||
        !all(isfinite(normal)) || dot(normal, normal) < 1e-8 ||
        !all(isfinite(geometric_normal)) || dot(geometric_normal, geometric_normal) < 1e-8)
        return float4(0);
    float4 point = float4(position, 1);
    float4 previous = float4(dot(sc.prev_view_proj[0], point), dot(sc.prev_view_proj[1], point),
                             dot(sc.prev_view_proj[2], point), dot(sc.prev_view_proj[3], point));
    if (!all(isfinite(previous)) || !(previous.w > 0)) return float4(0);
    float3 ndc = previous.xyz / previous.w;
    float3 tangent = normalize(cross(geometric_normal, abs(geometric_normal.z) < .9 ?
                                      float3(0,0,1) : float3(0,1,0)));
    float3 bitangent = cross(geometric_normal, tangent);
    float4 dt = float4(dot(sc.prev_view_proj[0].xyz, tangent), dot(sc.prev_view_proj[1].xyz, tangent),
                       dot(sc.prev_view_proj[2].xyz, tangent), dot(sc.prev_view_proj[3].xyz, tangent));
    float4 db = float4(dot(sc.prev_view_proj[0].xyz, bitangent), dot(sc.prev_view_proj[1].xyz, bitangent),
                       dot(sc.prev_view_proj[2].xyz, bitangent), dot(sc.prev_view_proj[3].xyz, bitangent));
    float3 plane = cross(dt.xyz - ndc * dt.w, db.xyz - ndc * db.w);
    if (!all(isfinite(plane)) || abs(plane.z) <= 1e-6 * length(plane)) return float4(0);
    float2 slope = -plane.xy / plane.z;
    float2 previous_uv = ndc.xy * float2(.5, -.5) + .5;
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
            if (w > 0) {
                float4 surface = de_rwtex2d(pc.history_surface)[q];
                float expected = ndc.z + dot(slope, (center - previous_uv) * float2(2, -2));
                if (!all(isfinite(surface)) || !(surface.x > 0) || !(expected > 0) ||
                    dot(surface.yzw, normal) < .85 ||
                    abs(surface.x - expected) > max(.002 * expected, 2e-4)) continue;
                // Raw reverse-Z tolerance alone accepts metre-scale separation at a distance.
                float separation = sc.proj[3].w > .5
                    ? abs(surface.x - expected) / max(abs(sc.proj[2].z), 1e-8)
                    : abs(expected / surface.x - 1) * abs(previous.w);
                if (separation > max(pc.thickness, .01)) continue;
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
