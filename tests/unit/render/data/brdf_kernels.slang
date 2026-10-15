// tests/unit/render/data/brdf_kernels.slang: test_brdf.c's GPU side (DENG-298). Evaluates the
// functions of shaders/brdf/ and a reciprocal test BSDF built from them (the S13 placeholder
// Standard; DENG-299 replaced the model itself) for a list of cases the test wrote, so the test
// can compare them with tests/unit/render/brdf_ref.c.
// Cooked at test time with the engine's shaders/ directory and this one on one asset root.
import bindless;
import push_constants;
import "brdf/bsdf_common";
import "brdf/ggx";
import "brdf/fresnel";
import "brdf/diffuse";
import "brdf/sampling";

/// Floats each case writes (test_brdf.c BK_OUT).
static const uint BK_OUT = 24;

struct BrdfParams
{
    uint cases; // float4 x 4 per case (test_brdf.c bk_case)
    uint out_;  // BK_OUT floats per case
    uint count;
    uint reserved;
};

/// The reciprocal test BSDF (the S13 placeholder Standard, brdf_ref.c brdf_standard_*) for a grey
/// surface: GGX with Schlick Fresnel (F0 = 0.04 x specular for dielectrics, the base colour for
/// metals) plus Lambert weighted by (1 - F)(1 - metallic); x cos(theta_i).
float standard_eval(float base, float roughness, float metallic, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return 0.0;
    float a = max(roughness * roughness, 1e-3);
    float3 h = normalize(wo + wi);
    float f0 = lerp(0.04, base, metallic);
    float f = de_fresnel_schlick1(f0, 1.0, dot(wo, h));
    float spec = f * de_ggx_specular(a, a, wo, wi);
    float diff = (1.0 - f) * ((1.0 - metallic) * DE_INV_PI) * base;
    return (spec + diff) * wi.z;
}

/// Its sampling density: VNDF with probability 0.5 + 0.5 metallic, else the cosine hemisphere.
float standard_pdf(float roughness, float metallic, float3 wo, float3 wi)
{
    if (!(wo.z > 0.0) || !(wi.z > 0.0))
        return 0.0;
    float a = max(roughness * roughness, 1e-3);
    float ps = 0.5 + 0.5 * metallic;
    return ps * de_ggx_reflect_pdf(a, a, wo, wi) + (1.0 - ps) * de_cosine_hemisphere_pdf(wi.z);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_brdf(uint3 id : SV_DispatchThreadID, uniform BrdfParams pc)
{
    uint i = id.x;
    if (i >= pc.count)
        return;
    float4 a = de_robuffer<float4>(pc.cases)[i * 4u + 0u];  // ax, ay, u0, u1
    float4 o = de_robuffer<float4>(pc.cases)[i * 4u + 1u];  // wo xyz, roughness
    float4 n = de_robuffer<float4>(pc.cases)[i * 4u + 2u];  // wi xyz, metallic
    float4 fr = de_robuffer<float4>(pc.cases)[i * 4u + 3u]; // cos, f0, tint / eta, base
    float ax = a.x, ay = a.y;
    float3 wo = o.xyz, wi = n.xyz;
    float3 h = normalize(wo + wi);
    float v[BK_OUT];
    // D of a cosine given bit-identically to both sides (fr.x): near a narrow lobe's peak D(cos)
    // amplifies an ulp of cos computed from normalize(wo + wi) beyond 1e-4.
    v[0] = de_ggx_d(ax, fr.x);
    v[1] = de_ggx_d_aniso(ax, ay, h);
    v[2] = de_ggx_g1(ax, ay, wo);
    v[3] = de_ggx_g2(ax, ay, wo, wi);
    v[4] = de_fresnel_schlick1(fr.y, 1.0, fr.x);
    v[5] = de_fresnel_f82_tint(float3(fr.y, fr.y, fr.y), float3(fr.z, fr.z, fr.z), fr.x).x;
    v[6] = de_fresnel_dielectric(fr.x, 1.0 + fr.z);
    v[7] = de_f0_from_ior(1.0 + fr.z, 1.0);
    v[8] = de_diffuse_lambert();
    v[9] = de_diffuse_burley(o.w, wo.z, wi.z, dot(wi, h));
    float3 hs = de_ggx_vndf_sample(ax, ay, wo, a.zw);
    v[10] = hs.x;
    v[11] = hs.y;
    v[12] = hs.z;
    v[13] = de_ggx_vndf_pdf(ax, ay, wo, hs);
    v[14] = de_ggx_reflect_pdf(ax, ay, wo, wi);
    v[15] = standard_eval(fr.w, o.w, n.w, wo, wi);
    v[16] = standard_eval(fr.w, o.w, n.w, wi, wo);
    v[17] = standard_pdf(o.w, n.w, wo, wi);
    float3 c = de_sample_cosine_hemisphere(a.zw);
    v[18] = c.x;
    v[19] = c.y;
    v[20] = c.z;
    v[21] = de_radical_inverse2(i * 2654435761u);
    v[22] = de_ggx_specular(ax, ay, wo, wi);
    // The albedo estimator's weight of this case's VNDF sample (F = 1): f cos / pdf.
    float3 ws = de_reflect(wo, hs);
    float ps = de_ggx_reflect_pdf(ax, ay, wo, ws);
    v[23] = ws.z > 0.0 && ps > 0.0 ? de_ggx_specular(ax, ay, wo, ws) * ws.z / ps : 0.0;
    for (uint k = 0; k < BK_OUT; ++k)
        de_buffer<float>(pc.out_)[i * BK_OUT + k] = v[k];
}
