// shaders/sky/sky_ibl.slang: the sky as the image-based lighting environment (DENG-838, SPEC
// §12.13, §12.8). Engine shader `sky_ibl`. cs_sky_cube writes the sky's radiance (nits, not
// pre-exposed: the IBL is in absolute units like a cooked HDRI) into mip 0 of a face strip
// (size x 6 size, the faces stacked in layer order +X, -X, +Y, -Y, +Z, -Z with the face
// directions of probe_prefilter.slang and de_cooked_env.h). No sun or moon disk: the driven
// directional light carries them. The probe prefilter kernels (S14) then build the GGX mips.
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
};

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
    de_rwtex2d(pc.idx.x)[id.xy] = float4(min(L, float3(60000.0, 60000.0, 60000.0)), 1.0);
}
