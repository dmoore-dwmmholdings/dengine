// shaders/brdf/fresnel.slang: Fresnel reflectance (SPEC §12.6; DENG-298). Pure functions.
//
//   de_fresnel_schlick      Schlick's approximation between F0 (normal incidence) and F90
//   de_f0_from_ior          F0 of a dielectric interface from the two indices of refraction
//   de_fresnel_dielectric   the exact unpolarised dielectric Fresnel (total internal reflection
//                           gives 1); for transmission (DENG-301)
//   de_fresnel_f82_tint     metals: Schlick with a tint at 82 degrees (Hoffman 2019, "Fresnel
//                           Equations Considered Harmful"; the OpenPBR "F82-tint" form): the
//                           Schlick curve minus a * mu * (1 - mu)^6, with `a` chosen so the value
//                           at mu = 1/7 (about 82 degrees) is `tint` times Schlick's. tint = 1 is
//                           plain Schlick.
module fresnel;

import bsdf_common;

/// Schlick: f0 + (f90 - f0) (1 - cos)^5, cos clamped to [0, 1].
public float3 de_fresnel_schlick(float3 f0, float3 f90, float cos_theta)
{
    return f0 + (f90 - f0) * de_pow5(1.0 - saturate(cos_theta));
}
public float de_fresnel_schlick1(float f0, float f90, float cos_theta)
{
    return f0 + (f90 - f0) * de_pow5(1.0 - saturate(cos_theta));
}

/// ((ior - outside) / (ior + outside))^2: F0 of light arriving from a medium of index `outside`.
public float de_f0_from_ior(float ior, float outside)
{
    float r = (ior - outside) / (ior + outside);
    return r * r;
}

/// The exact Fresnel reflectance of a dielectric for cos(theta_i) = `cos_i` (> 0: arriving from
/// the outside) and relative index `eta` = inside / outside. Total internal reflection gives 1.
public float de_fresnel_dielectric(float cos_i, float eta)
{
    cos_i = saturate(cos_i);
    float sin2_t = (1.0 - cos_i * cos_i) / (eta * eta);
    if (sin2_t >= 1.0)
        return 1.0;
    float cos_t = sqrt(1.0 - sin2_t);
    float rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
    float rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
    return 0.5 * (rs * rs + rp * rp);
}

/// The F82-tint metal Fresnel: `f0` the colour at normal incidence, `tint` the factor on the
/// Schlick value at mu = 1/7. Clamped to [0, 1].
public float3 de_fresnel_f82_tint(float3 f0, float3 tint, float cos_theta)
{
    const float mu_bar = 1.0 / 7.0;
    const float denom = 0.056652780; // mu_bar (1 - mu_bar)^6 = (1/7)(6/7)^6
    float3 schlick_bar = de_fresnel_schlick(f0, float3(1.0, 1.0, 1.0), mu_bar);
    float3 a = schlick_bar * (1.0 - tint) / denom;
    float mu = saturate(cos_theta);
    float m = 1.0 - mu;
    float m6 = m * m * m;
    m6 *= m6;
    return saturate(de_fresnel_schlick(f0, float3(1.0, 1.0, 1.0), mu) - a * (mu * m6));
}
