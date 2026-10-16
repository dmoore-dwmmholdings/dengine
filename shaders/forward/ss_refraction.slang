module ss_refraction;
import bindless;
import push_constants;
import scene_color_pyramid;
import "../scene/scene_types";
import "../lighting/lighting_common";
import "../lighting/ibl";
import "../probes/reflection_probe_sample";
import "../materials/standard/volume";

public struct RefractionScene {
    public uint color;
    public uint depth;
    public uint width;
    public uint height;
    public uint depth_pitch;
    public uint levels;
    public uint reserved0;
    public uint reserved1;
};
public struct RefractionRay {
    public float3 origin;
    public float3 direction;
    public float distance;
    public bool tir;
};
public struct RefractionResult {
    public float3 radiance;
    public float3 direction;
    public float2 uv;
    public float distance;
    public float confidence;
    public uint steps;
    public bool tir;
};
// Mean curvature uses the mesh's smooth base normal, before material normal maps. Degenerate,
// planar and concave neighborhoods use the parallel-interface model, never a universal sphere.
public float refraction_curvature(float3 dx, float3 dy, float3 nx, float3 ny) {
    float denominator = dot(dx, dx) + dot(dy, dy);
    float k = denominator > 1e-12 ? (dot(dx, nx) + dot(dy, ny)) / denominator : 0;
    return isfinite(k) ? max(k, 0.0) : 0;
}
public
RefractionRay refraction_exit(float3 position, float3 view, float3 normal, float3 geometry,
                              float curvature, float thickness, float ior) {
    RefractionRay ray;
    ray.origin = position;
    ray.direction = -view;
    ray.distance = 0;
    ray.tir = false;
    if (abs(ior - 1) < 1e-3) {
        // An absent interface cannot undergo TIR, including an exactly grazing ray.
        bool curved = isfinite(curvature) && curvature > 0 && curvature * thickness > .25;
        ray.distance = thickness <= 0 ? 0
                       : curved       ? max(dot(geometry, view) * thickness, 0)
                                      : thickness / max(abs(dot(geometry, view)), 1e-4);
        ray.origin = position + ray.direction * ray.distance;
        return ray;
    }
    bool inside = dot(view, geometry) < 0;
    float3 n = dot(normal, geometry) < 0 ? -normal : normal;
    float3 h = inside ? -n : n;
    float3 direction;
    // volume.de_refract takes n_other/n_wo, the reciprocal of GLSL's eta convention.
    if (!de_refract(view, h, inside ? 1 / ior : ior, direction)) {
        ray.direction = reflect(-view, h);
        ray.tir = true;
        return ray;
    }
    ray.direction = direction;
    if (thickness <= 0)
        return ray;
    if (inside) {
        ray.distance = thickness;
        return ray;
    }
    bool curved = isfinite(curvature) && curvature > 0 && curvature * thickness > .25;
    float3 exit_normal = -geometry;
    float distance = thickness / max(-dot(direction, geometry), 1e-4);
    if (curved) {
        // The material thickness is a diameter here: tangent-sphere radius = world thickness/2.
        // The true chord and exit normal recover both Snell interfaces for a smooth sphere.
        distance = max(-dot(geometry, direction) * thickness, 0.0);
        exit_normal = normalize(geometry + direction * (2 * distance / thickness));
    }
    ray.origin = position + direction * distance;
    ray.distance = distance;
    if (!de_refract(-direction, -exit_normal, 1 / ior, ray.direction)) {
        ray.direction = reflect(-view, h);
        ray.tir = true;
    }
    return ray;
}
public bool refraction_project(DeSceneConstants sc, float3 p, out float2 uv) {
    float4 q = float4(dot(sc.view_proj[0], float4(p, 1)), dot(sc.view_proj[1], float4(p, 1)),
                      dot(sc.view_proj[2], float4(p, 1)), dot(sc.view_proj[3], float4(p, 1)));
    uv = 0;
    // Orthographic clip.w stays one even behind the camera.
    if (!(q.w > 0) || !(-dot(sc.view[2], float4(p, 1)) > 0) || !all(isfinite(q)))
        return false;
    uv = q.xy / q.w * float2(.5, -.5) + .5;
    return all(uv >= 0) && all(uv <= 1);
}
float3 refraction_position(DeSceneConstants sc, float2 uv, float z) {
    float4 clip = float4(uv * float2(2, -2) + float2(-1, 1), z, 1);
    float4 p = float4(dot(sc.inv_view_proj[0], clip), dot(sc.inv_view_proj[1], clip),
                      dot(sc.inv_view_proj[2], clip), dot(sc.inv_view_proj[3], clip));
    return p.xyz / p.w;
}
public float3 refraction_environment(DeLightingView lv, uint cluster, float3 position, float3 direction,
                              float roughness) {
    float4 probe =
        de_probe_radiance(lv.inputs.probe_records, lv.inputs.probe_list, lv.inputs.probe_entry,
                          lv.inputs.probe_sampler, cluster, position, direction, roughness);
    return lerp(de_ibl_radiance(lv.ibl, direction, roughness), probe.rgb, saturate(probe.a)) *
           lv.pre_exposure;
}
public
RefractionResult ss_refract(RefractionScene scene, DeSceneConstants sc, DeLightingView lv,
                            uint cluster, uint2 pixel, float3 position, float3 view, float3 normal,
                            float3 geometry, float curvature, float thickness, float ior,
                            float roughness) {
    RefractionRay ray =
        refraction_exit(position, view, normal, geometry, curvature, thickness, max(ior, 1e-4));
    RefractionResult result;
    result.direction = ray.direction;
    result.distance = ray.distance;
    result.tir = ray.tir;
    result.uv = (float2(pixel) + .5) / float2(scene.width, scene.height);
    result.steps = 0;
    result.confidence = 0;
    result.radiance = refraction_environment(lv, cluster, position, ray.direction, roughness);
    if (ray.tir || scene.color == 0 || scene.levels == 0)
        return result;
    float2 uv;
    if (!refraction_project(sc, position, uv))
        return result;
    bool identity = abs(ior - 1) < 1e-3;
    // IOR 1 has no interface. This specialization preserves the exact mip0 bytes and does not
    // fade a known same-pixel opaque hit into unrelated sky/probe radiance at the screen edge.
    if (identity && roughness <= 0) {
        result.radiance =
            scene_color_load(scene.color,
                             uint4(scene.width, scene.height, scene_color_pitch(scene.width), 0),
                             int2(pixel))
                .rgb;
        result.confidence = 1;
        return result;
    }
    bool hit = thickness <= 0;
    float travel = 0;
    float view_direction = -dot(sc.view[2].xyz, ray.direction);
    float origin_depth = -dot(sc.view[2], float4(ray.origin, 1));
    float surface_depth = -dot(sc.view[2], float4(position, 1));
    if (!hit && scene.depth != 0 && view_direction > 1e-5) {
        // Bounded projected-depth march: advance to the sampled opaque depth plane, then
        // reproject/retest. Eight fetches maximum; discontinuities/nonconvergence fall back.
        for (uint step = 0; step < 8; ++step) {
            result.steps = step + 1;
            float3 p = ray.origin + ray.direction * travel;
            if (!refraction_project(sc, p, uv))
                break;
            uint2 sample = min(uint2(uv * float2(scene.width, scene.height)),
                               uint2(scene.width - 1, scene.height - 1));
            float z =
                asfloat(de_bytes(scene.depth).load(sample.y * scene.depth_pitch + sample.x * 4));
            if (!(z > 0))
                break;
            float3 opaque = refraction_position(
                sc, (float2(sample) + .5) / float2(scene.width, scene.height), z);
            float target_depth = -dot(sc.view[2], float4(opaque, 1));
            if (target_depth < surface_depth - 1e-4)
                break;
            float gap = target_depth - (origin_depth + view_direction * travel);
            if (abs(gap) <= max(.001, target_depth * 1e-5)) {
                hit = true;
                break;
            }
            float next = travel + gap / view_direction;
            if (!isfinite(next) || next < 0)
                break;
            travel = next;
        }
    }
    if (!hit)
        return result;
    result.uv = uv;
    float2 edge = min(uv, 1 - uv) * float2(scene.width, scene.height);
    result.confidence = smoothstep(0.0, 2.0, min(edge.x, edge.y));
    float max_lod = float(scene.levels - 1);
    float pixels_per_metre = abs(sc.proj[1].y) * float(scene.height) * .5;
    if (sc.proj[3].w == 0)
        pixels_per_metre /= max(origin_depth + view_direction * travel, 1e-4);
    float lod =
        clamp(roughness * max_lod + log2(1 + roughness * travel * pixels_per_metre), 0.0, max_lod);
    float3 screen =
        scene_color_sample(scene.color, scene.width, scene.height, scene.levels, uv, lod);
    result.radiance = lerp(result.radiance, screen, result.confidence);
    return result;
}

struct RefractionQuery {
    float4 position_thickness;
    float4 view_ior;
    float4 normal_roughness;
    float4 geometry_curvature;
    float4 attenuation;
};
struct RefractionQueryParams {
    uint scene;
    uint slot;
    uint view;
    uint entry;
    uint queries;
    uint output;
    uint count;
    uint reserved;
    RefractionScene source;
};
[shader("compute")][numthreads(64, 1, 1)] void
cs_ss_refraction_reference(uint3 id : SV_DispatchThreadID, uniform RefractionQueryParams pc) {
    if (id.x >= pc.count)
        return;
    RefractionQuery q = de_robuffer<RefractionQuery>(pc.queries)[id.x];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    uint2 pixel = uint2(id.x % pc.source.width, id.x / pc.source.width);
    RefractionResult r =
        ss_refract(pc.source, sc, lv, 0, pixel, q.position_thickness.xyz, q.view_ior.xyz,
                   q.normal_roughness.xyz, q.geometry_curvature.xyz, q.geometry_curvature.w,
                   q.position_thickness.w, q.view_ior.w, q.normal_roughness.w);
    de_buffer<float4>(pc.output)[id.x * 4] = float4(r.radiance, r.confidence);
    de_buffer<float4>(pc.output)[id.x * 4 + 1] = float4(r.uv, r.distance, r.tir ? 1 : 0);
    de_buffer<float4>(pc.output)[id.x * 4 + 2] = float4(r.direction, float(r.steps));
    de_buffer<float4>(pc.output)[id.x * 4 + 3] =
        float4(volume_transmittance(q.attenuation.rgb, q.attenuation.w, r.distance), 1);
}
