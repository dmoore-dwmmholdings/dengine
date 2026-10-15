// shaders/lighting/deferred_lighting.slang: deferred direct lighting over the thin G-buffer (SPEC
// §12.5 step 5; DENG-303). Engine shader `lighting_deferred`; src/render/lighting/lighting_pass.c
// runs, per view and frame, after the material resolve (DENG-238):
//
//   cs_lighting_clear_split  8 x 8 threads per group: the diffuse and specular targets to 0 (only
//                            when the frame has them)
//   cs_lighting_standard     de_lighting_tiles<StandardModel>: one 64-thread group per tile
//                            of a material model's tile list (classification, DENG-237; an indirect
//                            dispatch per model), one thread per pixel of the tile whose G-buffer
//                            model is the dispatch's model. Unlit (model 1) is never dispatched,
//                            and an Unlit pixel is never touched.
//   cs_cluster_heatmap       8 x 8 threads per group: the debug view of the cluster light counts
//                            (r.cluster_debug)
//
// Per pixel: the surface from the G-buffer (de_surface_from_gb), the position from the depth, then
// evaluate_direct_clustered<M> (lighting_common.slang) over the view's directional lights and its
// cluster's local lights, plus evaluate_indirect. The radiance, times pre_exposure, is added to
// the HDR lighting target (which already holds the emission the resolve wrote). Split pixels
// (DE_LIGHTING_SPLIT, or the G-buffer's subsurface flag) also get their diffuse and specular parts
// in separate targets when the frame has them; a subsurface pixel's diffuse part then goes only to
// the diffuse target (the SSS pass, DENG-306, adds it back blurred).
//
// The kernel is generic over the material model (material_interface.slang); this file instantiates
// it for the built-in Standard model, which also lights the built-ins not implemented yet and game
// models (de_material_model_register) until the registry carries a lighting kernel.
import bindless;
import push_constants;
import "../lighting/lighting_common";
import "../materials/standard/standard";
import "../visbuffer/gbuffer";
import "../visbuffer/vis_types";

/// Threads per group of de_lighting_tiles.
public static const uint DE_LIGHTING_GROUP = 64;

/// Push constants of every kernel (112 bytes; lighting_pass.c lp_params).
public struct LightingParams
{
    public uint view;       // DeLightingView ring (de_robuffer)
    public uint entry;      // this frame's entry
    public uint scene;      // DeSceneConstants table
    public uint view_slot;
    public uint depth;      // the view's depth (sampled, reverse-Z)
    public uint gb0;        // the G-buffer (sampled)
    public uint gb1;
    public uint gb2;
    public uint lighting;   // the HDR lighting target (storage, read-modify-write)
    public uint diffuse;    // split targets (storage; 0 = none this frame)
    public uint specular;
    public uint lists;      // classification's tile lists (raw u32)
    public uint args;       // classification's DeVisDispatchArgs per list (raw)
    public uint tile;       // classification's tile edge
    public uint tiles_x;
    public uint tile_count; // entries per tile list
    public uint model;      // this dispatch's material model
    public uint width;      // the view's internal size
    public uint height;
    public uint tile_counters; // raw u32 per model: tiles lit (tests)
    public DeClusterLists cl;
};

/// One dispatch of the tiles of model pc.model through model M (thread `t` of the group).
public void de_lighting_tiles<M : IMaterialModel>(uint3 group, uint t, LightingParams pc)
{
    uint index = de_vis_group_index(group);
    uint count = de_bytes(pc.args).load(pc.model * 16u + 12u);
    if (index >= count)
        return;
    uint packed = de_bytes(pc.lists).load((pc.model * pc.tile_count + index) * 4u);
    uint tx = packed & 0xFFFFu, ty = packed >> 16;
    if (t == 0u)
        de_rwbytes(pc.tile_counters).atomic_add(pc.model * 4u, 1u);
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    Texture2D<float4> depth = de_tex2d(pc.depth);
    Texture2D<float4> gb0 = de_tex2d(pc.gb0);
    Texture2D<float4> gb1 = de_tex2d(pc.gb1);
    Texture2D<float4> gb2 = de_tex2d(pc.gb2);
    RWTexture2D<float4> lighting = de_rwtex2d(pc.lighting);
    bool targets = pc.diffuse != DE_BINDLESS_INVALID && pc.specular != DE_BINDLESS_INVALID;
    bool ortho = (lv.flags & DE_LIGHTING_ORTHO) != 0u;
    float3 forward_eye = normalize(sc.view[2].xyz); // towards the eye (view space +z)
    float2 inv_size = float2(1.0 / float(pc.width), 1.0 / float(pc.height));
    for (uint i = t; i < pc.tile * pc.tile; i += DE_LIGHTING_GROUP)
    {
        uint2 p = uint2(tx * pc.tile + i % pc.tile, ty * pc.tile + i / pc.tile);
        if (p.x >= pc.width || p.y >= pc.height)
            continue;
        int3 pl = int3(int(p.x), int(p.y), 0);
        float4 lit = lighting[p];
        if (lit.a < 0.5)
            continue; // no surface
        DeGbSurface g = gb_decode(gb0.Load(pl), gb1.Load(pl), gb2.Load(pl));
        if (g.model != pc.model || (g.flags & DE_GB_FLAG_UNLIT) != 0u)
            continue;
        float z = depth.Load(pl).r;
        if (!(z > 0.0))
            continue;
        float2 ndc = float2((float(p.x) + 0.5) * inv_size.x * 2.0 - 1.0,
                            1.0 - (float(p.y) + 0.5) * inv_size.y * 2.0);
        float4 c = float4(ndc, z, 1.0);
        float4 w = float4(dot(sc.inv_view_proj[0], c), dot(sc.inv_view_proj[1], c),
                          dot(sc.inv_view_proj[2], c), dot(sc.inv_view_proj[3], c));
        float3 pos = w.xyz / w.w;
        float3 v = ortho ? forward_eye : normalize(sc.camera_position.xyz - pos);
        float view_depth = -dot(sc.view[2], float4(pos, 1.0));
        uint cluster = de_cluster_index(lv, p, view_depth);
        SurfaceData s = de_surface_from_gb(g);
        s.brdf_lut = lv.brdf_lut; // the Standard model's energy LUT (DENG-299)
        bool sss = (g.flags & DE_GB_FLAG_SUBSURFACE) != 0u;
        bool split = targets && (sss || (lv.flags & DE_LIGHTING_SPLIT) != 0u);
        DeDirect d = evaluate_direct_clustered<M>(s, pos, v, p, lv, pc.cl, cluster, split);
        float3 indirect = evaluate_indirect(s, pos, v, p, lv);
        float e = lv.pre_exposure;
        if (split)
        {
            de_rwtex2d(pc.diffuse)[p] = float4(d.diffuse * e, 1.0);
            de_rwtex2d(pc.specular)[p] = float4(d.specular * e, 1.0);
        }
        float3 add = sss && split ? d.specular + indirect : d.diffuse + d.specular + indirect;
        lighting[p] = float4(lit.rgb + add * e, lit.a);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_lighting_standard(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                          uniform LightingParams pc)
{
    de_lighting_tiles<StandardModel>(group, thread.x, pc);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_lighting_clear_split(uint3 id : SV_DispatchThreadID, uniform LightingParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    float4 z = float4(0.0, 0.0, 0.0, 0.0);
    de_rwtex2d(pc.diffuse)[id.xy] = z;
    de_rwtex2d(pc.specular)[id.xy] = z;
}

/// The heat colour of `n` lights (0 black, then blue -> green -> yellow -> red at 32+).
float3 heat(uint n)
{
    if (n == 0u)
        return float3(0.0, 0.0, 0.0);
    float x = saturate(float(n) / 32.0);
    return saturate(float3(2.0 * x - 0.5, 1.5 - abs(2.0 * x - 1.0) * 1.5, 1.0 - 2.0 * x));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_cluster_heatmap(uint3 id : SV_DispatchThreadID, uniform LightingParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    float z = de_tex2d(pc.depth).Load(int3(int(id.x), int(id.y), 0)).r;
    float view_depth = lv.near;
    if (z > 0.0)
    {
        float2 ndc = float2((float(id.x) + 0.5) / float(pc.width) * 2.0 - 1.0,
                            1.0 - (float(id.y) + 0.5) / float(pc.height) * 2.0);
        float4 c = float4(ndc, z, 1.0);
        float4 w = float4(dot(sc.inv_view_proj[0], c), dot(sc.inv_view_proj[1], c),
                          dot(sc.inv_view_proj[2], c), dot(sc.inv_view_proj[3], c));
        view_depth = -dot(sc.view[2], float4(w.xyz / w.w, 1.0));
    }
    uint cluster = de_cluster_index(lv, id.xy, view_depth);
    uint n = de_robuffer<uint2>(pc.cl.grid)[cluster].y;
    de_rwtex2d(pc.lighting)[id.xy] = float4(heat(n), 1.0);
}
