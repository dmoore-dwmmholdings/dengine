/* Current-frame opaque HDR Gaussian levels and reverse-Z depth snapshot (DENG-342).
 * Both are packed transient GPU buffers, avoiding render-target sampling and image descriptor
 * state conflicts. Mip0 is an exact RGBA16F copy; rows align to 256 bytes, levels to 512 bytes.
 * State owns only a pre-warmed kernel. Context resources belong to their graph, with no history
 * or per-view persistent allocations; normal graph frame fences own reuse and retirement. */
#ifndef DE_RENDER_SCENE_COLOR_PYRAMID_H
#define DE_RENDER_SCENE_COLOR_PYRAMID_H
#include "render/passes/vis/vis_stages.h"
DE_EXTERN_C_BEGIN
#define DE__SCENE_COLOR_LEVELS 16u
typedef struct de__scene_color_level {
    u32 width, height, pitch, offset;
} de__scene_color_level;
typedef struct de__scene_color_layout {
    de__scene_color_level level[DE__SCENE_COLOR_LEVELS];
    u32 count, depth_pitch;
    u64 bytes, depth_bytes;
} de__scene_color_layout;
typedef struct de__scene_color_params {
    u32 color, src_width, src_height, src_pitch;
    u32 src_offset, width, height, pitch;
    u32 offset, reserved0, reserved1, reserved2;
} de__scene_color_params;
DE_STATIC_ASSERT(sizeof(de__scene_color_params) == 48, "SceneColorParams is 48 bytes");
typedef struct de__scene_color_pyramid de__scene_color_pyramid;
typedef struct de__scene_color_context {
    de_rg_resource color, depth;
    de__scene_color_layout layout;
} de__scene_color_context;
de_result de__scene_color_layout_make(u32 width, u32 height, de__scene_color_layout *out);
de_result de__scene_color_create(de__vis_shared *vis, de__scene_color_pyramid **out);
void de__scene_color_destroy(de__scene_color_pyramid *p);
de_result de__scene_color_build(de__scene_color_pyramid *p, de_rg *g, de_arena *arena,
                                de_rg_resource color, de_rg_resource depth, u32 width, u32 height,
                                de__scene_color_context *out);
DE_EXTERN_C_END
#endif
