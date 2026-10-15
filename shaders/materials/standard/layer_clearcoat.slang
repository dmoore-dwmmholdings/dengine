// shaders/materials/standard/layer_clearcoat.slang: the clearcoat layer of the Standard model
// (KHR_materials_clearcoat; SPEC §12.6; DENG-300). standard.slang composes it at insertion
// point C, over everything below:
//
//   f = f_below x min(1 - c E_c(mu_o), 1 - c E_c(mu_i)) + c F_c(wo.h) D G2 / (4 mu_o mu_i) mu_i
//
// c the clearcoat factor; the coat is a GGX lobe of roughness clearcoat_roughness and IOR 1.5
// (F0 0.04, F90 1) around its own normal (the clearcoat normal map, else the vertex normal); E_c
// its directional albedo with that Fresnel (the energy LUT: 0.04 A + B), the cosines measured
// against the coat's normal. Albedo scaling in both directions keeps the stack energy
// preserving and reciprocal (KHR: the coat attenuates the base along the view and the light).
module layer_clearcoat;

import bindless;
import "../../brdf/bsdf_common";
import "../../brdf/ggx";
import "../../brdf/fresnel";
import "../../brdf/energy";
import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";

/// de_cooked_tex_slot values of the clearcoat inputs.
public static const uint DE_STD_TEX_CLEARCOAT = 5;           // R
public static const uint DE_STD_TEX_CLEARCOAT_ROUGHNESS = 6; // G
public static const uint DE_STD_TEX_CLEARCOAT_NORMAL = 7;    // tangent-space normal map
/// The coat's F0 (IOR 1.5).
public static const float DE_CLEARCOAT_F0 = 0.04;

/// Sets the clearcoat inputs of `s` from the material record alone (lighting): factor and
/// roughness (DeGpuMaterial.clearcoat.xy), the shading normal as the coat's, the CLEARCOAT
/// feature.
public void de_clearcoat_from_material(inout SurfaceData s, DeGpuMaterial m)
{
    s.clearcoat = saturate(m.clearcoat.x);
    s.clearcoat_roughness = saturate(m.clearcoat.y);
    s.clearcoat_normal = s.normal;
    if (s.clearcoat > 0.0)
        s.features |= DE_MATERIAL_FEATURE_CLEARCOAT;
    else
        s.features &= ~DE_MATERIAL_FEATURE_CLEARCOAT;
}

/// The same with the clearcoat textures and the coat normal (resolve): the vertex normal (facing
/// the viewer for a double-sided material), perturbed by the clearcoat normal map when there is
/// one and the mesh has tangents.
public void de_clearcoat_resolve(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v,
                                 TextureLodInfo lod)
{
    de_clearcoat_from_material(s, m);
    float4 white = float4(1.0, 1.0, 1.0, 1.0);
    s.clearcoat *= de_resolve_sample(m, DE_STD_TEX_CLEARCOAT, v, lod, white).r;
    s.clearcoat_roughness *= de_resolve_sample(m, DE_STD_TEX_CLEARCOAT_ROUGHNESS, v, lod, white).g;
    if (!(s.clearcoat > 0.0))
    {
        s.features &= ~DE_MATERIAL_FEATURE_CLEARCOAT;
        return;
    }
    float3 n = v.normal;
    float4 tangent = v.tangent;
    if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u && dot(v.geometric_normal, v.view) < 0.0)
    {
        n = -n;
        tangent.w = -tangent.w;
    }
    uint unused;
    if (de_resolve_texture(m, DE_STD_TEX_CLEARCOAT_NORMAL, unused) != DE_BINDLESS_INVALID &&
        v.has_tangent)
    {
        float4 nm = de_resolve_sample(m, DE_STD_TEX_CLEARCOAT_NORMAL, v, lod,
                                      float4(0.5, 0.5, 1.0, 1.0));
        float3 tn = nm.xyz * 2.0 - 1.0;
        float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
        float3 b = cross(n, t) * tangent.w;
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
    s.clearcoat_normal = n;
}

/// What evaluate, sample and pdf share for the coat of one surface and view direction.
public struct ClearcoatLayer
{
    public float factor;
    public float roughness;
    public float alpha;
    public ShadingFrame frame; // the coat's frame in the base's local frame (z = the coat normal)
    public float3 wo;          // wo in the coat's frame
    public float e_o;          // c E_c(mu_o)
    public uint lut;
};

/// The coat of `s` seen from base-local `wo`.
public ClearcoatLayer de_clearcoat_layer(SurfaceData s, float3 wo)
{
    ClearcoatLayer c;
    c.factor = saturate(s.clearcoat);
    c.roughness = saturate(s.clearcoat_roughness);
    c.alpha = de_ggx_alpha(c.roughness);
    c.lut = s.brdf_lut;
    float3 n = de_surface_frame(s).to_local(s.clearcoat_normal);
    float l = length(n);
    c.frame = de_frame_from_normal(l > 0.0 ? n / l : float3(0.0, 0.0, 1.0));
    c.wo = c.frame.to_local(wo);
    c.e_o = c.wo.z > 0.0 ? c.factor * de_clearcoat_albedo(c, c.wo.z) : 0.0;
    return c;
}

/// The coat's directional albedo at cos(theta) `mu` against its normal, for factor 1.
public float de_clearcoat_albedo(ClearcoatLayer c, float mu)
{
    EnergyTerms t = de_energy_lookup(c.lut, mu, c.roughness);
    return DE_CLEARCOAT_F0 * t.a + t.b;
}

/// What the coat leaves of the layers below for the pair (wo, wi) (base-local wi).
public float de_clearcoat_scale(ClearcoatLayer c, float3 wi)
{
    float mu_i = c.frame.to_local(wi).z;
    float e_i = mu_i > 0.0 ? c.factor * de_clearcoat_albedo(c, mu_i) : 0.0;
    return saturate(min(1.0 - c.e_o, 1.0 - e_i));
}

/// The coat lobe x cos(theta_i) (against the coat's normal), base-local directions.
public float de_clearcoat_evaluate(ClearcoatLayer c, float3 wi)
{
    float3 wi_c = c.frame.to_local(wi);
    if (!(c.wo.z > 0.0) || !(wi_c.z > 0.0))
        return 0.0;
    float3 h = normalize(c.wo + wi_c);
    float f = de_fresnel_schlick1(DE_CLEARCOAT_F0, 1.0, dot(c.wo, h));
    return c.factor * f * de_ggx_specular(c.alpha, c.alpha, c.wo, wi_c) * wi_c.z;
}

/// The density of a visible-normal sample of the coat lobe (over the sphere).
public float de_clearcoat_pdf(ClearcoatLayer c, float3 wi)
{
    return de_ggx_reflect_pdf(c.alpha, c.alpha, c.wo, c.frame.to_local(wi));
}

/// A visible-normal sample of the coat lobe (base-local direction).
public float3 de_clearcoat_sample(ClearcoatLayer c, float2 u)
{
    float3 h = de_ggx_vndf_sample(c.alpha, c.alpha, c.wo, u);
    return c.frame.to_world(de_reflect(c.wo, h));
}
