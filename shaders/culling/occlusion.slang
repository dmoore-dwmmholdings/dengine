// shaders/culling/occlusion.slang: the HZB occlusion test (SPEC §12.5 step 1; DENG-233).
//
//     import occlusion;   // from shaders/culling/ (cull_common.slang imports it)
//
// The HZB of a view (src/render/passes/vis/hzb.c, built by hzb_build.slang) holds, per texel, the
// minimum reverse-Z depth of the depth texels it covers: the farthest depth, the most conservative
// occluder. Mip 0 covers the whole viewport at the previous power of two of the depth size.
//
// de_occlusion_visible projects the 8 corners of a box with the culling view-projection:
//   - a corner at or in front of the near plane (w <= 0 or z >= w): visible (the box crosses it);
//   - the screen rectangle, widened by half a mip-0 texel, picks the coarsest mip at which it
//     covers at most 2 x 2 texels;
//   - the box's nearest depth (the largest z / w of its corners: depth is monotonic along view z,
//     and a box's nearest point to the camera plane is a corner) against the minimum of those
//     texels: occluded when it is smaller, i.e. behind every occluder there (with a relative
//     bias of 2^-15 toward visible, for rounding).
// A box entirely off screen is left to the frustum test (visible here).
//
// The engine shader "vis_occlusion" (shaders/engine_shaders.cmake) is this file: its kernel
// cs_hzb_debug draws one HZB mip into a colour texture for r.hzb_debug.
module occlusion;

import push_constants;

/// What the test needs of a view's HZB.
public struct DeHzbInfo
{
    public uint tex;    // sampled bindless index of the HZB (all mips, Load)
    public uint width;  // mip 0 size
    public uint height;
    public uint mips;
};

/// True when the box (render-origin space) may be visible over the HZB `h` seen through the
/// view-projection rows `vp`.
public bool de_occlusion_visible(float4 vp[4], float3 mn, float3 mx, DeHzbInfo h)
{
    float2 lo = float2(1e30, 1e30);
    float2 hi = float2(-1e30, -1e30);
    float nearest = 0.0;
    for (uint c = 0; c < 8; ++c)
    {
        float4 p = float4((c & 1) != 0 ? mx.x : mn.x, (c & 2) != 0 ? mx.y : mn.y,
                          (c & 4) != 0 ? mx.z : mn.z, 1.0);
        float4 clip = float4(dot(vp[0], p), dot(vp[1], p), dot(vp[2], p), dot(vp[3], p));
        if (clip.w <= 1e-6 || clip.z >= clip.w)
            return true; // at or in front of the near plane
        float3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearest = max(nearest, ndc.z);
    }
    // Normalised screen coordinates, y down (row 0 of the depth texture is the top).
    float2 uvlo = float2(lo.x * 0.5 + 0.5, 0.5 - hi.y * 0.5);
    float2 uvhi = float2(hi.x * 0.5 + 0.5, 0.5 - lo.y * 0.5);
    if (uvhi.x < 0.0 || uvhi.y < 0.0 || uvlo.x > 1.0 || uvlo.y > 1.0)
        return true;
    float2 size = float2(float(h.width), float(h.height));
    float2 pad = 0.5 / size; // half a mip-0 texel: float rounding never shrinks the rectangle
    int2 t0 = int2(floor(saturate(uvlo - pad) * size));
    int2 t1 = int2(floor(saturate(uvhi + pad) * size));
    int2 last = int2(int(h.width) - 1, int(h.height) - 1);
    t0 = clamp(t0, int2(0, 0), last);
    t1 = clamp(t1, int2(0, 0), last);
    uint mip = 0;
    while (mip + 1 < h.mips && (((t1.x >> mip) - (t0.x >> mip)) > 1 || ((t1.y >> mip) - (t0.y >> mip)) > 1))
        ++mip;
    int2 a = t0 >> mip;
    int2 b = t1 >> mip;
    Texture2D<float4> t = de_tex2d(h.tex);
    float d = t.Load(int3(a.x, a.y, int(mip))).r;
    d = min(d, t.Load(int3(b.x, a.y, int(mip))).r);
    d = min(d, t.Load(int3(a.x, b.y, int(mip))).r);
    d = min(d, t.Load(int3(b.x, b.y, int(mip))).r);
    // Biased toward visible by 2^-15 (relative): a face perpendicular to the view has its corner
    // depth equal to the depth the raster wrote for it, and the two are computed differently.
    return nearest * (1.0 + 1.0 / 32768.0) >= d;
}

/// r.hzb_debug: one HZB mip drawn into a colour texture of the view's size.
struct HzbDebugParams
{
    uint hzb;    // sampled bindless index
    uint mip;
    uint dst;    // storage bindless index of the colour texture (RGBA16F)
    uint width;  // its size
    uint height;
    uint hzb_width; // mip 0 size
    uint hzb_height;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hzb_debug(uint3 id : SV_DispatchThreadID, uniform HzbDebugParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    uint mw = max(pc.hzb_width >> pc.mip, 1u);
    uint mh = max(pc.hzb_height >> pc.mip, 1u);
    uint x = min(id.x * mw / pc.width, mw - 1u);
    uint y = min(id.y * mh / pc.height, mh - 1u);
    float d = de_tex2d(pc.hzb).Load(int3(int(x), int(y), int(pc.mip))).r;
    // Reverse-Z depth falls fast with distance: a fourth root keeps far depths readable.
    float v = sqrt(sqrt(saturate(d)));
    de_rwtex2d(pc.dst)[id.xy] = float4(v, v, v, 1.0);
}
