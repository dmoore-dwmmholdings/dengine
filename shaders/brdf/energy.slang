// shaders/brdf/energy.slang: GGX energy compensation (SPEC §12.6; DENG-299). The single-scatter
// GGX lobe loses the energy of the light that bounces more than once between microfacets, up to
// half of it for rough metals. Multi-scatter compensation (Kulla and Conty 2017, "Revisiting
// Physically Based Shading at Imageworks") adds it back as a second, diffuse-like lobe:
//
//   f_ms(wo, wi) = F_ms (1 - E(mu_o)) (1 - E(mu_i)) / (pi (1 - E_avg))
//   F_ms         = F_avg^2 E_avg / (1 - F_avg (1 - E_avg)),  F_avg = F0 + (F90 - F0) / 21
//
// with E(mu) the directional albedo of the single-scatter lobe for F = 1 and E_avg its
// cosine-weighted average. The lobe is reciprocal, and with F = 1 the two lobes reflect all the
// light (the white furnace).
//
// The energy LUT (brdf_lut_gen, lut_gen.slang; src/render/materials/brdf_luts.c makes it at
// renderer start) is a DE_BRDF_LUT_SIZE^2 RGBA32F texture: x = mu = cos(theta_o) with texel
// centres at (i + 0.5) / size, y = perceptual roughness r with texel centres at j / (size - 1)
// (both ends exact: the roughest row is r = 1). Per texel:
//
//   R  A(mu, r)  = integral of f_ss / F (1 - Fc) cos      the split-sum DFG scale and bias: the
//   G  B(mu, r)  = integral of f_ss / F Fc cos            albedo with Schlick (F0, F90) is
//                  (Fc = (1 - wo.h)^5)                    F0 A + F90 B; E = A + B
//   B  A_avg(r) = 2 integral of A(mu) mu dmu            the averages (the same for every mu)
//   A  B_avg(r) = 2 integral of B(mu) mu dmu
//
// de_energy_lookup filters it bilinearly with Load (no sampler, no filterable-format requirement).
// Without a LUT (DE_BINDLESS_INVALID) it uses an analytic fit of A and B (Karis 2014) and a
// 4-point Gauss-Legendre average of it, so a pass that has no LUT still conserves energy roughly.
module energy;

import bindless;
import bsdf_common;

/// Texels per side of the energy LUT.
public static const uint DE_BRDF_LUT_SIZE = 32;
/// Smallest GGX alpha (perceptual roughness 0.0447): narrower lobes alias and overflow D.
public static const float DE_GGX_ALPHA_MIN = 0.002;

/// The GGX alpha of perceptual roughness `r`: r^2, at least DE_GGX_ALPHA_MIN.
public float de_ggx_alpha(float r)
{
    return max(r * r, DE_GGX_ALPHA_MIN);
}

/// One lookup of the energy LUT.
public struct EnergyTerms
{
    public float a;     // A(mu, r)
    public float b;     // B(mu, r)
    public float a_avg; // A_avg(r)
    public float b_avg; // B_avg(r)

    /// The single-scatter albedo for F = 1 at mu, and its average.
    public float e()
    {
        return a + b;
    }
    public float e_avg()
    {
        return a_avg + b_avg;
    }
};

/// The analytic fit of the split-sum DFG (Karis 2014, "Physically Based Shading on Mobile").
float2 karis_ab(float mu, float r)
{
    float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 q = r * c0 + c1;
    float a004 = min(q.x * q.x, exp2(-9.28 * mu)) * q.x + q.y;
    return saturate(float2(-1.04, 1.04) * a004 + q.zw);
}

/// The energy terms at cos(theta) `mu` and perceptual roughness `r` from LUT `lut` (bindless
/// texture index; DE_BINDLESS_INVALID: the analytic fallback).
public EnergyTerms de_energy_lookup(uint lut, float mu, float r)
{
    EnergyTerms t;
    mu = saturate(mu);
    r = saturate(r);
    if (lut == DE_BINDLESS_INVALID)
    {
        float2 ab = karis_ab(mu, r);
        t.a = ab.x;
        t.b = ab.y;
        // 2 integral of f(mu) mu dmu with 4-point Gauss-Legendre on [0, 1].
        static const float x[4] = {0.0694318442, 0.3300094782, 0.6699905218, 0.9305681558};
        static const float w[4] = {0.1739274226, 0.3260725774, 0.3260725774, 0.1739274226};
        float2 avg = float2(0.0, 0.0);
        for (uint k = 0; k < 4u; ++k)
            avg += w[k] * x[k] * karis_ab(x[k], r);
        t.a_avg = 2.0 * avg.x;
        t.b_avg = 2.0 * avg.y;
        return t;
    }
    float n = float(DE_BRDF_LUT_SIZE);
    float2 p = float2(mu * n - 0.5, r * (n - 1.0));
    p = clamp(p, float2(0.0, 0.0), float2(n - 1.0, n - 1.0));
    int2 i0 = int2(floor(p));
    int2 i1 = min(i0 + int2(1, 1), int2(int(DE_BRDF_LUT_SIZE) - 1, int(DE_BRDF_LUT_SIZE) - 1));
    float2 f = p - float2(i0);
    Texture2D<float4> tex = de_tex2d(lut);
    float4 v00 = tex.Load(int3(i0.x, i0.y, 0));
    float4 v10 = tex.Load(int3(i1.x, i0.y, 0));
    float4 v01 = tex.Load(int3(i0.x, i1.y, 0));
    float4 v11 = tex.Load(int3(i1.x, i1.y, 0));
    float4 v = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
    t.a = v.x;
    t.b = v.y;
    t.a_avg = v.z;
    t.b_avg = v.w;
    return t;
}

/// The cosine-weighted average of Schlick's Fresnel between f0 and f90: f0 + (f90 - f0) / 21.
public float3 de_fresnel_avg(float3 f0, float3 f90)
{
    return f0 + (f90 - f0) * (1.0 / 21.0);
}

/// Kulla-Conty's Fresnel of the multi-scatter lobe for average Fresnel `f_avg` and albedo average
/// `e_avg`.
public float3 de_energy_fms(float3 f_avg, float e_avg)
{
    return f_avg * f_avg * e_avg / (1.0 - f_avg * (1.0 - e_avg));
}

/// The multi-scatter lobe without the cosine: f_ms / F_ms = (1 - E_o)(1 - E_i) / (pi (1 - E_avg)).
public float de_energy_ms(float e_o, float e_i, float e_avg)
{
    return (1.0 - e_o) * (1.0 - e_i) / (DE_PI * max(1.0 - e_avg, 1e-4));
}

/// The directional albedo of the specular lobes (single plus multiple scattering) with Schlick
/// (f0, f90): f0 A + f90 B + F_ms (1 - E), and its average (f0 A_avg + f90 B_avg + F_ms
/// (1 - E_avg)) in `avg`.
public float3 de_energy_specular_albedo(EnergyTerms t, float3 f0, float3 f90, bool multiscatter,
                                        out float3 avg)
{
    float3 e = f0 * t.a + f90 * t.b;
    avg = f0 * t.a_avg + f90 * t.b_avg;
    if (multiscatter)
    {
        float3 fms = de_energy_fms(de_fresnel_avg(f0, f90), t.e_avg());
        e += fms * (1.0 - t.e());
        avg += fms * (1.0 - t.e_avg());
    }
    return e;
}
