// shaders/culling/hzb_build.slang: the hierarchical depth buffer (SPEC §12.5 step 1; DENG-233).
// The engine shader "vis_hzb_build" (shaders/engine_shaders.cmake); src/render/passes/vis/hzb.c
// runs it.
//
// The HZB is an R32F texture with a full mip chain. Mip 0 has the previous power of two of the
// source depth's size in each dimension (between half and the whole size), so every mip halves
// exactly. Each texel holds the reduction (`op`) of the source texels it covers:
//
//   DE_HZB_OP_MIN  the minimum: with reverse-Z the farthest depth, the conservative occluder
//                  (occlusion culling, DENG-233)
//   DE_HZB_OP_MAX  the maximum: the nearest depth (the closest-depth pyramid of S15 DENG-339
//                  reuses this shader with this op)
//
// Kernels, in this order:
//   cs_hzb_level0   mip 0 from the source (a sampled texture: the D32 depth, or any float
//                   texture). Texel x covers source texels floor(x * sw / dw) to
//                   ceil((x + 1) * sw / dw) - 1 (at most 3 per axis, as sw / dw < 2): every
//                   source texel that overlaps it, so odd and non-power-of-two sizes stay
//                   conservative.
//   cs_hzb_reduce6  single-pass style (SPD): one 256-thread group reduces a 64 x 64 block of mip
//                   b into up to 6 mips (32 x 32 .. 1 x 1) through groupshared memory; one
//                   dispatch per 6 mips (two for a 4096-wide HZB). No cross-group tail: reading
//                   another group's texture writes within one dispatch is not portable to Metal.
//   cs_hzb_reduce   one mip from the one before (the multi-dispatch path, r.hzb_single_pass 0).
// Mips past b are read through their storage views (the pass declares them read + write, so they
// are in the shader-write state); texels past a mip's edge clamp to it, which never changes a
// min or a max.
import push_constants;

static const uint DE_HZB_OP_MIN = 0;
static const uint DE_HZB_OP_MAX = 1;

struct HzbParams
{
    uint src;        // level0: sampled bindless index of the source; reduce: storage index of mip b
    uint src_width;  // the source's (or mip b's) size
    uint src_height;
    uint dst_width;  // the size of the first mip written
    uint dst_height;
    uint count;      // mips written (reduce6: 1 .. 6)
    uint op;         // DE_HZB_OP_*
    uint reserved;
    uint4 dst[2];    // storage bindless index of each mip written, in order (up to 6)
};

float reduce_op(float a, float b, uint op)
{
    return op == DE_HZB_OP_MAX ? max(a, b) : min(a, b);
}

uint dst_index(HzbParams pc, uint i)
{
    return i < 4u ? pc.dst[0][i] : pc.dst[1][i - 4u];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hzb_level0(uint3 id : SV_DispatchThreadID, uniform HzbParams pc)
{
    if (id.x >= pc.dst_width || id.y >= pc.dst_height)
        return;
    uint x0 = id.x * pc.src_width / pc.dst_width;
    uint x1 = min(((id.x + 1u) * pc.src_width + pc.dst_width - 1u) / pc.dst_width, pc.src_width) - 1u;
    uint y0 = id.y * pc.src_height / pc.dst_height;
    uint y1 = min(((id.y + 1u) * pc.src_height + pc.dst_height - 1u) / pc.dst_height, pc.src_height) - 1u;
    Texture2D<float4> src = de_tex2d(pc.src);
    float v = src.Load(int3(int(x0), int(y0), 0)).r;
    for (uint y = y0; y <= y1; ++y)
        for (uint x = x0; x <= x1; ++x)
            v = reduce_op(v, src.Load(int3(int(x), int(y), 0)).r, pc.op);
    de_rwtex2d(dst_index(pc, 0))[id.xy] = float4(v, 0.0, 0.0, 0.0);
}

/// The reduction of the 2 x 2 texels of `src` under texel `p` of the next mip (clamped).
float reduce_2x2(RWTexture2D<float4> src, uint2 p, uint2 size, uint op)
{
    uint2 last = size - 1u;
    uint2 a = min(p * 2u, last);
    uint2 b = min(p * 2u + 1u, last);
    float v = src[a].r;
    v = reduce_op(v, src[uint2(b.x, a.y)].r, op);
    v = reduce_op(v, src[uint2(a.x, b.y)].r, op);
    v = reduce_op(v, src[b].r, op);
    return v;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_hzb_reduce(uint3 id : SV_DispatchThreadID, uniform HzbParams pc)
{
    if (id.x >= pc.dst_width || id.y >= pc.dst_height)
        return;
    float v = reduce_2x2(de_rwtex2d(pc.src), id.xy, uint2(pc.src_width, pc.src_height), pc.op);
    de_rwtex2d(dst_index(pc, 0))[id.xy] = float4(v, 0.0, 0.0, 0.0);
}

groupshared float g_tile[32][32];

[shader("compute")]
[numthreads(16, 16, 1)]
void cs_hzb_reduce6(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uniform HzbParams pc)
{
    RWTexture2D<float4> src = de_rwtex2d(pc.src);
    uint2 src_size = uint2(pc.src_width, pc.src_height);
    // First mip: each thread makes 2 x 2 texels of the group's 32 x 32 block.
    uint2 base = gid.xy * 32u;
    for (uint j = 0; j < 2u; ++j)
    {
        for (uint i = 0; i < 2u; ++i)
        {
            uint2 local = tid.xy * 2u + uint2(i, j);
            uint2 p = base + local;
            float v = reduce_2x2(src, p, src_size, pc.op);
            g_tile[local.y][local.x] = v;
            if (p.x < pc.dst_width && p.y < pc.dst_height)
                de_rwtex2d(dst_index(pc, 0))[p] = float4(v, 0.0, 0.0, 0.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Further mips from groupshared memory: 16 x 16, 8 x 8, ... 1 x 1 per group.
    uint side = 16u;
    for (uint m = 1u; m < pc.count; ++m)
    {
        float v = 0.0;
        bool active = tid.x < side && tid.y < side;
        if (active)
        {
            uint2 q = tid.xy * 2u;
            v = g_tile[q.y][q.x];
            v = reduce_op(v, g_tile[q.y][q.x + 1u], pc.op);
            v = reduce_op(v, g_tile[q.y + 1u][q.x], pc.op);
            v = reduce_op(v, g_tile[q.y + 1u][q.x + 1u], pc.op);
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            g_tile[tid.y][tid.x] = v;
            uint2 p = gid.xy * side + tid.xy;
            uint mw = max(pc.dst_width >> m, 1u);
            uint mh = max(pc.dst_height >> m, 1u);
            if (p.x < mw && p.y < mh)
                de_rwtex2d(dst_index(pc, m))[p] = float4(v, 0.0, 0.0, 0.0);
        }
        GroupMemoryBarrierWithGroupSync();
        side >>= 1u;
    }
}
