// shaders/visbuffer/vis_expand.slang: the compute meshlet expansion of the fallback raster path
// (SPEC §12.5 step 2, §2; DENG-235). Engine shader `vis_raster_expand`; run by
// src/render/passes/vis/raster_compute.c for one view, per culling phase:
//
//   cs_expand_clear  one thread, the first raster phase of the frame: zeroes the counters
//                    (DE_VIS_EXPAND_COUNTER_*)
//   cs_expand        one 128-thread group per visible meshlet of the phase (the expansion
//                    dispatch of DeVisRasterArgs, bytes 16..31), one thread per triangle. The
//                    group transforms the meshlet's vertices once (groupshared), culls each
//                    triangle with triangle_culled (r.raster_tri_cull), compacts the kept
//                    ones with a groupshared prefix sum and appends them with one atomic add per
//                    group. Each kept triangle gets a slot: the packed vis ID goes to the
//                    triangle buffer and the indices 3 * slot + 0..2 to the index buffer. Slots
//                    past the capacity are dropped but counted.
//   cs_expand_args   one thread: the phase's DeVisDrawIndexedArgs (one draw, instance count 1).
//                    Phase 1 draws slots [0, n1) and stores n1; phase 2 draws [n1, n), so the
//                    phases' triangles never overlap.
//
// vis_vs_ps.slang draws them: vertex 3 * slot + corner reads the triangle's packed vis ID, finds
// its corner through the visible-meshlet list and passes the ID to the pixel shader flat. Why not
// shared vertices plus SV_PrimitiveID: in a Vulkan fragment shader without a geometry stage,
// PrimitiveId needs the geometryShader feature, which MoltenVK does not have.
//
// No wave operations: this path must run where mesh shaders do not, which includes the CI
// software rasterizers (lavapipe, WARP). The prefix sum is a groupshared Hillis-Steele scan.
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";

/// Counters (a raw buffer of DE_VIS_EXPAND_COUNTER_BYTES): byte offsets of each u32.
public static const uint DE_VIS_EXPAND_COUNTER_TRIANGLES = 0; // kept triangles asked for, both
                                                               // phases (can exceed the capacity)
public static const uint DE_VIS_EXPAND_COUNTER_PHASE1 = 4;    // slots stored after phase 1
public static const uint DE_VIS_EXPAND_COUNTER_TESTED = 8;    // triangles of expanded meshlets
public static const uint DE_VIS_EXPAND_COUNTER_MESHLETS = 12; // meshlets expanded
public static const uint DE_VIS_EXPAND_COUNTER_BYTES = 16;

/// ExpandParams.flags
public static const uint DE_VIS_EXPAND_TRI_CULL = 1; // de_vis_triangle_culled (r.raster_tri_cull)

/// Threads per group of cs_expand: the triangle limit of a meshlet rounded up to a power of two.
public static const uint DE_VIS_EXPAND_GROUP = 128;

/// Push constants of the three kernels (bindless indices; 80 bytes).
struct ExpandParams
{
    uint instances;      // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint meshes;         // DeGpuMesh table (DE_ACCESS_SHADER_READ)
    uint materials;      // DeGpuMaterial table (DE_ACCESS_SHADER_READ); 0 = none
    uint material_count; // its entries
    uint scene;          // DeSceneConstants table (DE_ACCESS_SHADER_READ)
    uint view_slot;      // the view's entry in it
    uint visible;        // DeVisMeshlet list (DE_ACCESS_SHADER_READ)
    uint raster_args;    // the phase's DeVisRasterArgs (DE_ACCESS_INDIRECT | SHADER_READ)
    uint counters;       // raw counters (read-modify-write)
    uint tris;           // raw u32 per slot: the packed vis ID (DE_ACCESS_SHADER_WRITE)
    uint indices;        // raw u32 x 3 per slot (DE_ACCESS_SHADER_WRITE)
    uint draw_args;      // raw DeVisDrawIndexedArgs (args kernel: DE_ACCESS_SHADER_WRITE)
    uint capacity;       // slots of `tris` and `indices`
    uint phase;          // 1 or 2
    uint flags;          // DE_VIS_EXPAND_*
    float width;         // the target's size in pixels (small-triangle test)
    float height;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

/// Margin of the small-triangle test, in pixels (as meshlet_cull.slang's small-meshlet test).
static const float DE_VIS_EXPAND_SMALL_EPS_PX = 1.0 / 32.0;

/// True when [lo, hi] (pixels) holds a pixel centre k + 0.5, with the test's margin.
bool covers_centre(float lo, float hi)
{
    float eps = DE_VIS_EXPAND_SMALL_EPS_PX + 1e-5 * (abs(lo) + abs(hi));
    return floor(hi - 0.5 + eps) >= ceil(lo - 0.5 - eps);
}

/// The per-triangle cull of the expansion: true when the triangle with clip positions a, b, c
/// adds nothing to the vis buffer.
///   - Facing (the mesh path's rule, vis_mesh.slang, with vis_decode.slang's functions): when
///     every w > 0, the NDC area times `winding` (-1 for a mirroring instance transform, else 1)
///     is zero, or negative for a material that is not double-sided.
///   - Outside (this path only; the rasterizer would clip it away): all three corners beyond one
///     clip plane (x and y against w; z above w, nearer than the near plane in reverse-Z; z below
///     0).
///   - Small (this path only): with every w > 0, the screen bounds hold no pixel centre in x or
///     in y (margin 1/32 px), so no sample is covered.
/// The two tests of this path only remove triangles that cover no sample, so the output stays
/// the mesh path's.
bool triangle_culled(float4 a, float4 b, float4 c, bool double_sided, float winding, float width,
                     float height)
{
    if ((a.x > a.w && b.x > b.w && c.x > c.w) || (a.x < -a.w && b.x < -b.w && c.x < -c.w) ||
        (a.y > a.w && b.y > b.w && c.y > c.w) || (a.y < -a.w && b.y < -b.w && c.y < -c.w) ||
        (a.z > a.w && b.z > b.w && c.z > c.w) || (a.z < 0.0 && b.z < 0.0 && c.z < 0.0))
        return true;
    if (!(a.w > 0.0 && b.w > 0.0 && c.w > 0.0))
        return false;
    float2 pa = a.xy / a.w, pb = b.xy / b.w, pc = c.xy / c.w;
    float area = de_vis_ndc_area2(pa, pb, pc) * winding;
    if (area == 0.0 || (!double_sided && area < 0.0))
        return true;
    // NDC to pixels. Flipping y maps pixel centres onto pixel centres, so y needs no flip.
    float2 size = float2(width, height);
    float2 lo = (min(pa, min(pb, pc)) * 0.5 + 0.5) * size;
    float2 hi = (max(pa, max(pb, pc)) * 0.5 + 0.5) * size;
    return !covers_centre(lo.x, hi.x) || !covers_centre(lo.y, hi.y);
}

groupshared float4 gs_clip[DE_VIS_MESHLET_MAX_VERTICES];
groupshared uint gs_scan[DE_VIS_EXPAND_GROUP];
groupshared uint gs_base;
groupshared uint gs_entry;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_expand_clear(uniform ExpandParams pc)
{
    de_rwbytes(pc.counters).store4(0, uint4(0, 0, 0, 0));
}

[shader("compute")]
[numthreads(128, 1, 1)]
void cs_expand(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uniform ExpandParams pc)
{
    // Every branch up to the last barrier is uniform over the group (it depends on the group's
    // meshlet only), so every thread reaches every barrier. One thread decides whether the group
    // has work: groups past the phase's count and meshlets of meshes the raster cannot read leave
    // after one lane's loads (a software rasterizer runs every lane's).
    uint t = thread.x;
    if (t == 0u)
    {
        DeVisRasterArgs args = de_robuffer<DeVisRasterArgs>(pc.raster_args)[0];
        uint index = de_vis_group_index(group);
        uint e = DE_VIS_ID_EMPTY;
        if (index < args.count)
        {
            DeVisMeshlet v = de_robuffer<DeVisMeshlet>(pc.visible)[args.first + index];
            uint mi = de_robuffer<DeGpuInstance>(pc.instances)[v.instance].mesh;
            DeGpuMesh me = de_robuffer<DeGpuMesh>(pc.meshes)[mi];
            if (de_vis_mesh_drawable(me) && v.meshlet < me.meshlet_count)
                e = args.first + index; // the visible-list entry: what the vis ID names
        }
        gs_entry = e;
    }
    GroupMemoryBarrierWithGroupSync();
    uint entry = gs_entry;
    if (entry == DE_VIS_ID_EMPTY)
        return;
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[vm.instance];
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    DeVisMeshletInfo m = de_vis_meshlet_info(mesh, vm.meshlet);

    if (t < m.vertex_count)
        gs_clip[t] = de_vis_clip(sc.view_proj, de_vis_transform_point(
                                                   inst.transform, de_vis_meshlet_position(mesh, m, t)));
    GroupMemoryBarrierWithGroupSync();

    bool keep = false;
    if (t < m.triangle_count)
    {
        uint3 lv = de_vis_meshlet_triangle(mesh, m, t);
        keep = all(lv < uint3(m.vertex_count, m.vertex_count, m.vertex_count));
        if (keep && (pc.flags & DE_VIS_EXPAND_TRI_CULL) != 0)
        {
            bool double_sided = de_vis_material_double_sided(
                pc.materials, pc.material_count, de_vis_instance_material(inst, m.submesh));
            float winding = de_vis_transform_det(inst.transform) < 0.0 ? -1.0 : 1.0;
            keep = !triangle_culled(gs_clip[lv.x], gs_clip[lv.y], gs_clip[lv.z], double_sided,
                                    winding, pc.width, pc.height);
        }
    }

    // Inclusive prefix sum of `keep` over the group.
    gs_scan[t] = keep ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < DE_VIS_EXPAND_GROUP; off <<= 1u)
    {
        uint v = t >= off ? gs_scan[t - off] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_scan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint total = gs_scan[DE_VIS_EXPAND_GROUP - 1u];
    DeRWBytes counters = de_rwbytes(pc.counters);
    if (t == 0u)
    {
        gs_base = total != 0u ? counters.atomic_add(DE_VIS_EXPAND_COUNTER_TRIANGLES, total) : 0u;
        counters.atomic_add(DE_VIS_EXPAND_COUNTER_TESTED, m.triangle_count);
        counters.atomic_add(DE_VIS_EXPAND_COUNTER_MESHLETS, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (!keep)
        return;
    uint slot = gs_base + gs_scan[t] - 1u;
    if (slot >= pc.capacity)
        return;
    de_rwbytes(pc.tris).store(slot * 4u, de_vis_id_pack(entry, t));
    uint i = slot * 3u;
    de_rwbytes(pc.indices).store3(slot * 12u, uint3(i, i + 1u, i + 2u));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_expand_args(uniform ExpandParams pc)
{
    DeRWBytes c = de_rwbytes(pc.counters);
    uint stored = min(c.load(DE_VIS_EXPAND_COUNTER_TRIANGLES), pc.capacity);
    uint first = 0u;
    if (pc.phase == 1u)
        c.store(DE_VIS_EXPAND_COUNTER_PHASE1, stored);
    else
        first = min(c.load(DE_VIS_EXPAND_COUNTER_PHASE1), stored);
    uint count = stored - first;
    // DeVisDrawIndexedArgs: index_count, instance_count, first_index, vertex_offset,
    // first_instance.
    DeRWBytes a = de_rwbytes(pc.draw_args);
    a.store4(0u, uint4(count * 3u, 1u, first * 3u, 0u));
    a.store(16u, 0u);
}
