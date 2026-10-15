// shaders/materials/standard/layer_sheen.slang: the sheen layer of the Standard model
// (KHR_materials_sheen; SPEC §12.6; DENG-300). standard.slang composes it at insertion point S:
//
//   f = f_below x min(1 - k E(mu_o), 1 - k E(mu_i)) + sheen_color D V cos(theta_i)
//
// with k = max(sheen_color) and E the sheen's directional albedo (brdf/sheen.slang, from the
// energy LUT): the KHR albedo scaling, in its reciprocal form (both directions). Sampled with the
// cosine hemisphere.
module layer_sheen;

import bindless;
import "../../brdf/bsdf_common";
import "../../brdf/sampling";
import "../../brdf/sheen";
import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";

/// de_cooked_tex_slot values of the sheen inputs.
public static const uint DE_STD_TEX_SHEEN_COLOR = 8;     // sRGB colour
public static const uint DE_STD_TEX_SHEEN_ROUGHNESS = 9; // A

/// Sets the sheen inputs of `s` from the material record alone (lighting, which has no
/// textures): DeGpuMaterial.sheen = (colour, roughness), and the SHEEN feature.
public void de_sheen_from_material(inout SurfaceData s, DeGpuMaterial m)
{
    s.sheen_color = max(m.sheen.rgb, float3(0.0, 0.0, 0.0));
    s.sheen_roughness = saturate(m.sheen.w);
    if (max(s.sheen_color.x, max(s.sheen_color.y, s.sheen_color.z)) > 0.0)
        s.features |= DE_MATERIAL_FEATURE_SHEEN;
    else
        s.features &= ~DE_MATERIAL_FEATURE_SHEEN;
}

/// The same with the sheen textures (resolve).
public void de_sheen_resolve(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v,
                             TextureLodInfo lod)
{
    de_sheen_from_material(s, m);
    if ((s.features & DE_MATERIAL_FEATURE_SHEEN) == 0u)
        return;
    float4 white = float4(1.0, 1.0, 1.0, 1.0);
    s.sheen_color *= de_resolve_sample(m, DE_STD_TEX_SHEEN_COLOR, v, lod, white).rgb;
    s.sheen_roughness *= de_resolve_sample(m, DE_STD_TEX_SHEEN_ROUGHNESS, v, lod, white).a;
}

/// What evaluate, sample and pdf share for the sheen of one surface and view direction.
public struct SheenLayer
{
    public float3 color;
    public float roughness; // clamped (DE_SHEEN_ROUGHNESS_MIN)
    public float k;         // max(color): the albedo scaling's weight
    public float e_o;       // E(mu_o)
    public uint lut;
};

public SheenLayer de_sheen_layer(SurfaceData s, float3 wo)
{
    SheenLayer l;
    l.color = s.sheen_color;
    l.roughness = de_sheen_roughness(s.sheen_roughness);
    l.k = max(l.color.x, max(l.color.y, l.color.z));
    l.lut = s.brdf_lut;
    l.e_o = de_sheen_albedo(l.lut, wo.z, l.roughness);
    return l;
}

/// What the sheen leaves of the layers below for the pair (wo, wi).
public float de_sheen_scale(SheenLayer l, float3 wi)
{
    float e_i = de_sheen_albedo(l.lut, wi.z, l.roughness);
    return saturate(min(1.0 - l.k * l.e_o, 1.0 - l.k * e_i));
}

/// The sheen lobe x cos(theta_i).
public float3 de_sheen_evaluate(SheenLayer l, float3 wo, float3 wi)
{
    return l.color * (de_sheen_brdf(l.roughness, wo, wi) * max(wi.z, 0.0));
}

/// The sheen lobe's albedo at wo (luminance-free: colour x E), for lobe selection.
public float3 de_sheen_albedo_o(SheenLayer l)
{
    return l.color * l.e_o;
}

/// The sheen lobe's sampling density (the cosine hemisphere).
public float de_sheen_pdf(float3 wi)
{
    return de_cosine_hemisphere_pdf(wi.z);
}
