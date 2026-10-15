// shaders/brdf/ggx.slang: the GGX (Trowbridge-Reitz) microfacet distribution, Smith masking and
// visible-normal sampling (SPEC §12.6; DENG-298). Pure functions; local shading frame (Z = the
// normal; bsdf_common.slang). `alpha` is the GGX width (perceptual roughness squared; a model
// clamps it away from 0 itself), `ax`/`ay` its values along X and Y for anisotropy.
//
//   de_ggx_d / de_ggx_d_aniso        D(h), normalised: integral of D(h) cos(theta_h) = 1
//   de_ggx_lambda / de_ggx_g1        Smith Lambda and G1 (masking of one direction)
//   de_ggx_g2                        height-correlated G2 = 1 / (1 + Lambda(wo) + Lambda(wi))
//   de_ggx_vndf_sample               a visible normal (Heitz 2018) through the spherical-cap
//                                    method (Dupuy and Benyoub 2023, "Sampling Visible GGX
//                                    Normals with Spherical Caps")
//   de_ggx_vndf_pdf                  its density over normals: G1(wo) max(0, wo.h) D(h) / wo.z
//   de_ggx_reflect_pdf               the density of the reflected direction: vndf / (4 wo.h)
//   de_ggx_specular                  the reflection BRDF D G2 / (4 |wo.z| |wi.z|) (no Fresnel)
module ggx;

import bsdf_common;

/// Isotropic D for cos(theta_h) = `cos_h` (0 below the horizon).
public float de_ggx_d(float alpha, float cos_h)
{
    if (!(cos_h > 0.0))
        return 0.0;
    float a2 = alpha * alpha;
    // cos^2 (a2 - 1) + 1, as sin^2 + a2 cos^2 with sin^2 = (1 - cos)(1 + cos): no cancellation
    // at the peak of a narrow lobe (cos_h near 1).
    float d = (1.0 - cos_h) * (1.0 + cos_h) + a2 * cos_h * cos_h;
    return a2 / (DE_PI * d * d);
}

/// Anisotropic D for local half vector `h` (unit).
public float de_ggx_d_aniso(float ax, float ay, float3 h)
{
    if (!(h.z > 0.0))
        return 0.0;
    float3 s = float3(h.x / ax, h.y / ay, h.z);
    float l2 = dot(s, s);
    return 1.0 / (DE_PI * ax * ay * l2 * l2);
}

/// Smith Lambda of local direction `w` (unit; either hemisphere).
public float de_ggx_lambda(float ax, float ay, float3 w)
{
    float z2 = w.z * w.z;
    float t = ax * ax * w.x * w.x + ay * ay * w.y * w.y;
    return (sqrt(1.0 + t / z2) - 1.0) * 0.5;
}

/// G1 of local direction `w` (0 for a grazing direction).
public float de_ggx_g1(float ax, float ay, float3 w)
{
    if (w.z == 0.0)
        return 0.0;
    return 1.0 / (1.0 + de_ggx_lambda(ax, ay, w));
}

/// Height-correlated G2 of local directions `wo` and `wi`.
public float de_ggx_g2(float ax, float ay, float3 wo, float3 wi)
{
    if (wo.z == 0.0 || wi.z == 0.0)
        return 0.0;
    return 1.0 / (1.0 + de_ggx_lambda(ax, ay, wo) + de_ggx_lambda(ax, ay, wi));
}

/// A visible normal for local `wo` (wo.z > 0) from uniform `u` in [0, 1)^2: the half vector of
/// the microfacet `wo` sees, distributed as de_ggx_vndf_pdf.
public float3 de_ggx_vndf_sample(float ax, float ay, float3 wo, float2 u)
{
    // Stretch to the hemisphere configuration (alpha = 1), sample the spherical cap of
    // directions whose half vector with wo is visible, unstretch.
    float3 v = normalize(float3(wo.x * ax, wo.y * ay, wo.z));
    float phi = 2.0 * DE_PI * u.x;
    float z = (1.0 - u.y) * (1.0 + v.z) - v.z;
    float sin_theta = sqrt(saturate(1.0 - z * z));
    float3 c = float3(sin_theta * cos(phi), sin_theta * sin(phi), z);
    float3 h = c + v;
    return normalize(float3(h.x * ax, h.y * ay, max(h.z, 0.0)));
}

/// The density of de_ggx_vndf_sample over half vectors: G1(wo) max(0, wo.h) D(h) / wo.z.
public float de_ggx_vndf_pdf(float ax, float ay, float3 wo, float3 h)
{
    if (!(wo.z > 0.0))
        return 0.0;
    return de_ggx_g1(ax, ay, wo) * max(dot(wo, h), 0.0) * de_ggx_d_aniso(ax, ay, h) / wo.z;
}

/// The density of `wi` = reflect(-wo, h) for h from de_ggx_vndf_sample (solid angle):
/// G1(wo) D(h) / (4 wo.z).
public float de_ggx_reflect_pdf(float ax, float ay, float3 wo, float3 wi)
{
    float3 h = wo + wi;
    float l2 = dot(h, h);
    if (!(wo.z > 0.0) || !(l2 > 0.0))
        return 0.0;
    h *= rsqrt(l2);
    float wo_h = dot(wo, h);
    if (!(wo_h > 0.0))
        return 0.0;
    return de_ggx_vndf_pdf(ax, ay, wo, h) / (4.0 * wo_h);
}

/// `wo` reflected about unit `h`.
public float3 de_reflect(float3 wo, float3 h)
{
    return 2.0 * dot(wo, h) * h - wo;
}

/// The microfacet reflection BRDF without Fresnel: D(h) G2(wo, wi) / (4 wo.z wi.z), 0 unless both
/// directions are above the surface.
public float de_ggx_specular(float ax, float ay, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return 0.0;
    float3 h = normalize(wo + wi);
    return de_ggx_d_aniso(ax, ay, h) * de_ggx_g2(ax, ay, wo, wi) / (4.0 * wo.z * wi.z);
}
