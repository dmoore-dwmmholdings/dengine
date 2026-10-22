#ifndef DE_ANIM_POSE_H
#define DE_ANIM_POSE_H
#include <dengine/de_math.h>
#include "anim/formats/de_cooked_skeleton.h"

/* Channel-separated local pose. Stages edit only the channels they need; model
 * and skin matrices are built once after the local-pose stages have completed. */
typedef struct de__anim_local_pose {
    u32 count;
    de_quat *rotations;
    de_vec3 *translations, *scales;
    /* Reserved for extraction/blending by DENG-186, not applied by single-clip playback. */
    de_vec3 root_translation;
    de_quat root_rotation;
} de__anim_local_pose;

static inline de_cooked_joint_pose de__anim_pose_joint(const de__anim_local_pose *p, u32 j) {
    de_vec3 t = p->translations[j], s = p->scales[j];
    de_quat r = p->rotations[j];
    return (de_cooked_joint_pose){{t.x, t.y, t.z}, {r.x, r.y, r.z, r.w}, {s.x, s.y, s.z}};
}
static inline void de__anim_pose_set_joint(de__anim_local_pose *p, u32 j,
                                           de_cooked_joint_pose value) {
    p->translations[j] = (de_vec3){value.t[0], value.t[1], value.t[2]};
    p->rotations[j] = (de_quat){value.r[0], value.r[1], value.r[2], value.r[3]};
    p->scales[j] = (de_vec3){value.s[0], value.s[1], value.s[2]};
}
#endif
