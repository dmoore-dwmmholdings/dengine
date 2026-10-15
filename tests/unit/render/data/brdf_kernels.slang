// tests/unit/render/data/brdf_kernels.slang: test_brdf.c's GPU side (DENG-298). Evaluates the
// functions of shaders/brdf/ and the S13 Standard model's BSDF (through IMaterialModel) for a
// list of cases the test wrote, so the test can compare them with tests/unit/render/brdf_ref.c.
// Cooked at test time with the engine's shaders/ directory and this one on one asset root.
import bindless;
import push_constants;
import "brdf/bsdf_common";
import "brdf/ggx";
import "brdf/fresnel";
import "brdf/diffuse";
import "brdf/sampling";
import "materials/material_interface";
import "visbuffer/resolve_model";

/// Floats each case writes (test_brdf.c BK_OUT).
static const uint BK_OUT = 24;

struct BrdfParams
{
    uint cases; // float4 x 4 per case (test_brdf.c bk_case)
    uint out_;  // BK_OUT floats per case
    uint count;
    uint reserved;
};

/// The Standard model's BSDF x cos for a grey surface (only the BSDF inputs are set).
float standard_eval(float base, float roughness, float metallic, float3 wo, float3 wi)
{
    SurfaceData s;
    s.base_color = float3(base, base, base);
    s.ao = 1.0;
    s.normal = float3(0.0, 0.0, 1.0);
    s.roughness = roughness;
    s.tangent = float4(0.0, 0.0, 0.0, 1.0);
    s.metallic = metallic;
    s.specular = 1.0;
    s.emissive = float3(0.0, 0.0, 0.0);
    s.model = 0u;
    s.features = 0u;
    return ResolveStandardModel.evaluate(s, wo, wi).x;
}

float standard_pdf(float roughness, float metallic, float3 wo, float3 wi)
{
    SurfaceData s;
    s.base_color = float3(0.5, 0.5, 0.5);
    s.ao = 1.0;
    s.normal = float3(0.0, 0.0, 1.0);
    s.roughness = roughness;
    s.tangent = float4(0.0, 0.0, 0.0, 1.0);
    s.metallic = metallic;
    s.specular = 1.0;
    s.emissive = float3(0.0, 0.0, 0.0);
    s.model = 0u;
    s.features = 0u;
    return ResolveStandardModel.pdf(s, wo, wi);
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
