// shaders/materials/standard/lobe_transmission.slang: the transmission lobes of the Standard model
// (SPEC §12.6; DENG-301). standard.slang composes them at insertion point T, under the sheen and
// the coat's attenuation along the view:
//
//   dielectric = fresnel_mix(specular_brdf,
//                            mix(mix(diffuse_brdf, diffuse_btdf, dt), specular_btdf, t))
//
// as the KHR texts write it (KHR_materials_transmission, KHR_materials_diffuse_transmission),
// everything x (1 - metallic):
//
//   specular_btdf  rough refraction (Walter et al. 2007, "Microfacet Models for Refraction
//                  through Rough Surfaces") into a medium of index `ior`, (1 - F) with the base's
//                  KHR Schlick Fresnel, tinted by the base colour; sampled by refracting a visible
//                  normal. Without the 1 / eta^2 radiance compression (as the KHR reference): the
//                  interface passes on (1 - F) of the light both ways, so glass seen through one
//                  interface stays bright, and a path in and out of a volume is unchanged.
//                  Single scattering: like GGX without compensation it loses light at high
//                  roughness. Thin-walled surfaces (thickness 0, KHR_materials_volume absent) and
//                  IOR 1 (refraction a delta there) use the KHR reference's thin form instead:
//                  the reflection lobe mirrored through the surface, so light passes straight
//                  through, blurred by the roughness.
//   diffuse_btdf   Lambert below the surface x diffuse_transmission_color, with the diffuse
//                  reflection's coupling to the specular albedo (1 - Ed(mu_o))(1 - Ed(|mu_i|)) /
//                  (1 - Ed_avg), so reflection and transmission share the diffuse energy.
//
// de_interface_inside_* is the dielectric interface seen from inside a volume (wo.z < 0): the
// path tracer's and RT refraction's exit. It reflects back inside and refracts out with the exact
// dielectric Fresnel, so past the critical angle it transmits nothing and reflects everything.
module lobe_transmission;

import bindless;
import "../../brdf/bsdf_common";
import "../../brdf/ggx";
import "../../brdf/fresnel";
import "../../brdf/sampling";
import "../../brdf/energy";
import "../material_interface";
import "../../scene/scene_types";
import "../../visbuffer/resolve_model";
import standard_base;
import volume;

/// de_cooked_tex_slot values of the transmission inputs.
public static const uint DE_STD_TEX_TRANSMISSION = 12;               // R
public static const uint DE_STD_TEX_THICKNESS = 13;                  // G
public static const uint DE_STD_TEX_DIFFUSE_TRANSMISSION = 14;       // A
public static const uint DE_STD_TEX_DIFFUSE_TRANSMISSION_COLOR = 15; // sRGB colour

/// Sets the transmission inputs of `s` from the material record alone (lighting): transmission
/// (DeGpuMaterial.clearcoat.w), thickness (extra.x, object space; no instance scale here),
/// attenuation (volume), diffuse transmission (extra.y; its colour white), and the TRANSMISSION
/// + FORWARD, VOLUME and DIFFUSE_TRANSMISSION features.
public void de_transmission_from_material(inout SurfaceData s, DeGpuMaterial m)
{
    s.transmission = saturate(m.clearcoat.w);
    s.thickness = max(m.extra.x, 0.0);
    s.attenuation_color = m.volume.rgb;
    s.attenuation_distance = m.volume.w;
    s.diffuse_transmission = saturate(m.extra.y);
    s.diffuse_transmission_color = float3(1.0, 1.0, 1.0);
    de_transmission_set_features(s);
}

/// The feature bits of the transmission inputs of `s`: TRANSMISSION and FORWARD (the forward
/// pass draws it, DENG-305) for specular transmission, VOLUME for a thickness, DIFFUSE_TRANSMISSION
/// for diffuse transmission. FORWARD is only set here (alpha blending sets it too).
public void de_transmission_set_features(inout SurfaceData s)
{
    s.features &= ~(DE_MATERIAL_FEATURE_TRANSMISSION | DE_MATERIAL_FEATURE_VOLUME |
                    DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION);
    if (s.transmission > 0.0)
        s.features |= DE_MATERIAL_FEATURE_TRANSMISSION | DE_MATERIAL_FEATURE_FORWARD;
    if (s.thickness > 0.0 && (s.transmission > 0.0 || s.diffuse_transmission > 0.0))
        s.features |= DE_MATERIAL_FEATURE_VOLUME;
    if (s.diffuse_transmission > 0.0)
        s.features |= DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION;
}

/// The same with the textures (resolve); the thickness is scaled to world units by the instance's
/// uniform-scale estimate (VertexAttribs.object_scale).
public void de_transmission_resolve(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v,
                                    TextureLodInfo lod)
{
    s.transmission = saturate(m.clearcoat.w);
    s.thickness = max(m.extra.x, 0.0);
    s.attenuation_color = m.volume.rgb;
    s.attenuation_distance = m.volume.w;
    s.diffuse_transmission = saturate(m.extra.y);
    s.diffuse_transmission_color = float3(1.0, 1.0, 1.0);
    float4 white = float4(1.0, 1.0, 1.0, 1.0);
    if (s.transmission > 0.0)
        s.transmission *= de_resolve_sample(m, DE_STD_TEX_TRANSMISSION, v, lod, white).r;
    if (s.thickness > 0.0)
        s.thickness *= de_resolve_sample(m, DE_STD_TEX_THICKNESS, v, lod, white).g * v.object_scale;
    if (s.diffuse_transmission > 0.0)
    {
        s.diffuse_transmission *= de_resolve_sample(m, DE_STD_TEX_DIFFUSE_TRANSMISSION, v, lod,
                                                    white).a;
        s.diffuse_transmission_color =
            de_resolve_sample(m, DE_STD_TEX_DIFFUSE_TRANSMISSION_COLOR, v, lod, white).rgb;
    }
    de_transmission_set_features(s);
}

/// True when the surface's specular transmission refracts (a volume with an IOR other than 1);
/// otherwise it is thin-walled.
public bool de_transmission_refracts(SurfaceData s)
{
    return (s.features & DE_MATERIAL_FEATURE_VOLUME) != 0u && s.thickness > 0.0 &&
           abs(s.ior - 1.0) > 1e-3;
}

/// What evaluate, sample and pdf share for the transmission lobes of one surface and view.
public struct TransmissionLobes
{
    public float t;        // specular transmission weight: transmission x (1 - metallic)
    public float dt;       // diffuse transmission weight: (1 - t) x dt x (1 - metallic)
    public float keep;     // what the reflected diffuse keeps: (1 - transmission)(1 - dt)
    public float3 tint;    // the specular BTDF's tint: the base colour
    public float3 dt_color;
    public bool refracts;  // volumetric refraction; else thin-walled
    public float eta;      // the IOR (inside / outside)
    public float alpha;    // the base's GGX alpha
    public float3 f0, f90; // the dielectric Fresnel
    public float3 ed_o, ed_avg; // the diffuse coupling (the base's)
    public bool multiscatter;
    public uint lut;
    public float roughness;
    public float albedo_t, albedo_dt; // the lobes' albedos at wo (luminance), for sampling
};

float luminance3(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/// The transmission lobes of `s` seen from local `wo` (wo.z > 0), with the base's terms `b`.
/// `has_t` / `has_dt`: which lobes the surface has (the caller checks the feature bits).
public TransmissionLobes de_transmission_lobes(SurfaceData s, StandardBase b, float3 wo,
                                               bool has_t, bool has_dt)
{
    TransmissionLobes l;
    float m = saturate(s.metallic);
    float t = has_t ? saturate(s.transmission) : 0.0;
    float dt = has_dt ? saturate(s.diffuse_transmission) : 0.0;
    l.t = t * (1.0 - m);
    l.dt = (1.0 - t) * dt * (1.0 - m);
    l.keep = (1.0 - t) * (1.0 - dt);
    l.tint = s.base_color;
    l.dt_color = s.diffuse_transmission_color;
    l.refracts = de_transmission_refracts(s);
    l.eta = max(s.ior, 1.0);
    l.alpha = b.alpha;
    l.f0 = b.fresnel.f0_d;
    l.f90 = b.fresnel.f90_d;
    l.ed_o = b.ed_o;
    l.ed_avg = b.ed_avg;
    l.multiscatter = b.multiscatter;
    l.lut = s.brdf_lut;
    l.roughness = b.roughness;
    // Albedo estimates: the specular BTDF transmits what the interface does not reflect.
    float r_o = luminance3(l.f0 * b.e_o.a + l.f90 * b.e_o.b);
    l.albedo_t = l.t * luminance3(l.tint) * saturate(b.e_o.e() - r_o);
    l.albedo_dt = l.dt * luminance3(l.dt_color * (1.0 - l.ed_o));
    return l;
}

/// The mirror image of `w` through the surface (z negated).
float3 mirror(float3 w)
{
    return float3(w.x, w.y, -w.z);
}

/// The specular BTDF x |cos(theta_i)| for `wo` above and `wi` below the surface (no weight).
public float3 de_transmission_specular(TransmissionLobes l, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z < 0.0))
        return float3(0.0, 0.0, 0.0);
    if (!l.refracts)
    {
        // Thin-walled: the reflection lobe mirrored through the surface.
        float3 wm = mirror(wi);
        float3 h = normalize(wo + wm);
        float3 f = de_fresnel_schlick(l.f0, l.f90, dot(wo, h));
        return (1.0 - f) * l.tint * (de_ggx_specular(l.alpha, l.alpha, wo, wm) * wm.z);
    }
    float3 h = wo + wi * l.eta;
    float hl = length(h);
    if (!(hl > 0.0))
        return float3(0.0, 0.0, 0.0);
    h /= hl;
    if (h.z < 0.0)
        h = -h;
    float wo_h = dot(wo, h), wi_h = dot(wi, h);
    if (!(wo_h > 0.0) || !(wi_h < 0.0))
        return float3(0.0, 0.0, 0.0);
    float denom = wo_h + l.eta * wi_h;
    float3 f = de_fresnel_schlick(l.f0, l.f90, wo_h);
    float d = de_ggx_d(l.alpha, h.z);
    float g = de_ggx_g2(l.alpha, l.alpha, wo, wi);
    // Without the 1 / eta^2 radiance compression (see the header): what the interface does not
    // reflect goes through, so a raster pass that sees one interface keeps glass bright, and a
    // path through a closed volume (in, then out) is the same either way.
    float bt = d * g * l.eta * l.eta * abs(wi_h) * wo_h / (wo.z * abs(wi.z) * denom * denom);
    return (1.0 - f) * l.tint * (bt * abs(wi.z));
}

/// The density of de_transmission_specular_sample, over the whole sphere (like the reflection
/// lobe's: a sample can land on the wrong side of the surface, where evaluate is 0).
public float de_transmission_specular_pdf(TransmissionLobes l, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0))
        return 0.0;
    if (!l.refracts)
        return de_ggx_reflect_pdf(l.alpha, l.alpha, wo, mirror(wi));
    float3 h = wo + wi * l.eta;
    float hl = length(h);
    if (!(hl > 0.0))
        return 0.0;
    h /= hl;
    if (h.z < 0.0)
        h = -h;
    float wo_h = dot(wo, h), wi_h = dot(wi, h);
    if (!(wo_h > 0.0) || !(wi_h < 0.0))
        return 0.0;
    float denom = wo_h + l.eta * wi_h;
    return de_ggx_vndf_pdf(l.alpha, l.alpha, wo, h) * l.eta * l.eta * abs(wi_h) / (denom * denom);
}

/// A direction for `wo` from uniform `u`, normally below the surface (a rough lobe can send it
/// above, where evaluate is 0); false when there is none.
public bool de_transmission_specular_sample(TransmissionLobes l, float3 wo, float2 u, out float3 wi)
{
    float3 h = de_ggx_vndf_sample(l.alpha, l.alpha, wo, u);
    if (!l.refracts)
    {
        wi = mirror(de_reflect(wo, h));
        return true;
    }
    return de_refract(wo, h, l.eta, wi);
}

/// The diffuse BTDF x |cos(theta_i)| for `wo` above and `wi` below the surface (no weight).
public float3 de_transmission_diffuse(TransmissionLobes l, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z < 0.0))
        return float3(0.0, 0.0, 0.0);
    EnergyTerms e_i = de_energy_lookup(l.lut, -wi.z, l.roughness);
    float3 avg_i;
    float3 ed_i = de_energy_specular_albedo(e_i, l.f0, l.f90, l.multiscatter, avg_i);
    float3 coupling = (1.0 - l.ed_o) * (1.0 - ed_i) / max(1.0 - l.ed_avg, float3(1e-4, 1e-4, 1e-4));
    return l.dt_color * coupling * (DE_INV_PI * -wi.z);
}

/// Both transmission lobes with their weights, x |cos(theta_i)|; `diffuse` the diffuse one.
public float3 de_transmission_evaluate(TransmissionLobes l, float3 wo, float3 wi,
                                       out float3 diffuse)
{
    diffuse = l.dt > 0.0 ? l.dt * de_transmission_diffuse(l, wo, wi) : float3(0.0, 0.0, 0.0);
    float3 spec = l.t > 0.0 ? l.t * de_transmission_specular(l, wo, wi) : float3(0.0, 0.0, 0.0);
    return spec + diffuse;
}

/// The diffuse BTDF's sampling density (the cosine hemisphere below the surface).
public float de_transmission_diffuse_pdf(float3 wi)
{
    return de_cosine_hemisphere_pdf(-wi.z);
}

/// A cosine-distributed direction below the surface.
public float3 de_transmission_diffuse_sample(float2 u)
{
    return mirror(de_sample_cosine_hemisphere(u));
}

/* ---------- The interface seen from inside a volume ---------- */

/// BSDF x |cos(theta_i)| of the dielectric interface for `wo` inside the volume (wo.z < 0):
/// reflection back inside (wi.z < 0) and refraction out (wi.z > 0), exact Fresnel with relative
/// index 1 / ior (total internal reflection past the critical angle).
public float3 de_interface_inside_evaluate(float alpha, float ior, float3 wo, float3 wi)
{
    float3 o = mirror(wo), i = mirror(wi); // the inside's frame: o.z > 0
    if (!(o.z > 0.0))
        return float3(0.0, 0.0, 0.0);
    float eta = 1.0 / max(ior, 1.0);
    if (i.z > 0.0)
    {
        float3 h = normalize(o + i);
        float f = de_fresnel_dielectric(dot(o, h), eta);
        float r = f * de_ggx_specular(alpha, alpha, o, i) * i.z;
        return float3(r, r, r);
    }
    float3 h = o + i * eta;
    float hl = length(h);
    if (!(hl > 0.0))
        return float3(0.0, 0.0, 0.0);
    h /= hl;
    if (h.z < 0.0)
        h = -h;
    float o_h = dot(o, h), i_h = dot(i, h);
    if (!(o_h > 0.0) || !(i_h < 0.0))
        return float3(0.0, 0.0, 0.0);
    float denom = o_h + eta * i_h;
    float f = de_fresnel_dielectric(o_h, eta);
    float bt = de_ggx_d(alpha, h.z) * de_ggx_g2(alpha, alpha, o, i) * eta * eta * abs(i_h) * o_h /
               (o.z * abs(i.z) * denom * denom);
    float r = (1.0 - f) * bt * abs(i.z);
    return float3(r, r, r);
}

/// The density of de_interface_inside_sample.
public float de_interface_inside_pdf(float alpha, float ior, float3 wo, float3 wi)
{
    float3 o = mirror(wo), i = mirror(wi);
    if (!(o.z > 0.0))
        return 0.0;
    float eta = 1.0 / max(ior, 1.0);
    if (i.z > 0.0)
    {
        float3 h = normalize(o + i);
        float o_h = dot(o, h);
        if (!(o_h > 0.0))
            return 0.0;
        return de_fresnel_dielectric(o_h, eta) * de_ggx_vndf_pdf(alpha, alpha, o, h) / (4.0 * o_h);
    }
    float3 h = o + i * eta;
    float hl = length(h);
    if (!(hl > 0.0))
        return 0.0;
    h /= hl;
    if (h.z < 0.0)
        h = -h;
    float o_h = dot(o, h), i_h = dot(i, h);
    if (!(o_h > 0.0) || !(i_h < 0.0))
        return 0.0;
    float denom = o_h + eta * i_h;
    return (1.0 - de_fresnel_dielectric(o_h, eta)) * de_ggx_vndf_pdf(alpha, alpha, o, h) * eta *
           eta * abs(i_h) / (denom * denom);
}

/// A direction for `wo` inside the volume: a visible normal, then reflection with probability F
/// (u.z) and refraction otherwise.
public BsdfSample de_interface_inside_sample(float alpha, float ior, float3 wo, float3 u)
{
    BsdfSample r;
    r.wi = float3(0.0, 0.0, -1.0);
    r.weight = float3(0.0, 0.0, 0.0);
    r.pdf = 0.0;
    r.lobe = DE_BSDF_LOBE_SPECULAR;
    float3 o = mirror(wo);
    if (!(o.z > 0.0))
        return r;
    float eta = 1.0 / max(ior, 1.0);
    float3 h = de_ggx_vndf_sample(alpha, alpha, o, u.xy);
    float f = de_fresnel_dielectric(dot(o, h), eta);
    float3 i;
    if (u.z < f || !de_refract(o, h, eta, i))
        i = de_reflect(o, h);
    else
        r.lobe = DE_BSDF_LOBE_TRANSMISSION;
    r.wi = mirror(i);
    r.pdf = de_interface_inside_pdf(alpha, ior, wo, r.wi);
    if (r.pdf > 0.0)
        r.weight = de_interface_inside_evaluate(alpha, ior, wo, r.wi) / r.pdf;
    return r;
}
