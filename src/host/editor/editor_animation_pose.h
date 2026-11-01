#ifndef DE_EDITOR_ANIMATION_POSE_H
#define DE_EDITOR_ANIMATION_POSE_H
#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"
#include <de_editor_protocol.h>

#define DE__ED_ANIMATION_JOINTS 4096u
/* Loaded, parsed cooked views only. Results borrow their names until serialization.
 * All transforms and records live in the request arena. Failure preserves out/arena. */
de_result de__ed_animation_sample(const de_cooked_clip_view *clip,
                                  const de_cooked_skeleton_view *skeleton,
                                  const de_ep_animation_preview_params *params, u32 clip_version,
                                  u32 skeleton_version, de_ep_str skeleton_id, de_ep_arena *arena,
                                  de_ep_animation_preview *out);
#endif
