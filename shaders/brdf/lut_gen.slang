// shaders/brdf/lut_gen.slang: generates the energy LUT of energy.slang (SPEC §12.6; DENG-299).
// Engine shader `brdf_lut_gen`; src/render/materials/brdf_luts.c runs it once at renderer start
// (and tests compare it with the CPU reference of the same file).
//
// One group of DE_BRDF_LUT_SIZE threads per roughness row r_j = j / (size - 1): thread i
// integrates A and B at mu_i = (i + 0.5) / size with `samples` visible-normal samples (a fixed
// Hammersley set, so the result does not depend on time or on the device's scheduling), then the
// group averages the row (midpoint rule: A_avg = 2 sum A(mu_k) mu_k / size) and every thread
// writes its texel into a buffer that brdf_luts.c copies into the texture. The estimator of
// f_ss cos / pdf for a VNDF sample with F = 1 is G2 / G1.
import bindless;
import push_constants;
import bsdf_common;
import ggx;
import energy;
import sampling;

struct LutParams
{
    uint target;  // a float4 buffer, row-major DE_BRDF_LUT_SIZE^2 (copied into the RGBA32F texture)
    uint samples; // VNDF samples per texel
    uint reserved0;
    uint reserved1;
};

groupshared float2 g_ab[DE_BRDF_LUT_SIZE];

[shader("compute")]
[numthreads(32, 1, 1)]
void cs_brdf_lut(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                 uniform LutParams pc)
{
    uint i = thread.x, j = group.x;
    float n = float(DE_BRDF_LUT_SIZE);
    float mu = (float(i) + 0.5) / n;
    float alpha = de_ggx_alpha(float(j) / (n - 1.0));
    float3 wo = float3(sqrt(1.0 - mu * mu), 0.0, mu);
    float a = 0.0, b = 0.0;
    for (uint k = 0; k < pc.samples; ++k)
    {
        float3 h = de_ggx_vndf_sample(alpha, alpha, wo, de_hammersley(k, pc.samples));
        float3 wi = de_reflect(wo, h);
        if (!(wi.z > 0.0))
            continue;
        float w = de_ggx_g2(alpha, alpha, wo, wi) / de_ggx_g1(alpha, alpha, wo);
        float fc = de_pow5(1.0 - saturate(dot(wo, h)));
        a += w * (1.0 - fc);
        b += w * fc;
    }
    float2 ab = float2(a, b) / float(pc.samples);
    g_ab[i] = ab;
    GroupMemoryBarrierWithGroupSync();
    float2 avg = float2(0.0, 0.0);
    for (uint k = 0; k < DE_BRDF_LUT_SIZE; ++k)
        avg += g_ab[k] * ((float(k) + 0.5) / n);
    avg *= 2.0 / n;
    de_buffer<float4>(pc.target)[j * DE_BRDF_LUT_SIZE + i] = float4(ab, avg);
}
