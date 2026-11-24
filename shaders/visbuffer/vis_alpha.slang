// shaders/visbuffer/vis_alpha.slang: the alpha-tested vis-buffer raster (SPEC §12.5 step 2,
// §12.6; DENG-236). The engine shader "vis_alpha" (shaders/engine_shaders.cmake);
// src/render/passes/vis/raster_alpha.c runs it, per phase, right after that phase's opaque raster.
//
// Meshlet culling (meshlet_cull.slang) puts the meshlets whose submesh material is alpha-tested
// (alpha_mode mask) in the alpha bucket: a region of the same visible-meshlet list, so their
// packed vis IDs share the opaque ones' index space. They draw into the same targets as the opaque
// raster (the R32_UINT vis ID and the D32 reverse-Z depth), with the same triangle rule (back faces
// of single-sided materials, zero area; r.raster_tri_cull), but in pipelines of their own: the
// pixel shader samples the material's base-colour alpha and discards below alpha_cutoff, which
// costs early depth, so the opaque pipelines never carry it.
//
//   ms_alpha + ps_alpha_mesh   the mesh-shader path (DENG-234's ms_vis plus UV0 per vertex and the
//                              material per primitive); one 64-thread group per alpha meshlet of
//                              the phase (DeVisRasterArgs of the alpha bucket)
//   vs_alpha + ps_alpha_draw   the compute path's draw: vis_expand.slang (DENG-235) expands the
//                              alpha meshlets into triangle slots unchanged; vertex 3 * slot +
//                              corner decodes its corner, UV0 and material
//
// DENG-830: an instance with DE_GPU_INSTANCE_DITHER (DeRenderFade) is also sent here, whatever its
// material: the pixel shader drops a fixed 4x4 ordered-dither share of its pixels (dither_test),
// and runs the alpha test only for a mask material. The signed fade is in the instance's
// reserved0: d > 0 drops the pixels whose threshold is below d, d < 0 keeps exactly those below
// -d, so the values d and -d cover every pixel once (a cross-fade without gaps). The CPU side
// (extract.c) turns DeRenderFade { fade_out, invert } into this value.
//
// The alpha test (alpha_test): alpha = base_color.a * texture.a, the texture being the material's
// base-colour slot (DE_COOKED_TEX_BASE_COLOR) sampled at UV0 with the pass's sampler (linear,
// repeat; the material tables have no sampler before S14) and hardware derivatives, so the mip is
// the hardware's; a slot without a texture samples nothing (alpha = base_color.a). The texel is
// whatever mip is resident (texture streaming, S20): the test never waits. No alpha to coverage
// (one sample per pixel).
import push_constants;
import "../sprite/sprite_vis";
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";

/// The base-colour texture slot of DeGpuMaterial.textures (de_cooked_tex_slot).
public static const uint DE_VIS_ALPHA_TEX_BASE_COLOR = 0;
/// AlphaParams.flags
public static const uint DE_VIS_ALPHA_TRI_CULL = 1; // back-face and zero-area culling

/// Push constants of every entry point (32 bytes).
struct AlphaParams
{
    uint scene;     // DeSceneConstants table (de_robuffer): the tables and view_proj
    uint view_slot; // the view's entry in it
    uint visible;   // the visible-meshlet list (de_robuffer<DeVisMeshlet>)
    uint args;      // mesh path: the phase's alpha DeVisRasterArgs (de_robuffer, entry 0)
    uint tris;      // compute path: the expansion's triangle slots (packed vis ID per slot)
    uint flags;     // DE_VIS_ALPHA_*
    uint sampler;   // bindless sampler of the alpha test
    uint reserved;
};

/// True when the pixel at `uv` of material slot `material` passes the alpha test.
bool alpha_test(DeSceneConstants sc, uint material, float2 uv, uint sampler)
{
    if (material >= sc.material_count)
        return true;
    DeGpuMaterial m = de_robuffer<DeGpuMaterial>(sc.materials)[material];
    float a = m.base_color.w;
    uv = de_sprite_uv(m, uv);
    uint tex = de_material_texture(m, 0); // DE_VIS_ALPHA_TEX_BASE_COLOR
    if (tex != DE_BINDLESS_INVALID) {
        if((m.textures[0].x&DE_GPU_MATERIAL_TEXTURE_POINT)!=0) {
            uint w,h;de_tex2d(tex).GetDimensions(w,h);
            int2 pixel=clamp(int2(floor(uv*float2(w,h))),int2(0),int2(w,h)-1);
            a*=de_tex2d(tex).Load(int3(pixel,0)).a;
        }else a *= de_tex2d(tex).Sample(de_sampler(sampler), uv).a;
    }
    return a >= m.pbr.w; // alpha_cutoff
}

/// The 4x4 Bayer threshold of `pixel`, in (0, 1).
float dither_threshold(float2 pixel)
{
    static const uint k_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    uint2 p = uint2(pixel) & 3u;
    return (float(k_bayer[p.y * 4u + p.x]) + 0.5) / 16.0;
}

/// DENG-830: whether the pixel survives the signed fade `d` (0: no fade).
bool dither_test(float d, float2 pixel)
{
    if (d == 0.0)
        return true;
    float n = dither_threshold(pixel);
    return d > 0.0 ? n >= d : n < -d;
}

/// The instance's signed fade (0 without DE_GPU_INSTANCE_DITHER).
float instance_dither(DeGpuInstance inst)
{
    return (inst.flags & DE_GPU_INSTANCE_DITHER) != 0 ? asfloat(inst.reserved0) : 0.0;
}

/// The pixel test of both paths: the dither, then the alpha test (for a dithered instance only
/// when its material is a mask: any material reaches this pass through the dither).
bool alpha_keep(DeSceneConstants sc, uint material, float2 uv, uint sampler, float dither,
                float2 pixel)
{
    if (dither != 0.0)
    {
        if (!dither_test(dither, pixel))
            return false;
        if (material >= sc.material_count)
            return true;
        if (de_robuffer<DeGpuMaterial>(sc.materials)[material].alpha_mode !=
            DE_GPU_MATERIAL_ALPHA_MASK)
            return true;
    }
    return alpha_test(sc, material, uv, sampler);
}

// ---------- Mesh path ----------

struct AlphaVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct AlphaPrimitive
{
    nointerpolation uint id : VIS_ID;
    nointerpolation uint material : VIS_MATERIAL;
    nointerpolation float dither : VIS_DITHER;
};

groupshared float4 g_alpha_clip[DE_VIS_MESHLET_MAX_VERTICES];

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void ms_alpha(uint3 group : SV_GroupID, uint tid : SV_GroupThreadID, uniform AlphaParams pc,
              out vertices AlphaVertex verts[64], out indices uint3 tris[124],
              out primitives AlphaPrimitive prims[124])
{
    DeVisRasterArgs a = de_robuffer<DeVisRasterArgs>(pc.args)[0];
    uint index = de_vis_group_index(group);
    uint entry = a.first + index;
    // SetMeshOutputCounts is called once, on every path (D3D12 requires it).
    bool live = index < a.count;
    DeSceneConstants sc;
    DeGpuInstance inst;
    DeGpuMesh mesh;
    DeVisMeshletInfo m;
    m.vertex_offset = 0;
    m.triangle_offset = 0;
    m.vertex_count = 0;
    m.triangle_count = 0;
    m.submesh = 0;
    if (live)
    {
        sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
        DeVisMeshlet e = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
        inst = de_robuffer<DeGpuInstance>(sc.instances)[e.instance];
        mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
        live = de_vis_mesh_drawable(mesh) && e.meshlet < mesh.meshlet_count;
        if (live)
            m = de_vis_meshlet_info(mesh, e.meshlet);
    }
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);
    if (!live)
        return;

    if (tid < m.vertex_count)
    {
        float4 clip = de_vis_clip(sc.view_proj, de_vis_transform_point(
                                                    inst.transform, de_vis_meshlet_position(mesh, m, tid)));
        g_alpha_clip[tid] = clip;
        AlphaVertex v;
        v.pos = clip;
        v.uv = de_vis_meshlet_uv0(mesh, m, tid);
        verts[tid] = v;
    }
    GroupMemoryBarrierWithGroupSync();

    uint material = de_vis_instance_material(inst, m.submesh);
    bool cull_tris = (pc.flags & DE_VIS_ALPHA_TRI_CULL) != 0;
    bool two_sided = de_vis_material_double_sided(sc.materials, sc.material_count, material);
    float winding = de_vis_transform_det(inst.transform) < 0.0 ? -1.0 : 1.0;
    for (uint k = 0; k < 2; ++k)
    {
        uint t = tid + k * 64u;
        if (t >= m.triangle_count)
            break;
        uint3 tri = de_vis_meshlet_triangle(mesh, m, t);
        bool cull = false;
        if (cull_tris)
        {
            float4 c0 = g_alpha_clip[tri.x], c1 = g_alpha_clip[tri.y], c2 = g_alpha_clip[tri.z];
            if (c0.w > 0.0 && c1.w > 0.0 && c2.w > 0.0)
            {
                float area = de_vis_ndc_area2(c0.xy / c0.w, c1.xy / c1.w, c2.xy / c2.w) * winding;
                cull = area == 0.0 || (!two_sided && area < 0.0);
            }
        }
        // A culled triangle is degenerate (as vis_mesh.slang: Metal drops SV_CullPrimitive).
        tris[t] = cull ? uint3(tri.x, tri.x, tri.x) : tri;
        AlphaPrimitive pr;
        pr.id = de_vis_id_pack(entry, t);
        pr.material = material;
        pr.dither = instance_dither(inst);
        prims[t] = pr;
    }
}

struct AlphaPixel
{
    nointerpolation uint id : VIS_ID;
    nointerpolation uint material : VIS_MATERIAL;
    nointerpolation float dither : VIS_DITHER;
};

[shader("fragment")]
uint ps_alpha_mesh(AlphaVertex v, AlphaPixel p, uniform AlphaParams pc) : SV_Target
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_keep(sc, p.material, v.uv, pc.sampler, p.dither, v.pos.xy))
        discard;
    return p.id;
}

// ---------- Compute path's draw ----------

struct AlphaDrawVertex
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint id : VIS_ID;
    nointerpolation uint material : VIS_MATERIAL;
    nointerpolation float dither : VIS_DITHER;
};

[shader("vertex")]
AlphaDrawVertex vs_alpha(uint vid : SV_VertexID, uniform AlphaParams pc)
{
    AlphaDrawVertex o;
    uint slot = vid / 3u;
    uint corner = vid - slot * 3u;
    uint id = de_bytes(pc.tris).load(slot * 4u);
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[de_vis_id_meshlet(id)];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[vm.instance];
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(sc.meshes)[inst.mesh];
    DeVisMeshletInfo m = de_vis_meshlet_info(mesh, vm.meshlet);
    uint local = de_vis_load_u8(de_bytes(mesh.meshlet_triangles),
                                m.triangle_offset + de_vis_id_triangle(id) * 3u + corner);
    o.pos = de_vis_clip(sc.view_proj, de_vis_transform_point(
                                          inst.transform, de_vis_meshlet_position(mesh, m, local)));
    o.uv = de_vis_meshlet_uv0(mesh, m, local);
    o.id = id;
    o.material = de_vis_instance_material(inst, m.submesh);
    o.dither = instance_dither(inst);
    return o;
}

[shader("fragment")]
uint ps_alpha_draw(AlphaDrawVertex v, uniform AlphaParams pc) : SV_Target
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_keep(sc, v.material, v.uv, pc.sampler, v.dither, v.pos.xy))
        discard;
    return v.id;
}
