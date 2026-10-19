// shaders/culling/meshlet_cull.slang: GPU meshlet culling (SPEC §12.5 step 1; DENG-231).
//
// Three kernels, run by src/render/passes/vis/cull_meshlets.c for one view:
//
//   cs_meshlet_cull_clear  one thread, first phase of the frame: zeroes the counters
//                          (DE_VIS_MESHLET_COUNTER_*)
//   cs_meshlet_cull        one 64-thread group per work item (DeVisWorkItem from instance culling,
//                          dispatched indirectly with its DeVisDispatchArgs), one thread per
//                          meshlet of the item. Tests the meshlet in this order: frustum, backface
//                          cone, small meshlet, then the occlusion hook cull_occlusion_test (DENG-233).
//                          A visible meshlet goes to the bucket meshlet_bucket() picks; each
//                          bucket is appended with one atomic add per wave (wave compaction).
//                          Entries past the list's capacity are dropped but counted.
//   cs_meshlet_cull_args   one thread per phase: the phase's range of the visible list and the
//                          raster arguments of both raster paths (DeVisRasterArgs), for the opaque
//                          and (DENG-236) the alpha-tested bucket
//
// Both phases append to the same visible-meshlet list: phase 1 from entry 0, phase 2 after the
// last entry phase 1 stored (DE_VIS_MESHLET_COUNTER_PHASE1), so every entry, and the packed vis ID
// that names it, is unique within the view's frame.
//
// Buckets (DENG-236): meshlet_bucket() sends a meshlet whose submesh material is alpha-tested (a
// READY material with alpha_mode mask) to the alpha bucket. Its entries live in the same list,
// after the opaque entries' capacity: [capacity, capacity + alpha_capacity), phase 1 first, phase
// 2 after it (DE_VIS_MESHLET_COUNTER_ALPHA, _ALPHA_PHASE1), with their own raster arguments. So
// the packed vis IDs of both buckets share one index space and stay unique.
//
// DENG-305: a meshlet whose submesh material is forward (blended or transmissive;
// forward/forward_routing.slang de_material_forward) goes to DE_VIS_BUCKET_FORWARD: it is never
// appended to the list (the forward transparent pass draws it after lighting), only counted
// (DE_VIS_MESHLET_COUNTER_FORWARD).
//
// The tests (the CPU reference is in cull_meshlets.c; vis_stages.h declares it):
//
//   Frustum   The meshlet's bounding sphere (DMSH v2 record, object space) transformed by the
//             instance's 3x4: the centre exactly, the radius scaled by the largest axis scale
//             (the longest column), which is conservative under non-uniform scale. Then
//             de_cull_sphere (no far plane). Off with r.cull_disable (DE_VIS_CULL_FRUSTUM).
//   Cone      The normal cone of the record, apex form: the meshlet faces away from a camera at
//             c when dot(normalize(apex - c), axis) >= cutoff (an orthographic view uses its
//             forward direction for normalize(apex - c)). The test runs in object space: the
//             camera goes through the inverse of the instance transform. Which side of a
//             triangle's plane a point is on does not change under an invertible affine map, so
//             this is exact for any scale and rotation, where a world-space cone would not be.
//             Skipped for a cutoff >= 1 (no cone); a transform whose determinant is not positive
//             (singular, or mirrored: which winding a raster path treats as front then is its
//             choice); a double-sided material; and a material slot the GPU scene does not
//             resolve (DE_GPU_NONE: the mesh's own default material, which may be double-sided).
//             Off with r.cull_meshlet_cone.
//   Small     The exact screen rectangle of the world sphere under the culling view-projection
//             (tangent planes of the pencil row_x - t * row_w, solved in closed form; works for
//             perspective, jittered and orthographic matrices). When the rectangle holds no pixel
//             centre (k + 0.5) in x, or none in y, the meshlet covers no sample and is culled. A
//             margin of DE_VIS_SMALL_EPS_PX pixels keeps it conservative against rasterizer
//             snapping. A sphere that reaches the near plane skips the test. Off with
//             r.cull_small_tri.
//
// Per instance: the view's layer mask (instance culling already applied it) and, when the view
// draws shadow casters only (DE_VIS_MESHLET_CASTERS_ONLY), DE_GPU_INSTANCE_CAST_SHADOWS.
//
// Wave intrinsics (WaveActiveCountBits, WavePrefixSum, WaveIsFirstLane, WaveReadLaneFirst) work
// for any wave size: a 64-thread group is one wave64, two wave32 or more smaller waves, and each
// wave adds its own count. Nothing here assumes a wave size. (WavePrefixSum of 0/1 rather than
// WavePrefixCountBits, which Slang does not provide for Metal.)
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import cull_common;
import "../forward/forward_routing"; // DENG-305

/// Counters (a raw buffer of DE_VIS_MESHLET_COUNTER_BYTES): byte offsets of each u32.
public static const uint DE_VIS_MESHLET_COUNTER_VISIBLE = 0; // visible meshlets asked for, both
                                                              // phases (can exceed the capacity)
public static const uint DE_VIS_MESHLET_COUNTER_TESTED = 4;  // meshlets tested, both phases
public static const uint DE_VIS_MESHLET_COUNTER_PHASE1 = 8;  // entries stored after phase 1: the
                                                              // first entry of phase 2
public static const uint DE_VIS_MESHLET_COUNTER_ALPHA = 16;  // DENG-236: alpha-tested meshlets
                                                              // asked for, both phases
public static const uint DE_VIS_MESHLET_COUNTER_ALPHA_PHASE1 = 20; // alpha entries stored after
                                                                    // phase 1
public static const uint DE_VIS_MESHLET_COUNTER_FORWARD = 24; // DENG-305: forward meshlets (not
                                                               // in the vis buffer), both phases
public static const uint DE_VIS_MESHLET_COUNTER_BYTES = 32;

/// MeshletCullParams.flags
public static const uint DE_VIS_MESHLET_CONE = 1;         // backface-cone test (r.cull_meshlet_cone)
public static const uint DE_VIS_MESHLET_SMALL = 2;        // small-meshlet test (r.cull_small_tri)
public static const uint DE_VIS_MESHLET_CASTERS_ONLY = 4; // only DE_GPU_INSTANCE_CAST_SHADOWS

/// Output buckets (meshlet_bucket).
public static const uint DE_VIS_BUCKET_OPAQUE = 0;
public static const uint DE_VIS_BUCKET_ALPHA = 1; // DENG-236: alpha-tested (alpha_mode mask)
/// DeGpuMaterial.alpha_mode of an alpha-tested material (de_cooked_alpha_mode).
public static const uint DE_VIS_ALPHA_MODE_MASK = 1;
public static const uint DE_VIS_BUCKET_NONE = 0xFFFFFFFF;
public static const uint DE_VIS_BUCKET_FORWARD = 2; // DENG-305: blended or transmissive (counted)

/// Bytes of one DMSH v2 meshlet record (render/formats/de_cooked_mesh.h).
public static const uint DE_VIS_MESHLET_RECORD_BYTES = 64;

/// Margin of the small-meshlet test, in pixels (plus a relative part, small_meshlet_culled).
static const float DE_VIS_SMALL_EPS_PX = 1.0 / 32.0;

/// Push constants of all three kernels (bindless indices; 64 bytes).
struct MeshletCullParams
{
    uint cull_view;       // UPLOAD buffer of DeVisCullView (de_robuffer), instance culling's
    uint cull_view_index; // this frame's entry in it
    uint instances;       // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint meshes;          // DeGpuMesh table (DE_ACCESS_SHADER_READ)
    uint materials;       // DeGpuMaterial table (DE_ACCESS_SHADER_READ); 0 = none
    uint material_count;  // its entries
    uint work_items;      // DeVisWorkItem list of the phase (DE_ACCESS_SHADER_READ)
    uint work_args;       // its DeVisDispatchArgs (DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ)
    uint visible;         // DeVisMeshlet list (DE_ACCESS_SHADER_WRITE)
    uint counters;        // raw counters (read-modify-write)
    uint raster_args;     // DeVisRasterArgs of the phase (args kernel: DE_ACCESS_SHADER_WRITE)
    uint capacity;        // entries of `visible`
    uint phase;           // 1 or 2
    uint flags;           // DE_VIS_MESHLET_*
    float width;          // the view's internal size in pixels (small-meshlet test)
    float height;
    uint alpha_capacity;    // DENG-236: entries of the alpha bucket (after `capacity`)
    uint alpha_raster_args; // DeVisRasterArgs of the phase's alpha bucket (args kernel: WRITE)
    uint reserved0;
    uint reserved1;
};

/// One meshlet record's culling data (object space).
struct MeshletBounds
{
    float4 sphere; // centre xyz, radius
    float4 apex;   // cone apex xyz, cutoff
    float3 axis;   // cone axis
    uint submesh;
};

MeshletBounds load_meshlet(DeBytes records, uint meshlet)
{
    uint at = meshlet * DE_VIS_MESHLET_RECORD_BYTES;
    MeshletBounds m;
    m.submesh = records.load(at + 8) >> 16;
    m.sphere = asfloat(records.load4(at + 16));
    m.apex = asfloat(records.load4(at + 32));
    m.axis = asfloat(records.load3(at + 48));
    return m;
}

/// The largest axis scale of a 3x4 transform (rows): the longest column of its linear part.
float meshlet_axis_scale(float4 rows[3])
{
    float sx = rows[0].x * rows[0].x + rows[1].x * rows[1].x + rows[2].x * rows[2].x;
    float sy = rows[0].y * rows[0].y + rows[1].y * rows[1].y + rows[2].y * rows[2].y;
    float sz = rows[0].z * rows[0].z + rows[1].z * rows[1].z + rows[2].z * rows[2].z;
    return sqrt(max(sx, max(sy, sz)));
}

/// True when the material of `submesh` is known (a READY entry of the materials table) and
/// single-sided: only then may the cone test cull.
bool cone_allowed(DeGpuInstance inst, uint submesh, MeshletCullParams pc)
{
    if (submesh >= DE_GPU_INSTANCE_MATERIALS || pc.materials == 0)
        return false;
    uint slot = inst.materials[submesh >> 2][submesh & 3];
    if (slot == DE_GPU_NONE || slot >= pc.material_count)
        return false;
    uint flags = de_robuffer<DeGpuMaterial>(pc.materials)[slot].flags;
    return (flags & DE_GPU_MATERIAL_READY) != 0 && (flags & DE_GPU_MATERIAL_DOUBLE_SIDED) == 0;
}

/// The cone test in object space: true when every triangle of the meshlet faces away.
bool cone_culled(float4 rows[3], MeshletBounds m, DeVisCullView v)
{
    if (!(m.apex.w < 1.0))
        return false; // no cone (cutoff 1) or a bad record
    float3 r0 = rows[0].xyz, r1 = rows[1].xyz, r2 = rows[2].xyz;
    // The inverse of the linear part: the adjugate (columns are cross products of the rows)
    // over the determinant.
    float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
    float det = dot(r0, c0);
    if (!(det > 1e-30))
        return false; // singular or mirrored
    float3 dir;
    if (v.lod.y != 0.0)
    {
        // Orthographic: the forward direction (the near plane's inward normal) in object space.
        float3 f = v.planes[4].xyz;
        dir = (c0 * f.x + c1 * f.y + c2 * f.z) / det;
    }
    else
    {
        float3 t = float3(rows[0].w, rows[1].w, rows[2].w);
        float3 q = v.camera.xyz - t;
        float3 cam = (c0 * q.x + c1 * q.y + c2 * q.z) / det;
        dir = m.apex.xyz - cam;
    }
    float len2 = dot(dir, dir);
    if (!(len2 > 0.0))
        return false;
    return dot(dir, m.axis) >= m.apex.w * sqrt(len2);
}

/// The two roots of the tangent condition of the pencil a - t * b against sphere (c, r): the
/// sphere's extent along the clip-space coordinate of row `a` over the w row `b`. `w` is dot(b,
/// (c, 1)), already known to exceed r * |b.xyz|.
float2 sphere_extent(float4 a, float4 b, float3 c, float r, float w)
{
    float A = dot(a.xyz, c) + a.w;
    float alpha = dot(a.xyz, a.xyz), beta = dot(b.xyz, b.xyz), k = dot(a.xyz, b.xyz);
    float r2 = r * r;
    float3 m = w * a.xyz - A * b.xyz;
    float3 x = cross(a.xyz, b.xyz);
    float d = r2 * max(dot(m, m) - r2 * dot(x, x), 0.0);
    float s = sqrt(d);
    float den = w * w - r2 * beta;
    float mid = A * w - r2 * k;
    return float2((mid - s) / den, (mid + s) / den);
}

/// True when [lo, hi] (pixels) holds a sample centre k + 0.5, with the test's margin.
bool covers_centre(float lo, float hi)
{
    float eps = DE_VIS_SMALL_EPS_PX + 1e-5 * (abs(lo) + abs(hi));
    return floor(hi - 0.5 + eps) >= ceil(lo - 0.5 - eps);
}

/// The small-meshlet test: true when the world sphere (c, r) covers no pixel centre.
bool small_meshlet_culled(DeVisCullView v, float3 c, float r, float width, float height)
{
    float4 rw = v.view_proj[3];
    float w = dot(rw.xyz, c) + rw.w;
    // Only spheres entirely beyond the near plane (camera.w; 0 for orthographic views).
    if (!(w - r * length(rw.xyz) > v.camera.w))
        return false;
    float2 ex = sphere_extent(v.view_proj[0], rw, c, r, w);
    float2 ey = sphere_extent(v.view_proj[1], rw, c, r, w);
    // NDC to pixels. Flipping y maps pixel centres onto pixel centres, so y needs no flip.
    float2 px = (ex * 0.5 + 0.5) * width;
    float2 py = (ey * 0.5 + 0.5) * height;
    return !covers_centre(px.x, px.y) || !covers_centre(py.x, py.y);
}

/// Every test of one meshlet of `inst`.
bool meshlet_visible(DeGpuInstance inst, MeshletBounds m, DeVisCullView v, MeshletCullParams pc)
{
    bool skinned=(inst.flags&DE_GPU_INSTANCE_SKINNED)!=0;
    float3 c = float3(dot(inst.transform[0], float4(m.sphere.xyz, 1.0)),
                      dot(inst.transform[1], float4(m.sphere.xyz, 1.0)),
                      dot(inst.transform[2], float4(m.sphere.xyz, 1.0)));
    float r = m.sphere.w * meshlet_axis_scale(inst.transform);
    if(skinned) {c=inst.sphere.xyz;r=inst.sphere.w;}
    if ((v.flags & DE_VIS_CULL_FRUSTUM) != 0 && !de_cull_sphere(v, c, r))
        return false;
    if (!skinned && (pc.flags & DE_VIS_MESHLET_CONE) != 0 && cone_allowed(inst, m.submesh, pc) &&
        cone_culled(inst.transform, m, v))
        return false;
    if ((pc.flags & DE_VIS_MESHLET_SMALL) != 0 &&
        small_meshlet_culled(v, c, r, pc.width, pc.height))
        return false;
    DeCullBounds b;
    b.aabb_min = c - float3(r, r, r);
    b.aabb_max = c + float3(r, r, r);
    b.center = c;
    b.radius = r;
    return cull_occlusion_test(b, pc.phase);
}

/// The output bucket of a visible meshlet: forward when the submesh's material is blended or
/// transmissive (DENG-305), alpha-tested when it is a READY material with alpha_mode mask
/// (DENG-236), else opaque.
uint meshlet_bucket(DeGpuInstance inst, uint submesh, MeshletCullParams pc)
{
    if (submesh >= DE_GPU_INSTANCE_MATERIALS || pc.materials == 0)
        return DE_VIS_BUCKET_OPAQUE;
    uint slot = inst.materials[submesh >> 2][submesh & 3];
    if (slot == DE_GPU_NONE || slot >= pc.material_count)
        return DE_VIS_BUCKET_OPAQUE;
    DeGpuMaterial mat = de_robuffer<DeGpuMaterial>(pc.materials)[slot];
    if (de_material_forward(mat))
        return DE_VIS_BUCKET_FORWARD; // DENG-305: the forward transparent pass draws it
    if ((mat.flags & DE_GPU_MATERIAL_READY) != 0 && mat.alpha_mode == DE_VIS_ALPHA_MODE_MASK)
        return DE_VIS_BUCKET_ALPHA;
    return DE_VIS_BUCKET_OPAQUE;
}

/// Appends `e` to list `list` (capacity `capacity`) for the lanes with `want`, with one atomic
/// add per wave on the counter at byte `counter`. Call from every lane of the group (uniform
/// control flow): the wave operations need all of them.
void append_compacted(bool want, DeVisMeshlet e, DeRWBytes counters, uint counter, uint list,
                      uint capacity)
{
    append_compacted_at(want, e, counters, counter, list, 0u, capacity);
}

/// append_compacted into the list's entries [offset, offset + capacity) (DENG-236: a bucket's
/// region of the one list).
void append_compacted_at(bool want, DeVisMeshlet e, DeRWBytes counters, uint counter, uint list,
                         uint offset, uint capacity)
{
    uint n = WaveActiveCountBits(want);
    if (n == 0)
        return;
    uint base = 0;
    if (WaveIsFirstLane())
        base = counters.atomic_add(counter, n);
    base = WaveReadLaneFirst(base);
    uint at = base + WavePrefixSum(want ? 1u : 0u);
    if (want && at < capacity)
        de_buffer<DeVisMeshlet>(list)[offset + at] = e;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_meshlet_cull_clear(uniform MeshletCullParams pc)
{
    de_rwbytes(pc.counters).store4(0, uint4(0, 0, 0, 0));
    de_rwbytes(pc.counters).store4(16, uint4(0, 0, 0, 0)); // DENG-236 alpha, DENG-305 forward
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_meshlet_cull(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                     uniform MeshletCullParams pc)
{
    // The whole group returns together, so the wave operations below see every lane.
    uint item_index = de_vis_group_index(group);
    if (item_index >= de_robuffer<DeVisDispatchArgs>(pc.work_args)[0].count)
        return;
    DeVisWorkItem item = de_robuffer<DeVisWorkItem>(pc.work_items)[item_index];
    DeVisCullView v = de_robuffer<DeVisCullView>(pc.cull_view)[pc.cull_view_index];
    de_cull_occlusion_begin(v); // DENG-233: the view's HZB for cull_occlusion_test (phase 2)
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[item.instance];
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];

    uint lane = thread.x;
    uint meshlet = item.meshlet_first + lane;
    // A mesh without a meshlet buffer (bindless 0) has nothing to read: its meshlets are not
    // tested. (D3D12 has no null descriptor at slot 0's read-only view.)
    bool tested = lane < item.meshlet_count && meshlet < mesh.meshlet_count &&
                  mesh.meshlets != DE_BINDLESS_INVALID;
    bool instance_ok = (inst.layer_mask & v.layer_mask) != 0 &&
                       ((pc.flags & DE_VIS_MESHLET_CASTERS_ONLY) == 0 ||
                        (inst.flags & DE_GPU_INSTANCE_CAST_SHADOWS) != 0);
    uint bucket = DE_VIS_BUCKET_NONE;
    if (tested && instance_ok)
    {
        MeshletBounds m = load_meshlet(de_bytes(mesh.meshlets), meshlet);
        if (meshlet_visible(inst, m, v, pc))
            bucket = meshlet_bucket(inst, m.submesh, pc);
    }

    DeRWBytes counters = de_rwbytes(pc.counters);
    uint n = WaveActiveCountBits(tested);
    if (n != 0 && WaveIsFirstLane())
        counters.atomic_add(DE_VIS_MESHLET_COUNTER_TESTED, n);
    DeVisMeshlet e;
    e.instance = item.instance;
    e.meshlet = meshlet;
    append_compacted(bucket == DE_VIS_BUCKET_OPAQUE, e, counters, DE_VIS_MESHLET_COUNTER_VISIBLE,
                     pc.visible, pc.capacity);
    append_compacted_at(bucket == DE_VIS_BUCKET_ALPHA, e, counters, DE_VIS_MESHLET_COUNTER_ALPHA,
                        pc.visible, pc.capacity, pc.alpha_capacity);
    uint nf = WaveActiveCountBits(bucket == DE_VIS_BUCKET_FORWARD); // DENG-305: counted only
    if (nf != 0 && WaveIsFirstLane())
        counters.atomic_add(DE_VIS_MESHLET_COUNTER_FORWARD, nf);
}

/// The raster arguments of entries [first, first + count) (both paths, DeVisRasterArgs).
DeVisRasterArgs raster_args_of(uint first, uint count)
{
    uint2 g = de_vis_dispatch_groups(count);
    DeVisRasterArgs a;
    a.mesh_x = g.x;
    a.mesh_y = g.y;
    a.mesh_z = 1u;
    a.first = first;
    a.expand_x = g.x;
    a.expand_y = g.y;
    a.expand_z = 1u;
    a.count = count;
    return a;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void cs_meshlet_cull_args(uniform MeshletCullParams pc)
{
    DeRWBytes c = de_rwbytes(pc.counters);
    uint stored = min(c.load(DE_VIS_MESHLET_COUNTER_VISIBLE), pc.capacity);
    uint first = 0;
    if (pc.phase == 1)
        c.store(DE_VIS_MESHLET_COUNTER_PHASE1, stored);
    else
        first = min(c.load(DE_VIS_MESHLET_COUNTER_PHASE1), stored);
    uint count = stored - first;
    uint2 g = de_vis_dispatch_groups(count);
    DeVisRasterArgs a;
    a.mesh_x = g.x;
    a.mesh_y = g.y;
    a.mesh_z = 1u;
    a.first = first;
    a.expand_x = g.x;
    a.expand_y = g.y;
    a.expand_z = 1u;
    a.count = count;
    de_buffer<DeVisRasterArgs>(pc.raster_args)[0] = a;
    // DENG-236: the alpha bucket, the same way within its region of the list.
    if (pc.alpha_raster_args == 0)
        return;
    uint alpha_stored = min(c.load(DE_VIS_MESHLET_COUNTER_ALPHA), pc.alpha_capacity);
    uint alpha_first = 0;
    if (pc.phase == 1)
        c.store(DE_VIS_MESHLET_COUNTER_ALPHA_PHASE1, alpha_stored);
    else
        alpha_first = min(c.load(DE_VIS_MESHLET_COUNTER_ALPHA_PHASE1), alpha_stored);
    de_buffer<DeVisRasterArgs>(pc.alpha_raster_args)[0] =
        raster_args_of(pc.capacity + alpha_first, alpha_stored - alpha_first);
}
