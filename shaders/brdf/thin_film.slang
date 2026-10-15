// shaders/brdf/thin_film.slang: thin-film iridescence for KHR_materials_iridescence (SPEC §12.6;
// DENG-300). Pure functions.
//
// de_thin_film_fresnel is Belcour and Barla 2017 ("A Practical Extension to Microfacet Theory for
// the Modeling of Varying Iridescence") as the KHR extension's reference implementation writes
// it: the Airy series of a film of index `film_ior` and thickness `thickness_nm` on a base whose
// F0 is `base_f0`, light arriving from a medium of index `outside_ior`, its first three terms
// (m = 0, 1, 2) integrated against the CIE sensitivities with Gaussian fits in Fourier space
// (de_thin_film_sensitivity), then XYZ to linear Rec.709. The m = 0 term is the incoherent sum, the
// others are the interference; a film thinner than ~30 nm fades into the outside medium.
module thin_film;

import bsdf_common;

float sq(float x)
{
    return x * x;
}
float3 sq3(float3 x)
{
    return x * x;
}

/// F0 of an interface from the transmitted and incident indices.
float ior_to_f0(float t, float i)
{
    return sq((t - i) / (t + i));
}
float3 ior_to_f03(float3 t, float i)
{
    return sq3((t - i) / (t + i));
}

/// The index whose F0 against 1 is `f0` (per channel).
float3 f0_to_ior(float3 f0)
{
    float3 s = sqrt(f0);
    return (1.0 + s) / (1.0 - s);
}

float schlick(float f0, float mu)
{
    return f0 + (1.0 - f0) * de_pow5(1.0 - saturate(mu));
}
float3 schlick3(float3 f0, float mu)
{
    return f0 + (1.0 - f0) * de_pow5(1.0 - saturate(mu));
}

/// The CIE sensitivities in Fourier space at optical path difference `opd_nm` and phase shift
/// `shift`, in linear Rec.709 (Belcour and Barla's Gaussian fit).
public float3 de_thin_film_sensitivity(float opd_nm, float3 shift)
{
    float phase = 2.0 * DE_PI * opd_nm * 1.0e-9;
    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * DE_PI * var) * cos(pos * phase + shift) * exp(-sq(phase) * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * DE_PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) *
             exp(-4.5282e+09 * sq(phase));
    xyz /= 1.0685e-7;
    return float3(3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
                  -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
                  0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);
}

/// The Fresnel reflectance (linear Rec.709) of a thin film over a base of F0 `base_f0` for
/// cos(theta) `mu` in the outside medium.
public float3 de_thin_film_fresnel(float outside_ior, float film_ior, float mu, float thickness_nm,
                                   float3 base_f0)
{
    // The film's index fades to the outside's as it gets thinner than 30 nm.
    float ior2 = lerp(outside_ior, film_ior, smoothstep(0.0, 30.0, thickness_nm));
    float sin2 = sq(outside_ior / ior2) * (1.0 - sq(mu));
    float cos2_sq = 1.0 - sin2;
    if (cos2_sq < 0.0)
        return float3(1.0, 1.0, 1.0); // total internal reflection
    float cos2 = sqrt(cos2_sq);
    // First interface.
    float r12 = schlick(ior_to_f0(ior2, outside_ior), mu);
    float t121 = 1.0 - r12;
    float phi12 = ior2 < outside_ior ? DE_PI : 0.0;
    float phi21 = DE_PI - phi12;
    // Second interface.
    float3 base_ior = f0_to_ior(clamp(base_f0, 0.0, 0.9999));
    float3 r23 = schlick3(ior_to_f03(base_ior, ior2), cos2);
    float3 phi23 = float3(base_ior.x < ior2 ? DE_PI : 0.0, base_ior.y < ior2 ? DE_PI : 0.0,
                          base_ior.z < ior2 ? DE_PI : 0.0);
    // Phase shift, compound terms.
    float opd = 2.0 * ior2 * thickness_nm * cos2;
    float3 phi = phi21 + phi23;
    float3 r123 = clamp(r12 * r23, 1e-5, 0.9999);
    float3 r123s = sqrt(r123);
    float3 rs = sq(t121) * r23 / (1.0 - r123);
    float3 i = r12 + rs; // m = 0
    float3 cm = rs - t121;
    for (uint m = 1; m <= 2u; ++m)
    {
        cm *= r123s;
        i += cm * 2.0 * de_thin_film_sensitivity(float(m) * opd, float(m) * phi);
    }
    return max(i, float3(0.0, 0.0, 0.0));
}
