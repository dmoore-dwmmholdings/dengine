// shaders/materials/standard/standard_base.slang: the base lobes of the Standard material model
// (SPEC §12.6; DENG-299). standard.slang composes them with the layers (DENG-300) and the
// transmission lobes (DENG-301); those tasks add files of their own next to this one.
//
// glTF 2.0 metallic-roughness with KHR_materials_ior and KHR_materials_specular:
//
//   dielectric  F0 = min(((ior - 1) / (ior + 1))^2 x specular_color, 1) x specular, F90 = specular
//   metal       F0 = base colour, F90 = 1
//   blend       F0 and F90 interpolated by metallic (the Schlick Fresnel is linear in both, so this
//               is glTF's mix of the dielectric and the metal BRDF for the single-scatter lobe)
//
// Lobes (directions in the local shading frame, Z = the normal; values without the cosine):
//
//   specular   GGX single scatter: F(wo.h) D G2 / (4 mu_o mu_i), alpha = max(r^2, 0.002)
//   multi      Kulla-Conty multiple scattering (brdf/energy.slang): F_ms (1 - E_o)(1 - E_i) /
//              (pi (1 - E_avg)), so rough metals keep their energy (the white furnace gives 1)
//   diffuse    Lambert x base colour x (1 - metallic), coupled to the dielectric specular albedo
//              Ed: (1 - Ed(mu_o))(1 - Ed(mu_i)) / (1 - Ed_avg). What the dielectric's specular
//              reflects is taken from the diffuse, so a white dielectric reflects at most 1; the
//              form is reciprocal (the path tracer needs that).
//
// Sampling picks a lobe by its albedo at wo (Fresnel-weighted, from the energy LUT): the visible
// normals of the GGX lobe (VNDF) for the single-scatter specular, the cosine hemisphere for the
// multi-scatter and diffuse lobes. pdf is the mixture of both densities, so sample, pdf and
// evaluate agree (MIS-ready).
module standard_base;

import bindless;
import "../../brdf/bsdf_common";
import "../../brdf/ggx";
import "../../brdf/fresnel";
import "../../brdf/sampling";
import "../../brdf/energy";
import "../material_interface";

/// Rec. 709 luminance, to weigh coloured albedos for lobe selection.
float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// F0 of a dielectric of index `ior` against air (KHR_materials_ior; IOR below 1 is clamped).
public float de_standard_f0_ior(float ior)
{
    return de_f0_from_ior(max(ior, 1.0), 1.0);
}

/// The Fresnel parameters of a Standard surface.
public struct StandardFresnel
{
    public float3 f0_d;  // dielectric F0: min(F0(ior) x specular_color, 1) x specular
    public float3 f90_d; // dielectric F90: specular
    public float3 f0;    // blended with the metal's (base colour) by metallic
    public float3 f90;   // blended with the metal's (1) by metallic
};

public StandardFresnel de_standard_fresnel(SurfaceData s)
{
    StandardFresnel f;
    f.f0_d = min(de_standard_f0_ior(s.ior) * s.specular_color, float3(1.0, 1.0, 1.0)) * s.specular;
    f.f90_d = float3(s.specular, s.specular, s.specular);
    f.f0 = lerp(f.f0_d, s.base_color, s.metallic);
    f.f90 = lerp(f.f90_d, float3(1.0, 1.0, 1.0), s.metallic);
    return f;
}

/// What evaluate, sample and pdf share for one surface and view direction.
public struct StandardBase
{
    public float roughness;
    public float alpha;
    public StandardFresnel fresnel;
    public EnergyTerms e_o;   // the energy LUT at mu_o
    public bool multiscatter; // the Kulla-Conty lobe is on
    public float3 fms;        // its Fresnel (blended F0, F90)
    public float3 ed_o;       // the dielectric specular albedo at mu_o (the diffuse coupling)
    public float3 ed_avg;     // and its average
    public float3 diffuse;    // base colour x (1 - metallic) / pi
    public float p_specular;  // the probability that sample picks the VNDF lobe
};

/// The shared terms for surface `s` seen from local `wo` (wo.z > 0). `multiscatter` false
/// leaves out the compensation lobe (tests show what it adds).
public StandardBase de_standard_base(SurfaceData s, float3 wo, bool multiscatter)
{
    StandardBase b;
    b.roughness = saturate(s.roughness);
    b.alpha = de_ggx_alpha(b.roughness);
    b.fresnel = de_standard_fresnel(s);
    b.e_o = de_energy_lookup(s.brdf_lut, wo.z, b.roughness);
    b.multiscatter = multiscatter;
    b.fms = multiscatter ? de_energy_fms(de_fresnel_avg(b.fresnel.f0, b.fresnel.f90), b.e_o.e_avg())
                         : float3(0.0, 0.0, 0.0);
    b.ed_o = de_energy_specular_albedo(b.e_o, b.fresnel.f0_d, b.fresnel.f90_d, multiscatter,
                                       b.ed_avg);
    b.diffuse = s.base_color * ((1.0 - saturate(s.metallic)) * DE_INV_PI);
    // Lobe albedos at wo: single-scatter specular, multi-scatter, diffuse.
    float ss = luminance(b.fresnel.f0 * b.e_o.a + b.fresnel.f90 * b.e_o.b);
    float ms = luminance(b.fms) * (1.0 - b.e_o.e());
    float df = luminance(b.diffuse * DE_PI * (1.0 - b.ed_o));
    float total = ss + ms + df;
    b.p_specular = total > 0.0 ? saturate(ss / total) : 1.0;
    return b;
}

/// BSDF x cos(theta_i) of the base lobes for local `wo`, `wi` (0 unless both are above the
/// surface).
public float3 de_standard_base_evaluate(StandardBase b, SurfaceData s, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return float3(0.0, 0.0, 0.0);
    float3 h = normalize(wo + wi);
    float3 f = de_fresnel_schlick(b.fresnel.f0, b.fresnel.f90, dot(wo, h));
    float3 r = f * de_ggx_specular(b.alpha, b.alpha, wo, wi);
    EnergyTerms e_i = de_energy_lookup(s.brdf_lut, wi.z, b.roughness);
    if (b.multiscatter)
        r += b.fms * de_energy_ms(b.e_o.e(), e_i.e(), b.e_o.e_avg());
    float3 ed_avg_i;
    float3 ed_i = de_energy_specular_albedo(e_i, b.fresnel.f0_d, b.fresnel.f90_d, b.multiscatter,
                                            ed_avg_i);
    float3 coupling = (1.0 - b.ed_o) * (1.0 - ed_i) / max(1.0 - b.ed_avg, float3(1e-4, 1e-4, 1e-4));
    r += b.diffuse * coupling;
    return r * wi.z;
}

/// The density with which de_standard_base_sample returns `wi` for `wo`, over the whole sphere: a
/// VNDF sample can reflect below the horizon (evaluate is 0 there), so the density integrates to 1
/// over the sphere, not over the upper hemisphere.
public float de_standard_base_pdf(StandardBase b, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0))
        return 0.0;
    return b.p_specular * de_ggx_reflect_pdf(b.alpha, b.alpha, wo, wi) +
           (1.0 - b.p_specular) * de_cosine_hemisphere_pdf(wi.z);
}

/// A direction for `wo` from uniform `u` (u.z picks the lobe), with its weight evaluate / pdf.
public BsdfSample de_standard_base_sample(StandardBase b, SurfaceData s, float3 wo, float3 u)
{
    BsdfSample r;
    r.wi = float3(0.0, 0.0, 1.0);
    r.weight = float3(0.0, 0.0, 0.0);
    r.pdf = 0.0;
    r.lobe = 0u;
    if (!(wo.z > 0.0))
        return r;
    if (u.z < b.p_specular)
    {
        r.wi = de_reflect(wo, de_ggx_vndf_sample(b.alpha, b.alpha, wo, u.xy));
        r.lobe = DE_BSDF_LOBE_SPECULAR;
    }
    else
    {
        r.wi = de_sample_cosine_hemisphere(u.xy);
        r.lobe = DE_BSDF_LOBE_DIFFUSE;
    }
    r.pdf = de_standard_base_pdf(b, wo, r.wi);
    if (r.pdf > 0.0)
        r.weight = de_standard_base_evaluate(b, s, wo, r.wi) / r.pdf;
    return r;
}
