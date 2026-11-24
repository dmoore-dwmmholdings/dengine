// shaders/volumetrics/clouds_march.slang: the clouds' raymarch (DENG-208, S34). Engine shader
// `clouds_march`, entry cs_clouds_march, pass clouds.march. One thread per 2 x 2 block of the
// view: it marches one pixel of the block per frame (frame & 3 in Bayer order), so the four
// frames of a block cover all its pixels (clouds.reconstruct puts them together).
// Per pixel: the ray's part inside the layer (up to the opaque depth), at most steps.x steps with
// a blue-noise jitter (DENG-852) shifted every frame; per sample with density: steps.y light
// steps toward the sun (Beer-Powder), the dual-lobe phase over steps.z multi-scattering octaves,
// the sky's SH ambient (the IBL, DENG-838) from above and below. Energy-conserving integration.
// Out: rgb in-scatter (pre-exposed), a transmittance; and the transmittance-weighted distance (m).
import bindless;
import push_constants;
import "../lighting/lighting_common";
import "clouds_common";

struct MarchParams
{
    uint header, entry, depth, color;
    uint dist, lview, lentry, width;
    uint height, mw, mh, flags; // flags: 1 the IBL ambient
};

/// DENG-852: the start jitter. A 64x64 blue-noise tile (tex2.w, r channel) indexed by the pixel,
/// shifted by the golden ratio each frame (a low-discrepancy sequence per pixel, so the error
/// averages out in the temporal history). Without the tile, interleaved gradient noise.
float march_jitter(CloudsHeader h, uint2 px, uint frame)
{
    float n;
    if (h.tex2.w != 0u)
        n = de_tex2d(h.tex2.w).Load(int3(int2(px & 63u), 0)).r;
    else
        n = frac(52.9829189 * frac(dot(float2(px) + float(frame & 63u) * 5.588238,
                                       float2(0.06711056, 0.00583715))));
    return frac(n + float(frame & 255u) * 0.61803398875);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_march(uint3 id : SV_DispatchThreadID, uniform MarchParams pc)
{
    if (id.x >= pc.mw || id.y >= pc.mh)
        return;
    CloudsHeader h = clouds_header(pc.header, pc.entry);
    uint frame = h.tex2.z;
    uint2 px = clouds_block_pixel(id.xy, frame, pc.width, pc.height);
    float2 uv = (float2(px) + 0.5) / float2(pc.width, pc.height);
    float3 ray = h.ray00.xyz + h.ray_du.xyz * uv.x + h.ray_dv.xyz * uv.y;
    float3 dir = normalize(ray);
    float limit = 1e30;
    float d = de_tex2d(pc.depth).Load(int3(int2(px), 0)).r;
    if (d > 0.0)
    {
        float zv = (d * h.proj.w - h.proj.y) / (h.proj.x - d * h.proj.z);
        limit = max(-zv, 0.0) * length(float3(h.vray.xy + h.vray.zw * uv, -1.0));
    }
    float2 seg = clouds_segment(h, dir, limit);
    float3 L = float3(0.0, 0.0, 0.0);
    float T = 1.0;
    float wsum = 0.0;
    if (seg.y > seg.x)
    {
        float pre = h.misc.y;
        float3 amb_up, amb_down;
        if ((pc.flags & 1u) != 0u)
        {
            DeLightingView lv = de_robuffer<DeLightingView>(pc.lview)[pc.lentry];
            amb_up = max(de_ibl_irradiance(lv.ibl, float3(0.0, 0.0, 1.0)), 0.0) * (pre / CLOUDS_PI);
            amb_down = max(de_ibl_irradiance(lv.ibl, float3(0.0, 0.0, -1.0)), 0.0) * (pre / CLOUDS_PI);
        }
        else
        {
            amb_up = clouds_sun_light(h, float3(0.0, 0.0, h.layer.z - h.layer.x - h.layer.w)) *
                     (0.05 / CLOUDS_PI);
            amb_down = amb_up * 0.3;
        }
        uint steps = clamp(h.steps.x, 8u, 128u);
        uint lsteps = min(h.steps.y, 6u);
        uint octaves = clamp(h.steps.z, 1u, 4u);
        float len = seg.y - seg.x;
        float dt = len / float(steps);
        float jitter = march_jitter(h, px, frame);
        float cos_theta = dot(dir, h.sun.xyz);
        float bottom = h.layer.y - h.layer.x, thick = h.layer.z - h.layer.y;
        for (uint i = 0; i < steps; ++i)
        {
            float t = seg.x + (float(i) + jitter) * dt;
            float3 rel = dir * t;
            float sigma_t = clouds_extinction(h, rel, true);
            if (sigma_t <= 0.0)
                continue;
            // The light march toward the sun: steps of 40 m doubling (about 2.5 km for six).
            float od = 0.0, tl = 0.0, ds = 40.0;
            for (uint k = 0; k < lsteps; ++k)
            {
                od += clouds_extinction(h, rel + h.sun.xyz * (tl + ds * 0.5), k < 3u) * ds;
                tl += ds;
                ds *= 2.0;
            }
            float3 sun = clouds_sun_light(h, rel);
            float sigma_s = sigma_t * CLOUDS_ALBEDO;
            // Powder: the dark edges seen with the sun behind the viewer, none when backlit.
            float powder = lerp(1.0 - exp(-sigma_t * 100.0), 1.0, saturate(cos_theta * 0.5 + 0.5));
            float3 S = float3(0.0, 0.0, 0.0);
            float a = 1.0;
            for (uint o = 0; o < octaves; ++o)
            {
                S += a * clouds_phase(cos_theta, a) * exp(-a * od) * sun;
                a *= 0.5;
            }
            S *= powder;
            float f = saturate((clouds_altitude(h, rel) - bottom) / max(thick, 1.0));
            float3 amb = lerp(amb_up * 0.4 + amb_down * 0.4, amb_up, f);
            S = (S + amb) * sigma_s;
            float st = exp(-sigma_t * dt);
            L += T * (S - S * st) / sigma_t;
            wsum += t * T * (1.0 - st);
            T *= st;
            if (T < 0.01)
                break;
        }
    }
    float dist = (1.0 - T) > 1e-3 ? wsum / (1.0 - T) : (seg.y > seg.x ? 0.5 * (seg.x + seg.y) : 1e5);
    L = min(L, float3(CLOUDS_MAX_VALUE, CLOUDS_MAX_VALUE, CLOUDS_MAX_VALUE));
    // The early-out leaves up to 1 % through, enough to show the sun disk (about 1e5 times the
    // sky) through an overcast deck: remap so that an opaque cloud is fully opaque.
    T = saturate((T - 0.01) / 0.99);
    de_rwtex2d(pc.color)[id.xy] = float4(L, T);
    de_rwtex2d(pc.dist)[id.xy] = float4(dist, 0.0, 0.0, 0.0);
}
