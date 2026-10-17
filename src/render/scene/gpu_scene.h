/*
 * gpu_scene.h: the GPU scene: persistent instance, mesh, material, light and per-view tables that
 * mirror the render world, with incremental uploads (SPEC §12.4, §12.1, §12.6; DENG-177).
 * Internal to de_render; include as "render/scene/gpu_scene.h".
 *
 * Files:
 *   gpu_table.c  one table: a GPU_ONLY storage buffer of fixed-stride entries, its bindless index,
 *                a slot allocator, the dirty set, capacity doubling with a GPU-side copy
 *   gpu_scene.c  the tables, the entry packing, the upload plan and the render-graph passes
 *   shaders/scene/scene_types.slang  the entries as shaders see them (the C mirrors are below)
 *   shaders/scene/scatter.slang      the scatter kernels
 *
 * Tables (de__gs_table_id), each one storage buffer read through its bindless index:
 *   INSTANCES  one entry per render-world instance slot (DENG-175): transforms (current and
 *              previous, rows of a 3x4 relative to the render origin), world AABB and sphere,
 *              mesh and material slots, layer mask, flags, entity bits for picking.
 *   MESHES     one entry per mesh handle some instance uses (refcounted): the bindless indices of
 *              the mesh buffers, counts, quantization, bounds and the LOD table (DENG-105 records).
 *   MATERIALS  one entry per material handle some instance uses: its parameter block (DENG-302,
 *              render/materials/material_params.h, packed by de__material_pack): the model, the
 *              feature bits, the Standard parameters and the textures' bindless indices. An
 *              instance's empty material slot takes its mesh's default material for that submesh.
 *   LIGHTS     one entry per render-world light slot: transform rows, colour, the DeLight
 *              parameters (pack_light; S14 DENG-303 changes it).
 *   VIEWS      the scene constants: one entry per view slot (de__gpu_scene_set_view): the tables'
 *              bindless indices and counts, the render origin, view and projection matrices,
 *              current and previous (motion vectors). DENG-176 binds a view's slot per view.
 * Free slots hold an entry without ..._LIVE / ..._READY flags. Slot allocation is
 * deterministic: instance and light slots are the render world's; mesh, material and view slots
 * come from a LIFO free list (the last slot freed is reused first), else the next new slot.
 *
 * One frame, on the thread that renders (inside de_renderer_render_frame):
 *
 *     de__gpu_scene_update(gs, frame);            // the render-world buffer of this frame
 *     de__gpu_scene_set_view(gs, slot, &params);  // each view, every frame (DENG-176)
 *     de__rg_begin(g, arena);
 *     de__gpu_scene_add_passes(gs, g);            // first: the upload passes, if any
 *     ... the frame's passes; readers import a table with de__gpu_scene_import ...
 *     de__rg_finalize(g); de__rg_compile(g); de__rg_execute(g, &desc);
 *     de__gpu_scene_frame_end(gs, g, submitted);  // after execute, whatever it returned
 *     de__rg_reset(g);
 *
 * Uploads. update() takes the render world's dirty and removed lists (every slot after
 * `origin_changed`, and in the first frame) and refreshes the mesh, material and view entries
 * (they are packed and compared with a CPU copy each frame, so a hot reload or a texture that
 * finishes loading updates them). DENG-302: with a material instance pool (the renderer), only the
 * material blocks marked since the last update are packed (de__gpu_scene_material_changed, new
 * slots), unless the pool's epoch moved (a material or texture published, reloaded or unloaded):
 * then every referenced block is re-checked once.
 *
 * add_passes() packs every changed entry straight into DENG-105's upload ring (de__uploader_stage:
 * the renderer's uploader, or the scene's own ring without an asset system) and adds up to three
 * passes on DE_QUEUE_GRAPHICS:
 *   gpu_scene.grow     a table that grew: the old buffer is copied into the new one on the GPU
 *   gpu_scene.upload   runs of at least DE__GS_COPY_MIN_RUN changed entries: copy_buffer from the
 *                      ring; shorter runs: their entries and slots into GPU_ONLY scratch buffers
 *   gpu_scene.scatter  scatter.slang writes the scratch entries to their slots (instances,
 *                      lights; without a scatter pipeline every run is copied)
 * Nothing changed: no pass and no byte. The passes declare every table and scratch buffer
 * whichever changed, so the pass set (and the compiled graph, DENG-172's cache) is one of a few
 * and a steady frame allocates nothing. The tables are exported in DE_ACCESS_SHADER_READ, the
 * state they are in between graphs (readers: de__gpu_scene_import). The stage retires when the
 * graph's frame-end value completes (de__gpu_scene_frame_end). A frame stages at most `budget`
 * bytes; what does not fit (or a ring that stays full) uploads in the next frames (the slots stay
 * dirty). A frame that was not submitted marks everything dirty again.
 *
 * Threads: one call at a time. update() (de__gpu_scene_prepare) reads asset records, so it runs
 * where the asset system is not being updated: the renderer calls it on the main thread at the
 * kick, while the render thread is idle (DENG-422). Everything else may run on the thread that
 * renders. The pass callbacks run on task workers (DENG-173) and only read this frame's plan.
 * Records of unloaded assets stay alive until the frame fence passes the frames that could name
 * them (render/loaders/render_assets.h, "Deferred unloads").
 *
 * Memory: tables, scratch buffers and CPU arrays grow (doubling) with the scene and never shrink.
 * A scene that does not grow allocates nothing per frame (render_gpu_scene.gpu_scene.
 * no_heap_after_warmup). An old buffer is destroyed (deferred, de_gpu_buffer_destroy) after the
 * frame that copied it was submitted.
 */
#ifndef DE_RENDER_SCENE_GPU_SCENE_H
#define DE_RENDER_SCENE_GPU_SCENE_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/materials/material_params.h"

#include <stddef.h>

DE_EXTERN_C_BEGIN

struct de__render_world_frame;
struct de_render_uploader;
struct de_render_mesh;
struct de_render_material;
struct de_asset_system;

/* =================================================================================================
 * Entries: the C mirrors of shaders/scene/scene_types.slang
 * ============================================================================================== */

/** DE_GPU_NONE: a mesh or material slot that names nothing. */
#define DE__GS_NONE 0xFFFFFFFFu

/** DeGpuInstance.flags */
enum {
    DE__GS_INSTANCE_LIVE = 1u,
    DE__GS_INSTANCE_CAST_SHADOWS = 2u,
    DE__GS_INSTANCE_VISIBLE_IN_RT = 4u,
    DE__GS_INSTANCE_HAS_BOUNDS = 8u
};
/** DeGpuMesh.flags */
enum { DE__GS_MESH_READY = 1u, DE__GS_MESH_MESHLETS = 2u };
/** DeGpuMaterial.flags (render/materials/material_params.h, DENG-302) */
enum {
    DE__GS_MATERIAL_READY = DE__MATERIAL_FLAG_READY,
    DE__GS_MATERIAL_DOUBLE_SIDED = DE__MATERIAL_FLAG_DOUBLE_SIDED,
    DE__GS_MATERIAL_UNLIT = DE__MATERIAL_FLAG_UNLIT
};
/** DeGpuMaterial.model (DENG-237): the built-in material models (DENG-2 Q12); game models get the
 * ids after them (S14 DENG-298). At most DE__GS_MATERIAL_MODELS (one u32 bit each). */
enum {
    DE__GS_MATERIAL_MODEL_STANDARD = 0u,
    DE__GS_MATERIAL_MODEL_UNLIT = 1u,
    DE__GS_MATERIAL_MODEL_SPRITE = 2u,
    DE__GS_MATERIAL_MODEL_TERRAIN = 3u,
    DE__GS_MATERIAL_MODEL_WATER = 4u
};
#define DE__GS_MATERIAL_MODELS 32u
/** DeGpuLight.flags */
enum { DE__GS_LIGHT_LIVE = 1u, DE__GS_LIGHT_CAST_SHADOWS = 2u };
/** Sizes of the arrays in the entries. */
#define DE__GS_INSTANCE_MATERIALS 8u
#define DE__GS_MESH_LODS          8u
#define DE__GS_MATERIAL_TEXTURES  DE__MATERIAL_TEXTURES

/* Field types: de__gs_uint, de__gs_float, de__gs_uint4, de__gs_float4 (material_params.h). */

/*
 * The one definition of each entry: F(c_type, slang_type, name, array_dims). It declares the C
 * struct and the field table de__gs_layouts (the test layout_match compares that table with the
 * structs it parses from scene_types.slang).
 */
#define DE__GS_INSTANCE_FIELDS(F)                                                                  \
    F(de__gs_float4, float4, transform, [3])                                                       \
    F(de__gs_float4, float4, prev_transform, [3])                                                  \
    F(de__gs_float4, float4, aabb_min, )                                                           \
    F(de__gs_float4, float4, aabb_max, )                                                           \
    F(de__gs_float4, float4, sphere, )                                                             \
    F(de__gs_uint, uint, mesh, )                                                                   \
    F(de__gs_uint, uint, flags, )                                                                  \
    F(de__gs_uint, uint, layer_mask, )                                                             \
    F(de__gs_uint, uint, reserved0, )                                                              \
    F(de__gs_uint, uint, entity_lo, )                                                              \
    F(de__gs_uint, uint, entity_hi, )                                                              \
    F(de__gs_uint, uint, reserved1, )                                                              \
    F(de__gs_uint, uint, reserved2, )                                                              \
    F(de__gs_uint4, uint4, materials, [2])

#define DE__GS_MESH_LOD_FIELDS(F)                                                                  \
    F(de__gs_uint, uint, index_first, )                                                            \
    F(de__gs_uint, uint, index_count, )                                                            \
    F(de__gs_uint, uint, meshlet_first, )                                                          \
    F(de__gs_uint, uint, meshlet_count, )                                                          \
    F(de__gs_uint, uint, triangle_count, )                                                         \
    F(de__gs_uint, uint, vertex_count, )                                                           \
    F(de__gs_float, float, error, )                                                                \
    F(de__gs_uint, uint, reserved, )

#define DE__GS_MESH_FIELDS(F)                                                                      \
    F(de__gs_uint, uint, vertices, )                                                               \
    F(de__gs_uint, uint, meshlets, )                                                               \
    F(de__gs_uint, uint, meshlet_vertices, )                                                       \
    F(de__gs_uint, uint, meshlet_triangles, )                                                      \
    F(de__gs_uint, uint, indices, )                                                                \
    F(de__gs_uint, uint, vertex_count, )                                                           \
    F(de__gs_uint, uint, index_count, )                                                            \
    F(de__gs_uint, uint, meshlet_count, )                                                          \
    F(de__gs_uint, uint, submesh_count, )                                                          \
    F(de__gs_uint, uint, lod_count, )                                                              \
    F(de__gs_uint, uint, flags, )                                                                  \
    F(de__gs_uint, uint, reserved, )                                                               \
    F(de__gs_float4, float4, position_offset, )                                                    \
    F(de__gs_float4, float4, position_scale, )                                                     \
    F(de__gs_float4, float4, uv_transform, [2])                                                    \
    F(de__gs_float4, float4, bounds_min, )                                                         \
    F(de__gs_float4, float4, bounds_max, )                                                         \
    F(de__gs_float4, float4, sphere, )                                                             \
    F(de__gs_uint4, uint4, stream_offset, [2])                                                     \
    F(de__gs_uint4, uint4, stream_format, [2])                                                     \
    F(de__gs_mesh_lod, DeGpuMeshLod, lods, [8])

/* DeGpuMaterial: the material parameter block (DENG-302), defined in material_params.h. */
#define DE__GS_MATERIAL_FIELDS(F) DE__MATERIAL_PARAMS_FIELDS(F)

#define DE__GS_LIGHT_FIELDS(F)                                                                     \
    F(de__gs_float4, float4, transform, [3])                                                       \
    F(de__gs_float4, float4, color, )                                                              \
    F(de__gs_float4, float4, params, )                                                             \
    F(de__gs_float4, float4, area, )                                                               \
    F(de__gs_uint, uint, kind, )                                                                   \
    F(de__gs_uint, uint, flags, )                                                                  \
    F(de__gs_uint, uint, entity_lo, )                                                              \
    F(de__gs_uint, uint, entity_hi, )

#define DE__GS_VIEW_FIELDS(F)                                                                      \
    F(de__gs_float4, float4, view, [4])                                                            \
    F(de__gs_float4, float4, proj, [4])                                                            \
    F(de__gs_float4, float4, view_proj, [4])                                                       \
    F(de__gs_float4, float4, inv_view_proj, [4])                                                   \
    F(de__gs_float4, float4, prev_view_proj, [4])                                                  \
    F(de__gs_float4, float4, camera_position, )                                                    \
    F(de__gs_float4, float4, origin_high, )                                                        \
    F(de__gs_float4, float4, origin_low, )                                                         \
    F(de__gs_float4, float4, viewport, )                                                           \
    F(de__gs_uint, uint, instances, )                                                              \
    F(de__gs_uint, uint, meshes, )                                                                 \
    F(de__gs_uint, uint, materials, )                                                              \
    F(de__gs_uint, uint, lights, )                                                                 \
    F(de__gs_uint, uint, instance_count, )                                                         \
    F(de__gs_uint, uint, mesh_count, )                                                             \
    F(de__gs_uint, uint, material_count, )                                                         \
    F(de__gs_uint, uint, light_count, )                                                            \
    F(de__gs_uint, uint, view_slot, )                                                              \
    F(de__gs_uint, uint, flags, )                                                                  \
    F(de__gs_float, float, pre_exposure, )                                                         \
    F(de__gs_float, float, prev_pre_exposure, )

#define DE__GS_DECLARE_FIELD(ctype, stype, name, dims) ctype name dims;

/** DeGpuMeshLod (32 bytes). */
typedef struct de__gs_mesh_lod {
    DE__GS_MESH_LOD_FIELDS(DE__GS_DECLARE_FIELD)
} de__gs_mesh_lod;
/** DeGpuInstance (208 bytes). */
typedef struct de__gs_instance {
    DE__GS_INSTANCE_FIELDS(DE__GS_DECLARE_FIELD)
} de__gs_instance;
/** DeGpuMesh (480 bytes). */
typedef struct de__gs_mesh {
    DE__GS_MESH_FIELDS(DE__GS_DECLARE_FIELD)
} de__gs_mesh;
/** DeGpuMaterial (256 bytes): the material parameter block (material_params.h, DENG-302). */
typedef de__material_params de__gs_material;
/** DeGpuLight (112 bytes). */
typedef struct de__gs_light {
    DE__GS_LIGHT_FIELDS(DE__GS_DECLARE_FIELD)
} de__gs_light;
/** DeSceneConstants (432 bytes). */
typedef struct de__gs_view {
    DE__GS_VIEW_FIELDS(DE__GS_DECLARE_FIELD)
} de__gs_view;

/* No padding anywhere (entries are compared and hashed as bytes), and 16-byte multiples. */
DE_STATIC_ASSERT(sizeof(de__gs_mesh_lod) == 32, "DeGpuMeshLod is 32 bytes");
DE_STATIC_ASSERT(sizeof(de__gs_instance) == 208, "DeGpuInstance is 208 bytes");
DE_STATIC_ASSERT(sizeof(de__gs_mesh) == 480, "DeGpuMesh is 480 bytes");
DE_STATIC_ASSERT(sizeof(de__gs_light) == 112, "DeGpuLight is 112 bytes");
DE_STATIC_ASSERT(sizeof(de__gs_view) == 432, "DeSceneConstants is 432 bytes");
DE_STATIC_ASSERT(offsetof(de__gs_instance, mesh) == 144 &&
                     offsetof(de__gs_instance, materials) == 176,
                 "DeGpuInstance layout");
DE_STATIC_ASSERT(offsetof(de__gs_mesh, position_offset) == 48 && offsetof(de__gs_mesh, lods) == 224,
                 "DeGpuMesh layout");
DE_STATIC_ASSERT(offsetof(de__gs_light, kind) == 96, "DeGpuLight layout");
DE_STATIC_ASSERT(offsetof(de__gs_view, camera_position) == 320 &&
                     offsetof(de__gs_view, instances) == 384,
                 "DeSceneConstants layout");

/** One field of an entry, for the layout check. */
typedef struct de__gs_layout_field {
    const char *name;
    const char *slang_type; /**< "uint", "float", "uint4", "float4" or a struct name */
    u32 offset, size;       /**< bytes; size covers the whole array */
} de__gs_layout_field;

/** One entry type: its Slang name, size and fields in order. */
typedef struct de__gs_layout {
    const char *slang_name;
    u32 size;
    const de__gs_layout_field *fields;
    u32 field_count;
} de__gs_layout;

/** The six entry types (DeGpuMeshLod, DeGpuInstance, DeGpuMesh, DeGpuMaterial, DeGpuLight,
 * DeSceneConstants). */
const de__gs_layout *de__gs_layouts(u32 *count);

/** One `public static const uint` of scene_types.slang and its C value. */
typedef struct de__gs_layout_const {
    const char *name;
    u32 value;
} de__gs_layout_const;
const de__gs_layout_const *de__gs_layout_consts(u32 *count);

/* =================================================================================================
 * Tables (gpu_table.c)
 * ============================================================================================== */

/** The tables of a GPU scene. */
typedef enum de__gs_table_id {
    DE__GS_INSTANCES = 0,
    DE__GS_MESHES,
    DE__GS_MATERIALS,
    DE__GS_LIGHTS,
    DE__GS_VIEWS,
    DE__GS_TABLE_COUNT
} de__gs_table_id;

/**
 * One persistent table: `capacity` entries of `stride` bytes in a GPU_ONLY buffer
 * (STORAGE | COPY_DST | COPY_SRC, structured with that stride), of which [0, count) are in use.
 */
typedef struct de__gpu_table {
    const char *name;
    de_gpu_device *dev;
    const de_allocator *alloc;
    u32 stride;
    u32 capacity;
    u32 count; /**< slots in use: the high-water mark (free slots below it are zeroed) */
    de_gpu_buffer buffer;
    u32 bindless;
    u32 state; /**< access the buffer is in between graphs: NONE (new) or DE_ACCESS_SHADER_READ */

    /* A growth not uploaded yet: the buffer before it, and how many entries to copy from it. The
     * scene destroys it after the frame that copies it (de__gpu_table_retire_old). */
    de_gpu_buffer old;
    u32 old_state;
    u32 old_count;

    /* Dirty set: a bit per slot and the list of set bits (unsorted); or everything. */
    u64 *dirty_bits;
    u32 *dirty;
    u32 dirty_count;
    bool all_dirty;

    /* LIFO free list (tables whose slots the scene allocates: meshes, materials, views). */
    u32 *free;
    u32 free_count;

    /* CPU copy of every entry (meshes, materials, views: packed each frame and compared), or
     * NULL. */
    u8 *shadow;
    bool want_shadow;

    u32 growths; /**< capacity changes so far */
} de__gpu_table;

/** Creates the table's buffer and arrays (`shadow`: keep a CPU copy of the entries). */
de_result de__gpu_table_init(de__gpu_table *t, de_gpu_device *dev, const de_allocator *alloc,
                             const char *name, u32 stride, u32 capacity, bool shadow);
/** Destroys the buffers (deferred) and frees the arrays. */
void de__gpu_table_fini(de__gpu_table *t);
/**
 * Makes room for `n` entries: doubles the capacity until it fits, with a new buffer whose bindless
 * index replaces the old one. The first growth since the last upload keeps the old buffer for the
 * GPU-side copy of its [0, count) entries; a second one destroys the intermediate buffer (nothing
 * used it). Raises `count` to `n`; the new slots are not marked.
 */
de_result de__gpu_table_reserve(de__gpu_table *t, u32 n);
/** Marks slot `s` (< count) changed. */
void de__gpu_table_mark(de__gpu_table *t, u32 s);
/** Marks every slot changed (an origin change, the first frame, a lost frame). */
void de__gpu_table_mark_all(de__gpu_table *t);
/** Clears the dirty set. */
void de__gpu_table_clear_dirty(de__gpu_table *t);
/** Sorts the dirty list ascending (after an incremental frame). */
void de__gpu_table_sort_dirty(de__gpu_table *t);
/** Takes a slot: the last one freed, else slot `count` (reserving it). DE__GS_NONE when out of
 * memory (error set). */
u32 de__gpu_table_alloc(de__gpu_table *t);
/** Returns slot `s` to the free list and marks it (its entry is zeroed by the caller's packing). */
void de__gpu_table_free(de__gpu_table *t, u32 s);
/** Destroys `old` (deferred until the GPU is done with it) after the frame that copied it. */
void de__gpu_table_retire_old(de__gpu_table *t);
/** GPU bytes of the buffer. */
u64 de__gpu_table_bytes(const de__gpu_table *t);

/* =================================================================================================
 * The scene (gpu_scene.c)
 * ============================================================================================== */

typedef struct de__gpu_scene de__gpu_scene;

/** Where the scene finds the runtime records of meshes, materials and textures. */
typedef struct de__gpu_scene_assets {
    /** The READY record of `m`, or NULL (not loaded, or invalid). */
    const struct de_render_mesh *(*mesh)(void *user, de_mesh m);
    /** The READY record of `m`, or NULL. */
    const struct de_render_material *(*material)(void *user, de_material m);
    /** The sampled bindless index of `t`, or DE_GPU_BINDLESS_INVALID while it is not loaded. */
    u32 (*texture_bindless)(void *user, de_texture t);
    void *user;
    /** DENG-302, optional: the default material of submesh `submesh` of READY mesh `mesh` (an
     * instance's empty material slot takes it); invalid for none. NULL: empty slots stay empty
     * (DE_GPU_NONE). The asset-system resolver uses the mesh record's submesh material IDs. */
    de_material (*default_material)(void *user, de_mesh mesh, u32 submesh);
} de__gpu_scene_assets;

/** de__gpu_scene_create settings. `(desc){0}` with a device is valid. */
typedef struct de__gpu_scene_desc {
    de_gpu_device *device;
    const de_allocator *allocator; /**< NULL = the heap */
    /** Records come from this asset system (de_render_mesh_get, ...), unless `resolver` is set.
     * NULL and no resolver: every mesh and material stays not READY. */
    struct de_asset_system *assets;
    /** Tests: the records come from here instead (when `mesh` is set). */
    de__gpu_scene_assets resolver;
    /** The ring to stage in (the renderer's, DENG-105). NULL: the scene makes its own ring of
     * `staging_bytes` on first use. */
    struct de_render_uploader *uploader;
    u64 staging_bytes;                        /**< own ring size (0 = 64 MiB, rounded up to 512) */
    u64 budget_bytes;                         /**< bytes one frame may stage (0 = half the ring) */
    u32 initial_capacity[DE__GS_TABLE_COUNT]; /**< entries (0 = defaults) */
    /**
     * The scatter kernels (cs_scatter_instances, cs_scatter_lights of shaders/scene/scatter.slang
     * cooked for this device's backend) and their reflection. Without them every changed run is
     * copied with copy_buffer.
     */
    de_gpu_shader_blob scatter_instances, scatter_lights;
    u32 scatter_threadgroup[3];
    u32 scatter_push_constant_bytes;
    /**
     * No scatter blobs given: load the engine's own kernels, the engine shader "scene_scatter"
     * (DENG-421, render/shaders/engine_shaders.h: the in-process cook through the cache in Dev
     * builds, the embedded build-time cook in every build, a placeholder on the null backend).
     * Without code for the backend, and on devices below the Base tier, it runs copy-only
     * (logged once).
     */
    bool builtin_scatter;
    /**
     * The scene constants are written by de__gpu_scene_view_pass inside each view's graph (the
     * renderer, DENG-176), not by de__gpu_scene_add_passes. The view table then never grows:
     * give it initial_capacity[DE__GS_VIEWS] slots.
     */
    bool views_in_view_graphs;
    /**
     * DENG-302: the material instance pool (the renderer's). Material records come from it
     * (asset materials and runtime instances), blocks are packed only when marked or when its
     * epoch moves, and the DMAT model names resolve through `material_models`. NULL: the records
     * come from `assets` / `resolver`, and every referenced block is packed each frame.
     */
    struct de__material_instances *materials;
    const struct de__material_models *material_models;
} de__gpu_scene_desc;

/** One view's matrices for its scene constants (de__gpu_scene_set_view). */
typedef struct de__gpu_view_params {
    de_mat4 view;            /**< render-origin space -> view space */
    de_mat4 proj;            /**< view -> clip (reverse-Z) */
    de_vec3 camera_position; /**< relative to the render origin */
    u32 width, height;       /**< the view's internal resolution in pixels */
    u32 flags;               /**< DeSceneConstants.flags (free for the caller) */
    /** DENG-304: this frame's and the previous frame's exposure (lighting/exposure.c): passes
     * that write radiance multiply it by pre_exposure (0 = 1). */
    f32 pre_exposure, prev_pre_exposure;
} de__gpu_view_params;

/** Counters (de__gpu_scene_stats). */
typedef struct de__gpu_scene_stats {
    u64 frames;                            /**< update() calls */
    u64 frame_bytes;                       /**< bytes staged and copied by the last frame */
    u64 total_bytes;                       /**< all frames */
    u32 frame_entries[DE__GS_TABLE_COUNT]; /**< entries the last frame uploaded, per table */
    u32 frame_copies;                      /**< copy_buffer commands of the last frame */
    u32 frame_scattered;                   /**< entries the last frame scattered */
    u32 frame_dispatches;                  /**< scatter dispatches of the last frame */
    u32 frame_passes;                      /**< passes the last frame added */
    u32 count[DE__GS_TABLE_COUNT];         /**< slots in use */
    u32 capacity[DE__GS_TABLE_COUNT];      /**< entries the buffers hold */
    u32 live_meshes, live_materials;       /**< referenced mesh and material slots */
    u64 gpu_bytes;                         /**< tables and scratch buffers */
    u64 deferred_frames;                   /**< frames that left changes for later (budget, ring) */
    u32 growths;                           /**< table capacity changes so far */
    /** DENG-236: referenced materials that are READY and alpha-tested (alpha_mode mask), as the
     * last update packed them. 0: the alpha-tested raster adds no pass. */
    u32 mask_materials;
    /** DENG-302: material blocks the last update packed (marked, new or re-checked). */
    u32 material_packs;
    /** DENG-302: READY referenced materials whose resolve permutation or model changed in the
     * last update (de__material_permutation_of). */
    u32 permutation_switches;
    /** DENG-302: the resolve permutation each model's tiles use this frame: the largest of its
     * READY referenced materials' (0 without any). */
    u32 material_permutation[DE__GS_MATERIAL_MODELS];
} de__gpu_scene_stats;

/** A table as readers see it. */
typedef struct de__gpu_scene_table_info {
    de_gpu_buffer buffer;
    u32 bindless;
    u32 stride;
    u32 count;
    u32 capacity;
    u32 state; /**< the access it is in between graphs */
} de__gpu_scene_table_info;

/** Creates a GPU scene. DE_ERR_INVALID_ARGUMENT (no device), DE_ERR_OUT_OF_MEMORY, the RHI's
 * errors (buffers, scatter pipelines). */
de_result de__gpu_scene_create(const de__gpu_scene_desc *desc, de__gpu_scene **out);
/** Destroys it (its buffers through the deferred destruction). NULL is ignored. */
void de__gpu_scene_destroy(de__gpu_scene *gs);

/**
 * Takes this frame's render-world buffer: the instance and light changes, the slots they free, the
 * render origin (an origin change marks every instance and light), the mesh and material refcounts
 * and entries. `f` must stay unchanged until de__gpu_scene_frame_end (it does: DENG-175 refills a
 * buffer only frames_in_flight + 1 frames later).
 * @return DE_OK; DE_ERR_OUT_OF_MEMORY (a table could not grow: the changes it missed stay marked).
 */
de_result de__gpu_scene_update(de__gpu_scene *gs, const struct de__render_world_frame *f);
/**
 * Sets the scene constants of view slot `slot` for this frame (call it every frame for every view
 * that renders, after update()). The previous view-projection is the one set in the frame before
 * (rebased when the origin moved), or this one for a view that was not set then. Only a change of
 * the packed entry uploads anything.
 */
de_result de__gpu_scene_set_view(de__gpu_scene *gs, u32 slot, const de__gpu_view_params *p);
/** Frees view slot `slot` (its entry is zeroed). */
void de__gpu_scene_remove_view(de__gpu_scene *gs, u32 slot);
/**
 * Stages this frame's changes and adds the upload passes to `g` (between de__rg_begin and
 * de__rg_finalize, before the passes that read the tables). Adds nothing when nothing changed.
 * @return DE_OK (also when the ring is full: the changes wait); DE_ERR_* of a scratch buffer.
 */
de_result de__gpu_scene_add_passes(de__gpu_scene *gs, de_rg *g);
/**
 * After de__rg_execute of `g`: `submitted` true closes the frame's stage on the graph's frame-end
 * value (de__rg_frame_end) and retires the buffers the frame replaced; false (the graph was not
 * executed) releases the stage and marks every entry for upload again.
 */
void de__gpu_scene_frame_end(de__gpu_scene *gs, de_rg *g, bool submitted);

/**
 * The renderer's per-frame step, part 1, on the MAIN thread at the kick (de__renderer_kick_prepare,
 * DENG-422): de__gpu_scene_update with `f`, the render-world buffer about to be rendered (NULL: a
 * frame without a render world; only the frame counter moves). This is the only call that reads
 * asset records (mesh, material, texture): the render thread never touches the asset system,
 * which the main thread updates while the render thread records. The render thread is idle here.
 */
de_result de__gpu_scene_prepare(de__gpu_scene *gs, const struct de__render_world_frame *f);
/**
 * Part 2, on the thread that renders (de_renderer_render_frame, before the views): the upload
 * passes of what part 1 prepared, in the scene's own graph on `arena`, compiled and executed when
 * there are any (`frame_index` labels its timings), then de__gpu_scene_frame_end. Queue order
 * makes the uploads visible to every view graph after it. Reads no asset record.
 */
de_result de__gpu_scene_frame(de__gpu_scene *gs, de_arena *arena, u64 frame_index);
/**
 * Inside view slot `slot`'s graph `g` (a view stage, DENG-176): sets its scene constants
 * (de__gpu_scene_set_view) and, when the packed entry changed, stages it and adds the copy pass
 * `gpu_scene.view` to `g`, which leaves the view table in DE_ACCESS_SHADER_READ for the stages
 * after it. A static camera adds nothing. Needs `views_in_view_graphs`; `slot` must be below the
 * view table's capacity. The frame's view stage retires with de__gpu_scene_views_end.
 */
de_result de__gpu_scene_view_pass(de__gpu_scene *gs, de_rg *g, u32 slot,
                                  const de__gpu_view_params *p);
/** After every view graph of the frame was submitted: the frame's view stage retires when
 * `fence` reaches `value` (the renderer's frame fence and the frame's value). */
void de__gpu_scene_views_end(de__gpu_scene *gs, de_gpu_fence fence, u64 value);

/** Imports table `t` into graph `g` in its between-graphs state (DE_ACCESS_SHADER_READ once it was
 * uploaded) for a pass that reads it. Invalid handle when the table has no buffer. */
de_rg_resource de__gpu_scene_import(de__gpu_scene *gs, de_rg *g, de__gs_table_id t);
/** Table `t`'s buffer, bindless index, stride, count and capacity (they change when it grows). */
de__gpu_scene_table_info de__gpu_scene_table(const de__gpu_scene *gs, de__gs_table_id t);
/** The counters. */
const de__gpu_scene_stats *de__gpu_scene_stats_get(const de__gpu_scene *gs);
/** Copies the counters into the GPU-scene fields of `s` (de_render_stats). */
void de__gpu_scene_fill_stats(const de__gpu_scene *gs, de_render_stats *s);
/** Shared immutable mesh packing, including the vertex decode metadata. */
void de__gpu_scene_pack_mesh_record(const struct de_render_mesh *, de__gs_mesh *);
/** The mesh slot of `m` (DE__GS_NONE when no instance uses it). */
u32 de__gpu_scene_mesh_slot(const de__gpu_scene *gs, de_mesh m);
/** The material slot of `m` (DE__GS_NONE when no instance uses it). */
u32 de__gpu_scene_material_slot(const de__gpu_scene *gs, de_material m);

/**
 * DENG-302: material `m`'s parameters changed (de_material_set_params, an instance destroyed):
 * its block is packed at the next update, if an instance uses it. Main thread (update's thread),
 * any time between updates. NULL `gs` ok.
 */
void de__gpu_scene_material_changed(de__gpu_scene *gs, de_material m);

/** DENG-302: the resolve permutation the tiles of `model` use this frame (index into
 * de__material_permutations), as the last update chose it. Thread that renders. */
u32 de__gpu_scene_material_permutation(const de__gpu_scene *gs, u32 model);
/** True when the scene was created with scatter pipelines. */
bool de__gpu_scene_has_scatter(const de__gpu_scene *gs);

/** Tests: applies this frame's plan for table `t` (copies and scatters, not the growth copy) to a
 * CPU copy of it, `mirror` of `mirror_entries` entries. Between add_passes and frame_end. */
void de__gpu_scene_debug_apply(const de__gpu_scene *gs, de__gs_table_id t, u8 *mirror,
                               u32 mirror_entries);
/** Tests: packs entry `slot` of table `t` as this frame uploads it (instances and lights from
 * the frame given to update(); meshes, materials and views from their CPU copies). */
void de__gpu_scene_debug_pack(const de__gpu_scene *gs, de__gs_table_id t, u32 slot, void *out);

/* ---- DENG-305: CPU reads of the tables the last update() made (the forward transparent pass's
 * list, on the render thread while the frame's view graphs are built): what the GPU holds once
 * that frame's uploads ran. Valid until the next update(). ---- */
/** Instance slot `s`'s mesh slot (`*mesh`) and its DE__GS_INSTANCE_MATERIALS material slots
 * (`*materials`), DE__GS_NONE where none. False for a slot past the instance arrays. */
bool de__gpu_scene_instance_slots(const de__gpu_scene *gs, u32 s, u32 *mesh, const u32 **materials);
/** The CPU copy of entry `slot` of the mesh or material table (NULL for another table or a slot
 * past its count). */
const void *de__gpu_scene_entry(const de__gpu_scene *gs, de__gs_table_id t, u32 slot);

DE_EXTERN_C_END

#endif /* DE_RENDER_SCENE_GPU_SCENE_H */
