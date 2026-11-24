// shaders/sky/sky_ibl.slang: the sky as the image-based lighting environment (DENG-838, SPEC
// §12.13, §12.8). Engine shader `sky_ibl`. cs_sky_cube writes the sky's radiance (nits, not
// pre-exposed: the IBL is in absolute units like a cooked HDRI) into mip 0 of a face strip
// (size x 6 size, the faces stacked in layer order +X, -X, +Y, -Y, +Z, -Z with the face
// directions of probe_prefilter.slang and de_cooked_env.h). No sun or moon disk: the driven
// directional light carries them. The probe prefilter kernels (S14) then build the GGX mips.
// DENG-853: with clouds, the cloud layer (sky_ibl.c de__sky_ibl_clouds_for) is blended in as a
// grey diffuse layer, so the IBL (and its SH9) darken and lose the blue sky under overcast.
import bindless;
import push_constants;
import "sky_common";

struct SkyIblParams
{
    uint4 idx;   // target (storage, mip 0), transmittance, multiscatter, sampler (linear clamp)
    float4 sun;  // direction (world, +Z up), weight (lux)
    float4 moon; // direction, weight
    float4 misc; // radius of the viewer (km), mie scale, ground albedo, steps
    uint4 size;  // face size, 0, 0, 0
    float4 cloud; // DENG-853: cover (0: no clouds), diffuse transmission, bottom, top radius (km)
};

/// The light reaching the cloud layer from a body toward `dir` of illuminance `weight`: on its
/// top (cos of the elevation), plus, for broken clouds (cover < 1), on the sides that face a low
/// body (half the sine), so scattered clouds at sunset stay lit.
static float3 sky_ibl_layer_light(SkyIblParams pc, float3 dir, float weight, float cover)
{
    if (weight <= 0.0 || dir.z <= 0.0)
        return float3(0.0, 0.0, 0.0);
    float facing = dir.z + (1.0 - cover) * 0.5 * sqrt(max(1.0 - dir.z * dir.z, 0.0));
    return weight * facing * sky_transmittance(pc.idx.y, pc.idx.w, pc.cloud.w, dir.z);
}

/// DENG-853: `L` (the clear sky toward `dir` from radius `r`) with the cloud layer. The layer is a
/// diffuse slab: its underside shows the light above it times the diffuse transmission, its top
/// the rest (reflected, minus a little absorption), both Lambertian (radiance E / pi). Toward the
/// horizon the view crosses more of the layer, so its cover grows (1 - (1 - c)^(1 / mu)). Under it
/// the ground in the lower hemisphere is lit by the overcast (albedo x the underside), and the
/// air below the layer keeps its share of the clear sky's in-scatter.
static float3 sky_ibl_clouds(SkyIblParams pc, float3 dir, float r, float3 L)
{
    float cover = saturate(pc.cloud.x);
    float td = saturate(pc.cloud.y);
    float rb = pc.cloud.z, rt = pc.cloud.w;
    // The light above the layer: the sun and the moon, plus the clear sky's diffuse light (about
    // 15 % of the direct light at the layer's height).
    float3 E = (sky_ibl_layer_light(pc, pc.sun.xyz, pc.sun.w, cover) +
                sky_ibl_layer_light(pc, pc.moon.xyz, pc.moon.w, cover)) * 1.15;
    float3 base = E * (td / SKY_PI);
    float3 top = E * ((1.0 - td) * 0.95 / SKY_PI);
    float thick = max(rt - rb, 1e-3);
    float w_up = saturate((rt - r) / thick);   // 1 under the layer, 0 over it
    float w_down = saturate((r - rb) / thick); // 0 under the layer, 1 over it
    float mu = abs(dir.z);
    float c_dir = 1.0 - pow(1.0 - cover, 1.0 / max(mu, 0.2));
    if (dir.z > 0.0)
    {
        float below = 1.0 - exp(-max(rb - r, 0.0) / SKY_RAYLEIGH_H);
        return lerp(L, base + below * L, c_dir * w_up);
    }
    float albedo = pc.misc.z;
    L = lerp(L, albedo * base, cover * w_up);
    return lerp(L, top, c_dir * w_down);
}

static float3 sky_ibl_face_dir(uint f, float2 st)
{
    if (f == 0u)
        return normalize(float3(1.0, -st.y, -st.x));
    if (f == 1u)
        return normalize(float3(-1.0, -st.y, st.x));
    if (f == 2u)
        return normalize(float3(st.x, 1.0, st.y));
    if (f == 3u)
        return normalize(float3(st.x, -1.0, -st.y));
    if (f == 4u)
        return normalize(float3(st.x, -st.y, 1.0));
    return normalize(float3(-st.x, -st.y, -1.0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_sky_cube(uint3 id : SV_DispatchThreadID, uniform SkyIblParams pc)
{
    uint n = pc.size.x;
    if (id.x >= n || id.y >= 6u * n)
        return;
    uint face = id.y / n;
    float2 st = (float2(float(id.x), float(id.y % n)) + 0.5) / float(n) * 2.0 - 1.0;
    float3 dir = sky_ibl_face_dir(face, st);
    SkyLight sun, moon;
    sun.dir = pc.sun.xyz;
    sun.weight = pc.sun.w;
    moon.dir = pc.moon.xyz;
    moon.weight = pc.moon.w;
    float3 L = sky_integrate(pc.idx.y, pc.idx.z, pc.idx.w, pc.misc.y, pc.misc.z, pc.misc.x, dir,
                             sun, moon, uint(pc.misc.w));
    if (pc.cloud.x > 0.0)
        L = sky_ibl_clouds(pc, dir, pc.misc.x, L);
    de_rwtex2d(pc.idx.x)[id.xy] = float4(min(L, float3(60000.0, 60000.0, 60000.0)), 1.0);
}
