// shaders/visbuffer/resolve_standard.slang: the material resolve of the visibility buffer (SPEC
// §12.5 step 4; DENG-238). Engine shader `vis_resolve`; src/render/passes/vis/resolve.c runs it
// once per view and frame, after classification (DENG-237):
//
//   cs_resolve_clear     8 x 8 threads per group over the view: the G-buffer targets (and the
//                        test-only debug targets) to 0
//   cs_feedback_clear    64 threads per group: the feedback buffer to DE_FEEDBACK_NONE, the
//                        processed-tile counters to 0
//   cs_resolve_standard  one 64-thread group per tile of a material model's tile list (indirect,
//                        on that list's DeVisDispatchArgs), for every model but Unlit: each pixel
//                        of the tile whose surface has the dispatch's model is reconstructed
//                        (resolve_common.slang), its Standard texture inputs are sampled with
//                        analytic derivatives (SampleGrad) and the thin G-buffer is written
//                        (gbuffer.slang). One pixel per tile records texture-streaming feedback.
//   cs_resolve_unlit     the same for the Unlit model: base colour into GB0 and, as radiance, into
//                        the lighting target; GB2 the model and DE_GB_FLAG_UNLIT (a minimal
//                        proof of per-model dispatch; S14 DENG-301 replaces it)
//
// The vis IDs come from classification's raw buffer copy (vis_ids_buffer). Structured so S14's
// material interface (resolve_gbuffer, DENG-298) can replace standard_surface without changing the
// pass. Texture inputs follow de_standard_material / glTF: base colour x factor x vertex colour,
// metallic-roughness (B, G) x factors, a tangent-space normal map scaled by normal_scale, occlusion
// (R) and emissive x emissive strength. Each texture slot samples UV0, or UV1 when its bit of
// DeGpuMaterial.texcoord_bits is set.
import push_constants;
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

/// Push constants of every kernel (bindless indices; 112 bytes).
struct ResolveParams
{
    uint ids;               // raw vis IDs (vis_ids_buffer, DE_ACCESS_SHADER_READ)
    uint pitch;             // its row pitch in bytes
    uint width;             // the view's size in pixels
    uint height;
    uint tile;              // classification's tile edge (8 or 16)
    uint tiles_x;
    uint tile_count;        // entries per tile list
    uint lists;             // classification's tile lists (raw u32)
    uint args;              // classification's DeVisDispatchArgs per list (raw)
    uint model;             // the dispatch's material model (its list)
    uint scene;             // DeSceneConstants table (DE_ACCESS_SHADER_READ)
    uint view_slot;
    uint visible;           // DeVisMeshlet list (both regions)
    uint visible_count;
    uint sampler;           // the material sampler (linear, wrap, anisotropic)
    uint frame;             // the frame index (feedback jitter)
    uint gb0;               // storage indices of the targets (gbuffer.slang)
    uint gb1;
    uint gb2;
    uint motion;
    uint lighting;
    uint feedback;          // raw u32 per sampled texture index (DE_ACCESS_SHADER_WRITE)
    uint feedback_capacity; // its entries
    uint counters;          // raw u32 per model: tiles processed (DE_ACCESS_SHADER_WRITE)
    uint debug_pos;         // test-only RGBA32F targets (flags DE_RESOLVE_DEBUG)
    uint debug_uv;
    uint flags;
    uint reserved;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void cs_resolve_clear(uint3 id : SV_DispatchThreadID, uniform ResolveParams pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    float4 z = float4(0.0, 0.0, 0.0, 0.0);
    de_rwtex2d(pc.gb0)[id.xy] = z;
    de_rwtex2d(pc.gb1)[id.xy] = z;
    de_rwtex2d(pc.gb2)[id.xy] = z;
    de_rwtex2d(pc.motion)[id.xy] = z;
    de_rwtex2d(pc.lighting)[id.xy] = z;
    if ((pc.flags & DE_RESOLVE_DEBUG) != 0u)
    {
        de_rwtex2d(pc.debug_pos)[id.xy] = z;
        de_rwtex2d(pc.debug_uv)[id.xy] = z;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_feedback_clear(uint3 id : SV_DispatchThreadID, uniform ResolveParams pc)
{
    if (id.x < pc.feedback_capacity)
        de_rwbytes(pc.feedback).store(id.x * 4u, DE_FEEDBACK_NONE);
    if (id.x < DE_GPU_MATERIAL_MODELS)
        de_rwbytes(pc.counters).store(id.x * 4u, 0u);
}

/// The material's texture in slot `s` (bindless index, 0 = none) and whether it samples UV1.
uint material_texture(DeGpuMaterial m, uint s, out bool uv1)
{
    uv1 = ((m.texcoord_bits >> s) & 1u) != 0u;
    return m.textures[s >> 2][s & 3u];
}

/// Everything the Standard inputs need of one point.
struct ShadeInput
{
    DeResolvePoint r;
    float2 uv[2];
    float2 dx[2];
    float2 dy[2];
    bool record; // this pixel records feedback
};

/// Samples texture slot `s` of `m` with analytic derivatives (and records its mip); `fallback`
/// without a texture.
float4 sample_slot(DeGpuMaterial m, uint s, ShadeInput in_, ResolveParams pc, float4 fallback)
{
    bool use_uv1;
    uint t = material_texture(m, s, use_uv1);
    if (t == DE_BINDLESS_INVALID)
        return fallback;
    uint set = use_uv1 ? 1u : 0u;
    if (in_.record)
        de_feedback_record(pc.feedback, pc.feedback_capacity, t, in_.dx[set], in_.dy[set]);
    return de_tex2d(t).SampleGrad(de_sampler(pc.sampler), in_.uv[set], in_.dx[set], in_.dy[set]);
}

/// The material of slot `slot`, or the engine's default (white, dielectric, roughness 0.5) for the
/// default slot, a slot past the table or a material not loaded yet.
DeGpuMaterial material_of(uint slot, DeSceneConstants sc, out bool found)
{
    found = sc.materials != DE_BINDLESS_INVALID && slot != DE_GPU_NONE &&
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
        m.texcoord_bits = 0u;
        m.model = DE_GPU_MATERIAL_MODEL_STANDARD;
        m.textures[0] = uint4(0, 0, 0, 0);
        m.textures[1] = uint4(0, 0, 0, 0);
        m.textures[2] = uint4(0, 0, 0, 0);
        m.textures[3] = uint4(0, 0, 0, 0);
        m.textures[4] = uint4(0, 0, 0, 0);
    }
    return m;
}

/// The Standard model's surface of one point, and its emissive radiance.
DeGbSurface standard_surface(DeGpuMaterial m, ShadeInput in_, ResolveParams pc,
                             DeSceneConstants sc, out float3 emissive)
{
    DeResolvePoint r = in_.r;
    float4 tangent;
    float3 geometric;
    float3 n = de_resolve_normal(r, tangent, geometric);
    // Double-sided materials face the viewer.
    float3 to_eye = sc.camera_position.xyz - de_resolve_world_pos(r);
    if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u && dot(geometric, to_eye) < 0.0)
    {
        n = -n;
        tangent.w = -tangent.w;
    }
    float4 base = m.base_color * de_resolve_color(r) *
                  sample_slot(m, DE_RESOLVE_TEX_BASE_COLOR, in_, pc, float4(1.0, 1.0, 1.0, 1.0));
    float4 mr = sample_slot(m, DE_RESOLVE_TEX_METAL_ROUGH, in_, pc, float4(1.0, 1.0, 1.0, 1.0));
    float4 nm = sample_slot(m, DE_RESOLVE_TEX_NORMAL, in_, pc, float4(0.5, 0.5, 1.0, 1.0));
    float4 ao = sample_slot(m, DE_RESOLVE_TEX_OCCLUSION, in_, pc, float4(1.0, 1.0, 1.0, 1.0));
    float4 em = sample_slot(m, DE_RESOLVE_TEX_EMISSIVE, in_, pc, float4(1.0, 1.0, 1.0, 1.0));
    bool unused;
    if (material_texture(m, DE_RESOLVE_TEX_NORMAL, unused) != DE_BINDLESS_INVALID &&
        de_vis_mesh_has_stream(r.mesh, DE_VIS_STREAM_TANGENT))
    {
        float3 tn = (nm.xyz * 2.0 - 1.0) * float3(m.pbr.z, m.pbr.z, 1.0);
        float3 t = normalize(tangent.xyz - n * dot(n, tangent.xyz));
        float3 b = cross(n, t) * tangent.w;
        n = normalize(t * tn.x + b * tn.y + n * tn.z);
    }
    DeGbSurface s;
    s.base_color = base.rgb;
    s.ao = ao.r;
    s.normal = n;
    s.roughness = saturate(m.pbr.y * mr.g);
    s.metallic = saturate(m.pbr.x * mr.b);
    s.specular = m.specular.w;
    s.model = m.model & (DE_GPU_MATERIAL_MODELS - 1u);
    s.flags = 0u;
    if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u)
        s.flags |= DE_GB_FLAG_DOUBLE_SIDED;
    if (m.clearcoat.w > 0.0 || m.extra.y > 0.0)
        s.flags |= DE_GB_FLAG_TRANSMISSION;
    if (m.extra.z > 0.0)
        s.flags |= DE_GB_FLAG_SUBSURFACE;
    if (m.clearcoat.x > 0.0)
        s.flags |= DE_GB_FLAG_CLEARCOAT;
    emissive = m.emissive.rgb * m.emissive.w * em.rgb;
    return s;
}

/// One dispatch of the tiles of model `pc.model` (Unlit when `unlit`).
void resolve_tiles(uint3 group, uint t, ResolveParams pc, bool unlit)
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
        ShadeInput in_;
        in_.r = de_resolve_point(id, p, size, sc, pc.visible, pc.visible_count);
        if (!in_.r.valid || de_resolve_model(in_.r, sc) != pc.model)
            continue;
        in_.uv[0] = de_resolve_uv(in_.r, 0u, in_.dx[0], in_.dy[0]);
        in_.uv[1] = de_resolve_uv(in_.r, 1u, in_.dx[1], in_.dy[1]);
        in_.record = i == record_pixel;
        bool found;
        DeGpuMaterial m = material_of(de_resolve_material(in_.r), sc, found);
        float3 emissive;
        DeGbSurface s;
        if (unlit)
        {
            float4 base = m.base_color * de_resolve_color(in_.r) *
                          sample_slot(m, DE_RESOLVE_TEX_BASE_COLOR, in_, pc,
                                      float4(1.0, 1.0, 1.0, 1.0));
            s.base_color = base.rgb;
            s.ao = 1.0;
            s.metallic = 0.0;
            s.specular = 0.0;
            s.model = pc.model;
            s.flags = DE_GB_FLAG_UNLIT;
            emissive = base.rgb + m.emissive.rgb * m.emissive.w;
            de_rwtex2d(pc.gb0)[p] = gb0_encode(s);
            de_rwtex2d(pc.gb2)[p] = gb2_encode(s);
        }
        else
        {
            s = standard_surface(m, in_, pc, sc, emissive);
            de_rwtex2d(pc.gb0)[p] = gb0_encode(s);
            de_rwtex2d(pc.gb1)[p] = gb1_encode(s);
            de_rwtex2d(pc.gb2)[p] = gb2_encode(s);
        }
        de_rwtex2d(pc.motion)[p] = float4(de_resolve_motion(in_.r, sc), 0.0, 0.0);
        de_rwtex2d(pc.lighting)[p] = float4(emissive, 1.0);
        if ((pc.flags & DE_RESOLVE_DEBUG) != 0u)
        {
            de_rwtex2d(pc.debug_pos)[p] = float4(de_resolve_world_pos(in_.r), 1.0);
            de_rwtex2d(pc.debug_uv)[p] = float4(in_.uv[0], in_.uv[1]);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_standard(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                         uniform ResolveParams pc)
{
    resolve_tiles(group, thread.x, pc, false);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_resolve_unlit(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                      uniform ResolveParams pc)
{
    resolve_tiles(group, thread.x, pc, true);
}
