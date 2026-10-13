/*
 * clip_decode.c: the reference sampler of cooked clips (DCLP, de_cooked_clip.h; DENG-104).
 *
 * Tests use it to check the cooker, and the S19 playback and S27 animator build on it. It reads
 * the blob in place (no allocation) and uses only + - * / and de_dsqrt, through the inline
 * helpers of de_cooked_clip.h that the cooker also uses, so the cooker measures its error with
 * exactly this arithmetic. The anim module is DETERMINISTIC (-ffp-contract=off): this file gives
 * the same poses on every host.
 */
#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"

#include <dengine/de_base.h>

bool de__anim_clip_sample(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skel,
                          f32 time, de_cooked_joint_pose *out) {
    if (clip == NULL || skel == NULL || out == NULL || clip->joint_count != skel->joint_count)
        return false;
    for (u32 j = 0; j < skel->joint_count; ++j)
        out[j] = de_cooked_skeleton_rest(skel, j);
    if (!(time > 0.0f)) /* also NaN */
        time = 0.0f;
    if (time > clip->duration)
        time = clip->duration;
    for (u32 i = 0; i < clip->track_count; ++i) {
        de_cooked_clip_track t = de_cooked_clip_get_track(clip, i);
        f32 v[4] = {0, 0, 0, 0};
        de_cooked_clip_track_sample(clip, &t, time, v);
        de_cooked_clip_apply(&t, v, &out[t.joint]);
    }
    return true;
}
