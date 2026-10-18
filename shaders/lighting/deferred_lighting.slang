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
// Per pixel: the surface from the G-buffer (de_surface_from_gb), completed from the pixel's
// material (DENG-429: vis ID -> visible meshlet -> instance -> submesh slot, then the model's
// layers_from_material: clearcoat, sheen, anisotropy, iridescence, transmission; the triangle's
// tangent for anisotropy), the position from the depth, then
// evaluate_direct_clustered<M> (lighting_common.slang) over the view's directional lights and its
// cluster's local lights, plus evaluate_indirect_split (IBL, DENG-304). The radiance, times
// pre_exposure (the view's exposure, exposure.c), is added to
// the HDR lighting target (which already holds the emission the resolve wrote). With
// DE_LIGHTING_SPLIT every pixel also gets its diffuse and specular parts in separate targets. With
// DE_LIGHTING_SSS (DENG-306: the view's sss quality is on, the frame has a diffuse target) a
// pixel with the G-buffer's subsurface flag writes its diffuse part only to the diffuse target and
// its specular part to the lighting target; the SSS pass (passes/sss/sss.c) adds the diffuse back
// blurred. The split is the model's own (IMaterialSplitModel.evaluate_split, exact for metals).
//
// The kernel is generic over the material model (material_interface.slang); this file instantiates
// it for the built-in Standard model, which also lights the built-ins not implemented yet and game
// models (de_material_model_register) until the registry carries a lighting kernel.
import bindless;
import push_constants;
import "../lighting/lighting_common";
import "../materials/standard/standard";
import "../sprite/sprite_vis";
float de_sprite_height(uint tex,float2 uv) {
    uint w,h; de_tex2d(tex).GetDimensions(w,h);
    return de_tex2d(tex).Load(int3(min(uint2(saturate(uv)*float2(w,h)),uint2(w-1,h-1)),0)).r;
}
import "../visbuffer/gbuffer";
import "../visbuffer/resolve_common";
import "../visbuffer/vis_decode";
import "../visbuffer/vis_types";

/// Threads per group of de_lighting_tiles.
public static const uint DE_LIGHTING_GROUP = 64;

/// Push constants of every kernel (128 bytes, the limit; lighting_pass.c lp_params).
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
    // DENG-429: the pixel's material and triangle (the layers the G-buffer does not keep).
    public uint ids;           // the vis IDs (raw u32), rows `ids_pitch` bytes apart
    public uint ids_pitch;
    public uint visible;       // de_robuffer<DeVisMeshlet>: the view's visible-meshlet list
    public uint visible_count; // its entries (opaque and alpha regions)
};

/// The READY material of the pixel at `p` (through its vis ID, the visible meshlet, the instance
/// and the submesh's slot, as the resolve finds it); false for none (the surface then keeps what
/// the G-buffer gave). `id` is the pixel's vis ID.
bool pixel_material(uint2 p, DeSceneConstants sc, LightingParams pc, out uint id,
                    out DeGpuMaterial m)
{
    m = (DeGpuMaterial)0;
    id = DE_VIS_ID_EMPTY;
    if (pc.ids == DE_BINDLESS_INVALID || pc.visible == DE_BINDLESS_INVALID ||
        sc.materials == DE_BINDLESS_INVALID)
        return false;
    id = de_bytes(pc.ids).load(p.y * pc.ids_pitch + p.x * 4u);
    if (id == DE_VIS_ID_EMPTY)
        return false;
    uint entry = de_vis_id_meshlet(id);
    if (entry >= pc.visible_count)
        return false;
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[vm.instance];
    if (inst.mesh == DE_GPU_NONE || inst.mesh >= sc.mesh_count)
        return false;
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    if (mesh.meshlets == DE_BINDLESS_INVALID || vm.meshlet >= mesh.meshlet_count)
        return false;
    uint slot = de_vis_instance_material(inst, de_vis_meshlet_info(mesh, vm.meshlet).submesh);
    if (slot == DE_GPU_NONE || slot >= sc.material_count)
        return false;
    m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    return (m.flags & DE_GPU_MATERIAL_READY) != 0u;
}

/// The tangent frame of an anisotropic pixel, which the G-buffer does not keep: the triangle's
/// interpolated tangent, as the resolve computes it (resolve_common.slang), with the
/// double-sided flip. Without a tangent stream the surface keeps none (the frame is then built
/// from the normal, as in the resolve).
void pixel_tangent(inout SurfaceData s, uint id, uint2 p, DeSceneConstants sc, LightingParams pc,
                   DeGpuMaterial m, float3 v)
{
    DeResolvePoint r = de_resolve_point(id, p, float2(float(pc.width), float(pc.height)), sc,
                                        pc.visible, pc.visible_count);
    if (!r.valid || !de_vis_mesh_has_stream(r.mesh, DE_VIS_STREAM_TANGENT))
        return;
    float4 tangent;
    float3 geometric;
    de_resolve_normal(r, tangent, geometric);
    if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u && dot(geometric, v) < 0.0)
        tangent.w = -tangent.w;
    s.tangent = tangent;
}

/// One dispatch of the tiles of model pc.model through model M (thread `t` of the group).
public void de_lighting_tiles<M : IMaterialSplitModel>(uint3 group, uint t, LightingParams pc)
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
    bool has_diffuse = pc.diffuse != DE_BINDLESS_INVALID;
    bool has_specular = pc.specular != DE_BINDLESS_INVALID;
    bool split_all = has_diffuse && has_specular && (lv.flags & DE_LIGHTING_SPLIT) != 0u;
    bool sss_on = has_diffuse && (lv.flags & DE_LIGHTING_SSS) != 0u; // DENG-306
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
        // DENG-429: the layers and transmission parameters from the pixel's material, and the
        // tangent frame for anisotropy.
        uint id;
        DeGpuMaterial m;
        if (pixel_material(p, sc, pc, id, m))
        {
            M.layers_from_material(s, m);
            if (m.model == DE_GPU_MATERIAL_MODEL_SPRITE && m.extra.w != 0.0) {
                DeResolvePoint rp = de_resolve_point(id,p,float2(pc.width,pc.height),sc,
                                                    pc.visible,pc.visible_count);
                if (rp.valid) {
                    float2 dx,dy;
                    float2 uv = de_sprite_uv(m,de_resolve_uv(rp,0u,dx,dy));
                    uint htex = de_material_texture(m,18u);
                    if (htex != DE_BINDLESS_INVALID)
                        pos += s.normal * de_sprite_height(htex,uv) * m.extra.w;
                }
            }
            if ((s.features & DE_MATERIAL_FEATURE_ANISOTROPY) != 0u)
                pixel_tangent(s, id, p, sc, pc, m, v);
        }
        // DENG-306: a subsurface pixel's diffuse goes to the diffuse target only, where the SSS
        // pass blurs it; without the SSS pass it is lit like any other pixel.
        bool sss = sss_on && (g.flags & DE_GB_FLAG_SUBSURFACE) != 0u;
        bool split = sss || split_all;
        DeDirect d;
        if (lv.inputs.restir_diffuse != 0u) {
            d = evaluate_direct_clustered<M>(s, pos, v, p, lv, pc.cl, cluster, split, false);
            d.diffuse += de_input_load(lv.inputs.restir_diffuse, p, float4(0)).rgb;
            d.specular += de_input_load(lv.inputs.restir_specular, p, float4(0)).rgb;
        } else {
            d = evaluate_direct_clustered<M>(s, pos, v, p, lv, pc.cl, cluster, split);
        }
        float3 ind_d, ind_s;
        evaluate_indirect_split(s, pos, v, p, lv, ind_d, ind_s); // DENG-304: IBL
        float e = lv.pre_exposure;
        if (split)
        {
            de_rwtex2d(pc.diffuse)[p] = float4((d.diffuse + ind_d) * e, 1.0);
            if (has_specular)
                de_rwtex2d(pc.specular)[p] = float4((d.specular + ind_s) * e, 1.0);
        }
        float3 add = sss ? d.specular + ind_s : d.diffuse + d.specular + ind_d + ind_s;
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
