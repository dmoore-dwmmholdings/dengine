#ifndef DE_ANIM_BLEND_H
#define DE_ANIM_BLEND_H
#include "anim/pose.h"

/* Internal operators for the graph evaluator. Poses and masks are caller-owned;
 * no allocation occurs. Exact output/input aliasing is supported, partial channel
 * overlap is not. Cooked poses contain finite TRS and unit rotations. */
bool de__anim_pose_delta(de__anim_local_pose *out, const de__anim_local_pose *sample,
                         const de__anim_local_pose *reference, bool deterministic);
/* Additive layers take a cached delta pose, override layers take a sampled pose.
 * NULL mask means all bones; root_mask independently weights the root-motion
 * slot. All weights must be in [0,1]. Invalid arguments leave output untouched. */
bool de__anim_pose_layer(de__anim_local_pose *out, const de__anim_local_pose *base,
                         const de__anim_local_pose *layer, f32 weight, const f32 *mask,
                         f32 root_mask, bool additive, bool deterministic);
typedef struct de__anim_bone_mask {
    de_name bone;
    f32 weight;
    bool include_children;
} de__anim_bone_mask;
/* Bind-time union: overlapping rules take their maximum weight. No rules means
 * all bones; unknown names/invalid weights fail without modifying the mask. */
bool de__anim_mask_resolve(const de_cooked_skeleton_view *, const de__anim_bone_mask *,
                           u32 rule_count, f32 *mask);
#endif
