/*
 * render_assets.h: the runtime `texture`, `mesh`, `material` and `model` asset types and the GPU
 * upload path behind them (SPEC §11.4, §11.8, §12.1; DENG-105). Internal to de_render; include as
 * "render/loaders/render_assets.h". The renderer (S12, DENG-174) calls
 * de_render_register_asset_types from de_renderer_create; until then tests and tools call it.
 *
 * Pipeline of a GPU asset (§11.4): I/O and zstd (S07) -> the type's `load` on a frame worker
 * parses the cooked blob (DTEX, DMSH v2, DMAT, DMDL; src/render/formats/), creates the GPU objects
 * and stages the bytes in the upload ring -> de_render_uploader_flush, once per frame on the main
 * thread, records the copies on DE_QUEUE_COPY and signals the uploader's timeline fence -> the
 * type's `ready` hook (de_asset_type_desc.ready) publishes the asset only once the fence has
 * reached the value of its last copy. So a slot is READY (and LOADED is sent) never before the
 * GPU has its data.
 *
 * The upload ring: one DE_MEM_UPLOAD buffer of de_asset_system_desc.staging_ring_bytes (default
 * 128 MiB). Loaders reserve ring space under a mutex and copy their bytes into it; when the ring
 * is full they keep a heap copy instead, and the flush moves it into the ring piece by piece as
 * space retires. Ring space retires when the copy fence passes the frame that read it. Pieces are
 * subresources (textures) and ranges of at most 16 MiB (buffers), so an asset larger than the ring
 * uploads over several frames; one subresource larger than the ring cannot load
 * (DE_ERR_UNSUPPORTED). The cvar `asset.upload_mb_per_frame` (default 64) caps the bytes one
 * flush copies (one piece always goes, however large).
 *
 * Queue ownership: each finished upload ends with a release barrier (COPY_DST -> SHADER_READ,
 * DE_QUEUE_COPY -> DE_QUEUE_GRAPHICS). The renderer records the matching acquire halves on its
 * graphics list with de_render_uploader_acquire, and makes that submit wait for the fence value
 * it returns, before the first use of the resources (Vulkan with a separate transfer family,
 * D3D12; Metal needs nothing, the call is still correct there).
 *
 * Frames: the uploader copies only in de_render_uploader_flush, so de_asset_wait (which only
 * updates the asset system) never sees a texture or mesh READY. Tools and tests drive frames
 * themselves: flush, de_render_uploader_acquire_submit, then de_asset_system_update, until the
 * request is final.
 *
 * Threads: registration, flush, acquire and destroy on the main thread; loaders on frame workers;
 * `unload` on any thread. The accessors read a slot's object: main thread, or any thread while
 * the asset is held loaded. The allocator must be thread-safe (loaders allocate on workers).
 */
#ifndef DE_RENDER_LOADERS_RENDER_ASSETS_H
#define DE_RENDER_LOADERS_RENDER_ASSETS_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_texture.h"
#include "render/formats/de_cooked_sprite_proxy.h"

DE_EXTERN_C_BEGIN

/** The GPU upload path of the render asset types: the ring, the queue and the copy fence. */
typedef struct de_render_uploader de_render_uploader;

/**
 * Registers the runtime types `texture`, `mesh`, `material` and `model` with `s`
 * (de_asset_type_register), with loaders that create their GPU objects on `d`, and creates the
 * uploader they share (`*out`). In builds with the cooker (DE_HAS_COOKER: Debug and Dev) the
 * `texture` and `model` types also get their importers (de_texture_import, de_model_import), so
 * source assets cook in-process; `mesh` is a sub-asset type of the model importer and has none;
 * `material` is too, and has the `.mat.toml` importer (de_mat_toml_import, DENG-302). The upload
 * ring has `staging_ring_bytes` of the system's desc.
 *
 * `a` (NULL = the heap) must be thread-safe; it holds the uploader and every runtime record.
 * Destroy the asset system first (its unloads use the uploader), then the uploader, then `d`.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `s`, `d` or `out`); DE_ERR_ALREADY_EXISTS (one of
 *         the four names is registered already); DE_ERR_OUT_OF_MEMORY; the RHI's errors for the
 *         ring buffer or the fence. `*out` is NULL on failure, except when the failure came after
 *         some of the types were registered (the registry cannot unregister): their loaders then
 *         fail, and `*out` must still be destroyed after the asset system.
 */
de_result de_render_register_asset_types(de_asset_system *s, de_gpu_device *d,
                                         const de_allocator *a, de_render_uploader **out);

/** Options of de__render_register_asset_types_ex (tests and tools). `(desc){0}` = the defaults of
 * de_render_register_asset_types. */
typedef struct de__render_assets_desc {
    de_gpu_device *device;
    const de_allocator *allocator;
    /** Ring size in bytes, a multiple of 512 (0 = the system's staging_ring_bytes). */
    u64 ring_bytes;
    /** Bytes one flush may copy (0 = the cvar asset.upload_mb_per_frame). */
    u64 frame_cap_bytes;
    /** Register the types without importers even when the cooker is linked (tests that load
     * cooked bytes from files). */
    bool no_import;
} de__render_assets_desc;

/** de_render_register_asset_types with options. */
de_result de__render_register_asset_types_ex(de_asset_system *s, const de__render_assets_desc *d,
                                             de_render_uploader **out);

/** Destroys the uploader (after the asset system that used it). Waits for nothing: the ring and
 * the fence are destroyed deferred by the RHI. NULL is ignored. */
void de_render_uploader_destroy(de_render_uploader *u);

/**
 * Once per frame, on the main thread: retires ring space whose copies completed, then records the
 * queued uploads in order into one DE_QUEUE_COPY command list (up to the per-frame byte cap), with
 * a release barrier to DE_QUEUE_GRAPHICS after each finished upload, and submits it signalling
 * the uploader's fence. Records and submits nothing when nothing is queued. No heap allocation.
 */
void de_render_uploader_flush(de_render_uploader *u);

/**
 * Records, into `cmd` (a DE_QUEUE_GRAPHICS list), the acquire halves of the release barriers of
 * every upload that finished since the last call, and fills `*wait` (nullable) with the fence
 * value the submit of `cmd` must wait for (`{0}` when there was nothing). Call it before the
 * first pass that may use a new asset, and submit `cmd` in the same frame, before the next
 * de_asset_system_update (an unload could otherwise destroy a resource the list names).
 * @return The number of barriers recorded.
 */
u32 de_render_uploader_acquire(de_render_uploader *u, de_gpu_cmd *cmd, de_gpu_wait *wait);
/**
 * For hosts without a renderer (tools, tests): records the pending acquire halves on a graphics
 * list of its own and submits it with the wait. A flush finishes no upload while 4096 release
 * barriers are pending, so something must take them every frame: the renderer's
 * de_render_uploader_acquire, or this. Main thread. Returns the number of barriers.
 */
u32 de_render_uploader_acquire_submit(de_render_uploader *u);

/** Counters of an uploader (main thread). */
typedef struct de_render_upload_stats {
    u64 frames;           /**< copy lists submitted */
    u64 bytes;            /**< bytes copied, all frames */
    u64 last_frame_bytes; /**< bytes the last flush copied */
    u32 last_frame_pieces;
    u32 queued;     /**< uploads waiting for copies */
    u64 spilled;    /**< uploads that did not fit in the ring when staged (heap copy) */
    u64 ring_bytes; /**< ring size */
    u64 ring_used;  /**< bytes reserved and not retired */
    u32 ring_records;
    u32 pending_acquires; /**< barriers for the next de_render_uploader_acquire */
    u64 submitted;        /**< last fence value signalled by a submit */
    u64 completed;        /**< the fence's completed value as last seen */
} de_render_upload_stats;
de_render_upload_stats de_render_uploader_stats(const de_render_uploader *u);

/** The fence the copy lists signal (one value per submitted flush). */
de_gpu_fence de_render_uploader_fence(const de_render_uploader *u);

/* ---------- Deferred unloads (DENG-422) ----------
 *
 * With pipelined rendering the render thread records frame N while the main thread runs
 * de_asset_system_update for N + 1, and frames N - 1 .. N are in flight on the GPU. The GPU scene
 * reads mesh, material and texture records only on the main thread, at the kick (render/scene/
 * gpu_scene.h), but the entries it uploaded name the objects' bindless indices until the next
 * kick repacks them. So the four types' `unload` cancels the object's pending upload at once and
 * keeps the record and its GPU objects on the uploader's retire list until the renderer's frame
 * fence reaches the value of the last frame prepared before the unload; the next flush (the kick)
 * frees them. Without a frame fence (no renderer: tools, tests) they are freed at once.
 */

/** A record waiting on the retire list (embedded in each record; internal). */
typedef struct de__render_retire {
    struct de__render_retire *next;
    u64 value; /**< the frame-fence value after which nothing uses the object */
    void *object;
    void (*free_fn)(de_render_uploader *u, void *object);
} de__render_retire;

/** The renderer's frame fence (frame n signals n + 1): unloads wait for it from now on. Main
 * thread, before the first frame. An invalid fence turns the deferral off. */
void de__render_uploader_set_frame_fence(de_render_uploader *u, de_gpu_fence fence);
/** The renderer prepares frame `frame_index` (the kick, main thread): an object unloaded from now
 * on may be named by that frame, so it waits for fence value frame_index + 1. */
void de__render_uploader_frame_prepared(de_render_uploader *u, u64 frame_index);
/** Frees the retired objects whose frame-fence value has completed. Main thread; the flush calls
 * it. Returns how many it freed. */
u32 de__render_uploader_retire_poll(de_render_uploader *u);
/** Tests: objects waiting on the retire list, and freed from it so far. */
void de__render_uploader_retire_stats(de_render_uploader *u, u32 *pending, u64 *freed);
/** Tests: the retire list sees the frame fence's completed value as at most `cap` (UINT64_MAX =
 * off), as if the GPU were late. */
void de__render_uploader_test_hold_frames(de_render_uploader *u, u64 cap);

/* ---------- Runtime records (the slots' objects) ---------- */

/** The environment data of a texture cooked with `usage = "environment"` (ENVS, de_cooked_env.h),
 * kept on the CPU for image-based lighting (S14). */
typedef struct de_render_env {
    u32 face_size, mip_count, sample_count;
    f32 mip_per_roughness; /**< mip = roughness x this (DE_COOKED_ENV_ROUGHNESS_LINEAR) */
    f32 rotation_deg, clamp_luminance, mean_luminance, max_luminance;
    f32 sh9[9][3]; /**< irradiance coefficients, RGB */
} de_render_env;

/** A loaded `texture`. */
typedef struct de_render_texture {
    de_gpu_texture texture; /**< SAMPLED | COPY_DST | COPY_SRC */
    u32 bindless;           /**< the sampled view (de_gpu_texture_bindless(.., 0, false)) */
    u32 dim;                /**< de_gpu_texture_dim */
    u32 format;             /**< de_gpu_format */
    u32 width, height, layers, mip_count;
    u32 flags; /**< DE_COOKED_TEXTURE_SRGB, _CUBE, _PIXEL_ART, _HAS_ALPHA */
    u32 usage; /**< de_cooked_texture_usage */
    u64 gpu_bytes;
    const de_cooked_sprite_proxy *sprite_proxy; /**< optional cooked silhouette extrusion */
    bool has_env;
    de_render_env env; /**< valid when has_env */
    struct de__upload *upload_;
    size_t block_size_;
    de__render_retire retire_; /**< internal: the retire list (DENG-422) */
} de_render_texture;

/** The GPU buffers of a mesh. */
typedef enum de_render_mesh_buffer {
    DE_RENDER_MESH_VERTICES = 0,      /**< every vertex stream, each at a 256-byte offset */
    DE_RENDER_MESH_MESHLETS,          /**< DMSH meshlet records, 64 bytes each */
    DE_RENDER_MESH_MESHLET_VERTICES,  /**< u32 mesh vertex index per meshlet vertex */
    DE_RENDER_MESH_MESHLET_TRIANGLES, /**< u8 x 3 local indices, padded per meshlet */
    DE_RENDER_MESH_INDICES,           /**< u32 indices of every LOD */
    DE_RENDER_MESH_BUFFER_COUNT
} de_render_mesh_buffer;

/** One submesh: its default material and its LOD 0 bounds. */
typedef struct de_render_submesh {
    de_asset_id material; /**< {0}: the engine's default material */
    f32 bounds_min[3], bounds_max[3];
} de_render_submesh;

/** A loaded `mesh` (DMSH v2): GPU buffers and what culling and LOD selection read on the CPU. */
typedef struct de_render_mesh {
    /** GPU_ONLY buffers, filled by copies; invalid (and bindless 0) for an empty section (no
     * meshlets). */
    de_gpu_buffer buffers[DE_RENDER_MESH_BUFFER_COUNT];
    u32 bindless[DE_RENDER_MESH_BUFFER_COUNT];
    u64 buffer_bytes[DE_RENDER_MESH_BUFFER_COUNT];
    /** Byte offset of each stream (de_cooked_stream) in the vertex buffer; UINT32_MAX = none. */
    u32 stream_offset[DE_COOKED_STREAM_COUNT];
    u32 stream_format[DE_COOKED_STREAM_COUNT]; /**< de_cooked_vertex_format, 0 = none */
    u32 vertex_count, index_count, meshlet_count, meshlet_vertex_count, meshlet_triangle_bytes;
    u32 submesh_count, lod_count;
    f32 bounds_min[3], bounds_max[3];
    f32 sphere[4];
    f32 position_offset[3], position_scale[3];
    f32 uv_offset[2][2], uv_scale[2][2];
    f32 lod_error, max_position_error;
    de_cooked_mesh_lod lods[DE_COOKED_MESH_MAX_LODS];
    const de_render_submesh *submeshes;        /**< submesh_count */
    const de_cooked_mesh_range *lod_submeshes; /**< lod_count x submesh_count, LOD-major */
    u64 gpu_bytes;
    struct de__upload *upload_;
    size_t block_size_;
    de__render_retire retire_; /**< internal: the retire list (DENG-422) */
    de_asset_id asset_id_;
    struct de_render_mesh *published_next_;
    bool published_;
} de_render_mesh;

/** A loaded `material` (DMAT): the CPU parameters. The GPU scene packs them into the material's
 * parameter block (render/materials/material_params.h, DENG-302); de_material_set_params changes
 * them in place (main thread). */
typedef struct de_render_material {
    /** The parameters as de_render.h stores them, texture handles included. */
    de_standard_material standard;
    u32 flags;                        /**< DE_COOKED_MAT_* */
    u32 alpha_mode;                   /**< de_cooked_alpha_mode */
    de_cooked_material_params params; /**< every cooked parameter (some have no standard field) */
    /** Per slot (de_cooked_tex_slot): the texture, its asset ID and texcoord set; invalid / {0}
     * for a slot without one. */
    de_texture textures[DE_COOKED_TEX_COUNT];
    de_asset_id texture_ids[DE_COOKED_TEX_COUNT];
    u8 texcoord[DE_COOKED_TEX_COUNT];
    de__render_retire retire_; /**< internal: the retire list (DENG-422) */
    /** DENG-298: the material model id (de_material_model_id) when it is a game model's (5..31);
     * 0 = the built-in the parameters select (Unlit when `standard.unlit`, else Standard). */
    u32 model;
    /** DENG-302: the model the cooked material names (DMAT v2 header word 28): 0 = none (`model`
     * decides), else de_cooked_material_model_hash of the model's name, resolved against the
     * renderer's models each time the block is packed (a game model may register later). */
    u32 model_hash;
} de_render_material;

/** One node of a loaded model. */
typedef struct de_render_model_node {
    const char *name; /**< the node's sub-asset name */
    u32 parent;       /**< node index, DE_COOKED_NONE for a root */
    de_mesh mesh;     /**< invalid when the node has none */
    u32 skin;         /**< index into the model's skeleton IDs, DE_COOKED_NONE */
    f32 translation[3], rotation[4], scale[3];
} de_render_model_node;

/** A loaded `model` (DMDL): the node table with handles to its meshes, and its prefab. */
typedef struct de_render_model {
    u32 node_count, root_count, mesh_count, material_count;
    const de_render_model_node *nodes;
    const u32 *roots;
    const de_mesh *meshes; /**< in the model's mesh ID order */
    const de_material *materials;
    de_prefab prefab; /**< the `#prefab` sub-asset; invalid when the model has none */
    size_t block_size_;
} de_render_model;

/**
 * The runtime record behind a handle, or NULL when the handle is stale, of another type, or the
 * asset is not READY. Valid until the asset is unloaded (eviction, or a hot reload's retirement,
 * reload_retire_frames updates after the swap).
 */
const de_render_texture *de_render_texture_get(de_asset_system *s, de_texture t);
const de_render_mesh *de_render_mesh_get(de_asset_system *s, de_mesh m);
const de_render_material *de_render_material_get(de_asset_system *s, de_material m);
const de_render_model *de_render_model_get(de_asset_system *s, de_model m);

DE_EXTERN_C_END

#endif /* DE_RENDER_LOADERS_RENDER_ASSETS_H */
