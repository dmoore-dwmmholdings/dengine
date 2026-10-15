// shaders/brdf/lut_gen.slang: generates the energy LUT of energy.slang and the sheen rows of
// sheen.slang (SPEC §12.6; DENG-299, DENG-300). Engine shader `brdf_lut_gen`;
// src/render/materials/brdf_luts.c runs it once at renderer start (and tests compare it with the
// CPU reference of the same file).
//
// One group of DE_BRDF_LUT_SIZE threads per row; thread i is mu_i = (i + 0.5) / size. Every
// estimate uses a fixed Hammersley set of `samples` points, so the result does not depend on time
// or on the device's scheduling. The group then averages the row (midpoint rule: avg = 2 sum
// v(mu_k) mu_k / size) and every thread writes its texel into a buffer that brdf_luts.c copies
// into the texture.
//
//   rows 0 .. size-1        energy (DENG-299), roughness r_j = j / (size - 1): A and B with VNDF
//                           samples, whose estimator of f_ss cos / pdf with F = 1 is G2 / G1;
//                           texel (A, B, A_avg, B_avg)
//   rows size .. 2 size-1   sheen (DENG-300), roughness DE_SHEEN_ROUGHNESS_MIN .. 1: the albedo
//                           E of the Charlie lobe (sheen colour 1) with uniform hemisphere
//                           samples (estimator 2 pi D V mu_i); texel (E, E_avg, 0, 0)
import bindless;
import push_constants;
import bsdf_common;
import ggx;
import energy;
import sampling;
import sheen;

struct LutParams
{
    uint target;  // a float4 buffer, row-major size x 2 size (copied into the RGBA32F texture)
    uint samples; // samples per texel
    uint reserved0;
    uint reserved1;
};

groupshared float2 g_ab[DE_BRDF_LUT_SIZE];

/// The energy texel (A, B) at mu for GGX alpha `alpha`.
float2 energy_ab(float mu, float alpha, uint samples)
{
    float3 wo = float3(sqrt(1.0 - mu * mu), 0.0, mu);
    float a = 0.0, b = 0.0;
    for (uint k = 0; k < samples; ++k)
    {
        float3 h = de_ggx_vndf_sample(alpha, alpha, wo, de_hammersley(k, samples));
        float3 wi = de_reflect(wo, h);
        if (!(wi.z > 0.0))
            continue;
        float w = de_ggx_g2(alpha, alpha, wo, wi) / de_ggx_g1(alpha, alpha, wo);
        float fc = de_pow5(1.0 - saturate(dot(wo, h)));
        a += w * (1.0 - fc);
        b += w * fc;
    }
    return float2(a, b) / float(samples);
}

/// The sheen albedo at mu for sheen roughness `r` (colour 1).
float sheen_e(float mu, float r, uint samples)
{
    float3 wo = float3(sqrt(1.0 - mu * mu), 0.0, mu);
    float e = 0.0;
    for (uint k = 0; k < samples; ++k)
    {
        float2 u = de_hammersley(k, samples);
        float z = u.x, phi = 2.0 * DE_PI * u.y;
        float st = sqrt(max(0.0, 1.0 - z * z));
        float3 wi = float3(st * cos(phi), st * sin(phi), z);
        e += de_sheen_brdf(r, wo, wi) * wi.z;
    }
    return e * (2.0 * DE_PI) / float(samples);
}

[shader("compute")]
[numthreads(32, 1, 1)]
void cs_brdf_lut(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                 uniform LutParams pc)
{
    uint i = thread.x, j = group.x;
    float n = float(DE_BRDF_LUT_SIZE);
    float mu = (float(i) + 0.5) / n;
    float2 v;
    if (j < DE_BRDF_LUT_SIZE)
        v = energy_ab(mu, de_ggx_alpha(float(j) / (n - 1.0)), pc.samples);
    else
    {
        float t = float(j - DE_BRDF_LUT_SIZE) / (n - 1.0);
        v = float2(sheen_e(mu, lerp(DE_SHEEN_ROUGHNESS_MIN, 1.0, t), pc.samples), 0.0);
    }
    g_ab[i] = v;
    GroupMemoryBarrierWithGroupSync();
    float2 avg = float2(0.0, 0.0);
    for (uint k = 0; k < DE_BRDF_LUT_SIZE; ++k)
        avg += g_ab[k] * ((float(k) + 0.5) / n);
    avg *= 2.0 / n;
    float4 texel = j < DE_BRDF_LUT_SIZE ? float4(v, avg) : float4(v.x, avg.x, 0.0, 0.0);
    de_buffer<float4>(pc.target)[j * DE_BRDF_LUT_SIZE + i] = texel;
}
