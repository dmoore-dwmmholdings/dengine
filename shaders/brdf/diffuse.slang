// shaders/brdf/diffuse.slang: diffuse reflection (SPEC §12.6; DENG-298). Pure functions; the
// values are the BRDF (multiply by the albedo and cos(theta_i) yourself).
//
//   de_diffuse_lambert   1 / pi
//   de_diffuse_burley    Burley 2012 ("Physically-Based Shading at Disney"): Lambert times a
//                        retro-reflection factor per direction, F90 = 0.5 + 2 roughness cos^2 of
//                        the angle to the half vector. Symmetric in the two directions.
module diffuse;

import bsdf_common;

public float de_diffuse_lambert()
{
    return DE_INV_PI;
}

/// `roughness` perceptual; `cos_o`, `cos_i` the directions' cos(theta) (clamped to [0, 1]);
/// `cos_ih` the cosine between wi and the half vector.
public float de_diffuse_burley(float roughness, float cos_o, float cos_i, float cos_ih)
{
    float f90 = 0.5 + 2.0 * roughness * cos_ih * cos_ih;
    float fo = 1.0 + (f90 - 1.0) * de_pow5(1.0 - saturate(cos_o));
    float fi = 1.0 + (f90 - 1.0) * de_pow5(1.0 - saturate(cos_i));
    return DE_INV_PI * fo * fi;
}
