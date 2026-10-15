// shaders/sss/sss_separable.slang: screen-space separable subsurface scattering with the Burley
// profile (SPEC §12.6; DENG-306). Engine shader `sss_separable`; src/render/passes/sss/sss.c runs
// it per view and frame after deferred lighting, when the view's sss quality is not off:
//
//   cs_sss_clear   one thread: the tile counter to 0
//   cs_sss_tiles   8 x 8 threads per group, one group per 8 x 8 tile of the view: a tile with a
//                  G-buffer SUBSURFACE pixel is appended to the tile list
//   cs_sss_args    one thread: the indirect arguments (one group per listed tile)
//   cs_sss_blur_h  64 threads per listed tile, one per pixel; at each SUBSURFACE pixel: the
//                  material's subsurface amount and mean free path (through the pixel's vis ID:
//                  the visible-meshlet list, the instance and its material slot), the profile's d
//                  per channel (burley_profile.slang, with the base colour as albedo) into the
//                  params target, and the diffuse light blurred along x into the temp target
//   cs_sss_blur_v  the same tiles: the temp target blurred along y, then recombined into the HDR
//                  lighting target: lighting += blurred * amount + diffuse * (1 - amount)
//
// Deferred lighting (DE_LIGHTING_SSS) wrote a SUBSURFACE pixel's diffuse light only to the diffuse
// target and its specular light to the lighting target, so the recombine completes the pixel.
// Only SUBSURFACE pixels are written: every other pixel of the lighting target is untouched.
//
// The kernel. Taps come from a table (sss.c, built once at create for every odd tap count): bins
// of equal mass of the folded radial distribution of the profile (burley_profile.slang), in units
// of d, a centre bin and symmetric pairs, each tap at its bin's median. A pixel uses the largest
// of its channels' d as the reference: tap k sits at world distance s_k * d_ref along the axis,
// and channel c weighs it with the exact mass of the tap's bin under its own d_c (the
// distribution's difference over the bin; half of it for each tap of a pair). The weights of
// each channel sum to 1. World distance becomes pixels with the view's projection and the
// pixel's view depth (pixels per metre), so the world-space radius stays constant with distance.
//
// Edges. A tap on a pixel that is not SUBSURFACE, outside the view, facing away (normals more
// than 60 degrees apart) or at another depth takes the centre's value instead, so the blur keeps
// its weight and nothing bleeds across. The depth test compares the depth difference with the
// kernel's reach (4 d_ref): beyond it the tap is rejected; with follow-surface
// (DE_SSS_FOLLOW_SURFACE) the tap fades to the centre's value in proportion instead.
import bindless;
import push_constants;
import "../scene/scene_types";
import "../visbuffer/gbuffer";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";
import burley_profile;

/// Tile edge in pixels, threads per group.
public static const uint DE_SSS_TILE = 8;
public static const uint DE_SSS_GROUP = 64;
/// SssParams.flags
public static const uint DE_SSS_FOLLOW_SURFACE = 1;
/// The bins of one kernel table at most (63 taps).
public static const uint DE_SSS_MAX_BINS = 32;
/// Byte offsets in the counter buffer.
public static const uint DE_SSS_COUNTER_TILES = 0;

/// Push constants (112 bytes): bindless indices (0 = none) and sizes.
public struct SssParams
{
    public uint scene;         // scene-constants table (de_robuffer<DeSceneConstants>)
    public uint view_slot;
    public uint depth;         // sampled: the view's depth (reverse-Z)
    public uint gb0;           // sampled: thin G-buffer
    public uint gb1;
    public uint gb2;
    public uint diffuse;       // sampled: the diffuse target lighting wrote (pre-exposed)
    public uint temp;          // blur_h: storage write; blur_v: sampled
    public uint params;        // blur_h: storage write (d rgb in metres, amount); blur_v: sampled
    public uint lighting;      // blur_v: storage, read-modify-write
    public uint ids;           // raw u32 vis IDs, rows `pitch` bytes apart
    public uint pitch;
    public uint visible;       // de_robuffer<DeVisMeshlet>: the view's visible-meshlet list
    public uint visible_count; // its entries (opaque and alpha regions)
    public uint tiles;         // raw u32 tile entries (x | y << 16)
    public uint counter;       // raw u32 counters (DE_SSS_COUNTER_*)
    public uint args;          // raw DeVisDispatchArgs
    public uint kernel;        // de_robuffer<float4> of every table's bins
    public uint kernel_first;  // this table's first bin
    public uint bins;          // its bins: (taps + 1) / 2
    public uint width;
    public uint height;
    public uint tiles_x;
    public uint flags;         // DE_SSS_*
    public float max_px;       // the farthest tap in pixels
    public uint reserved0;
    public uint reserved1;
    public uint reserved2;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_sss_clear(uniform SssParams pc)
{
    de_rwbytes(pc.counter).store(DE_SSS_COUNTER_TILES, 0u);
}

groupshared uint gs_any;

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_sss_tiles(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                  uniform SssParams pc)
{
    uint t = thread.y * DE_SSS_TILE + thread.x;
    if (t == 0u)
        gs_any = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 p = group.xy * DE_SSS_TILE + thread.xy;
    if (p.x < pc.width && p.y < pc.height)
    {
        DeGbSurface g = gb_decode(float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0),
                                  de_tex2d(pc.gb2).Load(int3(int(p.x), int(p.y), 0)));
        if ((g.flags & DE_GB_FLAG_SUBSURFACE) != 0u)
            gs_any = 1u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (t == 0u && gs_any != 0u)
    {
        uint i = de_rwbytes(pc.counter).atomic_add(DE_SSS_COUNTER_TILES, 1u);
        de_rwbytes(pc.tiles).store(i * 4u, group.x | (group.y << 16));
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_sss_args(uniform SssParams pc)
{
    DeVisDispatchArgs a =
        de_vis_dispatch_args(de_rwbytes(pc.counter).load(DE_SSS_COUNTER_TILES));
    de_rwbytes(pc.args).store4(0u, uint4(a.x, a.y, a.z, a.count));
}

/// The pixel of thread `t` of a listed tile's group, false past the list or the view.
bool tile_pixel(uint3 group, uint t, SssParams pc, out uint2 p)
{
    p = uint2(0u, 0u);
    uint index = de_vis_group_index(group);
    if (index >= de_bytes(pc.args).load(12u))
        return false;
    uint packed = de_bytes(pc.tiles).load(index * 4u);
    p = uint2((packed & 0xFFFFu) * DE_SSS_TILE + t % DE_SSS_TILE,
              (packed >> 16) * DE_SSS_TILE + t / DE_SSS_TILE);
    return p.x < pc.width && p.y < pc.height;
}

/// The material of the pixel's vis ID (its subsurface amount in w, its mean free path in xyz;
/// zero when the pixel has no material that resolves).
float4 material_sss(uint2 p, DeSceneConstants sc, SssParams pc)
{
    uint id = de_bytes(pc.ids).load(p.y * pc.pitch + p.x * 4u);
    if (id == DE_VIS_ID_EMPTY)
        return float4(0.0, 0.0, 0.0, 0.0);
    uint entry = de_vis_id_meshlet(id);
    if (entry >= pc.visible_count)
        return float4(0.0, 0.0, 0.0, 0.0);
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[vm.instance];
    if (inst.mesh == DE_GPU_NONE || inst.mesh >= sc.mesh_count)
        return float4(0.0, 0.0, 0.0, 0.0);
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    if (mesh.meshlets == DE_BINDLESS_INVALID || vm.meshlet >= mesh.meshlet_count)
        return float4(0.0, 0.0, 0.0, 0.0);
    uint slot = de_vis_instance_material(inst, de_vis_meshlet_info(mesh, vm.meshlet).submesh);
    if (sc.materials == DE_BINDLESS_INVALID || slot == DE_GPU_NONE || slot >= sc.material_count)
        return float4(0.0, 0.0, 0.0, 0.0);
    DeGpuMaterial m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    if ((m.flags & DE_GPU_MATERIAL_READY) == 0u)
        return float4(0.0, 0.0, 0.0, 0.0);
    return float4(max(m.subsurface.xyz, float3(0.0, 0.0, 0.0)), saturate(m.extra.z));
}

/// The pixel's view depth (metres, positive) from the depth target.
float view_depth(uint2 p, DeSceneConstants sc, SssParams pc)
{
    float z = de_tex2d(pc.depth).Load(int3(int(p.x), int(p.y), 0)).r;
    float2 ndc = float2((float(p.x) + 0.5) / float(pc.width) * 2.0 - 1.0,
                        1.0 - (float(p.y) + 0.5) / float(pc.height) * 2.0);
    float4 c = float4(ndc, z, 1.0);
    float4 w = float4(dot(sc.inv_view_proj[0], c), dot(sc.inv_view_proj[1], c),
                      dot(sc.inv_view_proj[2], c), dot(sc.inv_view_proj[3], c));
    float3 pos = w.xyz / w.w;
    return -dot(sc.view[2], float4(pos, 1.0));
}

/// Pixels per metre at view depth `vz` along x (`horizontal`) or y.
float pixels_per_metre(DeSceneConstants sc, SssParams pc, float vz, bool horizontal)
{
    float scale = horizontal ? 0.5 * float(pc.width) * abs(sc.proj[0].x)
                             : 0.5 * float(pc.height) * abs(sc.proj[1].y);
    bool ortho = sc.proj[3].w == 1.0; // row 3 is (0, 0, -1, 0) in a perspective
    return ortho ? scale : scale / max(vz, 1e-4);
}

/// The blur of `src` at `p` along `dir` with the profile of `d` (metres per channel).
float3 blur(Texture2D<float4> src, uint2 p, int2 dir, float3 d, DeSceneConstants sc,
            SssParams pc, float3 centre, float vz, float3 n)
{
    float d_ref = max(d.x, max(d.y, d.z));
    if (!(d_ref > 0.0))
        return centre;
    float ppm = pixels_per_metre(sc, pc, vz, dir.x != 0);
    float reach = 4.0 * d_ref; // the depth difference that rejects a tap
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < pc.bins; ++k)
    {
        float4 bin = de_robuffer<float4>(pc.kernel)[pc.kernel_first + k];
        float lo = bin.y * d_ref, hi = bin.z * d_ref;
        float3 w = float3(de_burley_cdf(hi, d.x) - de_burley_cdf(lo, d.x),
                          de_burley_cdf(hi, d.y) - de_burley_cdf(lo, d.y),
                          de_burley_cdf(hi, d.z) - de_burley_cdf(lo, d.z));
        if (k == 0u)
        {
            sum += w * centre;
            continue;
        }
        float off = min(bin.x * d_ref * ppm, pc.max_px);
        int step = int(round(off));
        for (int side = -1; side <= 1; side += 2)
        {
            int2 q = int2(p) + dir * (step * side);
            float3 c = centre;
            if (step != 0 && q.x >= 0 && q.y >= 0 && q.x < int(pc.width) && q.y < int(pc.height))
            {
                int3 ql = int3(q, 0);
                DeGbSurface g = gb_decode(float4(0.0, 0.0, 0.0, 0.0), de_tex2d(pc.gb1).Load(ql),
                                          de_tex2d(pc.gb2).Load(ql));
                if ((g.flags & DE_GB_FLAG_SUBSURFACE) != 0u && dot(g.normal, n) > 0.5)
                {
                    float dz = abs(view_depth(uint2(q), sc, pc) - vz) / reach;
                    float3 s = src.Load(ql).rgb;
                    if ((pc.flags & DE_SSS_FOLLOW_SURFACE) != 0u)
                        c = lerp(s, centre, saturate(dz));
                    else if (dz <= 1.0)
                        c = s;
                }
            }
            sum += 0.5 * w * c;
        }
    }
    return sum;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sss_blur_h(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                   uniform SssParams pc)
{
    uint2 p;
    if (!tile_pixel(group, thread.x, pc, p))
        return;
    int3 pl = int3(int(p.x), int(p.y), 0);
    DeGbSurface g = gb_decode(de_tex2d(pc.gb0).Load(pl), de_tex2d(pc.gb1).Load(pl),
                              de_tex2d(pc.gb2).Load(pl));
    if ((g.flags & DE_GB_FLAG_SUBSURFACE) == 0u)
        return;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    float4 mat = material_sss(p, sc, pc);
    float3 d = de_burley_d3(mat.xyz, g.base_color);
    float amount = mat.w;
    if (!(amount > 0.0))
        d = float3(0.0, 0.0, 0.0);
    float3 centre = de_tex2d(pc.diffuse).Load(pl).rgb;
    float vz = view_depth(p, sc, pc);
    float3 h = blur(de_tex2d(pc.diffuse), p, int2(1, 0), d, sc, pc, centre, vz, g.normal);
    de_rwtex2d(pc.temp)[p] = float4(h, 1.0);
    de_rwtex2d(pc.params)[p] = float4(d, amount);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_sss_blur_v(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                   uniform SssParams pc)
{
    uint2 p;
    if (!tile_pixel(group, thread.x, pc, p))
        return;
    int3 pl = int3(int(p.x), int(p.y), 0);
    DeGbSurface g = gb_decode(float4(0.0, 0.0, 0.0, 0.0), de_tex2d(pc.gb1).Load(pl),
                              de_tex2d(pc.gb2).Load(pl));
    if ((g.flags & DE_GB_FLAG_SUBSURFACE) == 0u)
        return;
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    float4 prm = de_tex2d(pc.params).Load(pl);
    float3 diffuse = de_tex2d(pc.diffuse).Load(pl).rgb;
    float3 result = diffuse;
    if (prm.w > 0.0)
    {
        float3 centre = de_tex2d(pc.temp).Load(pl).rgb;
        float vz = view_depth(p, sc, pc);
        float3 v = blur(de_tex2d(pc.temp), p, int2(0, 1), prm.xyz, sc, pc, centre, vz, g.normal);
        result = v * prm.w + diffuse * (1.0 - prm.w);
    }
    RWTexture2D<float4> lighting = de_rwtex2d(pc.lighting);
    float4 lit = lighting[p];
    lighting[p] = float4(lit.rgb + result, lit.a);
}
