// shaders/materials/material_interface.slang: the material model contract (SPEC §12.6, §12.2;
// DENG-298). Every material model, built-in or a game's (de_material_model_register), is a type
// implementing IMaterialModel. The passes are generic over it and specialised per model at compile
// time (Slang generics; no dynamic dispatch):
//
//   resolve   (vis buffer, visbuffer/resolve_model.slang de_resolve_tiles<M>): load_params, then
//             resolve_gbuffer per pixel of the model's tiles; the SurfaceData goes into the thin
//             G-buffer (de_surface_to_gb; gbuffer.slang is the contract)
//   lighting  (deferred, DENG-303): de_surface_from_gb, then evaluate per light
//   RT / path tracing (S16 DENG-48, S17 DENG-72): resolve_gbuffer at a hit, then evaluate,
//             sample and pdf
//
// Directions passed to evaluate / sample / pdf are in the local shading frame of the surface (Z =
// the shading normal, X its tangent; bsdf_common.slang), unit, pointing away from the surface:
// `wo` towards the viewer, `wi` towards the light. de_surface_frame gives the frame of a
// SurfaceData. evaluate returns BSDF x |cos(theta_i)| (what a light estimate multiplies by the
// incoming radiance); sample returns weight = evaluate / pdf.
//
// Stability (DENG-298 notes): S14-S17 code against these signatures. Change them only by adding:
// new SurfaceData fields at the end, new feature bits at the end (never renumber), new interface
// requirements only with every model updated in the same change.
//
// Feature flags: DE_MATERIAL_FEATURE_* is the one set of per-material feature bits of S14
// (src/render/materials/material_models.h mirrors them; a test checks the values). A model sets
// them in SurfaceData.features; de_surface_to_gb maps the ones the G-buffer stores.
module material_interface;

import bindless;
__exported import "../brdf/bsdf_common";
__exported import "../scene/scene_types"; // DeGpuMaterial (load_params)
import "../visbuffer/gbuffer";
import "../visbuffer/feedback";

/// SurfaceData.features (and the material's feature set): append only.
public static const uint DE_MATERIAL_FEATURE_CLEARCOAT = 0x1;
public static const uint DE_MATERIAL_FEATURE_SHEEN = 0x2;
public static const uint DE_MATERIAL_FEATURE_ANISOTROPY = 0x4;
public static const uint DE_MATERIAL_FEATURE_IRIDESCENCE = 0x8;
public static const uint DE_MATERIAL_FEATURE_TRANSMISSION = 0x10;
public static const uint DE_MATERIAL_FEATURE_VOLUME = 0x20;
public static const uint DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION = 0x40;
public static const uint DE_MATERIAL_FEATURE_SUBSURFACE = 0x80;
public static const uint DE_MATERIAL_FEATURE_UNLIT = 0x100;
public static const uint DE_MATERIAL_FEATURE_FORWARD = 0x200; // blend or transmissive: forward pass
public static const uint DE_MATERIAL_FEATURE_DOUBLE_SIDED = 0x400;
public static const uint DE_MATERIAL_FEATURE_ALPHA_MASK = 0x800;

/// The interpolated vertex data at a surface point (world space, relative to the render origin).
public struct VertexAttribs
{
    public float3 position;
    public float3 normal;           // unit; the mesh's shading normal (geometric without one)
    public float4 tangent;          // xyz unit, w the bitangent sign (mirroring included)
    public float3 geometric_normal; // unit; the triangle's, by its winding and the transform
    public float3 view;             // unit; from the point towards the eye (ray origin)
    public float4 color;            // vertex colour 0 (white without colours)
    public float2 uv[2];            // UV0, UV1 (0 without the stream)
    public bool has_tangent;        // the mesh has a tangent stream
};

/// How textures are sampled at the point: UV derivatives per UV set (analytic one-pixel
/// differences in the resolve, ray cones in RT), the sampler, and texture-streaming feedback.
public struct TextureLodInfo
{
    public float2 dx[2];
    public float2 dy[2];
    public uint sampler;           // bindless sampler index
    public uint feedback;          // raw u32 buffer (feedback.slang); used when `record`
    public uint feedback_capacity;
    public bool record;            // this point records the mips it samples
};

/// What a model resolves a point to, and what lighting and RT shade with. Append only.
public struct SurfaceData
{
    public float3 base_color; // linear
    public float ao;          // ambient occlusion (1 = none)
    public float3 normal;     // world, unit: the shading normal (normal map applied)
    public float roughness;   // perceptual (alpha = roughness^2)
    public float4 tangent;    // world xyz, w bitangent sign; xyz 0 = no tangent frame
    public float metallic;
    public float specular;    // dielectric specular factor (KHR_materials_specular)
    public float3 emissive;   // radiance the surface emits
    public uint model;        // material model id (< DE_GPU_MATERIAL_MODELS)
    public uint features;     // DE_MATERIAL_FEATURE_*
};

/// A material model. Every requirement is static: a model is a type, not an object.
public interface IMaterialModel
{
    /// The model's parameters, as load_params builds them from a material record.
    associatedtype Params;

    /// The parameters of `material` (the GPU scene's material record of the point).
    static Params load_params(DeGpuMaterial material);

    /// The surface at a point (textures sampled through `lod`). Must not use derivatives.
    static SurfaceData resolve_gbuffer(Params params, VertexAttribs v, TextureLodInfo lod);

    /// BSDF x |cos(theta_i)| for local directions `wo`, `wi`.
    static float3 evaluate(SurfaceData s, float3 wo, float3 wi);

    /// A direction for `wo` from uniform `u` in [0, 1)^3 (u.z picks the lobe).
    static BsdfSample sample(SurfaceData s, float3 wo, float3 u);

    /// The solid-angle density with which `sample` returns `wi` for `wo`.
    static float pdf(SurfaceData s, float3 wo, float3 wi);

    /// The DE_BSDF_LOBE_* this surface has.
    static uint lobes(SurfaceData s);
};

/// Samples `texture` (bindless index; `fallback` when DE_BINDLESS_INVALID) with UV set `set` of
/// `v` and its derivatives in `lod`, and records the mip in the feedback when `lod.record`.
public float4 de_material_sample(TextureLodInfo lod, VertexAttribs v, uint texture, uint set,
                                 float4 fallback)
{
    if (texture == DE_BINDLESS_INVALID)
        return fallback;
    uint s = set != 0u ? 1u : 0u;
    if (lod.record)
        de_feedback_record(lod.feedback, lod.feedback_capacity, texture, lod.dx[s], lod.dy[s]);
    return de_tex2d(texture).SampleGrad(de_sampler(lod.sampler), v.uv[s], lod.dx[s], lod.dy[s]);
}

/// The shading frame of `s`: its tangent when it has one, else one built from the normal.
public ShadingFrame de_surface_frame(SurfaceData s)
{
    if (dot(s.tangent.xyz, s.tangent.xyz) > 0.0)
        return de_frame_from_tangent(s.normal, s.tangent);
    return de_frame_from_normal(s.normal);
}

/// The G-buffer flags of a feature set.
public uint de_features_to_gb_flags(uint features)
{
    uint f = 0u;
    if ((features & DE_MATERIAL_FEATURE_DOUBLE_SIDED) != 0u)
        f |= DE_GB_FLAG_DOUBLE_SIDED;
    if ((features & (DE_MATERIAL_FEATURE_TRANSMISSION | DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION)) !=
        0u)
        f |= DE_GB_FLAG_TRANSMISSION;
    if ((features & DE_MATERIAL_FEATURE_SUBSURFACE) != 0u)
        f |= DE_GB_FLAG_SUBSURFACE;
    if ((features & DE_MATERIAL_FEATURE_CLEARCOAT) != 0u)
        f |= DE_GB_FLAG_CLEARCOAT;
    if ((features & DE_MATERIAL_FEATURE_UNLIT) != 0u)
        f |= DE_GB_FLAG_UNLIT;
    return f;
}

/// What the thin G-buffer keeps of `s` (gb0/gb1/gb2_encode store it).
public DeGbSurface de_surface_to_gb(SurfaceData s)
{
    DeGbSurface g;
    g.base_color = s.base_color;
    g.ao = s.ao;
    g.normal = s.normal;
    g.roughness = s.roughness;
    g.metallic = s.metallic;
    g.specular = s.specular;
    g.model = s.model & (DE_GPU_MATERIAL_MODELS - 1u);
    g.flags = de_features_to_gb_flags(s.features);
    return g;
}

/// A SurfaceData from what the G-buffer kept (lighting): no tangent frame, no emission (the
/// lighting target already holds it), the features the G-buffer flags name.
public SurfaceData de_surface_from_gb(DeGbSurface g)
{
    SurfaceData s;
    s.base_color = g.base_color;
    s.ao = g.ao;
    s.normal = g.normal;
    s.roughness = g.roughness;
    s.tangent = float4(0.0, 0.0, 0.0, 1.0);
    s.metallic = g.metallic;
    s.specular = g.specular;
    s.emissive = float3(0.0, 0.0, 0.0);
    s.model = g.model;
    s.features = 0u;
    if ((g.flags & DE_GB_FLAG_DOUBLE_SIDED) != 0u)
        s.features |= DE_MATERIAL_FEATURE_DOUBLE_SIDED;
    if ((g.flags & DE_GB_FLAG_TRANSMISSION) != 0u)
        s.features |= DE_MATERIAL_FEATURE_TRANSMISSION;
    if ((g.flags & DE_GB_FLAG_SUBSURFACE) != 0u)
        s.features |= DE_MATERIAL_FEATURE_SUBSURFACE;
    if ((g.flags & DE_GB_FLAG_CLEARCOAT) != 0u)
        s.features |= DE_MATERIAL_FEATURE_CLEARCOAT;
    if ((g.flags & DE_GB_FLAG_UNLIT) != 0u)
        s.features |= DE_MATERIAL_FEATURE_UNLIT;
    return s;
}
