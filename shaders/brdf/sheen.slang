// shaders/brdf/sheen.slang: the sheen lobe of KHR_materials_sheen (SPEC §12.6; DENG-300). Pure
// functions; local shading frame (Z = the normal).
//
//   de_sheen_d          Charlie distribution (Estevez and Kulla 2017, "Production Friendly
//                       Microfacet Sheen BRDF"): (2 + 1/a) sin(theta_h)^(1/a) / (2 pi), a = r^2
//   de_sheen_v          its visibility: 1 / ((1 + L(mu_o) + L(mu_i)) 4 mu_o mu_i), with the
//                       paper's fitted L (the KHR sample viewer's form), clamped to 1
//   de_sheen_brdf       D V (times sheen_color by the caller); symmetric in the two directions
//   de_sheen_albedo     its directional albedo E(mu, r) (sheen_color 1) from rows
//                       DE_BRDF_LUT_SIZE .. 2 DE_BRDF_LUT_SIZE - 1 of the energy LUT (energy.slang)
//
// The KHR layering scales what is under the sheen by min(1 - max(sheen_color) E(mu_o),
// 1 - max(sheen_color) E(mu_i)) (energy preserving and reciprocal).
module sheen;

import bindless;
import bsdf_common;
import energy;

/// Smallest sheen roughness: below it the Charlie lobe is a sliver at grazing that no table holds.
public static const float DE_SHEEN_ROUGHNESS_MIN = 0.07;

/// The clamped sheen roughness.
public float de_sheen_roughness(float r)
{
    return clamp(r, DE_SHEEN_ROUGHNESS_MIN, 1.0);
}

/// Charlie D for cos(theta_h) = `cos_h`, sheen roughness `r` (clamped by the caller).
public float de_sheen_d(float r, float cos_h)
{
    float a = r * r;
    float inv = 1.0 / a;
    float sin2 = max(1.0 - cos_h * cos_h, 0.0);
    return (2.0 + inv) * pow(sin2, inv * 0.5) / (2.0 * DE_PI);
}

float sheen_l(float x, float a)
{
    float t = (1.0 - a) * (1.0 - a);
    float pa = lerp(21.5473, 25.3245, t);
    float pb = lerp(3.82987, 3.32435, t);
    float pc = lerp(0.19823, 0.16801, t);
    float pd = lerp(-1.97760, -1.27393, t);
    float pe = lerp(-4.32054, -4.85967, t);
    return pa / (1.0 + pb * pow(x, pc)) + pd * x + pe;
}

/// The sheen Lambda of cos(theta) `mu` (Estevez and Kulla's fit, made symmetric about 0.5).
public float de_sheen_lambda(float mu, float a)
{
    return mu < 0.5 ? exp(sheen_l(mu, a)) : exp(2.0 * sheen_l(0.5, a) - sheen_l(1.0 - mu, a));
}

/// The sheen visibility term (clamped to [0, 1]).
public float de_sheen_v(float r, float mu_o, float mu_i)
{
    float a = r * r;
    float l = 1.0 + de_sheen_lambda(mu_o, a) + de_sheen_lambda(mu_i, a);
    return saturate(1.0 / (l * (4.0 * mu_o * mu_i)));
}

/// The sheen BRDF for sheen colour 1 (no cosine), 0 unless both directions are above.
public float de_sheen_brdf(float r, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return 0.0;
    float3 h = normalize(wo + wi);
    return de_sheen_d(r, h.z) * de_sheen_v(r, wo.z, wi.z);
}

/// The sheen's directional albedo for sheen colour 1 at cos(theta) `mu`, from LUT `lut` (rows
/// r = DE_SHEEN_ROUGHNESS_MIN .. 1, both ends exact; DE_BINDLESS_INVALID: a coarse fit).
public float de_sheen_albedo(uint lut, float mu, float r)
{
    mu = saturate(mu);
    r = de_sheen_roughness(r);
    if (lut == DE_BINDLESS_INVALID)
    {
        // A coarse fit of the table: E rises with roughness and towards grazing angles.
        float g = 1.0 - mu;
        return saturate(r * (0.18 + 0.30 * g * g * g));
    }
    float n = float(DE_BRDF_LUT_SIZE);
    float2 p = float2(mu * n - 0.5, (r - DE_SHEEN_ROUGHNESS_MIN) / (1.0 - DE_SHEEN_ROUGHNESS_MIN) *
                                        (n - 1.0));
    p = clamp(p, float2(0.0, 0.0), float2(n - 1.0, n - 1.0));
    int2 i0 = int2(floor(p));
    int2 i1 = min(i0 + int2(1, 1), int2(int(DE_BRDF_LUT_SIZE) - 1, int(DE_BRDF_LUT_SIZE) - 1));
    float2 f = p - float2(i0);
    int row = int(DE_BRDF_LUT_SIZE); // the sheen rows follow the energy rows
    Texture2D<float4> tex = de_tex2d(lut);
    float e00 = tex.Load(int3(i0.x, i0.y + row, 0)).x;
    float e10 = tex.Load(int3(i1.x, i0.y + row, 0)).x;
    float e01 = tex.Load(int3(i0.x, i1.y + row, 0)).x;
    float e11 = tex.Load(int3(i1.x, i1.y + row, 0)).x;
    return lerp(lerp(e00, e10, f.x), lerp(e01, e11, f.x), f.y);
}
