// shaders/culling/instance_cull.slang: GPU instance culling (SPEC §12.5 step 1, §11.8; DENG-229).
//
// Three kernels, run in this order by src/render/passes/vis/cull_instances.c (one view, one phase):
//
//   cs_instance_cull_clear  one thread: zeroes the counters (vis_types.slang DE_VIS_COUNTER_*)
//   cs_instance_cull        one thread per instance slot: skips free slots, instances without
//                           bounds, without a READY mesh with meshlets, or whose layer mask
//                           misses the view's; tests the bounding sphere, then the AABB, against
//                           the frustum (no far plane); then the two-phase occlusion rule below
//                           (DENG-233); picks the LOD; appends one DeVisWorkItem per chunk of at
//                           most 64 meshlets with an atomic counter. Items past the work list's
//                           capacity are dropped, but still counted (the CPU grows the list).
//   cs_instance_cull_args   one thread: the indirect dispatch arguments of meshlet culling, one
//                           group per work item (never zero groups; `count` is the clamped number)
//
// LOD selection (the CPU reference is de__vis_select_lod, vis_stages.h): the coarsest LOD whose
// object-space error, scaled by the instance's largest axis scale and projected to pixels at the
// distance of the bounding sphere's closest point (at least the near plane), is at most
// r.lod_error_px; then r.lod_bias LODs coarser (negative: finer), clamped to the mesh's LODs.
// DeVisCullView.lod.x already holds (height / 2 * proj[1][1]) / r.lod_error_px, so LOD l fits
// when error_l * scale * lod.x <= distance. An orthographic view uses distance 1.
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import cull_common;

/// Push constants of all three kernels (bindless indices).
struct InstanceCullParams
{
    uint cull_view;       // UPLOAD buffer of DeVisCullView (de_robuffer)
    uint cull_view_index; // this frame's entry in it
    uint instances;       // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint meshes;          // DeGpuMesh table (DE_ACCESS_SHADER_READ)
    uint work_items;      // DeVisWorkItem list (DE_ACCESS_SHADER_WRITE)
    uint counters;        // raw counters (clear, cull: SHADER_WRITE; args: SHADER_READ)
    uint args;            // DeVisDispatchArgs (DE_ACCESS_SHADER_WRITE)
    uint phase;           // 1 or 2 (phase 2 also reads the HZB and rewrites the visibility flags)
};

import lod_common;

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_instance_cull_clear(uniform InstanceCullParams pc)
{
    DeRWBytes c = de_rwbytes(pc.counters);
    c.store4(0, uint4(0, 0, 0, 0));
}

/// Whether slot `i` was visible after the last frame's phase 2 (always with occlusion off). Phase
/// 1 reads the flags read-only, phase 2 through the read-write view (it rewrites them).
bool was_visible(DeVisCullView v, uint i, uint phase)
{
    if ((v.flags & DE_VIS_CULL_OCCLUSION) == 0)
        return true;
    if (i >= v.visibility_count || v.visibility == 0)
        return false;
    uint f = 0u;
    [branch]
    if (phase == 1u)
        f = de_bytes(v.visibility).load(i * 4u);
    else
        f = de_rwbytes(v.visibility).load(i * 4u);
    return f != 0u;
}

/// Appends instance `i`'s chunks of `mesh` at the LOD for `v` to the work list.
void emit(uint i, DeGpuInstance inst, DeGpuMesh mesh, DeVisCullView v, InstanceCullParams pc)
{
    DeRWBytes counters = de_rwbytes(pc.counters);
    uint lod = select_lod(mesh, max_axis_scale(inst.transform), inst.sphere.xyz, inst.sphere.w, v);
    DeGpuMeshLod l = mesh.lods[lod];
    counters.atomic_add(DE_VIS_COUNTER_INSTANCES_VISIBLE, 1);
    if (l.meshlet_count == 0)
        return;
    counters.atomic_add(DE_VIS_COUNTER_MESHLETS, l.meshlet_count);
    uint chunks = (l.meshlet_count + DE_VIS_CHUNK_MESHLETS - 1) / DE_VIS_CHUNK_MESHLETS;
    uint base = counters.atomic_add(DE_VIS_COUNTER_WORK_ITEMS, chunks);
    for (uint k = 0; k < chunks; ++k)
    {
        uint at = base + k;
        if (at >= v.work_capacity)
            break;
        DeVisWorkItem w;
        w.instance = i;
        w.lod = lod;
        w.meshlet_first = l.meshlet_first + k * DE_VIS_CHUNK_MESHLETS;
        w.meshlet_count = min(l.meshlet_count - k * DE_VIS_CHUNK_MESHLETS, DE_VIS_CHUNK_MESHLETS);
        de_buffer<DeVisWorkItem>(pc.work_items)[at] = w;
    }
}

// Two-phase occlusion (DENG-233). An instance is a candidate when it is live, has bounds, a READY
// mesh with meshlets and a layer the view draws, and passes the frustum test.
//   Phase 1: a candidate that was visible after the last frame is drawn (no occlusion test).
//   Phase 2: every candidate is tested against this frame's HZB; one that passes and was not
//            drawn in phase 1 is drawn now. Every slot's flag is rewritten: 1 for a candidate that
//            passes, 0 otherwise (also for slots that are not candidates).
// With occlusion off every candidate counts as visible last frame: phase 1 draws them all and
// phase 2 draws nothing.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_instance_cull(uint3 id : SV_DispatchThreadID, uniform InstanceCullParams pc)
{
    DeVisCullView v = de_robuffer<DeVisCullView>(pc.cull_view)[pc.cull_view_index];
    de_cull_occlusion_begin(v);
    uint i = id.x;
    if (i >= v.instance_count)
        return;
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[i];
    DeGpuMesh mesh;
    uint need = DE_GPU_INSTANCE_LIVE | DE_GPU_INSTANCE_HAS_BOUNDS;
    uint mesh_need = DE_GPU_MESH_READY | DE_GPU_MESH_MESHLETS;
    bool candidate = (inst.flags & need) == need && inst.mesh != DE_GPU_NONE &&
                     (inst.layer_mask & v.layer_mask) != 0 &&
                     !de_cull_hidden(v, inst.entity_lo, inst.entity_hi);
    if (candidate)
    {
        mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];
        candidate = (mesh.flags & mesh_need) == mesh_need && mesh.lod_count != 0;
    }
    if (candidate)
    {
        de_rwbytes(pc.counters).atomic_add(DE_VIS_COUNTER_INSTANCES_TESTED, 1);
        if ((v.flags & DE_VIS_CULL_FRUSTUM) != 0)
            candidate = de_cull_sphere(v, inst.sphere.xyz, inst.sphere.w) &&
                        de_cull_aabb(v, inst.aabb_min.xyz, inst.aabb_max.xyz);
    }
    bool drawn_in_phase1 = candidate && was_visible(v, i, pc.phase);
    if (pc.phase == 1u)
    {
        if (drawn_in_phase1)
            emit(i, inst, mesh, v, pc);
        return;
    }
    bool visible = false;
    if (candidate)
    {
        DeCullBounds b;
        b.aabb_min = inst.aabb_min.xyz;
        b.aabb_max = inst.aabb_max.xyz;
        b.center = inst.sphere.xyz;
        b.radius = inst.sphere.w;
        visible = cull_occlusion_test(b, pc.phase);
    }
    if (v.visibility != 0)
        de_rwbytes(v.visibility).store(i * 4u, visible ? 1u : 0u);
    if (visible && !drawn_in_phase1)
        emit(i, inst, mesh, v, pc);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_instance_cull_args(uniform InstanceCullParams pc)
{
    DeVisCullView v = de_robuffer<DeVisCullView>(pc.cull_view)[pc.cull_view_index];
    uint n = min(de_bytes(pc.counters).load(DE_VIS_COUNTER_WORK_ITEMS), v.work_capacity);
    de_buffer<DeVisDispatchArgs>(pc.args)[0] = de_vis_dispatch_args(n);
}
