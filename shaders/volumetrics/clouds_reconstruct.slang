// shaders/volumetrics/clouds_reconstruct.slang: the clouds' temporal reconstruction (DENG-208,
// S34). Engine shader `clouds_reconstruct`, entry cs_clouds_reconstruct, pass clouds.reconstruct.
// Full resolution. Each pixel takes its block's march (clouds.march: one pixel of each 2 x 2 block
// a frame) and last frame's result reprojected at the cloud distance, clamped to the 3 x 3
// neighbourhood of this frame's march so that moved or uncovered clouds do not ghost. The pixel
// marched this frame keeps more of its fresh value. Without a history (the first frame, an origin
// rebase, a probe capture) the march is used as it is.
import bindless;
import push_constants;
import "clouds_common";

struct ReconstructParams
{
    uint header, entry, march, dist;
    uint prev, cur, width, height;
    uint mw, mh, sampler, pad;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_clouds_reconstruct(uint3 id : SV_DispatchThreadID, uniform ReconstructParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    CloudsHeader h = clouds_header(pc.header, pc.entry);
    uint2 p = id.xy;
    int2 b = int2(min(p / 2u, uint2(pc.mw - 1u, pc.mh - 1u)));
    Texture2D<float4> march = de_tex2d(pc.march);
    float4 cur = march.Load(int3(b, 0));
    float w = h.shape.w;
    if (w > 0.0)
    {
        bool fresh = all(clouds_block_pixel(uint2(b), h.tex2.z, pc.width, pc.height) == p);
        float4 mn = cur, mx = cur;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 q = clamp(b + int2(x, y), int2(0, 0), int2(int(pc.mw) - 1, int(pc.mh) - 1));
                float4 s = march.Load(int3(q, 0));
                mn = min(mn, s);
                mx = max(mx, s);
            }
        float2 uv = (float2(p) + 0.5) / float2(pc.width, pc.height);
        float3 dir = normalize(h.ray00.xyz + h.ray_du.xyz * uv.x + h.ray_dv.xyz * uv.y);
        float dist = de_tex2d(pc.dist).Load(int3(b, 0)).r;
        float4 c = float4(h.camera.xyz + dir * min(dist, 1e6), 1.0);
        float4 clip = float4(dot(h.prev_vp[0], c), dot(h.prev_vp[1], c), dot(h.prev_vp[2], c),
                             dot(h.prev_vp[3], c));
        if (clip.w > 0.0)
        {
            float2 puv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (all(puv >= 0.0) && all(puv <= 1.0))
            {
                float4 hist = de_tex2d(pc.prev).SampleLevel(de_sampler(pc.sampler), puv, 0.0);
                if (all(hist == hist))
                    cur = lerp(cur, clamp(hist, mn, mx), fresh ? w * 0.5 : w);
            }
        }
    }
    de_rwtex2d(pc.cur)[p] = cur;
}
