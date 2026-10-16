#ifndef DE_RENDER_SHADOWS_INTERNAL_H
#define DE_RENDER_SHADOWS_INTERNAL_H
#include "render/passes/vis/vis_stages.h"
DE_EXTERN_C_BEGIN
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
} de__shadow_map_ref;
typedef struct de__csm_gpu {
    f32 matrix[4][4], sphere[4], light[4], params[4];
    u32 identity[4];
    de__shadow_map_ref map;
} de__csm_gpu;
DE_STATIC_ASSERT(sizeof(de__csm_gpu) == 160, "CsmCascade is 160 bytes");
DE_STATIC_ASSERT(sizeof(de__shadow_map_ref) == 32, "ShadowMapRef is 32 bytes");
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
DE_EXTERN_C_END
#endif
