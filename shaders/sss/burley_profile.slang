// shaders/sss/burley_profile.slang: Burley's normalized diffusion profile (SPEC §12.6; DENG-306).
// Pure functions with no bindings: the screen-space separable SSS (sss_separable.slang) and the
// path tracer's random-walk SSS (S17, DENG-73) use the same parameter mapping, so raster and
// path-traced subsurface scattering match. src/render/passes/sss/sss.c has the same arithmetic
// in C (the kernel tables and the tests).
//
// Christensen and Burley 2015, "Approximate Reflectance Profiles for Efficient Subsurface
// Scattering". The profile over the surface plane, for distance r from the entry point:
//
//     R(r) = (e^(-r/d) + e^(-r/(3d))) / (8 pi d r)          integrates to 1 over the plane
//
// so the distance itself has the density 2 pi r R(r) = (e^(-r/d) + e^(-r/(3d))) / (4d) and the
// distribution function 1 - e^(-r/d)/4 - 3 e^(-r/(3d))/4. The shape parameter d comes from the
// mean free path (de_standard_material.subsurface_radius, metres per channel) and the surface
// albedo A (the base colour) through the searchlight fit s(A) = 1.85 - A + 7 |A - 0.8|^3:
// d = mean free path / s(A).
module burley_profile;

public static const float DE_BURLEY_PI = 3.14159265358979323846;

/// The searchlight scaling factor s(A) of Christensen-Burley (A clamped to [0, 1]).
public float de_burley_scale(float albedo)
{
    float a = saturate(albedo);
    float t = abs(a - 0.8);
    return 1.85 - a + 7.0 * t * t * t;
}

/// The shape parameter d (metres) of mean free path `mfp` (metres) and surface albedo `albedo`.
public float de_burley_d(float mfp, float albedo)
{
    return max(mfp, 0.0) / de_burley_scale(albedo);
}

/// Per channel.
public float3 de_burley_d3(float3 mfp, float3 albedo)
{
    return float3(de_burley_d(mfp.x, albedo.x), de_burley_d(mfp.y, albedo.y),
                  de_burley_d(mfp.z, albedo.z));
}

/// R(r), the profile over the plane (1/m^2); 0 for d <= 0 or r <= 0.
public float de_burley_profile(float r, float d)
{
    if (!(d > 0.0) || !(r > 0.0))
        return 0.0;
    return (exp(-r / d) + exp(-r / (3.0 * d))) / (8.0 * DE_BURLEY_PI * d * r);
}

/// The density of the distance, 2 pi r R(r) (1/m); 0 for d <= 0.
public float de_burley_pdf(float r, float d)
{
    if (!(d > 0.0) || r < 0.0)
        return 0.0;
    return (exp(-r / d) + exp(-r / (3.0 * d))) / (4.0 * d);
}

/// The distribution of the distance: the share of the profile within radius r (1 for d <= 0).
public float de_burley_cdf(float r, float d)
{
    if (!(d > 0.0))
        return 1.0;
    float x = max(r, 0.0) / d;
    return 1.0 - 0.25 * exp(-x) - 0.75 * exp(-x / 3.0);
}

/// The radius whose distribution value is u (in [0, 1)), for d = 1: Newton iterations from a
/// start that brackets the root; multiply by d. Accurate to 1e-5 in u.
public float de_burley_sample_unit(float u)
{
    u = clamp(u, 0.0, 0.999999);
    // Start: the slower lobe's inverse, an upper bound of the root.
    float x = -3.0 * log(max((1.0 - u) / 0.75, 1e-30));
    x = max(x, 0.0);
    for (uint i = 0; i < 12; ++i)
    {
        float e1 = exp(-x), e3 = exp(-x / 3.0);
        float f = 1.0 - 0.25 * e1 - 0.75 * e3 - u;
        float df = 0.25 * e1 + 0.25 * e3;
        x = max(x - f / df, 0.0);
    }
    return x;
}

/// A radius sampled from the profile of d with uniform `u` (the path tracer's entry point).
public float de_burley_sample(float u, float d)
{
    return de_burley_sample_unit(u) * max(d, 0.0);
}
