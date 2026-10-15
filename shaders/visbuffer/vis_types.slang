// shaders/visbuffer/vis_types.slang: the types the visibility-buffer passes share (SPEC §12.5;
// DENG-229). Every S13 pass imports it:
//
//     import "../visbuffer/vis_types";
//
// The frame (src/render/passes/vis/vis_frame.c) runs, per view:
//   cull_instances -> cull_meshlets -> raster (phase 1), hzb_build, the same three for phase 2,
//   raster_alpha, classify, resolve.
// Instance culling writes DeVisWorkItem entries (one per chunk of at most DE_VIS_CHUNK_MESHLETS
// meshlets of one instance's LOD) and DeVisDispatchArgs for meshlet culling, which dispatches one
// thread group per work item. Meshlet culling writes DeVisMeshlet entries, the visible-meshlet list
// that the packed vis ID indexes.
//
// Packed vis ID (DENG-2 addendum): (visible_meshlet_index << 7) | triangle_in_meshlet, 25 + 7
// bits. A meshlet has at most 124 triangles (DENG-103), which fit 7 bits; the visible-meshlet list
// maps the index to (instance, meshlet). Limit: DE_VIS_ID_MAX_VISIBLE_MESHLETS per view per frame.
// DE_VIS_ID_EMPTY marks a pixel nothing covers.
//
// The C mirrors are in src/render/passes/vis/vis_stages.h (the constants and the structs, with
// static size checks); the test vis.types.layout_match compares the constants here with them.
// Buffer element layout follows scene_types.slang: uint fields only, so Vulkan std430, Metal and
// D3D12 structured buffers agree; each buffer's stride is its element size.
module vis_types;

/// Meshlets in one work item at most: meshlet culling runs one 64-thread group per work item.
public static const uint DE_VIS_CHUNK_MESHLETS = 64;
/// Thread groups along x of one dispatch at most (every backend); a larger count goes on in y.
public static const uint DE_VIS_MAX_GROUPS_X = 65535;

/// Packed vis ID: bits of the triangle index, its mask, and the visible-meshlet limit (1 << 25).
public static const uint DE_VIS_ID_TRIANGLE_BITS = 7;
public static const uint DE_VIS_ID_TRIANGLE_MASK = 127;
public static const uint DE_VIS_ID_MAX_VISIBLE_MESHLETS = 33554432;
/// The vis ID of a pixel no triangle covers.
public static const uint DE_VIS_ID_EMPTY = 0xFFFFFFFF;

/// The instance-cull counters: a raw buffer (de_rwbytes / de_bytes) of DE_VIS_COUNTER_BYTES; byte
/// offsets of each u32.
public static const uint DE_VIS_COUNTER_WORK_ITEMS = 0;        // work items asked for (can exceed
                                                               // the capacity: the rest dropped)
public static const uint DE_VIS_COUNTER_INSTANCES_VISIBLE = 4; // passed every test
public static const uint DE_VIS_COUNTER_INSTANCES_TESTED = 8;  // live, mesh ready, layer match
public static const uint DE_VIS_COUNTER_MESHLETS = 12;         // meshlets of the visible LODs
public static const uint DE_VIS_COUNTER_BYTES = 16;

/// One chunk of one visible instance's LOD (16 bytes).
public struct DeVisWorkItem
{
    public uint instance;      // DeGpuInstance slot
    public uint lod;           // the LOD chosen for the view
    public uint meshlet_first; // first meshlet of the chunk (index into the mesh's meshlet table)
    public uint meshlet_count; // 1 .. DE_VIS_CHUNK_MESHLETS
};

/// One visible meshlet (8 bytes): what a vis ID's meshlet index names.
public struct DeVisMeshlet
{
    public uint instance;
    public uint meshlet; // index into the mesh's meshlet table
};

/// de_gpu_cmd_dispatch_indirect arguments (x, y, z at offset 0) plus the item count the groups
/// cover (16 bytes). Always valid: x and y are at least 1, so no dispatch has zero groups; a
/// consumer runs group de_vis_group_index(group_id) and returns when it is >= count.
public struct DeVisDispatchArgs
{
    public uint x;
    public uint y;
    public uint z;
    public uint count;
};

/// de_gpu_cmd_dispatch_mesh_indirect arguments (12 bytes apart).
public struct DeVisMeshArgs
{
    public uint x;
    public uint y;
    public uint z;
};

/// de_gpu_cmd_draw_indexed_indirect arguments (20 bytes apart).
public struct DeVisDrawIndexedArgs
{
    public uint index_count;
    public uint instance_count;
    public uint first_index;
    public int vertex_offset;
    public uint first_instance;
};

/// The vis ID of `triangle` (0 .. 127) of visible meshlet `visible_meshlet`.
public uint de_vis_id_pack(uint visible_meshlet, uint triangle)
{
    return (visible_meshlet << DE_VIS_ID_TRIANGLE_BITS) | (triangle & DE_VIS_ID_TRIANGLE_MASK);
}

/// The visible-meshlet index of a vis ID.
public uint de_vis_id_meshlet(uint id)
{
    return id >> DE_VIS_ID_TRIANGLE_BITS;
}

/// The triangle of a vis ID within its meshlet.
public uint de_vis_id_triangle(uint id)
{
    return id & DE_VIS_ID_TRIANGLE_MASK;
}

/// The groups (x, y) of an indirect dispatch of `count` groups: x <= DE_VIS_MAX_GROUPS_X, both at
/// least 1 (count 0 gives one group that does nothing).
public uint2 de_vis_dispatch_groups(uint count)
{
    if (count <= DE_VIS_MAX_GROUPS_X)
        return uint2(max(count, 1u), 1u);
    return uint2(DE_VIS_MAX_GROUPS_X, (count + DE_VIS_MAX_GROUPS_X - 1u) / DE_VIS_MAX_GROUPS_X);
}

/// The linear index of thread group `group_id` of a dispatch made by de_vis_dispatch_groups.
public uint de_vis_group_index(uint3 group_id)
{
    return group_id.y * DE_VIS_MAX_GROUPS_X + group_id.x;
}

/// The arguments of an indirect dispatch of one group per item, for `count` items.
public DeVisDispatchArgs de_vis_dispatch_args(uint count)
{
    uint2 g = de_vis_dispatch_groups(count);
    DeVisDispatchArgs a;
    a.x = g.x;
    a.y = g.y;
    a.z = 1u;
    a.count = count;
    return a;
}

/// The raster arguments of one culling phase (32 bytes; DENG-231). Meshlet culling
/// (shaders/culling/meshlet_cull.slang) writes one per phase for both raster paths. The phase's
/// visible meshlets are the entries [first, first + count) of the view's visible-meshlet list
/// (DeVisMeshlet): phase 2's entries follow phase 1's, so a packed vis ID names one entry.
///   bytes  0..11  de_gpu_cmd_dispatch_mesh_indirect arguments: one mesh-shader group per visible
///                 meshlet of the phase, de_vis_dispatch_groups(count) (DENG-234)
///   bytes 12..15  first: the phase's first entry
///   bytes 16..31  de_gpu_cmd_dispatch_indirect arguments and count, as DeVisDispatchArgs: one
///                 group per visible meshlet (DENG-235)
/// Like DeVisDispatchArgs they are always valid (x, y >= 1): a group whose de_vis_group_index is
/// >= count does nothing, and entry first + index is the meshlet it draws.
public struct DeVisRasterArgs
{
    public uint mesh_x;
    public uint mesh_y;
    public uint mesh_z;
    public uint first;
    public uint expand_x;
    public uint expand_y;
    public uint expand_z;
    public uint count;
};
