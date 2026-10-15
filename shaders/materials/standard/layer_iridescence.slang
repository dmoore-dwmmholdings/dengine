// shaders/materials/standard/layer_iridescence.slang: the iridescence inputs of the Standard model
// (KHR_materials_iridescence; SPEC §12.6; DENG-300). The effect is in the base's Fresnel
// (standard_base.slang de_standard_base_fresnel): Schlick mixed with the thin film's
// (brdf/thin_film.slang, Belcour and Barla 2017) by the iridescence factor, at cos(theta_d) =
// wo.h so the lobe stays reciprocal.
//
//   factor     iridescenceFactor x texture R
//   ior        iridescenceIor (the film's)
//   thickness  mix(iridescenceThicknessMinimum, iridescenceThicknessMaximum, texture G) nm; the
//              maximum without the texture. Thickness or factor 0: no film, the base Fresnel.
module layer_iridescence;

import bindless;
import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";

/// de_cooked_tex_slot values of the iridescence inputs.
public static const uint DE_STD_TEX_IRIDESCENCE = 17;           // R
public static const uint DE_STD_TEX_IRIDESCENCE_THICKNESS = 18; // G

/// Sets the iridescence of `s` from the material record alone (lighting): DeGpuMaterial
/// .iridescence = (factor, ior, thickness min, thickness max), the thickness the maximum, the
/// IRIDESCENCE feature.
public void de_iridescence_from_material(inout SurfaceData s, DeGpuMaterial m)
{
    s.iridescence = saturate(m.iridescence.x);
    s.iridescence_ior = m.iridescence.y;
    s.iridescence_thickness = max(m.iridescence.w, 0.0);
    if (s.iridescence > 0.0 && s.iridescence_thickness > 0.0)
        s.features |= DE_MATERIAL_FEATURE_IRIDESCENCE;
    else
        s.features &= ~DE_MATERIAL_FEATURE_IRIDESCENCE;
}

/// The same with the iridescence textures (resolve).
public void de_iridescence_resolve(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v,
                                   TextureLodInfo lod)
{
    de_iridescence_from_material(s, m);
    if (!(s.iridescence > 0.0))
        return;
    float4 white = float4(1.0, 1.0, 1.0, 1.0);
    s.iridescence *= de_resolve_sample(m, DE_STD_TEX_IRIDESCENCE, v, lod, white).r;
    float g = de_resolve_sample(m, DE_STD_TEX_IRIDESCENCE_THICKNESS, v, lod, white).g;
    s.iridescence_thickness = max(lerp(m.iridescence.z, m.iridescence.w, g), 0.0);
    if (s.iridescence > 0.0 && s.iridescence_thickness > 0.0)
        s.features |= DE_MATERIAL_FEATURE_IRIDESCENCE;
    else
        s.features &= ~DE_MATERIAL_FEATURE_IRIDESCENCE;
}
