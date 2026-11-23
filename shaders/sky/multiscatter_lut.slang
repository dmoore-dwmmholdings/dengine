// shaders/sky/multiscatter_lut.slang: the multiple-scattering LUT of Hillaire 2020 §5.5 (SPEC
// §12.13; DENG-206). Engine shader `sky_multiscatter`. One group of 64 threads per texel of the
// 32 x 32 LUT (u: cos sun zenith, v: altitude); each thread integrates one direction of a
// uniform sphere set, and the group sums them. Texel: Psi_ms = L_2 / (1 - f_ms), per unit
// illuminance. Writes a float4 buffer that sky_luts.c copies into the RGBA32F texture.
import bindless;
import push_constants;
import "sky_common";

struct MultiscatterParams
{
    uint target;       // float4 buffer, row-major N x N
    uint transmittance; // texture
    uint smp;          // linear clamp sampler
    float mie_scale;
    float albedo;
    uint steps;
    uint reserved0, reserved1;
};

#define MS_DIRS 64
groupshared float3 g_l2[MS_DIRS];
groupshared float3 g_f[MS_DIRS];

[shader("compute")]
[numthreads(MS_DIRS, 1, 1)]
void cs_multiscatter(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                     uniform MultiscatterParams pc)
{
    float N = float(SKY_MULTISCATTER_N);
    float2 uv = (float2(group.xy) + 0.5) / N;
    float mu_s = uv.x * 2.0 - 1.0;
    float r = SKY_BOTTOM + max(uv.y, 1e-4) * (SKY_TOP - SKY_BOTTOM);
    float3 sun = float3(sqrt(max(0.0, 1.0 - mu_s * mu_s)), 0.0, mu_s);

    // A uniform sphere direction per thread (Fibonacci lattice).
    uint k = thread.x;
    float z = 1.0 - (2.0 * float(k) + 1.0) / float(MS_DIRS);
    float phi = float(k) * 2.39996322972865332;
    float s = sqrt(max(0.0, 1.0 - z * z));
    float3 dir = float3(s * cos(phi), s * sin(phi), z);

    bool ground = sky_hits_ground(r, dir.z);
    float t_max = ground ? sky_dist_to_ground(r, dir.z) : sky_dist_to_top(r, dir.z);
    float3 L = float3(0.0, 0.0, 0.0), F = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    float iso = 1.0 / (4.0 * SKY_PI);
    float n = float(pc.steps);
    for (uint i = 0; i < pc.steps; ++i)
    {
        float dt = t_max / n;
        float t = (float(i) + 0.5) * dt;
        float3 p = float3(0.0, 0.0, r) + dir * t;
        float rp = length(p);
        float mus = dot(p / rp, sun);
        SkyMedium m = sky_medium(rp - SKY_BOTTOM, pc.mie_scale);
        float3 scat = m.rayleigh + m.mie;
        float3 ext = max(m.extinction, float3(1e-7, 1e-7, 1e-7));
        float vis = sky_hits_ground(rp, mus) ? 0.0 : 1.0;
        float3 tl = sky_transmittance(pc.transmittance, pc.smp, rp, mus);
        float3 S = vis * tl * scat * iso; // isotropic phase (Hillaire §5.5)
        float3 st = exp(-ext * dt);
        L += thr * (S - S * st) / ext;
        F += thr * (scat - scat * st) / ext;
        thr *= st;
    }
    if (ground)
    {
        float3 p = float3(0.0, 0.0, r) + dir * t_max;
        float3 up = normalize(p);
        float m0 = dot(up, sun);
        L += thr * saturate(m0) * sky_transmittance(pc.transmittance, pc.smp, SKY_BOTTOM, m0) *
             (pc.albedo / SKY_PI);
    }
    g_l2[k] = L;
    g_f[k] = F;
    GroupMemoryBarrierWithGroupSync();
    if (k == 0)
    {
        float3 l2 = float3(0.0, 0.0, 0.0), f = float3(0.0, 0.0, 0.0);
        for (uint j = 0; j < MS_DIRS; ++j)
        {
            l2 += g_l2[j];
            f += g_f[j];
        }
        // Mean over the sphere (uniform pdf 1 / 4 pi times the isotropic 1 / 4 pi of the
        // integrals: Hillaire's 1 / N average with the phase in the integrand).
        l2 /= float(MS_DIRS);
        f /= float(MS_DIRS);
        float3 psi = l2 / max(float3(1.0, 1.0, 1.0) - f, float3(1e-3, 1e-3, 1e-3));
        de_buffer<float4>(pc.target)[group.y * SKY_MULTISCATTER_N + group.x] = float4(psi, 1.0);
    }
}
