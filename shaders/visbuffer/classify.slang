// shaders/visbuffer/classify.slang: material classification of the visibility buffer (SPEC §12.5
// step 3, §12.6; DENG-237). Engine shader `vis_classify`; src/render/passes/vis/classify.c runs it
// once per view and frame, after both raster phases:
//
//   cs_classify_clear  one 64-thread group: zeroes the list counters
//   cs_classify        one 64-thread group per 64 / tile tiles of a tile row (r.classify_tile:
//                      8 x 8 or 16 x 16 pixels), one thread per pixel row of a tile, so a thread
//                      decodes a meshlet's model once per run of pixels. Each pixel's vis ID goes to its
//                      material model: visible-list entry -> instance and meshlet -> the meshlet
//                      record's submesh -> the instance's material slot -> DeGpuMaterial.model
//                      (vis_decode.slang, vis_types.slang). The group ORs the models into the
//                      tile's mask (groupshared atomics: no wave operations, so it runs on the CI
//                      software rasterizers too), writes the mask and appends the tile once to the
//                      list of each model it holds, and to the background list when a pixel holds
//                      no ID (DE_VIS_ID_EMPTY). Pixels past the view's edge belong to no list.
//   cs_classify_args   one 64-thread group: per list, a DeVisDispatchArgs of one group per tile
//                      (de_vis_dispatch_args), the list's length in `count`
//
// The vis IDs come from a raw buffer copy of the vis-ID target (classify.c): the target has no
// sampled or storage view (a uint image in the float-typed bindless tables fails Vulkan
// validation), and resolve (DENG-238) reads the same buffer.
//
// Lists: list k (model k < DE_GPU_MATERIAL_MODELS, then DE_VIS_CLASSIFY_BACKGROUND) holds its
// tiles at entries [k * tile_count, k * tile_count + count_k) of `lists`, each x | y << 16. Each
// list can hold every tile, so nothing overflows. The order within a list is not deterministic.
import push_constants;
import "../scene/scene_types";
import "../visbuffer/vis_types";
import "../visbuffer/vis_decode";

/// Lists: one per material model, then the background list (pixels with no ID).
public static const uint DE_VIS_CLASSIFY_LISTS = 33;
public static const uint DE_VIS_CLASSIFY_BACKGROUND = 32;
/// A tile entry: x in the low 16 bits, y in the high ones.
public static const uint DE_VIS_CLASSIFY_TILE_BITS = 16;
/// Threads per group of every kernel.
public static const uint DE_VIS_CLASSIFY_GROUP = 64;

/// Push constants of the three kernels (bindless indices; 80 bytes).
struct ClassifyParams
{
    uint ids;            // raw u32 per pixel, rows `pitch` bytes apart (DE_ACCESS_SHADER_READ)
    uint pitch;
    uint width;          // the view's size in pixels
    uint height;
    uint tile;           // tile edge in pixels: 8 or 16
    uint tiles_x;        // tiles per row
    uint tile_count;     // tiles in all (entries per list)
    uint visible;        // DeVisMeshlet list (DE_ACCESS_SHADER_READ)
    uint visible_count;  // its entries, opaque and alpha regions (DENG-236); past them: Standard
    uint instances;      // DeGpuInstance table (DE_ACCESS_SHADER_READ)
    uint meshes;         // DeGpuMesh table (DE_ACCESS_SHADER_READ)
    uint materials;      // DeGpuMaterial table (DE_ACCESS_SHADER_READ); 0 = none
    uint material_count; // its entries
    uint lists;          // raw u32 tile entries (DE_ACCESS_SHADER_WRITE)
    uint counters;       // raw u32 per list (read-modify-write)
    uint args;           // raw DeVisDispatchArgs per list (args kernel: DE_ACCESS_SHADER_WRITE)
    uint masks;          // raw u32 per tile (DE_ACCESS_SHADER_WRITE)
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

/// The material model of visible-list entry `entry`'s triangles: the model of the instance's
/// material for the meshlet's submesh, or Standard for the default material (DE_GPU_NONE), a
/// slot past the table, a material not loaded yet, or a mesh without meshlet records.
uint entry_model(uint entry, ClassifyParams pc)
{
    if (entry >= pc.visible_count)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    DeVisMeshlet vm = de_robuffer<DeVisMeshlet>(pc.visible)[entry];
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(pc.instances)[vm.instance];
    DeGpuMesh mesh = de_robuffer<DeGpuMesh>(pc.meshes)[inst.mesh];
    if (mesh.meshlets == DE_BINDLESS_INVALID || vm.meshlet >= mesh.meshlet_count)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    uint slot = de_vis_instance_material(inst, de_vis_meshlet_info(mesh, vm.meshlet).submesh);
    if (pc.materials == DE_BINDLESS_INVALID || slot == DE_GPU_NONE || slot >= pc.material_count)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    DeGpuMaterial m = de_robuffer<DeGpuMaterial>(pc.materials)[slot];
    if ((m.flags & DE_GPU_MATERIAL_READY) == 0)
        return DE_GPU_MATERIAL_MODEL_STANDARD;
    return m.model & (DE_GPU_MATERIAL_MODELS - 1u);
}

groupshared uint gs_mask[DE_VIS_CLASSIFY_GROUP / 8u];
groupshared uint gs_background[DE_VIS_CLASSIFY_GROUP / 8u];

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_classify_clear(uint3 thread : SV_GroupThreadID, uniform ClassifyParams pc)
{
    if (thread.x < DE_VIS_CLASSIFY_LISTS)
        de_rwbytes(pc.counters).store(thread.x * 4u, 0u);
}

// One group covers a row of 64 / tile tiles (8 of 8 x 8, 4 of 16 x 16): thread t takes row t % tile
// of tile t / tile, `tile` neighbouring pixels, so it decodes a meshlet's model once for the run
// of pixels it covers.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_classify(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                 uniform ClassifyParams pc)
{
    uint t = thread.x;
    uint per_group = DE_VIS_CLASSIFY_GROUP / pc.tile;
    uint local = t / pc.tile; // this thread's tile in the group
    uint row = t % pc.tile;
    if (t < per_group)
    {
        gs_mask[t] = 0u;
        gs_background[t] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint tx = group.x * per_group + local;
    uint y = group.y * pc.tile + row;
    uint mask = 0u, background = 0u;
    if (tx < pc.tiles_x && y < pc.height)
    {
        DeBytes ids = de_bytes(pc.ids);
        uint last_entry = DE_VIS_ID_EMPTY, last_model = 0u;
        uint x0 = tx * pc.tile;
        uint x1 = min(x0 + pc.tile, pc.width);
        for (uint x = x0; x < x1; ++x)
        {
            uint id = ids.load(y * pc.pitch + x * 4u);
            if (id == DE_VIS_ID_EMPTY)
            {
                background = 1u;
                continue;
            }
            uint entry = de_vis_id_meshlet(id);
            if (entry != last_entry)
            {
                last_entry = entry;
                last_model = entry_model(entry, pc);
            }
            mask |= 1u << last_model;
        }
    }
    if (mask != 0u)
        InterlockedOr(gs_mask[local], mask);
    if (background != 0u)
        InterlockedOr(gs_background[local], 1u);
    GroupMemoryBarrierWithGroupSync();

    // The masks, and each tile once into the list of each model it holds (and the background).
    for (uint i = t; i < per_group * DE_VIS_CLASSIFY_LISTS; i += DE_VIS_CLASSIFY_GROUP)
    {
        uint lt = i / DE_VIS_CLASSIFY_LISTS;
        uint k = i % DE_VIS_CLASSIFY_LISTS;
        uint x = group.x * per_group + lt;
        if (x >= pc.tiles_x)
            continue;
        uint m = gs_mask[lt];
        if (k == 0u)
            de_rwbytes(pc.masks).store((group.y * pc.tiles_x + x) * 4u, m);
        bool in_list = k < DE_VIS_CLASSIFY_BACKGROUND ? ((m >> k) & 1u) != 0u
                                                      : gs_background[lt] != 0u;
        if (in_list)
        {
            uint at = de_rwbytes(pc.counters).atomic_add(k * 4u, 1u);
            de_rwbytes(pc.lists)
                .store((k * pc.tile_count + at) * 4u, x | (group.y << DE_VIS_CLASSIFY_TILE_BITS));
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_classify_args(uint3 thread : SV_GroupThreadID, uniform ClassifyParams pc)
{
    uint t = thread.x;
    if (t >= DE_VIS_CLASSIFY_LISTS)
        return;
    DeVisDispatchArgs a = de_vis_dispatch_args(de_rwbytes(pc.counters).load(t * 4u));
    de_rwbytes(pc.args).store4(t * 16u, uint4(a.x, a.y, a.z, a.count));
}
