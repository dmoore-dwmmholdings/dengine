// shaders/lighting/ibl.slang: image-based lighting from a prefiltered environment and its SH9
// irradiance (SPEC §12.5 step 5, §12.7; DENG-304). lighting_common.slang's evaluate_indirect calls
// de_ibl_evaluate, so the deferred pass and the forward pass (DENG-305) light with the same code.
//
//   diffuse   albedo / pi x E(n) x (1 - Ed(mu_o)): E the SH9 irradiance (DENG-100's cooked
//             coefficients x DeEnvironment.intensity), Ed the dielectric specular albedo of the
//             Standard model's energy LUT (DENG-299), the same coupling as its diffuse lobe; for a
//             uniform environment this is exactly the integral of the Standard diffuse lobe
//   specular  the prefiltered radiance at mip = roughness x mip_per_roughness along the GGX
//             dominant direction (Lagarde 2014), x the split-sum albedo of the Standard Fresnel
//             with Kulla-Conty multi-scattering (de_energy_specular_albedo), x Lagarde's specular
//             occlusion from AO, x horizon fading
//
// The environment rotates about +Z by DeEnvironment.rotation: a world direction d sees the
// environment at Rz(-rotation) d, for the SH9 and the prefiltered radiance alike.
//
// The prefiltered environment is a 2D "strip" of the cooked cube (src/render/lighting/ibl.c): the
// six faces stacked vertically in DTEX layer order (+X -X +Y -Y +Z -Z), every mip, the same
// format. The bindless tables have no cube type, so the shader picks the face itself
// (de_cooked_env_direction_face's convention) and clamps the face coordinates half a texel of the
// coarser mip inside the face, so bilinear filtering never reads a neighbouring face.
//
// Without an environment (no DeEnvironment, or its HDRI is not loaded): a constant radiance
// (r.ambient_fallback; default black) from every direction.
module ibl;

import bindless;
import "../materials/material_interface";
import "../brdf/bsdf_common";
import "../brdf/energy";
import "../materials/standard/standard_base";

/// DeIbl.flags: an environment strip is bound (else the constant ambient).
public static const uint DE_IBL_ENVIRONMENT = 1;
/// Rows of the strip: the cube's faces.
public static const uint DE_IBL_STRIP_FACES = 6;

/// One view's image-based lighting constants (192 bytes; ibl.c de__ibl_gpu).
public struct DeIbl
{
    public float4 sh[9];    // irradiance SH9 coefficients E_k (rgb) x intensity; sh[0].a: the
                            // intensity, which scales the prefiltered radiance
    public float4 rotation; // cos(rotation), sin(rotation), mip_per_roughness, the strip's last mip
    public float4 ambient;  // rgb: the constant radiance without an environment (nits)
    public uint strip;      // the strip texture (sampled bindless index)
    public uint sampler;    // linear, mip-linear, clamp
    public uint face_size;  // texels per face edge at mip 0
    public uint flags;      // DE_IBL_*
};

/// World direction `d` as the environment sees it (rotated by -rotation about +Z).
public float3 de_ibl_rotate(DeIbl ibl, float3 d)
{
    float c = ibl.rotation.x, s = ibl.rotation.y;
    return float3(c * d.x + s * d.y, -s * d.x + c * d.y, d.z);
}

/// The irradiance E(n) (rgb lux) of the environment at unit world normal `n` (SH9).
public float3 de_ibl_irradiance(DeIbl ibl, float3 n)
{
    if ((ibl.flags & DE_IBL_ENVIRONMENT) == 0u)
        return ibl.ambient.rgb * DE_PI;
    float3 d = de_ibl_rotate(ibl, n);
    float x = d.x, y = d.y, z = d.z;
    float b[9];
    b[0] = 0.282094791773878;
    b[1] = 0.488602511902920 * y;
    b[2] = 0.488602511902920 * z;
    b[3] = 0.488602511902920 * x;
    b[4] = 1.092548430592079 * x * y;
    b[5] = 1.092548430592079 * y * z;
    b[6] = 0.315391565252520 * (3.0 * z * z - 1.0);
    b[7] = 1.092548430592079 * x * z;
    b[8] = 0.546274215296040 * (x * x - y * y);
    float3 e = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < 9u; ++k)
        e += ibl.sh[k].rgb * b[k];
    return max(e, float3(0.0, 0.0, 0.0));
}

/// The cube face of direction `d` (de_cooked_env_direction_face: the largest component picks the
/// face, ties X before Y before Z) and its face coordinates sc, tc in [-1, 1].
public uint de_ibl_cube_face(float3 d, out float2 st)
{
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        float inv = 1.0 / a.x;
        st.y = -d.y * inv;
        st.x = d.x >= 0.0 ? -d.z * inv : d.z * inv;
        return d.x >= 0.0 ? 0u : 1u;
    }
    if (a.y >= a.z)
    {
        float inv = 1.0 / a.y;
        st.x = d.x * inv;
        st.y = d.y >= 0.0 ? d.z * inv : -d.z * inv;
        return d.y >= 0.0 ? 2u : 3u;
    }
    float inv = 1.0 / a.z;
    st.y = -d.y * inv;
    st.x = d.z >= 0.0 ? d.x * inv : -d.x * inv;
    return d.z >= 0.0 ? 4u : 5u;
}

/// The strip coordinates of world direction `d` at mip `lod` (clamped inside its face).
public float2 de_ibl_strip_uv(DeIbl ibl, float3 d, float lod)
{
    float2 st;
    uint face = de_ibl_cube_face(de_ibl_rotate(ibl, d), st);
    float2 uv = st * 0.5 + 0.5;
    float texels = max(float(ibl.face_size) / exp2(ceil(max(lod, 0.0))), 1.0);
    float h = 0.5 / texels;
    uv = clamp(uv, float2(h, h), float2(1.0 - h, 1.0 - h));
    return float2(uv.x, (float(face) + uv.y) / float(DE_IBL_STRIP_FACES));
}

/// The prefiltered radiance (rgb nits) of the environment in world direction `d` at perceptual
/// roughness `roughness`.
public float3 de_ibl_radiance(DeIbl ibl, float3 d, float roughness)
{
    if ((ibl.flags & DE_IBL_ENVIRONMENT) == 0u)
        return ibl.ambient.rgb;
    float lod = clamp(saturate(roughness) * ibl.rotation.z, 0.0, ibl.rotation.w);
    float2 uv = de_ibl_strip_uv(ibl, d, lod);
    return de_tex2d(ibl.strip).SampleLevel(de_sampler(ibl.sampler), uv, lod).rgb * ibl.sh[0].a;
}

/// The direction around which the GGX lobe of perceptual roughness `r` gathers most light
/// (Lagarde and de Rousiers 2014): between the mirror direction and the normal.
public float3 de_ibl_dominant_dir(float3 n, float3 r, float roughness)
{
    float a = roughness * roughness;
    float t = (1.0 - a) * (sqrt(1.0 - a) + a);
    return normalize(lerp(n, r, t));
}

/// Lagarde's specular occlusion from ambient occlusion `ao` at n.v `nov`.
public float de_ibl_specular_occlusion(float nov, float ao, float roughness)
{
    return saturate(pow(max(nov + ao, 0.0), exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

/// The environment's light leaving surface `s` towards `v` (unit, world, towards the eye): the
/// diffuse and specular parts (rgb nits, not pre-exposed), with `ao` the ambient occlusion. `lut`:
/// the energy LUT (DE_BINDLESS_INVALID: the analytic fallback).
public void de_ibl_evaluate(SurfaceData s, float3 v, DeIbl ibl, uint lut, float ao,
                            out float3 diffuse, out float3 specular)
{
    float3 n = s.normal;
    // DENG-300 hook (anisotropy): with DE_MATERIAL_FEATURE_ANISOTROPY, bend n towards the
    // anisotropic lobe here (layer_anisotropy.slang's bent normal) before the reflection below.
    float nov = max(dot(n, v), 1e-4);
    float r = saturate(s.roughness);
    StandardFresnel fr = de_standard_fresnel(s);
    EnergyTerms t = de_energy_lookup(lut, nov, r);
    float3 ed_avg, spec_avg;
    float3 ed = de_energy_specular_albedo(t, fr.f0_d, fr.f90_d, true, ed_avg);
    float3 spec_albedo = de_energy_specular_albedo(t, fr.f0, fr.f90, true, spec_avg);
    float3 albedo = s.base_color * (1.0 - saturate(s.metallic));
    diffuse = albedo * (de_ibl_irradiance(ibl, n) * DE_INV_PI) * (1.0 - ed) * ao;
    float3 refl = reflect(-v, n);
    float horizon = min(1.0 + dot(refl, n), 1.0);
    float3 dir = de_ibl_dominant_dir(n, refl, r);
    specular = de_ibl_radiance(ibl, dir, r) * spec_albedo *
               (de_ibl_specular_occlusion(nov, ao, r) * horizon * horizon);
}
