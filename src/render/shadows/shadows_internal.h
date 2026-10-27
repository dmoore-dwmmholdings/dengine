#ifndef DE_RENDER_SHADOWS_INTERNAL_H
#define DE_RENDER_SHADOWS_INTERNAL_H
#include "render/passes/vis/vis_stages.h"
#include <dengine/de_debug.h>
#include <stddef.h>
DE_EXTERN_C_BEGIN

/* Implicit complete quadtree: one byte per node, allocated once; no allocation during use.
 * Tile size includes guards. A tile handle is valid until freed or the allocator is reset. */
typedef struct de__atlas_alloc {
    const de_allocator *allocator;
    u8 *nodes;
    u32 size, node_count, occupied;
} de__atlas_alloc;
typedef struct de__atlas_tile {
    u32 node, x, y, size;
} de__atlas_tile;
de_result de__atlas_alloc_init(de__atlas_alloc *a, const de_allocator *allocator, u32 size);
void de__atlas_alloc_destroy(de__atlas_alloc *a);
bool de__atlas_alloc_take(de__atlas_alloc *a, u32 size, de__atlas_tile *out);
bool de__atlas_alloc_free(de__atlas_alloc *a, de__atlas_tile tile);
/** Internal contexts never consume de_view slots. The pool grows in 32-context blocks. */
typedef struct de__shadow_views de__shadow_views;
typedef struct de__shadow_view_desc {
    de__gpu_scene *scene;
    de_rg_resource depth;
    u32 context, layer, rect[4];
    u32 width, height, layer_mask;
    de_mat4 view, proj;
    de_vec3 camera;
    de_dvec3 origin;
    u64 frame, frames_completed;
    bool clear_depth; /**< Clears this entire array layer; atlas owners clear once separately. */
} de__shadow_view_desc;
de_result de__shadow_views_create(de_gpu_device *dev, const de_allocator *alloc,
                                  de__shadow_views **out);
void de__shadow_views_destroy(de__shadow_views *s);
de_result de__shadow_view_build(de__shadow_views *s, de_rg *g, const de__shadow_view_desc *d,
                                de__vis_context **out);
/** After this graph is discarded or its execution attempted. A submission error may still
 * have executed earlier batches: attempted=true conservatively holds every ring entry until
 * frames_completed advances from the renderer's frame fence. Never abort actual submitted work.
 * End must precede graph/arena reset: scratch ownership references live in the graph arena. */
void de__shadow_views_end(de__shadow_views *s, const de_rg *graph, bool attempted);
/** Exact D32 layer bits -> sampled-only R32 mirror through reusable aligned GPU scratch. */
de_result de__shadow_copy_layer(de__shadow_views *s, de_rg *g, de_rg_resource depth, u32 layer,
                                de_rg_resource mirror, u32 width, u32 height);
u32 de__shadow_views_capacity(const de__shadow_views *s);
/* S15 CSM contract. Directional source_radius is an angular radius in radians. */
#define DE__CSM_CASCADES 4u
#define DE__CSM_LIGHTS   2u
typedef struct de__csm_cascade {
    de_mat4 view, proj;
    de_vec3 center;
    f32 radius, texel, near_distance, far_distance;
} de__csm_cascade;
typedef struct de__csm_camera {
    de_vec3 position, forward;
    de_dvec3 origin;
    f32 near_z, distance, tan_half_fov, aspect, ortho_half_height;
} de__csm_camera;
void de__csm_quality(de_quality q, u32 *cascades, u32 *resolution, u32 *taps);
void de__csm_splits(f32 near_z, f32 far_z, u32 count, f32 out[5]);
de_result de__csm_fit(const de__csm_camera *camera, de_vec3 light_direction, u32 cascades,
                      u32 resolution, de__csm_cascade out[4]);
typedef struct de__shadow_map_ref {
    u32 texture, layer, taps, resolution;
    f32 uv_clamp[4];
    /* Light-space endpoints; angular source radius for ortho, metres for perspective. */
    f32 depth_projection[4]; /* near, far, tan-half-FOV, source radius */
    f32 uv_projection[4];    /* UV/world (ortho) or UV*depth/world (perspective), max texels, pad */
    u32 filter_state[4];     /* projection: 1 ortho / 2 perspective, quality, frame, blocker taps */
} de__shadow_map_ref;
de_cvar de__shadow_pcss_register(void);
typedef struct de__csm_gpu {
    f32 matrix[4][4], sphere[4], light[4], params[4];
    u32 identity[4];
    de__shadow_map_ref map;
} de__csm_gpu;
DE_STATIC_ASSERT(sizeof(de__csm_gpu) == 208, "CsmCascade is 208 bytes");
DE_STATIC_ASSERT(sizeof(de__shadow_map_ref) == 80, "ShadowMapRef is 80 bytes");
DE_STATIC_ASSERT(offsetof(de__shadow_map_ref, depth_projection) == 32, "PCF prefix is unchanged");
DE_STATIC_ASSERT(offsetof(de__csm_gpu, map) == 128, "CSM map offset is unchanged");
typedef struct de__csm de__csm;
de_result de__csm_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__csm **out);
de_result de__csm_create(de_renderer *r, de__csm **out);
void de__csm_destroy(de__csm *s);
de_result de__csm_build(de__csm *s, const de__view_frame *vf, const de__vis_context *vc);
struct de__lighting_inputs;
void de__csm_publish(de__csm *s, de_view view, de_rg *g, u64 frame, struct de__lighting_inputs *in);
void de__csm_complete(de__csm *s, de_view view, const de_rg *graph, bool attempted, bool success);
typedef struct de__csm_info {
    u32 cascades, resolution, lights, draws, capacity;
    de_gpu_texture depth, mirrors[8];
    de__csm_gpu data[9];
    bool ready;
} de__csm_info;
bool de__csm_get(const de__csm *s, de_view view, de__csm_info *out);

#define DE__ATLAS_LIGHTS  32u
#define DE__ATLAS_FACES   (DE__ATLAS_LIGHTS * 6u)
#define DE__ATLAS_RECORDS (1u + DE__ATLAS_FACES)
/* Perspective metadata also retains linear near/far and local source radius for future PCSS. */
typedef struct de__atlas_gpu {
    f32 matrix[4][4], position[4], projection[4], tile[4];
    u32 identity[4];
    de__shadow_map_ref map;
} de__atlas_gpu;
DE_STATIC_ASSERT(sizeof(de__atlas_gpu) == 208, "AtlasShadow is 208 bytes");
DE_STATIC_ASSERT(offsetof(de__atlas_gpu, map) == 128, "Atlas map offset is unchanged");
typedef struct de__shadow_atlas de__shadow_atlas;
f32 de__atlas_light_range(const DeLight *light, f32 cutoff);
/** Conservative group-mask receiver admission; output contains ranked light slots. */
u32 de__atlas_select_lights(const struct de__render_world_frame *world,
                            const struct de__rw_view *camera, u32 mask, bool unknown_receivers,
                            u32 limit, u32 out[DE__ATLAS_LIGHTS]);
de_result de__shadow_atlas_create_ex(de_gpu_device *dev, const de_allocator *alloc,
                                     de__shadow_atlas **out);
de_result de__shadow_atlas_create(de_renderer *r, de__shadow_atlas **out);
/* Register every active receiver before building views: one shared atlas per effective caster
 * mask, stable primary-camera ranking, maximum group resolution and a global tile quota. */
void de__shadow_atlas_frame_begin(de__shadow_atlas *s, u64 frame,
                                  const struct de__rw_view *ranking_camera);
de_result de__shadow_atlas_frame_view(de__shadow_atlas *s, de_view view, u32 layer_mask,
                                      de_quality quality);
bool de__shadow_atlas_capture_ready(const de__shadow_atlas *s, de_view view);
void de__shadow_atlas_destroy(de__shadow_atlas *s);
de_result de__shadow_atlas_build(de__shadow_atlas *s, const de__view_frame *vf,
                                 const de__vis_context *vc);
void de__shadow_atlas_complete(de__shadow_atlas *s, de_view view, const de_rg *graph,
                               bool attempted, bool success);
void de__shadow_atlas_publish(de__shadow_atlas *s, de_view view, de_rg *g, u64 frame,
                              struct de__lighting_inputs *in);
typedef struct de__atlas_info {
    u32 size, lights, rendered, occupied, capacity;
    bool ready;
    de_gpu_texture depth, mirror;
    de_gpu_buffer records; /**< Immutable upload entry used by this receiver's latest frame. */
    de__atlas_gpu data[DE__ATLAS_RECORDS];
} de__atlas_info;
bool de__shadow_atlas_get(const de__shadow_atlas *s, de_view view, de__atlas_info *out);
/* S19 may flag an existing cached light with animated casters without changing public DeLight.
 * A flagged influence invalidates every receiver frame; false resumes ordinary change tracking. */
bool de__shadow_atlas_dynamic(de__shadow_atlas *s, de_view view, u64 entity, bool dynamic);
DE_EXTERN_C_END
#endif
