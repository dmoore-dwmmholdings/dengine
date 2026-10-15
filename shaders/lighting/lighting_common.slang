// shaders/lighting/lighting_common.slang: what every lit pass shares (SPEC §12.5 steps 5-7;
// DENG-303): the per-view lighting constants, the froxel cluster grid, and the direct and indirect
// light estimates generic over the material model. The deferred pass (deferred_lighting.slang)
// uses it today; the forward transparent pass (DENG-305), volumetric fog (S34) and particles reuse
// the same cluster lists.
//
// A CONTRACT, append only (with lighting_inputs.slang): DENG-304 implements evaluate_indirect;
// S15 adds shadow terms through lighting_inputs. Change signatures only by adding parameters with
// every caller updated in the same change.
//
// Clusters: tiles of tile_px x tile_px pixels x `slices` exponential depth slices. Slice k < S - 1
// covers view depth [near (far/near)^(k/(S-1)), near (far/near)^((k+1)/(S-1))); the last slice
// extends from `far` to infinity. Index = (slice x tiles_y + tile y) x tiles_x + tile x.
module lighting_common;

import bindless;
__exported import "../lighting/lights";
__exported import "../lighting/lighting_inputs";
__exported import "../materials/material_interface";
import "../lighting/ltc";
__exported import "../lighting/ibl";

/// DeLightingView.flags
public static const uint DE_LIGHTING_ORTHO = 1; // an orthographic projection
public static const uint DE_LIGHTING_SPLIT = 2; // r.split_lighting: diffuse / specular targets
/// DENG-306: subsurface scattering runs after lighting: SUBSURFACE pixels write their diffuse to
/// the diffuse target only, and their specular to the lighting target.
public static const uint DE_LIGHTING_SSS = 4;
/// Directional lights shaded at most (more are counted, not shaded).
public static const uint DE_LIGHTING_MAX_DIRECTIONAL = 4;
/// Byte offsets of the light counters (a raw buffer of DE_LIGHTING_COUNTER_BYTES).
public static const uint DE_LIGHTING_COUNTER_LOCAL = 0;       // local lights asked for
public static const uint DE_LIGHTING_COUNTER_DIRECTIONAL = 4; // directional lights asked for
public static const uint DE_LIGHTING_COUNTER_PROXIES = 8;     // emissive proxies asked for
public static const uint DE_LIGHTING_COUNTER_OVERFLOW = 12;   // entries over the cluster cap
public static const uint DE_LIGHTING_COUNTER_ENTRIES = 16;    // entries after the cap
public static const uint DE_LIGHTING_COUNTER_DROPPED = 20;    // entries past the list capacity
public static const uint DE_LIGHTING_COUNTER_BYTES = 32;

/// One view's lighting constants (368 bytes; lighting.h de__lighting_view), uploaded per frame.
public struct DeLightingView
{
    public uint tiles_x;
    public uint tiles_y;
    public uint slices;
    public uint tile_px;
    public uint cluster_count;
    public uint max_per_cluster;
    public uint list_capacity;   // entries of the cluster index list
    public uint local_capacity;  // entries of the local light list
    public float near;           // the first slice's near depth (the camera's near plane)
    public float far;            // where the last (infinite) slice starts
    public float log_scale;      // slice = floor(log(depth) x log_scale + log_bias)
    public float log_bias;
    public float cutoff;         // lux at which a light's derived range ends (r.light_cutoff_lux)
    public float emissive_threshold; // nit m^2 above which an emissive mesh gets a proxy light
    public float pre_exposure;   // lighting output scale (exposure.c, DENG-304)
    public uint proxy_capacity;  // emissive proxies at most per frame
    public uint flags;           // DE_LIGHTING_*
    public uint light_slots;     // the GPU scene's light table slots
    public uint instance_slots;  // its instance table slots
    public uint brdf_lut;        // the energy LUT's bindless index (SurfaceData.brdf_lut; DENG-299)
    public float4 inv_proj[4];   // clip -> view space (rows)
    public LightingInputs inputs;
    public DeIbl ibl;            // DENG-304: image-based lighting (ibl.slang)
};

/// Where a view's cluster lists are (bindless indices, all read-only for shading).
public struct DeClusterLists
{
    public uint lights;     // DeClusterLight[local_capacity]
    public uint dir_lights; // DeClusterLight[DE_LIGHTING_MAX_DIRECTIONAL]
    public uint grid;       // uint2 per cluster: offset into `list`, count
    public uint list;       // uint per entry: index into `lights`
    public uint counters;   // raw DE_LIGHTING_COUNTER_*
    public uint ltc1;       // the LTC tables (sampled textures) and their sampler
    public uint ltc2;
    public uint sampler;
};

/// The slice of view depth `depth` (> 0).
public uint de_cluster_slice(DeLightingView lv, float depth)
{
    float k = floor(log(max(depth, lv.near)) * lv.log_scale + lv.log_bias);
    return uint(clamp(k, 0.0, float(lv.slices - 1u)));
}

/// The cluster of pixel `p` at view depth `depth`.
public uint de_cluster_index(DeLightingView lv, uint2 p, float depth)
{
    uint tx = min(p.x / lv.tile_px, lv.tiles_x - 1u);
    uint ty = min(p.y / lv.tile_px, lv.tiles_y - 1u);
    return (de_cluster_slice(lv, depth) * lv.tiles_y + ty) * lv.tiles_x + tx;
}

/// The near depth of slice `k` (k = slices: infinity).
public float de_cluster_slice_depth(DeLightingView lv, uint k)
{
    if (k >= lv.slices)
        return 1e30;
    return exp((float(k) - lv.log_bias) / lv.log_scale);
}

/// Direct light estimate: diffuse and specular radiance leaving the surface towards the eye.
public struct DeDirect
{
    public float3 diffuse;
    public float3 specular;
};

/// The surface with base colour 0: what the split takes as its specular part (the diffuse part
/// is the rest; exact when the model's diffuse lobe is linear in base colour and its dielectric
/// specular does not depend on it).
public SurfaceData de_surface_specular_only(SurfaceData s)
{
    SurfaceData t = s;
    t.base_color = float3(0.0, 0.0, 0.0);
    return t;
}

/// Adds the light reaching `s` from direction `wi` (world) with illuminance `e` (rgb lux). The
/// split is the model's own (DENG-306: evaluate_split), exact for metals too.
void add_light<M : IMaterialSplitModel>(inout DeDirect r, SurfaceData s, ShadingFrame f,
                                        float3 wo, float3 wi, float3 e, bool split)
{
    float3 wil = f.to_local(wi);
    if (!(wil.z > 0.0) && (s.features & DE_MATERIAL_FEATURE_TRANSMISSION) == 0u)
        return;
    if (split)
    {
        float3 diffuse;
        float3 total = M.evaluate_split(s, wo, wil, diffuse) * e;
        diffuse *= e;
        r.diffuse += diffuse;
        r.specular += total - diffuse;
        return;
    }
    r.diffuse += M.evaluate(s, wo, wil) * e;
}

/// The direct light at surface `s` (point `p`, unit direction `v` towards the eye, pixel `px` for
/// the screen-space inputs) from the view's directional lights and the local lights of cluster
/// `cluster`, shaded through model M (area lights through LTC with the surface's Standard
/// parameters: F0 = 0.04 specular for dielectrics and the base colour for metals, F90 = specular
/// for dielectrics). With `split` false everything is in `diffuse`.
public DeDirect evaluate_direct_clustered<M : IMaterialSplitModel>(SurfaceData s, float3 p,
                                                                   float3 v, uint2 px,
                                                                   DeLightingView lv,
                                                                   DeClusterLists cl,
                                                                   uint cluster, bool split)
{
    DeDirect r;
    r.diffuse = float3(0.0, 0.0, 0.0);
    r.specular = float3(0.0, 0.0, 0.0);
    ShadingFrame f = de_surface_frame(s);
    float3 wo = f.to_local(v);
    if ((s.features & DE_MATERIAL_FEATURE_DOUBLE_SIDED) != 0u && wo.z < 0.0)
        wo.z = -wo.z;
    DeBytes counters = de_bytes(cl.counters);
    uint ndir = min(counters.load(DE_LIGHTING_COUNTER_DIRECTIONAL), DE_LIGHTING_MAX_DIRECTIONAL);
    for (uint i = 0; i < ndir; ++i)
    {
        DeClusterLight l = de_robuffer<DeClusterLight>(cl.dir_lights)[i];
        DeLightHit h = de_light_punctual(l, p);
        float vis = de_input_shadow_directional(lv.inputs, px, i);
        if (i == 0u)
            vis *= de_input_contact_shadow(lv.inputs, px);
        add_light<M>(r, s, f, wo, h.wi, h.illuminance * vis, split);
    }
    uint2 cell = de_robuffer<uint2>(cl.grid)[cluster];
    float3 f0 = lerp(float3(0.04, 0.04, 0.04) * s.specular, s.base_color, s.metallic);
    float f90 = lerp(s.specular, 1.0, s.metallic);
    float3 albedo = s.base_color * (1.0 - s.metallic);
    for (uint k = 0; k < cell.y; ++k)
    {
        uint index = de_robuffer<uint>(cl.list)[cell.x + k];
        DeClusterLight l = de_robuffer<DeClusterLight>(cl.lights)[index];
        uint kind = de_light_kind(l);
        float vis = de_input_shadow_local(lv.inputs, px, index);
        if (kind == DE_LIGHT_KIND_RECT || kind == DE_LIGHT_KIND_DISK)
        {
            float3 d, sp;
            de_ltc_area_light(l, p, s.normal, v, s.roughness, f0, f90, cl.ltc1, cl.ltc2,
                              cl.sampler, d, sp);
            if (split)
            {
                r.diffuse += d * albedo * vis;
                r.specular += sp * vis;
            }
            else
            {
                r.diffuse += (d * albedo + sp) * vis;
            }
            continue;
        }
        DeLightHit h = de_light_punctual(l, p);
        if (!any(h.illuminance > 0.0))
            continue;
        SurfaceData sw = s;
        sw.roughness = de_light_widened_roughness(s.roughness, l.direction.w, h.distance);
        add_light<M>(r, sw, f, wo, h.wi, h.illuminance * vis, split);
    }
    return r;
}

/// Indirect light leaving `s` towards the eye, split into its diffuse and specular parts (not
/// pre-exposed): image-based lighting (ibl.slang, DENG-304) with the G-buffer AO times the AO
/// input, blended with the indirect override inputs where a producer wrote them (S15).
public void evaluate_indirect_split(SurfaceData s, float3 p, float3 v, uint2 px, DeLightingView lv,
                                    out float3 diffuse, out float3 specular)
{
    float ao = s.ao * de_input_ao(lv.inputs, px);
    de_ibl_evaluate(s, v, lv.ibl, lv.brdf_lut, ao, diffuse, specular);
    float4 od = de_input_indirect_diffuse_override(lv.inputs, px);
    float4 os = de_input_indirect_specular_override(lv.inputs, px);
    diffuse = lerp(diffuse, od.rgb * ao, saturate(od.a));
    specular = lerp(specular, os.rgb, saturate(os.a));
}

/// Indirect light leaving `s` towards the eye (evaluate_indirect_split's sum). The deferred pass
/// and the forward pass (DENG-305) both use it.
public float3 evaluate_indirect(SurfaceData s, float3 p, float3 v, uint2 px, DeLightingView lv)
{
    float3 d, sp;
    evaluate_indirect_split(s, p, v, px, lv, d, sp);
    return d + sp;
}
