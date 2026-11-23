// shaders/sky/sky_common.slang: the physical atmosphere of the sky (SPEC §12.13; DENG-206).
//
// The model is Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique") on the Bruneton 2017 parameterization. Distances are in kilometres. Radiance is per
// unit illuminance of the light at the top of the atmosphere; a caller multiplies by the light's
// illuminance (lux) and by the view's pre-exposure (the sky-view LUT already has both).
//
// STABLE CONTRACT: fog (DENG-207), clouds (DENG-208), IBL and the RT miss shader import this module.
// Keep these names and their meanings: sky_medium, sky_phase_rayleigh, sky_phase_mie,
// sky_dist_to_top, sky_dist_to_ground, sky_hits_ground, sky_transmittance_uv,
// sky_transmittance_rmu, sky_transmittance, sky_multiscatter_uv, sky_multiscatter, sky_view_uv,
// sky_view_dir, sky_integrate, sky_radiance. The constants mirror src/world/sky/sky_internal.h.
module sky_common;

import bindless;

public static const float SKY_PI = 3.14159265358979323846;
public static const float SKY_BOTTOM = 6360.0; // ground radius, km
public static const float SKY_TOP = 6460.0;    // top of the atmosphere, km
public static const float3 SKY_RAYLEIGH_SCAT = float3(5.802e-3, 13.558e-3, 33.1e-3); // 1/km
public static const float SKY_RAYLEIGH_H = 8.0;                                     // km
public static const float SKY_MIE_SCAT = 3.996e-3; // 1/km at turbidity 2 (mie scale 1)
public static const float SKY_MIE_EXT = 4.440e-3;  // 1/km
public static const float SKY_MIE_H = 1.2;         // km
public static const float SKY_MIE_G = 0.8;
public static const float3 SKY_OZONE_ABS = float3(0.650e-3, 1.881e-3, 0.085e-3); // 1/km
public static const float SKY_OZONE_CENTER = 25.0;                               // km
public static const float SKY_OZONE_HALF_WIDTH = 15.0;                           // km

public static const uint SKY_TRANSMITTANCE_W = 256;
public static const uint SKY_TRANSMITTANCE_H = 64;
public static const uint SKY_MULTISCATTER_N = 32;
public static const uint SKY_VIEW_W = 192;
public static const uint SKY_VIEW_H = 108;

/// The medium at altitude `h` (km above ground). `mie_scale` is turbidity - 1.
public struct SkyMedium
{
    public float3 rayleigh; // scattering
    public float mie;       // scattering
    public float3 extinction;
};

public SkyMedium sky_medium(float h, float mie_scale)
{
    SkyMedium m;
    float hr = exp(-max(h, 0.0) / SKY_RAYLEIGH_H);
    float hm = exp(-max(h, 0.0) / SKY_MIE_H) * mie_scale;
    float ho = max(0.0, 1.0 - abs(h - SKY_OZONE_CENTER) / SKY_OZONE_HALF_WIDTH);
    m.rayleigh = SKY_RAYLEIGH_SCAT * hr;
    m.mie = SKY_MIE_SCAT * hm;
    m.extinction = m.rayleigh + SKY_MIE_EXT * hm + SKY_OZONE_ABS * ho;
    return m;
}

public float sky_phase_rayleigh(float c)
{
    return 3.0 / (16.0 * SKY_PI) * (1.0 + c * c);
}

/// Cornette-Shanks.
public float sky_phase_mie(float c, float g)
{
    float g2 = g * g;
    float d = max(1.0 + g2 - 2.0 * g * c, 1e-4);
    return 3.0 / (8.0 * SKY_PI) * (1.0 - g2) * (1.0 + c * c) / ((2.0 + g2) * d * sqrt(d));
}

/// Distance from radius r along mu (cosine from zenith) to the top sphere.
public float sky_dist_to_top(float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + SKY_TOP * SKY_TOP;
    return max(0.0, -r * mu + sqrt(max(disc, 0.0)));
}

public float sky_dist_to_ground(float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + SKY_BOTTOM * SKY_BOTTOM;
    return max(0.0, -r * mu - sqrt(max(disc, 0.0)));
}

public bool sky_hits_ground(float r, float mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + SKY_BOTTOM * SKY_BOTTOM >= 0.0;
}

/// The transmittance LUT's uv of (r, mu) with the texel-centre remap (Bruneton 2017).
public float2 sky_transmittance_uv(float r, float mu)
{
    float H = sqrt(SKY_TOP * SKY_TOP - SKY_BOTTOM * SKY_BOTTOM);
    float rho = sqrt(max(r * r - SKY_BOTTOM * SKY_BOTTOM, 0.0));
    float d = sky_dist_to_top(r, mu);
    float d_min = SKY_TOP - r, d_max = rho + H;
    float x_mu = (d_max > d_min) ? (d - d_min) / (d_max - d_min) : 0.0;
    float x_r = rho / H;
    float W = float(SKY_TRANSMITTANCE_W), Hh = float(SKY_TRANSMITTANCE_H);
    return float2(0.5 / W + saturate(x_mu) * (1.0 - 1.0 / W),
                  0.5 / Hh + saturate(x_r) * (1.0 - 1.0 / Hh));
}

/// The inverse of sky_transmittance_uv.
public void sky_transmittance_rmu(float2 uv, out float r, out float mu)
{
    float W = float(SKY_TRANSMITTANCE_W), Hh = float(SKY_TRANSMITTANCE_H);
    float x_mu = (uv.x - 0.5 / W) / (1.0 - 1.0 / W);
    float x_r = (uv.y - 0.5 / Hh) / (1.0 - 1.0 / Hh);
    float H = sqrt(SKY_TOP * SKY_TOP - SKY_BOTTOM * SKY_BOTTOM);
    float rho = H * x_r;
    r = sqrt(rho * rho + SKY_BOTTOM * SKY_BOTTOM);
    float d_min = SKY_TOP - r, d_max = rho + H;
    float d = d_min + x_mu * (d_max - d_min);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

public float3 sky_transmittance(uint lut, uint smp, float r, float mu)
{
    return de_tex2d(lut).SampleLevel(de_sampler(smp), sky_transmittance_uv(r, mu), 0.0).rgb;
}

/// The multi-scattering LUT: u = cos sun zenith (-1..1), v = altitude (0..top).
public float2 sky_multiscatter_uv(float r, float mu_s)
{
    float N = float(SKY_MULTISCATTER_N);
    float2 x = float2(saturate(mu_s * 0.5 + 0.5),
                      saturate((r - SKY_BOTTOM) / (SKY_TOP - SKY_BOTTOM)));
    return 0.5 / N + x * (1.0 - 1.0 / N);
}

public float3 sky_multiscatter(uint lut, uint smp, float r, float mu_s)
{
    return de_tex2d(lut).SampleLevel(de_sampler(smp), sky_multiscatter_uv(r, mu_s), 0.0).rgb;
}

/// The sky-view LUT's uv of world direction `dir` (+Z up) seen from radius r. u is the world
/// azimuth atan2(x, y) over 0..1 (wraps); v puts the horizon at 0.5 with more texels near it.
public float2 sky_view_uv(float r, float3 dir)
{
    float rr = max(r, SKY_BOTTOM + 1e-3);
    float vhor = sqrt(rr * rr - SKY_BOTTOM * SKY_BOTTOM);
    float beta = acos(clamp(vhor / rr, -1.0, 1.0)); // the ground's angle below horizontal
    float zha = SKY_PI - beta;                       // zenith angle of the horizon
    float zen = acos(clamp(dir.z, -1.0, 1.0));
    float v;
    if (zen < zha)
        v = (1.0 - sqrt(max(1.0 - zen / zha, 0.0))) * 0.5;
    else
        v = sqrt(saturate((zen - zha) / max(beta, 1e-6))) * 0.5 + 0.5;
    float az = atan2(dir.x, dir.y) / (2.0 * SKY_PI);
    float u = az - floor(az);
    float Hh = float(SKY_VIEW_H);
    return float2(u, 0.5 / Hh + v * (1.0 - 1.0 / Hh));
}

/// The inverse of sky_view_uv (u is the texel's 0..1 azimuth, without a remap: it wraps).
public float3 sky_view_dir(float r, float2 uv)
{
    float rr = max(r, SKY_BOTTOM + 1e-3);
    float vhor = sqrt(rr * rr - SKY_BOTTOM * SKY_BOTTOM);
    float beta = acos(clamp(vhor / rr, -1.0, 1.0));
    float zha = SKY_PI - beta;
    float Hh = float(SKY_VIEW_H);
    float v = saturate((uv.y - 0.5 / Hh) / (1.0 - 1.0 / Hh));
    float zen;
    if (v < 0.5)
    {
        float c = 1.0 - 2.0 * v;
        zen = zha * (1.0 - c * c);
    }
    else
    {
        float c = v * 2.0 - 1.0;
        zen = zha + beta * c * c;
    }
    float az = uv.x * 2.0 * SKY_PI;
    float s = sin(zen);
    return float3(s * sin(az), s * cos(az), cos(zen));
}

/// One light of the sky: its direction (toward the light) and its weight (illuminance at the top
/// of the atmosphere, in the caller's units; 0 = off).
public struct SkyLight
{
    public float3 dir;
    public float weight;
};

/// The luminance scattered toward the viewer along `dir` from radius r (+Z up at the viewer), from
/// two lights, with the multiple-scattering term and the ground's Lambert bounce (albedo). Uses
/// the transmittance and multi-scattering LUTs; `steps` segments.
public float3 sky_integrate(uint tlut, uint mslut, uint smp, float mie_scale, float albedo, float r,
                            float3 dir, SkyLight l0, SkyLight l1, uint steps)
{
    float mu = dir.z;
    bool ground = sky_hits_ground(r, mu);
    float t_max = ground ? sky_dist_to_ground(r, mu) : sky_dist_to_top(r, mu);
    if (r > SKY_TOP)
        return float3(0.0, 0.0, 0.0); // outside the atmosphere: not handled (space views)
    float3 L = float3(0.0, 0.0, 0.0);
    float3 thr = float3(1.0, 1.0, 1.0);
    float c0 = dot(dir, l0.dir), c1 = dot(dir, l1.dir);
    float pr0 = sky_phase_rayleigh(c0), pm0 = sky_phase_mie(c0, SKY_MIE_G);
    float pr1 = sky_phase_rayleigh(c1), pm1 = sky_phase_mie(c1, SKY_MIE_G);
    float t_prev = 0.0;
    float n = float(steps);
    for (uint i = 0; i < steps; ++i)
    {
        // Quadratic spacing: more samples near the viewer.
        float a = (float(i) + 1.0) / n;
        float t = t_max * a * a;
        float tm = 0.5 * (t + t_prev);
        float dt = t - t_prev;
        t_prev = t;
        float3 p = float3(0.0, 0.0, r) + dir * tm;
        float rp = length(p);
        float3 up = p / rp;
        SkyMedium m = sky_medium(rp - SKY_BOTTOM, mie_scale);
        float3 S = float3(0.0, 0.0, 0.0);
        if (l0.weight > 0.0)
        {
            float mus = dot(up, l0.dir);
            float vis = sky_hits_ground(rp, mus) ? 0.0 : 1.0;
            float3 tl = sky_transmittance(tlut, smp, rp, mus);
            float3 ms = sky_multiscatter(mslut, smp, rp, mus);
            S += l0.weight * (vis * tl * (m.rayleigh * pr0 + m.mie * pm0) +
                              ms * (m.rayleigh + m.mie));
        }
        if (l1.weight > 0.0)
        {
            float mus = dot(up, l1.dir);
            float vis = sky_hits_ground(rp, mus) ? 0.0 : 1.0;
            float3 tl = sky_transmittance(tlut, smp, rp, mus);
            float3 ms = sky_multiscatter(mslut, smp, rp, mus);
            S += l1.weight * (vis * tl * (m.rayleigh * pr1 + m.mie * pm1) +
                              ms * (m.rayleigh + m.mie));
        }
        float3 ext = max(m.extinction, float3(1e-7, 1e-7, 1e-7));
        float3 st = exp(-ext * dt);
        L += thr * (S - S * st) / ext;
        thr *= st;
    }
    if (ground)
    {
        float3 p = float3(0.0, 0.0, r) + dir * t_max;
        float3 n = normalize(p);
        float3 g = float3(0.0, 0.0, 0.0);
        float m0 = dot(n, l0.dir), m1 = dot(n, l1.dir);
        if (l0.weight > 0.0)
            g += l0.weight * saturate(m0) * sky_transmittance(tlut, smp, SKY_BOTTOM, m0);
        if (l1.weight > 0.0)
            g += l1.weight * saturate(m1) * sky_transmittance(tlut, smp, SKY_BOTTOM, m1);
        L += thr * g * (albedo / SKY_PI);
    }
    return L;
}

/// The sky's radiance toward `dir` (world, +Z up) from the sky-view LUT `skyview` for a viewer at
/// radius r: in the LUT's units (the raster view's: pre-exposed luminance).
public float3 sky_radiance(uint skyview, uint smp, float r, float3 dir)
{
    return de_tex2d(skyview).SampleLevel(de_sampler(smp), sky_view_uv(r, dir), 0.0).rgb;
}
