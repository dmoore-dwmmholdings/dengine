// shaders/visbuffer/resolve_model.slang: the material resolve, generic over the material model
// (SPEC §12.5 step 4, §12.6; DENG-238, DENG-298). No entry points: resolve_standard.slang
// (engine shader `vis_resolve`) instantiates de_resolve_tiles for the built-in models, and a
// game's material model shader (de_material_model_register) for its own:
//
//     import "visbuffer/resolve_model";
//     struct ToonModel : IMaterialModel { ... }
//     [shader("compute")]
//     [numthreads(64, 1, 1)]
//     void cs_resolve_model(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
//                           uniform ResolveParams pc)
//     {
//         de_resolve_tiles<ToonModel>(group, thread.x, pc);
//     }
//
// de_resolve_tiles<M> runs one 64-thread group per tile of model pc.model's tile list
// (classification, DENG-237; an indirect dispatch on that list's DeVisDispatchArgs): each pixel of
// the tile whose surface has that model is reconstructed (resolve_common.slang) into
// VertexAttribs and a TextureLodInfo (analytic UV derivatives; one pixel per tile records
// texture-streaming feedback), M.load_params reads the material record, M.resolve_gbuffer gives
// the SurfaceData, and the thin G-buffer (gbuffer.slang), the motion vector and the emission go
// to the targets. A surface with DE_MATERIAL_FEATURE_UNLIT keeps GB1 cleared.
//
// The models are elsewhere: Standard in materials/standard/standard.slang (DENG-299), Unlit in
// materials/unlit/unlit.slang (DENG-301).
module resolve_model;

import bindless;
__exported import "../materials/material_interface";
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";
import "../visbuffer/resolve_common";
import "../visbuffer/gbuffer";
import "../visbuffer/feedback";

/// ResolveParams.flags
public static const uint DE_RESOLVE_DEBUG = 1; // also write the debug position and UV targets
/// Threads per group of the per-tile kernels.
public static const uint DE_RESOLVE_GROUP = 64;
/// de_cooked_tex_slot values the Standard model samples.
public static const uint DE_RESOLVE_TEX_BASE_COLOR = 0;
public static const uint DE_RESOLVE_TEX_METAL_ROUGH = 1;
public static const uint DE_RESOLVE_TEX_NORMAL = 2;
public static const uint DE_RESOLVE_TEX_OCCLUSION = 3;
public static const uint DE_RESOLVE_TEX_EMISSIVE = 4;

/// Push constants of every resolve kernel (bindless indices; 112 bytes; resolve.c rs_params).
public struct ResolveParams
{
    public uint ids;               // raw vis IDs (vis_ids_buffer, DE_ACCESS_SHADER_READ)
    public uint pitch;             // its row pitch in bytes
    public uint width;             // the view's size in pixels
    public uint height;
    public uint tile;              // classification's tile edge (8 or 16)
    public uint tiles_x;
    public uint tile_count;        // entries per tile list
    public uint lists;             // classification's tile lists (raw u32)
    public uint args;              // classification's DeVisDispatchArgs per list (raw)
    public uint model;             // the dispatch's material model (its list)
    public uint scene;             // DeSceneConstants table (DE_ACCESS_SHADER_READ)
    public uint view_slot;
    public uint visible;           // DeVisMeshlet list (both regions)
    public uint visible_count;
    public uint sampler;           // the material sampler (linear, wrap, anisotropic)
    public uint frame;             // the frame index (feedback jitter)
    public uint gb0;               // storage indices of the targets (gbuffer.slang)
    public uint gb1;
    public uint gb2;
    public uint motion;
    public uint lighting;
    public uint feedback;          // raw u32 per sampled texture index (DE_ACCESS_SHADER_WRITE)
    public uint feedback_capacity; // its entries
    public uint counters;          // raw u32 per model: tiles processed (DE_ACCESS_SHADER_WRITE)
    public uint debug_pos;         // test-only RGBA32F targets (flags DE_RESOLVE_DEBUG)
    public uint debug_uv;
    public uint flags;
    public uint reserved;
};

/// The material of slot `slot`, or the engine's default (white, dielectric, roughness 0.5) for the
/// default slot, a slot past the table or a material not loaded yet.
// Keep the large reconstruction records in the caller on Metal. Slang otherwise emits
// by-value helper boundaries and thread-local copies; inlining before MSL emission lets the
// native compiler eliminate those copies. Other targets and the reconstruction math are unchanged.
#if defined(DE_SHADER_TARGET_METAL) && defined(DE_VIS_RESOLVE_INLINE)
[ForceInline]
#endif
public DeGpuMaterial de_resolve_material_of(uint slot, DeSceneConstants sc)
{
    bool found = sc.materials != DE_BINDLESS_INVALID && slot != DE_GPU_NONE &&
                 slot < sc.material_count;
    DeGpuMaterial m = {};
    if (found)
    {
        m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
        found = (m.flags & DE_GPU_MATERIAL_READY) != 0u;
    }
    if (!found)
    {
        m.base_color = float4(1.0, 1.0, 1.0, 1.0);
        m.emissive = float4(0.0, 0.0, 0.0, 0.0);
        m.pbr = float4(0.0, 0.5, 1.0, 0.5);
        m.specular = float4(1.0, 1.0, 1.0, 1.0);
        m.clearcoat = float4(0.0, 0.0, 1.5, 0.0);
        m.extra = float4(0.0, 0.0, 0.0, 0.0);
        m.flags = 0u;
        m.features = 0u;
        m.model = DE_GPU_MATERIAL_MODEL_STANDARD;
        m.textures[0] = uint4(0, 0, 0, 0);
        m.textures[1] = uint4(0, 0, 0, 0);
        m.textures[2] = uint4(0, 0, 0, 0);
        m.textures[3] = uint4(0, 0, 0, 0);
        m.textures[4] = uint4(0, 0, 0, 0);
    }
    return m;
}

/// The bindless index of texture slot `s` of `m` (0 = none) and the UV set it samples.
#if defined(DE_SHADER_TARGET_METAL) && defined(DE_VIS_RESOLVE_INLINE)
[ForceInline]
#endif
public uint de_resolve_texture(DeGpuMaterial m, uint s, out uint set)
{
    set = de_material_texcoord(m, s);
    return de_material_texture(m, s);
}

/// Samples texture slot `s` of `m` at the point; `fallback` without a texture.
#if defined(DE_SHADER_TARGET_METAL) && defined(DE_VIS_RESOLVE_INLINE)
[ForceInline]
#endif
public float4 de_resolve_sample(DeGpuMaterial m, uint s, VertexAttribs v, TextureLodInfo lod,
                                float4 fallback)
{
    uint set;
    uint t = de_resolve_texture(m, s, set);
    if(t!=DE_BINDLESS_INVALID && (m.textures[s>>2][s&3]&DE_GPU_MATERIAL_TEXTURE_POINT)!=0) {
        uint w,h;de_tex2d(t).GetDimensions(w,h);
        int2 pixel=clamp(int2(floor(v.uv[set]*float2(w,h))),int2(0),int2(w,h)-1);
        return de_tex2d(t).Load(int3(pixel,0));
    }
    return de_material_sample(lod, v, t, set, fallback);
}

/// The vertex data of resolve point `r` seen from `eye`.
#if defined(DE_SHADER_TARGET_METAL) && defined(DE_VIS_RESOLVE_INLINE)
[ForceInline]
#endif
public VertexAttribs de_resolve_attribs(DeResolvePoint r, float3 eye, out TextureLodInfo lod)
{
    VertexAttribs v;
    v.position = de_resolve_world_pos(r);
    v.normal = de_resolve_normal(r, v.tangent, v.geometric_normal);
    float3 to_eye = eye - v.position;
    float l2 = dot(to_eye, to_eye);
    v.view = l2 > 0.0 ? to_eye * rsqrt(l2) : v.normal;
    v.color = de_resolve_color(r);
    v.uv[0] = de_resolve_uv(r, 0u, lod.dx[0], lod.dy[0]);
    v.uv[1] = de_resolve_uv(r, 1u, lod.dx[1], lod.dy[1]);
    v.has_tangent = de_vis_mesh_has_stream(r.mesh, DE_VIS_STREAM_TANGENT);
    v.object_scale = pow(abs(de_vis_transform_det(r.inst.transform)), 1.0 / 3.0); // DENG-301
    lod.sampler = 0u;
    lod.feedback = 0u;
    lod.feedback_capacity = 0u;
    lod.record = false;
    return v;
}

/// One dispatch of the tiles of model pc.model through model M (thread `t` of the group).
#if defined(DE_SHADER_TARGET_METAL) && defined(DE_VIS_RESOLVE_INLINE)
[ForceInline]
#endif
public void de_resolve_tiles<M : IMaterialModel>(uint3 group, uint t, ResolveParams pc)
{
    uint index = de_vis_group_index(group);
    uint count = de_bytes(pc.args).load(pc.model * 16u + 12u);
    if (index >= count)
        return;
    uint packed = de_bytes(pc.lists).load((pc.model * pc.tile_count + index) * 4u);
    uint tx = packed & 0xFFFFu, ty = packed >> 16;
    if (t == 0u)
        de_rwbytes(pc.counters).atomic_add(pc.model * 4u, 1u);
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    uint record_pixel = de_feedback_pixel(ty * pc.tiles_x + tx, pc.frame, pc.tile);
    float2 size = float2(float(pc.width), float(pc.height));
    DeBytes ids = de_bytes(pc.ids);
    for (uint i = t; i < pc.tile * pc.tile; i += DE_RESOLVE_GROUP)
    {
        uint2 p = uint2(tx * pc.tile + i % pc.tile, ty * pc.tile + i / pc.tile);
        if (p.x >= pc.width || p.y >= pc.height)
            continue;
        uint id = ids.load(p.y * pc.pitch + p.x * 4u);
        if (id == DE_VIS_ID_EMPTY)
            continue;
        DeResolvePoint r = de_resolve_point(id, p, size, sc, pc.visible, pc.visible_count);
        if (!r.valid || de_resolve_model(r, sc) != pc.model)
            continue;
        TextureLodInfo lod;
        VertexAttribs v = de_resolve_attribs(r, sc.camera_position.xyz, lod);
        lod.sampler = pc.sampler;
        lod.feedback = pc.feedback;
        lod.feedback_capacity = pc.feedback_capacity;
        lod.record = i == record_pixel;
        DeGpuMaterial m = de_resolve_material_of(de_resolve_material(r), sc);
        SurfaceData s = M.resolve_gbuffer(M.load_params(m), v, lod);
        s.model = pc.model;
        DeGbSurface g = de_surface_to_gb(s);
        de_rwtex2d(pc.gb0)[p] = gb0_encode(g);
        if ((s.features & DE_MATERIAL_FEATURE_UNLIT) == 0u)
            de_rwtex2d(pc.gb1)[p] = gb1_encode(g);
        de_rwtex2d(pc.gb2)[p] = gb2_encode(g);
        de_rwtex2d(pc.motion)[p] = float4(de_resolve_motion(r, sc), 0.0, 0.0);
        // DENG-304: emission is pre-exposed like every radiance in the HDR lighting target.
        float exposure = sc.pre_exposure > 0.0 ? sc.pre_exposure : 1.0;
        de_rwtex2d(pc.lighting)[p] = float4(s.emissive * exposure, 1.0);
        if ((pc.flags & DE_RESOLVE_DEBUG) != 0u)
        {
            de_rwtex2d(pc.debug_pos)[p] = float4(v.position, 1.0);
            de_rwtex2d(pc.debug_uv)[p] = float4(v.uv[0], v.uv[1]);
        }
    }
}
