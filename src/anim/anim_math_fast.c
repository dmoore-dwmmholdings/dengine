/* Visual-only adapter; deterministic inline recipes never call this branch. */
#include "anim/anim_math.h"
de_vec3 de__anim_fast_lerp(de_vec3 a, de_vec3 b, f32 t) {
    return de_v3_lerp(a, b, t);
}
de_quat de__anim_fast_normalize(de_quat q) {
    return de_quat_normalize(q);
}
de_quat de__anim_fast_nlerp(de_quat a, de_quat b, f32 t) {
    return de_quat_nlerp(a, b, t);
}
de_quat de__anim_fast_slerp(de_quat a, de_quat b, f32 t) {
    return de_quat_slerp(a, b, t);
}
