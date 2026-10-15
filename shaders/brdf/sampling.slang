// shaders/brdf/sampling.slang: direction sampling and low-discrepancy sequences (SPEC §12.6;
// DENG-298). Pure functions; local frame (Z up; bsdf_common.slang). `u` values are uniform in
// [0, 1).
//
//   de_sample_concentric_disk     the unit disk (Shirley and Chiu 1997: low distortion)
//   de_sample_cosine_hemisphere   cos(theta) / pi over the upper hemisphere (Malley's method)
//   de_sample_uniform_sphere      1 / (4 pi) over the sphere
//   de_radical_inverse2           van der Corput in base 2
//   de_hammersley                 point i of an n-point Hammersley set
//   de_r2                         point i of Roberts' R2 sequence (open-ended, 2D)
//   de_pcg_hash / de_u32_to_unit  a 32-bit hash (PCG output permutation) and a u32 to [0, 1)
module sampling;

import bsdf_common;

public float2 de_sample_concentric_disk(float2 u)
{
    float2 o = u * 2.0 - 1.0;
    if (o.x == 0.0 && o.y == 0.0)
        return float2(0.0, 0.0);
    float r, theta;
    if (abs(o.x) > abs(o.y))
    {
        r = o.x;
        theta = (DE_PI / 4.0) * (o.y / o.x);
    }
    else
    {
        r = o.y;
        theta = (DE_PI / 2.0) - (DE_PI / 4.0) * (o.x / o.y);
    }
    return r * float2(cos(theta), sin(theta));
}

public float3 de_sample_cosine_hemisphere(float2 u)
{
    float2 d = de_sample_concentric_disk(u);
    float z = sqrt(max(0.0, 1.0 - dot(d, d)));
    return float3(d.x, d.y, z);
}

/// The density of de_sample_cosine_hemisphere for cos(theta) = `cos_theta`.
public float de_cosine_hemisphere_pdf(float cos_theta)
{
    return max(cos_theta, 0.0) * DE_INV_PI;
}

public float3 de_sample_uniform_sphere(float2 u)
{
    float z = 1.0 - 2.0 * u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * DE_PI * u.y;
    return float3(r * cos(phi), r * sin(phi), z);
}

public float de_radical_inverse2(uint i)
{
    return float(reversebits(i)) * 2.3283064365386963e-10; // 2^-32
}

public float2 de_hammersley(uint i, uint n)
{
    return float2((float(i) + 0.5) / float(n), de_radical_inverse2(i));
}

/// Roberts' R2: frac(0.5 + i (1/g, 1/g^2)), g the plastic number. The multiply is done on 32-bit
/// fixed point so long sequences keep their precision.
public float2 de_r2(uint i)
{
    uint x = 0x80000000u + i * 3242174889u; // 2^32 / g
    uint y = 0x80000000u + i * 2447445414u; // 2^32 / g^2
    return float2(float(x >> 8), float(y >> 8)) * 5.9604644775390625e-8; // 2^-24
}

public uint de_pcg_hash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

public float de_u32_to_unit(uint v)
{
    return float(v >> 8) * 5.9604644775390625e-8; // 2^-24
}
