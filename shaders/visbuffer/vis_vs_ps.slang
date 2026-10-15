// shaders/visbuffer/vis_vs_ps.slang: the draw of the compute-expansion raster path (SPEC §12.5
// step 2; DENG-235). Engine shader `vis_raster_draw`; src/render/passes/vis/raster_compute.c makes
// one DE_PIPE_GRAPHICS pipeline of vs_draw and ps_draw: the R32_UINT vis-ID target
// (de__vis_raster_targets, cleared by the mesh path's "vis.vis_ids_clear"), the view's D32
// reverse-Z depth with test GREATER_EQUAL and write, no hardware cull (the expansion culled
// already, with the mesh path's rule). One indexed indirect draw per phase draws what
// vis_expand.slang wrote: vertex 3 * slot + corner (the index buffer holds exactly that) reads the
// slot's packed vis ID, decodes its corner through the visible-meshlet list and the meshlet record
// (vis_decode.slang, the mesh path's arithmetic) and passes the ID flat; the pixel shader writes
// it (uint SV_Target into DE_FMT_R32_UINT).
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";

/// Push constants of every entry point (one block for the vertex/pixel pair; 32 bytes).
struct DrawParams
{
    uint instances; // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint meshes;    // DeGpuMesh table (DE_ACCESS_SHADER_READ)
    uint scene;     // DeSceneConstants table (DE_ACCESS_SHADER_READ)
    uint view_slot; // the view's entry in it
    uint visible;   // DeVisMeshlet list (DE_ACCESS_SHADER_READ)
    uint tris;      // raw u32 per slot: the packed vis ID (DE_ACCESS_SHADER_READ)
    uint reserved0;
    uint reserved1;
};

struct VisVertex
{
    float4 pos : SV_Position;
    nointerpolation uint id : VISID;
};

[shader("vertex")]
VisVertex vs_draw(uint vid : SV_VertexID, uniform DrawParams pc)
{
    VisVertex o;
    uint slot = vid / 3u;
    uint corner = vid - slot * 3u;
    uint id = de_bytes(pc.tris).load(slot * 4u);
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[de_vis_id_meshlet(id)];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[vm.instance];
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeVisMeshletInfo m = de_vis_meshlet_info(mesh, vm.meshlet);
    DeBytes tris = de_bytes(mesh.meshlet_triangles);
    uint local = de_vis_load_u8(tris, m.triangle_offset + de_vis_id_triangle(id) * 3u + corner);
    o.pos = de_vis_clip(sc.view_proj, de_vis_transform_point(
                                          inst.transform, de_vis_meshlet_position(mesh, m, local)));
    o.id = id;
    return o;
}

[shader("fragment")]
uint ps_draw(VisVertex v, uniform DrawParams pc) : SV_Target
{
    return v.id;
}
