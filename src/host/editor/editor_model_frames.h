#ifndef DE_EDITOR_MODEL_FRAMES_H
#define DE_EDITOR_MODEL_FRAMES_H
#include <dengine/de_render.h>
#include "anim/formats/de_cooked_skeleton.h"
#include "render/loaders/render_assets.h"

#define DE__ED_FRAME_LINES  32768u
#define DE__ED_FRAME_JOINTS 4096u
typedef struct de__ed_frame_line {
    de_dvec3 a, b;
    u8 rgba[4];
    bool marker;
} de__ed_frame_line;

/* A unique skin among the selected mesh's skinned instances. Missing: NotFound; multiple:
 * Unsupported. Never chooses an arbitrary skeleton or changes *skin on failure. */
de_result de__ed_model_skin(const de_render_model *model, u32 mesh, u32 *skin);
/* Append actual transformed object-space boxes or cooked inverse-bind joints and parent links.
 * Caller owns all bounded storage; failure preserves *count (scratch/output bytes may change). */
de_result de__ed_model_box(const de_transform *pose, const f32 min[3], const f32 max[3],
                           de__ed_frame_line *lines, u32 capacity, u32 *count);
de_result de__ed_model_bind(const de_cooked_skeleton_view *skeleton, const de_transform *pose,
                            de_dvec3 *joints, u32 joint_capacity, de__ed_frame_line *lines,
                            u32 capacity, u32 *count);
/* Through-surface diagnostic compositing onto a real GPU readback. Frozen camera/origin,
 * homogeneous near/viewport clipping, no depth test or global debug-draw state. No allocation. */
void de__ed_model_frame_draw(const de__ed_frame_line *lines, u32 count,
                             const de_view_frame_info *frame, u32 size, u8 *rgba);
#endif
