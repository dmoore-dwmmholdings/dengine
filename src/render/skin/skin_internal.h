#ifndef DE_RENDER_SKIN_INTERNAL_H
#define DE_RENDER_SKIN_INTERNAL_H
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"
#include "render/rt/rt_internal.h"

typedef struct de__skin_entry {
    de_entity entity;
    u32 joint_count, palette_capacity[4];
    const de_render_mesh *source;
    de__gs_mesh packed;
    de_gpu_buffer output[2], palette[4], description[4], ranges;
    de_gpu_accel blas[2][8];
    bool blas_built[2][8];
    u32 current, slot;
    bool active, initialized;
    u64 refits, rebuilds;
} de__skin_entry;
typedef struct de__skin_input {
    de_entity entity;
    de_affine *joints;
    u32 count, capacity;
} de__skin_input;

typedef struct de__skin {
    de_renderer *renderer;
    de__skin_entry *entries;
    u32 count, capacity;
    /* Simulation staging: PostUpdate writes, Extract copies; render never reads these. */
    de__skin_input *inputs;
    u32 input_count, input_capacity;
    de_gpu_pipeline pipeline;
    de_cvar interval;
    u64 frame;
} de__skin;
de_result de__skin_init(de_renderer *r);
de_result de__skin_extract(de_renderer *r, de__render_world_frame *frame);
de_result de__skin_frame_copy(const de_allocator *alloc, de__render_world_frame *dst,
                              const de__render_world_frame *src);
void de__skin_frame_free(const de_allocator *alloc, de__render_world_frame *frame);

de_result de__skin_prepare(de_renderer *r, const de__render_world_frame *frame,
                           const de__gpu_scene_assets *assets);
de__skin_entry *de__skin_instance(const de_renderer *r, u32 slot);
void de__skin_destroy(de_renderer *r);
void de__skin_release_rt(de_renderer *r);
de_result de__skin_rt_prepare(de_renderer *r);
bool de__skin_rt_metadata(de_renderer *r, const de__gs_mesh *mesh, de__rt_mesh_gpu *out);
#endif
