// shaders/visbuffer/vis_mesh.slang: the mesh-shader vis-buffer raster (SPEC §12.5 step 2;
// DENG-234). The engine shader "vis_mesh" (shaders/engine_shaders.cmake);
// src/render/passes/vis/raster_mesh.c runs it.
//
// One mesh-shader group (64 threads, no amplification stage: culling is done) per visible
// meshlet of one phase: group i draws entry first + base + i of the view's visible-meshlet list
// (DeVisRasterArgs, vis_types.slang). Each thread decodes one vertex (vis_decode.slang) and up to
// two triangles. A triangle carries its packed vis ID (index << 7 | triangle, index = the entry of
// the visible list) as a per-primitive attribute, and the pixel shader writes it to the R32_UINT
// target; depth is reverse-Z (GREATER_EQUAL).
//
// Triangle culling in the mesh shader (flags & VIS_MESH_TRI_CULL, r.raster_tri_cull; a culled
// triangle is written degenerate, which covers no pixel on every target):
//   - back faces of single-sided materials (de_vis_material_double_sided false). Front faces wind
//     counter-clockwise in NDC (y up); a mirroring instance transform (negative determinant)
//     reverses the winding of its triangles, so the test is multiplied by the determinant's sign
//     and a mirrored instance shows the same faces as the unmirrored one;
//   - triangles of zero area in NDC.
//   Triangles with a vertex at or behind the camera plane (w <= 0) are never culled here; the
//   rasterizer clips them. One pipeline (cull mode NONE) serves single- and double-sided
//   materials.
//
// cs_raster_mesh_split (one thread): writes the mesh dispatches of a phase whose meshlet count may
// exceed the per-dispatch group limit (DE_VIS_MESH_GROUPS_MAX): dispatch k covers the entries
// [k * DE_VIS_MESH_GROUPS_MAX, ...), at 12 bytes each.
import push_constants;
import "../scene/scene_types";
import vis_types;
import vis_decode;

/// Mesh-shader groups of one dispatch at most (every API's total-group limit: 2^22).
public static const uint DE_VIS_MESH_GROUPS_MAX = 4194304;
/// RasterMeshParams.flags
static const uint VIS_MESH_TRI_CULL = 1; // backface (single-sided) and zero-area culling

struct RasterMeshParams
{
    uint scene;     // scene-constants table (de_robuffer<DeSceneConstants>)
    uint view_slot; // this view's entry
    uint visible;   // the visible-meshlet list (de_robuffer<DeVisMeshlet>)
    uint args;      // this phase's DeVisRasterArgs (de_robuffer, entry 0)
    uint base;      // first group of this dispatch (k * DE_VIS_MESH_GROUPS_MAX)
    uint flags;     // VIS_MESH_*
    uint split;     // split kernel: the DeVisMeshArgs buffer it writes (de_buffer)
    uint split_count; // split kernel: dispatches to write
};

struct VisVertex
{
    float4 pos : SV_Position;
};

struct VisPrimitive
{
    nointerpolation uint id : VIS_ID;
};

groupshared float4 g_clip[DE_VIS_MESHLET_MAX_VERTICES];

[shader("mesh")]
[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void ms_vis(uint3 group : SV_GroupID, uint tid : SV_GroupThreadID, uniform RasterMeshParams pc,
            out vertices VisVertex verts[64], out indices uint3 tris[124],
            out primitives VisPrimitive prims[124])
{
    DeVisRasterArgs a = de_robuffer<DeVisRasterArgs>(pc.args)[0];
    uint index = pc.base + de_vis_group_index(group);
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
        // A mesh without its meshlet or vertex buffers (bindless 0) has nothing to draw: D3D12
        // has no null descriptor at slot 0's read-only view, and the dummy there is 256 bytes.
        live = de_vis_mesh_drawable(mesh) && e.meshlet < mesh.meshlet_count;
        if (live)
            m = de_vis_meshlet_info(mesh, e.meshlet);
    }
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);
    if (!live)
        return;

    if (tid < m.vertex_count)
    {
        float3 p = de_vis_meshlet_position(mesh, m, tid);
        float4 clip = de_vis_clip(sc.view_proj, de_vis_transform_point(inst.transform, p));
        g_clip[tid] = clip;
        VisVertex v;
        v.pos = clip;
        verts[tid] = v;
    }
    GroupMemoryBarrierWithGroupSync();

    bool cull_tris = (pc.flags & VIS_MESH_TRI_CULL) != 0;
    bool two_sided = de_vis_material_double_sided(
        sc.materials, sc.material_count, de_vis_instance_material(inst, m.submesh));
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
            float4 c0 = g_clip[tri.x], c1 = g_clip[tri.y], c2 = g_clip[tri.z];
            if (c0.w > 0.0 && c1.w > 0.0 && c2.w > 0.0)
            {
                float area = de_vis_ndc_area2(c0.xy / c0.w, c1.xy / c1.w, c2.xy / c2.w) * winding;
                cull = area == 0.0 || (!two_sided && area < 0.0);
            }
        }
        // A culled triangle becomes degenerate (one vertex three times): it covers no pixel. The
        // per-primitive SV_CullPrimitive flag is not honoured by every target (Metal).
        tris[t] = cull ? uint3(tri.x, tri.x, tri.x) : tri;
        VisPrimitive pr;
        pr.id = de_vis_id_pack(entry, t);
        prims[t] = pr;
    }
}

struct VisPixel
{
    nointerpolation uint id : VIS_ID;
};

[shader("fragment")]
uint ps_vis(VisVertex v, VisPixel p) : SV_Target
{
    return p.id;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_raster_mesh_split(uniform RasterMeshParams pc)
{
    DeVisRasterArgs a = de_robuffer<DeVisRasterArgs>(pc.args)[0];
    for (uint k = 0; k < pc.split_count; ++k)
    {
        uint start = k * DE_VIS_MESH_GROUPS_MAX;
        uint n = a.count > start ? min(a.count - start, DE_VIS_MESH_GROUPS_MAX) : 0u;
        uint2 g = de_vis_dispatch_groups(n);
        DeVisMeshArgs m;
        m.x = g.x;
        m.y = g.y;
        m.z = 1u;
        de_buffer<DeVisMeshArgs>(pc.split)[k] = m;
    }
}

// "vis.vis_ids_clear": a full-screen triangle that writes DE_VIS_ID_EMPTY (an integer target
// cannot be cleared to 0xFFFFFFFF through the RHI's float clear colour on every backend).
struct VisClearVertex
{
    float4 pos : SV_Position;
};

[shader("vertex")]
VisClearVertex vs_vis_clear(uint v : SV_VertexID)
{
    float2 uv = float2(float((v << 1) & 2u), float(v & 2u));
    VisClearVertex o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
uint ps_vis_clear(VisClearVertex v) : SV_Target
{
    return DE_VIS_ID_EMPTY;
}
