#ifndef DE_RENDER_RT_INTERNAL_H
#define DE_RENDER_RT_INTERNAL_H
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/loaders/render_assets.h"
#include "render/views_internal.h"
#define DE__RT_RING 8u
#define DE__RT_LODS 8u
/* Shader mirror: handles and first indices per LOD, then submesh range metadata. */
typedef struct de__rt_mesh_gpu {
    u32 blas[8], index_first[8];
    u32 ranges, submeshes, resident_mask, reserved;
} de__rt_mesh_gpu;
typedef struct de__rt_mesh {
    const de_render_mesh *source;
    de_asset_id asset_id;
    u64 base_bytes, bytes;
    de__gs_mesh packed;
    de_gpu_buffer positions, description, ranges;
    de_gpu_accel blas[8], compact[8];
    u64 sizes[8], compact_sizes[8], compact_value[8];
    u32 live_lods;
    bool seen;
} de__rt_mesh;
typedef struct de__rt_context {
    de_view view;
    de_rg *graph;
    u64 frame;
    de_gpu_accel tlas;
    de_gpu_buffer instances, selected_lods, scene, query;
    de_rg_resource dependency;
    u32 tlas_index, metadata, count, capacity, scene_slot;
} de__rt_context;
typedef struct de__rt_view {
    de__rt_context context;
    de_gpu_buffer cull[DE__RT_RING], query[DE__RT_RING], dependency;
} de__rt_view;
typedef struct de__rt_scene {
    de_renderer *renderer;
    de__rt_mesh *meshes;
    u32 mesh_count, mesh_capacity;
    de_gpu_pipeline decode, generate, trace;
    de_gpu_sampler sampler;
    de_gpu_fence fence;
    u64 value, bytes;
    de_gpu_buffer metadata[DE__RT_RING];
    u32 metadata_capacity, metadata_index;
    de__rt_view views[32];
    u32 blas_count, compacted_count;
    de_cvar golden_sample; /**< Dev/test stochastic epoch; never a submission/fence index. */
} de__rt_scene;
/* Stable batch query ABI; ray origin/direction are render-origin-relative. */
typedef struct de__rt_ray {
    f32 origin[4], direction[4];
} de__rt_ray;
typedef struct de__rt_hit {
    f32 t;
    u32 instance, primitive;
    f32 u, v;
    u32 lod, reserved0, reserved1;
} de__rt_hit;
#define DE__RT_CAST_SHADOWS     1u
#define DE__RT_ACCEPT_FIRST_HIT 2u
/** Normal production batch path for45–50: buffers use sizeof(ray)/sizeof(hit) strides.
 * Caller declares graph read(context->dependency), ray SHADER_READ and hit SHADER_WRITE. */
void de__rt_trace(de_renderer *, de_gpu_cmd *, const de__rt_context *, de_gpu_buffer rays,
                  de_gpu_buffer hits, u32 count, u32 flags);
/** Main-thread mesh snapshot/build/compaction lifecycle; renderer thread is idle at the kick. */
de_result de__rt_prepare(de_renderer *);
void de__rt_scene_destroy(de_renderer *);
void de__rt_view_stage(de__view_frame *, void *);
/** Later effects declare an explicit read of context->dependency before reading its TLAS. */
de__rt_context *de__rt_context_get(de_renderer *, de_view, de_rg *, u64 frame);
/** Entry point shared by the actual loader registry and focused native fixtures. */
de_result de__rt_mesh_publish(de__rt_scene *, const de_render_mesh *);
void de__rt_mesh_evict(de__rt_scene *, const de_render_mesh *);
de_result de__rt_scene_create(de_renderer *, de__rt_scene **);
/** Dev/test-only deterministic stochastic frame; normal rendering returns the real frame. */
u64 de__rt_sample_frame(const de_renderer *, u64 frame);
#endif
