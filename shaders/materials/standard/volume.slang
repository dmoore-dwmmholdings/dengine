// shaders/materials/standard/volume.slang: KHR_materials_volume absorption and refraction (SPEC
// §12.6; DENG-301). Pure functions with stable signatures: the forward pass (DENG-305),
// screen-space refraction (S15 DENG-342), RT refraction (S16 DENG-50) and the path tracer call
// them, so the four agree.
//
//   de_volume_sigma_a        the absorption coefficient: sigma_a = -ln(attenuation_color) /
//                            attenuation_distance (per channel; 0 for an infinite or non-positive
//                            distance: no absorption), in 1 / world units
//   de_volume_transmittance  Beer-Lambert: exp(-sigma_a d) after a path of `distance` world units
//   volume_transmittance     the same (the name the S14 issues use)
//   de_refract               the refracted direction through a facet, or false on total internal
//                            reflection
module volume;

/// Distances at or above this are infinite (KHR's default attenuationDistance is +inf).
public static const float DE_VOLUME_DISTANCE_INF = 1.0e30;

public float3 de_volume_sigma_a(float3 attenuation_color, float attenuation_distance)
{
    if (!(attenuation_distance > 0.0) || attenuation_distance >= DE_VOLUME_DISTANCE_INF)
        return float3(0.0, 0.0, 0.0);
    float3 c = clamp(attenuation_color, float3(1e-6, 1e-6, 1e-6), float3(1.0, 1.0, 1.0));
    return -log(c) / attenuation_distance;
}

public float3 de_volume_transmittance(float3 attenuation_color, float attenuation_distance,
                                      float distance)
{
    return exp(-de_volume_sigma_a(attenuation_color, attenuation_distance) * max(distance, 0.0));
}

public float3 volume_transmittance(float3 attenuation_color, float attenuation_distance,
                                   float distance)
{
    return de_volume_transmittance(attenuation_color, attenuation_distance, distance);
}

/// `wo` (unit, on the side `h` points to) refracted through a facet of normal `h` into a medium
/// `eta` times as dense (eta = n_other / n_wo); false on total internal reflection.
public bool de_refract(float3 wo, float3 h, float eta, out float3 wt)
{
    float cos_i = dot(h, wo);
    float inv = 1.0 / eta;
    float sin2_t = inv * inv * max(0.0, 1.0 - cos_i * cos_i);
    if (sin2_t >= 1.0)
    {
        wt = float3(0.0, 0.0, 0.0);
        return false;
    }
    float cos_t = sqrt(1.0 - sin2_t);
    wt = -wo * inv + h * (inv * cos_i - cos_t);
    return true;
}
