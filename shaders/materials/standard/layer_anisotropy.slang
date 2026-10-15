// shaders/materials/standard/layer_anisotropy.slang: the anisotropy inputs of the Standard model
// (KHR_materials_anisotropy; SPEC §12.6; DENG-300). The lobe itself is the base's GGX made
// anisotropic in standard_base.slang (alpha_t = mix(alpha, 1, strength^2) along the direction,
// alpha_b = alpha across it, the anisotropic D, G and VNDF sampling of brdf/ggx.slang).
//
//   strength   anisotropyStrength x texture B
//   direction  the texture's RG (x 2 - 1; (1, 0) without the texture) rotated by
//              anisotropyRotation, counter-clockwise from the tangent towards the bitangent
//
// de_anisotropy_bent_normal is the KHR reference's bent normal for image-based lighting (DENG-304
// calls it when the ANISOTROPY feature is set).
module layer_anisotropy;

import bindless;
import "../../brdf/bsdf_common";
import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";

/// de_cooked_tex_slot of the anisotropy texture (RG direction, B strength).
public static const uint DE_STD_TEX_ANISOTROPY = 16;

/// `d` rotated by `angle` radians in the tangent plane.
float2 rotate(float2 d, float angle)
{
    float c = cos(angle), s = sin(angle);
    return float2(d.x * c - d.y * s, d.x * s + d.y * c);
}

/// Sets the anisotropy of `s` from the material record alone (lighting): strength
/// (DeGpuMaterial.extra.w), direction (1, 0) rotated by DeGpuMaterial.subsurface.w, the
/// ANISOTROPY feature.
public void de_anisotropy_from_material(inout SurfaceData s, DeGpuMaterial m)
{
    s.anisotropy = saturate(m.extra.w);
    s.anisotropy_direction = rotate(float2(1.0, 0.0), m.subsurface.w);
    if (s.anisotropy > 0.0)
        s.features |= DE_MATERIAL_FEATURE_ANISOTROPY;
    else
        s.features &= ~DE_MATERIAL_FEATURE_ANISOTROPY;
}

/// The same with the anisotropy texture (resolve).
public void de_anisotropy_resolve(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v,
                                  TextureLodInfo lod)
{
    de_anisotropy_from_material(s, m);
    if ((s.features & DE_MATERIAL_FEATURE_ANISOTROPY) == 0u)
        return;
    float4 t = de_resolve_sample(m, DE_STD_TEX_ANISOTROPY, v, lod, float4(1.0, 0.5, 1.0, 1.0));
    float2 d = t.xy * 2.0 - 1.0;
    float l = length(d);
    s.anisotropy_direction = rotate(l > 0.0 ? d / l : float2(1.0, 0.0), m.subsurface.w);
    s.anisotropy *= t.z;
    if (!(s.anisotropy > 0.0))
        s.features &= ~DE_MATERIAL_FEATURE_ANISOTROPY;
}

/// The anisotropy direction of `s` in world space (along the stretched highlight's tangent).
public float3 de_anisotropy_tangent(SurfaceData s)
{
    ShadingFrame f = de_surface_frame(s);
    return normalize(f.t * s.anisotropy_direction.x + f.b * s.anisotropy_direction.y);
}

/// The KHR reference's bent normal for image-based lighting: the normal of the anisotropic
/// highlight's plane seen from view direction `v` (world, towards the viewer), blended back to
/// the shading normal `n` as the surface gets smooth or the anisotropy weak. `t` and `b` are the
/// anisotropy direction and the one across it (world, unit), `roughness` perceptual.
public float3 de_anisotropy_bent_normal(float3 n, float3 t, float3 b, float3 v, float strength,
                                        float roughness)
{
    float3 bent = normalize(cross(cross(b, v), b));
    float a = 1.0 - strength * (1.0 - roughness);
    a *= a;
    a *= a;
    return normalize(lerp(bent, n, a));
}
