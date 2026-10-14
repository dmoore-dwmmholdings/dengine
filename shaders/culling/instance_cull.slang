// shaders/culling/instance_cull.slang: GPU instance culling (SPEC §12.5 step 1, §11.8; DENG-229).
//
// Three kernels, run in this order by src/render/passes/vis/cull_instances.c (one view, one phase):
//
//   cs_instance_cull_clear  one thread: zeroes the counters (vis_types.slang DE_VIS_COUNTER_*)
//   cs_instance_cull        one thread per instance slot: skips free slots, instances without
//                           bounds, without a READY mesh with meshlets, or whose layer mask
//                           misses the view's; tests the bounding sphere, then the AABB, against
//                           the frustum (no far plane) and the occlusion hook (cull_common); picks
//                           the LOD; appends one DeVisWorkItem per chunk of at most 64 meshlets
//                           with an atomic counter. Items past the work list's capacity are
//                           dropped, but still counted (the CPU grows the list from that count).
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
    uint phase;           // 1 or 2
};

/// The largest axis scale of a 3x4 transform (rows): the longest column of its linear part.
float max_axis_scale(float4 rows[3])
{
    float sx = rows[0].x * rows[0].x + rows[1].x * rows[1].x + rows[2].x * rows[2].x;
    float sy = rows[0].y * rows[0].y + rows[1].y * rows[1].y + rows[2].y * rows[2].y;
    float sz = rows[0].z * rows[0].z + rows[1].z * rows[1].z + rows[2].z * rows[2].z;
    return sqrt(max(sx, max(sy, sz)));
}

/// The LOD of `m` for an instance with bounding sphere (c, r) and axis scale `scale`.
uint select_lod(DeGpuMesh m, float scale, float3 c, float r, DeVisCullView v)
{
    float dist = 1.0;
    if (v.lod.y == 0.0)
    {
        float3 d = c - v.camera.xyz;
        dist = max(sqrt(d.x * d.x + d.y * d.y + d.z * d.z) - r, v.camera.w);
    }
    int count = int(min(m.lod_count, DE_GPU_MESH_LODS));
    int lod = 0;
    for (int l = count - 1; l > 0; --l)
    {
        if (m.lods[l].error * scale * v.lod.x <= dist)
        {
            lod = l;
            break;
        }
    }
    return uint(clamp(lod + int(v.lod.z), 0, count - 1));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_instance_cull_clear(uniform InstanceCullParams pc)
{
    DeRWBytes c = de_rwbytes(pc.counters);
    c.store4(0, uint4(0, 0, 0, 0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_instance_cull(uint3 id : SV_DispatchThreadID, uniform InstanceCullParams pc)
{
    DeVisCullView v = de_robuffer<DeVisCullView>(pc.cull_view)[pc.cull_view_index];
    uint i = id.x;
    if (i >= v.instance_count)
        return;
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[i];
    uint need = DE_GPU_INSTANCE_LIVE | DE_GPU_INSTANCE_HAS_BOUNDS;
    if ((inst.flags & need) != need || inst.mesh == DE_GPU_NONE)
        return;
    if ((inst.layer_mask & v.layer_mask) == 0)
        return;
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];
    uint mesh_need = DE_GPU_MESH_READY | DE_GPU_MESH_MESHLETS;
    if ((mesh.flags & mesh_need) != mesh_need || mesh.lod_count == 0)
        return;

    DeRWBytes counters = de_rwbytes(pc.counters);
    counters.atomic_add(DE_VIS_COUNTER_INSTANCES_TESTED, 1);
    if ((v.flags & DE_VIS_CULL_FRUSTUM) != 0)
    {
        if (!de_cull_sphere(v, inst.sphere.xyz, inst.sphere.w))
            return;
        if (!de_cull_aabb(v, inst.aabb_min.xyz, inst.aabb_max.xyz))
            return;
    }
    DeCullBounds b;
    b.aabb_min = inst.aabb_min.xyz;
    b.aabb_max = inst.aabb_max.xyz;
    b.center = inst.sphere.xyz;
    b.radius = inst.sphere.w;
    if (!cull_occlusion_test(b, pc.phase))
        return;

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

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_instance_cull_args(uniform InstanceCullParams pc)
{
    DeVisCullView v = de_robuffer<DeVisCullView>(pc.cull_view)[pc.cull_view_index];
    uint n = min(de_bytes(pc.counters).load(DE_VIS_COUNTER_WORK_ITEMS), v.work_capacity);
    de_buffer<DeVisDispatchArgs>(pc.args)[0] = de_vis_dispatch_args(n);
}
