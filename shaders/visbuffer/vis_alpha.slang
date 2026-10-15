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
// The alpha test (alpha_test): alpha = base_color.a * texture.a, the texture being the material's
// base-colour slot (DE_COOKED_TEX_BASE_COLOR) sampled at UV0 with the pass's sampler (linear,
// repeat; the material tables have no sampler before S14) and hardware derivatives, so the mip is
// the hardware's; a slot without a texture samples nothing (alpha = base_color.a). The texel is
// whatever mip is resident (texture streaming, S20): the test never waits. No alpha to coverage
// (one sample per pixel).
import push_constants;
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
    uint tex = m.textures[0].x; // DE_VIS_ALPHA_TEX_BASE_COLOR
    if (tex != DE_BINDLESS_INVALID)
        a *= de_tex2d(tex).Sample(de_sampler(sampler), uv).a;
    return a >= m.pbr.w; // alpha_cutoff
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
        prims[t] = pr;
    }
}

struct AlphaPixel
{
    nointerpolation uint id : VIS_ID;
    nointerpolation uint material : VIS_MATERIAL;
};

[shader("fragment")]
uint ps_alpha_mesh(AlphaVertex v, AlphaPixel p, uniform AlphaParams pc) : SV_Target
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_test(sc, p.material, v.uv, pc.sampler))
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
    return o;
}

[shader("fragment")]
uint ps_alpha_draw(AlphaDrawVertex v, uniform AlphaParams pc) : SV_Target
{
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    if (!alpha_test(sc, v.material, v.uv, pc.sampler))
        discard;
    return v.id;
}
